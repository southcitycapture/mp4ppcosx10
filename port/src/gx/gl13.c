/* The only file in the port that talks to OpenGL, and the window it talks to.
 *
 * The target is a Radeon 9000 (R250) on Mac OS X 10.5: **OpenGL 1.3**, six
 * texture units, `ARB_texture_env_combine` + `ATI_texture_env_combine3` +
 * the crossbar, `EXT_blend_subtract`, `EXT_texture_compression_s3tc`, no
 * fragment programs, no FBOs, no NPOT.  The development Mac's driver is far
 * newer than that, which is a hazard rather than a help: it is very easy to
 * write something here that works on the Mac and silently does nothing on the
 * G4.  Two things guard against it.
 *
 *  - `--glcheck` checks every GL entry point this file calls against a written
 *    list of what GL 1.3 plus those named extensions actually contains, and
 *    fails loudly on anything else.  The list is maintained by hand on
 *    purpose: adding a GL call without adding it to the list is exactly the
 *    mistake worth catching.
 *  - the extension flags below are answered from the *card's* feature set, not
 *    the host's, unless the host is poorer still.  So the fallback paths the
 *    G4 will take are the ones that get exercised on the Mac.
 *
 * The window is 640x480 -- the game's own EFB and XFB size -- scaled by
 * `--scale`.  `--headless` runs the whole GX pipeline with no window and no
 * GL at all, which is what a scripted run wants.
 */
#include "gx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_skin.h"

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#if defined(__APPLE__) && defined(__ppc__)
#include <OpenGL/OpenGL.h>
#endif
#endif

int gl13_have_combine3 = 1;
int gl13_have_crossbar = 1;
int gl13_have_s3tc = 1;
int gl13_have_blend_subtract = 1;
int gl13_have_depth_texture = 1;
int gl13_max_tex_units = 6; /* the Radeon 9000's number, not the host's */

#define EFB_W 640
#define EFB_H 480

#ifndef PORT_NO_SDL
static SDL_Window* window;
static SDL_GLContext ctx;
#endif
static int gl_on;
static unsigned frame_no;
static const char* pending_shot;

/* ---- --glcheck ------------------------------------------------------------ */
/* Everything GL this backend is allowed to call.  GL 1.3 core plus the four
 * named extensions; anything else is a bug that would show up on the G4 as a
 * missing symbol or a silently ignored call. */
static const char* const GL13_ALLOWED[] = {
    "glAlphaFunc", "glBindTexture", "glBlendEquation", "glBlendFunc", "glClear",
    "glClearColor", "glClearDepth", "glColorMask", "glColorPointer",
    "glCompressedTexImage2D", "glCopyTexSubImage2D", "glCullFace", "glDepthFunc",
    "glDepthMask", "glDepthRange", "glDisable", "glDisableClientState",
    "glDrawArrays", "glEnable", "glEnableClientState", "glFinish", "glFogf",
    "glFogfv", "glFogi", "glFrontFace", "glGenTextures", "glGetString",
    "glGetIntegerv", "glLightf", "glLightfv", "glLoadIdentity", "glLoadMatrixf",
    "glMaterialfv", "glMatrixMode", "glNormalPointer", "glPixelStorei",
    "glReadPixels", "glScissor", "glTexCoordPointer", "glTexEnvf", "glTexEnvfv",
    "glTexEnvi", "glTexImage2D", "glTexParameterf", "glTexParameteri",
    "glVertexPointer", "glViewport", "glActiveTexture", "glClientActiveTexture",
    "glDeleteTextures", "glShadeModel", "glLightModelfv", "glLightModeli",
    "glColorMaterial", "glPolygonMode", "glHint", "glGetError",
    "glGetTexImage", /* M20: port_gx_copy_read, the copy m415 reads back */
    /* M16, the per-draw submit (PLAN.md 31): GL_APPLE_vertex_array_range,
     * GL_APPLE_fence and GL_EXT_multi_draw_arrays are all in g4-glinfo.log,
     * and every one of them is resolved at run time and skipped when absent. */
    "glVertexArrayRangeAPPLE", "glFlushVertexArrayRangeAPPLE",
    "glVertexArrayParameteriAPPLE", "glGenFencesAPPLE", "glSetFenceAPPLE",
    "glFinishFenceAPPLE", "glTestFenceAPPLE", "glMultiDrawArraysEXT",
    "glDrawElements",
    "glDrawRangeElements", /* M21: one call per batch (GL 1.2 core) */
    /* M23: gl13_downsample_read, the copy read back at half size */
    "glBegin", "glEnd", "glTexCoord2f", "glVertex2f", "glPushMatrix", "glPopMatrix",
    "glOrtho", "glTexEnvi",
};

int gl13_check(const char* fn) {
    size_t i;
    for (i = 0; i < sizeof(GL13_ALLOWED) / sizeof(GL13_ALLOWED[0]); i++) {
        if (strcmp(GL13_ALLOWED[i], fn) == 0) {
            return 0;
        }
    }
    port_log("*** --glcheck: %s is not in the GL 1.3 + named-extension set the "
             "Radeon 9000 provides\n", fn);
    port_fatal("--glcheck: %s", fn);
    return 0;
}

#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

/* ---- ATI_texture_env_combine3, spelled out so no host header is needed ---- */
#ifndef GL_COLOR_SUM
#define GL_COLOR_SUM 0x8458
#endif
#ifndef GL_MODULATE_ADD_ATI
#define GL_MODULATE_ADD_ATI 0x8744
#define GL_MODULATE_SIGNED_ADD_ATI 0x8745
#define GL_MODULATE_SUBTRACT_ATI 0x8746
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

/* The 1x1 white texture's GL name; see glc_white_texture below. */
static unsigned glc_white_name;
static unsigned glc_emitted, glc_elided;

/* --gltrace F: every GL call the shadow lets through during frame F, with
 * its arguments, so two submit shapes can be diffed call by call (M16). */
unsigned gl13_frame_number(void);
int gl13_trace_armed(void) {
    unsigned f = gl13_frame_number() + 1;
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
    /* The vertex program's binding, enable and parameter block are GL state
     * this shadow does not hold, and they are forgotten for the same reason.
     * So is the TEV state cache (PLAN.md 28.5): it skips the `glTexEnv` calls
     * on the strength of the shadow already holding the right values, and a
     * shadow that has forgotten them is a cache that must forget too. */
    gx_vprog_invalidate();
    gx_tev_cache_invalidate();
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

void glc_stats(unsigned* emitted, unsigned* elided) {
    *emitted = glc_emitted;
    *elided = glc_elided;
}

void glc_active_texture(int unit) {
    HIT(glc.active_tex == unit);
    glc.active_tex = unit;
    TR("glActiveTexture %d\n", unit);
    GL(glActiveTexture)((GLenum)(GL_TEXTURE0 + unit));
}

void glc_client_active_texture(int unit) {
    HIT(glc.client_active_tex == unit);
    glc.client_active_tex = unit;
    TR("glClientActiveTexture %d\n", unit);
    GL(glClientActiveTexture)((GLenum)(GL_TEXTURE0 + unit));
}

void glc_bind_texture(int unit, unsigned name) {
    HIT(glc.unit[unit].tex_name == name);
    glc.unit[unit].tex_name = name;
    glc_active_texture(unit);
    TR("glBindTexture unit %d name %u\n", unit, name);
    GL(glBindTexture)(GL_TEXTURE_2D, (GLuint)name);
}

/* The binding a texture *upload* leaves behind: gx_tex.c has to bind the name
 * it is about to fill, and the shadow has to be told rather than guess. */
void glc_note_bind(int unit, unsigned name) { glc.unit[unit].tex_name = name; }

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
void glc_tex_matrix(int unit, float su, float sv) { glc_tex_matrix_fold(unit, su, sv, 0.0f); }
void glc_tex_matrix_fold(int unit, float su, float sv, float tv) {
    GLfloat m[16];
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

/* M21: the colour sum (GL 1.4 core, GL_EXT_secondary_color on GL 1.3 -- the
 * token is the same, 0x8458, and g4-glinfo.log lists the extension).  Adds
 * the fragment's secondary colour after the texture units; the vertex
 * program writes it (gx_vprog.c) for the hilite fold and nothing else. */
#define GLC_COLOR_SUM 0x8458
void glc_color_sum(int on) {
    glc_enable(GLC_COLOR_SUM, on, &glc.color_sum_on);
}

void glc_projection(const float* m) {
    HIT(glc.proj_valid && memcmp(glc.proj, m, sizeof(float) * 16) == 0);
    memcpy(glc.proj, m, sizeof(float) * 16);
    glc.proj_valid = 1;
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glLoadMatrixf)((const GLfloat*)m);
    GL(glMatrixMode)(GL_MODELVIEW);
}

void glc_modelview_identity(void) {
    HIT(glc.modelview_identity);
    glc.modelview_identity = 1;
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glLoadIdentity)();
}

/* The vertex arrays never move: `verts` is a static buffer and every draw
 * reads it from index zero, so the pointers are set once for the life of the
 * process and only the per-unit enables change. */
void glc_vertex_array(const void* p, int stride) {
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

void glc_fogcoord_array(const void* p, int stride) {
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
        glc_FogCoordPointerEXT(GL_FLOAT, (GLsizei)stride, p);
    }
}

/* The NPOT fold this unit is carrying, so a vertex program can apply it: the
 * fixed-function GL_TEXTURE matrix is not consulted while a program is bound,
 * and the program has to reproduce it (gx_vprog.c). */
void glc_get_tex_scale(int unit, float* su, float* sv) {
    float tv;
    glc_get_tex_fold(unit, su, sv, &tv);
}
void glc_get_tex_fold(int unit, float* su, float* sv, float* tv) {
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


/* ---- the vertex ring: GL_APPLE_vertex_array_range + GL_APPLE_fence --------
 *
 * M16 (PLAN.md 31).  The board profile put 28% of the frame inside Apple's
 * `gleDrawArraysOrElements_IMM_Exec` -- the *immediate* client-array path,
 * which copies every draw's vertices into the command buffer, selects a
 * generated "vertex submit function" for the layout, and pages the buffer off
 * to the kernel (`gldPageoffBuffer` -> `io_connect_map_memory`, 7% of the
 * frame on its own) every time the copies fill it.  None of that is geometry.
 *
 * `GL_APPLE_vertex_array_range` is this driver's way of not copying: a range of
 * client memory the card can read by DMA, so an array pointer inside it is a
 * reference in the command stream rather than a copy.  The decoder writes the
 * packed source vertex straight into that range (it is `src_buf` in
 * gx_draw.c), one `glFlushVertexArrayRangeAPPLE` per batch pushes the CPU
 * cache out ahead of the DMA, and the draw is a pointer.
 *
 * The cost of not copying is that the memory is now *read later*, so it must
 * not be overwritten until the card has finished with it.  The range is a
 * ring split into VAR_CHUNKS equal chunks; a fence is set on a chunk when the
 * writer leaves it (after the draws that read it were issued), and finished
 * before the writer enters it again a lap later.  With a frame at 2 MB and an
 * 8 MB ring the wait is on a fence three frames old and never blocks, and
 * `var_waits_blocked` counts the times it did.
 *
 * The tokens and entry points are spelled out here for the reason
 * gx_vprog.c gives: the 10.4u SDK's headers do not have them. */
#define VAR_VERTEX_ARRAY_RANGE_APPLE        0x851D
#define VAR_VERTEX_ARRAY_STORAGE_HINT_APPLE 0x851F
#define VAR_STORAGE_SHARED_APPLE            0x85BF
#define VAR_STORAGE_CACHED_APPLE            0x85BE

#ifndef PORT_NO_SDL
typedef void (*var_range_t)(GLsizei, const void*);
typedef void (*var_param_t)(GLenum, GLint);
typedef void (*fence_gen_t)(GLsizei, GLuint*);
typedef void (*fence_set_t)(GLuint);
typedef void (*fence_finish_t)(GLuint);
typedef GLboolean (*fence_test_t)(GLuint);
typedef void (*multidraw_t)(GLenum, const GLint*, const GLsizei*, GLsizei);

static var_range_t   var_VertexArrayRangeAPPLE;
static var_range_t   var_FlushVertexArrayRangeAPPLE;
static var_param_t   var_VertexArrayParameteriAPPLE;
static fence_gen_t   var_GenFencesAPPLE;
static fence_set_t   var_SetFenceAPPLE;
static fence_finish_t var_FinishFenceAPPLE;
static fence_test_t  var_TestFenceAPPLE;
static multidraw_t   var_MultiDrawArraysEXT;
#endif

#define VAR_CHUNKS 4
#ifdef PORT_NO_SDL
typedef unsigned GLuint;
#endif
static u8* var_ring;
static size_t var_ring_bytes;
static int var_on;
static int var_multidraw;
static GLuint var_fences[VAR_CHUNKS];
static signed char var_fence_valid[VAR_CHUNKS];
static unsigned var_waits, var_waits_blocked, var_sets, var_flushes;

int gl13_var_active(void) { return var_on; }
int gl13_have_multidraw(void) { return var_multidraw; }

void gl13_var_stats(unsigned* waits, unsigned* blocked, unsigned* sets, unsigned* flushes) {
    *waits = var_waits;
    *blocked = var_waits_blocked;
    *sets = var_sets;
    *flushes = var_flushes;
}

/* Allocate the ring and, when the extensions are there and --novar is not,
 * hand it to the driver.  Returns the ring either way: without VAR it is an
 * ordinary buffer the client-array path copies from, exactly as the static
 * `src_buf` was, so `--novar` and `--oldsubmit` measure the submit shape and
 * the copy separately. */
u8* gl13_var_setup(size_t bytes) {
    void* mem = NULL;
    if (var_ring) {
        return var_ring;
    }
    /* page-aligned, which is what the driver wants for a DMA range; valloc
     * because posix_memalign is 10.6 and the target is 10.4/10.5 */
    mem = valloc(bytes);
    if (!mem) {
        port_fatal("vertex ring: cannot allocate %lu bytes", (unsigned long)bytes);
        return NULL;
    }
    memset(mem, 0, bytes);
    var_ring = (u8*)mem;
    var_ring_bytes = bytes;
#ifndef PORT_NO_SDL
    if (gl_on && !port_opt.novar && !port_opt.oldsubmit) {
        const char* ext = (const char*)GL(glGetString)(GL_EXTENSIONS);
        int have = ext && strstr(ext, "GL_APPLE_vertex_array_range") &&
                   strstr(ext, "GL_APPLE_fence");
        if (have) {
            var_VertexArrayRangeAPPLE = (var_range_t)SDL_GL_GetProcAddress("glVertexArrayRangeAPPLE");
            var_FlushVertexArrayRangeAPPLE = (var_range_t)SDL_GL_GetProcAddress("glFlushVertexArrayRangeAPPLE");
            var_VertexArrayParameteriAPPLE = (var_param_t)SDL_GL_GetProcAddress("glVertexArrayParameteriAPPLE");
            var_GenFencesAPPLE = (fence_gen_t)SDL_GL_GetProcAddress("glGenFencesAPPLE");
            var_SetFenceAPPLE = (fence_set_t)SDL_GL_GetProcAddress("glSetFenceAPPLE");
            var_FinishFenceAPPLE = (fence_finish_t)SDL_GL_GetProcAddress("glFinishFenceAPPLE");
            var_TestFenceAPPLE = (fence_test_t)SDL_GL_GetProcAddress("glTestFenceAPPLE");
            have = var_VertexArrayRangeAPPLE && var_FlushVertexArrayRangeAPPLE &&
                   var_VertexArrayParameteriAPPLE && var_GenFencesAPPLE &&
                   var_SetFenceAPPLE && var_FinishFenceAPPLE && var_TestFenceAPPLE;
        }
        if (have) {
            (port_opt.glcheck ? gl13_check("glGenFencesAPPLE") : 0);
            var_GenFencesAPPLE(VAR_CHUNKS, var_fences);
            (port_opt.glcheck ? gl13_check("glVertexArrayParameteriAPPLE") : 0);
            var_VertexArrayParameteriAPPLE(VAR_VERTEX_ARRAY_STORAGE_HINT_APPLE,
                                           VAR_STORAGE_SHARED_APPLE);
            (port_opt.glcheck ? gl13_check("glVertexArrayRangeAPPLE") : 0);
            var_VertexArrayRangeAPPLE((GLsizei)bytes, var_ring);
            GL(glEnableClientState)(VAR_VERTEX_ARRAY_RANGE_APPLE);
            var_on = 1;
        }
        var_multidraw = 0;
        if (ext && strstr(ext, "GL_EXT_multi_draw_arrays")) {
            var_MultiDrawArraysEXT = (multidraw_t)SDL_GL_GetProcAddress("glMultiDrawArraysEXT");
            var_multidraw = var_MultiDrawArraysEXT != NULL;
        }
        port_log("port> vertex ring: %lu KB in %d chunks, vertex_array_range %s, "
                 "multi_draw_arrays %s\n",
                 (unsigned long)(bytes / 1024), VAR_CHUNKS,
                 var_on ? "on" : (have ? "off (--novar)" : "absent"),
                 var_multidraw ? "yes" : "no");
    }
#endif
    return var_ring;
}

static int var_chunk_of(size_t off) {
    size_t c = off / (var_ring_bytes / VAR_CHUNKS);
    return c >= VAR_CHUNKS ? VAR_CHUNKS - 1 : (int)c;
}

/* The writer is about to write [off, off+len): finish the fence of every
 * chunk that span touches, if one was set when the writer last left it. */
void gl13_var_enter(size_t off, size_t len) {
#ifndef PORT_NO_SDL
    int c0, c1, c;
    if (!var_on || !len) {
        return;
    }
    c0 = var_chunk_of(off);
    c1 = var_chunk_of(off + len - 1);
    for (c = c0; c <= c1; c++) {
        if (var_fence_valid[c]) {
            var_waits++;
            if (!var_TestFenceAPPLE(var_fences[c])) {
                var_waits_blocked++;
                var_FinishFenceAPPLE(var_fences[c]);
            }
            var_fence_valid[c] = 0;
        }
    }
#else
    (void)off;
    (void)len;
#endif
}

/* The draws reading [off, off+len) have been issued and the writer now
 * stands at `cursor`: flush the CPU cache over the range ahead of the DMA
 * (before the draw, see gl13_var_flush) and fence every chunk the writer has
 * finished with. */
void gl13_var_flush(const void* p, size_t len) {
#ifndef PORT_NO_SDL
    if (!var_on || !len) {
        return;
    }
    var_flushes++;
    var_FlushVertexArrayRangeAPPLE((GLsizei)len, p);
#else
    (void)p;
    (void)len;
#endif
}

void gl13_var_left(size_t from, size_t cursor, int wrapped) {
#ifndef PORT_NO_SDL
    int c0, c1, c;
    if (!var_on) {
        return;
    }
    c0 = var_chunk_of(from);
    c1 = var_chunk_of(cursor);
    if (wrapped) {
        /* the writer wrapped: every chunk from `from`'s to the end is done,
         * and so is every chunk before the one the cursor now stands in */
        for (c = c0; c < VAR_CHUNKS; c++) {
            var_SetFenceAPPLE(var_fences[c]);
            var_fence_valid[c] = 1;
            var_sets++;
        }
        c0 = 0;
    }
    for (c = c0; c < c1; c++) {
        var_SetFenceAPPLE(var_fences[c]);
        var_fence_valid[c] = 1;
        var_sets++;
    }
#else
    (void)from;
    (void)cursor;
    (void)wrapped;
#endif
}

/* M21: one indexed draw per batch.  glDrawRangeElements is GL 1.2 core and in
 * the 10.4u SDK's gl.h; `wide` selects GL_UNSIGNED_INT indices. */
void gl13_draw_range_elements(unsigned mode, unsigned lo, unsigned hi, int n, int wide,
                              const void* idx) {
#ifndef PORT_NO_SDL
    if (gl13_trace_armed()) {
        port_log("gltrace> glDrawRangeElements mode %04x range %u..%u count %d %s\n", mode,
                 lo, hi, n, wide ? "u32" : "u16");
    }
    GL(glDrawRangeElements)((GLenum)mode, (GLuint)lo, (GLuint)hi, (GLsizei)n,
                            wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, idx);
#else
    (void)mode; (void)lo; (void)hi; (void)n; (void)wide; (void)idx;
#endif
}

void gl13_multi_draw_arrays(unsigned mode, const int* first, const int* count, int n) {
#ifndef PORT_NO_SDL
    (port_opt.glcheck ? gl13_check("glMultiDrawArraysEXT") : 0);
    var_MultiDrawArraysEXT((GLenum)mode, (const GLint*)first, (const GLsizei*)count,
                           (GLsizei)n);
#else
    (void)mode;
    (void)first;
    (void)count;
    (void)n;
#endif
}

/* ---- bring-up ------------------------------------------------------------- */

static void report_caps(void) {
#ifndef PORT_NO_SDL
    const char* ver = (const char*)GL(glGetString)(GL_VERSION);
    const char* ren = (const char*)GL(glGetString)(GL_RENDERER);
    const char* ext = (const char*)GL(glGetString)(GL_EXTENSIONS);
    GLint units = 0;
    GL(glGetIntegerv)(GL_MAX_TEXTURE_UNITS, &units);
    port_log("port> GL: %s / %s, %d texture units\n", ren ? ren : "?", ver ? ver : "?",
             (int)units);
    /* The card is the floor and the host is the ceiling: take the smaller. */
    if (units > 0 && units < gl13_max_tex_units) {
        gl13_max_tex_units = (int)units;
    }
    if (ext) {
        if (!strstr(ext, "ATI_texture_env_combine3")) {
            gl13_have_combine3 = 0;
        }
        if (!strstr(ext, "texture_env_crossbar") && !strstr(ext, "ARB_texture_env_crossbar")) {
            gl13_have_crossbar = 0;
        }
        if (!strstr(ext, "texture_compression_s3tc")) {
            gl13_have_s3tc = 0;
        }
        if (!strstr(ext, "blend_subtract")) {
            gl13_have_blend_subtract = 0;
        }
        if (!strstr(ext, "depth_texture")) {
            gl13_have_depth_texture = 0;
        }
    }
    port_log("port> GL feature set used: combine3 %d, crossbar %d, s3tc %d, "
             "blend_subtract %d, depth_texture %d, %d units\n",
             gl13_have_combine3, gl13_have_crossbar, gl13_have_s3tc,
             gl13_have_blend_subtract, gl13_have_depth_texture, gl13_max_tex_units);
    /* --glinfo is the N64 ports' driver dump: the whole GL identity and every
     * extension string, one per line, plus the limits the backend leans on.
     * The card is the specification for this port and this is how it is
     * recorded rather than remembered. */
    if (port_opt.glinfo) {
        static const struct {
            GLenum e;
            const char* name;
        } limits[] = {
            { GL_MAX_TEXTURE_UNITS, "GL_MAX_TEXTURE_UNITS" },
            { GL_MAX_TEXTURE_SIZE, "GL_MAX_TEXTURE_SIZE" },
            { GL_MAX_LIGHTS, "GL_MAX_LIGHTS" },
            { GL_MAX_MODELVIEW_STACK_DEPTH, "GL_MAX_MODELVIEW_STACK_DEPTH" },
            { GL_MAX_PROJECTION_STACK_DEPTH, "GL_MAX_PROJECTION_STACK_DEPTH" },
            { GL_MAX_TEXTURE_STACK_DEPTH, "GL_MAX_TEXTURE_STACK_DEPTH" },
            { GL_RED_BITS, "GL_RED_BITS" },
            { GL_GREEN_BITS, "GL_GREEN_BITS" },
            { GL_BLUE_BITS, "GL_BLUE_BITS" },
            { GL_ALPHA_BITS, "GL_ALPHA_BITS" },
            { GL_DEPTH_BITS, "GL_DEPTH_BITS" },
            { GL_STENCIL_BITS, "GL_STENCIL_BITS" },
        };
        const char* ven = (const char*)GL(glGetString)(GL_VENDOR);
        size_t li;
        int n = 0;
        port_log("---- --glinfo ----\n");
        port_log("GL_VENDOR    %s\n", ven ? ven : "?");
        port_log("GL_RENDERER  %s\n", ren ? ren : "?");
        port_log("GL_VERSION   %s\n", ver ? ver : "?");
        for (li = 0; li < sizeof(limits) / sizeof(limits[0]); li++) {
            GLint v = 0;
            GL(glGetIntegerv)(limits[li].e, &v);
            port_log("%-30s %d\n", limits[li].name, (int)v);
        }
        if (ext) {
            const char* p = ext;
            while (*p) {
                const char* q = p;
                while (*q && *q != ' ') {
                    q++;
                }
                if (q > p) {
                    port_log("ext %.*s\n", (int)(q - p), p);
                    n++;
                }
                p = *q ? q + 1 : q;
            }
        }
        port_log("---- %d extensions ----\n", n);
    }
#endif
}

int gl13_init(void) {
#ifdef PORT_NO_SDL
    port_log("port> built without SDL2: running headless\n");
    return 0;
#else
    int scale = port_opt.scale > 0 ? port_opt.scale : 1;
    if (port_opt.headless) {
        port_log("port> --headless: the GX pipeline runs, nothing is presented\n");
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        port_log("port> SDL_Init failed (%s); running headless\n", SDL_GetError());
        return 0;
    }
    /* No core profile: the whole backend is fixed function, which is what the
     * Radeon 9000 has and all it has. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    window = SDL_CreateWindow("Mario Party 4", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, EFB_W * scale, EFB_H * scale,
                              SDL_WINDOW_OPENGL);
    if (!window) {
        port_log("port> SDL_CreateWindow failed (%s); running headless\n",
                 SDL_GetError());
        return 0;
    }
    ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        port_log("port> SDL_GL_CreateContext failed (%s); running headless\n",
                 SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(0); /* the game paces itself at the retrace gate */
    gl_on = 1;
    glc_invalidate();
    report_caps();
#if defined(__APPLE__) && defined(__ppc__)
    /* --mpgl (M17): Apple's multithreaded GL engine, which moves the driver's
     * own command processing -- the copy into the command buffer and the
     * page-off to the kernel that were 28-35% of a drawn frame in the M16
     * profile (PLAN.md 31.1) -- onto a second thread.  This G4 is a dual
     * 1 GHz (PowerMac3,5), so that thread has a CPU of its own.  Whether
     * Leopard's ATI driver honours it on a Radeon 9000 is what the return
     * value says; kCGLCEMPEngine is 313 and is not in the 10.4u SDK. */
    if (port_opt.mpgl) {
        CGLContextObj cgl = CGLGetCurrentContext();
        CGLError err = cgl ? CGLEnable(cgl, (CGLContextEnable)313) : (CGLError)-1;
        port_log("port> --mpgl: CGLEnable(kCGLCEMPEngine) = %d (%s)\n", (int)err,
                 err == 0 ? "on: the driver runs on its own thread" : "refused");
    }
#endif
    /* The vertex-program probe needs a live context, so it runs here and not
     * in report_caps: it compiles and loads a program. */
    if (port_opt.vprobe || port_opt.glinfo || !port_opt.cpuxf) {
        gx_vprog_probe();
    }
    GL(glPixelStorei)(GL_UNPACK_ALIGNMENT, 1);
    GL(glEnable)(GL_SCISSOR_TEST);
    GL(glShadeModel)(GL_SMOOTH);
    GL(glClearColor)(0.0f, 0.0f, 0.0f, 1.0f);
    GL(glClearDepth)(1.0);
    GL(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return 1;
#endif
}

void gl13_shutdown(void) {
    glc_white_name = 0; /* dies with the context */
#ifndef PORT_NO_SDL
    if (ctx) {
        SDL_GL_DeleteContext(ctx);
        ctx = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    if (gl_on) {
        SDL_Quit();
    }
#endif
    gl_on = 0;
}

/* --nodraw / --ffto: the renderer switched off underneath a live GL context.
 *
 * `gl13_live()` is already the question every GL-touching path in the backend
 * asks -- the texture cache, the TEV chain, the EFB copy, phase 2 of the
 * vertex path -- because `--headless` has to answer it too.  So switching the
 * renderer off is not a new pipeline: it is this one function returning 0,
 * plus the two places that would otherwise *decode* something before finding
 * out that nobody wants it (the display-list vertex decode in gx_draw.c and
 * the texture decode in gx_tex.c), which ask `gl13_draw_off()` directly.
 *
 * `gl_on` still means "there is a window and a context", so `gl13_write_ppm`,
 * the shutdown path and `--dumpframe` keep working; drawing comes back with
 * `gl13_set_draw_off(0)` and a texture-cache flush.  Nothing here is visible to
 * the game: GX is a write-only command stream apart from the draw-sync token,
 * which lives in gx_state.c and is untouched. */
static int draw_off;

int gl13_live(void) { return gl_on && !draw_off; }
int gl13_have_context(void) { return gl_on; }
int gl13_draw_off(void) { return draw_off; }

void gl13_set_draw_off(int v) {
    draw_off = v ? 1 : 0;
    if (!draw_off) {
        /* Everything the shadow believes about GL state was recorded before
         * the pause, and the pause emitted nothing; forget it all. */
        glc_invalidate();
    }
}


/* ---- per-frame ------------------------------------------------------------ */

/* GXCopyDisp's clear is deferred to just after the swap.  On the console
 * GXCopyDisp *copies* the EFB into the XFB and only then clears the EFB for
 * the next frame, so the clear never touches the image being shown.  Here the
 * back buffer is the image, so clearing it when the game asks -- inside
 * HuSysDoneRender, before SwapBuffers -- throws away the frame that was just
 * drawn and presents a solid clear colour instead.  Every frame renders
 * correctly and every frame is black, which is a memorable way to spend an
 * afternoon.  See PLAN.md §12. */
static int clear_pending;
static GXColor clear_color;
static u32 clear_z;

void gl13_clear_at_swap(GXColor c, u32 z) {
    clear_pending = 1;
    clear_color = c;
    clear_z = z;
}

void gl13_clear(GXColor c, u32 z) {
    if (!gl_on) {
        return;
    }
    GL(glScissor)(0, 0, EFB_W, EFB_H);
    GL(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GL(glDepthMask)(GL_TRUE);
    GL(glClearColor)(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
    GL(glClearDepth)((double)z / (double)0xFFFFFF);
    GL(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* The clear sets scissor, both masks and the clear colour behind the
     * shadow's back, so the shadow forgets.  Once a frame, which is nothing. */
    glc_invalidate();
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

    /* GX's front face is the opposite of GL's default. */
    {
        int on = gx.cull != GX_CULL_NONE;
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

    glc_enable(GL_DEPTH_TEST, gx.z_enable ? 1 : 0, &glc.depth_on);
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
    if (glc.depth_mask != (signed char)(gx.z_update ? 1 : 0)) {
        glc.depth_mask = (signed char)(gx.z_update ? 1 : 0);
        glc_emitted++;
        GL(glDepthMask)(gx.z_update ? GL_TRUE : GL_FALSE);
    } else {
        glc_elided++;
    }
    {
        signed char cm[4];
        cm[0] = cm[1] = cm[2] = (signed char)(gx.color_update ? 1 : 0);
        cm[3] = (signed char)(gx.alpha_update ? 1 : 0);
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
        int on = (gx.blend_mode == GX_BM_BLEND || gx.blend_mode == GX_BM_SUBTRACT);
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
        if (pass_all || use == GX_ALWAYS) {
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
     * in eye z, which is what GL_FOG is. */
    if (gx.fog_type == GX_FOG_NONE) {
        glc_enable(GL_FOG, 0, &glc.fog_on);
    } else {
        GLfloat c[4];
        int mode;
        float density = 0.0f;
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
            if (glc.fog_start != gx.fog_startz || glc.fog_end != gx.fog_endz) {
                glc.fog_start = gx.fog_startz;
                glc.fog_end = gx.fog_endz;
                glc_emitted++;
                GL(glFogf)(GL_FOG_START, gx.fog_startz);
                GL(glFogf)(GL_FOG_END, gx.fog_endz);
            } else {
                glc_elided++;
            }
        } else {
            density = 1.0f / (gx.fog_endz - gx.fog_startz + 1.0f);
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

/* ---- present -------------------------------------------------------------- */

void gl13_write_ppm(const char* path) {
#ifndef PORT_NO_SDL
    unsigned char* buf;
    FILE* f;
    int y;
    int w = EFB_W, h = EFB_H;
    if (!gl_on) {
        port_log("port> --dumpframe: nothing to write, there is no GL context\n");
        return;
    }
    buf = (unsigned char*)malloc((size_t)w * h * 3);
    if (!buf) {
        return;
    }
    GL(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
    GL(glReadPixels)(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    f = fopen(path, "wb");
    if (!f) {
        port_log("port> --dumpframe: cannot write %s\n", path);
        free(buf);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--) { /* GL reads bottom-up */
        fwrite(buf + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    }
    fclose(f);
    free(buf);
    port_log("port> --dumpframe: wrote %s\n", path);
#else
    (void)path;
#endif
}

/* --dumpframe's argument is a frame *set*, not a frame: "187", "1,90,186" or
 * "1-400/20" (first-last/step), and any comma-separated mixture of the three.
 * A spread is what the comparison against Dolphin actually needs -- the two
 * sides do not agree on absolute frame numbers, because the port skips the
 * console's DVD seek, so you shoot a spread on both, find the matching pair
 * once, and reuse it (PLAN.md §5.1). */
static int frame_wanted(unsigned n) {
    const char* p = port_opt.dumpframe;
    if (!p) {
        return 0;
    }
    while (*p) {
        long a, b, step = 1;
        char* e;
        a = strtol(p, &e, 10);
        if (e == p) {
            break;
        }
        p = e;
        b = a;
        if (*p == '-') {
            b = strtol(p + 1, &e, 10);
            p = e;
        }
        if (*p == '/') {
            step = strtol(p + 1, &e, 10);
            p = e;
            if (step < 1) {
                step = 1;
            }
        }
        if ((long)n >= a && (long)n <= b && ((long)n - a) % step == 0) {
            return 1;
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

void gl13_present(void) {
#ifndef PORT_NO_SDL
    /* Nothing after the last draw of a frame -- the readback, the swap, the
     * clear -- wants a vertex program bound, and gxdemo and the PPM reader
     * both use fixed function. */
    gx_vprog_disable();
    gx_vprog_frame_reset();
    gx_skin_frame_end();
    frame_no++;
    port_scenelog();
    port_ovllog();
    port_nanwatch();
    if (frame_wanted(frame_no) && draw_off) {
        /* --nodraw: the EFB holds whatever was last drawn, which is not this
         * frame.  Writing it would put a wrong picture under the right name,
         * and an md5 taken from it would be a lie. */
        port_log("port> --dumpframe %u skipped: drawing is off (--nodraw); "
                 "use --ffto %u to arrive there with the renderer on\n",
                 frame_no, frame_no);
    } else if (frame_wanted(frame_no)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame-%05u.ppm",
                 port_opt.shotdir ? port_opt.shotdir : ".", frame_no);
        gl13_write_ppm(path);
    }
    if (pending_shot && !draw_off) {
        /* a consumed frame's EFB is not this frame; frame mode draws the
         * next one (gl13_shot_pending) and the shot is taken then */
        gl13_write_ppm(pending_shot);
        pending_shot = NULL;
    }
    if (!gl_on || draw_off) {
        return; /* nothing was drawn, so there is nothing to show */
    }
    SDL_GL_SwapWindow(window);
    /* The clear the game asked for is run by gl13_begin_frame(), which the
     * gate calls once it knows the next frame is drawn -- under --realtime a
     * consumed frame in between may have asked for a different colour, and
     * the clear that precedes a drawn frame has to be the *latest* one. */
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                port_request_reset();
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        port_request_reset();
                        break;
                    case SDLK_F12: {
                        static char path[1024];
                        snprintf(path, sizeof(path), "%s/shot-%05u.ppm",
                                 port_opt.shotdir ? port_opt.shotdir : ".", frame_no);
                        pending_shot = path;
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
#endif
}

unsigned gl13_frame_number(void) { return frame_no; }

/* For the gate's frame-mode decision (src/platform/framemode.c): is frame N
 * one --dumpframe wants, and is an F12 shot waiting for a drawn frame. */
int gl13_frame_wanted(unsigned n) { return frame_wanted(n); }
int gl13_shot_pending(void) { return pending_shot != NULL; }

/* The frame the game is about to build will be drawn: run the clear its last
 * GXCopyDisp asked for.  In lockstep this follows the swap immediately, which
 * is where the clear used to be; under --realtime it is the first thing a
 * drawn frame does after any number of consumed ones. */
void gl13_begin_frame(void) {
    if (clear_pending && gl_on && !draw_off) {
        clear_pending = 0;
        gl13_clear(clear_color, clear_z);
    }
}

/* M23 (PLAN.md 38): a texture drawn into a rectangle of the back buffer and
 * read back from there.  port_gx_copy_read wants the shadow map at the
 * game's size -- 192x192 -- and the copy holds it at the pass's 384x384 in
 * a 512x512 texture; glGetTexImage of that is a megabyte back over AGP,
 * 120 ms on the Radeon 9000, and Stamp Out!'s intro asks 120 times.  A
 * bilinear quad at exactly 2:1 is the copy unit's 2x2 box filter (every
 * output pixel's centre lands on a texel boundary in both axes), and a
 * glReadPixels of 192x192 is a seventh of the bytes.  The rectangle is the
 * shadow region itself, which the game's clear-after copy wipes right
 * after.  The state is set directly and the shadow forgets it. */
void gl13_downsample_read(unsigned name, float su, float sv, int x, int y, int w, int h,
                          unsigned char* out_rgba) {
    int i;
    if (!gl_on) {
        return;
    }
    for (i = 5; i >= 0; i--) {
        GL(glActiveTexture)(GL_TEXTURE0 + i);
        if (i) {
            GL(glDisable)(GL_TEXTURE_2D);
        }
    }
    gx_vprog_disable();
    GL(glEnable)(GL_TEXTURE_2D);
    GL(glBindTexture)(GL_TEXTURE_2D, name);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL(glTexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    GL(glDisable)(GL_BLEND);
    GL(glDisable)(GL_DEPTH_TEST);
    GL(glDisable)(GL_ALPHA_TEST);
    GL(glDisable)(GL_CULL_FACE);
    GL(glDisable)(GL_FOG);
    GL(glDisable)(GL_LIGHTING);
    GL(glDisable)(GL_COLOR_SUM);
    GL(glDisable)(GL_SCISSOR_TEST);
    GL(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GL(glViewport)(0, 0, EFB_W, EFB_H);
    GL(glMatrixMode)(GL_TEXTURE);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
    GL(glOrtho)(0.0, (double)EFB_W, 0.0, (double)EFB_H, -1.0, 1.0);
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
    GL(glBegin)(GL_QUADS);
    GL(glTexCoord2f)(0.0f, 0.0f);
    GL(glVertex2f)((float)x, (float)y);
    GL(glTexCoord2f)(su, 0.0f);
    GL(glVertex2f)((float)(x + w), (float)y);
    GL(glTexCoord2f)(su, sv);
    GL(glVertex2f)((float)(x + w), (float)(y + h));
    GL(glTexCoord2f)(0.0f, sv);
    GL(glVertex2f)((float)x, (float)(y + h));
    GL(glEnd)();
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_TEXTURE);
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
    GL(glReadPixels)(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    glc_invalidate();
}

/* --restore sets the frame number back to the snapshot's, so --dumpframe,
 * --perfwin, the texture cache's validation epoch and every log line agree
 * with the run being continued rather than with this process's age. */
void gl13_set_frame_number(unsigned n) { frame_no = n; }

void gl13_snap_register(void) {
    port_snap_register("gl13.frame_no", &frame_no, sizeof(frame_no));
}

void gl13_state_report(void) {
    unsigned e, l;
    glc_stats(&e, &l);
    if (!e && !l) {
        return;
    }
    port_log("port> GL state: %u calls emitted, %u elided (%.1f%% of %u)\n", e, l,
             (e + l) ? 100.0 * (double)l / (double)(e + l) : 0.0, e + l);
}
