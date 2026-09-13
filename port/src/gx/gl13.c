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

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
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
#ifndef GL_MODULATE_ADD_ATI
#define GL_MODULATE_ADD_ATI 0x8744
#define GL_MODULATE_SIGNED_ADD_ATI 0x8745
#define GL_MODULATE_SUBTRACT_ATI 0x8746
#endif

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
    report_caps();
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

int gl13_live(void) { return gl_on; }


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
    GL(glViewport)((GLint)gx.vp[0], (GLint)(EFB_H - (gx.vp[1] + gx.vp[3])),
                   (GLsizei)gx.vp[2], (GLsizei)gx.vp[3]);
    GL(glDepthRange)(gx.vp[4], gx.vp[5]);
    GL(glScissor)((GLint)gx.scissor[0],
                  (GLint)(EFB_H - (GLint)(gx.scissor[1] + gx.scissor[3])),
                  (GLsizei)gx.scissor[2], (GLsizei)gx.scissor[3]);

    /* GX's front face is the opposite of GL's default. */
    switch (gx.cull) {
        case GX_CULL_NONE:
            GL(glDisable)(GL_CULL_FACE);
            break;
        case GX_CULL_FRONT:
            GL(glEnable)(GL_CULL_FACE);
            GL(glCullFace)(GL_BACK);
            break;
        case GX_CULL_BACK:
            GL(glEnable)(GL_CULL_FACE);
            GL(glCullFace)(GL_FRONT);
            break;
        default:
            GL(glEnable)(GL_CULL_FACE);
            GL(glCullFace)(GL_FRONT_AND_BACK);
            break;
    }
    GL(glFrontFace)(GL_CCW);

    if (gx.z_enable) {
        GL(glEnable)(GL_DEPTH_TEST);
        GL(glDepthFunc)(gl_compare(gx.z_func));
    } else {
        GL(glDisable)(GL_DEPTH_TEST);
    }
    GL(glDepthMask)(gx.z_update ? GL_TRUE : GL_FALSE);
    GL(glColorMask)(gx.color_update ? GL_TRUE : GL_FALSE,
                    gx.color_update ? GL_TRUE : GL_FALSE,
                    gx.color_update ? GL_TRUE : GL_FALSE,
                    gx.alpha_update ? GL_TRUE : GL_FALSE);

    if (gx.blend_mode == GX_BM_BLEND || gx.blend_mode == GX_BM_SUBTRACT) {
        GL(glEnable)(GL_BLEND);
        if (gx.blend_mode == GX_BM_SUBTRACT) {
            /* GX_BM_SUBTRACT is dst - src with both factors one. */
            GL(glBlendFunc)(GL_ONE, GL_ONE);
            if (gl13_have_blend_subtract) {
                GL(glBlendEquation)(GL_FUNC_REVERSE_SUBTRACT);
            }
        } else {
            if (gl13_have_blend_subtract) {
                GL(glBlendEquation)(GL_FUNC_ADD);
            }
            GL(glBlendFunc)(gl_blend_src(gx.blend_src), gl_blend_dst(gx.blend_dst));
        }
    } else {
        GL(glDisable)(GL_BLEND);
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
            } else if (c1 != GX_ALWAYS) {
                gx_warn("GXSetAlphaCompare: two live AND comparisons, the first is used");
            }
        } else if (gx.alpha_op == GX_AOP_OR) {
            if (c0 == GX_ALWAYS || c1 == GX_ALWAYS) {
                pass_all = 1;
            } else if (c0 == GX_NEVER) {
                use = c1;
                ref = gx.alpha_ref1;
            } else if (c1 != GX_NEVER) {
                gx_warn("GXSetAlphaCompare: two live OR comparisons, the first is used");
            }
        } else {
            gx_warn("GXSetAlphaCompare: XOR/XNOR is reduced to its first test");
        }
        if (pass_all || use == GX_ALWAYS) {
            GL(glDisable)(GL_ALPHA_TEST);
        } else {
            GL(glEnable)(GL_ALPHA_TEST);
            GL(glAlphaFunc)(gl_compare(use), ref / 255.0f);
        }
    }

    /* Fog.  Nine sites in the whole game, all of them linear or exponential
     * in eye z, which is what GL_FOG is. */
    if (gx.fog_type == GX_FOG_NONE) {
        GL(glDisable)(GL_FOG);
    } else {
        GLfloat c[4];
        c[0] = gx.fog_color.r / 255.0f;
        c[1] = gx.fog_color.g / 255.0f;
        c[2] = gx.fog_color.b / 255.0f;
        c[3] = gx.fog_color.a / 255.0f;
        GL(glEnable)(GL_FOG);
        GL(glFogfv)(GL_FOG_COLOR, c);
        switch (gx.fog_type & 7) {
            case 4: /* EXP  */
                GL(glFogi)(GL_FOG_MODE, GL_EXP);
                GL(glFogf)(GL_FOG_DENSITY, 1.0f / (gx.fog_endz - gx.fog_startz + 1.0f));
                break;
            case 5: /* EXP2 */
                GL(glFogi)(GL_FOG_MODE, GL_EXP2);
                GL(glFogf)(GL_FOG_DENSITY, 1.0f / (gx.fog_endz - gx.fog_startz + 1.0f));
                break;
            default:
                GL(glFogi)(GL_FOG_MODE, GL_LINEAR);
                GL(glFogf)(GL_FOG_START, gx.fog_startz);
                GL(glFogf)(GL_FOG_END, gx.fog_endz);
                break;
        }
    }
}

/* ---- transform ------------------------------------------------------------ */
/* GX's projection maps eye z to [-1, 0] and GL's to [-1, 1], so every
 * projection gets one extra row operation: z' = 2z + w.  GX's 3x4 matrices
 * are row major and GL's are column major, so the load transposes. */

void gl13_apply_transform(void) {
    GLfloat m[16];
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
        m[10] = 2.0f * p[4] + 1.0f; /* [-1,0] -> [-1,1] */
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
    GL(glMatrixMode)(GL_PROJECTION);
    GL(glLoadMatrixf)(m);

    /* The modelview is identity: gx_draw.c transforms positions and normals on
     * the CPU with the loaded position/normal matrices, because the game loads
     * normal matrices that are not the inverse transpose of the position
     * matrix and GL would compute its own. */
    GL(glMatrixMode)(GL_MODELVIEW);
    GL(glLoadIdentity)();
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
    frame_no++;
    if (frame_wanted(frame_no)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame-%05u.ppm",
                 port_opt.shotdir ? port_opt.shotdir : ".", frame_no);
        gl13_write_ppm(path);
    }
    if (pending_shot) {
        gl13_write_ppm(pending_shot);
        pending_shot = NULL;
    }
    if (!gl_on) {
        return;
    }
    SDL_GL_SwapWindow(window);
    if (clear_pending) {
        clear_pending = 0;
        gl13_clear(clear_color, clear_z);
    }
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
