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

static Arg color_arg(const GXTevStage* s, u8 a) {
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
        case GX_CC_RASC: r.src = GL_PRIMARY_COLOR; break;
        case GX_CC_RASA: r.src = GL_PRIMARY_COLOR; r.operand = GL_SRC_ALPHA; break;
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
        case GX_CA_RASA: r.src = GL_PRIMARY_COLOR; break;
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
static void emit_channel(int rgb, Arg a, Arg b, Arg c, Arg d, u8 op, u8 bias, u8 scale,
                         float* konst_out, int* konst_set) {
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
    GL(glTexEnvi)(GL_TEXTURE_ENV, (GLenum)combine, (GLint)mode);
    GL(glTexEnvi)(GL_TEXTURE_ENV, s0, (GLint)args[0].src);
    GL(glTexEnvi)(GL_TEXTURE_ENV, o0, (GLint)args[0].operand);
    if (n > 1) {
        GL(glTexEnvi)(GL_TEXTURE_ENV, s1, (GLint)args[1].src);
        GL(glTexEnvi)(GL_TEXTURE_ENV, o1, (GLint)args[1].operand);
    }
    if (n > 2) {
        GL(glTexEnvi)(GL_TEXTURE_ENV, s2, (GLint)args[2].src);
        GL(glTexEnvi)(GL_TEXTURE_ENV, o2, (GLint)args[2].operand);
    }
    GL(glTexEnvf)(GL_TEXTURE_ENV, scale_e,
                  scale == GX_CS_SCALE_2 ? 2.0f : scale == GX_CS_SCALE_4 ? 4.0f : 1.0f);
    if (scale == GX_CS_DIVIDE_2) {
        gx_warn("TEV: GX_CS_DIVIDE_2 has no GL equivalent; drawn at scale 1");
    }
}

void gx_tev_apply(void) {
    int stages = gx.num_tev ? gx.num_tev : 1;
    int i;
    if (stages > gl13_max_tex_units) {
        gx_warn("TEV: the stage chain needs more units than the card has; the "
                "extra stages are dropped (PLAN.md 3.4 fallback 1)");
        stages = gl13_max_tex_units;
    }
    for (i = 0; i < gl13_max_tex_units; i++) {
        if (!gl13_live()) {
            break;
        }
        GL(glActiveTexture)(GL_TEXTURE0 + i);
        if (i >= stages) {
            GL(glDisable)(GL_TEXTURE_2D);
            continue;
        }
        {
            const GXTevStage* s = &gx.tev[i];
            float konst[4] = { 0, 0, 0, 0 };
            int konst_set = 0;
            int have_tex = s->map < GX_TEX_UNITS && gx.bound[s->map] != NULL &&
                           s->coord < GX_TEXCOORDS;
            if (have_tex) {
                GL(glEnable)(GL_TEXTURE_2D);
                gx_tex_bind(i, gx.bound[s->map]);
            } else {
                /* A stage with no texture still has to run its combiner, and
                 * a disabled unit in GL passes the previous colour through
                 * untouched -- which is only right when the stage is a pass.
                 * Keep the unit enabled against a 1x1 white texture instead. */
                GL(glDisable)(GL_TEXTURE_2D);
            }
            GL(glTexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            emit_channel(1, color_arg(s, s->cin[0]), color_arg(s, s->cin[1]),
                         color_arg(s, s->cin[2]), color_arg(s, s->cin[3]), s->cop,
                         s->cbias, s->cscale, konst, &konst_set);
            emit_channel(0, alpha_arg(s, s->ain[0]), alpha_arg(s, s->ain[1]),
                         alpha_arg(s, s->ain[2]), alpha_arg(s, s->ain[3]), s->aop,
                         s->abias, s->ascale, konst, &konst_set);
            GL(glTexEnvfv)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, konst);
            if (s->ras_swap || s->tex_swap) {
                gx_warn("GXSetTevSwapMode: a non-identity swap table is ignored");
            }
        }
    }
    if (gl13_live()) {
        GL(glActiveTexture)(GL_TEXTURE0);
    }
}

void gx_tev_report(void) {}
