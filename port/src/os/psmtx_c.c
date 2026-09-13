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

#include <dolphin/types.h>
#include <dolphin/mtx.h>

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
f32 C_VECMag(const Vec* v) { return sqrtf(C_VECSquareMag(v)); }

void C_VECNormalize(const Vec* src, Vec* dst) {
    f32 m = C_VECSquareMag(src);
    if (m > 0.0f) {
        m = 1.0f / sqrtf(m);
    }
    dst->x = src->x * m;
    dst->y = src->y * m;
    dst->z = src->z * m;
}

f32 C_VECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

f32 C_VECDistance(const Vec* a, const Vec* b) { return sqrtf(C_VECSquareDistance(a, b)); }

/* ---- PS* forwarders ------------------------------------------------------ */

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

void PSMTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
        dstBase[i].y = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
        dstBase[i].z = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    }
}
