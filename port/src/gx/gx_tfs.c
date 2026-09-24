/* The indirect warp on GL_ATI_text_fragment_shader (M35, PLAN.md 50.13).
 *
 * GXSetTevIndWarp offsets a TEV stage's texel coordinate by a texel of an
 * *indirect* map through a 2x3 matrix: the water of Mario Medley (the
 * caustic), Makin' Waves and Cheep Cheep Sweep (the ripple) is a scene copy
 * sampled through a bump map.  GL 1.3's fixed function has no dependent
 * texture read; the Radeon 9000 has one, behind `GL_ATI_text_fragment_shader`
 * (the R200's two-pass register machine: six samples, eight instruction pairs
 * a pass, eight constants, a second pass whose SampleMap may take its
 * coordinate from a register the first pass computed).  PLAN.md 49.6 read the
 * game's every warp as ONE shape -- signed offsets, no replace, ITS_1 -- and
 * this file builds that shape, and the TEV chain around it, as a program:
 *
 *   prelim pass:   sample the indirect map(s); pass each warped stage's
 *                  coordinate into its register; per (map, matrix) one
 *                  MUL of the signed texel by the matrix's row scales (the
 *                  constant, at `eighth` for its precision); one ADD per
 *                  warped stage
 *   output pass:   sample every direct stage's map, the warped ones through
 *                  their register; then the TEV stages as ALU pairs --
 *                  (d + lerp(a, b, c)) * scale + bias, clamped, is MOV /
 *                  ADD / MUL / MAD / LERP, and a LERP + ADD when all four
 *                  inputs are live
 *
 * The scale of an offset: the hardware reads the indirect texel as an 8-bit
 * integer, biased by -128 when signed, and the matrix times 2^exp gives an
 * offset in *texels of the direct map*; in the program the texel is the
 * sample's channel doubled and biased (2 * (v - 0.5) = (val - 127.5) /
 * 127.5), and the constant carries |m| * 127.5 * 2^exp * fold / size --
 * the NPOT fold of gl13.c, so the offset lands in the padded GL texture's
 * space -- with the sign in the sample's `neg` when both rows share it and
 * in the constant's own `2x.bias` when they do not.  Which indirect channel
 * a row reads (S = the texel's alpha, T = blue, U = green, the hardware's
 * order) is a swap table on the map's decode (gx_tex.c, exact), so one MUL
 * covers both rows.  A matrix with two live entries in one row is not
 * built (no game has one).
 *
 * Everything the shader cannot say -- a comparison op, a swap GL cannot
 * select, more units, pairs or constants than the pass has, a program the
 * driver refuses -- falls back to the fixed-function path with the direct
 * stage unwarped, counted by reason in the report.  `--notfs` is the A/B;
 * `--tfsprobe` compiles the three games' chains from their state, draws a
 * read-back test and prints; `--tfslog` prints every program compiled.
 */
#include "gx_internal.h"

unsigned gl13_frame_number(void);
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#include "gx_rt.h"
#endif

#define TFS_TARGET 0x8200u /* GL_TEXT_FRAGMENT_SHADER_ATI */
#define TFS_MAXK 8
#define TFS_MAXREG 6
#define TFS_MAXPAIRS 8
#define TFS_TEXT 6144

/* ---- the layout: which unit samples what ------------------------------------ */

typedef struct TfsLayout {
    int on;                      /* 1 = the shader's draw; 0 = not (why in `why`) */
    const char* why;
    int whyi;                    /* its index in why_names, or -1 */
    int stages;
    int nunits;
    u8 unit_kind[GX_TEX_UNITS];  /* 0 none, 1 a direct stage's map, 2 an indirect map */
    u8 unit_idx[GX_TEX_UNITS];   /* the stage (kind 1) / the indirect stage (kind 2) */
    u8 unit_coord[GX_TEX_UNITS];
    u8 unit_map[GX_TEX_UNITS];
    u8 unit_swap[GX_TEX_UNITS];  /* the decode swap the unit's map is bound through */
    u8 warped[GX_TEV_STAGES];    /* the stage's coordinate is offset */
    u8 warp_unit[GX_TEV_STAGES]; /* the indirect unit that offsets it */
    u8 warp_mtx[GX_TEV_STAGES];  /* 0..2 */
    u8 warp_col[GX_TEV_STAGES][2]; /* the column each row reads (0 S, 1 T, 2 U), 0xff none */
} TfsLayout;

static unsigned long stat_draws, stat_fallback, stat_compiles, stat_refused;
static unsigned long stat_why[8];
static const char* why_names[8] = {
    "unsigned or replace-mode warp", "an indirect coordinate scale", "a full matrix row",
    "no unit left for the indirect map", "a stage shape the pass cannot say",
    "over the pass's pairs or constants", "the driver refused the program", "no register left"
};

static int mtx_row_col(const GXIndMtx* m, int row, int* col, float* val) {
    int c, n = 0;
    *col = -1;
    *val = 0.0f;
    for (c = 0; c < 3; c++) {
        if (m->m[row][c] != 0.0f) {
            n++;
            *col = c;
            *val = m->m[row][c];
        }
    }
    return n;
}

/* the GX channel an indirect column is read from (the hardware's .abg) */
static u8 col_chan(int col) {
    return col == 0 ? GX_CH_ALPHA : col == 1 ? GX_CH_BLUE : col == 2 ? GX_CH_GREEN : GX_CH_ALPHA;
}

static void layout_decide(TfsLayout* L) {
    int stages = gx.num_tev ? gx.num_tev : 1;
    int i, nwarp = 0;
    memset(L, 0, sizeof(*L));
    L->stages = stages;
    L->why = NULL;
    L->whyi = -1;
    if (!port_opt.tfs || !gl13_have_tfs) {
        return;
    }
    if (!gx.num_ind && !(port_opt.tfsall > 0 && stages >= port_opt.tfsall)) {
        return;
    }
    for (i = 0; i < stages && i < GX_TEV_STAGES; i++) {
        const GXTevStage* s = &gx.tev[i];
        const GXIndWarp* w = &gx.ind_warp[i];
        const GXIndMtx* m;
        int k, c0, c1;
        float v0, v1;
        if (!w->on || s->direct || gx.ind_tile[i].on || w->mtx < 1 || w->mtx > 3) {
            continue;
        }
        if (gx_bound_tex(s->map) == NULL || s->coord >= gx.num_texgens) {
            continue; /* a warp on a stage that samples nothing offsets nothing */
        }
        k = w->ind;
        if (k >= gx.num_ind || k >= 4) {
            continue;
        }
        if (!w->sgn || w->rep) {
            L->why = why_names[0];
            L->whyi = 0;
            return;
        }
        if (gx.ind[k].scale_s != GX_ITS_1 || gx.ind[k].scale_t != GX_ITS_1) {
            L->why = why_names[1];
            L->whyi = 1;
            return;
        }
        if (gx_bound_tex(gx.ind[k].map) == NULL || gx.ind[k].coord >= gx.num_texgens) {
            continue; /* no map to read the offset from: unwarped, as the hardware would read zeros? no -- as the port did */
        }
        m = &gx.ind_mtx[w->mtx - 1];
        if (mtx_row_col(m, 0, &c0, &v0) > 1 || mtx_row_col(m, 1, &c1, &v1) > 1) {
            L->why = why_names[2];
            L->whyi = 2;
            return;
        }
        L->warped[i] = 1;
        L->warp_mtx[i] = (u8)(w->mtx - 1);
        L->warp_col[i][0] = (u8)(c0 < 0 ? 0xff : c0);
        L->warp_col[i][1] = (u8)(c1 < 0 ? 0xff : c1);
        nwarp++;
    }
    if (!nwarp && !(port_opt.tfsall > 0 && stages >= port_opt.tfsall)) {
        return;
    }
    if (stages > gl13_max_tex_units || stages > TFS_MAXREG) {
        L->why = why_names[3];
        L->whyi = 3;
        return;
    }
    /* the direct stages on units 0..stages-1, as the fixed-function path */
    for (i = 0; i < stages; i++) {
        const GXTevStage* s = &gx.tev[i];
        if (gx_bound_tex(s->map) != NULL && s->coord < gx.num_texgens) {
            L->unit_kind[i] = 1;
            L->unit_idx[i] = (u8)i;
            L->unit_coord[i] = s->coord;
            L->unit_map[i] = s->map;
            L->unit_swap[i] = (u8)(gx.swap_tbl[s->tex_swap & 3][0] | (gx.swap_tbl[s->tex_swap & 3][1] << 2) |
                                   (gx.swap_tbl[s->tex_swap & 3][2] << 4) | (gx.swap_tbl[s->tex_swap & 3][3] << 6));
        }
    }
    L->nunits = stages;
    /* the indirect maps after them, one unit per (coordinate, map, swap) */
    for (i = 0; i < stages; i++) {
        const GXIndWarp* w = &gx.ind_warp[i];
        const GXIndStage* is;
        u8 tbl[4], swap;
        int u;
        if (!L->warped[i]) {
            continue;
        }
        is = &gx.ind[w->ind];
        tbl[0] = col_chan(L->warp_col[i][0] == 0xff ? -1 : L->warp_col[i][0]);
        tbl[1] = col_chan(L->warp_col[i][1] == 0xff ? -1 : L->warp_col[i][1]);
        tbl[2] = GX_CH_ALPHA;
        tbl[3] = GX_CH_ALPHA;
        swap = (u8)(tbl[0] | (tbl[1] << 2) | (tbl[2] << 4) | (tbl[3] << 6));
        for (u = stages; u < L->nunits; u++) {
            if (L->unit_kind[u] == 2 && L->unit_coord[u] == is->coord && L->unit_map[u] == is->map &&
                L->unit_swap[u] == swap) {
                break;
            }
        }
        if (u == L->nunits) {
            if (u >= gl13_max_tex_units || u >= TFS_MAXREG) {
                L->why = why_names[3];
                L->whyi = 3;
                memset(L->warped, 0, sizeof(L->warped));
                return;
            }
            L->unit_kind[u] = 2;
            L->unit_idx[u] = w->ind;
            L->unit_coord[u] = is->coord;
            L->unit_map[u] = is->map;
            L->unit_swap[u] = swap;
            L->nunits = u + 1;
        }
        L->warp_unit[i] = (u8)u;
    }
    L->on = 1;
}

/* M41 (PLAN.md 56): inside one draw's state walk (gx_draw.c draw_apply ->
 * gx_unit_memo) the layout is a function of GX state no setter can change,
 * and it was decided afresh for every unit by every caller -- the vertex
 * program's key, the register-fix shape, the z pre-pass, the CPU path's
 * arrays: a dozen memsets and scans a draw.  Decided once a draw instead. */
static int tfs_memo_on, tfs_memo_valid;
static TfsLayout tfs_memo_L;
void gx_tfs_memo(int on) {
    tfs_memo_on = on;
    tfs_memo_valid = 0;
}

int gx_tfs_layout(int u, u8* coord, u8* map) {
    TfsLayout Lown;
    const TfsLayout* L = &Lown;
    if (tfs_memo_on) {
        if (!tfs_memo_valid) {
            layout_decide(&tfs_memo_L);
            tfs_memo_valid = 1;
        }
        L = &tfs_memo_L;
    } else {
        layout_decide(&Lown);
    }
    if (!L->on) {
        return -1;
    }
    if (u < 0 || u >= L->nunits || !L->unit_kind[u]) {
        return 0;
    }
    *coord = L->unit_coord[u];
    *map = L->unit_map[u];
    return 1;
}

/* ---- the program text -------------------------------------------------------- */

typedef struct Src {
    char s[28];
    u8 zero, one;
} Src;

typedef struct Gen {
    const TfsLayout* L;
    char body[TFS_TEXT];
    int blen;
    int fail; /* a why index + 1 */
    /* constants */
    int nk;
    float kval[TFS_MAXK][4];
    u8 kdyn[TFS_MAXK];
    /* registers */
    u8 reg_sampled2[TFS_MAXREG]; /* holds a sample in the output pass */
    u8 reg_taken[TFS_MAXREG];    /* holds a TEV register / is r0 */
    int where_c[4], where_a[4];  /* PREV, REG0..2: the register, or -1 = still the constant */
    int reg_of[4];               /* the register a TEV register was given, or -1 */
    int pairs;
    int ncol, nalpha;            /* instructions of the current stage, for the pairing */
    /* the prelim pass's MULs: (indirect unit, matrix, direct map) -> scratch */
    int nmul;
    u8 mul_unit[8], mul_mtx[8], mul_dmap[8], mul_k[8], mul_biased[8], mul_neg[8];
    u8 mul_dunit[8];
    u8 mul_mod[8];               /* the MUL's destination modifier (dst_mod) */
} Gen;

static void emit(Gen* g, const char* fmt, ...) {
    va_list ap;
    int n;
    if (g->blen >= TFS_TEXT - 1) {
        g->fail = 6;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(g->body + g->blen, (size_t)(TFS_TEXT - g->blen), fmt, ap);
    va_end(ap);
    if (n < 0 || g->blen + n >= TFS_TEXT) {
        g->fail = 6;
        g->blen = TFS_TEXT - 1;
        return;
    }
    g->blen += n;
}

static int kslot(Gen* g, const float* v, int dyn) {
    int i;
    if (!dyn) {
        for (i = 0; i < g->nk; i++) {
            if (!g->kdyn[i] && memcmp(g->kval[i], v, 16) == 0) {
                return i;
            }
        }
    }
    if (g->nk >= TFS_MAXK) {
        g->fail = 6;
        return 0;
    }
    memcpy(g->kval[g->nk], v, 16);
    g->kdyn[g->nk] = (u8)dyn;
    return g->nk++;
}

static void src_set(Src* s, const char* t) {
    memset(s, 0, sizeof(*s));
    snprintf(s->s, sizeof(s->s), "%s", t);
}
static Src src_zero(void) { Src s; src_set(&s, "0"); s.zero = 1; return s; }
static Src src_one(void) { Src s; src_set(&s, "1"); s.one = 1; return s; }
static Src src_const(Gen* g, const float* v, int alpha_rep) {
    Src s;
    int k = kslot(g, v, 0);
    char t[28];
    snprintf(t, sizeof(t), alpha_rep ? "c%d.a" : "c%d", k);
    src_set(&s, t);
    return s;
}
static Src src_reg(int r, int alpha_rep) {
    Src s;
    char t[28];
    snprintf(t, sizeof(t), alpha_rep ? "r%d.a" : "r%d", r);
    src_set(&s, t);
    return s;
}

/* a TEV register (0 PREV, 1..3 REG0..2) read as a colour or as an alpha */
static Src src_tevreg(Gen* g, int n, int as_alpha, int in_alpha_op) {
    int r = as_alpha ? g->where_a[n] : g->where_c[n];
    if (r >= 0) {
        return src_reg(r, as_alpha && !in_alpha_op);
    }
    {
        float v[4];
        v[0] = gx.tev_reg[n].r / 255.0f;
        v[1] = gx.tev_reg[n].g / 255.0f;
        v[2] = gx.tev_reg[n].b / 255.0f;
        v[3] = gx.tev_reg[n].a / 255.0f;
        return src_const(g, v, as_alpha && !in_alpha_op);
    }
}

static Src src_konst(Gen* g, const GXTevStage* s, int in_alpha_op) {
    float kc[4], ka[4], v[4];
    gx_tev_stage_konst(s, 0, kc);
    gx_tev_stage_konst(s, 1, ka);
    v[0] = kc[0];
    v[1] = kc[1];
    v[2] = kc[2];
    v[3] = ka[3];
    (void)in_alpha_op;
    return src_const(g, v, 0);
}

static Src src_ras(Gen* g, const GXTevStage* s, int as_alpha, int in_alpha_op) {
    const u8* t = gx.swap_tbl[s->ras_swap & 3];
    Src r;
    if (as_alpha) {
        if (t[3] != GX_CH_ALPHA) {
            g->fail = 5;
        }
        src_set(&r, in_alpha_op ? "color0" : "color0.a");
        return r;
    }
    if (t[0] == GX_CH_RED && t[1] == GX_CH_GREEN && t[2] == GX_CH_BLUE) {
        src_set(&r, "color0");
    } else if (t[0] == GX_CH_ALPHA && t[1] == GX_CH_ALPHA && t[2] == GX_CH_ALPHA) {
        src_set(&r, "color0.a");
    } else {
        g->fail = 5;
        src_set(&r, "color0");
    }
    return r;
}

static Src csrc(Gen* g, int stage, u8 in) {
    const GXTevStage* s = &gx.tev[stage];
    float half[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    switch (in) {
        case GX_CC_CPREV: return src_tevreg(g, 0, 0, 0);
        case GX_CC_APREV: return src_tevreg(g, 0, 1, 0);
        case GX_CC_C0: return src_tevreg(g, 1, 0, 0);
        case GX_CC_A0: return src_tevreg(g, 1, 1, 0);
        case GX_CC_C1: return src_tevreg(g, 2, 0, 0);
        case GX_CC_A1: return src_tevreg(g, 2, 1, 0);
        case GX_CC_C2: return src_tevreg(g, 3, 0, 0);
        case GX_CC_A2: return src_tevreg(g, 3, 1, 0);
        case GX_CC_TEXC:
            if (!g->L->unit_kind[stage]) { return src_one(); } /* the white texture, as the fixed function */
            return src_reg(stage, 0);
        case GX_CC_TEXA:
            if (!g->L->unit_kind[stage]) { return src_one(); }
            return src_reg(stage, 1);
        case GX_CC_RASC: return src_ras(g, s, 0, 0);
        case GX_CC_RASA: return src_ras(g, s, 1, 0);
        case GX_CC_ONE: return src_one();
        case GX_CC_HALF: return src_const(g, half, 0);
        case GX_CC_KONST: return src_konst(g, s, 0);
        default: return src_zero();
    }
}

static Src asrc(Gen* g, int stage, u8 in) {
    const GXTevStage* s = &gx.tev[stage];
    switch (in) {
        case GX_CA_APREV: return src_tevreg(g, 0, 1, 1);
        case GX_CA_A0: return src_tevreg(g, 1, 1, 1);
        case GX_CA_A1: return src_tevreg(g, 2, 1, 1);
        case GX_CA_A2: return src_tevreg(g, 3, 1, 1);
        case GX_CA_TEXA:
            if (!g->L->unit_kind[stage]) { return src_one(); }
            return src_reg(stage, 0);
        case GX_CA_RASA: return src_ras(g, s, 1, 1);
        case GX_CA_KONST: return src_konst(g, s, 1);
        default: return src_zero();
    }
}

static int reg_free_out(Gen* g, int avoid) {
    int r;
    for (r = 1; r < TFS_MAXREG && r < gl13_max_tex_units; r++) {
        if (!g->reg_sampled2[r] && !g->reg_taken[r] && r != avoid) {
            return r;
        }
    }
    return -1;
}

static const float k_half[4] = { 0.5f, 0.5f, 0.5f, 0.5f };

/* one channel of one stage: (d +/- lerp(a, b, c) + bias) * scale, clamped */
static void emit_stage_channel(Gen* g, int alpha, int dst, Src a, Src b, Src c, Src d, u8 op,
                               u8 bias, u8 scale, u8 clamp) {
    const char* mask = alpha ? "a" : "rgb";
    const char* smod = scale == 1 ? ".2x" : scale == 2 ? ".4x" : scale == 3 ? ".half" : "";
    const char* cmod = clamp ? ".sat" : "";
    char tail[16];  /* the scale and the clamp, on the channel's last instruction */
    char mods[16];  /* ...which is the bias ADD when there is one */
    int n = 0;
    Src x;   /* L when it is one source */
    int lk;  /* 0 = x, 1 = b*c, 2 = a*(1-c), 3 = lerp */
    int sub = op == GX_TEV_SUB;
    if (op != GX_TEV_ADD && op != GX_TEV_SUB) {
        g->fail = 5;
        return;
    }
    snprintf(tail, sizeof(tail), "%s%s", smod, cmod);
    snprintf(mods, sizeof(mods), "%s", bias ? "" : tail);
    memset(&x, 0, sizeof(x));
    if (c.zero || (!b.zero && !strcmp(a.s, b.s))) {
        x = a;
        lk = 0;
    } else if (a.zero) {
        if (b.one) { x = c; lk = 0; }
        else if (c.one) { x = b; lk = 0; }
        else { lk = 1; }
    } else if (b.zero) {
        if (c.one) { x = src_zero(); lk = 0; }
        else { lk = 2; }
    } else {
        lk = 3;
    }
    if (lk == 3) {
        /* LERP t, c, b, a = c*b + (1-c)*a: straight into dst when d is 0 */
        if (d.zero && !sub) {
            emit(g, "  LERP r%d.%s%s, %s, %s, %s;\n", dst, mask, mods, c.s, b.s, a.s);
            n++;
        } else {
            int tmp = reg_free_out(g, dst);
            if (tmp < 0) {
                g->fail = 8;
                return;
            }
            emit(g, "  LERP r%d.%s, %s, %s, %s;\n", tmp, mask, c.s, b.s, a.s);
            n++;
            x = src_reg(tmp, 0);
            lk = 0;
        }
    }
    if (lk == 0 && n == 0 && x.zero) {
        /* lerp is 0: the stage is d alone (or nothing) */
        emit(g, "  MOV r%d.%s%s, %s;\n", dst, mask, mods, d.zero ? "0" : d.s);
        n++;
    } else if (lk == 0 && n == 0) {
        if (d.zero) {
            if (sub) {
                emit(g, "  SUB r%d.%s%s, 0, %s;\n", dst, mask, mods, x.s);
            } else {
                emit(g, "  MOV r%d.%s%s, %s;\n", dst, mask, mods, x.s);
            }
        } else if (sub) {
            emit(g, "  SUB r%d.%s%s, %s, %s;\n", dst, mask, mods, d.s, x.s);
        } else {
            emit(g, "  ADD r%d.%s%s, %s, %s;\n", dst, mask, mods, x.s, d.s);
        }
        n++;
    } else if (lk == 0) {
        /* the LERP's temporary, and d */
        if (sub) {
            emit(g, "  SUB r%d.%s%s, %s, %s;\n", dst, mask, mods, d.s, x.s);
        } else {
            emit(g, "  ADD r%d.%s%s, %s, %s;\n", dst, mask, mods, x.s, d.s);
        }
        n++;
    } else if (lk == 1 || lk == 2) {
        /* b*c (+d), or a*(1-c) (+d) */
        const char* p = lk == 1 ? b.s : a.s;
        char q[40];
        snprintf(q, sizeof(q), lk == 1 ? "%s" : "%s.comp", c.s);
        if (d.zero) {
            emit(g, "  MUL r%d.%s%s, %s%s, %s;\n", dst, mask, mods, p, sub ? ".neg" : "", q);
        } else {
            emit(g, "  MAD r%d.%s%s, %s%s, %s, %s;\n", dst, mask, mods, p, sub ? ".neg" : "", q, d.s);
        }
        n++;
    }
    if (bias) {
        emit(g, "  %s r%d.%s%s, r%d, c%d;\n", bias == GX_TB_SUBHALF ? "SUB" : "ADD", dst, mask, tail, dst,
             kslot(g, k_half, 0));
        n++;
    }
    if (alpha) {
        g->nalpha += n;
    } else {
        g->ncol += n;
    }
}

/* The offset constant of one MUL from the matrix and the maps bound now:
 * |m| * 127.5 * 2^exp * fold / size per row, times 8 for the `eighth`; and
 * how the signs go -- both rows alike: the sample's `neg`; unlike: the
 * constant's own `2x.bias` carries them (c = 0.5 + k/2). */
/* the destination modifiers a MUL can carry, by the power of two they scale by */
static const char* const dst_mod[7] = { ".eighth", ".quarter", ".half", "", ".2x", ".4x", ".8x" };
static void mul_consts(int mtx, int dunit, const float* dims, float* v, int* neg, int* biased, int* mod) {
    const GXIndMtx* im = &gx.ind_mtx[mtx];
    float e = (float)ldexp(127.5, im->exp);
    float k0, k1, big, sc;
    int c0, c1, m;
    mtx_row_col(im, 0, &c0, &k0);
    mtx_row_col(im, 1, &c1, &k1);
    k0 = k0 * e * dims[dunit * 4 + 2] / (dims[dunit * 4 + 0] > 0 ? dims[dunit * 4 + 0] : 1.0f);
    k1 = k1 * e * dims[dunit * 4 + 3] / (dims[dunit * 4 + 1] > 0 ? dims[dunit * 4 + 1] : 1.0f);
    /* the constant is at most 1 (and eight bits): the MUL's destination
     * modifier carries the power of two that brings the larger row under it
     * -- eighth for a small ripple (m405's caustic, 0.006 a texel step),
     * 2x/4x for m434's reflection (half the map per texel step) */
    big = (float)fabs(k0) > (float)fabs(k1) ? (float)fabs(k0) : (float)fabs(k1);
    *biased = !((k0 < 0) == (k1 < 0) || k0 == 0.0f || k1 == 0.0f);
    if (*biased) {
        big *= 2.0f; /* the biased form (c = 0.5 + k/2) halves the constant's reach */
    }
    for (m = 0, sc = 0.125f; m < 6 && big > sc; m++, sc *= 2.0f) {
    }
    *mod = m;
    k0 /= sc;
    k1 /= sc;
    if (!*biased) {
        *neg = (k0 < 0) || (k1 < 0);
        v[0] = (float)fabs(k0);
        v[1] = (float)fabs(k1);
    } else {
        *neg = 0;
        v[0] = 0.5f + k0 * 0.5f;
        v[1] = 0.5f + k1 * 0.5f;
    }
    v[2] = 0.0f;
    v[3] = 0.0f;
    if (v[0] > 1.0f) v[0] = 1.0f;
    if (v[1] > 1.0f) v[1] = 1.0f;
    if (v[0] < 0.0f) v[0] = 0.0f;
    if (v[1] < 0.0f) v[1] = 0.0f;
}

static int build_text(Gen* g, const TfsLayout* L, const float* dims /* per unit: w, h, su, sv */,
                      char* out, int cap) {
    int stages = L->stages;
    int i, u, r;
    int scratch1 = -1;
    memset(g, 0, sizeof(*g));
    g->L = L;
    for (i = 0; i < 4; i++) {
        g->where_c[i] = -1;
        g->where_a[i] = -1;
        g->reg_of[i] = -1;
    }
    /* ---- the prelim pass (only a warp needs one) ---- */
    for (i = 0; i < stages; i++) {
        if (L->warped[i]) {
            break;
        }
    }
    if (i == stages) {
        goto output_pass;
    }
    emit(g, "StartPrelimPass;\n");
    for (u = 0; u < L->nunits; u++) {
        if (L->unit_kind[u] == 2) {
            emit(g, "  SampleMap r%d, t%d.stq_dq;\n", u, u);
        }
    }
    for (i = 0; i < stages; i++) {
        if (L->warped[i]) {
            emit(g, "  PassTexCoord r%d, t%d.stq_dq;\n", i, i);
        }
    }
    /* a scratch register the first pass owns: a unit that samples nothing in
     * it (an unwarped direct stage's, or an unused one) */
    for (r = 0; r < TFS_MAXREG && r < gl13_max_tex_units; r++) {
        if (L->unit_kind[r] != 2 && !(r < stages && L->warped[r])) {
            scratch1 = r;
            break;
        }
    }
    if (scratch1 < 0) {
        g->fail = 8;
        return 0;
    }
    /* the MULs: one per (indirect unit, matrix, direct map) */
    for (i = 0; i < stages; i++) {
        int m, k, neg, biased, mod;
        float v[4];
        if (!L->warped[i]) {
            continue;
        }
        for (m = 0; m < g->nmul; m++) {
            if (g->mul_unit[m] == L->warp_unit[i] && g->mul_mtx[m] == L->warp_mtx[i] &&
                g->mul_dmap[m] == L->unit_map[i]) {
                break;
            }
        }
        if (m < g->nmul) {
            continue;
        }
        if (g->nmul >= 8) {
            g->fail = 6;
            return 0;
        }
        mul_consts(L->warp_mtx[i], i, dims, v, &neg, &biased, &mod);
        g->mul_mod[g->nmul] = (u8)mod;
        g->mul_unit[g->nmul] = L->warp_unit[i];
        g->mul_mtx[g->nmul] = L->warp_mtx[i];
        g->mul_dmap[g->nmul] = L->unit_map[i];
        g->mul_dunit[g->nmul] = (u8)i;
        g->mul_biased[g->nmul] = (u8)biased;
        g->mul_neg[g->nmul] = (u8)neg;
        k = kslot(g, v, 1);
        g->mul_k[g->nmul] = (u8)k;
        emit(g, "  MUL r%d.rg%s, r%d%s.2x.bias, c%d%s;\n", scratch1, dst_mod[mod], L->warp_unit[i],
             neg ? ".neg" : "", k, biased ? ".2x.bias" : "");
        g->pairs++;
        {
            int j;
            for (j = 0; j < stages; j++) {
                if (L->warped[j] && L->warp_unit[j] == L->warp_unit[i] && L->warp_mtx[j] == L->warp_mtx[i] &&
                    L->unit_map[j] == L->unit_map[i]) {
                    emit(g, "  ADD r%d.rg, r%d, r%d;\n", j, j, scratch1);
                    g->pairs++;
                }
            }
        }
        g->nmul++;
    }
    if (g->pairs > TFS_MAXPAIRS) {
        g->fail = 6;
        return 0;
    }
    emit(g, "EndPass;\n");
output_pass:
    /* ---- the output pass ---- */
    g->pairs = 0;
    emit(g, "StartOutputPass;\n");
    for (u = 0; u < stages; u++) {
        if (L->unit_kind[u] == 1) {
            if (L->warped[u]) {
                emit(g, "  SampleMap r%d, r%d.str;\n", u, u);
            } else {
                emit(g, "  SampleMap r%d, t%d.stq_dq;\n", u, u);
            }
            g->reg_sampled2[u] = 1;
        }
    }
    g->reg_taken[0] = 1; /* PREV, and the output */
    for (i = 0; i < stages; i++) {
        const GXTevStage* s = &gx.tev[i];
        int last = i == stages - 1;
        int dc, da;
        Src a, b, c, d;
        g->ncol = 0;
        g->nalpha = 0;
        /* the destinations: PREV is r0; a TEV register gets a register of its
         * own on its first write; the last stage's result is the output */
        if (last || s->creg == GX_TEVPREV) {
            dc = 0;
        } else {
            int n = s->creg & 3;
            if (g->reg_of[n] < 0) {
                g->reg_of[n] = reg_free_out(g, -1);
                if (g->reg_of[n] < 0) { g->fail = 8; return 0; }
                g->reg_taken[g->reg_of[n]] = 1;
            }
            dc = g->reg_of[n];
        }
        if (last || s->areg == GX_TEVPREV) {
            da = 0;
        } else {
            int n = s->areg & 3;
            if (g->reg_of[n] < 0) {
                g->reg_of[n] = reg_free_out(g, -1);
                if (g->reg_of[n] < 0) { g->fail = 8; return 0; }
                g->reg_taken[g->reg_of[n]] = 1;
            }
            da = g->reg_of[n];
        }
        /* the stage's own sample is dead after it: its register can be a
         * LERP's scratch from the next stage on (reg_sampled2 cleared below) */
        a = csrc(g, i, s->cin[0]);
        b = csrc(g, i, s->cin[1]);
        c = csrc(g, i, s->cin[2]);
        d = csrc(g, i, s->cin[3]);
        emit_stage_channel(g, 0, dc, a, b, c, d, s->cop, s->cbias, s->cscale, s->cclamp);
        a = asrc(g, i, s->ain[0]);
        b = asrc(g, i, s->ain[1]);
        c = asrc(g, i, s->ain[2]);
        d = asrc(g, i, s->ain[3]);
        emit_stage_channel(g, 1, da, a, b, c, d, s->aop, s->abias, s->ascale, s->aclamp);
        if (g->fail) {
            return 0;
        }
        g->pairs += g->ncol > g->nalpha ? g->ncol : g->nalpha;
        /* what the stage wrote is where its register lives now */
        if (last) {
            g->where_c[0] = 0;
            g->where_a[0] = 0;
        } else {
            g->where_c[s->creg & 3] = dc;
            g->where_a[s->areg & 3] = da;
        }
        if (L->unit_kind[i] == 1) {
            g->reg_sampled2[i] = 0;
        }
    }
    if (g->pairs > TFS_MAXPAIRS) {
        g->fail = 6;
        return 0;
    }
    emit(g, "EndPass;\n");
    if (g->fail) {
        return 0;
    }
    /* --tfsdbg N (M35's G4 iteration): the draw's program replaced by a
     * diagnostic on its unit 0 -- 1: the direct map at its own coordinate,
     * one pass; 2: the same through a register in a second pass (the
     * dependent read alone); 3: pass 1's coordinate register read back as
     * the colour in pass 2; 4: the coordinate as the colour, one pass;
     * 5: pass 1 samples the map, pass 2 outputs it through PassTexCoord */
    if (port_opt.tfsdbg) {
        static const char* dbg[] = {
            NULL,
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
            "StartOutputPass;\n  PassTexCoord r0, r0.str;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
            "StartOutputPass;\n  PassTexCoord r0, r0.str;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.str;\n  MOV r2, r1;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n",
            /* 9: unit 0's map at the (2x4, unprojected) coordinate of set 2 */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t2.stq_dq;\n  MOV r0, r0;\nEndPass;\n",
            /* 10: unit 2's map at its own coordinate */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t2.stq_dq;\n  MOV r0, r2;\nEndPass;\n",
            /* 11: unit 1's map at its own (projective) coordinate */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r1, t1.stq_dq;\n  MOV r0, r1;\nEndPass;\n",
            /* 12: unit 0's map at set 2's coordinate through a register */
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t2.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n",
            /* 13: unit 3's map at its own coordinate; 14: unit 1 at set 0; 15: unit 0 at set 1 */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r3, t3.stq_dq;\n  MOV r0, r3;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r1, t0.stq_dq;\n  MOV r0, r1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t1.stq_dq;\n  MOV r0, r0;\nEndPass;\n",
            /* 16: the two samples summed; 17: env[3] (m405's C0/A0) as the colour;
             * 18: the lerp by env[3].a alone */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  SampleMap r1, t1.stq_dq;\n  ADD r0, r0, r1;\nEndPass;\n",
            "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[3];\nEndConstants;\nStartOutputPass;\n  MOV r0.rgb, c0.a;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[3];\nEndConstants;\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  SampleMap r1, t1.stq_dq;\n  LERP r0.rgb, c0.a, r1, r0;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 19..25 (with --tfsall 1: every draw): which coordinate set and unit a
             * one-texture draw really samples */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t1.stq_dq;\n  MUL r0.rgb, r0, color0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r1, t0.stq_dq;\n  MUL r0.rgb, r1, color0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MUL r0.rgb, r0, color0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  MOV r0.rgb, color0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0.rgb, r0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  SampleMap r1, t1.stq_dq;\n  MUL r0.rgb, r0, color0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r0.rgb, r0;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 26: two samples with the white texture enabled on unit 1 (gx_tfs_apply pads) */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  SampleMap r1, t1.stq_dq;\n  MOV r0.rgb, r0;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 27: the same, sampling unit 1 first */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r1, t1.stq_dq;\n  SampleMap r0, t0.stq_dq;\n  MOV r0.rgb, r0;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 28-30: no self-MOV: MUL by 1, ADD 0, the natural modulate */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MUL r0.rgb, r0, 1;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  ADD r0.rgb, r0, 0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MUL r0.rgb, r0, color0;\n  MUL r0.a, r0, color0;\nEndPass;\n",
            /* 31: unit 2 at t0 (projective); 32: unit 2 at t2 (its own); 33: unit 0 at t0 through
             * a register with the divide done by PassTexCoord; 34: unit 0 at t0.str_dr (divide by r) */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t0.stq_dq;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t2.stq_dq;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str_dr;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 35: unit 2 at t0 divided through a register; 36: the same undivided (str);
             * 37: unit 0 at t2 (planar); 38: unit 1 at t2 */
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r2, t0.stq_dq;\n  MOV r1, r2;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r2, r2.str;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r2, t0.str;\n  MOV r1, r2;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r2, r2.str;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t2.stq_dq;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r1, t2.stq_dq;\n  MOV r0.rgb, r1;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 39: black, alpha 1 (does the draw land at all?); 40: black, alpha from the sample */
            "!!ATIfs1.0\nStartOutputPass;\n  MOV r0.rgb, 0;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0.rgb, 0;\nEndPass;\n",
            /* 41-43: the sampler's own projective divide: unit 2 at t0.stq, t0.str; unit 0 at t0.stq */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t0.stq;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t0.str;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 44: unit 0 through a register holding (s/q, t/q, 1) */
            "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r0.b, 1;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 45: unit 2 (bound to stage 0's map by the bind loop under this dbg) at t2;
             * 46: unit 0 bound to the indirect map (unit 2's) at t2 */
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r2, t2.stq_dq;\n  MOV r0.rgb, r2;\n  MOV r0.a, 1;\nEndPass;\n",
            "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t2.stq_dq;\n  MOV r0.a, 1;\nEndPass;\n",
            /* 47: the fold done in the shader (the vertex program's at the identity for
             * unit 0 under this dbg): s' = s*su, t' = tv - |sv|*t, then the sample */
            "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\n  CONSTANT c1 = program.env[1];\nEndConstants;\n"
            "StartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MUL r0.r, r0, c0;\n  MAD r0.g, r0.neg, c0, c1;\nEndPass;\n"
            "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0.a, 1;\nEndPass;\n",
        };
        int n = port_opt.tfsdbg;
        if (n > 0 && n < (int)(sizeof(dbg) / sizeof(dbg[0]))) {
            g->nk = 0;
            snprintf(out, (size_t)cap, "%s", dbg[n]);
            return (int)strlen(out);
        }
    }
    /* assemble: the header, the constants, the body */
    {
        int n = snprintf(out, (size_t)cap, "!!ATIfs1.0\n");
        if (g->nk) {
            n += snprintf(out + n, (size_t)(cap - n), "StartConstants;\n");
            for (i = 0; i < g->nk && n < cap; i++) {
                n += snprintf(out + n, (size_t)(cap - n), "  CONSTANT c%d = program.env[%d];\n", i, i);
            }
            n += snprintf(out + n, (size_t)(cap - n), "EndConstants;\n");
        }
        if (n + g->blen + 1 >= cap) {
            g->fail = 6;
            return 0;
        }
        memcpy(out + n, g->body, (size_t)g->blen);
        n += g->blen;
        out[n] = '\0';
        return n;
    }
}

/* ---- the program cache and the GL state ---------------------------------------- */

typedef struct TfsProg {
    u32 hash;
    unsigned id; /* 0 = refused */
    int errpos;
    char msg[120];
} TfsProg;
#define TFS_PROGS 64
static TfsProg progs[TFS_PROGS];
static int nprogs;

static u32 text_hash(const char* s, int n) {
    u32 h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ (u8)s[i]) * 16777619u;
    }
    return h;
}

static TfsProg* prog_find(const char* text, int len) {
    u32 h = text_hash(text, len);
    int i;
    for (i = 0; i < nprogs; i++) {
        if (progs[i].hash == h) {
            return &progs[i];
        }
    }
    if (nprogs >= TFS_PROGS) {
        return NULL;
    }
    {
        TfsProg* p = &progs[nprogs++];
        RtCompile c;
        memset(p, 0, sizeof(*p));
        p->hash = h;
        memset(&c, 0, sizeof(c));
        c.text = text;
        c.len = len;
        c.target = TFS_TARGET;
        rt_compile_vprog(&c);
        stat_compiles++;
        p->id = c.id;
        p->errpos = c.errpos;
        snprintf(p->msg, sizeof(p->msg), "%s", c.msg);
        if (!p->id) {
            stat_refused++;
            port_log("port> tfs: program %d REFUSED at char %d: %s\n", nprogs - 1, c.errpos,
                     c.msg[0] ? c.msg : "(no message)");
            port_log("%s", text);
        } else if (port_opt.tfslog) {
            port_log("port> tfs: program %d compiled (id %u):\n%s", nprogs - 1, p->id, text);
        }
        return p;
    }
}

/* the current plan: the program for the TEV configuration last emitted */
static struct {
    int valid;      /* the plan below belongs to the TEV signature in `sig` */
    u32 sig;
    int on;         /* 1 = the shader draws it; 0 = the fixed function does */
    unsigned id;
    TfsLayout L;
    Gen g;
    u8 signs;       /* the sign pattern the text bakes (mul_neg/mul_biased), to notice a change */
} plan;
static int tfs_enabled = -1; /* GL_TEXT_FRAGMENT_SHADER_ATI: 1 on, 0 off, -1 unknown */
static unsigned tfs_bound;
static float env_shadow[TFS_MAXK][4];
static u8 env_valid[TFS_MAXK];

/* the fold the vertex program reads for a unit while the shader's draw has
 * the unit's GL matrix at the identity (gx_tfs_apply) */
static u8 fold_on[GX_TEX_UNITS];
static float fold_v[GX_TEX_UNITS][3];
int gx_tfs_fold(int unit, float* su, float* sv, float* tv) {
    if (unit < 0 || unit >= GX_TEX_UNITS || !fold_on[unit]) {
        return 0;
    }
    *su = fold_v[unit][0];
    *sv = fold_v[unit][1];
    *tv = fold_v[unit][2];
    return 1;
}

void gx_tfs_invalidate(void) {
    tfs_enabled = -1;
    tfs_bound = 0;
    memset(env_valid, 0, sizeof(env_valid));
    memset(fold_on, 0, sizeof(fold_on));
    plan.valid = 0;
}

void gx_tfs_off(void) {
#ifndef PORT_NO_SDL
    memset(fold_on, 0, sizeof(fold_on));
    if (tfs_enabled != 0 && gl13_have_tfs) {
        glDisable(TFS_TARGET);
        tfs_enabled = 0;
    }
#endif
}

static void env_set(int k, const float* vin) {
#ifndef PORT_NO_SDL
    /* the R200's constants are eight bits and the driver truncates
     * (--tfsprobe's "constant x8" read): round to the nearest step here */
    float v[4];
    int i;
    for (i = 0; i < 4; i++) {
        float x = vin[i] < 0.0f ? 0.0f : vin[i] > 1.0f ? 1.0f : vin[i];
        x = (float)(int)(x * 255.0f + 0.5f) / 255.0f + 0.5f / 255.0f;
        v[i] = x > 1.0f ? 1.0f : x;
    }
    if (env_valid[k] && memcmp(env_shadow[k], v, 16) == 0) {
        return;
    }
    memcpy(env_shadow[k], v, 16);
    env_valid[k] = 1;
    rt_ext_env_param4fv(TFS_TARGET, (GLuint)k, v);
#else
    (void)k;
    (void)v;
#endif
}

/* the dims the offset constants need: per unit (w, h, su, sv) of the bound map */
static void unit_dims(const TfsLayout* L, float* dims) {
    int u;
    for (u = 0; u < L->nunits; u++) {
        float su = 1.0f, sv = 1.0f, tv = 0.0f;
        const GXTexObjPort* o = L->unit_kind[u] ? gx_bound_tex(L->unit_map[u]) : NULL;
        dims[u * 4 + 0] = o ? (float)o->width : 1.0f;
        dims[u * 4 + 1] = o ? (float)o->height : 1.0f;
        if (L->unit_kind[u]) {
            glc_get_tex_fold(u, &su, &sv, &tv);
        }
        dims[u * 4 + 2] = su;
        dims[u * 4 + 3] = sv;
    }
}

int gx_tfs_apply(int stages, int emit_now) {
#ifndef PORT_NO_SDL
    TfsLayout L;
    float dims[GX_TEX_UNITS * 4];
    int u, i;
    u32 sig = gx_tev_last_sig();
    (void)stages;
    if (!port_opt.tfs || !gl13_have_tfs) {
        return 0;
    }
    if (!emit_now && plan.valid && plan.sig == sig && !plan.on) {
        return 0; /* this configuration is the fixed function's */
    }
    memset(fold_on, 0, sizeof(fold_on)); /* the previous draw's override, over */
    layout_decide(&L);
    if (!L.on) {
        if (L.why) {
            stat_fallback++;
            stat_why[L.whyi]++;
        }
        gx_tfs_off();
        plan.valid = 1;
        plan.sig = sig;
        plan.on = 0;
        return 0;
    }
    /* the binds, every draw (the texture cache notices a rewritten map) */
    for (u = 0; u < gl13_max_tex_units && u < GX_TEX_UNITS; u++) {
        if (u < L.nunits && L.unit_kind[u]) {
            GXTexObjPort* o = gx_bound_tex(L.unit_map[u]);
            if (port_opt.tfsdbg == 45 && u == 2) {
                o = gx_bound_tex(L.unit_map[0]); /* the direct map on the indirect unit */
            } else if (port_opt.tfsdbg == 46 && u == 0 && L.nunits > 2) {
                o = gx_bound_tex(L.unit_map[2]); /* the indirect map on unit 0 */
            }
            glc_unit_enable_tex2d(u, 1);
            gx_tex_bind_swapped(u, o, (port_opt.tfsdbg == 45 && u == 2) ? L.unit_swap[0] : L.unit_swap[u]);
            if (port_opt.tfsforcebind && o && o->gl_name) {
                /* the bind said again to GL whatever the shadow believes */
                glActiveTexture(GL_TEXTURE0 + u);
                glBindTexture(GL_TEXTURE_2D, o->gl_name);
                glEnable(GL_TEXTURE_2D);
                glActiveTexture(GL_TEXTURE0);
            }
        } else if (port_opt.tfsdbg >= 26 && u == 1) {
            glc_bind_texture(1, glc_white_texture());
            glc_unit_enable_tex2d(1, 1);
        } else {
            glc_unit_enable_tex2d(u, 0);
        }
    }
    unit_dims(&L, dims);
    if (port_opt.tfsmtx) {
        /* The units' fixed-function texture matrices (the NPOT fold gx_tex.c
         * loads at bind) are set to the identity for the shader's draw: the
         * driver applies a unit's matrix to the shader's coordinates on top
         * of the vertex program's own fold (M35's G4 runs: m434's pond
         * sampled its copies double-folded, white), and the vertex program
         * keeps carrying the fold through gx_tfs_fold(), which
         * glc_get_tex_fold consults while the override is on. */
        for (u = 0; u < gl13_max_tex_units && u < GX_TEX_UNITS; u++) {
            fold_on[u] = 0;
            if (u < L.nunits && L.unit_kind[u]) {
                fold_v[u][0] = dims[u * 4 + 2];
                fold_v[u][1] = dims[u * 4 + 3];
                {
                    float su, sv;
                    glc_get_tex_fold(u, &su, &sv, &fold_v[u][2]);
                }
                if (port_opt.tfsdbg == 47 && u == 0) {
                    float c[4];
                    c[0] = fold_v[0][0]; c[1] = (float)fabs(fold_v[0][1]); c[2] = 0; c[3] = 0;
                    env_set(0, c);
                    c[0] = 0; c[1] = fold_v[0][2]; c[2] = 0; c[3] = 0;
                    env_set(1, c);
                    fold_v[0][0] = 1.0f; fold_v[0][1] = 1.0f; fold_v[0][2] = 0.0f;
                }
                glc_tex_matrix_fold(u, 1.0f, 1.0f, 0.0f);
                fold_on[u] = 1;
            }
        }
    }
    if (port_opt.tfslog && (emit_now || !plan.valid || plan.sig != sig)) {
        port_log("port> tfs: frame %u draw: %d stages, %d units:", gl13_frame_number(), L.stages, L.nunits);
        for (u = 0; u < L.nunits; u++) {
            const GXTexObjPort* o = L.unit_kind[u] ? gx_bound_tex(L.unit_map[u]) : NULL;
            port_log(" u%d=%s%d(t%d,map%d,%p %ux%u fmt%u gl%u swap%02x fold %.3f %.3f)", u,
                     L.unit_kind[u] == 2 ? "ind" : L.unit_kind[u] ? "stage" : "none", L.unit_idx[u],
                     L.unit_coord[u], L.unit_map[u], o ? o->image : NULL, o ? o->width : 0, o ? o->height : 0,
                     o ? (unsigned)o->format : 0, o ? o->gl_name : 0, L.unit_swap[u], dims[u * 4 + 2], dims[u * 4 + 3]);
        }
        port_log("\n");
    }
    if (port_opt.tfsdump && !rt_on()) {
        /* --tfsdump (with --norenderthread): the first two warp draws' unit
         * textures read back as tfs-fFRAME-uN.ppm, to see what the shader
         * samples (M35: is m405's buffer 2 the pool floor or a flat mean?) */
        static int dumped;
        if (dumped < 2 && (int)gl13_frame_number() >= port_opt.tfsdump) {
            typedef void (*pf_gtlp_t)(GLenum, GLint, GLenum, GLint*);
            pf_gtlp_t gtlp = (pf_gtlp_t)SDL_GL_GetProcAddress("glGetTexLevelParameteriv");
            dumped++;
            for (u = 0; u < L.nunits && gtlp; u++) {
                GLint w = 0, h = 0;
                u8* px;
                char path[1024];
                FILE* f;
                int i2;
                if (!L.unit_kind[u]) continue;
                glActiveTexture(GL_TEXTURE0 + u);
                gtlp(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                gtlp(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                if (w <= 0 || h <= 0 || w > 2048 || h > 2048) continue;
                px = (u8*)malloc((size_t)w * h * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                snprintf(path, sizeof(path), "%s/tfs-f%u-u%d-%dx%d.ppm", port_opt.shotdir ? port_opt.shotdir : ".",
                         gl13_frame_number(), u, w, h);
                f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    for (i2 = 0; i2 < w * h; i2++) fwrite(px + i2 * 4, 1, 3, f);
                    fclose(f);
                }
                free(px);
                port_log("port> tfs: dumped %s\n", path);
            }
            glActiveTexture(GL_TEXTURE0);
        }
    }
    if (emit_now || !plan.valid || plan.sig != sig || !plan.on || memcmp(&L, &plan.L, sizeof(L)) != 0) {
        static char text[TFS_TEXT + 512];
        int len = build_text(&plan.g, &L, dims, text, (int)sizeof(text));
        plan.valid = 1;
        plan.sig = sig;
        plan.on = 0;
        plan.L = L;
        if (!len) {
            int w = plan.g.fail ? plan.g.fail - 1 : 4;
            if (w >= 0 && w < 8) {
                stat_why[w]++;
            }
            stat_fallback++;
            gx_tfs_off();
            return 0;
        }
        {
            TfsProg* p = prog_find(text, len);
            if (!p || !p->id) {
                stat_why[6]++;
                stat_fallback++;
                gx_tfs_off();
                return 0;
            }
            plan.id = p->id;
        }
        plan.on = 1;
    } else {
        /* the same configuration: the offsets' signs are baked in the text, so
         * a map of another fold under the same stages (a copy where a texture
         * was) means a new text */
        int m, redo = 0;
        for (m = 0; m < plan.g.nmul && !redo; m++) {
            float v[4];
            int neg, biased, mod;
            mul_consts(plan.g.mul_mtx[m], plan.g.mul_dunit[m], dims, v, &neg, &biased, &mod);
            if (neg != plan.g.mul_neg[m] || biased != plan.g.mul_biased[m] || mod != plan.g.mul_mod[m]) {
                redo = 1;
            }
        }
        if (redo) {
            gx_tev_cache_invalidate();
            return gx_tfs_apply(stages, 1);
        }
    }
    /* the constants: the static ones from the text, the dynamic ones from the
     * maps bound now */
    for (i = 0; i < plan.g.nk; i++) {
        float v[4];
        memcpy(v, plan.g.kval[i], 16);
        if (plan.g.kdyn[i]) {
            int m;
            for (m = 0; m < plan.g.nmul; m++) {
                if (plan.g.mul_k[m] == i) {
                    int neg, biased, mod;
                    mul_consts(plan.g.mul_mtx[m], plan.g.mul_dunit[m], dims, v, &neg, &biased, &mod);
                    break;
                }
            }
        }
        env_set(i, v);
        if (port_opt.tfslog && emit_now) {
            port_log("port> tfs:   c%d = %.4f %.4f %.4f %.4f%s\n", i, v[0], v[1], v[2], v[3],
                     plan.g.kdyn[i] ? " (dynamic)" : "");
        }
    }
    /* The fixed-function texture environment under the shader: the driver
     * validates it still (a COMBINE reading a crossbar source on a unit the
     * draw disabled is a software fallback, shader or not -- M35's G4 runs:
     * a one-texture draw sampled grey through the shader until this), so
     * every unit's env is the plain default for the shader's draw, and the
     * TEV cache forgets its config so the next fixed-function draw re-emits. */
    if (port_opt.tfsenvreset) {
        for (u = 0; u < gl13_max_tex_units && u < GX_TEX_UNITS; u++) {
            glc_texenvi(u, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }
        gx_tev_cache_invalidate();
    }
    if (tfs_bound != plan.id || !port_opt.tfsnorebind) {
        /* bound again every draw (M35's G4 runs: the driver samples the
         * textures a program was bound with, not the ones bound since --
         * m434's pond stayed the white texture of the program's first draw) */
        tfs_bound = plan.id;
        rt_ext_bind_program(TFS_TARGET, plan.id);
    }
    if (tfs_enabled != 1 || !port_opt.tfsnorebind) {
        glEnable(TFS_TARGET);
        tfs_enabled = 1;
    }
    stat_draws++;
    return 1;
#else
    (void)stages;
    (void)emit_now;
    return 0;
#endif
}

void gx_tfs_report(void) {
    if (!stat_draws && !stat_fallback && !stat_compiles) {
        return;
    }
    port_log("port> tfs (M35): %lu draws through the fragment shader, %lu programs compiled "
             "(%lu refused), %lu draws fell back to the fixed function\n",
             stat_draws, stat_compiles, stat_refused, stat_fallback);
    if (stat_fallback) {
        int i;
        for (i = 0; i < 8; i++) {
            if (stat_why[i]) {
                port_log("port> tfs:   %lu x %s\n", stat_why[i], why_names[i]);
            }
        }
    }
}

/* ---- --tfsprobe --------------------------------------------------------------- */

#ifndef PORT_NO_SDL
typedef void (*pf_genprog_t)(GLsizei, GLuint*);
typedef void (*pf_bindprog_t)(GLenum, GLuint);
typedef void (*pf_progstr_t)(GLenum, GLenum, GLsizei, const void*);
typedef void (*pf_envp4fv_t)(GLenum, GLuint, const GLfloat*);
typedef void (*pf_delprog_t)(GLsizei, const GLuint*);
typedef void (*pf_mtc2f_t)(GLenum, GLfloat, GLfloat);
static pf_mtc2f_t pf_mtc;
typedef void (*pf_mtc4f_t)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
static pf_mtc4f_t pf_mtc4;
static void probe_quad4(float r, float q) {
    glBegin(GL_QUADS);
    pf_mtc4(GL_TEXTURE0, 0, 0, r, q); pf_mtc4(GL_TEXTURE2, 0, 0, r, q); glVertex2f(0, 0);
    pf_mtc4(GL_TEXTURE0, q, 0, r, q); pf_mtc4(GL_TEXTURE2, q, 0, r, q); glVertex2f(1, 0);
    pf_mtc4(GL_TEXTURE0, q, q, r, q); pf_mtc4(GL_TEXTURE2, q, q, r, q); glVertex2f(1, 1);
    pf_mtc4(GL_TEXTURE0, 0, q, r, q); pf_mtc4(GL_TEXTURE2, 0, q, r, q); glVertex2f(0, 1);
    glEnd();
}
static void probe_quad(void) {
    glBegin(GL_QUADS);
    pf_mtc(GL_TEXTURE0, 0, 0); pf_mtc(GL_TEXTURE2, 0, 0); glVertex2f(0, 0);
    pf_mtc(GL_TEXTURE0, 1, 0); pf_mtc(GL_TEXTURE2, 1, 0); glVertex2f(1, 0);
    pf_mtc(GL_TEXTURE0, 1, 1); pf_mtc(GL_TEXTURE2, 1, 1); glVertex2f(1, 1);
    pf_mtc(GL_TEXTURE0, 0, 1); pf_mtc(GL_TEXTURE2, 0, 1); glVertex2f(0, 1);
    glEnd();
}

static int probe_compile(pf_genprog_t gen, pf_bindprog_t bind, pf_progstr_t str, const char* name,
                         const char* text, GLuint* id_out) {
    GLuint id = 0;
    GLint errpos = -1;
    gen(1, &id);
    bind(TFS_TARGET, id);
    str(TFS_TARGET, 0x8875 /* ASCII */, (GLsizei)strlen(text), text);
    glGetIntegerv(0x864B /* ERROR_POSITION */, &errpos);
    if (errpos != -1) {
        const char* m = (const char*)glGetString(0x8874);
        port_log("tfsprobe: %-28s REFUSED at char %d: %s\n", name, (int)errpos, m ? m : "(no message)");
        {
            /* the line the position falls in */
            int line = 1, i;
            for (i = 0; i < errpos && text[i]; i++) if (text[i] == '\n') line++;
            port_log("tfsprobe:   (line %d)\n", line);
        }
        *id_out = 0;
        return 0;
    }
    port_log("tfsprobe: %-28s compiled (id %u)\n", name, id);
    *id_out = id;
    return 1;
}

/* a state for one of the three games' water draws, from their source */
static void probe_state_m405(void) {
    float m[3][4];
    memset(&gx.tev, 0, sizeof(gx.tev));
    gx.num_tev = 2;
    gx.num_texgens = 4;
    gx.num_ind = 2;
    gx.ind[0].coord = 2; gx.ind[0].map = 2; gx.ind[0].scale_s = gx.ind[0].scale_t = GX_ITS_1;
    gx.ind[1].coord = 3; gx.ind[1].map = 3; gx.ind[1].scale_s = gx.ind[1].scale_t = GX_ITS_1;
    memset(m, 0, sizeof(m)); m[0][0] = -0.2f; m[1][1] = -0.2f; m[2][2] = 0.2f;
    GXSetIndTexMtx(GX_ITM_0, m, -2);
    memset(m, 0, sizeof(m)); m[0][0] = 0.5f; m[1][1] = 0.5f; m[2][2] = 0.5f;
    GXSetIndTexMtx(GX_ITM_1, m, 0);
    GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_TRUE, GX_FALSE, GX_ITM_0);
    GXSetTevIndWarp(GX_TEVSTAGE1, GX_INDTEXSTAGE1, GX_TRUE, GX_FALSE, GX_ITM_1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_A0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void probe_state_m417(void) {
    float m[3][4];
    memset(&gx.tev, 0, sizeof(gx.tev));
    gx.num_tev = 5;
    gx.num_texgens = 5;
    gx.num_ind = 3;
    gx.ind[0].coord = 1; gx.ind[0].map = 1; gx.ind[0].scale_s = gx.ind[0].scale_t = GX_ITS_1;
    gx.ind[1] = gx.ind[0];
    gx.ind[2] = gx.ind[0];
    memset(m, 0, sizeof(m)); m[0][0] = -0.5f; m[1][1] = -0.5f; m[2][2] = 0.5f;
    GXSetIndTexMtx(GX_ITM_0, m, -2);
    memset(m, 0, sizeof(m)); m[0][0] = 0.5f; m[1][1] = 0.5f; m[2][2] = 0.5f;
    GXSetIndTexMtx(GX_ITM_1, m, 0);
    memset(m, 0, sizeof(m)); m[0][0] = -0.65f; m[1][1] = -0.65f; m[2][2] = 0.65f;
    GXSetIndTexMtx(GX_ITM_2, m, -3);
    GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_TRUE, GX_FALSE, GX_ITM_0);
    GXSetTevIndWarp(GX_TEVSTAGE1, GX_INDTEXSTAGE1, GX_TRUE, GX_FALSE, GX_ITM_1);
    GXSetTevIndWarp(GX_TEVSTAGE2, GX_INDTEXSTAGE2, GX_TRUE, GX_FALSE, GX_ITM_2);
    GXSetTevIndWarp(GX_TEVSTAGE3, GX_INDTEXSTAGE2, GX_TRUE, GX_FALSE, GX_ITM_2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD2, GX_TEXMAP2, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_A0, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD3, GX_TEXMAP3, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_A1, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD4, GX_TEXMAP3, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASA, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG2);
    GXSetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVREG2);
    GXSetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE4, GX_CC_CPREV, GX_CC_C1, GX_CC_C2, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
}

static void probe_state_m434(void) {
    float m0[2][3] = { { 0.012f, 0, 0 }, { 0, 0.012f, 0 } };
    float m1[2][3] = { { 0, 0, 0.5f }, { 0, 0.5f, 0 } };
    memset(&gx.tev, 0, sizeof(gx.tev));
    gx.num_tev = 2;
    gx.num_texgens = 4;
    gx.num_ind = 2;
    gx.ind[0].coord = 2; gx.ind[0].map = 2; gx.ind[0].scale_s = gx.ind[0].scale_t = GX_ITS_1;
    gx.ind[1].coord = 3; gx.ind[1].map = 3; gx.ind[1].scale_s = gx.ind[1].scale_t = GX_ITS_1;
    GXSetIndTexMtx(GX_ITM_0, m0, 0);
    GXSetIndTexMtx(GX_ITM_1, m1, 0);
    GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_TRUE, GX_FALSE, GX_ITM_0);
    GXSetTevIndWarp(GX_TEVSTAGE1, GX_INDTEXSTAGE1, GX_TRUE, GX_FALSE, GX_ITM_1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_A0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* four real texture objects on maps 0..3, so the layout finds them bound */
static u8 probe_tex_i8[64 * 64];
static u8 probe_tex_rgb[64 * 64 * 2];
static void probe_bind_maps(void) {
    GXTexObj t;
    int i;
    for (i = 0; i < 64 * 64; i++) probe_tex_i8[i] = (u8)i;
    for (i = 0; i < 64 * 64; i++) { probe_tex_rgb[i * 2] = (u8)(i >> 8); probe_tex_rgb[i * 2 + 1] = (u8)i; }
    GXInitTexObj(&t, probe_tex_rgb, 64, 64, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXLoadTexObj(&t, GX_TEXMAP0);
    GXInitTexObj(&t, probe_tex_i8, 64, 64, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GXLoadTexObj(&t, GX_TEXMAP1);
    GXInitTexObj(&t, probe_tex_i8, 64, 64, GX_TF_IA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GXLoadTexObj(&t, GX_TEXMAP2);
    GXInitTexObj(&t, probe_tex_rgb, 64, 64, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXLoadTexObj(&t, GX_TEXMAP3);
}

static void probe_game(const char* name, pf_genprog_t gen, pf_bindprog_t bind, pf_progstr_t str) {
    TfsLayout L;
    Gen g;
    static char text[TFS_TEXT + 512];
    float dims[GX_TEX_UNITS * 4];
    int len, u;
    GLuint id;
    layout_decide(&L);
    if (!L.on) {
        port_log("tfsprobe: %s: the layout says no (%s)\n", name, L.why ? L.why : "no warped stage");
        return;
    }
    for (u = 0; u < L.nunits; u++) {
        const GXTexObjPort* o = L.unit_kind[u] ? gx_bound_tex(L.unit_map[u]) : NULL;
        dims[u * 4 + 0] = o ? (float)o->width : 1.0f;
        dims[u * 4 + 1] = o ? (float)o->height : 1.0f;
        dims[u * 4 + 2] = 1.0f;
        dims[u * 4 + 3] = 1.0f;
    }
    port_log("tfsprobe: %s: %d stages on %d units:", name, L.stages, L.nunits);
    for (u = 0; u < L.nunits; u++) {
        port_log(" u%d=%s%d(t%d,map%d,swap%02x)", u, L.unit_kind[u] == 2 ? "ind" : L.unit_kind[u] ? "stage" : "none",
                 L.unit_idx[u], L.unit_coord[u], L.unit_map[u], L.unit_swap[u]);
    }
    port_log("\n");
    len = build_text(&g, &L, dims, text, (int)sizeof(text));
    if (!len) {
        port_log("tfsprobe: %s: the text failed: %s\n", name, g.fail ? why_names[g.fail - 1] : "?");
        return;
    }
    port_log("%s", text);
    {
        int i;
        for (i = 0; i < g.nk; i++) {
            port_log("tfsprobe:   c%d = %.4f %.4f %.4f %.4f%s\n", i, g.kval[i][0], g.kval[i][1], g.kval[i][2],
                     g.kval[i][3], g.kdyn[i] ? " (dynamic)" : "");
        }
    }
    probe_compile(gen, bind, str, name, text, &id);
}

void gx_tfs_probe(void) {
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    pf_genprog_t gen;
    pf_bindprog_t bind;
    pf_progstr_t str;
    pf_envp4fv_t envp;
    pf_delprog_t del;
    GLuint id;
    port_log("---- tfs probe (M35) ----\n");
    port_log("GL_ATI_text_fragment_shader           %s\n",
             ext && strstr(ext, "GL_ATI_text_fragment_shader") ? "present" : "ABSENT");
    if (!ext || !strstr(ext, "GL_ATI_text_fragment_shader")) {
        port_shutdown(0);
        return;
    }
    gen = (pf_genprog_t)SDL_GL_GetProcAddress("glGenProgramsARB");
    bind = (pf_bindprog_t)SDL_GL_GetProcAddress("glBindProgramARB");
    str = (pf_progstr_t)SDL_GL_GetProcAddress("glProgramStringARB");
    envp = (pf_envp4fv_t)SDL_GL_GetProcAddress("glProgramEnvParameter4fvARB");
    del = (pf_delprog_t)SDL_GL_GetProcAddress("glDeleteProgramsARB");
    pf_mtc = (pf_mtc2f_t)SDL_GL_GetProcAddress("glMultiTexCoord2fARB");
    pf_mtc4 = (pf_mtc4f_t)SDL_GL_GetProcAddress("glMultiTexCoord4fARB");
    if (!gen || !bind || !str || !envp || !del || !pf_mtc) {
        port_log("tfsprobe: an ARB program entry point is missing\n");
        port_shutdown(0);
        return;
    }
    /* 1. the shapes the builder emits, one at a time */
    probe_compile(gen, bind, str, "pass-through",
                  "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n", &id);
    probe_compile(gen, bind, str, "two-pass dependent read",
                  "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                  "StartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  PassTexCoord r0, t0.stq_dq;\n"
                  "  MUL r1.rg.eighth, r2.2x.bias, c0;\n  ADD r0.rg, r0, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n", &id);
    probe_compile(gen, bind, str, "biased constant + neg",
                  "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                  "StartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  PassTexCoord r0, t0.stq_dq;\n"
                  "  MUL r1.rg.eighth, r2.neg.2x.bias, c0.2x.bias;\n  ADD r0.rg, r0, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n", &id);
    probe_compile(gen, bind, str, "alpha replicate + color0 + sat",
                  "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\n  CONSTANT c1 = program.env[1];\nEndConstants;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  SampleMap r1, t1.stq_dq;\n"
                  "  MUL r0.rgb, r0, c0;\n  MOV r0.a, c0;\n  MAD r0.rgb.sat, r1, c0.a, r0;\n  MOV r0.a, c1;\n"
                  "  LERP r0.rgb, r1.a, c1, r0;\n  MUL r0.rgb.2x, r1, color0.a;\nEndPass;\n", &id);
    probe_compile(gen, bind, str, "eight pairs in a prelim pass",
                  "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                  "StartPrelimPass;\n  SampleMap r5, t5.stq_dq;\n  PassTexCoord r0, t0.stq_dq;\n  PassTexCoord r1, t1.stq_dq;\n"
                  "  PassTexCoord r2, t2.stq_dq;\n  PassTexCoord r3, t3.stq_dq;\n"
                  "  MUL r4.rg.eighth, r5.2x.bias, c0;\n  ADD r0.rg, r0, r4;\n  MUL r4.rg.eighth, r5.2x.bias, c0;\n  ADD r1.rg, r1, r4;\n"
                  "  MUL r4.rg.eighth, r5.2x.bias, c0;\n  ADD r2.rg, r2, r4;\n  ADD r3.rg, r3, r4;\n  ADD r3.rg, r3, r4;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r0.str;\n  SampleMap r1, r1.str;\n  SampleMap r2, r2.str;\n  SampleMap r3, r3.str;\n"
                  "  MUL r0.rgb, r0, c0;\n  MAD r0.rgb, r1, c0.a, r0;\n  MAD r0.rgb, r2, c0.a, r0;\n  MUL r4.rgb, r3, color0.a;\n"
                  "  LERP r0.rgb, r4, c0, r0;\n  MOV r0.a, c0;\nEndPass;\n", &id);
    /* 2. the three games' chains through the builder (the swap tables as
     * GXInit leaves them: the identity) */
    {
        int t;
        for (t = 0; t < 4; t++) {
            gx.swap_tbl[t][0] = GX_CH_RED; gx.swap_tbl[t][1] = GX_CH_GREEN;
            gx.swap_tbl[t][2] = GX_CH_BLUE; gx.swap_tbl[t][3] = GX_CH_ALPHA;
        }
    }
    probe_bind_maps();
    probe_state_m405();
    probe_game("m405 Mario Medley", gen, bind, str);
    probe_state_m417();
    probe_game("m417 Makin' Waves", gen, bind, str);
    probe_state_m434();
    probe_game("m434 Cheep Cheep Sweep", gen, bind, str);
    /* 3. the numbers: a 256x256 gradient (r = s, g = t) sampled through an
     * indirect map of known texels; the read-back says where the offset
     * landed and how finely the constant resolves */
    {
        GLuint tex[2];
        static u8 grad[256 * 256 * 4];
        static u8 ind[4 * 4 * 4];
        int x, y, k;
        u8 px[4 * 8];
        GLint vp[4];
        float c0[4];
        pf_bindprog_t bp = bind;
        glGetIntegerv(GL_VIEWPORT, vp);
        for (y = 0; y < 256; y++) {
            for (x = 0; x < 256; x++) {
                u8* p = grad + (y * 256 + x) * 4;
                p[0] = (u8)x; p[1] = (u8)y; p[2] = 0; p[3] = 255;
            }
        }
        /* the indirect map: alpha (S) = 255 -> +1, blue (T) = 0 -> -1, everywhere */
        for (k = 0; k < 16; k++) { ind[k * 4] = 0; ind[k * 4 + 1] = 0; ind[k * 4 + 2] = 0; ind[k * 4 + 3] = 255; }
        glGenTextures(2, tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, grad);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glEnable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, ind);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_FOG);
        glDisable(0x8620); /* VERTEX_PROGRAM_ARB */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glActiveTexture(GL_TEXTURE2);
        glLoadIdentity();
        glActiveTexture(GL_TEXTURE0);
        glMatrixMode(GL_MODELVIEW);
        glViewport(0, 0, 64, 64);
        glDisable(GL_CULL_FACE);
        /* (0) the coordinates themselves as the colour, both selections */
        probe_compile(gen, bp, str, "read-back: coords str",
                      "!!ATIfs1.0\nStartOutputPass;\n  PassTexCoord r0, t0.str;\n  MOV r0, r0;\nEndPass;\n", &id);
        glEnable(TFS_TARGET);
        glClear(GL_COLOR_BUFFER_BIT);
        probe_quad();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glReadPixels(8, 48, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px + 4);
        port_log("tfsprobe: coords str: centre = %d %d %d %d, (8,48) = %d %d %d %d (expect 128 128 0 ?, 32 192 0 ?)\n",
                 px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
        probe_compile(gen, bp, str, "read-back: coords stq_dq",
                      "!!ATIfs1.0\nStartOutputPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n", &id);
        glClear(GL_COLOR_BUFFER_BIT);
        probe_quad();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        port_log("tfsprobe: coords stq_dq: centre = %d %d %d %d (expect 128 128 255 ?)\n", px[0], px[1], px[2], px[3]);
        probe_compile(gen, bp, str, "read-back: sample str",
                      "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n", &id);
        glClear(GL_COLOR_BUFFER_BIT);
        probe_quad();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        port_log("tfsprobe: sample str: centre = %d %d %d %d (expect ~128 128 0 255)\n", px[0], px[1], px[2], px[3]);
        probe_compile(gen, bp, str, "read-back: fixed function",
                      "!!ATIfs1.0\nStartOutputPass;\n  MOV r0, 1;\nEndPass;\n", &id);
        glDisable(TFS_TARGET);
        glClear(GL_COLOR_BUFFER_BIT);
        probe_quad();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        port_log("tfsprobe: fixed function (unit 0 modulate): centre = %d %d %d %d\n", px[0], px[1], px[2], px[3]);
        /* (a) the pass-through: the centre pixel should read (128, 128) */
        probe_compile(gen, bp, str, "read-back: pass-through",
                      "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n", &id);
        glEnable(TFS_TARGET);
        glClear(GL_COLOR_BUFFER_BIT);
        probe_quad();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        port_log("tfsprobe: pass-through centre = %d %d %d %d (expect ~128 128 0 255)\n", px[0], px[1], px[2], px[3]);
        /* (a2) the dependent read alone, six ways (M35's first G4 run read
         * black through the warp: which step loses the coordinate?) */
        {
            static const char* dep[][2] = {
                { "dep: pass stq_dq, same reg",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r0, t0.stq_dq;\n  MOV r1, r0;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass stq_dq, other reg",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass str, other reg",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.str;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass stq_dq, ADD 0, other reg",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  ADD r1.rg, r1, 0;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass stq_dq, ADD 0 rgb, other reg",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  ADD r1, r1, 0;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass str, MOV b 1, str_dr",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.str;\n  MOV r1.b, 1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str_dr;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: pass stq_dq, pass again in 2",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  PassTexCoord r1, r1.str;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "dep: coords of pass 1 as colour in 2",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  PassTexCoord r1, r1.str;\n  MOV r0, r1;\nEndPass;\n" },
                { "dep: sample in 1, sample again in 2",
                  "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r2, r0;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "one: sample stq",
                  "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq;\n  MOV r0, r0;\nEndPass;\n" },
                { "one: sample str_dr",
                  "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str_dr;\n  MOV r0, r0;\nEndPass;\n" },
                { "two: pass 2 MOV 1 (expect 255s)",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  MOV r0, 1;\nEndPass;\n" },
                { "two: pass 2 MOV color0",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  MOV r0, color0;\nEndPass;\n" },
                { "two: sample in 1, keep by PassTexCoord",
                  "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n"
                  "StartOutputPass;\n  PassTexCoord r0, r0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "two: pass 1 ALU on r0 only",
                  "!!ATIfs1.0\nStartPrelimPass;\n  MOV r0, 1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "two: pass 1 sample r1 (unit 1 unbound)",
                  "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  MOV r3, r2;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
            };
            int t;
            for (t = 0; t < (int)(sizeof(dep) / sizeof(dep[0])); t++) {
                probe_compile(gen, bp, str, dep[t][0], dep[t][1], &id);
                glEnable(TFS_TARGET);
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: %-40s centre = %d %d %d %d (expect ~130 130 0 255) glerror %04x\n", dep[t][0], px[0], px[1], px[2], px[3], (unsigned)glGetError());
            }
        }
        /* (a4) the clear colour tells "not drawn" from "drawn black"; the
         * coordinates with r and q given; the selections */
        {
            static const char* m[][2] = {
                { "m: one str", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: one stq", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: one str_dr", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str_dr;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: one stq_dq", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: two dep str", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.str;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: two dep stq_dq (illegal)", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: two t0 in 2 stq_dq", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "m: two sample in 1 only", "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n"
                  "StartOutputPass;\n  MOV r0, r0;\nEndPass;\n" },
            };
            static const float rq[][2] = { { 0, 1 }, { 1, 1 }, { 0, 2 } };
            int t, c;
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            for (t = 0; t < (int)(sizeof(m) / sizeof(m[0])); t++) {
                probe_compile(gen, bp, str, m[t][0], m[t][1], &id);
                glEnable(TFS_TARGET);
                for (c = 0; c < 3; c++) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    probe_quad4(rq[c][0], rq[c][1]);
                    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px + 4 * c);
                }
                port_log("tfsprobe: %-30s r0q1 = %3d %3d %3d %3d | r1q1 = %3d %3d %3d %3d | r0q2 = %3d %3d %3d %3d  (51 102 153 = not drawn)\n",
                         m[t][0], px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8], px[9], px[10], px[11]);
            }
            glClearColor(0, 0, 0, 0);
        }
        /* (a7) the game's way: client arrays through glDrawArrays, with and
         * without the vertex program */
        {
            static const float vtx[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            static const float tc[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            static const char* cases[][2] = {
                { "a: one stq_dq", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "a: one str", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "a: two dep str", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "a: two ind + dep str", "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                  "StartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  PassTexCoord r0, t0.stq_dq;\n"
                  "  MUL r1.rg.eighth, r2.2x.bias, c0;\n  ADD r0.rg, r0, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n" },
            };
            static const char VPT2[] =
                "!!ARBvp1.0\n"
                "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
                "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
                "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
                "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
                "MOV result.color, vertex.color;\n"
                "MOV result.texcoord[0], vertex.texcoord[0];\n"
                "MOV result.texcoord[2], vertex.texcoord[0];\n"
                "END\n";
            GLuint vid = 0;
            int t, rep, vp;
            float c0[4] = { 1.0f, 1.0f, 0, 0 };
            glReadBuffer(GL_BACK);
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, vtx);
            glClientActiveTexture(GL_TEXTURE0);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, tc);
            glClientActiveTexture(GL_TEXTURE2);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, tc);
            glClientActiveTexture(GL_TEXTURE0);
            gen(1, &vid);
            bind(0x8620, vid);
            str(0x8620, 0x8875, (GLsizei)(sizeof(VPT2) - 1), VPT2);
            for (vp = 0; vp < 2; vp++) {
                if (vp) glEnable(0x8620); else glDisable(0x8620);
                for (t = 0; t < (int)(sizeof(cases) / sizeof(cases[0])); t++) {
                    probe_compile(gen, bp, str, cases[t][0], cases[t][1], &id);
                    envp(TFS_TARGET, 0, c0);
                    glEnable(TFS_TARGET);
                    for (rep = 0; rep < 2; rep++) {
                        glClear(GL_COLOR_BUFFER_BIT);
                        glDrawArrays(GL_QUADS, 0, 4);
                        glFinish();
                        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        port_log("tfsprobe: %-22s vp %d rep %d: %d %d %d %d glerror %04x\n", cases[t][0], vp, rep, px[0], px[1], px[2], px[3], (unsigned)glGetError());
                    }
                    glDisable(TFS_TARGET);
                }
            }
            glDisable(0x8620);
            bind(0x8620, 0);
            glDisableClientState(GL_VERTEX_ARRAY);
            glClientActiveTexture(GL_TEXTURE0);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glClientActiveTexture(GL_TEXTURE2);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glClientActiveTexture(GL_TEXTURE0);
            glClearColor(0, 0, 0, 0);
        }
        /* (a6) the read itself: the back buffer named, a finish before the
         * read, the current colour white, the fixed function as the control,
         * and every case twice */
        {
            typedef void (*pf_color4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
            pf_color4f_t col4 = (pf_color4f_t)SDL_GL_GetProcAddress("glColor4f");
            static const char* cases[][2] = {
                { "r: one stq_dq", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "r: one str", "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "r: two dep str", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "r: two MOV 1", "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  MOV r0, 1;\nEndPass;\n" },
            };
            int t, rep;
            glReadBuffer(GL_BACK);
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            if (col4) col4(1, 1, 1, 1);
            glDisable(TFS_TARGET);
            for (rep = 0; rep < 2; rep++) {
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glFinish();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: r: fixed function, white colour: %d %d %d %d (expect 130 130 0 255) glerror %04x\n", px[0], px[1], px[2], px[3], (unsigned)glGetError());
            }
            for (t = 0; t < (int)(sizeof(cases) / sizeof(cases[0])); t++) {
                probe_compile(gen, bp, str, cases[t][0], cases[t][1], &id);
                glEnable(TFS_TARGET);
                for (rep = 0; rep < 2; rep++) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    probe_quad();
                    glFinish();
                    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    port_log("tfsprobe: %-16s rep %d: %d %d %d %d glerror %04x\n", cases[t][0], rep, px[0], px[1], px[2], px[3], (unsigned)glGetError());
                }
                glDisable(TFS_TARGET);
            }
            glClearColor(0, 0, 0, 0);
        }
        /* (a5) three-component coordinates, and a 3D texture (depth 1) on
         * unit 0: does a three-coordinate sample need a three-coordinate
         * target on this driver? */
        {
            typedef void (*pf_mtc3f_t)(GLenum, GLfloat, GLfloat, GLfloat);
            typedef void (*pf_teximg3d_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
            pf_mtc3f_t mtc3 = (pf_mtc3f_t)SDL_GL_GetProcAddress("glMultiTexCoord3fARB");
            pf_teximg3d_t ti3 = (pf_teximg3d_t)SDL_GL_GetProcAddress("glTexImage3D");
            static const char* one_str = "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n";
            static const char* one_dq = "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n";
            static const char* two_dep = "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                                         "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n";
            GLuint t3 = 0;
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            if (mtc3) {
                probe_compile(gen, bp, str, "3f: one str", one_str, &id);
                glEnable(TFS_TARGET);
                glClear(GL_COLOR_BUFFER_BIT);
                glBegin(GL_QUADS);
                mtc3(GL_TEXTURE0, 0, 0, 0); glVertex2f(0, 0); mtc3(GL_TEXTURE0, 1, 0, 0); glVertex2f(1, 0);
                mtc3(GL_TEXTURE0, 1, 1, 0); glVertex2f(1, 1); mtc3(GL_TEXTURE0, 0, 1, 0); glVertex2f(0, 1);
                glEnd();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: 3f coords, one str: %d %d %d %d (51 102 153 = not drawn)\n", px[0], px[1], px[2], px[3]);
                probe_compile(gen, bp, str, "3f: one stq_dq", one_dq, &id);
                glClear(GL_COLOR_BUFFER_BIT);
                glBegin(GL_QUADS);
                mtc3(GL_TEXTURE0, 0, 0, 0); glVertex2f(0, 0); mtc3(GL_TEXTURE0, 1, 0, 0); glVertex2f(1, 0);
                mtc3(GL_TEXTURE0, 1, 1, 0); glVertex2f(1, 1); mtc3(GL_TEXTURE0, 0, 1, 0); glVertex2f(0, 1);
                glEnd();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: 3f coords, one stq_dq: %d %d %d %d\n", px[0], px[1], px[2], px[3]);
            }
            if (ti3) {
                glActiveTexture(GL_TEXTURE0);
                glDisable(GL_TEXTURE_2D);
                glGenTextures(1, &t3);
                glBindTexture(0x806F /* GL_TEXTURE_3D */, t3);
                ti3(0x806F, 0, GL_RGBA8, 256, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grad);
                glTexParameteri(0x806F, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(0x806F, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glEnable(0x806F);
                port_log("tfsprobe: 3D texture bound on unit 0: glerror %04x\n", (unsigned)glGetError());
                probe_compile(gen, bp, str, "3D: one str", one_str, &id);
                glEnable(TFS_TARGET);
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: 3D texture, one str: %d %d %d %d\n", px[0], px[1], px[2], px[3]);
                probe_compile(gen, bp, str, "3D: one stq_dq", one_dq, &id);
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: 3D texture, one stq_dq: %d %d %d %d\n", px[0], px[1], px[2], px[3]);
                probe_compile(gen, bp, str, "3D: two dep str", two_dep, &id);
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: 3D texture, two dep str: %d %d %d %d\n", px[0], px[1], px[2], px[3]);
                glDisable(0x806F);
                glBindTexture(GL_TEXTURE_2D, tex[0]);
                glEnable(GL_TEXTURE_2D);
            }
            glClearColor(0, 0, 0, 0);
        }
        /* (a3) the same four under a vertex program (the game's coordinates
         * come from one): does the two-pass path need it? */
        {
            static const char VPT[] =
                "!!ARBvp1.0\n"
                "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
                "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
                "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
                "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
                "MOV result.color, vertex.color;\n"
                "MOV result.texcoord[0], vertex.texcoord[0];\n"
                "MOV result.texcoord[1], vertex.texcoord[0];\n"
                "MOV result.texcoord[2], vertex.texcoord[2];\n"
                "END\n";
            static const char* vt[][2] = {
                { "vp: one sample str",
                  "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "vp: one sample stq_dq",
                  "!!ATIfs1.0\nStartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "vp: two, sample in 2 from t0",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0, r0;\nEndPass;\n" },
                { "vp: two, dependent str",
                  "!!ATIfs1.0\nStartPrelimPass;\n  PassTexCoord r1, t0.stq_dq;\n  MOV r2, r1;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
                { "vp: two, dependent str, ind sampled",
                  "!!ATIfs1.0\nStartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  PassTexCoord r1, t0.stq_dq;\n  ADD r1.rg, r1, 0;\nEndPass;\n"
                  "StartOutputPass;\n  SampleMap r0, r1.str;\n  MOV r0, r0;\nEndPass;\n" },
            };
            GLuint vid = 0;
            GLint errpos = -1;
            int t;
            gen(1, &vid);
            bind(0x8620, vid);
            str(0x8620, 0x8875, (GLsizei)(sizeof(VPT) - 1), VPT);
            glGetIntegerv(0x864B, &errpos);
            port_log("tfsprobe: the probe's vertex program: %s\n", errpos == -1 ? "loaded" : "REFUSED");
            glEnable(0x8620);
            for (t = 0; t < (int)(sizeof(vt) / sizeof(vt[0])); t++) {
                probe_compile(gen, bp, str, vt[t][0], vt[t][1], &id);
                glEnable(TFS_TARGET);
                glClear(GL_COLOR_BUFFER_BIT);
                probe_quad();
                glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                port_log("tfsprobe: %-40s centre = %d %d %d %d (expect ~130 130 0 255) glerror %04x\n", vt[t][0], px[0], px[1], px[2], px[3], (unsigned)glGetError());
            }
            glDisable(0x8620);
            bind(0x8620, 0);
        }
        /* (b) the warp: S = +1 shifts s by +c0.r/8, T = -1 shifts t by -c0.g/8 */
        probe_compile(gen, bp, str, "read-back: warp",
                      "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                      "StartPrelimPass;\n  SampleMap r2, t2.stq_dq;\n  PassTexCoord r0, t0.stq_dq;\n"
                      "  MUL r1.rg.eighth, r2.2x.bias, c0;\n  ADD r0.rg, r0, r1;\nEndPass;\n"
                      "StartOutputPass;\n  SampleMap r0, r0.str;\n  MOV r0, r0;\nEndPass;\n", &id);
        for (k = 0; k < 4; k++) {
            static const float ks[4] = { 1.0f, 0.5f, 0.125f, 0.0625f };
            c0[0] = ks[k]; c0[1] = ks[k]; c0[2] = 0; c0[3] = 0;
            envp(TFS_TARGET, 0, c0);
            glClear(GL_COLOR_BUFFER_BIT);
            probe_quad();
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            port_log("tfsprobe: warp c0=%.4f: centre = %d %d (expect s %d, t %d)\n", ks[k], px[0], px[1],
                     (int)(128 + 255 * ks[k] / 8), (int)(128 - 255 * ks[k] / 8));
        }
        /* (c) the constant's resolution: c0 = n/2040 for n = 1..4 through the
         * eighth, read as the shift of the gradient (1/2040 of 256 texels =
         * 0.125 texel: the shift shows as one level every 8 steps) -- and a
         * plain MUL r0 = c0 * 8 to read the constant at 8x */
        probe_compile(gen, bp, str, "read-back: constant x8",
                      "!!ATIfs1.0\nStartConstants;\n  CONSTANT c0 = program.env[0];\nEndConstants;\n"
                      "StartOutputPass;\n  SampleMap r0, t0.stq_dq;\n  MOV r0.8x, c0;\nEndPass;\n", &id);
        for (k = 1; k <= 6; k++) {
            c0[0] = (float)k / 2040.0f; c0[1] = (float)k / 1020.0f; c0[2] = (float)k / 510.0f; c0[3] = 1.0f;
            envp(TFS_TARGET, 0, c0);
            glClear(GL_COLOR_BUFFER_BIT);
            probe_quad();
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            port_log("tfsprobe: constant x8: c0 = %d/2040, %d/1020, %d/510 -> %d %d %d (an 8-bit constant reads %d %d %d)\n",
                     k, k, k, px[0], px[1], px[2], (int)(8 * 255 * ((float)((int)(k / 8.0f + 0.5f)) / 255.0f) + 0.5f),
                     (int)(8 * 255 * ((float)((int)(k / 4.0f + 0.5f)) / 255.0f) + 0.5f),
                     (int)(8 * 255 * ((float)((int)(k / 2.0f + 0.5f)) / 255.0f) + 0.5f));
        }
        glDisable(TFS_TARGET);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
    port_log("---- tfs probe end ----\n");
    port_shutdown(0);
}
#else
void gx_tfs_probe(void) {}
#endif
