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
                    gx_warn("TEV: a stage needs two different constants; the first wins");
                    cfg_konst_collisions++;
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
        gx_warn("TEV: GX_CS_DIVIDE_2 has no GL equivalent; drawn at scale 1");
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
    if (emit) {
        cfg_konst_collisions = 0;
    }
    stat_draws_applied++;
    {
        int rk = -1; /* the first stage of a matched register triple, or -1 */
        int j;
        regfix_shape = 0;
        for (j = 0; j + 2 < stages; j++) {
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
        if (emit) {
            for (j = 0; j < stages; j++) {
                const GXTevStage* s = &gx.tev[j];
                if ((s->creg != GX_TEVPREV || s->areg != GX_TEVPREV) &&
                    !(rk >= 0 && j == (regfix_shape == 3 ? rk : rk + 1))) {
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
                                        (regfix_shape == 2 && regfix_k >= 0 && i == regfix_k + 1)
                                            ? SWAP_RED_TO_ALPHA /* M22: the mask's value in alpha */
                                            : SWAP_PACK(gx.swap_tbl[s->tex_swap & 3]));
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
            if (emit && regfix_k >= 0 && regfix_shape == 3 && i >= regfix_k && i <= regfix_k + 1) {
                regfix3_emit(i - regfix_k, i, regfix_k);
                stat_regfix2++;
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
                emit_channel(i, 0, alpha_arg(s, s->ain[0]), alpha_arg(s, s->ain[1]),
                             alpha_arg(s, s->ain[2]), alpha_arg(s, s->ain[3]), s->aop,
                             s->abias, s->ascale, konst, &konst_set);
                glc_texenv_color(i, konst);
                stat_hilite_stages++;
            } else if (emit) {
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
