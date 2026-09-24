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
#include "game/object.h" /* omcurovl, for --tintlog (M23) */

#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
#include "gx_rt.h" /* M27: every gl* below is the render thread's twin */
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

void gx_tev_stage_konst(const GXTevStage* s, int alpha, float* out) { konst_color(s, alpha, out); }

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

/* M30 (PLAN.md 45): the register-chain plan.  `ren` is what regchain_plan
 * decided for this input: 0 = as the source says, 1 = the previous unit's
 * RGB, 2 = the previous unit's alpha. */
static Arg color_arg(const GXTevStage* s, u8 a, u8 ren) {
    const u8* rt = ras_table(s);
    Arg r;
    memset(&r, 0, sizeof(r));
    r.operand = GL_SRC_COLOR;
    if (ren) {
        r.src = GL_PREVIOUS;
        r.operand = ren == 2 ? GL_SRC_ALPHA : GL_SRC_COLOR;
        return r;
    }
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
            if (gx_hilite_mode == 2 && s == &gx.tev[gx_hilite_stage]) {
                /* M22: the program put the specular in the primary alpha */
                r.operand = GL_SRC_ALPHA;
            }
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

static Arg alpha_arg(const GXTevStage* s, u8 a, u8 ren) {
    const u8* rt = ras_table(s);
    Arg r;
    memset(&r, 0, sizeof(r));
    r.operand = GL_SRC_ALPHA;
    if (ren) {
        r.src = GL_PREVIOUS;
        return r;
    }
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

static unsigned cfg_konst_collisions; /* stages colliding in the config being emitted; see below */

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

    if (c.is_zero && !b.is_zero && !port_opt.oldczero) {
        /* M35 (PLAN.md 50): lerp(a, b, 0) = a whatever b is -- b drops out and
         * the stage is a + d.  m425's sea (main.c fn_1_5C20: CPREV, TEXC,
         * ZERO, TEXC = PREV + TEXC) fell to "a four-input stage with no GL
         * 1.3 combiner; the d term wins" and drew its texture alone. */
        b.is_zero = 1;
        b.is_const = 0;
        b.src = GL_CONSTANT;
    }
    if (a.is_zero && b.is_zero && c.is_zero) {
        mode = GL_REPLACE;
        args[n++] = d;
    } else if (b.is_zero && c.is_zero && d.is_zero && !port_opt.oldakonst) {
        /* M35 (PLAN.md 50): lerp(a, 0, 0) + 0 = a -- a REPLACE, with no black
         * constant claiming the unit's RGB half.  The shadow pass's caster
         * stage (hsfdraw.c FaceDrawShadow: colour A1, alpha A0) is this
         * shape, and as an ADD(a, black) its black took the RGB half. */
        mode = GL_REPLACE;
        args[n++] = a;
    } else if (a.is_zero && d.is_zero) {
        mode = GL_MODULATE;
        args[n++] = b;
        args[n++] = c;
    } else if (b.is_zero && c.is_zero) {
        mode = GL_ADD;
        args[n++] = a;
        args[n++] = d;
    } else if (b.is_zero && d.is_zero) {
        /* M34 (PLAN.md 49.4): lerp(a, 0, c) = a * (1 - c) -- a modulate by
         * the complement, with no zero constant to claim the unit's one
         * GL_TEXTURE_ENV_COLOR.  As a GL_INTERPOLATE it went (b = a black
         * constant, a = KONST, c), the black claimed the constant first and
         * KONST "collided" with it: m404's guide line, KONST * (1 - TEXA),
         * drew with alpha 0 * (1 - TEXA) = nothing. */
        if (c.is_zero) {
            mode = GL_REPLACE; /* lerp(a, 0, 0) = a */
            args[n++] = a;
        } else {
            mode = GL_MODULATE;
            args[n++] = a;
            c.operand = c.operand == GL_SRC_ALPHA ? GL_ONE_MINUS_SRC_ALPHA
                      : c.operand == GL_ONE_MINUS_SRC_ALPHA ? GL_SRC_ALPHA
                      : c.operand == GL_SRC_COLOR ? GL_ONE_MINUS_SRC_COLOR
                      : c.operand == GL_ONE_MINUS_SRC_COLOR ? GL_SRC_COLOR : c.operand;
            args[n++] = c;
        }
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

    if (scale == GX_CS_DIVIDE_2 && mode == GL_REPLACE) {
        /* M34 (PLAN.md 49.4): GL's scale is 1, 2 or 4, never a half, but a
         * one-term stage halves as a modulate by a 0.5 constant (the unit's
         * constant has a free half for it when the term is not a constant
         * itself).  The game's one DIVIDE_2 is m404's guide line, RASC / 2. */
        mode = GL_MODULATE;
        memset(&args[1], 0, sizeof(args[1]));
        args[1].src = GL_CONSTANT;
        args[1].operand = rgb ? GL_SRC_COLOR : GL_SRC_ALPHA;
        args[1].is_const = 1;
        args[1].konst[0] = args[1].konst[1] = args[1].konst[2] = args[1].konst[3] = 0.5f;
        n = 2;
        scale = GX_CS_SCALE_1;
    }
    /* M35 (PLAN.md 50): in the colour channel, a register's or konst's
     * *alpha* read as a colour (GX_CC_A0/A1/A2, the alpha konst selects) is a
     * broadcast of one number, which the unit's constant can carry in its
     * RGB half just as well as in its A half -- and the A half is what the
     * stage's alpha channel, emitted after this one, needs for its own
     * constant.  The shadow pass's caster stage is exactly that pair:
     * colour A1 (the shadow's darkness) and alpha A0 (the material's), two
     * different numbers, and since M16 the colour's claim of the A half made
     * the alpha "collide" and draw with the darkness as its alpha: the caster
     * blended (SRCALPHA) at A1 over the black pass, and every shadow map in
     * the game held A1^2 instead of A1.  So: the RGB-operand constants claim
     * first, then an alpha-broadcast constant takes the RGB half when it is
     * free and the A half otherwise.  `--oldakonst` is the M16..M34 claim. */
    if (rgb && !port_opt.oldkonst && !port_opt.oldakonst &&
        !(n > 0 && args[0].is_zero) && !(n > 1 && args[1].is_zero) && !(n > 2 && args[2].is_zero)) {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < n; i++) {
                int wants_alpha;
                if (!args[i].is_const || args[i].is_zero) {
                    continue;
                }
                wants_alpha = args[i].operand == GL_SRC_ALPHA ||
                              args[i].operand == GL_ONE_MINUS_SRC_ALPHA;
                if (pass == 0 && wants_alpha) {
                    continue;
                }
                if (pass == 1 && !wants_alpha) {
                    continue;
                }
                if (wants_alpha && !(*konst_set & 1) &&
                    !((*konst_set & 2) && konst_out[3] == args[i].konst[3])) {
                    float v = args[i].konst[3];
                    args[i].konst[0] = args[i].konst[1] = args[i].konst[2] = v;
                    args[i].operand = args[i].operand == GL_SRC_ALPHA ? GL_SRC_COLOR
                                                                       : GL_ONE_MINUS_SRC_COLOR;
                    memcpy(konst_out, args[i].konst, sizeof(float) * 3);
                    *konst_set |= 1;
                    args[i].is_const = 2; /* claimed: skip it below */
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (args[i].is_const == 2) {
            args[i].is_const = 1;
            continue;
        }
        if (args[i].is_zero) {
            /* Zero as a live argument only survives here in shapes the table
             * above did not fold away; a black constant is the honest value,
             * and it claims its half of the unit's constant like any other. */
            args[i].src = GL_CONSTANT;
            args[i].operand = rgb ? GL_SRC_COLOR : GL_SRC_ALPHA;
            args[i].is_const = 1;
            memset(args[i].konst, 0, sizeof(args[i].konst));
        }
        if (args[i].is_const) {
            /* The unit's one GL_TEXTURE_ENV_COLOR is two constants, not one:
             * a colour operand reads its RGB and an alpha operand reads its A,
             * and GL never couples them.  Before M16 the four floats were
             * claimed whole, so a stage whose colour used K0 and whose alpha
             * used K0's alpha "collided" with itself (PLAN.md 31.4), and a
             * stage whose alpha wanted KASEL_1 got K0's alpha instead.
             * `konst_set` is a bitmask now: 1 = RGB claimed, 2 = A claimed. */
            int wants_alpha = args[i].operand == GL_SRC_ALPHA ||
                              args[i].operand == GL_ONE_MINUS_SRC_ALPHA;
            if (port_opt.oldkonst) {
                /* the pre-M16 claim, whole, for the A/B */
                if (args[i].is_zero) {
                    continue;
                }
                if (!*konst_set) {
                    memcpy(konst_out, args[i].konst, sizeof(float) * 4);
                    *konst_set = 3;
                } else if (memcmp(konst_out, args[i].konst, sizeof(float) * 4) != 0) {
                    gx_warn("TEV: a stage needs two different constants; the first wins");
                    cfg_konst_collisions++;
                }
            } else if (wants_alpha) {
                if (!(*konst_set & 2)) {
                    konst_out[3] = args[i].konst[3];
                    *konst_set |= 2;
                } else if (konst_out[3] != args[i].konst[3]) {
                    /* M30 (PLAN.md 45, cause B): the one collision the water
                     * shaders have -- the colour side lerps by a register's
                     * alpha (A0 = 0.25) and the alpha side wants KONST = 1 --
                     * has a second source for a 1.0 when the rasterised alpha
                     * is a known 1.0 (channel 0 unlit, its material alpha 255
                     * from the register): the primary colour's alpha.
                     * m434's pond drew at a quarter of its alpha without it. */
                    const GXChanCtrl* c0 = &gx.chan[0];
                    if (args[i].konst[3] >= 0.996f && !c0->enable && c0->mat_src == 0 &&
                        c0->mat.a == 255 && !port_opt.noregchain) {
                        args[i].src = GL_PRIMARY_COLOR;
                        args[i].is_const = 0;
                    } else {
                        gx_warn("TEV: a stage needs two different constants; the first wins");
                        cfg_konst_collisions++;
                    }
                }
            } else {
                if (!(*konst_set & 1)) {
                    memcpy(konst_out, args[i].konst, sizeof(float) * 3);
                    *konst_set |= 1;
                } else if (memcmp(konst_out, args[i].konst, sizeof(float) * 3) != 0) {
                    gx_warn("TEV: a stage needs two different constants; the first wins");
                    cfg_konst_collisions++;
                }
            }
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
        gx_warn("TEV: GX_CS_DIVIDE_2 on a stage with two or more terms has no GL 1.3 equivalent; drawn at scale 1");
    }
}

/* ---- a stage that writes a TEV register (M16, PLAN.md 31.3) ----------------
 *
 * A GX stage can write GX_TEVREG0..2 instead of PREV, and a later stage can
 * read that register back as C0..2 / A0..2.  GL 1.3 has one carrier between
 * units -- PREVIOUS -- and this port had always treated the destination as
 * PREV and the read-back as the register's *constant* (whatever
 * GXSetTevColor last put there), which is what the board eyes were: hsfdraw.c
 * SetTevStageTex's two-texture blend
 *
 *     stage A:  T_A * RAS            -> PREV      alpha T_Aa * K_A
 *     stage B:  T_B * RAS            -> REG2      alpha T_Ba * K_B
 *     stage C:  lerp(PREV, C2, A2)   -> PREV      alpha APREV
 *
 * drawn as stage B overwriting PREV and stage C lerping it with black by zero
 * (PLAN.md 30.6: the eye atlas lost, the 32x32 overlay alone).
 *
 * That shape *is* expressible, because RAS factors out of both terms:
 *
 *     out.rgb = RAS * (T_A * (1 - q) + T_B * q),   q = T_Ba * K_B
 *     out.a   = T_Aa * K_A
 *
 * which is three units with ARB_texture_env_crossbar (the card has it):
 *
 *     unit A:  rgb = T_A                     a = T_Ba * K_B        (crossbar: unit B's alpha)
 *     unit B:  rgb = lerp(PREV, T_B, PREV.a) a = T_Aa * K_A        (crossbar: unit A's alpha)
 *     unit C:  rgb = PREV * RAS              a = PREV
 *
 * Every other register write is still folded to PREV and is now *counted*
 * by --gxwarn instead of passing silently.  --noregfix keeps the old
 * rendering for the A/B. */
unsigned gl13_frame_number(void);
static int reg_write_warned;

/* the (ZERO, X, Y, ZERO) modulate shape, either order */
static int is_modulate(const u8* in, u8 x, u8 y, u8 zero) {
    return in[0] == zero && in[3] == zero &&
           ((in[1] == x && in[2] == y) || (in[1] == y && in[2] == x));
}

static int stage_plain(const GXTevStage* s) {
    return s->cop == GX_TEV_ADD && s->aop == GX_TEV_ADD && s->cbias == GX_TB_ZERO &&
           s->abias == GX_TB_ZERO && s->cscale == GX_CS_SCALE_1 &&
           s->ascale == GX_CS_SCALE_1 && ras_table(s) == NULL &&
           SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]) == SWAP_IDENTITY;
}

/* Does the triple starting at stage k have the shape above?  Returns the
 * register it goes through (0..2) or -1. */
static int regfix_match(int k, int stages) {
    const GXTevStage *a, *b, *c;
    int reg;
    if (port_opt.noregfix || !gl13_have_crossbar || k + 2 >= stages) {
        return -1;
    }
    a = &gx.tev[k];
    b = &gx.tev[k + 1];
    c = &gx.tev[k + 2];
    if (!stage_plain(a) || !stage_plain(b) || !stage_plain(c)) {
        return -1;
    }
    if (a->creg != GX_TEVPREV || a->areg != GX_TEVPREV || c->creg != GX_TEVPREV ||
        c->areg != GX_TEVPREV) {
        return -1;
    }
    if (b->creg != b->areg || b->creg == GX_TEVPREV) {
        return -1;
    }
    reg = (int)b->creg - GX_TEVREG0;
    if (reg < 0 || reg > 2) {
        return -1;
    }
    /* both texture stages sample something, on the same colour channel */
    if (gx_bound_tex(a->map) == NULL || a->coord >= GX_TEXCOORDS ||
        gx_bound_tex(b->map) == NULL || b->coord >= GX_TEXCOORDS || a->chan != b->chan ||
        gx.num_ind) {
        return -1;
    }
    /* A: T*RAS, alpha a modulate of two of {TEXA, KONST, RASA} */
    if (!is_modulate(a->cin, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO)) {
        return -1;
    }
    {
        int i, ok = 1;
        for (i = 1; i <= 2; i++) {
            u8 v = a->ain[i];
            if (v != GX_CA_TEXA && v != GX_CA_KONST && v != GX_CA_RASA && v != GX_CA_ZERO) {
                ok = 0;
            }
        }
        /* a real modulate: (ZERO, X, ZERO, ZERO) is lerp(0, X, 0) = 0 on GX */
        if (!ok || a->ain[0] != GX_CA_ZERO || a->ain[3] != GX_CA_ZERO ||
            a->ain[1] == GX_CA_ZERO || a->ain[2] == GX_CA_ZERO) {
            return -1;
        }
    }
    /* B: T*RAS into the register, alpha T_Ba*K_B or nothing */
    if (!is_modulate(b->cin, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO)) {
        return -1;
    }
    /* C: lerp(PREV, Creg, q) with q the register's alpha or B's texture alpha;
     * alpha passes PREV */
    if (c->cin[0] != GX_CC_CPREV || c->cin[1] != GX_CC_C0 + 2 * reg || c->cin[3] != GX_CC_ZERO) {
        return -1;
    }
    if (c->cin[2] == GX_CC_A0 + 2 * reg) {
        if (!is_modulate(b->ain, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO)) {
            return -1;
        }
    } else if (c->cin[2] == GX_CC_TEXA) {
        if (c->map != b->map || gx_bound_tex(c->map) == NULL) {
            return -1;
        }
    } else {
        return -1;
    }
    if (c->ain[0] != GX_CA_ZERO || c->ain[1] != GX_CA_ZERO || c->ain[2] != GX_CA_ZERO ||
        c->ain[3] != GX_CA_APREV) {
        return -1;
    }
    return reg;
}

/* M22 (PLAN.md 37): the second register shape, hsfdraw.c:1290-1305 -- the
 * results-screen portraits, and any material with a reflection *mask*:
 *
 *     stage A:  T_A * RAS              -> PREV    alpha  T_Aa * K_A (or one source)
 *     stage B:  T_mask * K             -> REG2    alpha  APREV       -> REG2
 *     stage C:  lerp(PREV, T_ref, C2)  -> PREV    alpha  APREV
 *
 * i.e. the reflection replaces the lit face where the mask says so, by
 * K * mask per channel.  Three units, the mask's value carried in an alpha:
 *
 *     unit A:  as the stage says (the generic emitter)
 *     unit B:  rgb = PREV                a = T_mask.a * K   -- the mask is bound
 *              with red swapped into alpha through the texture cache
 *     unit C:  rgb = lerp(PREV, T_ref, PREV.a)   a = stage A's alpha, its
 *              texture read across (crossbar) and its constant in this unit's
 *
 * Exact when the mask is grey (one value for the three channels) and K is
 * the scalar hsfdraw.c's SetKColor packs, which is what the game has.  The
 * textured highlight that follows the triple on the portraits is the hilite
 * fold's mode 2 (gx_internal.h). */
static int regfix2_match(int k, int stages) {
    const GXTevStage *a, *b, *c;
    int reg, i;
    if (port_opt.noregfix || port_opt.nohilitetex || !gl13_have_crossbar || k + 2 >= stages) {
        return -1;
    }
    a = &gx.tev[k];
    b = &gx.tev[k + 1];
    c = &gx.tev[k + 2];
    if (!stage_plain(a) || !stage_plain(b) || !stage_plain(c)) {
        return -1;
    }
    if (a->creg != GX_TEVPREV || a->areg != GX_TEVPREV || c->creg != GX_TEVPREV ||
        c->areg != GX_TEVPREV || b->creg != b->areg || b->creg == GX_TEVPREV) {
        return -1;
    }
    reg = (int)b->creg - GX_TEVREG0;
    if (reg < 0 || reg > 2) {
        return -1;
    }
    if (gx_bound_tex(a->map) == NULL || a->coord >= GX_TEXCOORDS ||
        gx_bound_tex(b->map) == NULL || b->coord >= GX_TEXCOORDS ||
        gx_bound_tex(c->map) == NULL || c->coord >= GX_TEXCOORDS || gx.num_ind) {
        return -1;
    }
    /* A: T*RAS; alpha a modulate of two of {TEXA, KONST} or one of them */
    if (!is_modulate(a->cin, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO)) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        u8 v = a->ain[i];
        if (v != GX_CA_TEXA && v != GX_CA_KONST && v != GX_CA_ZERO) {
            return -1;
        }
    }
    if (a->ain[0] != GX_CA_ZERO) {
        return -1;
    }
    if (a->ain[3] != GX_CA_ZERO) {
        if (a->ain[1] != GX_CA_ZERO || a->ain[2] != GX_CA_ZERO) {
            return -1; /* X*Y + Z: three sources, not this shape */
        }
    } else if (a->ain[1] == GX_CA_ZERO || a->ain[2] == GX_CA_ZERO) {
        return -1; /* lerp(0, X, 0) = 0 on GX */
    }
    /* B: T_mask * K into the register, alpha APREV */
    if (!is_modulate(b->cin, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO)) {
        return -1;
    }
    if (b->ain[0] != GX_CA_ZERO || b->ain[1] != GX_CA_ZERO || b->ain[2] != GX_CA_ZERO ||
        b->ain[3] != GX_CA_APREV) {
        return -1;
    }
    /* C: lerp(PREV, T_ref, Creg), alpha APREV */
    if (c->cin[0] != GX_CC_CPREV || c->cin[1] != GX_CC_TEXC || c->cin[2] != GX_CC_C0 + 2 * reg ||
        c->cin[3] != GX_CC_ZERO) {
        return -1;
    }
    if (c->ain[0] != GX_CA_ZERO || c->ain[1] != GX_CA_ZERO || c->ain[2] != GX_CA_ZERO ||
        c->ain[3] != GX_CA_APREV) {
        return -1;
    }
    return reg;
}

/* M22: the third register shape, hsfdraw.c:1243 (texCol[i].a == 1, an
 * animated texture with a tint over the material -- the results-screen
 * portrait faces, drawn as a separate 16-vertex quad after the frame):
 *
 *     stage B:  T_i * K_rgb          -> REG2   alpha  K_a * APREV -> REG2
 *     stage C:  lerp(PREV, C2, K_c)  -> PREV   alpha  T_i.a * APREV
 *
 * = PREV * (1 - K_c) + T_i * K_rgb * K_c, and the register's alpha is dead
 * (C reads PREV's).  One unit says it -- INTERPOLATE(TEXTURE, PREVIOUS,
 * CONSTANT.a = K_c) -- when K_rgb is white, which the portraits' is; a
 * tint is counted and dropped.  Unit B passes everything through. */
static int regfix3_match(int k, int stages) {
    const GXTevStage *b, *c;
    int reg;
    if (port_opt.noregfix || port_opt.nohilitetex || k + 1 >= stages) {
        return -1;
    }
    b = &gx.tev[k];
    c = &gx.tev[k + 1];
    if (!stage_plain(b) || !stage_plain(c)) {
        return -1;
    }
    if (b->creg != b->areg || b->creg == GX_TEVPREV || c->creg != GX_TEVPREV ||
        c->areg != GX_TEVPREV) {
        return -1;
    }
    reg = (int)b->creg - GX_TEVREG0;
    if (reg < 0 || reg > 2) {
        return -1;
    }
    if (gx_bound_tex(b->map) == NULL || b->coord >= GX_TEXCOORDS || c->map != b->map ||
        c->coord != b->coord || gx.num_ind) {
        return -1;
    }
    if (!is_modulate(b->cin, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO)) {
        return -1;
    }
    if (c->cin[0] != GX_CC_CPREV || c->cin[1] != GX_CC_C0 + 2 * reg || c->cin[2] != GX_CC_KONST ||
        c->cin[3] != GX_CC_ZERO) {
        return -1;
    }
    if (!is_modulate(c->ain, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO)) {
        return -1;
    }
    return reg;
}

static unsigned stat_regfix3_tinted;

static unsigned stat_regfix3_tint_drawn; /* M23: K_c = 1, the tint modulates the texture */

static void regfix3_emit(int which, int unit, int k) {
    const GXTevStage* b = &gx.tev[k];
    const GXTevStage* c = &gx.tev[k + 1];
    float konst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float tint[4], kc[4];
    if (!gl13_live()) {
        return;
    }
    glc_texenvi(unit, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glc_texenvf(unit, GL_RGB_SCALE, 1.0f);
    glc_texenvf(unit, GL_ALPHA_SCALE, 1.0f);
    if (which == 0) {
        /* everything passes: the register write is folded into unit C */
        glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        konst_color(b, 0, tint);
        konst_color(c, 0, kc);
        if ((tint[0] < 0.996f || tint[1] < 0.996f || tint[2] < 0.996f) &&
            (kc[0] < 0.996f || port_opt.notint)) {
            stat_regfix3_tinted++;
            gx_warn("TEV: a tinted texture in the lerp-by-konst register shape; "
                    "the tint is dropped (PLAN.md 37)");
            /* M23 --tintlog: where they are (the dropped ones).  One line per frame that has
             * any, the first pair's tint, konst weight and texture. */
            if (port_opt.tintlog) {
                static unsigned last_frame = ~0u, lines;
                unsigned f = gl13_frame_number();
                if (f != last_frame && lines < 400) {
                    const GXTexObjPort* t = gx_bound_tex(b->map);
                    last_frame = f;
                    lines++;
                    port_log("port> tintlog: frame %u ovl %d tint %.2f %.2f %.2f  lerp %.2f  "
                             "tex %p %ux%u fmt %u (pair %u)\n",
                             f, (int)omcurovl, tint[0], tint[1], tint[2], kc[0],
                             t ? t->image : NULL, t ? t->width : 0u, t ? t->height : 0u,
                             t ? (unsigned)t->format : 0u, stat_regfix3_tinted);
                }
            }
        }
    } else {
        konst_color(c, 0, kc);
        konst_color(b, 0, tint);
        if (kc[0] >= 0.996f && !port_opt.notint) {
            /* M23 (PLAN.md 38): K_c = 1 -- the lerp takes the texture whole,
             * PREV drops out, and the tint has a home: rgb = T * K_rgb.
             * Every tinted pair the walk counted (2,256 on 16,000 frames,
             * all of them the mode select's, K_rgb = 0.95) is this case. */
            konst[0] = tint[0];
            konst[1] = tint[1];
            konst[2] = tint[2];
            glc_texenvi(unit, GL_COMBINE_RGB, GL_MODULATE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE1_RGB, GL_CONSTANT);
            glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
            stat_regfix3_tint_drawn++;
        } else {
            /* rgb = lerp(PREV, T, K_c): Arg0*Arg2 + Arg1*(1-Arg2);  a = T.a * PREV.a */
            konst[3] = kc[0];
            glc_texenvi(unit, GL_COMBINE_RGB, GL_INTERPOLATE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE2_RGB, GL_CONSTANT);
            glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_ALPHA);
        }
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        glc_texenvi(unit, GL_SOURCE1_ALPHA, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
    }
    glc_texenv_color(unit, konst);
}

/* the swap that binds the mask with its red channel in alpha */
#define SWAP_RED_TO_ALPHA ((u8)(GX_CH_RED | (GX_CH_GREEN << 2) | (GX_CH_BLUE << 4) | \
                                (GX_CH_RED << 6)))

/* stage A's alpha, rebuilt in another unit: its texture read across the
 * crossbar, its constant in this unit's slot */
static void regfix_emit_alpha_of(int unit, const GXTevStage* a, int k, float* konst) {
    int i, n = 0;
    if (a->ain[3] != GX_CA_ZERO) {
        /* the one-source pass */
        u8 v = a->ain[3];
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
        if (v == GX_CA_TEXA) {
            glc_texenvi(unit, GL_SOURCE0_ALPHA, (int)(GL_TEXTURE0 + k));
        } else {
            konst_color(a, 1, konst);
            glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_CONSTANT);
        }
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        return;
    }
    for (i = 1; i <= 2; i++) {
        u8 v = a->ain[i];
        GLenum src;
        if (v == GX_CA_ZERO) {
            continue;
        }
        src = v == GX_CA_TEXA    ? (GLenum)(GL_TEXTURE0 + k)
              : v == GX_CA_RASA  ? GL_PRIMARY_COLOR
                                 : GL_CONSTANT;
        if (v == GX_CA_KONST) {
            konst_color(a, 1, konst);
        }
        glc_texenvi(unit, n == 0 ? GL_SOURCE0_ALPHA : GL_SOURCE1_ALPHA, (int)src);
        glc_texenvi(unit, n == 0 ? GL_OPERAND0_ALPHA : GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
        n++;
    }
    glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE); /* n is 2: the matcher says so */
}

static void regfix2_emit(int which, int unit, int k) {
    const GXTevStage* a = &gx.tev[k];
    const GXTevStage* b = &gx.tev[k + 1];
    float konst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!gl13_live() || which == 0) {
        return; /* unit A is the generic emitter's */
    }
    glc_texenvi(unit, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glc_texenvf(unit, GL_RGB_SCALE, 1.0f);
    glc_texenvf(unit, GL_ALPHA_SCALE, 1.0f);
    if (which == 1) {
        /* rgb = PREV;  a = T_mask.a (= its red, by the bind) * K */
        konst_color(b, 0, konst);
        konst[3] = konst[0];
        glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        glc_texenvi(unit, GL_SOURCE1_ALPHA, GL_CONSTANT);
        glc_texenvi(unit, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
    } else {
        /* rgb = lerp(PREV, T_ref, PREV.a): GL_INTERPOLATE is
         * Arg0*Arg2 + Arg1*(1-Arg2);  a = stage A's */
        int dbg = port_opt.regfix2dbg ? (int)(gl13_frame_number() % 4u) : 0;
        if (dbg == 1) { /* --regfix2dbg: unit A's output alone */
            glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        } else if (dbg == 2) { /* the reflection alone */
            glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        } else if (dbg == 3) { /* the mask factor, as grey */
            glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_ALPHA);
        } else {
            glc_texenvi(unit, GL_COMBINE_RGB, GL_INTERPOLATE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE2_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_ALPHA);
        }
        regfix_emit_alpha_of(unit, a, k, konst);
    }
    glc_texenv_color(unit, konst);
}

/* One of the three units of a matched triple: `which` is 0 (A), 1 (B), 2 (C)
 * and `unit` is the GL unit it lands on (== the stage index). */
/* M38 (PLAN.md 53.10): the fold without the crossbar, for the M30 three-
 * texture shape.  When the M16 triple's lerp factor is T_B.a itself
 * (stage C's c = TEXA), unit B can read it from its own texture, and unit A
 * can make stage A's alpha from its own; nothing is read across.  Exactly
 * the same values as the crossbar fold -- and on the Radeon 9000, in m448's
 * six-unit felt chain, unit 1's crossbar read of unit 0's texture comes back
 * with alpha 0, which the draw's alpha test (>= 1) turned into a black
 * table.  Set by regfix5_emit around its two calls; --foldxbar keeps the
 * crossbar fold for the A/B. */
static int regfix_no_xbar;
static unsigned stat_regfix_no_xbar;

static void regfix_emit(int which, int unit, int k) {
    const GXTevStage* a = &gx.tev[k];
    const GXTevStage* b = &gx.tev[k + 1];
    const GXTevStage* c = &gx.tev[k + 2];
    float konst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!gl13_live()) {
        return;
    }
    glc_texenvi(unit, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glc_texenvf(unit, GL_RGB_SCALE, 1.0f);
    glc_texenvf(unit, GL_ALPHA_SCALE, 1.0f);
    if (regfix_no_xbar && c->cin[2] == GX_CC_TEXA && which <= 1) {
        if (which == 0) {
            /* rgb = T_A;  a = stage A's alpha, from this unit's own texture */
            int i, n = 0;
            glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
            for (i = 1; i <= 2; i++) {
                u8 v = a->ain[i];
                GLenum src;
                if (v == GX_CA_ZERO) {
                    continue;
                }
                src = v == GX_CA_TEXA ? GL_TEXTURE : v == GX_CA_RASA ? GL_PRIMARY_COLOR : GL_CONSTANT;
                if (v == GX_CA_KONST) {
                    konst_color(a, 1, konst);
                }
                glc_texenvi(unit, n == 0 ? GL_SOURCE0_ALPHA : GL_SOURCE1_ALPHA, (int)src);
                glc_texenvi(unit, n == 0 ? GL_OPERAND0_ALPHA : GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
                n++;
            }
            glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE); /* n is 2: the matcher says so */
        } else {
            /* rgb = lerp(PREV = T_A, T_B, T_B.a) -- T_B.a is this unit's own
             * texel alpha;  a = PREV, stage A's */
            glc_texenvi(unit, GL_COMBINE_RGB, GL_INTERPOLATE);
            glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
            glc_texenvi(unit, GL_SOURCE2_RGB, GL_TEXTURE);
            glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_ALPHA);
            glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
            glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        }
        glc_texenv_color(unit, konst);
        stat_regfix_no_xbar++;
        return;
    }
    if (which == 0) {
        /* rgb = T_A;  a = q, read across from unit B */
        glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        if (c->cin[2] == GX_CC_TEXA) {
            glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
            glc_texenvi(unit, GL_SOURCE0_ALPHA, (int)(GL_TEXTURE0 + k + 1));
            glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        } else {
            konst_color(b, 1, konst);
            glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE);
            glc_texenvi(unit, GL_SOURCE0_ALPHA, (int)(GL_TEXTURE0 + k + 1));
            glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
            glc_texenvi(unit, GL_SOURCE1_ALPHA, GL_CONSTANT);
            glc_texenvi(unit, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
        }
    } else if (which == 1) {
        /* rgb = lerp(PREV = T_A, T_B, PREV.a = q): GL_INTERPOLATE is
         * Arg0*Arg2 + Arg1*(1-Arg2);  a = stage A's own alpha, its texture
         * read across from unit A */
        int i, n = 0;
        glc_texenvi(unit, GL_COMBINE_RGB, GL_INTERPOLATE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE2_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_ALPHA);
        for (i = 1; i <= 2; i++) {
            u8 v = a->ain[i];
            GLenum src;
            if (v == GX_CA_ZERO) {
                continue;
            }
            src = v == GX_CA_TEXA    ? (GLenum)(GL_TEXTURE0 + k)
                  : v == GX_CA_RASA  ? GL_PRIMARY_COLOR
                                     : GL_CONSTANT;
            if (v == GX_CA_KONST) {
                konst_color(a, 1, konst);
            }
            glc_texenvi(unit, n == 0 ? GL_SOURCE0_ALPHA : GL_SOURCE1_ALPHA, (int)src);
            glc_texenvi(unit, n == 0 ? GL_OPERAND0_ALPHA : GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
            n++;
        }
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_MODULATE); /* n is 2: the matcher says so */
    } else {
        /* rgb = PREV * RAS;  a = PREV */
        glc_texenvi(unit, GL_COMBINE_RGB, GL_MODULATE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    }
    glc_texenv_color(unit, konst);
}

/* M30 (PLAN.md 45, cause I): the M16 triple with a third texture -- hsfdraw.c
 * SetTevStageTex for a material of three textures, the kColor == 1 variant
 * (:1239) on the third: after A, B -> REG2, C = lerp(PREV, C2, T_B.a) come
 *
 *     stage B':  T_C * RAS            -> REG2     alpha 0
 *     stage C':  lerp(PREV, C2, T_C.a) -> PREV    alpha APREV
 *
 * m448's felt table (Goomba's Chip Flip: the felt, an environment map, a
 * decal, then the shadow).  RAS still factors out of everything, so the
 * fold is the M16 one with the multiply moved one unit later:
 *
 *     unit A:   rgb = T_A                           a = q (unit B's texel alpha, crossbar)
 *     unit B:   rgb = lerp(PREV, T_B, PREV.a)       a = stage A's alpha
 *     unit C:   rgb = lerp(PREV, T_C, T_C.a)        a = PREV      -- T_C bound here
 *     unit B':  rgb = PREV * RAS                    a = PREV
 *     unit C':  pass
 *
 * Until M30 stages B' and C' went down the generic path: B' overwrote PREV
 * with T_C * RAS and C' lerped that with REG2's constant (black) -- the felt
 * drew black wherever the decal's alpha was low. */
static int regfix5_match(int k, int stages) {
    const GXTevStage *b2, *c2;
    int reg;
    if (k + 4 >= stages || gl13_max_tex_units < k + 5) {
        return -1;
    }
    reg = regfix_match(k, stages);
    if (reg < 0) {
        return -1;
    }
    b2 = &gx.tev[k + 3];
    c2 = &gx.tev[k + 4];
    if (!stage_plain(b2) || !stage_plain(c2)) {
        return -1;
    }
    if (b2->creg != GX_TEVREG0 + reg || b2->areg != GX_TEVREG0 + reg || c2->creg != GX_TEVPREV ||
        c2->areg != GX_TEVPREV) {
        return -1;
    }
    if (gx_bound_tex(b2->map) == NULL || b2->coord >= GX_TEXCOORDS || c2->map != b2->map ||
        b2->chan != gx.tev[k].chan) {
        return -1;
    }
    if (!is_modulate(b2->cin, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO)) {
        return -1;
    }
    if (c2->cin[0] != GX_CC_CPREV || c2->cin[1] != GX_CC_C0 + 2 * reg || c2->cin[2] != GX_CC_TEXA ||
        c2->cin[3] != GX_CC_ZERO) {
        return -1;
    }
    if (c2->ain[0] != GX_CA_ZERO || c2->ain[1] != GX_CA_ZERO || c2->ain[2] != GX_CA_ZERO ||
        c2->ain[3] != GX_CA_APREV) {
        return -1;
    }
    return reg;
}

static unsigned stat_regfix5;

static void regfix5_emit(int which, int unit, int k) {
    float konst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!gl13_live()) {
        return;
    }
    if (which <= 1) {
        regfix_no_xbar = !port_opt.foldxbar;
        regfix_emit(which, unit, k); /* units A and B are the M16 triple's */
        regfix_no_xbar = 0;
        return;
    }
    glc_texenvi(unit, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glc_texenvf(unit, GL_RGB_SCALE, 1.0f);
    glc_texenvf(unit, GL_ALPHA_SCALE, 1.0f);
    if (which == 2) {
        /* rgb = lerp(PREV, T_C, T_C.a): GL_INTERPOLATE is Arg0*Arg2 + Arg1*(1-Arg2) */
        glc_texenvi(unit, GL_COMBINE_RGB, GL_INTERPOLATE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE2_RGB, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_ALPHA);
    } else if (which == 3) {
        glc_texenvi(unit, GL_COMBINE_RGB, GL_MODULATE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
    } else {
        glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
    }
    glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
    glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
    glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    glc_texenv_color(unit, konst);
}

/* M30 (PLAN.md 45, cause B): the fourth register shape, m427's headlamp
 * (map.c:2371) -- a projected lamp texture lit by the spot channel, a caustic
 * tinted by a constant, summed under the same light:
 *
 *     stage A:  T_lamp * RAS  (x4)    -> REGx
 *     stage B:  T_caustic.a * K       -> REGy      (K a register nobody writes)
 *     stage C:  C_y * RAS + C_x       -> PREV
 *
 * = RAS * (4 T_lamp + T_caustic.a * K) up to the clamp of the first term,
 * and that factorisation is three units:
 *
 *     unit A:  rgb = T_lamp, scale 4           a = PREV
 *     unit B:  rgb = T_caustic.a * K + PREV    a = PREV      (MODULATE_ADD_ATI)
 *     unit C:  rgb = PREV * RAS                a = stage C's own
 *
 * GX clamps 4 T_lamp RAS before the sum where this clamps 4 T_lamp: the
 * two differ only where the lamp texture is over a quarter and the cone
 * partial -- the bright core, where both saturate.  The chain planner below
 * cannot say this shape (stage C reads two stages back), so it is matched
 * here like the M16 and M22 triples. */
static int regfix4_match(int k, int stages) {
    const GXTevStage *a, *b, *c;
    int rx, ry, j;
    if (port_opt.noregfix || port_opt.noregchain || !gl13_have_combine3 || k + 2 >= stages) {
        return -1;
    }
    a = &gx.tev[k];
    b = &gx.tev[k + 1];
    c = &gx.tev[k + 2];
    if (!stage_plain(b) || !stage_plain(c)) {
        return -1;
    }
    if (a->cop != GX_TEV_ADD || a->cbias != GX_TB_ZERO || ras_table(a) != NULL ||
        SWAP_PACK(gx.swap_tbl[a->tex_swap & 3]) != SWAP_IDENTITY) {
        return -1;
    }
    if (a->creg == GX_TEVPREV || b->creg == GX_TEVPREV || a->creg == b->creg ||
        c->creg != GX_TEVPREV || c->areg != GX_TEVPREV) {
        return -1;
    }
    rx = (int)a->creg - GX_TEVREG0;
    ry = (int)b->creg - GX_TEVREG0;
    if (rx < 0 || rx > 2 || ry < 0 || ry > 2) {
        return -1;
    }
    if (gx_bound_tex(a->map) == NULL || a->coord >= GX_TEXCOORDS ||
        gx_bound_tex(b->map) == NULL || b->coord >= GX_TEXCOORDS || a->chan != c->chan) {
        return -1;
    }
    if (!is_modulate(a->cin, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO)) {
        return -1;
    }
    /* B: a texture channel times a constant register (one neither A nor B
     * writes) or KONST */
    if (b->cin[0] != GX_CC_ZERO || b->cin[3] != GX_CC_ZERO) {
        return -1;
    }
    {
        int have_tex = 0, have_k = 0;
        for (j = 1; j <= 2; j++) {
            u8 v = b->cin[j];
            if (v == GX_CC_TEXC || v == GX_CC_TEXA) {
                have_tex++;
            } else if (v == GX_CC_KONST) {
                have_k++;
            } else if (v == GX_CC_C0 || v == GX_CC_C1 || v == GX_CC_C2) {
                int r = (v - GX_CC_C0) / 2;
                if (r == rx || r == ry) {
                    return -1;
                }
                have_k++;
            } else {
                return -1;
            }
        }
        if (have_tex != 1 || have_k != 1) {
            return -1;
        }
    }
    /* C: C_y * RAS + C_x */
    if (c->cin[0] != GX_CC_ZERO || c->cin[1] != GX_CC_C0 + 2 * ry || c->cin[2] != GX_CC_RASC ||
        c->cin[3] != GX_CC_C0 + 2 * rx) {
        return -1;
    }
    return rx;
}

static unsigned stat_regfix4;

static void regfix4_emit(int which, int unit, int k) {
    const GXTevStage* a = &gx.tev[k];
    const GXTevStage* b = &gx.tev[k + 1];
    const GXTevStage* c = &gx.tev[k + 2];
    float konst[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int konst_set = 0;
    if (!gl13_live()) {
        return;
    }
    glc_texenvi(unit, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glc_texenvf(unit, GL_RGB_SCALE, 1.0f);
    glc_texenvf(unit, GL_ALPHA_SCALE, 1.0f);
    if (which == 0) {
        glc_texenvi(unit, GL_COMBINE_RGB, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvf(unit, GL_RGB_SCALE,
                    a->cscale == GX_CS_SCALE_2 ? 2.0f : a->cscale == GX_CS_SCALE_4 ? 4.0f : 1.0f);
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    } else if (which == 1) {
        int j;
        glc_texenvi(unit, GL_COMBINE_RGB, GL_MODULATE_ADD_ATI); /* Arg0 * Arg2 + Arg1 */
        for (j = 1; j <= 2; j++) {
            u8 v = b->cin[j];
            if (v == GX_CC_TEXC || v == GX_CC_TEXA) {
                glc_texenvi(unit, GL_SOURCE0_RGB, GL_TEXTURE);
                glc_texenvi(unit, GL_OPERAND0_RGB, v == GX_CC_TEXA ? GL_SRC_ALPHA : GL_SRC_COLOR);
            } else {
                Arg r = color_arg(b, v, 0);
                memcpy(konst, r.konst, sizeof(konst));
                glc_texenvi(unit, GL_SOURCE2_RGB, GL_CONSTANT);
                glc_texenvi(unit, GL_OPERAND2_RGB, GL_SRC_COLOR);
            }
        }
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_COMBINE_ALPHA, GL_REPLACE);
        glc_texenvi(unit, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    } else {
        glc_texenvi(unit, GL_COMBINE_RGB, GL_MODULATE);
        glc_texenvi(unit, GL_SOURCE0_RGB, GL_PREVIOUS);
        glc_texenvi(unit, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glc_texenvi(unit, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glc_texenvi(unit, GL_OPERAND1_RGB, GL_SRC_COLOR);
        konst[0] = konst[1] = konst[2] = konst[3] = 0.0f;
        emit_channel(unit, 0, alpha_arg(c, c->ain[0], 0), alpha_arg(c, c->ain[1], 0),
                     alpha_arg(c, c->ain[2], 0), alpha_arg(c, c->ain[3], 0), c->aop, c->abias,
                     c->ascale, konst, &konst_set);
    }
    glc_texenv_color(unit, konst);
}

/* ---- register chains (M30, PLAN.md 45) --------------------------------------
 *
 * Outside hsfdraw.c the game's register writes are *chains*: a stage writes
 * REG0, the next reads C0 and writes REG1, the next reads C1 -- m427's water
 * (map.c:1033), its headlamp (map.c:2371), m417's pool (water.c:826).  The
 * three M16/M22 shapes above are hsfdraw's and match nothing of that, so
 * every such stage was emitted with the *register's constant* in place of
 * the value the previous stage had just computed: m427's cave flooded with
 * whatever GXSetTevColor had last left in REG0 and REG1 (PLAN.md 41b, cause
 * B).
 *
 * GL's one carrier between units is PREVIOUS, and PREVIOUS after unit i-1 is
 * stage i-1's result *whatever GX register it went to*.  So a read at stage
 * i of the register stage i-1 wrote is exactly PREVIOUS (the RGB or the alpha
 * operand by which half was read), and a chain where every register read is
 * of the immediately preceding stage is exact.  Two more rules make the rest
 * honest:
 *
 *  - a stage whose register write nothing later reads (and which is not the
 *    last stage, whose result is the screen whatever it names) passes
 *    PREVIOUS through on that side instead, so a CPREV/APREV read after it
 *    still sees what GX's PREV holds;
 *  - a read of a register written two or more stages back, or of PREV across
 *    a live register write, has no fixed-function form and is counted
 *    (`regchain: unexpressible`) and drawn as before.
 *
 * --noregchain is the pre-M30 picture. */
static u8 rc_ren_c[GX_TEV_STAGES][4]; /* per stage, per colour input: 0 / 1 (PREV rgb) / 2 (PREV a) */
static u8 rc_ren_a[GX_TEV_STAGES][4]; /* per stage, per alpha input: 0 / 2 */
static u8 rc_pass_c[GX_TEV_STAGES];   /* the unit's RGB is REPLACE(PREVIOUS): a dead register write */
static u8 rc_pass_a[GX_TEV_STAGES];
static u8 rc_carry[GX_TEV_STAGES];    /* M31: the unit's alpha is the stage's colour product (below) */
static unsigned stat_rc_renamed, stat_rc_passed, stat_rc_unexpressible, stat_rc_chains;
static unsigned stat_rc_carried;
static int rc_live_after(int i, int reg, int side, int stages);

/* M31 (PLAN.md 46): the scalar-in-alpha fold, m417's pool (water.c:826).
 *
 *     stage 3:  T_foam * RASA          -> REG2     (T_foam an I8: grey)
 *     stage 4:  lerp(PREV, C1, C2)     -> PREV
 *
 * Stage 4 wants both GX's PREV (stage 2's result) and stage 3's, and GL has
 * one carrier -- but stage 3's result is a *scalar* (a grey texture times an
 * alpha), and the alpha channel is idle in this chain (every stage's alpha
 * is KONST into a register nobody reads).  So unit 3 passes its RGB through
 * and computes the product in its alpha (TEXTURE.a is the intensity: the
 * port decodes I4/I8 with d[3] = I), and unit 4 reads C2 as PREVIOUS.a:
 * INTERPOLATE(C1, PREVIOUS, PREVIOUS.a), exact.  The planner takes the fold
 * only where the plain rename cannot say the chain (the next stage reads
 * both PREV and the register), the stage's own alpha write is dead, and the
 * next stage needs no PREV alpha; everything else is the M30 plan. */
static int rc_carry_ok(const GXTevStage* s, int i, int stages) {
    const GXTevStage* n;
    GXTexObjPort* t;
    u8 x;
    int j, r;
    if (port_opt.nocarry || i + 1 >= stages) {
        return 0;
    }
    if (s->cop != GX_TEV_ADD || s->cbias != GX_TB_ZERO || ras_table(s) != NULL ||
        SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]) != SWAP_IDENTITY) {
        return 0;
    }
    if (s->creg == GX_TEVPREV || s->creg > GX_TEVREG0 + 2) {
        return 0;
    }
    r = (int)s->creg;
    /* the stage's own alpha write must be a register nobody reads */
    if (s->areg == GX_TEVPREV || s->areg > GX_TEVREG0 + 2 || rc_live_after(i, (int)s->areg, 1, stages)) {
        return 0;
    }
    /* T * X, X a scalar with an alpha form */
    if (s->cin[0] != GX_CC_ZERO || s->cin[3] != GX_CC_ZERO) {
        return 0;
    }
    if (s->cin[1] == GX_CC_TEXC) {
        x = s->cin[2];
    } else if (s->cin[2] == GX_CC_TEXC) {
        x = s->cin[1];
    } else {
        return 0;
    }
    if (x == GX_CC_KONST) {
        float k[4];
        konst_color(s, 0, k);
        if (k[0] != k[1] || k[1] != k[2]) {
            return 0; /* a coloured constant is not a scalar */
        }
    } else if (x != GX_CC_RASA && x != GX_CC_A0 && x != GX_CC_A1 && x != GX_CC_A2) {
        return 0;
    }
    t = gx_bound_tex(s->map);
    if (t == NULL || s->coord >= GX_TEXCOORDS || (t->format != GX_TF_I4 && t->format != GX_TF_I8)) {
        return 0;
    }
    /* the next stage: reads the register (as colour), reads PREV, and needs no
     * alpha from PREV or from any register on either side */
    n = &gx.tev[i + 1];
    {
        int reads_reg = 0, reads_prev = 0;
        for (j = 0; j < 4; j++) {
            u8 a = n->cin[j];
            if (a == GX_CC_C0 + 2 * (r - 1)) reads_reg++;
            if (a == GX_CC_CPREV) reads_prev++;
            if (a == GX_CC_APREV || a == GX_CC_A0 || a == GX_CC_A1 || a == GX_CC_A2) return 0;
            if (n->ain[j] == GX_CA_APREV || n->ain[j] == GX_CA_A0 || n->ain[j] == GX_CA_A1 ||
                n->ain[j] == GX_CA_A2) {
                return 0;
            }
        }
        if (!reads_reg || !reads_prev) {
            return 0; /* the plain rename says it */
        }
    }
    return 1;
}

/* which stage m > i reads register `reg` (1..3) on `side` before another
 * write to it: 1 if any */
static int rc_live_after(int i, int reg, int side, int stages) {
    int m, j;
    for (m = i + 1; m < stages; m++) {
        const GXTevStage* t = &gx.tev[m];
        for (j = 0; j < 4; j++) {
            if (side == 0) { /* the register's RGB: read as GX_CC_Cn only */
                if (t->cin[j] == GX_CC_C0 + 2 * (reg - 1)) return 1;
            } else {         /* the register's alpha: GX_CC_An or GX_CA_An */
                if (t->cin[j] == GX_CC_A0 + 2 * (reg - 1)) return 1;
                if (t->ain[j] == GX_CA_A0 + (reg - 1)) return 1;
            }
        }
        if ((side == 0 ? t->creg : t->areg) == reg) return 0; /* overwritten first */
    }
    return 0;
}

static void regchain_plan(int stages, int skip_from, int skip_n) {
    int last_c[4], last_a[4]; /* producer stage of PREV, REG0..2 on each side; -1 = the constant */
    /* M31: what GL's PREVIOUS holds after the unit before this one -- the
     * producer stage of its RGB and of its alpha (a pass leaves them), and
     * whether the alpha is a carried colour product (rc_carry) rather than
     * a GX alpha.  A read is expressible iff the GX value it names is what
     * PREVIOUS holds; the M30 plan tested `prod == i - 1`, which is the same
     * thing except across a pass, where it counted an exact read as
     * unexpressible (a count, not a picture). */
    int gl_c = -1, gl_a = -1, carry_of = -1;
    int i, j, any = 0, bad = 0;
    memset(rc_ren_c, 0, sizeof(rc_ren_c));
    memset(rc_ren_a, 0, sizeof(rc_ren_a));
    memset(rc_pass_c, 0, sizeof(rc_pass_c));
    memset(rc_pass_a, 0, sizeof(rc_pass_a));
    memset(rc_carry, 0, sizeof(rc_carry));
    if (port_opt.noregchain) {
        return;
    }
    for (j = 0; j < 4; j++) {
        last_c[j] = last_a[j] = -1;
    }
    for (i = 0; i < stages; i++) {
        const GXTevStage* s = &gx.tev[i];
        if (skip_from >= 0 && i >= skip_from && i < skip_from + skip_n) {
            /* a matched M16/M22/M30 shape: emitted whole, and it leaves its
             * result in PREV on both sides */
            if (i == skip_from + skip_n - 1) {
                last_c[0] = last_a[0] = i;
                gl_c = gl_a = i;
                carry_of = -1;
            }
            continue;
        }
        for (j = 0; j < 4; j++) {
            u8 a = s->cin[j];
            int prod = -2, side = 0; /* -2: not a register read */
            if (a == GX_CC_CPREV) { prod = last_c[0]; }
            else if (a == GX_CC_APREV) { prod = last_a[0]; side = 1; }
            else if (a == GX_CC_C0 || a == GX_CC_C1 || a == GX_CC_C2) { prod = last_c[1 + (a - GX_CC_C0) / 2]; }
            else if (a == GX_CC_A0 || a == GX_CC_A1 || a == GX_CC_A2) { prod = last_a[1 + (a - GX_CC_A0) / 2]; side = 1; }
            if (prod == -2 || prod == -1) {
                continue; /* not a register, or the register's constant: as before */
            }
            if (a == GX_CC_CPREV) {
                if (prod != gl_c) bad++; /* GL_PREVIOUS already, or lost */
            } else if (a == GX_CC_APREV) {
                if (prod != gl_a || carry_of >= 0) bad++;
            } else if (!side && carry_of >= 0 && prod == carry_of) {
                rc_ren_c[i][j] = 2; /* M31: the register's RGB rides in PREVIOUS.a */
                any = 1;
            } else if (side ? (prod == gl_a && carry_of < 0) : (prod == gl_c)) {
                rc_ren_c[i][j] = side ? 2 : 1;
                any = 1;
            } else {
                bad++;
            }
        }
        for (j = 0; j < 4; j++) {
            u8 a = s->ain[j];
            int prod = -2;
            if (a == GX_CA_APREV) { prod = last_a[0]; }
            else if (a == GX_CA_A0 || a == GX_CA_A1 || a == GX_CA_A2) { prod = last_a[1 + (a - GX_CA_A0)]; }
            if (prod == -2 || prod == -1) {
                continue;
            }
            if (a == GX_CA_APREV) {
                if (prod != gl_a || carry_of >= 0) bad++;
            } else if (prod == gl_a && carry_of < 0) {
                rc_ren_a[i][j] = 2;
                any = 1;
            } else {
                bad++;
            }
        }
        /* the write: a register nobody reads becomes a pass (not on the last
         * stage: its result is the screen), so PREV survives across it */
        if (s->creg != GX_TEVPREV && s->creg <= GX_TEVREG0 + 2) {
            if (i < stages - 1 && !rc_live_after(i, (int)s->creg, 0, stages)) {
                rc_pass_c[i] = 1;
                any = 1;
            } else if (last_c[0] == gl_c && rc_carry_ok(s, i, stages)) {
                /* M31: the next stage wants both PREV and this register; the
                 * product is a scalar, so it rides in the unit's alpha */
                rc_pass_c[i] = 1;
                rc_carry[i] = 1;
                last_c[s->creg] = i;
                last_a[s->areg] = i; /* its alpha write is dead (checked) */
                carry_of = i;
                gl_a = i;
                any = 1;
                stat_rc_carried++;
                continue;
            } else {
                last_c[s->creg] = i;
                gl_c = i;
            }
        } else {
            last_c[0] = i;
            gl_c = i;
        }
        if (s->areg != GX_TEVPREV && s->areg <= GX_TEVREG0 + 2) {
            if (i < stages - 1 && !rc_live_after(i, (int)s->areg, 1, stages)) {
                rc_pass_a[i] = 1;
                any = 1;
            } else {
                last_a[s->areg] = i;
                gl_a = i;
                carry_of = -1;
            }
        } else {
            last_a[0] = i;
            gl_a = i;
            carry_of = -1;
        }
    }
    if (any) {
        stat_rc_chains++;
    }
    if (bad) {
        stat_rc_unexpressible += (unsigned)bad;
        gx_warn("TEV regchain: a register read two or more stages after its write "
                "(or PREV across a live register write); GL has one carrier, so "
                "it is drawn as the register's constant (PLAN.md 45)");
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
    TEV_MIX(&gx_hilite_stage, sizeof(gx_hilite_stage)); /* M21 */
    TEV_MIX(&gx_hilite_mode, sizeof(gx_hilite_mode));   /* M22 */
    if (port_opt.regfix2dbg) {
        unsigned fr = gl13_frame_number();
        TEV_MIX(&fr, sizeof(fr));
    }
    TEV_MIX(&gx.num_ind, sizeof(gx.num_ind));
    TEV_MIX(gx.swap_tbl, sizeof(gx.swap_tbl));
    TEV_MIX(gx.kcolor, sizeof(gx.kcolor));
    TEV_MIX(gx.tev_reg, sizeof(gx.tev_reg));
    for (i = 0; i < stages; i++) {
        TEV_MIX(&gx.tev[i], sizeof(gx.tev[i]));
        TEV_MIX(&gx.ind_tile[i], sizeof(gx.ind_tile[i]));
        TEV_MIX(&gx.ind_warp[i], sizeof(gx.ind_warp[i])); /* M35 */
    }
    if (gx.num_ind) {
        /* M35: the warp's program is a function of the matrices and the
         * indirect stages too (gx_tfs.c) */
        TEV_MIX(gx.ind, sizeof(gx.ind));
        TEV_MIX(gx.ind_mtx, sizeof(gx.ind_mtx));
    }
#undef TEV_MIX
    return h;
}

static u32 tev_cache_sig;
static int tev_cache_live;
/* M21: the last applied config's signature, for gx_draw.c's mergeable-batch
 * count (--submitstats) */
u32 gx_tev_last_sig(void) { return tev_cache_sig; }
static int regfix_k;
static int regfix_shape;       /* 1 = the eyes' triple (M16), 2 = the mask's (M22) */
static unsigned stat_regfix, stat_regfix2;
/* The konst collision, counted the way a degradation should be: per draw
 * that carries one, whether or not the TEV cache re-emitted the config.
 * `gx_warn`'s own count only fires on a cache miss, which is why PLAN.md
 * 29.3 and 30.5 disagreed by a factor of fifty (PLAN.md 31.4). */
static unsigned long stat_konst_draws;    /* draws whose config has a collision   */
static unsigned long stat_konst_configs;  /* distinct configs (cache misses) with one */
static unsigned long stat_draws_applied;
static unsigned long tev_hits, tev_misses;
static unsigned stat_hilite_stages; /* M21: hilite stages emitted as a pass */

void gx_tev_cache_invalidate(void) { tev_cache_live = 0; }

/* M30 (PLAN.md 45): which TEV stage's texture and coordinate unit `u` reads
 * -- itself, except inside the three-texture shape, where the third texture
 * rides one unit early and the two units after it read none (-1).  Both
 * vertex paths bind by this, so the sampled coordinate follows the texture. */
static void regfix_decide(int stages);
/* M41 (PLAN.md 56): the shape once a draw (gx_tfs.c gx_tfs_memo's window) */
static int regfix_memo_on, regfix_memo_stages = -1;
void gx_tfs_memo(int on);
void gx_unit_memo(int on) {
    regfix_memo_on = on;
    regfix_memo_stages = -1;
    gx_tfs_memo(on);
}
int gx_tev_unit_stage(int u) {
    regfix_decide(gx.num_tev ? gx.num_tev : 1);
    if (regfix_shape == -1) {
        return u; /* M35: the fragment shader's draw, units as stages */
    }
    if (regfix_shape == 5 && regfix_k >= 0) {
        if (u == regfix_k + 2) {
            return u + 1;
        }
        if (u == regfix_k + 3 || u == regfix_k + 4) {
            return -1;
        }
    }
    return u;
}

/* The shape decision, on its own so that the vertex paths can ask which
 * stage a unit samples for (gx_tev_unit_stage) before the TEV is applied. */
int gx_tev_unit_source(int u, u8* coord, u8* map) {
    int stage;
    int t = gx_tfs_layout(u, coord, map);
    if (t >= 0) {
        return t; /* M35: the fragment shader's units, the indirect maps past the stages */
    }
    stage = u < (gx.num_tev ? gx.num_tev : 1) ? gx_tev_unit_stage(u) : -1;
    if (stage < 0 || stage >= GX_TEV_STAGES) {
        return 0;
    }
    *coord = gx.tev[stage].coord;
    *map = gx.tev[stage].map;
    return 1;
}

static void regfix_decide(int stages) {
    int rk = -1; /* the first stage of a matched register triple, or -1 */
    int j;
    if (regfix_memo_on) {
        if (regfix_memo_stages == stages) {
            return; /* decided for this draw with the same count */
        }
        regfix_memo_stages = stages;
    }
    regfix_shape = 0;
    {
        /* M35: a draw the fragment shader takes (or would take: the layout,
         * not the compile) keeps unit = stage, so the vertex paths and the
         * fallback bind the same thing */
        u8 c, m;
        if (gx_tfs_layout(0, &c, &m) >= 0) {
            regfix_shape = -1;
            regfix_k = -1;
            return;
        }
    }
    for (j = 0; j + 2 < stages; j++) {
        if (regfix5_match(j, stages) >= 0) {
            rk = j;
            regfix_shape = 5;
            break;
        }
        if (regfix_match(j, stages) >= 0) {
            rk = j;
            regfix_shape = 1;
            break;
        }
        if (regfix2_match(j, stages) >= 0) {
            rk = j;
            regfix_shape = 2;
            break;
        }
        if (regfix4_match(j, stages) >= 0) {
            rk = j;
            regfix_shape = 4;
            break;
        }
    }
    if (rk < 0) {
        for (j = 0; j + 1 < stages; j++) {
            if (regfix3_match(j, stages) >= 0) {
                rk = j;
                regfix_shape = 3;
                break;
            }
        }
    }
    regfix_k = rk;
}

#ifndef GX_RTI
/* The unit's texture bind, out of gx_tev_apply's loop (M43) */
static void tev_unit_bind(int i, const GXTevStage* s, GXTexObjPort* bound) {
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
                            (regfix_shape == 2 && regfix_k >= 0 && i == regfix_k + 1)
                                ? SWAP_RED_TO_ALPHA /* M22: the mask's value in alpha */
                                : SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]));
    }
}

/* M43 (PLAN.md 58.2, --rtgx): gx_tev_apply's texture binds alone, on the game
 * thread -- the texture cache reads the game's memory (the content hash, the
 * decode, the upload) -- while the render thread's instance of this file
 * emits the rest of the unit loop from the replica.  The same units, the same
 * objects, the same swaps as the loop; the binds reach the render thread's
 * shadow through the stream (gl13_state.c GLC_FWD). */
void gx_tev_bind_textures(void) {
    int stages = gx.num_tev ? gx.num_tev : 1;
    int i;
    if (stages > gl13_max_tex_units) {
        stages = gl13_max_tex_units;
    }
    regfix_decide(stages);
    for (i = 0; i < gl13_max_tex_units && i < stages; i++) {
        const GXTevStage* s = &gx.tev[i];
        GXTexObjPort* bound = gx_bound_tex(s->map);
        int have_tex = bound != NULL && s->coord < GX_TEXCOORDS;
        if (!gl13_live()) {
            break;
        }
        if (regfix_shape == 5 && regfix_k >= 0) {
            if (i == regfix_k + 2) {
                s = &gx.tev[i + 1];
                bound = gx_bound_tex(s->map);
                have_tex = bound != NULL && s->coord < GX_TEXCOORDS;
            } else if (i == regfix_k + 3 || i == regfix_k + 4) {
                bound = NULL;
                have_tex = 0;
            }
        }
        if (have_tex) {
            tev_unit_bind(i, s, bound);
        }
    }
}
#endif

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
        for (i = 0; i < stages && i < 16; i++) {
            const GXTevStage* s = &gx.tev[i];
            GXTexObjPort* t = gx_bound_tex(s->map);
            if (t != NULL && s->coord < GX_TEXCOORDS) {
                have_tex_bits |= 1u << i;
                /* M31: the carry fold reads the texture's format (I4/I8), so
                 * the plan is a function of it too */
                if (t->format == GX_TF_I4 || t->format == GX_TF_I8) {
                    have_tex_bits |= 1u << (16 + i);
                }
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
    if (emit) {
        cfg_konst_collisions = 0;
    }
    stat_draws_applied++;
    /* M35 (PLAN.md 50.13): a draw with an indirect warp goes to the fragment
     * shader when the card has one; the shader replaces the whole unit
     * chain, so nothing below runs for it */
    if (gx_tfs_apply(stages, emit)) {
        return;
    }
    regfix_decide(stages);
    {
        int rk = regfix_k;
        int j;
        if (emit) {
            regchain_plan(stages, rk, regfix_shape == 3 ? 2 : regfix_shape == 5 ? 5 : rk >= 0 ? 3 : 0); /* M30 */
            for (j = 0; j < stages; j++) {
                const GXTevStage* s = &gx.tev[j];
                if ((s->creg != GX_TEVPREV || s->areg != GX_TEVPREV) &&
                    !(rk >= 0 && j == (regfix_shape == 3 ? rk : rk + 1)) &&
                    port_opt.noregchain) {
                    gx_warn("TEV: a stage writes a TEV register other than PREV; GL "
                            "has only PREV, so it is treated as PREV");
                    reg_write_warned++;
                }
            }
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
            if (regfix_shape == 5 && regfix_k >= 0) {
                /* M30: the third texture rides one unit early, and the two
                 * units after it read no texture */
                if (i == regfix_k + 2) {
                    s = &gx.tev[i + 1];
                    bound = gx_bound_tex(s->map);
                    have_tex = bound != NULL && s->coord < GX_TEXCOORDS;
                } else if (i == regfix_k + 3 || i == regfix_k + 4) {
                    bound = NULL;
                    have_tex = 0;
                }
            }
            if (have_tex) {
                glc_unit_enable_tex2d(i, 1);
#ifndef GX_RTI
                /* M43: under --rtgx the game thread binds (gx_tev_bind_textures)
                 * and the render thread's instance emits the rest */
                tev_unit_bind(i, s, bound);
#endif
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
                    if (bound == NULL && s->map != GX_TEXMAP_NULL &&
                        !(regfix_shape == 5 && regfix_k >= 0 && (i == regfix_k + 3 || i == regfix_k + 4))) {
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
            if (emit && regfix_k >= 0 && regfix_shape == 3 && i >= regfix_k && i <= regfix_k + 1) {
                regfix3_emit(i - regfix_k, i, regfix_k);
                stat_regfix2++;
            } else if (emit && regfix_k >= 0 && regfix_shape == 5 && i >= regfix_k &&
                       i <= regfix_k + 4) {
                regfix5_emit(i - regfix_k, i, regfix_k);
                stat_regfix5++;
            } else if (emit && regfix_k >= 0 && regfix_shape != 3 && i >= regfix_k &&
                       i <= regfix_k + 2 && !(regfix_shape == 2 && i == regfix_k)) {
                /* M23 (PLAN.md 38): shape 3 is a *pair*; without the guard the
                 * stage after it (k+2) was emitted as the M16 triple's third
                 * unit -- PREV * RAS, alpha PREV -- and hsfdraw's invAlpha
                 * stage (colour pass, alpha APREV * A0) behind the mode
                 * select's file boxes lost its 0.3: the frames drew at 170
                 * where the console has 99. */
                if (regfix_shape == 2) {
                    regfix2_emit(i - regfix_k, i, regfix_k);
                    stat_regfix2++;
                } else if (regfix_shape == 4) {
                    regfix4_emit(i - regfix_k, i, regfix_k);
                    stat_regfix4++;
                } else {
                    regfix_emit(i - regfix_k, i, regfix_k);
                    stat_regfix++;
                }
            } else if (emit && i == gx_hilite_stage && gx_hilite_mode == 1) {
                /* M21: the hilite screen, lerp(CPREV, ONE, RASC[COLOR1A1]).
                 * The vertex side has already folded (1 - spec) into the
                 * primary colour and GL_COLOR_SUM adds spec after the
                 * units (gx_internal.h), so here the colour passes through
                 * and only the stage's alpha (APREV * A0) is emitted. */
                glc_texenvi(i, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                glc_texenvi(i, GL_COMBINE_RGB, GL_REPLACE);
                glc_texenvi(i, GL_SOURCE0_RGB, GL_PREVIOUS);
                glc_texenvi(i, GL_OPERAND0_RGB, GL_SRC_COLOR);
                glc_texenvf(i, GL_RGB_SCALE, 1.0f);
                emit_channel(i, 0, alpha_arg(s, s->ain[0], 0), alpha_arg(s, s->ain[1], 0),
                             alpha_arg(s, s->ain[2], 0), alpha_arg(s, s->ain[3], 0), s->aop,
                             s->abias, s->ascale, konst, &konst_set);
                glc_texenv_color(i, konst);
                stat_hilite_stages++;
            } else if (emit) {
                const u8* rc = rc_ren_c[i];
                const u8* ra = rc_ren_a[i];
                glc_texenvi(i, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                if (rc_pass_c[i]) {
                    /* M30: a register write nothing reads; PREV passes */
                    glc_texenvi(i, GL_COMBINE_RGB, GL_REPLACE);
                    glc_texenvi(i, GL_SOURCE0_RGB, GL_PREVIOUS);
                    glc_texenvi(i, GL_OPERAND0_RGB, GL_SRC_COLOR);
                    glc_texenvf(i, GL_RGB_SCALE, 1.0f);
                    stat_rc_passed++;
                } else {
                    emit_channel(i, 1, color_arg(s, s->cin[0], rc[0]), color_arg(s, s->cin[1], rc[1]),
                                 color_arg(s, s->cin[2], rc[2]), color_arg(s, s->cin[3], rc[3]), s->cop,
                                 s->cbias, s->cscale, konst, &konst_set);
                }
                if (rc_carry[i]) {
                    /* M31: the stage's colour product T * X in the alpha:
                     * TEXTURE.a (the grey texture's intensity) times X's
                     * alpha form -- the primary alpha for RASA, the
                     * constant's alpha for KONST / A0-2 */
                    u8 x = s->cin[1] == GX_CC_TEXC ? s->cin[2] : s->cin[1];
                    Arg t = alpha_arg(s, GX_CA_TEXA, 0);
                    Arg k;
                    Arg z;
                    memset(&z, 0, sizeof(z));
                    z.is_zero = 1;
                    z.src = GL_CONSTANT;
                    z.operand = GL_SRC_ALPHA;
                    if (x == GX_CC_RASA) {
                        k = alpha_arg(s, GX_CA_RASA, 0);
                    } else if (x == GX_CC_KONST) {
                        k = color_arg(s, GX_CC_KONST, 0); /* grey: checked */
                        k.operand = GL_SRC_ALPHA;
                        k.konst[3] = k.konst[0];
                    } else {
                        k = alpha_arg(s, (u8)(GX_CA_A0 + (x - GX_CC_A0) / 2), 0);
                    }
                    emit_channel(i, 0, z, t, k, z, GX_TEV_ADD, GX_TB_ZERO, s->cscale, konst,
                                 &konst_set);
                } else if (rc_pass_a[i]) {
                    glc_texenvi(i, GL_COMBINE_ALPHA, GL_REPLACE);
                    glc_texenvi(i, GL_SOURCE0_ALPHA, GL_PREVIOUS);
                    glc_texenvi(i, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
                    glc_texenvf(i, GL_ALPHA_SCALE, 1.0f);
                    stat_rc_passed++;
                } else {
                    emit_channel(i, 0, alpha_arg(s, s->ain[0], ra[0]), alpha_arg(s, s->ain[1], ra[1]),
                                 alpha_arg(s, s->ain[2], ra[2]), alpha_arg(s, s->ain[3], ra[3]), s->aop,
                                 s->abias, s->ascale, konst, &konst_set);
                }
                if (rc[0] | rc[1] | rc[2] | rc[3] | ra[0] | ra[1] | ra[2] | ra[3]) {
                    stat_rc_renamed++;
                }
                glc_texenv_color(i, konst);
            }
        }
    }
    /* M38 (PLAN.md 53.10): --foldcap N, the m448 bisect.  Every unit of the
     * M30 three-texture fold from the Nth on (the fold's five and the stage
     * after it) is overwritten with a pass, so the picture is the chain cut
     * after N units; 9 cycles N = 1..6 by frame, and six dumped frames in a
     * row are the whole bisect on one card. */
    if ((port_opt.foldcap == 21 || port_opt.foldcap == 22) && regfix_shape == 5 &&
        regfix_k >= 0 && gl13_live()) {
        /* 21: unit B's alpha is its crossbar read of unit A's texture alone
         * (GL_TEXTURE0 + k); 22: its constant alone.  The fold makes the
         * felt's alpha there -- MODULATE(T_A.a, konst.a) -- and every unit
         * after passes it on (PLAN.md 53.10). */
        int u = regfix_k + 1;
        glc_texenvi(u, GL_COMBINE_ALPHA, GL_REPLACE);
        glc_texenvi(u, GL_SOURCE0_ALPHA,
                    port_opt.foldcap == 21 ? (int)(GL_TEXTURE0 + regfix_k) : GL_CONSTANT);
        glc_texenvi(u, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        tev_cache_live = 0;
    } else if (port_opt.foldcap && regfix_shape == 5 && regfix_k >= 0 && gl13_live()) {
        /* 10 + N: the same, and the chain's last unit's alpha forced to 1 --
         * the draw alpha-tests at >= 1, so this shows the colour the chain
         * makes whatever its alpha is */
        int mode = port_opt.foldcap % 10, force_a = port_opt.foldcap >= 10;
        int cap = mode == 9 ? 1 + (int)(gl13_frame_number() % 6u) : mode;
        int u;
        for (u = regfix_k + cap; u < stages && u < gl13_max_tex_units; u++) {
            glc_texenvi(u, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glc_texenvi(u, GL_COMBINE_RGB, GL_REPLACE);
            glc_texenvi(u, GL_SOURCE0_RGB, GL_PREVIOUS);
            glc_texenvi(u, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glc_texenvi(u, GL_COMBINE_ALPHA, GL_REPLACE);
            glc_texenvi(u, GL_SOURCE0_ALPHA, GL_PREVIOUS);
            glc_texenvi(u, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
            glc_texenvf(u, GL_RGB_SCALE, 1.0f);
            glc_texenvf(u, GL_ALPHA_SCALE, 1.0f);
        }
        if (force_a) {
            static const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            u = (stages < gl13_max_tex_units ? stages : gl13_max_tex_units) - 1;
            glc_texenvi(u, GL_COMBINE_ALPHA, GL_REPLACE);
            glc_texenvi(u, GL_SOURCE0_ALPHA, GL_CONSTANT);
            glc_texenvi(u, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
            glc_texenv_color(u, (float*)one);
        }
        tev_cache_live = 0; /* the next draw emits again: the cap moves by frame */
    }

    if (emit && cfg_konst_collisions) {
        stat_konst_configs++;
    }
    if (cfg_konst_collisions) {
        stat_konst_draws++;
    }
}

void gx_tev_report(void) {
    if (stat_draws_applied) {
        port_log("port> tev: konst collision (two constants in one stage): %lu of %lu "
                 "draws carry one, in %lu distinct configs (PLAN.md 31.4)\n",
                 stat_konst_draws, stat_draws_applied, stat_konst_configs);
    }
    if (stat_hilite_stages) {
        port_log("port> tev: %u hilite stage emissions folded into the specular colour "
                 "sum (PLAN.md 36)\n", stat_hilite_stages);
    }
    if (stat_regfix2) {
        port_log("port> tev: %u unit emissions of the M22 register shapes (the mask/reflect "
                 "triple, the lerp-by-konst pair; %u tinted pairs had the tint dropped, "
                 "%u drawn tinted with K_c = 1, M23) "
                 "(PLAN.md 37)\n", stat_regfix2, stat_regfix3_tinted, stat_regfix3_tint_drawn);
    }
    if (stat_regfix5) {
        port_log("port> tev: %u unit emissions of the M30 three-texture shape (the M16 triple "
                 "and a second lerp-by-alpha pair; PLAN.md 45)\n", stat_regfix5);
        port_log("port> tev: %u of them without the crossbar (M38, PLAN.md 53.10; --foldxbar "
                 "for the old fold)\n",
                 stat_regfix_no_xbar);
    }
    if (stat_rc_carried) {
        port_log("port> tev: %u configs carried a grey-texture product in the alpha (m417's "
                 "pool; PLAN.md 46)\n", stat_rc_carried);
    }
    if (stat_regfix4) {
        port_log("port> tev: %u unit emissions of the M30 lamp shape (T*RAS -> REG, T.a*K -> REG, "
                 "C*RAS + C; PLAN.md 45)\n", stat_regfix4);
    }
    if (stat_rc_chains || stat_rc_unexpressible) {
        port_log("port> tev: register chains (M30, PLAN.md 45): %u configs with a chain, "
                 "%u stage emissions read the previous unit for a register, %u dead "
                 "register writes passed PREV through, %u reads unexpressible (drawn as the "
                 "constant)\n",
                 stat_rc_chains, stat_rc_renamed, stat_rc_passed, stat_rc_unexpressible);
    }
    if (stat_regfix || reg_write_warned) {
        port_log("port> tev: %u unit emissions through the register rewrite "
                 "(PLAN.md 31.3), %u stage emissions still folding a register "
                 "write to PREV\n",
                 stat_regfix, reg_write_warned);
    }
    if (port_opt.tevstats) {
        unsigned long tot = tev_hits + tev_misses;
        port_log("port> tev cache: %lu applies, %lu skipped (%.1f%%), %lu emitted\n",
                 tot, tev_hits, tot ? 100.0 * (double)tev_hits / (double)tot : 0.0,
                 tev_misses);
    }
}
