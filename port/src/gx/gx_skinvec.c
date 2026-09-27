/* M47 (PLAN.md 62): the skin decode's commonest shape in AltiVec (--skinvec).
 *
 * The scalar loop (gx_draw.c DECODE_SKIN_JOB) multiplies each rest-pose
 * position and normal the way SetEnvelop does: fmuls m[1]*y, fmadds m[0]*x,
 * fmadds m[2]*z, fadds m[3] (psmtx_c.c, read off the object code).  Here the
 * same four operations run one output component per lane: vmaddfp(C1, y, -0)
 * is the product rounded once with its sign (the -0 addend leaves a +0
 * product +0), two vmaddfp are the two fused terms with one rounding each as
 * fmadds has, and vaddfp the last add -- the same operations in the same
 * order on the same operands, so the same bits, PROVIDED the vector unit is
 * in Java mode (VSCR[NJ] = 0: denormal inputs and results as IEEE, as the
 * FPU has them).  Each call puts the thread in Java mode if it is not, and
 * counts it.  The columns C0..C3 of each entry's matrix are laid out by
 * gx_skin.c at the pose (plain copies).  Only the shape pos f32 / nrm f32 /
 * tex0 f32 with no colour (68% of m436's list calls) is here; every other
 * shape and every index no entry multiplies (the copy, the unnamed) is the
 * scalar path's.  --skinverify with --skinvec checks it bit for bit. */
#include "gx_internal.h"
#include "gx_skin.h"

#include <string.h>

unsigned long gx_skinvec_calls, gx_skinvec_nj_set;

#ifdef __ALTIVEC__
#include <altivec.h>
#undef vector
#undef pixel
#undef bool
typedef __vector float vf;

/* the 12 bytes at p (4-aligned); lane 3 is whatever follows.  The second
 * load names p + 11, the element's last byte, so it never reads a block
 * past the element's own. */
static inline vf ld3(const f32* p) {
    const u8* b = (const u8*)p;
    return (vf)vec_perm(vec_ld(0, b), vec_ld(11, b), vec_lvsl(0, b));
}

/* three element stores; the rotate puts lane j where (d + 4j) selects */
static inline void st3(vf r, f32* d) {
    r = vec_perm(r, r, vec_lvsr(0, (const u8*)d));
    vec_ste(r, 0, d);
    vec_ste(r, 4, d);
    vec_ste(r, 8, d);
}

static inline vf mulv(const vf* C, vf v) {
    const vf nz = (vf){ -0.0f, -0.0f, -0.0f, -0.0f };
    vf t = vec_madd(C[1], vec_splat(v, 1), nz);
    t = vec_madd(C[0], vec_splat(v, 0), t);
    t = vec_madd(C[2], vec_splat(v, 2), t);
    return vec_add(t, C[3]);
}

static void java_mode(void) {
    union {
        __vector unsigned short v;
        u32 w[4];
    } u;
    u.v = vec_mfvscr();
    if (u.w[3] & 0x10000u) {
        gx_skinvec_nj_set++;
        u.w[3] &= ~0x10000u;
        vec_mtvscr(u.v);
    }
}

u32 gx_skinvec_n2c0t1(const GxDecJob* j) {
    const GxSkinDec* sd = j->skin;
    const f32* rp = sd->rest_pos;
    const f32* rn = sd->rest_nrm;
    const u16* pent = sd->pent;
    const u16* nent = sd->nent;
    const vf* PV = (const vf*)sd->PV;
    const vf* NV = (const vf*)sd->NV;
    const u32 npos = sd->npos, nnrm = sd->nnrm;
    const int sn = j->skin_nrm;
    const int PREFETCH = j->prefetch;
    const u8* p = j->p;
    const u8* end = j->end;
    const u32 count = j->count;
    const u32 stride = j->stride;
    const u8* pb = j->plan[0].base;
    const u32 ps = j->plan[0].stride;
    const u8* nb = j->plan[1].base;
    const u32 ns = j->plan[1].stride;
    const u8* tb = j->plan[2].base;
    const u32 ts = j->plan[2].stride;
    const int off_nrm = j->off_nrm, off_clr = j->off_clr, off_tex = j->off_tex;
    const int clr_const = j->clr_const;
    const u32 clr = j->clr;
    const int per = 6;
    u8* v = j->dst;
    u32 i;
    gx_skinvec_calls++;
    java_mode();
    for (i = 0; i < count && p + per <= end; i++, v += stride) {
        u32 ix, e;
        f32* dp;
        const u8* q;
        if (clr_const) {
            *(u32*)(v + off_clr) = clr;
        }
        if (PREFETCH && p + (PREFETCH + 1) * per <= end) {
            const u8* pn = p + PREFETCH * per;
            const u32 px = ((u32)pn[0] << 8) | pn[1];
            const u32 nx = ((u32)pn[2] << 8) | pn[3];
            __builtin_prefetch(rp + 3 * px);
            __builtin_prefetch(sn ? (const u8*)(rn + 3 * nx) : nb + (size_t)nx * ns);
            __builtin_prefetch(tb + (size_t)(((u32)pn[4] << 8) | pn[5]) * ts);
        }
        ix = ((u32)p[0] << 8) | p[1];
        dp = (f32*)v;
        e = ix < npos ? pent[ix] : GX_SKIN_UNNAMED;
        if (e - 1u < (u32)(GX_SKIN_UNNAMED - 1)) {
            st3(mulv(PV + 4 * e, ld3(rp + 3 * ix)), dp);
        } else if (e == 0) {
            dp[0] = rp[3 * ix + 0];
            dp[1] = rp[3 * ix + 1];
            dp[2] = rp[3 * ix + 2];
        } else {
            q = pb + (size_t)ix * ps;
            dp[0] = *(const f32*)(q + 0);
            dp[1] = *(const f32*)(q + 4);
            dp[2] = *(const f32*)(q + 8);
        }
        p += 2;
        ix = ((u32)p[0] << 8) | p[1];
        dp = (f32*)(v + off_nrm);
        e = sn && ix < nnrm ? nent[ix] : GX_SKIN_UNNAMED;
        if (e != GX_SKIN_UNNAMED) {
            st3(mulv(NV + 4 * e, ld3(rn + 3 * ix)), dp);
        } else {
            q = nb + (size_t)ix * ns;
            dp[0] = *(const f32*)(q + 0);
            dp[1] = *(const f32*)(q + 4);
            dp[2] = *(const f32*)(q + 8);
        }
        p += 2;
        ix = ((u32)p[0] << 8) | p[1];
        q = tb + (size_t)ix * ts;
        dp = (f32*)(v + off_tex);
        dp[0] = *(const f32*)(q + 0);
        dp[1] = *(const f32*)(q + 4);
        p += 2;
    }
    return i;
}
#else
u32 gx_skinvec_n2c0t1(const GxDecJob* j) {
    (void)j;
    return 0;
}
#endif
