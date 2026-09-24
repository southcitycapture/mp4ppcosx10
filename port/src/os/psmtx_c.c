/* The paired-single matrix library, in scalar C.
 *
 * The game calls `PSMTX*` / `PSVEC*` directly at 219 sites, and `-DMTX_USE_C`
 * only redirects the generic `MTX*` / `VEC*` macros -- it does not remove the
 * `PS*` bodies from the SDK sources, which are Metrowerks paired-single
 * assembly and are dropped by the port's mirror.  Everything the game actually
 * calls is defined here: mostly forwarders to the `C_MTX*` bodies that are
 * already in the tree, plus the handful of functions the SDK never wrote in C.
 *
 * Two of those have no C equivalent in any SDK decompilation and are written
 * from the register-transfer semantics of `psmtx.c`:
 *
 *   PSMTXReorder        (19 sites) transposes a 3x4 row-major Mtx into the
 *                       4x3 "reordered" ROMtx the skinning fast paths want:
 *                       three rows of translation-free 3-vectors followed by
 *                       the translation column.
 *   PSMTXROMultVecArray (18 sites) is MTXMultVecArray against that layout.
 *
 * The 7450 has no paired singles at all, so there is nothing to preserve here
 * but the arithmetic.  This file is the first place to reach for AltiVec if
 * --perf ever says so: MTX/VEC is 74 symbols over 4,172 call sites, the
 * hottest SDK family after GX.
 */
#include <math.h>
#include <string.h>

#include <dolphin/types.h>
#include <dolphin/mtx.h>
#include "port.h"

/* ---- the sin/cos memo (M18 item 2, PLAN.md 33.4) ---------------------------
 *
 * SetEnvelopMtx builds T.Rz.Ry.Rx per bone per skinned model, three
 * PSMTXRotRad each, and libm's sinf/cosf on a 7450 are a few hundred cycles
 * the pair.  Motion curves hold many bones still for many frames and the
 * game's own mtxRot calls repeat the same angles, so a direct-mapped memo on
 * the angle's bits answers most calls from a table and the rest from libm --
 * the very same values either way, which is what makes it exact.
 * `--nosincos` is the A/B; the hit rate is in the shutdown report. */
/* M41 (PLAN.md 56): 4096 slots (was 1024) once C_MTXRotRad -- every object
 * walk's mtxRot -- came through here too */
#define SINCOS_SLOTS 4096
static struct {
    u32 bits;
    f32 s, c;
    u8 valid;
} sincos_memo[SINCOS_SLOTS];
static unsigned long sincos_hits, sincos_misses;

void port_sincosf(f32 rad, f32* s, f32* c) {
    union { f32 f; u32 u; } k;
    unsigned i;
    k.f = rad;
    i = ((k.u >> 3) ^ (k.u >> 15) ^ (k.u >> 25)) & (SINCOS_SLOTS - 1);
    if (!port_opt.nosincos && sincos_memo[i].valid && sincos_memo[i].bits == k.u) {
        *s = sincos_memo[i].s;
        *c = sincos_memo[i].c;
        sincos_hits++;
        return;
    }
    *s = sinf(rad);
    *c = cosf(rad);
    sincos_memo[i].bits = k.u;
    sincos_memo[i].s = *s;
    sincos_memo[i].c = *c;
    sincos_memo[i].valid = 1;
    sincos_misses++;
}

/* M41 (PLAN.md 56): C_MTXRotRad's pair (patches.txt), through the memo;
 * --norotmemo is libm at every call there, as before M41 */
void port_rotrad_sincosf(f32 rad, f32* s, f32* c) {
    if (port_opt.norotmemo) {
        *s = sinf(rad);
        *c = cosf(rad);
        return;
    }
    port_sincosf(rad, s, c);
}

void port_sincos_report(void) {
    if (sincos_hits + sincos_misses) {
        port_log("port> sincos memo: %lu hits, %lu misses (%.1f%% hit)%s\n", sincos_hits,
                 sincos_misses, 100.0 * sincos_hits / (sincos_hits + sincos_misses),
                 port_opt.nosincos ? " (--nosincos: every call to libm)" : "");
    }
}

/* ---- M28 (c): the sparse concats of the bone walk (PLAN.md 43) ------------
 *
 * SetEnvelopMtx (EnvelopeExec.c) builds a bone's matrix as
 * parent . T(pos) . Rz . Ry . Rx with four general 3x4 concats, each of which
 * loads all twelve of the right-hand matrix although nine of them are the
 * literal 0 and 1 of a translation or a single-axis rotation -- and each
 * rotation was first *written* as twelve floats by PSMTXRotTrig.  These four
 * bodies compute the same elements without the loads or the stores, and are
 * bit-exact with C_MTXConcat by construction, which needs one thing spelled
 * out: the ORDER GCC compiled C_MTXConcat in (port/build-ppc-darwin/.../mtx.o,
 * read with otool; -std=gnu11 contracts to fmadds), per element
 *
 *     m[i][j] = fmadds(a[i][2], b[2][j], fmadds(a[i][0], b[0][j], a[i][1] * b[1][j]))
 *     m[i][3] = a[i][3] + (the same over column 3)
 *
 * and what a fused multiply-add does with a literal 0 or 1:
 *
 *   x * 0            is a zero carrying x's sign;
 *   fma(x, 0, t)     is t + (that zero): t itself unless t is a zero, and then
 *                    -0 only if both are -0;
 *   fma(x, 1, t)     is round(x + t): x itself unless x is a zero (the same
 *                    rule), because t is then a zero too in every case below.
 *
 * So a product against a literal is one sign bit, and a chain of them is an
 * AND of sign bits that matters only when the surviving term is a zero.
 * The real products (the sines and cosines, the translation) stay real fmas
 * with the same operands in the same order; a zero that reaches one as an
 * addend is passed as the signed zero it is, and the hardware applies the
 * rule.  Inf and NaN entries are the one input this is not exact for (inf*0
 * is NaN in the general form); a bone matrix with either is already a broken
 * picture.  port/tests/mtx_test.c runs each body against C_MTXConcat over
 * random matrices seeded with zeros of both signs: 0 differ.  --nosparsemtx is
 * the general form, through the very calls the game makes. */
typedef union {
    f32 f;
    u32 u;
} FloatBits;

static inline u32 sgn(f32 x) {
    FloatBits b;
    b.f = x;
    return b.u & 0x80000000u;
}
static inline f32 zero_of(u32 sign) {
    FloatBits b;
    b.u = sign;
    return b.f;
}
/* fma(x, 0, t) */
static inline f32 add_zero(f32 t, f32 x) { return t != 0.0f ? t : zero_of(sgn(t) & sgn(x)); }
/* a[i][j] left alone by 1 and 0 terms whose signs AND to `zsign` */
static inline f32 keep(f32 a, u32 zsign) { return a != 0.0f ? a : zero_of(sgn(a) & zsign); }

#define FMA(x, y, t) __builtin_fmaf((x), (y), (t))

static unsigned long sparse_calls, sparse_general;

/* ab = a . T(x, y, z);  ab may alias a */
void port_mtx_concat_trans(const Mtx a, f32 x, f32 y, f32 z, Mtx ab) {
    int i;
    if (port_opt.nosparsemtx) {
        Mtx t;
        sparse_general++;
        PSMTXTrans(t, x, y, z);
        PSMTXConcat(a, t, ab);
        return;
    }
    sparse_calls++;
    for (i = 0; i < 3; i++) {
        f32 a0 = a[i][0], a1 = a[i][1], a2 = a[i][2], a3 = a[i][3];
        u32 z012 = sgn(a0) & sgn(a1) & sgn(a2);
        ab[i][0] = keep(a0, z012);
        ab[i][1] = keep(a1, z012);
        ab[i][2] = keep(a2, z012);
        ab[i][3] = a3 + FMA(a2, z, FMA(a0, x, a1 * y));
    }
}

/* ab = a . R(axis, rad), the rotation through PSMTXRotRad's own sin/cos memo;
 * ab may alias a */
void port_mtx_concat_rot(const Mtx a, char axis, f32 rad, Mtx ab) {
    f32 s, c;
    int i;
    if (port_opt.nosparsemtx) {
        Mtx r;
        sparse_general++;
        PSMTXRotRad(r, axis, rad);
        PSMTXConcat(a, r, ab);
        return;
    }
    sparse_calls++;
    port_sincosf(rad, &s, &c);
    switch (axis) {
    case 'z': /* [c -s 0; s c 0; 0 0 1] */
        for (i = 0; i < 3; i++) {
            f32 a0 = a[i][0], a1 = a[i][1], a2 = a[i][2], a3 = a[i][3];
            u32 z01 = sgn(a0) & sgn(a1);
            ab[i][0] = add_zero(FMA(a0, c, a1 * s), a2);
            ab[i][1] = add_zero(FMA(a0, -s, a1 * c), a2);
            ab[i][2] = keep(a2, z01);
            ab[i][3] = keep(a3, z01 & sgn(a2));
        }
        break;
    case 'y': /* [c 0 s; 0 1 0; -s 0 c] */
        for (i = 0; i < 3; i++) {
            f32 a0 = a[i][0], a1 = a[i][1], a2 = a[i][2], a3 = a[i][3];
            f32 z1 = zero_of(sgn(a1));
            ab[i][0] = FMA(a2, -s, FMA(a0, c, z1));
            ab[i][1] = keep(a1, sgn(a0) & sgn(a2));
            ab[i][2] = FMA(a2, c, FMA(a0, s, z1));
            ab[i][3] = keep(a3, sgn(a0) & sgn(a1) & sgn(a2));
        }
        break;
    case 'x': /* [1 0 0; 0 c -s; 0 s c] */
        for (i = 0; i < 3; i++) {
            f32 a0 = a[i][0], a1 = a[i][1], a2 = a[i][2], a3 = a[i][3];
            ab[i][0] = keep(a0, sgn(a1) & sgn(a2));
            ab[i][1] = FMA(a2, s, add_zero(a1 * c, a0));
            ab[i][2] = FMA(a2, c, add_zero(a1 * -s, a0));
            ab[i][3] = keep(a3, sgn(a0) & sgn(a1) & sgn(a2));
        }
        break;
    default: {
        Mtx r;
        PSMTXRotRad(r, axis, rad);
        PSMTXConcat(a, r, ab);
        break;
    }
    }
}

void port_sparse_report(void) {
    if (sparse_calls || sparse_general) {
        port_log("port> sparse concats (bone walk): %lu sparse, %lu general%s\n", sparse_calls,
                 sparse_general, port_opt.nosparsemtx ? " (--nosparsemtx)" : "");
    }
}

/* ---- M28 (d): the square root without libm (PLAN.md 43) ------------------
 *
 * The 7450 has no fsqrt; libm's sqrtf is a call into software.  It does have
 * frsqrte, a reciprocal square root estimate good to 5 bits, and a double-
 * precision FPU at full speed.  Four Newton steps take the estimate past 53
 * bits (5, 10, 20, 40, 80 -- the last bounded by the double's own rounding),
 * the product with x is the root to about a double ulp, and one correction
 * step (s += (x - s*s) * y/2, the residual through a fused multiply-add)
 * lands it within a double ulp or so of the true root.  Rounding THAT to
 * single is the correctly rounded sqrtf: the exact root of a 24-bit float is
 * never within 2^-50 (relative) of a single-precision rounding boundary
 * (the classical bound, 2p+2 bits), and a double ulp is 2^-52.  So the value
 * is libm's -- if libm's sqrtf is itself correctly rounded, which IEEE 754
 * requires and port/tests/mtx_test.c checks the way that settles it: every
 * positive float, all 2^31 of them, both ways, on the G4.  Zero, negative,
 * inf, NaN and the denormals go to libm.  --nofastsqrt: libm for everything. */
static unsigned long fastsqrt_calls, fastsqrt_libm;

f32 port_sqrtf(f32 x) {
    double d, y, s, h;
    if (port_opt.nofastsqrt || !(x >= 1.17549435e-38f) || x > 3.4e38f) {
        /* zero, negative, NaN, a denormal, inf: libm's own answer */
        fastsqrt_libm++;
        return sqrtf(x);
    }
    fastsqrt_calls++;
    d = x;
#if defined(__ppc__) || defined(__powerpc__)
    __asm__("frsqrte %0,%1" : "=f"(y) : "f"(d));
#else
    y = 1.0 / sqrt(d);
#endif
    h = 0.5 * d;
    y = y * (1.5 - h * y * y);
    y = y * (1.5 - h * y * y);
    y = y * (1.5 - h * y * y);
    y = y * (1.5 - h * y * y);
    s = d * y;
    s = s + __builtin_fma(-s, s, d) * (0.5 * y);
    {
        /* The rounding settled exactly, whatever the steps above lost: the
         * root lies on f's side of both of f's midpoints iff f is the
         * correctly rounded root, and a midpoint (25 significant bits)
         * squares exactly in a double (50), as does x (24) compare against
         * it.  A midpoint's square is never x itself (an odd 25-bit
         * mantissa squared has 49 bits), so there are no ties. */
        FloatBits fb, up, dn;
        double fd, mu, md;
        fb.f = (f32)s;
        up.u = fb.u + 1;
        dn.u = fb.u - 1;
        fd = fb.f;
        mu = 0.5 * (fd + (double)up.f);
        md = 0.5 * (fd + (double)dn.f);
        if (d > mu * mu) {
            return up.f;
        }
        if (d < md * md) {
            return dn.f;
        }
        return fb.f;
    }
}

void port_fastsqrt_report(void) {
    if (fastsqrt_calls || fastsqrt_libm) {
        port_log("port> sqrtf: %lu through frsqrte, %lu through libm%s\n", fastsqrt_calls,
                 fastsqrt_libm, port_opt.nofastsqrt ? " (--nofastsqrt)" : "");
    }
}

/* ---- the C_ bodies the SDK sources never spelled out --------------------- */

void C_MTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    f32 ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    f32 oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    f32 oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    dst->x = ox;
    dst->y = oy;
    dst->z = oz;
}

void C_MTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        C_MTXMultVec(m, &srcBase[i], &dstBase[i]);
    }
}

void C_MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    f32 ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    f32 oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    f32 oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
    dst->x = ox;
    dst->y = oy;
    dst->z = oz;
}

void C_VECAdd(const Vec* a, const Vec* b, Vec* r) {
    r->x = a->x + b->x;
    r->y = a->y + b->y;
    r->z = a->z + b->z;
}

void C_VECSubtract(const Vec* a, const Vec* b, Vec* r) {
    r->x = a->x - b->x;
    r->y = a->y - b->y;
    r->z = a->z - b->z;
}

f32 C_VECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void C_VECCrossProduct(const Vec* a, const Vec* b, Vec* r) {
    f32 x = a->y * b->z - a->z * b->y;
    f32 y = a->z * b->x - a->x * b->z;
    f32 z = a->x * b->y - a->y * b->x;
    r->x = x;
    r->y = y;
    r->z = z;
}

f32 C_VECSquareMag(const Vec* v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
/* M34 (PLAN.md 49.7): the game's sqrtf (include/msl_math.h says why).  MSL's
 * inline returns the argument for anything not > 0 -- zero, a negative
 * number, NaN -- and for +inf its frsqrte is 0, so x * 0 is NaN there too. */
f32 port_msl_sqrtf(f32 x) {
    if (x > 0.0f) {
        if (x > 3.4028235e38f) {
            return x * 0.0f;
        }
        return port_sqrtf(x);
    }
    return x;
}

f32 C_VECMag(const Vec* v) { return port_sqrtf(C_VECSquareMag(v)); }

void C_VECNormalize(const Vec* src, Vec* dst) {
    f32 m = C_VECSquareMag(src);
    if (m > 0.0f) {
        m = 1.0f / port_sqrtf(m);
    }
    dst->x = src->x * m;
    dst->y = src->y * m;
    dst->z = src->z * m;
}

f32 C_VECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

f32 C_VECDistance(const Vec* a, const Vec* b) { return port_sqrtf(C_VECSquareDistance(a, b)); }

/* ---- M31 (PLAN.md 46): the quaternion bodies the SDK only wrote in
 * paired-single asm -------------------------------------------------------
 *
 * quat.c has C_QUATAdd, C_QUATRotAxisRad, C_QUATMtx and C_QUATSlerp in C and
 * PSQUATMultiply / PSQUATNormalize / PSQUATInverse in asm only; the mirror
 * drops the asm and gen_stubs.py made the three C_ names loud stubs that
 * return without writing their result.  m417's raft (player.c:1064-1072)
 * multiplies three quaternions a frame and normalises the product to test
 * the tilt (`sp28.w < cosd(25)`) -- through the stubs the product never
 * moved and the normalised copy was an uninitialised stack Qtrn, which is
 * why the port's solo fell in the first frame of play (+846 against the
 * console's ~2,600).  The bodies below are the SDK's own C ones (the same
 * arithmetic the asm does; PSQUATNormalize's frsqrte+Newton reciprocal is a
 * correctly rounded 1/sqrt here, as C_VECNormalize's is). */
#define PORT_QUAT_EPSILON 0.00001f

void C_QUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) {
    Quaternion t;
    t.w = p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z;
    t.x = p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y;
    t.y = p->w * q->y + p->y * q->w + p->z * q->x - p->x * q->z;
    t.z = p->w * q->z + p->z * q->w + p->x * q->y - p->y * q->x;
    *pq = t;
}

void C_QUATNormalize(const Quaternion* src, Quaternion* unit) {
    f32 mag = src->x * src->x + src->y * src->y + src->z * src->z + src->w * src->w;
    if (mag >= PORT_QUAT_EPSILON) {
        mag = 1.0f / port_sqrtf(mag);
        unit->x = src->x * mag;
        unit->y = src->y * mag;
        unit->z = src->z * mag;
        unit->w = src->w * mag;
    } else {
        unit->x = unit->y = unit->z = unit->w = 0.0f;
    }
}

void C_QUATInverse(const Quaternion* src, Quaternion* inv) {
    f32 mag = src->x * src->x + src->y * src->y + src->z * src->z + src->w * src->w;
    f32 norminv;
    if (mag == 0.0f) {
        mag = 1.0f;
    }
    norminv = 1.0f / mag;
    inv->x = -src->x * norminv;
    inv->y = -src->y * norminv;
    inv->z = -src->z * norminv;
    inv->w = src->w * norminv;
}

/* ---- PS* forwarders ------------------------------------------------------ */

/* M31: the direct PS* calls the game makes outside the MTX_USE_C macros --
 * m428's rope (PSVECSubtract, 249,216 calls a play), mstory3's win effect
 * (PSVECSubtract/Mag/Add/Scale), m438's fire (PSMTXTranspose) -- and the
 * kerent exports (PSMTXQuat, PSQUAT*, PSMTXMultVecSR, PSVEC*) were the same
 * do-nothing stubs until M31. */
void PSMTXQuat(Mtx m, const Quaternion* q) { C_MTXQuat(m, q); }
void PSMTXTranspose(const Mtx src, Mtx xPose) { C_MTXTranspose(src, xPose); }
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVecSR(m, src, dst); }
void PSQUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r) { C_QUATAdd(p, q, r); }
void PSQUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) {
    C_QUATMultiply(p, q, pq);
}
void PSQUATNormalize(const Quaternion* src, Quaternion* unit) { C_QUATNormalize(src, unit); }
void PSQUATInverse(const Quaternion* src, Quaternion* inv) { C_QUATInverse(src, inv); }
void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) { C_VECAdd(a, b, ab); }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) { C_VECSubtract(a, b, a_b); }
void PSVECScale(const Vec* src, Vec* dst, f32 scale) { C_VECScale(src, dst, scale); }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return C_VECDotProduct(a, b); }
f32 PSVECMag(const Vec* v) { return C_VECMag(v); }
f32 PSVECSquareDistance(const Vec* a, const Vec* b) { return C_VECSquareDistance(a, b); }
f32 PSVECDistance(const Vec* a, const Vec* b) { return C_VECDistance(a, b); }

/* PSMTXIdentity is six paired-single stores and therefore writes all twelve
 * elements, translation column included.  C_MTXIdentity used to write only the
 * 3x3 -- patches.txt fixes that in the mirror, and port/tests/mtx_test.c holds
 * it fixed, because a caller with an uninitialised stack Mtx cannot tell. */
void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }
u32 PSMTXInvXpose(const Mtx src, Mtx invX) { return C_MTXInvXpose(src, invX); }
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) { C_MTXRotTrig(m, axis, sinA, cosA); }
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) { C_MTXRotAxisRad(m, axis, rad); }
void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z) { C_MTXTrans(m, x, y, z); }
void PSMTXScale(Mtx m, f32 x, f32 y, f32 z) { C_MTXScale(m, x, y, z); }
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}
void PSVECNormalize(const Vec* src, Vec* dst) { C_VECNormalize(src, dst); }

/* ---- the two with no C original ------------------------------------------ */

/* Mtx is row major, 3 rows of 4: [ R | t ].  ROMtx is 4 rows of 3: the three
 * *columns* of R, then t.  So dest[j][i] = src[i][j] for the rotation part and
 * dest[3][i] = src[i][3] for the translation. */
void PSMTXReorder(const Mtx src, ROMtx dest) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            dest[j][i] = src[i][j];
        }
        dest[3][i] = src[i][3];
    }
}

/* The AltiVec form (M17, PLAN.md 32).  The board's consumed frame spends
 * 9.5% of itself in this loop (the CPU skinning's vertex pass), and it is
 * a vector op by construction: one output vertex is three dot products of
 * the same (x, y, z, 1) against the four ROMtx rows, which is exactly one
 * lane each.
 *
 * Bit-exact with the scalar body, deliberately: GCC contracts
 * `m0*x + m1*y + m2*z + m3` as fmuls(m1, y), fmadds(m0, x, .),
 * fmadds(m2, z, .), fadds(m3, .) (the middle product first, then the two
 * fused terms, then the plain add -- read off build-ppc-darwin/psmtx.s),
 * and vmaddfp is a fused multiply-add with the same single rounding, so the
 * same four operations in the same order give the same bits.  The first
 * product is a vmaddfp with -0.0 as the addend, which is the multiply's
 * rounded value with its sign intact.  port/tests/mtx_test.c checks the two
 * bodies agree bit for bit on 540,000 random vertices (0 differ, on the G4).
 * `--altivec` runs it; see port_mtx_noaltivec below for why it is not the
 * default. */
#ifdef __ALTIVEC__
#include <altivec.h>
#undef vector
#undef pixel
#undef bool

static void romult_altivec(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    const __vector float nzero = (__vector float){ -0.0f, -0.0f, -0.0f, -0.0f };
    const u8* mp = (const u8*)m;
    __vector float M0, M1, M2, M3;
    u32 i;
    /* the four rows, unaligned: lvx reads the two aligned blocks either side
     * and vperm picks the sixteen bytes that start at the row.  The fourth
     * lane of each is whatever follows and is never stored. */
#define LDROW(off) ((__vector float)vec_perm(vec_ld(off, mp), vec_ld((off) + 15, mp), \
                                             vec_lvsl(off, mp)))
    M0 = LDROW(0);
    M1 = LDROW(12);
    M2 = LDROW(24);
    M3 = LDROW(36);
#undef LDROW
    for (i = 0; i < count; i++) {
        const u8* s = (const u8*)&srcBase[i];
        u8* d = (u8*)&dstBase[i];
        __vector float v = (__vector float)vec_perm(vec_ld(0, s), vec_ld(15, s), vec_lvsl(0, s));
        __vector float xs = vec_splat(v, 0);
        __vector float ys = vec_splat(v, 1);
        __vector float zs = vec_splat(v, 2);
        __vector float t = vec_madd(M1, ys, nzero);
        __vector float r;
        t = vec_madd(M0, xs, t);
        t = vec_madd(M2, zs, t);
        r = vec_add(M3, t);
        /* three element stores; the rotate puts element j in the lane that
         * (d + 4j) selects */
        r = vec_perm(r, r, vec_lvsr(0, d));
        vec_ste(r, 0, (float*)d);
        vec_ste(r, 4, (float*)d);
        vec_ste(r, 8, (float*)d);
    }
}
#endif

/* Off unless --altivec: measured on the G4 (PLAN.md 32), the consumed board
 * frame is 10.46 ms with the scalar loop and 10.82 with this one.  The
 * unaligned loads, the permutes and the three element stores per vertex
 * cost more than the FPU pipeline GCC builds for the scalar body -- the same
 * verdict M5 reached for the vertex path (PLAN.md 15.6).  Kept because it is
 * exact and the test proves it, and because the next AltiVec attempt should
 * start from a measured baseline rather than from a belief. */
int port_mtx_noaltivec = 1;

void PSMTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
#ifdef __ALTIVEC__
    if (!port_mtx_noaltivec) {
        romult_altivec(m, srcBase, dstBase, count);
        return;
    }
#endif
    for (i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
        dstBase[i].y = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
        dstBase[i].z = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    }
}
