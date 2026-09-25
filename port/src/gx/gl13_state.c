/* The GL state shadow and the translation of GX's raster state and
 * projection (M43, PLAN.md 58.2: moved out of gl13.c unchanged).
 *
 * This file is compiled twice.  The first object is the game thread's, as
 * every GL path always was.  The second (build .../rti/, -DGX_RTI with
 * src/gx/gx_rti.h) is the render thread's own instance for --rtgx: its own
 * shadow, its own caches, reading the render thread's replica of GXState
 * (gx_rtgx.c) -- see gx_rtgx.c for why and for the rules. */
#include "gx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#include "gx_rt.h" /* every gl* below is the render thread's twin */
#endif

extern int gl13_on; /* gl13.c: the window and its context are up */
#define gl_on gl13_on
#define EFB_W 640
#define EFB_H 480
#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

#ifndef GL_COLOR_SUM
#define GL_COLOR_SUM 0x8458
#endif

/* ---- the shadow of the GL state ------------------------------------------
 *
 * M2b measured 230 microseconds of fixed cost per `glDrawArrays` at the title
 * screen -- 83 ms of a 16.7 ms budget, 10.4 fps, and *none* of it geometry:
 * 27,041 vertices across 360 draws is 75 vertices a draw, which a G4
 * transforms in single-digit microseconds.  The cost was that
 * `gl13_apply_transform`, `gl13_apply_raster_state` and `gx_tev_apply`
 * re-emitted the **entire** pipeline configuration before every single draw:
 * a projection matrix, twenty raster calls, and six texture units' worth of
 * `glTexEnv*` -- roughly 130 GL calls per draw, 47,000 a frame -- and on this
 * driver each one re-runs a slice of state validation.
 *
 * So the backend now remembers what GL already has and emits only the
 * difference.  Everything below is that memory.  Three rules keep it honest:
 *
 *  - **one door.**  Nothing in the port calls a state-setting GL function
 *    directly any more; `gx_tev.c` and `gx_tex.c` go through the `glc_*`
 *    entry points, so there is exactly one place where the shadow can drift
 *    from the driver.
 *  - **write through, always.**  A cache miss sets the shadow *and* calls GL;
 *    a hit does neither.  There is no lazy flush and no ordering to get wrong.
 *  - **invalidate on anything the shadow cannot see.**  `glc_invalidate()`
 *    forgets everything, and is called at context creation and after any
 *    operation that touches GL behind the cache's back.
 *
 * `--glstats` prints calls emitted against calls elided, which is how a
 * regression here shows up as a number rather than as a slow afternoon.
 */

#define GLC_UNITS 8

typedef struct GlcUnit {
    unsigned tex_name;
    signed char tex2d_on;
    signed char coord_array_on;
    int env_mode;
    int combine_rgb, combine_a;
    int src_rgb[3], op_rgb[3];
    int src_a[3], op_a[3];
    float scale_rgb, scale_a;
    float env_color[4];
    float su, sv, tv;      /* the NPOT fold, this unit's GL_TEXTURE matrix:
                            * (s*su, t*sv + tv); tv is 0 except for an EFB
                            * copy, which is flipped (M24b, PLAN.md 39b) */
    const void* coord_ptr;
    int coord_stride;
} GlcUnit;

typedef struct Glc {
    int valid;
    GlcUnit unit[GLC_UNITS];
    int active_tex, client_active_tex;

    int vp[4];
    double dr_near, dr_far;
    int sc[4];

    signed char cull_on;
    int cull_face, front_face;
    float line_width; /* M30 */
    signed char depth_on;
    int depth_func;
    signed char depth_mask;
    signed char color_mask[4];
    signed char blend_on;
    int blend_src, blend_dst, blend_eq;
    signed char alpha_on;
    int alpha_func;
    float alpha_ref;
    signed char fog_on;
    int fog_mode;
    float fog_color[4], fog_density, fog_start, fog_end;

    float proj[16];
    signed char proj_valid;
    signed char modelview_identity;

    signed char vertex_array_on, color_array_on, normal_array_on;
    const void* vertex_ptr;
    const void* color_ptr;
    const void* normal_ptr;
    int vertex_stride, color_stride, normal_stride;
    /* the fog-coordinate array carries the palette slot (M18, gx_vprog.c) */
    signed char fog_array_on;
    const void* fog_ptr;
    int fog_stride;
    signed char color_sum_on;  /* M21: GL_COLOR_SUM (GL 1.4 / EXT_secondary_color) */
} Glc;

static Glc glc;

/* The 1x1 white texture's GL name; see glc_white_texture below.  M43: one
 * texture for both instances of this file (gx_rti.h): it is a fact about the
 * context, created once by the game thread at the render thread's start. */
#ifndef GX_RTI
unsigned glc_white_name_shared;
#else
extern unsigned glc_white_name_shared;
#endif
#define glc_white_name glc_white_name_shared

/* M43 (PLAN.md 58.2, --rtgx): while the render thread owns the shadow
 * (rtgx_owner_rt, gx_rtgx.c), the game thread's calls into it -- the texture
 * binds and uploads, the invalidations after a clear, a copy or the present
 * -- are not made here: they go into the stream as calls to the render
 * thread's instance, in order (GLC_FWD).  The render thread's instance never
 * forwards. */
#ifndef GX_RTI
#define GLC_FWD(op, a, b, x, y, z)                                                            \
    do {                                                                                      \
        if (rtgx_owner_rt) {                                                                  \
            rtgx_fwd(op, a, b, x, y, z);                                                      \
            return;                                                                           \
        }                                                                                     \
    } while (0)
#define GLC_OWNED(who)                                                                        \
    do {                                                                                      \
        if (rtgx_owner_rt) {                                                                  \
            rtgx_unexpected(who);                                                             \
        }                                                                                     \
    } while (0)
#else
#define GLC_FWD(op, a, b, x, y, z) do { } while (0)
#define GLC_OWNED(who) do { } while (0)
#endif
static unsigned glc_emitted, glc_elided;

/* --gltrace F: every GL call the shadow lets through during frame F, with
 * its arguments, so two submit shapes can be diffed call by call (M16). */
unsigned gl13_frame_number(void);
int gl13_trace_armed(void) {
    unsigned f;
    if (!port_opt.gltrace) {
        return 0; /* M43: before the call -- gl13_frame_number is in gl13.c now */
    }
    f = gl13_frame_number() + 1;
    return port_opt.gltrace && f <= (unsigned)port_opt.gltrace &&
           f + 40 > (unsigned)port_opt.gltrace; /* the forty frames up to F */
}
#define TR(...)                                                                          \
    do {                                                                                 \
        if (gl13_trace_armed()) {                                                        \
            port_log("gltrace> " __VA_ARGS__);                                           \
        }                                                                                \
    } while (0)

#define HIT(cond)                                                                        \
    do {                                                                                 \
        if (cond) {                                                                      \
            glc_elided++;                                                                \
            return;                                                                      \
        }                                                                                \
        glc_emitted++;                                                                   \
    } while (0)

void glc_invalidate(void) {
    GLC_FWD(RTGX_F_INVALIDATE, 0, 0, 0, 0, 0);
    /* The vertex program's binding, enable and parameter block are GL state
     * this shadow does not hold, and they are forgotten for the same reason.
     * So is the TEV state cache (PLAN.md 28.5): it skips the `glTexEnv` calls
     * on the strength of the shadow already holding the right values, and a
     * shadow that has forgotten them is a cache that must forget too. */
    gx_vprog_invalidate();
    gx_tev_cache_invalidate();
    gx_tfs_invalidate(); /* M35: the fragment program's bind and enable likewise */
    /* The white texture's *name* survives: it is a texture object, not
     * shadowed state, and the context is the same one.  M15 zeroed it here,
     * so every EFB copy-with-clear (which invalidates) re-created it on the
     * next textureless stage -- one leaked texture a frame, and each creation
     * re-bound whichever unit was active (PLAN.md 31.3).  gl13_shutdown
     * forgets it with the context. */
    memset(&glc, 0, sizeof(glc));
    /* -1 is "unknown": no GL enum or boolean is -1, so the first write of
     * every field is guaranteed to miss. */
    {
        int i, j;
        glc.active_tex = -1;
        glc.client_active_tex = -1;
        glc.cull_on = glc.depth_on = glc.blend_on = glc.alpha_on = glc.fog_on = -1;
        glc.depth_mask = -1;
        glc.vertex_array_on = glc.color_array_on = glc.normal_array_on = -1;
        glc.fog_array_on = -1;
        glc.color_sum_on = -1;
        glc.fog_ptr = (const void*)-1;
        glc.fog_stride = -1;
        glc.line_width = -1.0f; /* M30 */
        glc.proj_valid = 0;
        glc.modelview_identity = 0;
        for (i = 0; i < 4; i++) {
            glc.color_mask[i] = -1;
            glc.vp[i] = -1;
            glc.sc[i] = -1;
        }
        glc.dr_near = glc.dr_far = -1.0;
        for (i = 0; i < GLC_UNITS; i++) {
            GlcUnit* u = &glc.unit[i];
            u->tex_name = 0xFFFFFFFFu;
            u->tex2d_on = -1;
            u->coord_array_on = -1;
            u->env_mode = u->combine_rgb = u->combine_a = -1;
            u->scale_rgb = u->scale_a = -1.0f;
            u->su = u->sv = -1.0f;
            u->tv = 0.0f;
            u->coord_ptr = (const void*)-1;
            u->coord_stride = -1;
            for (j = 0; j < 3; j++) {
                u->src_rgb[j] = u->op_rgb[j] = -1;
                u->src_a[j] = u->op_a[j] = -1;
            }
            for (j = 0; j < 4; j++) {
                u->env_color[j] = -1.0f;
            }
        }
        glc.vertex_ptr = glc.color_ptr = glc.normal_ptr = (const void*)-1;
        glc.vertex_stride = glc.color_stride = glc.normal_stride = -1;
    }
    glc.valid = 1;
}

void glc_forget_white(void) { glc_white_name = 0; } /* M43: gl13_shutdown's */

void glc_stats(unsigned* emitted, unsigned* elided) {
    *emitted = glc_emitted;
    *elided = glc_elided;
}

void glc_active_texture(int unit) {
    GLC_FWD(RTGX_F_ACTIVE, unit, 0, 0, 0, 0);
    HIT(glc.active_tex == unit);
    glc.active_tex = unit;
    TR("glActiveTexture %d\n", unit);
    GL(glActiveTexture)((GLenum)(GL_TEXTURE0 + unit));
}

void glc_client_active_texture(int unit) {
    GLC_FWD(RTGX_F_CLIENT_ACTIVE, unit, 0, 0, 0, 0);
    HIT(glc.client_active_tex == unit);
    glc.client_active_tex = unit;
    TR("glClientActiveTexture %d\n", unit);
    GL(glClientActiveTexture)((GLenum)(GL_TEXTURE0 + unit));
}

void glc_bind_texture(int unit, unsigned name) {
    GLC_FWD(RTGX_F_BIND, unit, (int)name, 0, 0, 0);
    HIT(glc.unit[unit].tex_name == name);
    glc.unit[unit].tex_name = name;
    glc_active_texture(unit);
    TR("glBindTexture unit %d name %u\n", unit, name);
    GL(glBindTexture)(GL_TEXTURE_2D, (GLuint)name);
}

/* The binding a texture *upload* leaves behind: gx_tex.c has to bind the name
 * it is about to fill, and the shadow has to be told rather than guess. */
void glc_note_bind(int unit, unsigned name) {
    GLC_FWD(RTGX_F_NOTE_BIND, unit, (int)name, 0, 0, 0);
    glc.unit[unit].tex_name = name;
}

/* A 1x1 opaque white texture, for a TEV stage that has no texture of its own.
 *
 * GX lets a stage name GX_TEXMAP_NULL and still run its combiner -- the eye,
 * face and rim materials do exactly that for their last stage, which blends
 * the accumulated colour towards a TEV register.  GL 1.3 has no such thing: a
 * unit with GL_TEXTURE_2D disabled has its whole texture environment skipped,
 * so the stage does not run and the previous colour passes through untouched.
 * Binding this instead keeps the unit enabled, makes TEXC (1,1,1) and TEXA 1 --
 * the identity for every combiner that reads them -- and lets the stage run.
 * It is deliberately outside `glc`, which glc_invalidate memsets; the name is
 * dropped there as well, because a forgotten shadow may mean a new context. */

unsigned glc_white_texture(void) {
    GLC_OWNED("glc_white_texture");
    if (!glc_white_name) {
        static const unsigned char px[4] = { 255, 255, 255, 255 };
        GLuint n = 0;
        GL(glGenTextures)(1, &n);
        if (!n) {
            return 0;
        }
        int au = glc.active_tex >= 0 ? glc.active_tex : 0;
        unsigned had = glc.unit[au].tex_name;
        GL(glBindTexture)(GL_TEXTURE_2D, n);
        GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
        GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        /* The bind above went to whatever unit was active -- in the middle of
         * gx_tev_apply that is the unit *before* the textureless one, which
         * has just been bound to its own texture.  M15 recorded the white
         * name into that unit's shadow, which kept the shadow honest and the
         * picture wrong: the unit drew white until the next apply re-bound
         * it, and whether that was before or after the draw depended on the
         * submit shape (PLAN.md 31.3).  Put the unit's texture back. */
        if (had != 0xFFFFFFFFu) {
            GL(glBindTexture)(GL_TEXTURE_2D, (GLuint)had);
        } else {
            glc.unit[au].tex_name = (unsigned)n;
        }
        glc_white_name = (unsigned)n;
    }
    return glc_white_name;
}

void glc_unit_enable_tex2d(int unit, int on) {
    GLC_FWD(RTGX_F_ENABLE2D, unit, on, 0, 0, 0);
    HIT(glc.unit[unit].tex2d_on == (signed char)on);
    glc.unit[unit].tex2d_on = (signed char)on;
    glc_active_texture(unit);
    TR("glEnable/Disable TEXTURE_2D unit %d on %d\n", unit, on);
    if (on) {
        GL(glEnable)(GL_TEXTURE_2D);
    } else {
        GL(glDisable)(GL_TEXTURE_2D);
    }
}

/* One switch rather than a `glc_` entry point per texture-environment
 * parameter: gx_tev.c names the GL enum it wants and this decides where the
 * remembered copy lives.  A pname that is not in the table is passed straight
 * through uncached, which is the safe direction to be wrong in. */
static int* glc_env_slot_i(GlcUnit* u, unsigned pname) {
    switch (pname) {
        case GL_TEXTURE_ENV_MODE: return &u->env_mode;
        case GL_COMBINE_RGB: return &u->combine_rgb;
        case GL_COMBINE_ALPHA: return &u->combine_a;
        case GL_SOURCE0_RGB: return &u->src_rgb[0];
        case GL_SOURCE1_RGB: return &u->src_rgb[1];
        case GL_SOURCE2_RGB: return &u->src_rgb[2];
        case GL_OPERAND0_RGB: return &u->op_rgb[0];
        case GL_OPERAND1_RGB: return &u->op_rgb[1];
        case GL_OPERAND2_RGB: return &u->op_rgb[2];
        case GL_SOURCE0_ALPHA: return &u->src_a[0];
        case GL_SOURCE1_ALPHA: return &u->src_a[1];
        case GL_SOURCE2_ALPHA: return &u->src_a[2];
        case GL_OPERAND0_ALPHA: return &u->op_a[0];
        case GL_OPERAND1_ALPHA: return &u->op_a[1];
        case GL_OPERAND2_ALPHA: return &u->op_a[2];
        default: return NULL;
    }
}

void glc_texenvi(int unit, unsigned pname, int v) {
    GLC_OWNED("glc_texenvi");
    int* slot = glc_env_slot_i(&glc.unit[unit], pname);
    HIT(slot != NULL && *slot == v);
    if (slot) {
        *slot = v;
    }
    glc_active_texture(unit);
    TR("glTexEnvi unit %d pname %04x v %04x\n", unit, pname, v);
    GL(glTexEnvi)(GL_TEXTURE_ENV, (GLenum)pname, (GLint)v);
}

void glc_texenvf(int unit, unsigned pname, float v) {
    GLC_OWNED("glc_texenvf");
    GlcUnit* u = &glc.unit[unit];
    float* slot = pname == GL_RGB_SCALE ? &u->scale_rgb
                : pname == GL_ALPHA_SCALE ? &u->scale_a
                : NULL;
    HIT(slot != NULL && *slot == v);
    if (slot) {
        *slot = v;
    }
    glc_active_texture(unit);
    TR("glTexEnvf unit %d pname %04x v %f\n", unit, pname, v);
    GL(glTexEnvf)(GL_TEXTURE_ENV, (GLenum)pname, (GLfloat)v);
}

void glc_texenv_color(int unit, const float* c) {
    GLC_OWNED("glc_texenv_color");
    GlcUnit* u = &glc.unit[unit];
    HIT(memcmp(u->env_color, c, sizeof(float) * 4) == 0);
    memcpy(u->env_color, c, sizeof(float) * 4);
    glc_active_texture(unit);
    TR("glTexEnvColor unit %d %.3f %.3f %.3f %.3f\n", unit, c[0], c[1], c[2], c[3]);
    GL(glTexEnvfv)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, (const GLfloat*)c);
}

/* The only thing this backend spends GL_TEXTURE on is the NPOT fold, so the
 * whole matrix is three numbers and the cache can key on them: (s, t) ->
 * (s*su, t*sv + tv).  `tv` is zero for a decoded texture; an EFB copy
 * (M24b, PLAN.md 39b) is bound with sv negated and tv = +sv, because
 * glCopyTexSubImage2D fills the texture in GL's row order -- t = 0 is the
 * *bottom* of the copied region -- where a GX texture's t = 0 is its top. */
void glc_tex_matrix(int unit, float su, float sv) {
    GLC_FWD(RTGX_F_TEXMTX, unit, 0, su, sv, 0);
    glc_tex_matrix_fold(unit, su, sv, 0.0f);
}
void glc_tex_matrix_fold(int unit, float su, float sv, float tv) {
    GLfloat m[16];
    GLC_FWD(RTGX_F_TEXMTX_FOLD, unit, 0, su, sv, tv);
    GlcUnit* u = &glc.unit[unit];
    HIT(u->su == su && u->sv == sv && u->tv == tv);
    u->su = su;
    u->sv = sv;
    u->tv = tv;
    glc_active_texture(unit);
    memset(m, 0, sizeof(m));
    m[0] = su;
    m[5] = sv;
    m[13] = tv;
    m[10] = 1.0f;
    m[15] = 1.0f;
    TR("glTexMatrix unit %d su %f sv %f tv %f\n", unit, su, sv, tv);
    GL(glMatrixMode)(GL_TEXTURE);
    GL(glLoadMatrixf)(m);
    GL(glMatrixMode)(GL_MODELVIEW);
}

static void glc_enable(GLenum cap, int on, signed char* shadow) {
    HIT(*shadow == (signed char)on);
    *shadow = (signed char)on;
    TR("glEnable/Disable cap %04x on %d\n", (unsigned)cap, on);
    if (on) {
        GL(glEnable)(cap);
    } else {
        GL(glDisable)(cap);
    }
}

/* M33 (PLAN.md 48.3): the depth pre-pass for a draw whose alpha test would
 * kill fragments that the hardware still writes Z for (m435's pillar
 * sphere: the flare's quad holds the clouds off).  Between begin and end
 * the caller issues the draw once with the colour writes off and the alpha
 * test off; end puts the cache's idea of both back so the real draw's
 * apply re-emits them. */
int gl13_zprepass_wanted(void) {
    GLC_OWNED("gl13_zprepass_wanted");
    int u, ref, can_kill = 0;
    if (!port_opt.zprepass || !gx.z_enable || !gx.z_update || glc.alpha_on != 1) {
        return 0;
    }
    if (port_opt.zprepass == 1 && !gx.z_comploc) {
        return 0; /* Dolphin's rule: only with GXSetZCompLoc(GX_TRUE) */
    }
    /* only when a bound texture's alpha can fail the test (a cut-out); the
     * game's `GEQUAL 1' idiom on an opaque texture kills nothing */
    ref = (int)(glc.alpha_ref * 255.0f + 0.5f);
    for (u = 0; u < gl13_max_tex_units && u < 8; u++) {
        int stage = u < gx.num_tev ? gx_tev_unit_stage(u) : -1;
        /* M34: and only when the stage's alpha chain reads the texture's
         * alpha at all -- the board's balloons bind their hilite map (an I8,
         * alpha_min 0) to a stage whose alpha is A0 * APREV, and doubled
         * every frame for nothing (34 draws at frame 7000, no pixel moved) */
        if (stage >= 0 && gx_bound_tex(gx.tev[stage].map) != NULL &&
            (gx.tev[stage].ain[0] == GX_CA_TEXA || gx.tev[stage].ain[1] == GX_CA_TEXA ||
             gx.tev[stage].ain[2] == GX_CA_TEXA || gx.tev[stage].ain[3] == GX_CA_TEXA)) {
            int amin = (int)gx_unit_alpha_min[u];
            if (glc.alpha_func == GL_GEQUAL ? amin < ref
                : glc.alpha_func == GL_GREATER ? amin <= ref
                                               : 1) {
                can_kill = 1;
            }
        }
    }
    return can_kill;
}
void gl13_zprepass_begin(void) {
    GLC_OWNED("gl13_zprepass_begin");
    GL(glColorMask)(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    GL(glDisable)(GL_ALPHA_TEST);
}
void gl13_zprepass_end(void) {
    GLC_OWNED("gl13_zprepass_end");
    GL(glColorMask)(glc.color_mask[0] ? GL_TRUE : GL_FALSE, glc.color_mask[1] ? GL_TRUE : GL_FALSE,
                    glc.color_mask[2] ? GL_TRUE : GL_FALSE, glc.color_mask[3] ? GL_TRUE : GL_FALSE);
    GL(glEnable)(GL_ALPHA_TEST);
}

/* M21: the colour sum (GL 1.4 core, GL_EXT_secondary_color on GL 1.3 -- the
 * token is the same, 0x8458, and g4-glinfo.log lists the extension).  Adds
 * the fragment's secondary colour after the texture units; the vertex
 * program writes it (gx_vprog.c) for the hilite fold and nothing else. */
#define GLC_COLOR_SUM 0x8458
void glc_color_sum(int on) {
    GLC_OWNED("glc_color_sum");
    glc_enable(GLC_COLOR_SUM, on, &glc.color_sum_on);
}

void glc_projection(const float* m) {
    GLC_OWNED("glc_projection");
    HIT(glc.proj_valid && memcmp(glc.proj, m, sizeof(float) * 16) == 0);
    memcpy(glc.proj, m, sizeof(float) * 16);
    glc.proj_valid = 1;
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glLoadMatrixf)((const GLfloat*)m);
    GL(glMatrixMode)(GL_MODELVIEW);
}

void glc_modelview_identity(void) {
    GLC_OWNED("glc_modelview_identity");
    HIT(glc.modelview_identity);
    glc.modelview_identity = 1;
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glLoadIdentity)();
}

/* The vertex arrays never move: `verts` is a static buffer and every draw
 * reads it from index zero, so the pointers are set once for the life of the
 * process and only the per-unit enables change. */
void glc_vertex_array(const void* p, int stride) {
    GLC_OWNED("glc_vertex_array");
    /* The stride is part of the identity now: M9 packs the vertex to the
     * primitive, so the same base pointer can be handed over with a different
     * layout and eliding on the pointer alone would draw the old one. */
    HIT(glc.vertex_array_on == 1 && glc.vertex_ptr == p && glc.vertex_stride == stride);
    if (glc.vertex_array_on != 1) {
        glc.vertex_array_on = 1;
        GL(glEnableClientState)(GL_VERTEX_ARRAY);
    }
    glc.vertex_ptr = p;
    glc.vertex_stride = stride;
    GL(glVertexPointer)(3, GL_FLOAT, (GLsizei)stride, p);
}

void glc_color_array(const void* p, int stride) {
    GLC_OWNED("glc_color_array");
    HIT(glc.color_array_on == 1 && glc.color_ptr == p && glc.color_stride == stride);
    if (glc.color_array_on != 1) {
        glc.color_array_on = 1;
        GL(glEnableClientState)(GL_COLOR_ARRAY);
    }
    glc.color_ptr = p;
    glc.color_stride = stride;
    GL(glColorPointer)(4, GL_UNSIGNED_BYTE, (GLsizei)stride, p);
}

/* The normal array exists only for the vertex-program path: the CPU path does
 * its own lighting and never hands GL a normal (gx_draw.c's file header says
 * why), so before M11 there was nothing to shadow. */
void glc_normal_array(const void* p, int stride) {
    GLC_OWNED("glc_normal_array");
    HIT(glc.normal_array_on == (signed char)(p != NULL) && glc.normal_ptr == p &&
        glc.normal_stride == stride);
    if (glc.normal_array_on != (signed char)(p != NULL)) {
        glc.normal_array_on = (signed char)(p != NULL);
        if (p) {
            GL(glEnableClientState)(GL_NORMAL_ARRAY);
        } else {
            GL(glDisableClientState)(GL_NORMAL_ARRAY);
        }
    }
    glc.normal_ptr = p;
    glc.normal_stride = stride;
    if (p) {
        GL(glNormalPointer)(GL_FLOAT, (GLsizei)stride, p);
    }
}

/* The fog-coordinate array (GL_EXT_fog_coord, one float per vertex) is how a
 * vertex reaches `vertex.fogcoord` in an ARB vertex program, and M18 uses that
 * attribute for the vertex's matrix-palette slot (gx_vprog.c) -- nothing else
 * in the port ever hands GL a fog coordinate.  Resolved at run time like the
 * other extensions; a driver without it simply has no palette. */
#define GLC_FOG_COORDINATE_ARRAY_EXT 0x8457
typedef void (*fogptr_t)(GLenum, GLsizei, const void*);
static fogptr_t glc_FogCoordPointerEXT;
static int glc_fog_probed;

int glc_fogcoord_available(void) {
#ifndef PORT_NO_SDL
    if (!glc_fog_probed && gl_on) {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        glc_fog_probed = 1;
        if (ext && strstr(ext, "GL_EXT_fog_coord")) {
            glc_FogCoordPointerEXT = (fogptr_t)SDL_GL_GetProcAddress("glFogCoordPointerEXT");
        }
    }
#endif
    return glc_FogCoordPointerEXT != NULL;
}

/* M40 (PLAN.md 55): the array pointers' shadow forgotten -- the vertex
 * cache's buffer object was bound or unbound, and a pointer value means an
 * offset in one and an address in the other */
void glc_arrays_forget(void) {
    GLC_OWNED("glc_arrays_forget");
    int i;
    glc.vertex_ptr = glc.color_ptr = glc.normal_ptr = (const void*)-1;
    glc.fog_ptr = (const void*)-1;
    for (i = 0; i < GLC_UNITS; i++) {
        glc.unit[i].coord_ptr = (const void*)-1;
    }
}

void glc_fogcoord_array(const void* p, int stride) {
    GLC_OWNED("glc_fogcoord_array");
    if (!glc_fogcoord_available()) {
        return;
    }
    HIT(glc.fog_array_on == (signed char)(p != NULL) && glc.fog_ptr == p &&
        glc.fog_stride == stride);
    if (glc.fog_array_on != (signed char)(p != NULL)) {
        glc.fog_array_on = (signed char)(p != NULL);
        if (p) {
            GL(glEnableClientState)(GLC_FOG_COORDINATE_ARRAY_EXT);
        } else {
            GL(glDisableClientState)(GLC_FOG_COORDINATE_ARRAY_EXT);
        }
    }
    glc.fog_ptr = p;
    glc.fog_stride = stride;
    if (p) {
        rt_ext_fogcoord_pointer(GL_FLOAT, (GLsizei)stride, p);
    }
}

/* The NPOT fold this unit is carrying, so a vertex program can apply it: the
 * fixed-function GL_TEXTURE matrix is not consulted while a program is bound,
 * and the program has to reproduce it (gx_vprog.c). */
void glc_get_tex_scale(int unit, float* su, float* sv) {
    float tv;
    glc_get_tex_fold(unit, su, sv, &tv);
}
int gx_tfs_fold(int unit, float* su, float* sv, float* tv);
void glc_get_tex_fold(int unit, float* su, float* sv, float* tv) {
    if (gx_tfs_fold(unit, su, sv, tv)) {
        return; /* M35: the shader's draw has the GL matrix at the identity; the program keeps the fold */
    }
    /* The shadow starts at zero, and zero here is not "no fold" -- it is a
     * texture matrix that collapses every coordinate onto one texel.  The
     * fixed-function path never noticed, because GL only applies the matrix
     * once glc_tex_matrix has loaded it; a vertex program reads the number
     * and multiplies by it, so a unit that has not been bound yet has to
     * answer with the identity. */
    *su = glc.unit[unit].su != 0.0f ? glc.unit[unit].su : 1.0f;
    *sv = glc.unit[unit].sv != 0.0f ? glc.unit[unit].sv : 1.0f;
    *tv = glc.unit[unit].tv;
}

void glc_coord_array(int unit, const void* p, int stride) {
    GLC_OWNED("glc_coord_array");
    GlcUnit* u = &glc.unit[unit];
    HIT(u->coord_array_on == (signed char)(p != NULL) && u->coord_ptr == p &&
        u->coord_stride == stride);
    glc_client_active_texture(unit);
    if (p) {
        if (u->coord_array_on != 1) {
            GL(glEnableClientState)(GL_TEXTURE_COORD_ARRAY);
        }
        GL(glTexCoordPointer)(2, GL_FLOAT, (GLsizei)stride, p);
    } else if (u->coord_array_on != 0) {
        GL(glDisableClientState)(GL_TEXTURE_COORD_ARRAY);
    }
    u->coord_array_on = (signed char)(p != NULL);
    u->coord_ptr = p;
    u->coord_stride = stride;
}


/* ---- the raster state, straight out of GXState ---------------------------- */

static GLenum gl_compare(u8 c) {
    static const GLenum t[8] = { GL_NEVER,   GL_LESS,   GL_EQUAL,  GL_LEQUAL,
                                 GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return t[c & 7];
}

static GLenum gl_blend_src(u8 f) {
    switch (f) {
        case GX_BL_ZERO: return GL_ZERO;
        case GX_BL_ONE: return GL_ONE;
        case GX_BL_SRCCLR: return GL_DST_COLOR;  /* GX names the *other* one */
        case GX_BL_INVSRCCLR: return GL_ONE_MINUS_DST_COLOR;
        case GX_BL_SRCALPHA: return GL_SRC_ALPHA;
        case GX_BL_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case GX_BL_DSTALPHA: return GL_DST_ALPHA;
        default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

static GLenum gl_blend_dst(u8 f) {
    switch (f) {
        case GX_BL_ZERO: return GL_ZERO;
        case GX_BL_ONE: return GL_ONE;
        case GX_BL_SRCCLR: return GL_SRC_COLOR;
        case GX_BL_INVSRCCLR: return GL_ONE_MINUS_SRC_COLOR;
        case GX_BL_SRCALPHA: return GL_SRC_ALPHA;
        case GX_BL_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case GX_BL_DSTALPHA: return GL_DST_ALPHA;
        default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

void gl13_apply_raster_state(void) {
    GLC_OWNED("gl13_apply_raster_state");
    if (!gl_on) {
        return;
    }
    /* viewport and scissor: GX's y runs down from the top of a 640x480 EFB,
     * GL's runs up from the bottom. */
    {
        int vp[4];
        vp[0] = (int)gx.vp[0];
        vp[1] = (int)(EFB_H - (gx.vp[1] + gx.vp[3]));
        vp[2] = (int)gx.vp[2];
        vp[3] = (int)gx.vp[3];
        if (memcmp(glc.vp, vp, sizeof(vp)) != 0) {
            memcpy(glc.vp, vp, sizeof(vp));
            glc_emitted++;
            GL(glViewport)(vp[0], vp[1], (GLsizei)vp[2], (GLsizei)vp[3]);
        } else {
            glc_elided++;
        }
        if (glc.dr_near != (double)gx.vp[4] || glc.dr_far != (double)gx.vp[5]) {
            glc.dr_near = gx.vp[4];
            glc.dr_far = gx.vp[5];
            glc_emitted++;
            GL(glDepthRange)(gx.vp[4], gx.vp[5]);
        } else {
            glc_elided++;
        }
        vp[0] = (int)gx.scissor[0];
        vp[1] = (int)(EFB_H - (int)(gx.scissor[1] + gx.scissor[3]));
        vp[2] = (int)gx.scissor[2];
        vp[3] = (int)gx.scissor[3];
        if (memcmp(glc.sc, vp, sizeof(vp)) != 0) {
            memcpy(glc.sc, vp, sizeof(vp));
            glc_emitted++;
            GL(glScissor)(vp[0], vp[1], (GLsizei)vp[2], (GLsizei)vp[3]);
        } else {
            glc_elided++;
        }
    }

    /* M30 (PLAN.md 45, cause H): the line width.  GX counts in sixths of a
     * pixel; GL's minimum is one pixel and the picture is the EFB's size, so
     * the width lands unscaled.  --nolinewidth is the pre-M30 hairline. */
    {
        float w = port_opt.nolinewidth ? 1.0f : (float)(gx.line_width ? gx.line_width : 6) / 6.0f;
        if (w < 1.0f) {
            w = 1.0f;
        }
        if (glc.line_width != w) {
            glc.line_width = w;
            glc_emitted++;
            GL(glLineWidth)(w);
        } else {
            glc_elided++;
        }
    }

    /* GX's front face is the opposite of GL's default. */
    {
        int on = gx.cull != GX_CULL_NONE && !(gx_force_flags & 1);
        GLenum face = GL_FRONT;
        switch (gx.cull) {
            case GX_CULL_FRONT: face = GL_BACK; break;
            case GX_CULL_BACK: face = GL_FRONT; break;
            default: face = GL_FRONT_AND_BACK; break;
        }
        glc_enable(GL_CULL_FACE, on, &glc.cull_on);
        if (on) {
            if (glc.cull_face != (int)face) {
                glc.cull_face = (int)face;
                glc_emitted++;
                GL(glCullFace)(face);
            } else {
                glc_elided++;
            }
            if (glc.front_face != GL_CCW) {
                glc.front_face = GL_CCW;
                glc_emitted++;
                GL(glFrontFace)(GL_CCW);
            } else {
                glc_elided++;
            }
        }
    }

    glc_enable(GL_DEPTH_TEST, gx.z_enable && !(gx_force_flags & 2) ? 1 : 0, &glc.depth_on);
    if (gx.z_enable) {
        GLenum f = gl_compare(gx.z_func);
        if (glc.depth_func != (int)f) {
            glc.depth_func = (int)f;
            glc_emitted++;
            GL(glDepthFunc)(f);
        } else {
            glc_elided++;
        }
    }
    /* M33: --forceobj bit 32 = the z write forced on (m435's sphere read) */
    if (glc.depth_mask != (signed char)((gx.z_update || (gx_force_flags & 32)) ? 1 : 0)) {
        glc.depth_mask = (signed char)((gx.z_update || (gx_force_flags & 32)) ? 1 : 0);
        glc_emitted++;
        GL(glDepthMask)(glc.depth_mask ? GL_TRUE : GL_FALSE);
    } else {
        glc_elided++;
    }
    {
        signed char cm[4];
        cm[0] = cm[1] = cm[2] = (signed char)(gx.color_update || (gx_force_flags & 16) ? 1 : 0);
        cm[3] = (signed char)(gx.alpha_update || (gx_force_flags & 16) ? 1 : 0);
        if (memcmp(glc.color_mask, cm, 4) != 0) {
            memcpy(glc.color_mask, cm, 4);
            glc_emitted++;
            GL(glColorMask)(cm[0] ? GL_TRUE : GL_FALSE, cm[1] ? GL_TRUE : GL_FALSE,
                            cm[2] ? GL_TRUE : GL_FALSE, cm[3] ? GL_TRUE : GL_FALSE);
        } else {
            glc_elided++;
        }
    }

    {
        int on = (gx.blend_mode == GX_BM_BLEND || gx.blend_mode == GX_BM_SUBTRACT) &&
                 !(gx_force_flags & 8); /* M32: --forceobj bit 8 = the blend off */
        glc_enable(GL_BLEND, on, &glc.blend_on);
        if (on) {
            GLenum src, dst, eq;
            if (gx.blend_mode == GX_BM_SUBTRACT) {
                /* GX_BM_SUBTRACT is dst - src with both factors one. */
                src = GL_ONE;
                dst = GL_ONE;
                eq = GL_FUNC_REVERSE_SUBTRACT;
            } else {
                src = gl_blend_src(gx.blend_src);
                dst = gl_blend_dst(gx.blend_dst);
                eq = GL_FUNC_ADD;
            }
            if (gl13_have_blend_subtract && glc.blend_eq != (int)eq) {
                glc.blend_eq = (int)eq;
                glc_emitted++;
                GL(glBlendEquation)(eq);
            } else {
                glc_elided++;
            }
            if (glc.blend_src != (int)src || glc.blend_dst != (int)dst) {
                glc.blend_src = (int)src;
                glc.blend_dst = (int)dst;
                glc_emitted++;
                GL(glBlendFunc)(src, dst);
            } else {
                glc_elided++;
            }
        }
    }

    /* Alpha compare.  GX combines two comparisons with AND/OR/XOR/XNOR; GL
     * has one.  Most of the game's 59 sites reduce exactly -- the other test
     * is ALWAYS under AND or NEVER under OR -- and the ones that do not are
     * named once each by --gxwarn and drawn with the first test. */
    {
        u8 c0 = gx.alpha_comp0, c1 = gx.alpha_comp1;
        u8 use = c0, ref = gx.alpha_ref0;
        int pass_all = 0;
        if (gx.alpha_op == GX_AOP_AND) {
            if (c0 == GX_ALWAYS) {
                use = c1;
                ref = gx.alpha_ref1;
            } else if (c1 == GX_ALWAYS || (c1 == c0 && gx.alpha_ref1 == gx.alpha_ref0)) {
                /* exact: the second half is either vacuous or the same test */
            } else if (c0 == GX_NEVER || c1 == GX_NEVER) {
                use = GX_NEVER;
            } else {
                gx_warn("GXSetAlphaCompare: two live AND comparisons, the first is used");
            }
        } else if (gx.alpha_op == GX_AOP_OR) {
            if (c0 == GX_ALWAYS || c1 == GX_ALWAYS) {
                pass_all = 1;
            } else if (c0 == GX_NEVER) {
                use = c1;
                ref = gx.alpha_ref1;
            } else if (c1 == GX_NEVER || (c1 == c0 && gx.alpha_ref1 == gx.alpha_ref0)) {
                /* Exact, and this is the common one.  The game's own idiom is
                 * `GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_OR, GX_GEQUAL, 1)` --
                 * the same comparison written twice because GX has no way to
                 * say "just this one".  M2b counted 79,488 of these in 999
                 * frames, i.e. every draw, and reported each as a degradation;
                 * it never was one. */
            } else {
                gx_warn("GXSetAlphaCompare: two live OR comparisons, the first is used");
            }
        } else {
            gx_warn("GXSetAlphaCompare: XOR/XNOR is reduced to its first test");
        }
        if (pass_all || use == GX_ALWAYS || (gx_force_flags & 4)) {
            glc_enable(GL_ALPHA_TEST, 0, &glc.alpha_on);
        } else {
            GLenum f = gl_compare(use);
            float r = ref / 255.0f;
            glc_enable(GL_ALPHA_TEST, 1, &glc.alpha_on);
            if (glc.alpha_func != (int)f || glc.alpha_ref != r) {
                glc.alpha_func = (int)f;
                glc.alpha_ref = r;
                glc_emitted++;
                GL(glAlphaFunc)(f, r);
            } else {
                glc_elided++;
            }
        }
    }

    /* Fog.  Nine sites in the whole game, all of them linear or exponential
     * in eye z, which is what GL_FOG is.
     *
     * M30 (PLAN.md 45): the hardware's exponential fog is 1 - 2^(-8 f) (EXP)
     * or 1 - 2^(-8 f^2) (EXP2) of the *linear* factor f = (z_eye - start) /
     * (end - start) clamped to 0..1 (GXPixel.c's A, B, C; Dolphin's
     * PixelShaderGen).  GL's EXP is exp(-d * fc) of the fog coordinate with
     * no start, so the vertex program writes fc = max(0, |z_eye| -
     * state.fog.params.y) (gx_vprog.c) and d = 8 ln 2 / (end - start) makes
     * GL's factor 2^(-8 f) exactly (EXP2: exp(-(d fc)^2) = 2^(-8 f^2) with
     * d = sqrt(8 ln 2) / (end - start)).  Beyond `end` GX holds f at 1 (0.4%
     * unfogged) where GL keeps going: below a level.  Linear fog with that
     * offset coordinate is GL_LINEAR over (start/2, end - start/2): (E - (z
     * - S)) / (E - S) = (end - z) / (end - start).  The CPU vertex path has
     * no fog coordinate of its own and keeps the pre-M30 numbers
     * (`--oldfog` everywhere). */
    if (gx.fog_type == GX_FOG_NONE) {
        glc_enable(GL_FOG, 0, &glc.fog_on);
    } else {
        GLfloat c[4];
        int mode;
        float density = 0.0f;
        float fs = gx.fog_startz, fe = gx.fog_endz;
        int offs = !port_opt.oldfog && gx_vprog_available() && !port_opt.cpuxf;
        c[0] = gx.fog_color.r / 255.0f;
        c[1] = gx.fog_color.g / 255.0f;
        c[2] = gx.fog_color.b / 255.0f;
        c[3] = gx.fog_color.a / 255.0f;
        glc_enable(GL_FOG, 1, &glc.fog_on);
        if (memcmp(glc.fog_color, c, sizeof(c)) != 0) {
            memcpy(glc.fog_color, c, sizeof(c));
            glc_emitted++;
            GL(glFogfv)(GL_FOG_COLOR, c);
        } else {
            glc_elided++;
        }
        switch (gx.fog_type & 7) {
            case 4: mode = GL_EXP; break;
            case 5: mode = GL_EXP2; break;
            default: mode = GL_LINEAR; break;
        }
        if (glc.fog_mode != mode) {
            glc.fog_mode = mode;
            glc_emitted++;
            GL(glFogi)(GL_FOG_MODE, mode);
        } else {
            glc_elided++;
        }
        if (mode == GL_LINEAR) {
            if (offs) {
                fs = gx.fog_startz * 0.5f;
                fe = gx.fog_endz - gx.fog_startz * 0.5f;
            }
            if (glc.fog_start != fs || glc.fog_end != fe) {
                glc.fog_start = fs;
                glc.fog_end = fe;
                glc_emitted++;
                GL(glFogf)(GL_FOG_START, fs);
                GL(glFogf)(GL_FOG_END, fe);
            } else {
                glc_elided++;
            }
        } else {
            if (offs) {
                float range = gx.fog_endz - gx.fog_startz;
                if (range < 1.0e-6f) {
                    range = 1.0e-6f;
                }
                density = mode == GL_EXP2 ? 2.3548200f / range /* sqrt(8 ln 2) */
                                          : 5.5451774f / range; /* 8 ln 2 */
                if (glc.fog_start != fs) {
                    glc.fog_start = fs;
                    glc_emitted++;
                    GL(glFogf)(GL_FOG_START, fs); /* what the program subtracts */
                }
            } else {
                density = 1.0f / (gx.fog_endz - gx.fog_startz + 1.0f);
            }
            if (glc.fog_density != density) {
                glc.fog_density = density;
                glc_emitted++;
                GL(glFogf)(GL_FOG_DENSITY, density);
            } else {
                glc_elided++;
            }
        }
    }
}

/* ---- transform ------------------------------------------------------------ */
/* GX's projection maps eye z to [-1, 0] and GL's to [-1, 1], so every
 * projection gets one extra row operation: z' = 2z + w.  GX's 3x4 matrices
 * are row major and GL's are column major, so the load transposes.
 *
 * The sign of that `+ w` is the whole of M2b's missing 3D layer, and it is
 * worth writing the derivation down rather than the answer.  Under a
 * perspective projection **w_clip is `-z_eye`, not `+z_eye`** -- that is what
 * `M[3][2] = -1` two lines further down says.  So:
 *
 *     z_gx      = m22*z + m23           (GXSetProjection's stored elements)
 *     z_ndc_gx  = z_gx / w_clip         in [-1, 0]
 *     z_ndc_gl  = 2*z_ndc_gx + 1        in [-1, 1]
 *     z_clip_gl = w_clip * z_ndc_gl
 *               = 2*z_gx + w_clip
 *               = 2*(m22*z + m23) + (-z)
 *               = (2*m22 - 1)*z + 2*m23
 *
 * so `M[2][2] = 2*m22 - 1`.  The port had `2*m22 + 1`.  With the title
 * screen's projection (m22 = -3.05e-06, near 0.1, far 32768) that is
 * +0.99999389 where it should be -1.00000610 -- a coefficient of very nearly
 * the right magnitude and exactly the wrong sign, which puts **every**
 * perspective vertex at a z just past -1 in normalised device coordinates and
 * hands the lot to GL's near plane.  A vertex at z_eye = -949 came out at
 * z_ndc = -1.0002.
 *
 * That is why 27,041 vertices in 358 display lists submitted cleanly, drew
 * with no GL error, and put nothing on the screen; and it is why only the 3D
 * layer was missing, because the orthographic branch has w_clip = 1 and its
 * `+ 1` was right all along.  A clipping bug of two ten-thousandths.
 */

void gl13_apply_transform(void) {
    GLC_OWNED("gl13_apply_transform");
    float m[16];
    const f32* p;
    if (!gl_on) {
        return;
    }
    p = gx.proj;
    memset(m, 0, sizeof(m));
    if (gx.proj_type == GX_PERSPECTIVE) {
        m[0] = p[0];
        m[8] = p[1];
        m[5] = p[2];
        m[9] = p[3];
        m[10] = 2.0f * p[4] - 1.0f; /* [-1,0] -> [-1,1]; w_clip is -z_eye */
        m[14] = 2.0f * p[5];
        m[11] = -1.0f;
    } else {
        m[0] = p[0];
        m[12] = p[1];
        m[5] = p[2];
        m[13] = p[3];
        m[10] = 2.0f * p[4];
        m[14] = 2.0f * p[5] + 1.0f;
        m[15] = 1.0f;
    }
    glc_projection(m);

    /* The modelview is identity: gx_draw.c transforms positions and normals on
     * the CPU with the loaded position/normal matrices, because the game loads
     * normal matrices that are not the inverse transpose of the position
     * matrix and GL would compute its own. */
    glc_modelview_identity();
}
