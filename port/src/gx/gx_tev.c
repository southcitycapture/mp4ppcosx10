/* TEV -> the OpenGL 1.3 texture-environment chain.
 *
 * A GX TEV stage computes, per channel,
 *
 *     out = (d + lerp(a, b, c)) * scale + bias
 *         = (d + a*(1-c) + b*c) * scale + bias
 *
 * with a, b, c and d each chosen from sixteen sources.  A GL 1.3 texture unit
 * running `ARB_texture_env_combine` computes one of REPLACE, MODULATE, ADD,
 * ADD_SIGNED, SUBTRACT or INTERPOLATE over three arguments, and with
 * `ATI_texture_env_combine3` (which the Radeon 9000 has) also
 * `MODULATE_ADD_ATI` = Arg0*Arg2 + Arg1.  Those five shapes cover the GX
 * lerp exactly:
 *
 *   | GX inputs                     | what it computes | GL combiner        |
 *   |-------------------------------|------------------|--------------------|
 *   | a=0 b=0 c=0 d=X               | X                | REPLACE            |
 *   | a=0 b=X c=Y d=0               | X*Y              | MODULATE           |
 *   | a=X b=0 c=0 d=Y               | X+Y              | ADD                |
 *   | a=X b=Y c=Z d=0               | lerp(X,Y,Z)      | INTERPOLATE        |
 *   | a=0 b=X c=Y d=Z               | X*Y + Z          | MODULATE_ADD_ATI   |
 *
 * and PLAN.md §1.14 measured that 93 of the game's 121 `GXSetNumTevStages`
 * sites ask for one stage and the highest stage it ever names is
 * `GX_TEVSTAGE4`, so the six units are not the constraint the hardware makes
 * them look like.  Everything outside the table is counted and named by
 * `--gxwarn` rather than being silently drawn wrong; §3.9's
 * `ATI_text_fragment_shader` backend at M8 is where those come back.
 *
 * One GL limit with no GX equivalent: a texture unit has exactly one
 * `GL_TEXTURE_ENV_COLOR`, and a GX stage can read two different constants
 * (say `GX_CC_C0` and `GX_CC_KONST`).  When that happens the first constant
 * wins and the stage is named.
 */
#include "gx_internal.h"

#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
#endif

#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

#ifndef GL_MODULATE_ADD_ATI
#define GL_MODULATE_ADD_ATI 0x8744
#define GL_MODULATE_SIGNED_ADD_ATI 0x8745
#define GL_MODULATE_SUBTRACT_ATI 0x8746
#endif

/* An argument, resolved out of the GX source enum. */
typedef struct Arg {
    GLenum src;      /* GL_TEXTURE, GL_PREVIOUS, GL_PRIMARY_COLOR, GL_CONSTANT */
    GLenum operand;  /* GL_SRC_COLOR / GL_SRC_ALPHA / ONE_MINUS_...            */
    int is_zero;
    int is_const;
    float konst[4];  /* when is_const */
} Arg;

static void colorf(GXColor c, float* out) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
    out[3] = c.a / 255.0f;
}

/* The konst selects.  GX_TEV_KCSEL_8_8 .. _1_8 are the eight literal
 * fractions; _K0..K3 and their single-channel forms select a konst register. */
static void konst_color(const GXTevStage* s, int alpha, float* out) {
    u8 sel = alpha ? s->kasel : s->kcsel;
    int i;
    if (sel <= 7) { /* 8/8 down to 1/8 */
        float v = (float)(8 - sel) / 8.0f;
        for (i = 0; i < 4; i++) {
            out[i] = v;
        }
        return;
    }
    if (sel >= 0x0C && sel <= 0x0F) { /* K0..K3 as a whole colour */
        colorf(gx.kcolor[sel - 0x0C], out);
        return;
    }
    if (sel >= 0x10) { /* a single channel of K0..K3 broadcast */
        int reg = (sel - 0x10) & 3;
        int chan = (sel - 0x10) / 4; /* 0=R 1=G 2=B 3=A */
        float c[4];
        colorf(gx.kcolor[reg], c);
        for (i = 0; i < 4; i++) {
            out[i] = c[chan < 4 ? chan : 3];
        }
        return;
    }
    for (i = 0; i < 4; i++) {
        out[i] = 1.0f;
    }
}

/* ---- swap tables ----------------------------------------------------------
 *
 * `GXSetTevSwapModeTable` gives a stage a four-entry crossbar, and
 * `GXSetTevSwapMode` points the stage's *texture* colour and its *rasterised*
 * colour at one of four such tables.  Until M9 both were counted and ignored,
 * 136,000 times a menu walk, and the most visible consequence was that the
 * characters' eyes drew as nothing: the eye stage reads its texture through a
 * table that reroutes the channels, and the port sampled the unswapped texels.
 *
 * The two sides are not the same problem.
 *
 * **The texture side is exact.**  The swap happens before the combiner, on
 * texels that are ours to re-encode, so `gx_tex.c` keys the cache on
 * (texture, swap) and stores a copy with the channels already moved.  That is
 * not an approximation of the hardware; it is the same arithmetic done
 * earlier.
 *
 * **The rasterised side is where GL 1.3 runs out.**  A texture unit chooses an
 * operand per argument -- `GL_SRC_COLOR` or `GL_SRC_ALPHA` and their
 * complements -- and that is the whole crossbar it has.  So the two tables the
 * game can be expressing that GL can also say are the identity and "broadcast
 * one channel", and of the four channels only alpha has an operand.  Anything
 * else -- (G,B,R,A), say, or a table that pulls red into alpha -- has no
 * fixed-function form at all and is still counted and named.  See PLAN.md 21. */
#define SWAP_PACK(t) \
    ((u8)((t)[0] | ((t)[1] << 2) | ((t)[2] << 4) | ((t)[3] << 6)))
#define SWAP_IDENTITY ((u8)(GX_CH_RED | (GX_CH_GREEN << 2) | (GX_CH_BLUE << 4) | \
                            (GX_CH_ALPHA << 6)))

/* The stage's rasterised-colour table, or NULL when it is the identity. */
static const u8* ras_table(const GXTevStage* s) {
    const u8* t = gx.swap_tbl[s->ras_swap & 3];
    if (SWAP_PACK(t) == SWAP_IDENTITY) {
        return NULL;
    }
    return t;
}

/* `GX_CC_RASC` read through a swap table: the RGB the combiner should see.
 * Returns the GL operand, and warns when the table asks for something the
 * operand crossbar cannot say. */
static GLenum ras_rgb_operand(const u8* t) {
    if (!t) {
        return GL_SRC_COLOR;
    }
    if (t[0] == GX_CH_RED && t[1] == GX_CH_GREEN && t[2] == GX_CH_BLUE) {
        return GL_SRC_COLOR;
    }
    if (t[0] == t[1] && t[1] == t[2] && t[0] == GX_CH_ALPHA) {
        return GL_SRC_ALPHA; /* the one broadcast GL 1.3 can express */
    }
    gx_warn("GXSetTevSwapMode: a rasterised-colour swap GL 1.3's operand "
            "crossbar cannot express; the unswapped colour is used");
    return GL_SRC_COLOR;
}

/* ...and the alpha the combiner should see. */
static GLenum ras_alpha_operand(const u8* t) {
    if (!t || t[3] == GX_CH_ALPHA) {
        return GL_SRC_ALPHA;
    }
    gx_warn("GXSetTevSwapMode: a rasterised swap puts a colour channel into "
            "alpha, which GL 1.3 cannot select; alpha is left alone");
    return GL_SRC_ALPHA;
}

static Arg color_arg(const GXTevStage* s, u8 a) {
    const u8* rt = ras_table(s);
    Arg r;
    memset(&r, 0, sizeof(r));
    r.operand = GL_SRC_COLOR;
    switch (a) {
        case GX_CC_CPREV: r.src = GL_PREVIOUS; break;
        case GX_CC_APREV: r.src = GL_PREVIOUS; r.operand = GL_SRC_ALPHA; break;
        case GX_CC_C0: case GX_CC_C1: case GX_CC_C2:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            colorf(gx.tev_reg[1 + (a - GX_CC_C0) / 2], r.konst);
            break;
        case GX_CC_A0: case GX_CC_A1: case GX_CC_A2:
            r.src = GL_CONSTANT;
            r.operand = GL_SRC_ALPHA;
            r.is_const = 1;
            colorf(gx.tev_reg[1 + (a - GX_CC_A0) / 2], r.konst);
            break;
        case GX_CC_TEXC: r.src = GL_TEXTURE; break;
        case GX_CC_TEXA: r.src = GL_TEXTURE; r.operand = GL_SRC_ALPHA; break;
        case GX_CC_RASC:
            r.src = GL_PRIMARY_COLOR;
            r.operand = ras_rgb_operand(rt);
            break;
        case GX_CC_RASA:
            r.src = GL_PRIMARY_COLOR;
            r.operand = ras_alpha_operand(rt);
            break;
        case GX_CC_ONE:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            r.konst[0] = r.konst[1] = r.konst[2] = r.konst[3] = 1.0f;
            break;
        case GX_CC_HALF:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            r.konst[0] = r.konst[1] = r.konst[2] = r.konst[3] = 0.5f;
            break;
        case GX_CC_KONST:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            konst_color(s, 0, r.konst);
            break;
        default: /* GX_CC_ZERO */
            r.is_zero = 1;
            r.src = GL_CONSTANT;
            break;
    }
    return r;
}

static Arg alpha_arg(const GXTevStage* s, u8 a) {
    const u8* rt = ras_table(s);
    Arg r;
    memset(&r, 0, sizeof(r));
    r.operand = GL_SRC_ALPHA;
    switch (a) {
        case GX_CA_APREV: r.src = GL_PREVIOUS; break;
        case GX_CA_A0: case GX_CA_A1: case GX_CA_A2:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            colorf(gx.tev_reg[1 + (a - GX_CA_A0)], r.konst);
            break;
        case GX_CA_TEXA: r.src = GL_TEXTURE; break;
        case GX_CA_RASA:
            r.src = GL_PRIMARY_COLOR;
            r.operand = ras_alpha_operand(rt);
            break;
        case GX_CA_KONST:
            r.src = GL_CONSTANT;
            r.is_const = 1;
            konst_color(s, 1, r.konst);
            break;
        default: /* GX_CA_ZERO */
            r.is_zero = 1;
            r.src = GL_CONSTANT;
            break;
    }
    return r;
}

/* Choose the combiner for one channel and write it to the current unit. */
static void emit_channel(int unit, int rgb, Arg a, Arg b, Arg c, Arg d, u8 op, u8 bias,
                         u8 scale, float* konst_out, int* konst_set) {
    GLenum combine = rgb ? GL_COMBINE_RGB : GL_COMBINE_ALPHA;
    GLenum s0 = rgb ? GL_SOURCE0_RGB : GL_SOURCE0_ALPHA;
    GLenum s1 = rgb ? GL_SOURCE1_RGB : GL_SOURCE1_ALPHA;
    GLenum s2 = rgb ? GL_SOURCE2_RGB : GL_SOURCE2_ALPHA;
    GLenum o0 = rgb ? GL_OPERAND0_RGB : GL_OPERAND0_ALPHA;
    GLenum o1 = rgb ? GL_OPERAND1_RGB : GL_OPERAND1_ALPHA;
    GLenum o2 = rgb ? GL_OPERAND2_RGB : GL_OPERAND2_ALPHA;
    GLenum scale_e = rgb ? GL_RGB_SCALE : GL_ALPHA_SCALE;
    Arg args[3];
    GLenum mode;
    int n = 0, i;

    if (op != GX_TEV_ADD) {
        gx_warn("TEV: a comparison op (GX_TEV_COMP_*) is drawn as an add");
    }
    if (bias != GX_TB_ZERO) {
        gx_warn("TEV: a +/-0.5 bias is dropped");
    }

    if (a.is_zero && b.is_zero && c.is_zero) {
        mode = GL_REPLACE;
        args[n++] = d;
    } else if (a.is_zero && d.is_zero) {
        mode = GL_MODULATE;
        args[n++] = b;
        args[n++] = c;
    } else if (b.is_zero && c.is_zero) {
        mode = GL_ADD;
        args[n++] = a;
        args[n++] = d;
    } else if (d.is_zero) {
        /* GL_INTERPOLATE is Arg0*Arg2 + Arg1*(1-Arg2); GX's lerp is
         * a*(1-c) + b*c, so Arg0 = b, Arg1 = a, Arg2 = c. */
        mode = GL_INTERPOLATE;
        args[n++] = b;
        args[n++] = a;
        args[n++] = c;
    } else if (a.is_zero && gl13_have_combine3) {
        mode = GL_MODULATE_ADD_ATI; /* Arg0*Arg2 + Arg1 */
        args[n++] = b;
        args[n++] = d;
        args[n++] = c;
    } else {
        gx_warn("TEV: a four-input stage with no GL 1.3 combiner; the d term wins");
        mode = GL_REPLACE;
        args[n++] = d;
    }

    for (i = 0; i < n; i++) {
        if (args[i].is_const && !args[i].is_zero) {
            if (!*konst_set) {
                memcpy(konst_out, args[i].konst, sizeof(float) * 4);
                *konst_set = 1;
            } else if (memcmp(konst_out, args[i].konst, sizeof(float) * 4) != 0) {
                gx_warn("TEV: a stage needs two different constants; the first wins");
            }
        }
        if (args[i].is_zero) {
            /* Zero as a live argument only survives here in shapes the table
             * above did not fold away; a black constant is the honest value,
             * but it competes for the unit's single constant. */
            args[i].src = GL_CONSTANT;
            args[i].operand = rgb ? GL_SRC_COLOR : GL_SRC_ALPHA;
        }
    }

    if (!gl13_live()) {
        return;
    }
    glc_texenvi(unit, (unsigned)combine, (int)mode);
    glc_texenvi(unit, (unsigned)s0, (int)args[0].src);
    glc_texenvi(unit, (unsigned)o0, (int)args[0].operand);
    if (n > 1) {
        glc_texenvi(unit, (unsigned)s1, (int)args[1].src);
        glc_texenvi(unit, (unsigned)o1, (int)args[1].operand);
    }
    if (n > 2) {
        glc_texenvi(unit, (unsigned)s2, (int)args[2].src);
        glc_texenvi(unit, (unsigned)o2, (int)args[2].operand);
    }
    glc_texenvf(unit, (unsigned)scale_e,
                scale == GX_CS_SCALE_2 ? 2.0f : scale == GX_CS_SCALE_4 ? 4.0f : 1.0f);
    if (scale == GX_CS_DIVIDE_2) {
        gx_warn("TEV: GX_CS_DIVIDE_2 has no GL equivalent; drawn at scale 1");
    }
}

/* ---- the state cache (M13, PLAN.md 28.5) ---------------------------------
 *
 * The profile of a board frame puts `gx_tev_apply` and the four helpers it
 * calls -- `emit_channel`, `color_arg`, `alpha_arg`, `glc_texenvi` -- at 689
 * of the game thread's samples, third behind the display-list decode and
 * Apple's own GL dispatch; and the dispatch is partly the same work, because
 * every `glTexEnv` the shadow lets through makes the driver rebuild its
 * immediate-mode dispatch (`gldInitDispatch`, 555 samples).  The combiner is
 * recomputed from `gx.tev[]` on every one of ~930 draws a frame, and the game
 * changes it a few dozen times.
 *
 * So hash what the combiner is a function of and skip the emission when it has
 * not moved.  The *texture binds* are deliberately outside the cache: a bound
 * `GXTexObj` can have had its pixels rewritten under the same pointer, and
 * `gx_tex_bind_swapped`'s own content hash is what notices.  Only the
 * `glTexEnv*` side is cached, which is where the samples are.
 *
 * `--oldtev` restores the unconditional path for the A/B; `--tevstats` counts.
 */
static u32 tev_sig_hash(int stages, u32 have_tex_bits) {
    u32 h = 2166136261u;
    const u8* p;
    size_t n;
    int i;
#define TEV_MIX(ptr, len)                                                      \
    do {                                                                       \
        p = (const u8*)(ptr);                                                   \
        n = (size_t)(len);                                                      \
        while (n--) {                                                           \
            h = (h ^ *p++) * 16777619u;                                         \
        }                                                                       \
    } while (0)
    TEV_MIX(&gx.num_tev, sizeof(gx.num_tev));
    TEV_MIX(&have_tex_bits, sizeof(have_tex_bits));
    TEV_MIX(&gx.num_ind, sizeof(gx.num_ind));
    TEV_MIX(gx.swap_tbl, sizeof(gx.swap_tbl));
    TEV_MIX(gx.kcolor, sizeof(gx.kcolor));
    TEV_MIX(gx.tev_reg, sizeof(gx.tev_reg));
    for (i = 0; i < stages; i++) {
        TEV_MIX(&gx.tev[i], sizeof(gx.tev[i]));
        TEV_MIX(&gx.ind_tile[i], sizeof(gx.ind_tile[i]));
    }
#undef TEV_MIX
    return h;
}

static u32 tev_cache_sig;
static int tev_cache_live;
static unsigned long tev_hits, tev_misses;

void gx_tev_cache_invalidate(void) { tev_cache_live = 0; }

void gx_tev_apply(void) {
    int stages = gx.num_tev ? gx.num_tev : 1;
    int i;
    int emit = 1;
    u32 have_tex_bits = 0;
    u32 sig;
    if (stages > gl13_max_tex_units) {
        gx_warn("TEV: the stage chain needs more units than the card has; the "
                "extra stages are dropped (PLAN.md 3.4 fallback 1)");
        stages = gl13_max_tex_units;
    }
    if (!port_opt.oldtev) {
        for (i = 0; i < stages && i < 32; i++) {
            const GXTevStage* s = &gx.tev[i];
            if (gx_bound_tex(s->map) != NULL && s->coord < GX_TEXCOORDS) {
                have_tex_bits |= 1u << i;
            }
        }
        sig = tev_sig_hash(stages, have_tex_bits);
        if (tev_cache_live && sig == tev_cache_sig) {
            emit = 0;
            tev_hits++;
        } else {
            tev_cache_sig = sig;
            tev_cache_live = 1;
            tev_misses++;
        }
    }
    for (i = 0; i < gl13_max_tex_units; i++) {
        if (!gl13_live()) {
            break;
        }
        if (i >= stages) {
            glc_unit_enable_tex2d(i, 0);
            continue;
        }
        {
            const GXTevStage* s = &gx.tev[i];
            float konst[4] = { 0, 0, 0, 0 };
            int konst_set = 0;
            GXTexObjPort* bound = gx_bound_tex(s->map);
            int have_tex = bound != NULL && s->coord < GX_TEXCOORDS;
            if (have_tex) {
                glc_unit_enable_tex2d(i, 1);
                /* The tile-map case is composed on the CPU and bound as one
                 * ordinary texture (gx_tex.c); everything else indirect is
                 * still the direct stage alone. */
                if (gx.num_ind && gx.ind_tile[i].on) {
                    const GXIndTile* t = &gx.ind_tile[i];
                    GXTexObjPort* map = gx_bound_tex(gx.ind[t->ind].map);
                    if (!map || !gx_tex_bind_tiled(i, bound, map, t)) {
                        gx_warn("indirect texturing: the fixed-function path "
                                "draws the direct stage only (PLAN.md 3.4 case 3)");
                        gx_tex_bind_swapped(i, bound, SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]));
                    }
                } else {
                    if (gx.num_ind && !s->direct) {
                        gx_warn("indirect texturing: the fixed-function path "
                                "draws the direct stage only (PLAN.md 3.4 case 3)");
                    }
                    gx_tex_bind_swapped(i, bound,
                                        SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]));
                }
            } else {
                /* A stage with no texture still has to run its combiner, and
                 * a disabled unit in GL passes the previous colour through
                 * untouched -- which is only right when the stage is a pass.
                 * Keep the unit enabled against a 1x1 white texture instead.
                 *
                 * M15: that is what the comment said and what the code did NOT
                 * do -- it disabled the unit, and every GX_TEXMAP_NULL stage in
                 * the game was silently dropped.  On the §21.1 board frame that
                 * is 210 draws of 774, among them both of Mario's eyes, three
                 * of Luigi's face materials and four of Peach's, and none of
                 * Yoshi's, whose eyes are a single textured stage -- which is
                 * exactly the per-character symptom §29.4 recorded.  PLAN.md 30.
                 *
                 * --oldnulltev is the A/B lever: the pre-M15 drop. */
                unsigned white = port_opt.oldnulltev ? 0u : glc_white_texture();
                if (white) {
                    glc_bind_texture(i, white);
                    glc_unit_enable_tex2d(i, 1);
                    if (bound == NULL && s->map != GX_TEXMAP_NULL) {
                        /* Distinct from the stage that asks for no texture at
                         * all: this one named a texmap the game never loaded. */
                        gx_warn("TEV: a stage names a texmap that was never "
                                "loaded; it samples white");
                    }
                } else {
                    glc_unit_enable_tex2d(i, 0);
                    gx_warn("TEV: a stage with no texture is dropped, and the "
                            "previous stage's colour passes through");
                }
            }
            if (emit) {
                glc_texenvi(i, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                emit_channel(i, 1, color_arg(s, s->cin[0]), color_arg(s, s->cin[1]),
                             color_arg(s, s->cin[2]), color_arg(s, s->cin[3]), s->cop,
                             s->cbias, s->cscale, konst, &konst_set);
                emit_channel(i, 0, alpha_arg(s, s->ain[0]), alpha_arg(s, s->ain[1]),
                             alpha_arg(s, s->ain[2]), alpha_arg(s, s->ain[3]), s->aop,
                             s->abias, s->ascale, konst, &konst_set);
                glc_texenv_color(i, konst);
            }
        }
    }
}

void gx_tev_report(void) {
    if (port_opt.tevstats) {
        unsigned long tot = tev_hits + tev_misses;
        port_log("port> tev cache: %lu applies, %lu skipped (%.1f%%), %lu emitted\n",
                 tot, tev_hits, tot ? 100.0 * (double)tev_hits / (double)tot : 0.0,
                 tev_misses);
    }
}
