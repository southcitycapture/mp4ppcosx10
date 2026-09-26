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
#include <sys/stat.h>
#include <string.h>
#include "gx_skin.h"

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#if defined(__APPLE__) && defined(__ppc__)
#include <OpenGL/OpenGL.h>
#endif
#include "gx_rt.h" /* M27: every gl* below is the render thread's twin */
#endif

int gl13_have_combine3 = 1;
int gl13_have_crossbar = 1;
int gl13_have_s3tc = 1;
int gl13_have_blend_subtract = 1;
int gl13_have_depth_texture = 1;
int gl13_have_tfs = 0; /* M35: GL_ATI_text_fragment_shader, off until the string says so */
int gl13_max_tex_units = 6; /* the Radeon 9000's number, not the host's */

#define EFB_W 640
#define EFB_H 480

#ifndef PORT_NO_SDL
static SDL_Window* window;
static SDL_GLContext ctx;
#endif
int gl13_on; /* M43: gl13_state.c reads it too */
#define gl_on gl13_on
static unsigned frame_no;
static const char* pending_shot;
static int title_plain; /* M25: the window title is the verdict until the first present */
static void fs_setup(void); /* M25: --fullscreen, below gl13_present */

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
    "glLineWidth", /* M30: GXSetLineWidth (PLAN.md 45) */
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
/* M40 (PLAN.md 55): `bytes` is the whole range handed to the driver, the
 * ring's `ring_bytes` first and the static-geometry cache's region after it;
 * the fences' chunks cover the ring alone (the cache's region is written only
 * where nothing reads it, and reset behind a finish). */
/* M40: the vertex cache's buffer object (--vcache ... --vcachevbo): made
 * here, on the main thread before the render thread owns GL, the size of
 * the cache's region plus a 64-byte head so no pointer into it is NULL */
static unsigned gl13_vbo;
unsigned gl13_vc_vbo(void) { return gl13_vbo; }
static void vbo_create(size_t bytes) {
#ifndef PORT_NO_SDL
    typedef void (*gen_t)(GLsizei, GLuint*);
    typedef void (*bind_t)(GLenum, GLuint);
    typedef void (*data_t)(GLenum, long, const void*, GLenum);
    const char* ext = (const char*)GL(glGetString)(GL_EXTENSIONS);
    gen_t gen;
    bind_t bind;
    data_t data;
    GLuint id = 0;
    if (!ext || !strstr(ext, "GL_ARB_vertex_buffer_object")) {
        port_log("port> vcache: GL_ARB_vertex_buffer_object absent: the region stays in the "
                 "vertex range\n");
        return;
    }
    gen = (gen_t)SDL_GL_GetProcAddress("glGenBuffersARB");
    bind = (bind_t)SDL_GL_GetProcAddress("glBindBufferARB");
    data = (data_t)SDL_GL_GetProcAddress("glBufferDataARB");
    if (!gen || !bind || !data) {
        return;
    }
    gen(1, &id);
    bind(0x8892 /* GL_ARRAY_BUFFER_ARB */, id);
    data(0x8892, (long)(bytes + 64), NULL, 0x88E4 /* GL_STATIC_DRAW_ARB */);
    bind(0x8892, 0);
    gl13_vbo = id;
    port_log("port> vcache: a %lu KB GL_STATIC_DRAW_ARB buffer object (%u) holds the region\n",
             (unsigned long)(bytes / 1024), (unsigned)id);
#else
    (void)bytes;
#endif
}

u8* gl13_var_setup(size_t bytes, size_t ring_bytes) {
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
    var_ring_bytes = ring_bytes ? ring_bytes : bytes;
    if (port_opt.vcache_vbo && bytes > var_ring_bytes && gl_on) {
        vbo_create(bytes - var_ring_bytes);
    }
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
        port_log("port> vertex ring: %lu KB in %d chunks (+%lu KB static-geometry cache), "
                 "vertex_array_range %s, multi_draw_arrays %s\n",
                 (unsigned long)(var_ring_bytes / 1024), VAR_CHUNKS,
                 (unsigned long)((bytes - var_ring_bytes) / 1024),
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
            /* M27: first the render thread must have *issued* the draws that
             * read the chunk's last contents (rt_ring_enter waits for the
             * stream position rt_ring_left recorded), then the GPU must have
             * finished them (the fence, tested and finished on the replaying
             * side, counted there) */
            rt_ring_enter(c);
            rt_ext_wait_fence(var_fences[c], c);
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
    rt_ext_flush_var((GLsizei)len, p);
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
            rt_ext_set_fence(var_fences[c], c);
            var_fence_valid[c] = 1;
            var_sets++;
        }
        c0 = 0;
    }
    for (c = c0; c < c1; c++) {
        rt_ext_set_fence(var_fences[c], c);
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
    if (gl13_trace_armed()) {
        int i, tot = 0;
        for (i = 0; i < n; i++) {
            tot += count[i];
        }
        port_log("gltrace> glMultiDrawArraysEXT mode %04x n %d first %d.. count %d.. total %d\n", mode,
                 n, n ? first[0] : -1, n ? count[0] : -1, tot);
    }
    rt_ext_multi_draw_arrays((GLenum)mode, (const GLint*)first, (const GLsizei*)count,
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
        if (strstr(ext, "GL_ATI_text_fragment_shader")) {
            gl13_have_tfs = 1; /* M35: the indirect warp (gx_tfs.c) */
        }
    }
    port_log("port> GL feature set used: combine3 %d, crossbar %d, s3tc %d, "
             "blend_subtract %d, depth_texture %d, text_fragment_shader %d, %d units\n",
             gl13_have_combine3, gl13_have_crossbar, gl13_have_s3tc,
             gl13_have_blend_subtract, gl13_have_depth_texture, gl13_have_tfs, gl13_max_tex_units);
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
    /* M32: SDL 2.0.3 minimises a fullscreen window when the application
     * loses focus (Dashboard on F12, a Cmd-Tab, a dialog) and nothing on
     * Leopard brings it back but the Dock; the M32 walk lost the picture to
     * Dashboard that way.  The window is a desktop-sized borderless one
     * (SDL_WINDOW_FULLSCREEN_DESKTOP), so leaving it up costs nothing. */
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    /* No core profile: the whole backend is fixed function, which is what the
     * Radeon 9000 has and all it has. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    /* M25: the machine check's verdict is the title until the first drawn
     * frame reaches the screen (gl13_present restores the plain name). */
    window = SDL_CreateWindow(port_machine_title(), SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, EFB_W * scale, EFB_H * scale,
                              SDL_WINDOW_OPENGL |
                              (port_opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
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
    if (port_opt.fullscreen) {
        fs_setup();
    }
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
    if (port_opt.tfsprobe) {
        gx_tfs_probe(); /* M35: prints and quits */
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

/* M27: hand GL to the render thread (or to the inline replay).  After
 * gl13_init and port_workers_init, before the game: everything GL that had
 * to happen on the main thread has (the probes, the fences, the ring's VAR
 * setup, the white texture is lazy and goes through the door), and the two
 * lazy probes that would call glGetString at run time are answered now. */
void gx_draw_ring_ensure(void);
void gl13_rt_start(void) {
#ifndef PORT_NO_SDL
    if (!gl_on) {
        return;
    }
    gx_draw_ring_ensure();
    glc_fogcoord_available();
    glc_white_texture();
    rt_start(window, ctx);
#endif
}

void gl13_shutdown(void) {
    rt_stop(); /* M27: drain the stream, take the context back */
    glc_forget_white(); /* dies with the context */
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

/* M32: the same frame as a PNG (png_write.c) -- the F12 screenshot's format,
 * a file the Finder previews; the lab's PPMs stay PPMs. */
void gl13_write_png(const char* path) {
#ifndef PORT_NO_SDL
    unsigned char* buf;
    int w = EFB_W, h = EFB_H;
    if (!gl_on) {
        return;
    }
    buf = (unsigned char*)malloc((size_t)w * h * 3);
    if (!buf) {
        return;
    }
    GL(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
    GL(glReadPixels)(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    if (port_write_png(path, w, h, buf)) {
        port_log("port> screenshot: wrote %s\n", path);
    } else {
        port_log("port> screenshot: cannot write %s\n", path);
    }
    free(buf);
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

/* ---- --fullscreen (M25, PLAN.md 40.4) ------------------------------------
 *
 * The renderer draws the game exactly as in the window: 640x480 at 1:1 in
 * the bottom-left of the back buffer, every viewport, scissor, EFB copy and
 * read-back in EFB coordinates.  Fullscreen is a *present* step: the corner
 * is copied into a texture, the screen cleared to black and the texture
 * drawn scaled to the largest 4:3 rectangle that fits (letterboxed), and
 * after the swap the texture is drawn back 1:1 into the corner, so the back
 * buffer's corner holds the last drawn frame as the front buffer does in a
 * window -- which is where a consumed frame's GXCopyTex reads from
 * (gx_tex.c reads GL_BACK under fullscreen).  Nothing the game's frame
 * touches changes, so --dumpframe still reads the 1:1 corner before the
 * blit and the md5s hold. */
static int fs_on;
static int fs_w, fs_h;          /* the drawable */
static int fs_x, fs_y, fs_rw, fs_rh; /* the letterboxed rectangle */
static GLuint fs_tex;
#define FS_TEX_W 1024
#define FS_TEX_H 512

int gl13_fullscreen(void) { return fs_on; }

static void fs_setup(void) {
#ifndef PORT_NO_SDL
    double sx, sy, sc;
    SDL_GL_GetDrawableSize(window, &fs_w, &fs_h);
    if (fs_w < EFB_W || fs_h < EFB_H) {
        port_log("port> --fullscreen: the drawable is %dx%d, under 640x480; staying 1:1\n",
                 fs_w, fs_h);
        return;
    }
    sx = (double)fs_w / EFB_W;
    sy = (double)fs_h / EFB_H;
    sc = sx < sy ? sx : sy;
    fs_rw = (int)(EFB_W * sc + 0.5);
    fs_rh = (int)(EFB_H * sc + 0.5);
    fs_x = (fs_w - fs_rw) / 2;
    fs_y = (fs_h - fs_rh) / 2;
    GL(glGenTextures)(1, &fs_tex);
    GL(glBindTexture)(GL_TEXTURE_2D, fs_tex);
    GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGB8, FS_TEX_W, FS_TEX_H, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, NULL);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL(glBindTexture)(GL_TEXTURE_2D, 0);
    SDL_ShowCursor(SDL_DISABLE);
    fs_on = 1;
    port_log("port> --fullscreen: %dx%d, the picture at %dx%d from (%d,%d), scale %.3f\n",
             fs_w, fs_h, fs_rw, fs_rh, fs_x, fs_y, sc);
#endif
}

/* the state a blit needs; the shadow forgets it all afterwards */
static void fs_quad_begin(int vw, int vh) {
    int i;
    for (i = 5; i >= 0; i--) {
        GL(glActiveTexture)(GL_TEXTURE0 + i);
        if (i) {
            GL(glDisable)(GL_TEXTURE_2D);
        }
    }
    GL(glEnable)(GL_TEXTURE_2D);
    GL(glBindTexture)(GL_TEXTURE_2D, fs_tex);
    GL(glTexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    GL(glDisable)(GL_BLEND);
    GL(glDisable)(GL_DEPTH_TEST);
    GL(glDepthMask)(GL_FALSE);
    GL(glDisable)(GL_ALPHA_TEST);
    GL(glDisable)(GL_CULL_FACE);
    GL(glDisable)(GL_FOG);
    GL(glDisable)(GL_LIGHTING);
    GL(glDisable)(GL_COLOR_SUM);
    GL(glDisable)(GL_SCISSOR_TEST);
    GL(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GL(glViewport)(0, 0, vw, vh);
    GL(glMatrixMode)(GL_TEXTURE);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
    GL(glOrtho)(0.0, (double)vw, 0.0, (double)vh, -1.0, 1.0);
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glPushMatrix)();
    GL(glLoadIdentity)();
}

static void fs_quad(int x, int y, int w, int h) {
    const float su = (float)EFB_W / FS_TEX_W, sv = (float)EFB_H / FS_TEX_H;
    GL(glBegin)(GL_TRIANGLE_STRIP);
    GL(glTexCoord2f)(0.0f, 0.0f);
    GL(glVertex2f)((float)x, (float)y);
    GL(glTexCoord2f)(su, 0.0f);
    GL(glVertex2f)((float)(x + w), (float)y);
    GL(glTexCoord2f)(0.0f, sv);
    GL(glVertex2f)((float)x, (float)(y + h));
    GL(glTexCoord2f)(su, sv);
    GL(glVertex2f)((float)(x + w), (float)(y + h));
    GL(glEnd)();
}

static void fs_quad_end(void) {
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_TEXTURE);
    GL(glPopMatrix)();
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glDepthMask)(GL_TRUE);
    GL(glEnable)(GL_SCISSOR_TEST);
    glc_invalidate();
}

/* before the swap: the corner into the texture, the screen black, the
 * letterboxed picture */
static void fs_blit_out(void) {
    gx_vprog_disable();
    fs_quad_begin(fs_w, fs_h);
    GL(glCopyTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 0, 0, EFB_W, EFB_H);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL(glClearColor)(0.0f, 0.0f, 0.0f, 1.0f);
    GL(glClear)(GL_COLOR_BUFFER_BIT);
    fs_quad(fs_x, fs_y, fs_rw, fs_rh);
    fs_quad_end();
}

/* after the swap: the last drawn frame back into the corner at 1:1 */
static void fs_blit_back(void) {
    fs_quad_begin(EFB_W, EFB_H);
    /* nearest at 1:1: the texel itself, no filter rounding, so the corner
     * is the drawn frame's bytes (RGB; the alpha plane comes back opaque) */
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    fs_quad(0, 0, EFB_W, EFB_H);
    fs_quad_end();
}

/* M32: a quit from the window (Cmd-Q, Escape, the close button) goes through
 * the game's own reset path (sreset_poll.c: the fade, the save, OSResetSystem
 * -> port_shutdown), which is what a console's Reset button does.  If the
 * game has not got there within 300 drawn frames (ten seconds at 30 fps: a
 * screen whose loop never polls the watcher), a second ask leaves directly
 * through port_shutdown, which still flushes the card image and writes the
 * reports. */
static unsigned quit_asked_at;
static void gl13_quit_asked(void) {
    if (port_reset_requested() && quit_asked_at && frame_no > quit_asked_at + 300) {
        port_log("port> quit: the game's reset path did not finish in %u frames; "
                 "leaving directly (the card image is flushed on the way out)\n",
                 frame_no - quit_asked_at);
        port_shutdown(0);
    }
    if (!quit_asked_at) {
        quit_asked_at = frame_no ? frame_no : 1;
    }
    port_request_reset();
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
        gl13_write_png(pending_shot);
        pending_shot = NULL;
    }
    if (!gl_on || draw_off) {
        return; /* nothing was drawn, so there is nothing to show */
    }
    if (fs_on) {
        fs_blit_out();
    }
    rt_present(frame_no); /* M27: SDL_GL_SwapWindow on the GL thread */
    rt_halfwatch_report(0); /* M45 */
    if (fs_on) {
        fs_blit_back();
    }
    if (!title_plain) {
        /* the first drawn frame is on screen: the machine check's verdict
         * has had its say in the title bar */
        title_plain = 1;
        SDL_SetWindowTitle(window, "Mario Party 4");
    }
    /* The clear the game asked for is run by gl13_begin_frame(), which the
     * gate calls once it knows the next frame is drawn -- under --realtime a
     * consumed frame in between may have asked for a different colour, and
     * the clear that precedes a drawn frame has to be the *latest* one. */
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                gl13_quit_asked(); /* Cmd-Q, the red button, the Dock's Quit */
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_q && (e.key.keysym.mod & KMOD_GUI)) {
                    /* Cmd-Q as a key, for a session where the application
                     * menu's Quit is not the route (M32: System Events on
                     * the MacBook delivered the keystroke and no SDL_QUIT) */
                    gl13_quit_asked();
                    continue;
                }
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        gl13_quit_asked();
                        break;
                    case SDLK_F5:
                    case SDLK_F12: {
                        /* M32: a PNG on the Desktop, numbered by the drawn
                         * frame, so a player's bug report can carry a
                         * picture (--shotdir points it elsewhere).  F5 as
                         * well as F12 because Leopard gives F12 to Dashboard
                         * by default and the key never reaches the game. */
                        static char path[1024];
                        const char* home = getenv("HOME");
                        if (port_opt.shotdir) {
                            snprintf(path, sizeof(path), "%s/Mario Party 4 %05u.png",
                                     port_opt.shotdir, frame_no);
                        } else {
                            char desk[1024];
                            snprintf(desk, sizeof(desk), "%s/Desktop", home && *home ? home : ".");
                            mkdir(desk, 0755); /* a test user's home may lack one */
                            snprintf(path, sizeof(path), "%s/Mario Party 4 %05u.png", desk,
                                     frame_no);
                        }
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
