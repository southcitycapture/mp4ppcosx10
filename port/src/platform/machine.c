/* M25: the machine check.
 *
 * The port has run on exactly one PowerPC machine -- a dual 1 GHz Power Mac
 * G4 (PowerMac3,5) with a 64 MB Radeon 9000 under 10.5.4 -- and every
 * extension the renderer leans on was read off that card's list
 * (docs/g4-glinfo.log) and never asked for again.  Before the window opens
 * this file asks the machine it is actually on the same questions and says
 * what the answers mean:
 *
 *   (a) the inventory: model, CPUs and clock, CPU family and AltiVec, RAM,
 *       the OS, the GL renderer/vendor/version, the VRAM, the texture-unit
 *       count and every extension in the table below -- on the log, and the
 *       verdict word on the status line;
 *   (b) the verdict: `ok` / `degraded` / `unsupported`, one line per reason.
 *       Required is what the game cannot be drawn correctly without: the
 *       combiner extensions, the crossbar, the colour sum, four texture
 *       units.  Degraded is anything the port has a fallback for: no vertex
 *       program (phase 2 on the CPU, --cpuxf), no vertex array range (client
 *       arrays), one CPU (--threads 0), less VRAM (a smaller --texbudget),
 *       a slow clock.  Unsupported is a required extension missing, an OS
 *       older than the binary's floor, or not PowerPC at all -- which under
 *       Rosetta is said plainly.  `--machinecheck` prints it and exits
 *       0/1/2; `unsupported` refuses to run without `--force`;
 *   (c) the settings the verdict implies, applied unless the flag was given
 *       explicitly, and printed as `machine: applying --texbudget 24 (VRAM
 *       32 MB)` so a user's log always says why.
 *
 * `--fake-machine FILE` overrides any probe with `key=value` lines (the keys
 * are the members of PortMachine, named in machine_fake_load), so the three
 * machines the user does not own can be argued on the one he does.
 *
 * The extension table is derived from the code, not from memory: every
 * `strstr(ext, ...)` and every `GL_*` token gl13.c / gx_*.c use, with what
 * each one gates (gl13_have_*, vpl.have, var_on, var_multidraw, the colour
 * sum).  Keep it in step with gl13.c when a new extension is used.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <mach/machine.h>
#endif
#if defined(__APPLE__) && !defined(PORT_NO_SDL)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/CGLRenderers.h>
#include <CoreServices/CoreServices.h>
/* Leopard's Radeon 9000 driver answers kCGLCPCurrentRendererID 0x00001602
 * where the renderer table lists 0x00021602 (the 0x20000 bit is set in the
 * table only), so the match is on the low sixteen bits -- the vendor and
 * family -- rather than 10.5's kCGLRendererIDMatchingMask (0x00FE7F00),
 * which the 10.4u SDK does not define anyway. */
#define MACH_RID_MASK 0x0000FFFFL
#endif

/* ---- the inventory ----------------------------------------------------- */

#define MACH_EXT_MAX 8192

typedef struct PortMachine {
    char model[64];    /* hw.model                                          */
    int ncpu;          /* hw.ncpu                                           */
    unsigned mhz;      /* hw.cpufrequency / 1e6 (hw.clockrate on old kernels) */
    int cputype;       /* hw.cputype: 18 = PowerPC, 7 = x86                 */
    int cpusubtype;    /* hw.cpusubtype: 9 = 750, 10 = 7400, 11 = 7450, 100 = 970 */
    int altivec;       /* hw.optional.altivec / hw.vectorunit               */
    unsigned ram_mb;   /* hw.memsize (hw.physmem on old kernels)            */
    int os_major, os_minor, os_bugfix; /* Gestalt sys1/sys2/sys3            */
    int native;        /* sysctl.proc_native: 0 = translated (Rosetta)      */
    int host_build;    /* not the PowerPC binary at all (TARGET=host)       */
    int gl;            /* 1 = a context was made and the strings are real   */
    char gl_vendor[128];
    char gl_renderer[128];
    char gl_version[64];
    char gl_extensions[MACH_EXT_MAX];
    int vram_mb;       /* kCGLRPVideoMemory of the renderer in use; -1 unknown */
    int texunits;      /* GL_MAX_TEXTURE_UNITS                              */
    int maxtexsize;    /* GL_MAX_TEXTURE_SIZE                               */
    int faked;         /* --fake-machine overrode at least one probe        */
} PortMachine;

static PortMachine mach;

/* The extension table.  `need` is what the absence means to the picture:
 *   REQ  the game cannot be drawn correctly without it: unsupported
 *   DEG  a fallback exists and is named: degraded
 *   OPT  the port resolves it at run time and has nothing to lose without it
 *   NOT  on the G4's list, and the port does not use it (for the record)
 * `also` is an alternative spelling that satisfies the same need (none
 * today: the probes mirror gl13.c's strstr calls exactly). */
enum { NEED_REQ, NEED_DEG, NEED_OPT, NEED_NOT };

typedef struct MachExt {
    const char* name;
    const char* also;
    int need;
    const char* what;   /* what the port does with it */
    const char* without; /* the fallback (DEG) or the consequence (REQ) */
} MachExt;

static const MachExt exts[] = {
    { "GL_ARB_multitexture", NULL, NEED_REQ,
      "one texture unit per TEV stage (gl13.c glActiveTexture)",
      "no way to run more than one TEV stage" },
    { "GL_ARB_texture_env_combine", NULL, NEED_REQ,
      "the combiner every stage is emitted as (gx_tev.c GL_COMBINE_RGB/ALPHA)",
      "no combiner: every stage a plain modulate" },
    { "GL_ARB_texture_env_crossbar", NULL, NEED_REQ,
      "a unit reading another unit's texture: the register rewrites (gx_tev.c "
      "regfix/regfix2/regfix3 -- the board eyes, the walkway, the portraits)",
      "every GX_TEVREG write folded to PREV: the board eyes and walkway wrong "
      "(PLAN.md 31.3), the results portraits white" },
    { "GL_ATI_texture_env_combine3", NULL, NEED_REQ,
      "MODULATE_ADD_ATI = A*C + B (gx_tev.c): the X*Y+Z stage shape and the "
      "textured highlight (gx_draw.c hilite mode 2)",
      "four-input stages drawn as their d term; every title-screen character "
      "washed from the diffuse channel" },
    { "GL_EXT_secondary_color", NULL, NEED_REQ,
      "GL_COLOR_SUM (0x8458): the specular channel folded in after the units "
      "(gl13.c glc_color_sum, gx_vprog.c)",
      "every shiny (hilite) material white: Stamp Out!'s toys, the cake, Toad" },
    { "GL_ARB_vertex_program", NULL, NEED_DEG,
      "phase 2 -- transform, lighting, texgen -- on the vertex unit (gx_vprog.c)",
      "phase 2 on the CPU (--cpuxf): the pre-M11 speed, hilite materials "
      "without their highlight rather than white" },
    { "GL_APPLE_vertex_array_range", NULL, NEED_DEG,
      "the 8 MB vertex ring handed to the driver as DMA storage (gl13.c gl13_var_setup)",
      "client arrays copied per draw (the --novar path)" },
    { "GL_APPLE_fence", NULL, NEED_DEG,
      "the ring's chunk fences (gl13.c)",
      "client arrays copied per draw (the --novar path; the ring needs both)" },
    { "GL_EXT_multi_draw_arrays", NULL, NEED_DEG,
      "one glMultiDrawArraysEXT per batch of strips (gl13.c)",
      "one glDrawArrays per strip (the --nomultidraw path)" },
    { "GL_EXT_blend_subtract", NULL, NEED_DEG,
      "GX_BM_SUBTRACT as glBlendEquation(GL_FUNC_REVERSE_SUBTRACT) (gl13.c, gx_state.c)",
      "subtractive blends drawn as plain blends (a gx_warn names each)" },
    { "GL_EXT_gpu_program_parameters", NULL, NEED_OPT,
      "the matrix rows in one call (--envbulk, measured and off, PLAN.md 36)",
      "one call per row, which is the default anyway" },
    { "GL_EXT_fog_coord", NULL, NEED_OPT,
      "the matrix palette's per-vertex slot (--palette, measured slower and off, PLAN.md 33)",
      "no palette, which is the default anyway" },
    { "GL_EXT_texture_compression_s3tc", NULL, NEED_OPT,
      "noted at boot (gl13_have_s3tc); CMPR textures are decoded on the CPU",
      "nothing" },
    { "GL_ARB_depth_texture", NULL, NEED_OPT,
      "noted at boot (gl13_have_depth_texture); absent on the G4 too",
      "nothing" },
    { "GL_ATI_text_fragment_shader", NULL, NEED_NOT,
      "on the G4's list; not built (PLAN.md 36.3)", "nothing" },
};

#define N_EXTS (sizeof(exts) / sizeof(exts[0]))

/* ---- the probes -------------------------------------------------------- */

#if defined(__APPLE__)
static int sysctl_int(const char* name, int dflt) {
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) {
        return dflt;
    }
    return v;
}

static unsigned long long sysctl_u64(const char* name) {
    unsigned long long v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) {
        /* a 32-bit sysctl answers with a short length */
        unsigned v32 = 0;
        len = sizeof(v32);
        if (sysctlbyname(name, &v32, &len, NULL, 0) != 0) {
            return 0;
        }
        return v32;
    }
    return v;
}

static void sysctl_str(const char* name, char* out, size_t cap) {
    size_t len = cap - 1;
    out[0] = '\0';
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) {
        out[0] = '\0';
        return;
    }
    out[len < cap ? len : cap - 1] = '\0';
}
#endif

static void probe_cpu(void) {
#if defined(__APPLE__)
    unsigned long long hz;
    sysctl_str("hw.model", mach.model, sizeof(mach.model));
    mach.ncpu = sysctl_int("hw.ncpu", 1);
    hz = sysctl_u64("hw.cpufrequency");
    if (!hz) {
        hz = sysctl_u64("hw.cpufrequency_max");
    }
    if (hz) {
        /* rounded: the G4 reports 999,999,997 Hz */
        mach.mhz = (unsigned)((hz + 500000ull) / 1000000ull);
    } else {
        mach.mhz = (unsigned)sysctl_int("hw.clockrate", 0);
    }
    mach.cputype = sysctl_int("hw.cputype", 0);
    mach.cpusubtype = sysctl_int("hw.cpusubtype", 0);
    mach.altivec = sysctl_int("hw.optional.altivec", -1);
    if (mach.altivec < 0) {
        mach.altivec = sysctl_int("hw.vectorunit", 0);
    }
    {
        unsigned long long bytes = sysctl_u64("hw.memsize");
        if (!bytes) {
            bytes = sysctl_u64("hw.physmem");
        }
        mach.ram_mb = (unsigned)(bytes / (1024 * 1024));
    }
    /* Rosetta: Apple's documented test.  A native process reads 1; a
     * translated PowerPC process on an Intel Mac reads 0 (the MacBook: 0,
     * with hw.model faked to `PowerMac' and a 7400 at 2300 MHz); a PowerPC
     * kernel has no such key at all (Leopard 10.5.4 on the G4: "top level
     * name sysctl ... is invalid"), and no translator, so absent = native. */
    mach.native = sysctl_int("sysctl.proc_native", 1);
#else
    strcpy(mach.model, "(not Mac OS X)");
    mach.ncpu = 1;
    mach.native = 1;
#endif
#if !defined(__ppc__) && !defined(__ppc64__)
    mach.host_build = 1;
#endif
}

static void probe_os(void) {
#if defined(__APPLE__) && !defined(PORT_NO_SDL)
    SInt32 v = 0;
    if (Gestalt(gestaltSystemVersionMajor, &v) == noErr) {
        mach.os_major = (int)v;
        if (Gestalt(gestaltSystemVersionMinor, &v) == noErr) {
            mach.os_minor = (int)v;
        }
        if (Gestalt(gestaltSystemVersionBugFix, &v) == noErr) {
            mach.os_bugfix = (int)v;
        }
    } else if (Gestalt(gestaltSystemVersion, &v) == noErr) {
        /* pre-10.4: 0x1039 */
        mach.os_major = (int)((v >> 12) & 0xf) * 10 + (int)((v >> 8) & 0xf);
        mach.os_minor = (int)((v >> 4) & 0xf);
        mach.os_bugfix = (int)(v & 0xf);
    }
#endif
}

/* A CGL context with no window: the same pixel-format asks as gl13_init's
 * SDL window (accelerated, double-buffered, 24-bit colour, 8 alpha, 24
 * depth), so the renderer answering here is the one the window will get.
 * CGL contexts work from an SSH session on Leopard (the M2 probe), which is
 * what lets --machinecheck run headless. */
static void probe_gl(void) {
#if defined(__APPLE__) && !defined(PORT_NO_SDL)
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFADoubleBuffer,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    CGLContextObj ctx = NULL;
    long npix = 0;
    const char* s;
    GLint v = 0;

    mach.vram_mb = -1;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) {
        return;
    }
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError || !ctx) {
        CGLDestroyPixelFormat(pf);
        return;
    }
    CGLSetCurrentContext(ctx);
    s = (const char*)glGetString(GL_VENDOR);
    snprintf(mach.gl_vendor, sizeof(mach.gl_vendor), "%s", s ? s : "?");
    s = (const char*)glGetString(GL_RENDERER);
    snprintf(mach.gl_renderer, sizeof(mach.gl_renderer), "%s", s ? s : "?");
    s = (const char*)glGetString(GL_VERSION);
    snprintf(mach.gl_version, sizeof(mach.gl_version), "%s", s ? s : "?");
    s = (const char*)glGetString(GL_EXTENSIONS);
    snprintf(mach.gl_extensions, sizeof(mach.gl_extensions), "%s", s ? s : "");
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &v);
    mach.texunits = (int)v;
    v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    mach.maxtexsize = (int)v;
    mach.gl = 1;
    /* VRAM: the renderer this context landed on, looked up in the renderer
     * table by id (MACH_RID_MASK above).  If the id does not match anything
     * (the first G4 run read -1) the fallback is the accelerated renderer
     * with the most memory; the table is printed under --machinecheck. */
    {
        long rid = 0;
        int have_rid = CGLGetParameter(ctx, kCGLCPCurrentRendererID, &rid) == kCGLNoError;
        CGLRendererInfoObj info = NULL;
        long n = 0, i;
        long best_mb = -1;
        if (CGLQueryRendererInfo(0xffffffffUL, &info, &n) == kCGLNoError && info) {
            for (i = 0; i < n; i++) {
                long id = 0, mb = 0, acc = 0, tex = 0;
                if (CGLDescribeRenderer(info, i, kCGLRPRendererID, &id) != kCGLNoError) {
                    continue;
                }
                CGLDescribeRenderer(info, i, kCGLRPAccelerated, &acc);
                CGLDescribeRenderer(info, i, kCGLRPVideoMemory, &mb);
                CGLDescribeRenderer(info, i, kCGLRPTextureMemory, &tex);
                if (port_opt.machinecheck) {
                    port_log("port> machine: renderer %ld: id 0x%08lx accelerated %ld video %ld MB "
                             "texture %ld MB%s\n", i, id, acc, mb / (1024 * 1024),
                             tex / (1024 * 1024),
                             have_rid && (id & MACH_RID_MASK) ==
                                             (rid & MACH_RID_MASK)
                                 ? " (this context)" : "");
                }
                if (have_rid && (id & MACH_RID_MASK) ==
                                    (rid & MACH_RID_MASK)) {
                    mach.vram_mb = (int)(mb / (1024 * 1024));
                } else if (acc && mb / (1024 * 1024) > best_mb) {
                    best_mb = mb / (1024 * 1024);
                }
            }
            CGLDestroyRendererInfo(info);
        }
        if (mach.vram_mb < 0 && best_mb >= 0) {
            mach.vram_mb = (int)best_mb;
        }
        if (port_opt.machinecheck) {
            port_log("port> machine: kCGLCPCurrentRendererID %s 0x%08lx\n",
                     have_rid ? "=" : "not answered;", rid);
        }
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pf);
#else
    mach.vram_mb = -1;
#endif
}

/* ---- --fake-machine ---------------------------------------------------- */

static void fake_set(const char* k, const char* v) {
    mach.faked = 1;
    if (!strcmp(k, "model")) {
        snprintf(mach.model, sizeof(mach.model), "%s", v);
    } else if (!strcmp(k, "ncpu")) {
        mach.ncpu = atoi(v);
    } else if (!strcmp(k, "mhz")) {
        mach.mhz = (unsigned)atoi(v);
    } else if (!strcmp(k, "cputype")) {
        mach.cputype = atoi(v);
    } else if (!strcmp(k, "cpusubtype")) {
        mach.cpusubtype = atoi(v);
    } else if (!strcmp(k, "altivec")) {
        mach.altivec = atoi(v);
    } else if (!strcmp(k, "ram_mb")) {
        mach.ram_mb = (unsigned)atoi(v);
    } else if (!strcmp(k, "os")) {
        mach.os_major = mach.os_minor = mach.os_bugfix = 0;
        sscanf(v, "%d.%d.%d", &mach.os_major, &mach.os_minor, &mach.os_bugfix);
    } else if (!strcmp(k, "native")) {
        mach.native = atoi(v);
    } else if (!strcmp(k, "host_build")) {
        mach.host_build = atoi(v);
    } else if (!strcmp(k, "gl")) {
        mach.gl = atoi(v);
    } else if (!strcmp(k, "gl_vendor")) {
        snprintf(mach.gl_vendor, sizeof(mach.gl_vendor), "%s", v);
    } else if (!strcmp(k, "gl_renderer")) {
        snprintf(mach.gl_renderer, sizeof(mach.gl_renderer), "%s", v);
    } else if (!strcmp(k, "gl_version")) {
        snprintf(mach.gl_version, sizeof(mach.gl_version), "%s", v);
    } else if (!strcmp(k, "gl_extensions")) {
        snprintf(mach.gl_extensions, sizeof(mach.gl_extensions), "%s", v);
        mach.gl = 1;
    } else if (!strcmp(k, "vram_mb")) {
        mach.vram_mb = atoi(v);
    } else if (!strcmp(k, "texunits")) {
        mach.texunits = atoi(v);
    } else if (!strcmp(k, "maxtexsize")) {
        mach.maxtexsize = atoi(v);
    } else {
        port_log("port> machine: --fake-machine: unknown key `%s' ignored\n", k);
    }
}

static void machine_fake_load(const char* path) {
    FILE* f = fopen(path, "r");
    char line[MACH_EXT_MAX + 64];
    int n = 0;
    if (!f) {
        port_fatal("--fake-machine: cannot open %s", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        char* eq;
        char* end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
        eq = strchr(p, '=');
        if (!eq) {
            port_log("port> machine: --fake-machine: line without `=' ignored: %s\n", p);
            continue;
        }
        *eq = '\0';
        end = eq;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        eq++;
        while (*eq == ' ' || *eq == '\t') {
            eq++;
        }
        fake_set(p, eq);
        n++;
    }
    fclose(f);
    port_log("port> machine: --fake-machine %s: %d probe(s) overridden\n", path, n);
}

/* ---- the reading ------------------------------------------------------- */

static int has_ext(const char* name) {
    /* whole-token match: GL_ARB_texture_env_combine must not match
     * GL_ARB_texture_env_combine3's prefix (and vice versa) */
    const char* p = mach.gl_extensions;
    size_t n = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        int at_start = p == mach.gl_extensions || p[-1] == ' ';
        int at_end = p[n] == '\0' || p[n] == ' ';
        if (at_start && at_end) {
            return 1;
        }
        p += n;
    }
    return 0;
}

static int ext_present(const MachExt* e) {
    return has_ext(e->name) || (e->also && has_ext(e->also));
}

static const char* cpu_family(void) {
    if (mach.cputype == 18 /* CPU_TYPE_POWERPC */ || mach.cputype == 0) {
        switch (mach.cpusubtype) {
        case 9: return "750 (G3)";
        case 10: return "7400 (G4)";
        case 11: return "7450 (G4)";
        case 100: return "970 (G5)";
        default: return mach.cputype == 18 ? "PowerPC" : "?";
        }
    }
    if (mach.cputype == 7) {
        return "x86";
    }
    if (mach.cputype == 12) {
        return "ARM";
    }
    return "?";
}

/* The verdict, its reasons, and the settings it implies. */
enum { V_OK = 0, V_DEGRADED = 1, V_UNSUPPORTED = 2 };

static int verdict;
static char reasons[16][400];
static int nreasons;
static char title[160];
static char summary[200];

/* the settings the verdict implies; -1 = leave alone */
static int want_threads = -1;
static int want_texbudget = -1;
static int want_cpuxf = -1;

static void reason(int level, const char* fmt, ...) {
    va_list ap;
    if (nreasons < 16) {
        va_start(ap, fmt);
        vsnprintf(reasons[nreasons], sizeof(reasons[0]), fmt, ap);
        va_end(ap);
        nreasons++;
    }
    if (level > verdict) {
        verdict = level;
    }
}

static const char* verdict_word(void) {
    return verdict == V_OK ? "ok" : verdict == V_DEGRADED ? "degraded" : "unsupported";
}

/* The thresholds are the G4's numbers: a 1 GHz 7450 runs the board at
 * 100% speed with ~20 presented fps (PLAN.md 39.4); the process peaks at
 * 171 MB resident over a soak (PLAN.md 39.1); the texture cache's 40 MB
 * default budget was set against the 64 MB card (PLAN.md 33.1). */
#define MACH_MIN_OS_MAJOR 10
#define MACH_MIN_OS_MINOR 4     /* the binary is built -mmacosx-version-min=10.4 */
#define MACH_FULL_SPEED_MHZ 800
#define MACH_MIN_RAM_MB 256
#define MACH_FULL_VRAM_MB 64
#define MACH_TEXBUDGET_DEFAULT 40
#define MACH_MIN_TEX_UNITS 4

static void decide(void) {
    size_t i;
    verdict = V_OK;
    nreasons = 0;

    /* the CPU */
    if (mach.host_build) {
        reason(V_OK, "the host build (development): not a shipping machine, no CPU verdict");
    } else if (!mach.native) {
        reason(V_UNSUPPORTED, "not a PowerPC Mac: this is the PowerPC binary running under "
               "Rosetta (sysctl.proc_native = 0; Rosetta reports the machine as `%s'), "
               "which the port does not support; --force runs it anyway, at about half "
               "the G4's speed on the game's code",
               mach.model[0] ? mach.model : "?");
    } else if (mach.cputype != 18 && mach.cputype != 0) {
        reason(V_UNSUPPORTED, "not a PowerPC CPU (hw.cputype %d)", mach.cputype);
    } else {
        if (mach.ncpu < 2) {
            reason(V_DEGRADED, "one CPU: the mixer stays on the game thread (--threads 0), "
                   "about 1.2 ms a frame more than a dual");
            want_threads = 0;
        }
        if (mach.mhz && mach.mhz < MACH_FULL_SPEED_MHZ) {
            reason(V_DEGRADED, "%u MHz: will run below full speed (the board needs "
                   "~7 ms of game per 16.7 ms frame at 1000 MHz; expect %u%% speed "
                   "and fewer presented frames)",
                   mach.mhz, (unsigned)(mach.mhz * 100 / 1000));
        }
    }
    if (mach.ram_mb && mach.ram_mb < MACH_MIN_RAM_MB) {
        reason(V_DEGRADED, "%u MB of RAM: the process peaks at ~170 MB resident and "
               "the system will page", mach.ram_mb);
    }
    /* the OS */
    if (mach.os_major && !mach.host_build) {
        if (mach.os_major < MACH_MIN_OS_MAJOR ||
            (mach.os_major == MACH_MIN_OS_MAJOR && mach.os_minor < MACH_MIN_OS_MINOR)) {
            reason(V_UNSUPPORTED, "Mac OS X %d.%d.%d: the binary needs 10.4 or later "
                   "(-mmacosx-version-min=10.4)",
                   mach.os_major, mach.os_minor, mach.os_bugfix);
        }
    }
    /* the GL */
    if (!mach.gl) {
        if (port_opt.headless) {
            reason(V_OK, "GL: no context (headless run): the renderer was not judged");
        } else {
            reason(V_DEGRADED, "GL: no accelerated context could be made (no display, "
                   "or no accelerated renderer): the window will not open and the "
                   "run will be headless");
        }
    } else {
        for (i = 0; i < N_EXTS; i++) {
            const MachExt* e = &exts[i];
            if (ext_present(e) || e->need == NEED_NOT) {
                continue;
            }
            if (e->need == NEED_REQ) {
                reason(V_UNSUPPORTED, "%s missing: %s", e->name, e->without);
            } else if (e->need == NEED_DEG) {
                reason(V_DEGRADED, "%s missing: %s", e->name, e->without);
                if (!strcmp(e->name, "GL_ARB_vertex_program")) {
                    want_cpuxf = 1;
                }
            }
        }
        if (mach.texunits && mach.texunits < MACH_MIN_TEX_UNITS) {
            reason(V_UNSUPPORTED, "%d texture units: a stage is a unit and the game's "
                   "chains run to five stages (GX_TEVSTAGE4; the portraits' mask/reflect "
                   "triple plus its highlight); gx_tev.c drops the stages past the "
                   "card's units, so under %d the picture is wrong",
                   mach.texunits, MACH_MIN_TEX_UNITS);
        }
        if (mach.maxtexsize && mach.maxtexsize < 1024) {
            reason(V_DEGRADED, "GL_MAX_TEXTURE_SIZE %d: the game's largest textures "
                   "(1024 wide) would be refused", mach.maxtexsize);
        }
        if (mach.vram_mb == 0) {
            reason(V_DEGRADED, "VRAM: the driver reports 0 MB (software renderer?)");
        } else if (mach.vram_mb > 0 && mach.vram_mb < MACH_FULL_VRAM_MB) {
            /* three quarters of the card, rounded down to 4 MB: 32 -> 24,
             * 16 -> 12, 8 -> 4.  The framebuffer (640x480x32, double, depth
             * 24) is under 4 MB and the vertex ring lives in AGP memory. */
            int budget = (mach.vram_mb * 3 / 4) & ~3;
            if (budget < 4) {
                budget = 4;
            }
            want_texbudget = budget;
            reason(V_DEGRADED, "VRAM %d MB (under %d): the texture cache's budget "
                   "lowered to %d MB, so scene changes decode more often",
                   mach.vram_mb, MACH_FULL_VRAM_MB, budget);
        }
    }
    if (nreasons == 0) {
        reason(V_OK, "every probe within the G4's envelope");
    }
    snprintf(summary, sizeof(summary), "%s: %s, %d x %u MHz %s, %u MB RAM, %s, %d MB VRAM, "
             "%d units, OS %d.%d.%d",
             verdict_word(), mach.model[0] ? mach.model : "?", mach.ncpu, mach.mhz,
             cpu_family(), mach.ram_mb, mach.gl ? mach.gl_renderer : "no GL",
             mach.vram_mb, mach.texunits, mach.os_major, mach.os_minor, mach.os_bugfix);
    {
        /* the title has a 640-pixel window to fit in: the renderer without
         * its " OpenGL Engine" suffix */
        char ren[64];
        char* e;
        snprintf(ren, sizeof(ren), "%s", mach.gl ? mach.gl_renderer : "no GL");
        e = strstr(ren, " OpenGL Engine");
        if (e) {
            *e = '\0';
        }
        snprintf(title, sizeof(title), "Mario Party 4 - machine check: %s - %s, %s, %d MB",
                 verdict_word(), mach.model[0] ? mach.model : "?", ren, mach.vram_mb);
    }
}

static void print_inventory(void) {
    size_t i;
    port_log("port> machine: model %s, %d CPU%s at %u MHz, %s, AltiVec %s, %u MB RAM, "
             "Mac OS X %d.%d.%d%s%s\n",
             mach.model[0] ? mach.model : "?", mach.ncpu, mach.ncpu == 1 ? "" : "s",
             mach.mhz, cpu_family(), mach.altivec > 0 ? "yes" : "no", mach.ram_mb,
             mach.os_major, mach.os_minor, mach.os_bugfix,
             mach.host_build ? " (host build)" : !mach.native ? " (under Rosetta)" : "",
             mach.faked ? " [--fake-machine]" : "");
    if (mach.gl) {
        port_log("port> machine: GL %s / %s / %s, VRAM %d MB, %d texture units, "
                 "max texture %d\n",
                 mach.gl_vendor, mach.gl_renderer, mach.gl_version, mach.vram_mb,
                 mach.texunits, mach.maxtexsize);
        for (i = 0; i < N_EXTS; i++) {
            const MachExt* e = &exts[i];
            int have = ext_present(e);
            port_log("port> machine:   %-36s %-8s %s\n", e->name,
                     have ? "present" : "MISSING",
                     e->need == NEED_REQ ? "required" : e->need == NEED_DEG ? "degraded without"
                     : e->need == NEED_OPT ? "optional" : "not used");
        }
    } else {
        port_log("port> machine: GL: no context\n");
    }
}

static void print_verdict(void) {
    int i;
    port_log("port> machine: verdict %s\n", verdict_word());
    for (i = 0; i < nreasons; i++) {
        port_log("port> machine:   - %s\n", reasons[i]);
    }
}

/* The requirements in one paragraph (docs/requirements.md's first section),
 * for the dialogs. */
static const char* const REQUIREMENTS =
    "Mario Party 4 needs a PowerPC G4 Mac with Mac OS X 10.4 or later, 256 MB of "
    "memory and a Radeon 9000-class graphics card or better (four or more texture "
    "units with ATI's combiner extensions).  It runs at full speed on a dual 1 GHz G4 "
    "with a 64 MB Radeon 9000 under Mac OS X 10.5; slower machines run it below "
    "console speed.";

/* ---- the entry point --------------------------------------------------- */

const char* port_machine_title(void) { return title[0] ? title : "Mario Party 4"; }
const char* port_machine_summary(void) { return summary[0] ? summary : "?"; }
const char* port_machine_verdict(void) { return title[0] ? verdict_word() : "?"; }
int port_machine_degraded(void) { return verdict != V_OK; }

/* Lines for a first-run message: one per reason, then the requirements. */
int port_machine_reasons(const char** out, int cap) {
    int i;
    for (i = 0; i < nreasons && i < cap; i++) {
        out[i] = reasons[i];
    }
    return i;
}

void gx_tex_set_budget_mb(int mb);

void port_machine_check(void) {
    memset(&mach, 0, sizeof(mach));
    probe_cpu();
    probe_os();
    probe_gl();
    if (port_opt.fake_machine) {
        machine_fake_load(port_opt.fake_machine);
    }
    decide();
    print_inventory();
    print_verdict();

    /* (c) the settings, unless the flag was given */
    if (want_threads >= 0) {
        if (port_opt.threads == -1) {
            port_opt.threads = want_threads;
            port_log("port> machine: applying --threads %d (%d CPU)\n", want_threads, mach.ncpu);
        } else {
            port_log("port> machine: --threads %d given; not applying --threads %d\n",
                     port_opt.threads, want_threads);
        }
    }
    if (want_texbudget >= 0) {
        if (!port_opt.texbudget_set) {
            gx_tex_set_budget_mb(want_texbudget);
            port_log("port> machine: applying --texbudget %d (VRAM %d MB)\n", want_texbudget,
                     mach.vram_mb);
        } else {
            port_log("port> machine: --texbudget given; not applying --texbudget %d "
                     "(VRAM %d MB)\n", want_texbudget, mach.vram_mb);
        }
    }
    if (want_cpuxf > 0) {
        if (!port_opt.cpuxf) {
            port_opt.cpuxf = 1;
            port_log("port> machine: applying --cpuxf (no GL_ARB_vertex_program)\n");
        } else {
            port_log("port> machine: --cpuxf given already (no GL_ARB_vertex_program)\n");
        }
    }
    /* M31 (PLAN.md 46): the coroutine stacks' multiplier is a setting the
     * applied-settings block should own up to -- x2 is the default since the
     * M31 soak (6.5 h at x2, four boards, 129 minigame plays, no `stack
     * overlap error`, no fault); m459 needs it (x4 exhausts HEAP_HEAP). */
    port_log("port> machine: coroutine stacks x%d over the game's sizes (%s)\n",
             port_prc_stack_mul(),
             port_opt.stackmul != PORT_PRC_STACK_MUL ? "--stackmul given"
                                                     : "the default since M31; --stackmul N");

    if (port_opt.machinecheck) {
        /* the whole answer is above; the exit code is the verdict */
        fprintf(stderr, "machine check: %s\n", summary);
        exit(verdict);
    }
    if (verdict == V_UNSUPPORTED) {
        if (port_opt.force || port_opt.print_defaults) {
            port_log("port> machine: unsupported, %s\n",
                     port_opt.force ? "running anyway (--force)" : "printing --defaults anyway");
        } else {
            char text[2400];
            int i, n = 0;
            fprintf(stderr, "\nMario Party 4: this machine is unsupported:\n");
            n += snprintf(text + n, sizeof(text) - (size_t)n, "This machine cannot run the game:\n");
            for (i = 0; i < nreasons; i++) {
                fprintf(stderr, "  - %s\n", reasons[i]);
                if (n < (int)sizeof(text)) {
                    n += snprintf(text + n, sizeof(text) - (size_t)n, "\n- %s\n", reasons[i]);
                }
            }
            fprintf(stderr, "\nRun with --force to try anyway; --machinecheck prints the "
                            "whole inventory.\nWhat the game needs is in "
                            "port/docs/requirements.md.\n");
            if (n < (int)sizeof(text)) {
                snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", REQUIREMENTS);
            }
            port_log("port> machine: refusing to run (unsupported; --force overrides)\n");
            if (!port_opt.headless) {
                port_dialog_notice("Mario Party 4: this Mac is not supported", text);
            }
            exit(V_UNSUPPORTED);
        }
    }
    /* the first-run message: once per distinct machine summary, when the
     * verdict is degraded (an `ok' machine has nothing to be told) */
    if (!port_opt.noconfig && !port_opt.headless) {
        const char* seen = port_config_get("machine");
        if (!seen || strcmp(seen, summary) != 0) {
            if (verdict == V_DEGRADED) {
                char text[2400];
                int i, n = 0;
                n += snprintf(text + n, sizeof(text) - (size_t)n,
                              "The game will run on this Mac, but below what it was built for:\n");
                for (i = 0; i < nreasons && n < (int)sizeof(text); i++) {
                    n += snprintf(text + n, sizeof(text) - (size_t)n, "\n- %s\n", reasons[i]);
                }
                if (n < (int)sizeof(text)) {
                    snprintf(text + n, sizeof(text) - (size_t)n,
                             "\n%s\n\nThis message shows once per machine.", REQUIREMENTS);
                }
                port_log("port> machine: first run on this machine (degraded): showing the "
                         "requirements\n");
                port_dialog_notice("Mario Party 4: about this Mac", text);
            }
            port_config_set("machine", summary);
            port_config_save();
        }
    }
}
