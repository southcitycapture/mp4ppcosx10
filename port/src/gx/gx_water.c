/* M44 (PLAN.md 59): the water, three levels.
 *
 * Eleven minigames draw water through GX's indirect warp: a TEV stage's
 * texture coordinate offset, per pixel, by a texel of a second ("indirect",
 * or bump) map through a 2x3 matrix -- GXSetTevIndWarp(stage, ind, TRUE,
 * FALSE, ITM_n) with GXSetIndTexCoordScale(ITS_1, ITS_1), every site the same
 * shape (PLAN.md 49.6).  The Radeon 9000's fixed function has no dependent
 * read, and its fragment shader samples grey on this driver (50.14), so the
 * direct stage has been drawn unwarped: flat water.
 *
 * This is PLAN.md 3.9's option (a): the warp evaluated at the vertices.  For
 * each vertex of a warped draw, the indirect stage's texture coordinate (the
 * game's own texgen, evaluated by the CPU path exactly as the vertex program
 * would), a bilinear sample of the game's own indirect map (decoded on the
 * CPU from the bytes the port already decodes, gx_tex_cpu_rgba), the texel's
 * S, T, U (alpha, blue, green) biased by -128, the stage's matrix and 2^exp,
 * and the offset -- in texels of the direct stage's map, as the TEV adds it
 * -- added to that stage's coordinate.  GL interpolates the offset between
 * the vertices; a finer grid makes the interpolation closer to the per-pixel
 * warp.  It moves as the console's does because the game animates the map
 * and the texgen that reads it.  An approximation by design: the md5
 * references never reach a warped draw.
 *
 *   --water off    the direct stage alone, as before (flat water)
 *   --water cheap  the warp at the game's own vertices (its water grids are
 *                  30 x 30 and 30 x 36 already)
 *   --water full   each triangle subdivided (--watergrid N levels, default
 *                  1: four triangles a triangle) before the warp
 *   --water auto   (the default) by the machine's class and the screen: the
 *                  levels each screen's measured budget holds (PLAN.md 59)
 *
 * What is left out, and counted: an unsigned or replace-mode warp, a dynamic
 * matrix (ITM_S/T), an indirect map the CPU has no bytes for (an EFB copy),
 * two warps into one texture coordinate, a warped coordinate a plain stage
 * reads too.  None of the three pools' draws is refused by all of them. */
#include "gx_internal.h"

#include <math.h>
#include <string.h>

const u8* gx_tex_cpu_rgba(const GXTexObjPort* o, int* w, int* h);
int port_cur_mg_number(void);
int port_machine_class(void);
unsigned gl13_frame_number(void);

static unsigned long stat_draws, stat_verts, stat_warps, stat_refused[6];

/* The level for the screen in hand on the reference class (a dual 1 GHz G4
 * with a Radeon 9000): each warped screen measured at the three levels on
 * the G4 (PLAN.md 59.7, tools/m44_water_cost.py), the level whose cost its
 * cycle holds under 30 fps with a margin.  m430 has no room (its cheap level
 * is +2.1 ms on a cycle already at the bar); m423's full level costs 0.2 ms;
 * m442, m455 and m456 warp by an EFB copy the CPU has no bytes for, so their
 * level changes nothing.  A faster machine draws every one full, a machine
 * below the reference none. */
static int level_table(int mg) {
    switch (mg) {
        case 430: /* the rafts' river: no room */
            return GX_WATER_OFF;
        case 423: /* the lamp's water: full costs 0.2 ms */
            return GX_WATER_FULL;
        case 417: /* Makin' Waves: the ripple (+2.0 ms cheap, +13 ms full) */
        case 405: /* Mario Medley: the caustic (+2.4 / +9.8) */
        case 434: /* Cheep Cheep Sweep's pond: the caustic (the reflection's warp is refused) */
        case 410: /* (+0.0 / +0.6) */
        case 427: /* (+5.4 / +10.9) */
        default:
            return GX_WATER_CHEAP;
    }
}

int gx_water_level(void) {
    int cls;
    if (port_opt.water >= 0) {
        return port_opt.water;
    }
    cls = port_machine_class();
    if (cls == 2) {
        return GX_WATER_FULL;
    }
    if (cls == 0) {
        return GX_WATER_OFF;
    }
    return level_table(port_cur_mg_number());
}

/* the texel index under the map's wrap; n is a power of two for every GX
 * texture (checked by the plan), so repeat and mirror are masks */
static inline int wrap_i(int x, int n, int mode) {
    if (mode == GX_REPEAT) {
        return x & (n - 1);
    }
    if (mode == GX_MIRROR) {
        int m = x & (2 * n - 1);
        return m < n ? m : 2 * n - 1 - m;
    }
    return x < 0 ? 0 : x >= n ? n - 1 : x;
}

/* floor for |x| < 2^20 without libm's floorf or a divide: the 7455 has no
 * float-to-int move, so this is one fctiwz through the stack either way */
static inline int ifloor(float x) {
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

int gx_water_plan(GxWaterPlan* p) {
    int s, k;
    memset(p, 0, sizeof(*p));
    if (!gx.num_ind) {
        return 0;
    }
    for (k = 0; k < 4; k++) {
        p->ind[k].coord = 0xFF;
    }
    for (s = 0; s < gx.num_tev && s < GX_TEV_STAGES; s++) {
        const GXIndWarp* w = &gx.ind_warp[s];
        const GXTevStage* t = &gx.tev[s];
        const GXIndStage* is;
        const GXIndMtx* m;
        GXTexObjPort* direct;
        GXTexObjPort* bump;
        GxWaterWarp* ww;
        float sc;
        int r, c, j;
        if (t->direct || !w->on) {
            continue;
        }
        if (!w->sgn || w->rep || w->mtx < 1 || w->mtx > 3 || w->ind >= gx.num_ind || w->ind >= 4) {
            stat_refused[0]++;
            continue;
        }
        is = &gx.ind[w->ind];
        direct = gx_bound_tex(t->map);
        bump = gx_bound_tex(is->map);
        if (!direct || t->coord >= GX_TEXCOORDS || is->coord >= GX_TEXCOORDS || !bump ||
            direct->width == 0 || direct->height == 0) {
            stat_refused[1]++;
            continue;
        }
        if (p->ind[w->ind].coord == 0xFF) {
            int tw = 0, th = 0, q;
            const u8* rgba = gx_tex_cpu_rgba(bump, &tw, &th);
            if (!rgba || tw <= 0 || th <= 0 || (tw & (tw - 1)) || (th & (th - 1))) {
                stat_refused[2]++;
                continue;
            }
            /* the same map through the same coordinate as an earlier
             * indirect stage (m417's three): one sample serves both */
            p->ind[w->ind].same_as = 0xFF;
            for (q = 0; q < 4; q++) {
                if (q != w->ind && p->ind[q].coord == is->coord && p->ind[q].rgba == rgba &&
                    p->ind[q].div_s == (float)(1u << (is->scale_s & 7)) &&
                    p->ind[q].div_t == (float)(1u << (is->scale_t & 7))) {
                    p->ind[w->ind].same_as = (u8)q;
                    break;
                }
            }
            p->ind[w->ind].coord = is->coord;
            p->ind[w->ind].rgba = rgba;
            p->ind[w->ind].tw = tw;
            p->ind[w->ind].th = th;
            p->ind[w->ind].wrap_s = bump->wrap_s;
            p->ind[w->ind].wrap_t = bump->wrap_t;
            p->ind[w->ind].linear = bump->mag_filt != GX_NEAR;
            p->ind[w->ind].div_s = (float)(1u << (is->scale_s & 7));
            p->ind[w->ind].div_t = (float)(1u << (is->scale_t & 7));
        }
        /* one offset a coordinate, and no plain stage reading it */
        for (j = 0; j < p->nwarp; j++) {
            if (p->w[j].coord == t->coord) {
                break;
            }
        }
        if (j < p->nwarp) {
            stat_refused[3]++;
            continue;
        }
        for (j = 0; j < gx.num_tev && j < GX_TEV_STAGES; j++) {
            if (j != s && gx.tev[j].coord == t->coord &&
                (gx.tev[j].direct || !gx.ind_warp[j].on)) {
                break;
            }
        }
        if (j < gx.num_tev && j < GX_TEV_STAGES) {
            stat_refused[4]++;
            continue;
        }
        m = &gx.ind_mtx[w->mtx - 1];
        sc = ldexpf(1.0f, m->exp);
        ww = &p->w[p->nwarp++];
        ww->coord = t->coord;
        ww->ind = w->ind;
        for (r = 0; r < 2; r++) {
            float size = r == 0 ? (float)direct->width : (float)direct->height;
            for (c = 0; c < 3; c++) {
                ww->f[r][c] = m->m[r][c] * sc / size;
            }
        }
    }
    return p->nwarp;
}

/* The 7455 has no integer-to-float instruction: every (float)byte is a store,
 * a load of the same doubleword and a subtract -- a load-hit-store stall a
 * conversion, twelve of them a bilinear sample.  The biased texel values come
 * from a table instead (M17's trick for the decode). */
static float biased[256];
static int biased_ok;

/* a bilinear (or nearest) sample of the map's S, T, U -- alpha, blue, green
 * -- biased by -128 */
static inline void sample(const GxWaterInd* in, float s, float t, float out[3]) {
    const u8* px = in->rgba;
    int w = in->tw, h = in->th;
    float x = s * (float)w, y = t * (float)h;
    if (!(x > -1048576.0f && x < 1048576.0f && y > -1048576.0f && y < 1048576.0f)) {
        out[0] = out[1] = out[2] = 0.0f; /* a NaN or runaway coordinate: no offset */
        return;
    }
    if (!in->linear) {
        int xi = wrap_i(ifloor(x), w, in->wrap_s), yi = wrap_i(ifloor(y), h, in->wrap_t);
        const u8* q = px + ((size_t)yi * w + xi) * 4;
        out[0] = biased[q[3]];
        out[1] = biased[q[2]];
        out[2] = biased[q[1]];
        return;
    }
    x -= 0.5f;
    y -= 0.5f;
    {
        int ix = ifloor(x), iy = ifloor(y);
        float ax = x - (float)ix, ay = y - (float)iy;
        int x0 = wrap_i(ix, w, in->wrap_s), x1 = wrap_i(ix + 1, w, in->wrap_s);
        int y0 = wrap_i(iy, h, in->wrap_t), y1 = wrap_i(iy + 1, h, in->wrap_t);
        const u8* a = px + ((size_t)y0 * w + x0) * 4;
        const u8* b = px + ((size_t)y0 * w + x1) * 4;
        const u8* c = px + ((size_t)y1 * w + x0) * 4;
        const u8* d = px + ((size_t)y1 * w + x1) * 4;
        float wa = (1.0f - ax) * (1.0f - ay), wb = ax * (1.0f - ay);
        float wc = (1.0f - ax) * ay, wd = ax * ay;
        out[0] = wa * biased[a[3]] + wb * biased[b[3]] + wc * biased[c[3]] + wd * biased[d[3]];
        out[1] = wa * biased[a[2]] + wb * biased[b[2]] + wc * biased[c[2]] + wd * biased[d[2]];
        out[2] = wa * biased[a[1]] + wb * biased[b[1]] + wc * biased[c[1]] + wd * biased[d[1]];
    }
}

void gx_water_offsets(const GxWaterPlan* p, u8* out, int n, int stride, int off_tex) {
    int v, j;
    float inv_s[4], inv_t[4];
    if (!biased_ok) {
        int i;
        for (i = 0; i < 256; i++) {
            biased[i] = (float)i - 128.0f;
        }
        biased_ok = 1;
    }
    for (j = 0; j < 4; j++) {
        inv_s[j] = p->ind[j].coord != 0xFF ? 1.0f / p->ind[j].div_s : 0.0f;
        inv_t[j] = p->ind[j].coord != 0xFF ? 1.0f / p->ind[j].div_t : 0.0f;
    }
    stat_draws++;
    stat_verts += (unsigned long)n;
    stat_warps += (unsigned long)p->nwarp;
    for (v = 0; v < n; v++) {
        f32* tex = (f32*)(out + (size_t)v * stride + off_tex);
        float stu[4][3];
        int have = 0;
        float d[GX_TEV_STAGES][2];
        for (j = 0; j < p->nwarp; j++) {
            const GxWaterWarp* w = &p->w[j];
            const GxWaterInd* in = &p->ind[w->ind];
            const float* q;
            if (!(have & (1 << w->ind))) {
                if (in->same_as != 0xFF && (have & (1 << in->same_as))) {
                    stu[w->ind][0] = stu[in->same_as][0];
                    stu[w->ind][1] = stu[in->same_as][1];
                    stu[w->ind][2] = stu[in->same_as][2];
                } else {
                    sample(in, tex[2 * in->coord] * inv_s[w->ind],
                           tex[2 * in->coord + 1] * inv_t[w->ind], stu[w->ind]);
                }
                have |= 1 << w->ind;
            }
            q = stu[w->ind];
            d[j][0] = w->f[0][0] * q[0] + w->f[0][1] * q[1] + w->f[0][2] * q[2];
            d[j][1] = w->f[1][0] * q[0] + w->f[1][1] * q[1] + w->f[1][2] * q[2];
        }
        /* all offsets from the coordinates as the texgens made them, then
         * added (an indirect coordinate is never a warped one: checked) */
        for (j = 0; j < p->nwarp; j++) {
            tex[2 * p->w[j].coord] += d[j][0];
            tex[2 * p->w[j].coord + 1] += d[j][1];
        }
    }
}

void gx_water_report(void) {
    if (stat_draws || stat_refused[0] || stat_refused[1] || stat_refused[2] ||
        stat_refused[3] || stat_refused[4]) {
        port_log("port> water (M44): %lu warped draws, %lu vertices, %lu stage warps; refused: "
                 "shape %lu, no texture %lu, map not on the CPU %lu, coordinate shared %lu, "
                 "read unwarped too %lu (--water %s)\n",
                 stat_draws, stat_verts, stat_warps, stat_refused[0], stat_refused[1],
                 stat_refused[2], stat_refused[3], stat_refused[4],
                 port_opt.water < 0 ? "auto" : port_opt.water == 0 ? "off"
                 : port_opt.water == 1 ? "cheap" : "full");
    }
}
