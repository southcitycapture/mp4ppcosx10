/* Phase 2 of the vertex path, as an ARB vertex program.
 *
 * `gx_draw.c` splits a primitive in two (see its file header).  Phase 1 packs
 * a *model-space* source vertex -- position, normal, colour, the raw texcoords
 * a texgen will read -- and phase 2, `finish_vertices`, walks that run and
 * produces the *output* vertex GL's client arrays read: view-space position,
 * the lit colour, the generated texcoords.  Phase 2 is ~70% of every frame on
 * the G4 (PLAN.md §22.8), and every line of it is arithmetic a vertex unit
 * exists to do.
 *
 * The Radeon 9000's driver exposes `GL_ARB_vertex_program`, so phase 2 can be
 * deleted rather than optimised: the *source* layout becomes the vertex
 * arrays, a generated program does the transform, the lighting and the texgen,
 * and the CPU only decodes.  `--cpuxf` puts the CPU path back for the A/B,
 * exactly the way `--olddecode` does for the decode (PLAN.md §22.1).
 *
 * Three things make this a generator rather than one program:
 *
 *  - R200-class hardware has no branching and a small instruction store, so
 *    "eight lights, eight texgens, every channel mode" cannot be one program
 *    with the unused parts predicated off.  A *variant* is compiled for the
 *    exact shape the primitive has, keyed and cached like the TEV configs.
 *  - the instruction count therefore depends on the variant, and a variant
 *    that does not fit the card's native limit must not silently run in
 *    software (the driver would fall back to its own vertex emulation and be
 *    slower than this port's own loop).  Every variant is asked, after it
 *    loads, for `GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB`; one that says no is
 *    marked dead and every draw with that key goes down the CPU path.
 *  - the fallback has to be *counted*, or "coverage" is an opinion.
 *    `--vprogstats` prints draws and vertices on each path, and the reason
 *    each dead variant died.
 *
 * The exact GX semantics being reproduced are the ones already in
 * `light_channel` and `finish_vertices`; that C is the specification, and
 * Dolphin's `VideoCommon/VertexShaderGen.cpp` + `LightingShaderGen.h` were
 * read alongside it as the independent statement of the same thing.
 */
#include "gx_internal.h"
#include "gx_skin.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#endif

/* ---- the extension, declared here rather than trusted to a header ---------
 *
 * The cross-build uses the MacOSX10.4u SDK, whose `gl.h` stops at 1.1 and
 * whose `glext.h` is not the one the Leopard driver ships.  Every token and
 * every entry point this file needs is therefore written out and resolved at
 * run time through `SDL_GL_GetProcAddress`, which is also the honest thing to
 * do for an extension: a build that compiles is not a card that has it. */
#define VP_VERTEX_PROGRAM_ARB              0x8620
#define VP_PROGRAM_FORMAT_ASCII_ARB        0x8875
#define VP_PROGRAM_ERROR_POSITION_ARB      0x864B
#define VP_PROGRAM_ERROR_STRING_ARB        0x8874
#define VP_PROGRAM_INSTRUCTIONS_ARB        0x88A0
#define VP_MAX_PROGRAM_INSTRUCTIONS_ARB    0x88A1
#define VP_PROGRAM_NATIVE_INSTRUCTIONS_ARB 0x88A2
#define VP_MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB 0x88A3
#define VP_PROGRAM_TEMPORARIES_ARB         0x88A4
#define VP_MAX_PROGRAM_TEMPORARIES_ARB     0x88A5
#define VP_PROGRAM_NATIVE_TEMPORARIES_ARB  0x88A6
#define VP_MAX_PROGRAM_NATIVE_TEMPORARIES_ARB 0x88A7
#define VP_PROGRAM_PARAMETERS_ARB          0x88A8
#define VP_MAX_PROGRAM_PARAMETERS_ARB      0x88A9
#define VP_PROGRAM_NATIVE_PARAMETERS_ARB   0x88AA
#define VP_MAX_PROGRAM_NATIVE_PARAMETERS_ARB 0x88AB
#define VP_PROGRAM_ATTRIBS_ARB             0x88AC
#define VP_MAX_PROGRAM_ATTRIBS_ARB         0x88AD
#define VP_PROGRAM_NATIVE_ATTRIBS_ARB      0x88AE
#define VP_MAX_PROGRAM_NATIVE_ATTRIBS_ARB  0x88AF
#define VP_MAX_PROGRAM_ADDRESS_REGISTERS_ARB 0x88B1
#define VP_MAX_PROGRAM_LOCAL_PARAMETERS_ARB  0x88B4
#define VP_MAX_PROGRAM_ENV_PARAMETERS_ARB    0x88B5
#define VP_PROGRAM_UNDER_NATIVE_LIMITS_ARB   0x88B6

#ifndef PORT_NO_SDL
typedef void (*vp_genprog_t)(GLsizei, GLuint*);
typedef void (*vp_delprog_t)(GLsizei, const GLuint*);
typedef void (*vp_bindprog_t)(GLenum, GLuint);
typedef void (*vp_progstr_t)(GLenum, GLenum, GLsizei, const void*);
typedef void (*vp_envp4fv_t)(GLenum, GLuint, const GLfloat*);
typedef void (*vp_getprogiv_t)(GLenum, GLenum, GLint*);
typedef void (*vp_envp4fvn_t)(GLenum, GLuint, GLsizei, const GLfloat*);

static vp_genprog_t    vp_GenProgramsARB;
static vp_delprog_t    vp_DeleteProgramsARB;
static vp_bindprog_t   vp_BindProgramARB;
static vp_progstr_t    vp_ProgramStringARB;
static vp_envp4fv_t    vp_ProgramEnvParameter4fvARB;
static vp_getprogiv_t  vp_GetProgramivARB;
static vp_envp4fvn_t   vp_ProgramEnvParameters4fvEXT; /* GL_EXT_gpu_program_parameters */
#endif

/* the card's answers, filled by the probe */
typedef struct VpLimits {
    int have;             /* the extension is present and the entry points resolved */
    int max_instr, max_native_instr;
    int max_params, max_native_params;
    int max_temps, max_native_temps;
    int max_attribs, max_native_attribs;
    int max_addr_regs;
    int max_env, max_local;
    int trivial_ok;       /* a trivial program compiled, loaded and was native */
    int pal_ok;           /* ARL + relative env addressing loaded native (M18) */
    int pal_slots;        /* palette slots the parameter block holds          */
} VpLimits;
static VpLimits vpl;

/* the parameter block's layout; the block itself is described below */
#define VPE_POSMTX 0
#define VPE_NRMMTX 3
#define VPE_MAT    6
#define VPE_AMB    7
#define VPE_LIGHT  8
#define VPE_NLIGHTS 2  /* M18: was 8; nothing in this game lights with more than
                        * one (PLAN.md 25), and the room went to the palette */
/* M21: five params a light -- position, colour, distance attenuation (k),
 * angle attenuation (a), direction (the half-angle vector for a specular
 * light) -- the last two for the specular channel (PLAN.md 36) */
#define VPE_LSTRIDE 5
#define VPE_MAT1   (VPE_LIGHT + VPE_LSTRIDE * VPE_NLIGHTS)  /* 18: channel 1's material */
#define VPE_AMB1   (VPE_MAT1 + 1)                           /* 19: channel 1's ambient  */
#define VPE_TEXMTX (VPE_AMB1 + 1)                           /* 20 */
#define VPE_TEXSCL (VPE_TEXMTX + 3 * GX_TEXCOORDS)          /* 44 */
#define VPE_COUNT  (VPE_TEXSCL + GX_TEX_UNITS)              /* 50 */
/* M18: the matrix palette, GX_PAL_STRIDE params a slot, from here to the
 * card's native limit (192 on the Radeon 9000: 24 slots) */
#define VPE_PAL    VPE_COUNT
#define VPE_TOTAL  (VPE_PAL + GX_PAL_STRIDE * GX_PAL_SLOTS_MAX)

int gx_vprog_available(void) { return vpl.have && vpl.trivial_ok; }
int gx_vprog_native_instr_limit(void) { return vpl.max_native_instr; }

/* ---- the probe ------------------------------------------------------------
 *
 * Run from `gl13_init` when `--vprobe` (or `--glinfo`) is given, and once
 * unconditionally at first use so the limits are known before a variant is
 * generated.  It asks for every limit the generator is allowed to spend, then
 * compiles the smallest possible program and checks that it loads *and* stays
 * under the native limits -- the second half matters, because a driver will
 * happily accept a program it intends to run in software. */
static const char VP_TRIVIAL[] =
    "!!ARBvp1.0\n"
    "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
    "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
    "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
    "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
    "MOV result.color, vertex.color;\n"
    "MOV result.texcoord[0], vertex.texcoord[0];\n"
    "END\n";

/* M18: the shape the matrix palette needs -- an address register loaded from
 * the fog coordinate and a parameter read relative to it.  If the driver runs
 * *this* in software the palette is dead on arrival, so it is asked first. */
static const char VP_PALETTE[] =
    "!!ARBvp1.0\n"
    "ADDRESS a0;\n"
    "TEMP vp;\n"
    "PARAM pal[144] = { program.env[46..189] };\n" /* VPE_PAL .. +6*24-1 */
    "ARL a0.x, vertex.fogcoord.x;\n"
    "DP4 vp.x, pal[a0.x + 0], vertex.position;\n"
    "DP4 vp.y, pal[a0.x + 1], vertex.position;\n"
    "DP4 vp.z, pal[a0.x + 2], vertex.position;\n"
    "MOV vp.w, 1.0;\n"
    "DP4 result.position.x, state.matrix.projection.row[0], vp;\n"
    "DP4 result.position.y, state.matrix.projection.row[1], vp;\n"
    "DP4 result.position.z, state.matrix.projection.row[2], vp;\n"
    "DP4 result.position.w, state.matrix.projection.row[3], vp;\n"
    "MOV result.color, vertex.color;\n"
    "END\n";

#ifndef PORT_NO_SDL
static int vp_geti(GLenum pname) {
    GLint v = 0;
    vp_GetProgramivARB(VP_VERTEX_PROGRAM_ARB, pname, &v);
    return (int)v;
}
#endif

void gx_vprog_probe(void) {
#ifdef PORT_NO_SDL
    port_log("port> vprog: built without SDL2, no GL, no probe\n");
#else
    const char* ext;
    GLuint id = 0;
    GLint errpos = -1;
    static int done;

    if (done) {
        return;
    }
    done = 1;

    ext = (const char*)glGetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_ARB_vertex_program")) {
        port_log("port> vprog: GL_ARB_vertex_program is NOT present; "
                 "phase 2 stays on the CPU\n");
        return;
    }
    vp_GenProgramsARB = (vp_genprog_t)SDL_GL_GetProcAddress("glGenProgramsARB");
    vp_DeleteProgramsARB = (vp_delprog_t)SDL_GL_GetProcAddress("glDeleteProgramsARB");
    vp_BindProgramARB = (vp_bindprog_t)SDL_GL_GetProcAddress("glBindProgramARB");
    vp_ProgramStringARB = (vp_progstr_t)SDL_GL_GetProcAddress("glProgramStringARB");
    vp_ProgramEnvParameter4fvARB =
        (vp_envp4fv_t)SDL_GL_GetProcAddress("glProgramEnvParameter4fvARB");
    vp_GetProgramivARB = (vp_getprogiv_t)SDL_GL_GetProcAddress("glGetProgramivARB");
    vp_ProgramEnvParameters4fvEXT = strstr(ext, "GL_EXT_gpu_program_parameters")
        ? (vp_envp4fvn_t)SDL_GL_GetProcAddress("glProgramEnvParameters4fvEXT") : NULL;
    if (!vp_GenProgramsARB || !vp_DeleteProgramsARB || !vp_BindProgramARB ||
        !vp_ProgramStringARB || !vp_ProgramEnvParameter4fvARB || !vp_GetProgramivARB) {
        port_log("port> vprog: the extension string is there but an entry point "
                 "is not; phase 2 stays on the CPU\n");
        return;
    }
    vpl.have = 1;

    vpl.max_instr = vp_geti(VP_MAX_PROGRAM_INSTRUCTIONS_ARB);
    vpl.max_native_instr = vp_geti(VP_MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB);
    vpl.max_params = vp_geti(VP_MAX_PROGRAM_PARAMETERS_ARB);
    vpl.max_native_params = vp_geti(VP_MAX_PROGRAM_NATIVE_PARAMETERS_ARB);
    vpl.max_temps = vp_geti(VP_MAX_PROGRAM_TEMPORARIES_ARB);
    vpl.max_native_temps = vp_geti(VP_MAX_PROGRAM_NATIVE_TEMPORARIES_ARB);
    vpl.max_attribs = vp_geti(VP_MAX_PROGRAM_ATTRIBS_ARB);
    vpl.max_native_attribs = vp_geti(VP_MAX_PROGRAM_NATIVE_ATTRIBS_ARB);
    vpl.max_addr_regs = vp_geti(VP_MAX_PROGRAM_ADDRESS_REGISTERS_ARB);
    vpl.max_env = vp_geti(VP_MAX_PROGRAM_ENV_PARAMETERS_ARB);
    vpl.max_local = vp_geti(VP_MAX_PROGRAM_LOCAL_PARAMETERS_ARB);

    port_log("---- vprog probe ----\n");
    port_log("GL_ARB_vertex_program                     present\n");
    port_log("MAX_PROGRAM_INSTRUCTIONS            %6d\n", vpl.max_instr);
    port_log("MAX_PROGRAM_NATIVE_INSTRUCTIONS     %6d\n", vpl.max_native_instr);
    port_log("MAX_PROGRAM_PARAMETERS              %6d\n", vpl.max_params);
    port_log("MAX_PROGRAM_NATIVE_PARAMETERS       %6d\n", vpl.max_native_params);
    port_log("MAX_PROGRAM_TEMPORARIES             %6d\n", vpl.max_temps);
    port_log("MAX_PROGRAM_NATIVE_TEMPORARIES      %6d\n", vpl.max_native_temps);
    port_log("MAX_PROGRAM_ATTRIBS                 %6d\n", vpl.max_attribs);
    port_log("MAX_PROGRAM_NATIVE_ATTRIBS          %6d\n", vpl.max_native_attribs);
    port_log("MAX_PROGRAM_ADDRESS_REGISTERS       %6d\n", vpl.max_addr_regs);
    port_log("MAX_PROGRAM_ENV_PARAMETERS          %6d\n", vpl.max_env);
    port_log("MAX_PROGRAM_LOCAL_PARAMETERS        %6d\n", vpl.max_local);
    port_log("GL_EXT_gpu_program_parameters       %s\n",
             vp_ProgramEnvParameters4fvEXT ? "present (one call per palette upload)" : "absent");

    /* the trivial program: does it load, and is it native */
    vp_GenProgramsARB(1, &id);
    vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, id);
    vp_ProgramStringARB(VP_VERTEX_PROGRAM_ARB, VP_PROGRAM_FORMAT_ASCII_ARB,
                        (GLsizei)(sizeof(VP_TRIVIAL) - 1), VP_TRIVIAL);
    glGetIntegerv(VP_PROGRAM_ERROR_POSITION_ARB, &errpos);
    if (errpos != -1) {
        const char* msg = (const char*)glGetString(VP_PROGRAM_ERROR_STRING_ARB);
        port_log("trivial program REJECTED at char %d: %s\n", (int)errpos,
                 msg ? msg : "(no message)");
    } else {
        int native = vp_geti(VP_PROGRAM_UNDER_NATIVE_LIMITS_ARB);
        port_log("trivial program            loaded, %d instructions "
                 "(%d native), under native limits: %s\n",
                 vp_geti(VP_PROGRAM_INSTRUCTIONS_ARB),
                 vp_geti(VP_PROGRAM_NATIVE_INSTRUCTIONS_ARB),
                 native ? "YES" : "NO");
        vpl.trivial_ok = native ? 1 : 0;
    }
    vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, 0);
    vp_DeleteProgramsARB(1, &id);

    /* the palette shape (M18) */
    vp_GenProgramsARB(1, &id);
    vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, id);
    errpos = -1;
    vp_ProgramStringARB(VP_VERTEX_PROGRAM_ARB, VP_PROGRAM_FORMAT_ASCII_ARB,
                        (GLsizei)(sizeof(VP_PALETTE) - 1), VP_PALETTE);
    glGetIntegerv(VP_PROGRAM_ERROR_POSITION_ARB, &errpos);
    if (errpos != -1) {
        const char* msg = (const char*)glGetString(VP_PROGRAM_ERROR_STRING_ARB);
        port_log("palette program (ARL)      REJECTED at char %d: %s\n", (int)errpos,
                 msg ? msg : "(no message)");
    } else {
        int native = vp_geti(VP_PROGRAM_UNDER_NATIVE_LIMITS_ARB);
        port_log("palette program (ARL)      loaded, %d instructions (%d native), "
                 "under native limits: %s\n",
                 vp_geti(VP_PROGRAM_INSTRUCTIONS_ARB),
                 vp_geti(VP_PROGRAM_NATIVE_INSTRUCTIONS_ARB), native ? "YES" : "NO");
        vpl.pal_ok = native ? 1 : 0;
    }
    vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, 0);
    vp_DeleteProgramsARB(1, &id);
    vpl.pal_slots = 0;
    if (vpl.pal_ok && vpl.max_native_params > VPE_PAL) {
        vpl.pal_slots = (vpl.max_native_params - VPE_PAL) / GX_PAL_STRIDE;
        if (vpl.pal_slots > GX_PAL_SLOTS_MAX) {
            vpl.pal_slots = GX_PAL_SLOTS_MAX;
        }
        if (port_opt.palsize > 0 && port_opt.palsize < vpl.pal_slots) {
            vpl.pal_slots = port_opt.palsize;
        }
    }
    port_log("matrix palette             %d slots of %d params from env[%d]\n",
             vpl.pal_slots, GX_PAL_STRIDE, VPE_PAL);
    port_log("---- end vprog probe ----\n");
#endif
}

int gx_skin_palette_slots(void) { return vpl.pal_slots; }
int gx_vprog_palette_available(void) {
    return gx_vprog_available() && vpl.pal_ok && vpl.pal_slots > 0;
}

/* ---- the variant key -------------------------------------------------------
 *
 * Everything the generated text depends on, and nothing else.  Two draws with
 * the same key get the same program; the light *positions*, the matrices and
 * the colours are program parameters and never part of the key, which is why
 * a board frame with 850 draws compiles a handful of programs and then stops
 * compiling.
 *
 * The lights are keyed by *count* rather than by mask, because the uploader
 * packs the enabled lights densely into the parameter block in ascending GX
 * order.  `diff_fn` and `attn_fn` are per-channel on GX, not per-light, so the
 * count is genuinely all the text depends on. */
typedef struct VpKey {
    u8 have_nrm;
    u8 lit;            /* chan_mode == 2 */
    u8 mat_reg, amb_reg;
    u8 diff_fn, attn_fn;
    u8 nlights;
    u8 nunits;         /* GL units the draw will bind a coord array on */
    u8 unit_on[GX_TEX_UNITS];  /* 1 = this unit gets a generated coord     */
    u8 unit_tg[GX_TEX_UNITS];  /* which texgen slot it reads               */
    u8 tg_kind[GX_TEXCOORDS];  /* 0 texcoord, 1 position, 2 normal         */
    u8 tg_k[GX_TEXCOORDS];
    u8 tg_div[GX_TEXCOORDS];
    u8 vtxdiv;                 /* --vtxdivide: q divided at the vertex (pre-M26) */
    u8 viewtg;                 /* --viewtexgen: POS/NRM texgens read view space (pre-M26) */
    u8 tg_mtx[GX_TEXCOORDS];   /* 1 = a real matrix, 0 = identity          */
    u8 fog;
    u8 pal;            /* M18: the matrices come from the palette, indexed
                        * by vertex.fogcoord, not from env[0..5]          */
    u8 hilite;         /* M21: channel 1 (GX_AF_SPEC) computed and folded:
                        * 1: primary *= (1 - spec), secondary = spec;
                        * 2 (M22): primary = c0, primary.a = spec's
                        * luminance (the textured highlight)              */
    u8 mat1_reg, amb1_reg;
    u8 l0mask, l1mask; /* which of the packed lights each channel reads
                        * (all of them for channel 0 without the fold)    */
} VpKey;

/* ---- the parameter block ---------------------------------------------------
 *
 *   env[0..2]    position matrix, three rows of a 3x4
 *   env[3..5]    normal matrix, three rows of a 3x3 (w = 0)
 *   env[6]       the register material RGBA
 *   env[7]       the register ambient RGB
 *   env[8+5i]    light i: position / colour / (k0,k1,k2) / (a0,a1,a2) /
 *                direction (M21: the last two for the specular channel),
 *                i < VPE_NLIGHTS
 *   env[18], [19] channel 1's register material and ambient (M21)
 *   env[20+3t]   texgen t's matrix, three rows of a 3x4
 *   env[44+u]    GL unit u's (su, sv, 0, tv): the NPOT fold gx_tex.c puts in
 *                the fixed-function GL_TEXTURE matrix, which a vertex program
 *                bypasses and therefore has to apply itself; tv is the EFB
 *                copy's flip (M24b), zero for every decoded texture
 *   env[50..]    the matrix palette (M18): 23 slots of 6
 *
 * 50 of the card's 192 native parameters before the palette.  The block is
 * *environment* rather
 * than local state so one upload serves every variant: consecutive draws
 * usually share the lights and the texgen matrices and differ only in the
 * position matrix, and the shadow below emits only what changed. */

#ifndef PORT_NO_SDL
static float env_shadow[VPE_TOTAL][4];
static u8 env_valid[VPE_TOTAL];
static unsigned stat_env_set, stat_env_elided, stat_env_bulk; /* M21: --envbulk uploads */

static void env4(int i, float x, float y, float z, float w) {
    float v[4];
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
    if (env_valid[i] && env_shadow[i][0] == x && env_shadow[i][1] == y &&
        env_shadow[i][2] == z && env_shadow[i][3] == w) {
        stat_env_elided++;
        return;
    }
    env_valid[i] = 1;
    memcpy(env_shadow[i], v, sizeof(v));
    stat_env_set++;
    vp_ProgramEnvParameter4fvARB(VP_VERTEX_PROGRAM_ARB, (GLuint)i, v);
}
#endif

/* ---- the generator ---------------------------------------------------------
 *
 * The C in `gx_draw.c` is the specification and the order here follows it line
 * for line, so the two can be read side by side.  Where the arithmetic cannot
 * be reproduced exactly it is *because a GPU is not a 7450*, and those places
 * are named in PLAN.md §25 rather than papered over:
 *
 *  - the lit colour is quantised to eight bits by the CPU (`v * 255 + 0.5`)
 *    before the rasteriser ever sees it, and stays float here until the
 *    rasteriser quantises it at the end;
 *  - `RSQ` is the card's reciprocal square root, not `gx_math.h`'s refined
 *    `frsqrte`;
 *  - the CPU's `if (d2 > 0)` and `if (q != 0)` guards become a clamp against a
 *    tiny constant, which produces the same answer on the degenerate input
 *    (`RSQ(1e-30)` times a zero vector is still zero; `RCP` of a clamped
 *    non-positive denominator exceeds one and is then clamped to one, which is
 *    exactly what `den > 0.0f ? ... : 1.0f` did).
 */
typedef struct VpBuf {
    char* s;
    size_t len, cap;
    int instr;
} VpBuf;

static void vpb_add(VpBuf* b, const char* fmt, ...) {
    va_list ap;
    int n;
    if (b->len + 256 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        char* p = (char*)realloc(b->s, nc);
        if (!p) {
            return;
        }
        b->s = p;
        b->cap = nc;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += (size_t)n;
    }
}

/* Every call to this one is one instruction; `vpb_add` is for the text that is
 * not, so the count is maintained by construction rather than by parsing. */
static void vpi(VpBuf* b, const char* fmt, ...) {
    va_list ap;
    int n;
    if (b->len + 256 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        char* p = (char*)realloc(b->s, nc);
        if (!p) {
            return;
        }
        b->s = p;
        b->cap = nc;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += (size_t)n;
    }
    b->instr++;
}

static void vp_gen(const VpKey* k, VpBuf* b) {
    int i, u, t;
    int need_nrm;

    /* the normal is computed when lighting wants it or a texgen reads it */
    need_nrm = k->lit;
    for (t = 0; t < GX_TEXCOORDS; t++) {
        if (k->tg_kind[t] == 2 && k->viewtg) {
            need_nrm = 1;
        }
    }

    vpb_add(b, "!!ARBvp1.0\n");
    vpb_add(b, "# generated by gx_vprog.c -- phase 2 of the GX vertex path\n");
    vpb_add(b, "TEMP vp, nr, ac, mt, t0, t1;\n");
    if (k->pal) {
        /* relative addressing is only allowed into a declared PARAM array,
         * and an array bound to program.env is an alias of it, not a copy */
        vpb_add(b, "ADDRESS a0;\n");
        vpb_add(b, "PARAM pal[%d] = { program.env[%d..%d] };\n",
                GX_PAL_STRIDE * vpl.pal_slots, VPE_PAL,
                VPE_PAL + GX_PAL_STRIDE * vpl.pal_slots - 1);
    }

    /* --- position: model -> view, then view -> clip.
     * The view-space position is kept because the CPU's `op[]` is what the
     * lighting and a GX_TG_POS texgen read; the projection is the one
     * `gl13_apply_transform` has already loaded, so the driver tracks it.
     * With the palette (M18) the vertex's fog coordinate is its slot times
     * GX_PAL_STRIDE, and the matrices are read relative to it. */
    if (k->pal) {
        vpi(b, "ARL a0.x, vertex.fogcoord.x;\n");
        vpi(b, "DP4 vp.x, pal[a0.x + 0], vertex.position;\n");
        vpi(b, "DP4 vp.y, pal[a0.x + 1], vertex.position;\n");
        vpi(b, "DP4 vp.z, pal[a0.x + 2], vertex.position;\n");
    } else {
        vpi(b, "DP4 vp.x, program.env[%d], vertex.position;\n", VPE_POSMTX + 0);
        vpi(b, "DP4 vp.y, program.env[%d], vertex.position;\n", VPE_POSMTX + 1);
        vpi(b, "DP4 vp.z, program.env[%d], vertex.position;\n", VPE_POSMTX + 2);
    }
    vpi(b, "MOV vp.w, 1.0;\n");
    vpi(b, "DP4 result.position.x, state.matrix.projection.row[0], vp;\n");
    vpi(b, "DP4 result.position.y, state.matrix.projection.row[1], vp;\n");
    vpi(b, "DP4 result.position.z, state.matrix.projection.row[2], vp;\n");
    vpi(b, "DP4 result.position.w, state.matrix.projection.row[3], vp;\n");

    /* GL's own fog coordinate is |z_eye|, and the CPU path gets that for free
     * because it hands GL an already-view-space position under an identity
     * modelview.  A vertex program has to say it. */
    if (k->fog) {
        vpi(b, "ABS result.fogcoord.x, vp.z;\n");
    }

    /* --- the normal, transformed and renormalised (finish_vertices) */
    if (need_nrm) {
        if (k->have_nrm && k->pal) {
            vpi(b, "DP3 nr.x, pal[a0.x + 3], vertex.normal;\n");
            vpi(b, "DP3 nr.y, pal[a0.x + 4], vertex.normal;\n");
            vpi(b, "DP3 nr.z, pal[a0.x + 5], vertex.normal;\n");
            vpi(b, "DP3 t0.w, nr, nr;\n");
            vpi(b, "MAX t0.w, t0.w, 1.0e-30;\n");
            vpi(b, "RSQ t0.w, t0.w;\n");
            vpi(b, "MUL nr.xyz, nr, t0.w;\n");
        } else if (k->have_nrm) {
            vpi(b, "DP3 nr.x, program.env[%d], vertex.normal;\n", VPE_NRMMTX + 0);
            vpi(b, "DP3 nr.y, program.env[%d], vertex.normal;\n", VPE_NRMMTX + 1);
            vpi(b, "DP3 nr.z, program.env[%d], vertex.normal;\n", VPE_NRMMTX + 2);
            vpi(b, "DP3 t0.w, nr, nr;\n");
            vpi(b, "MAX t0.w, t0.w, 1.0e-30;\n");
            vpi(b, "RSQ t0.w, t0.w;\n");
            vpi(b, "MUL nr.xyz, nr, t0.w;\n");
        } else {
            /* what phase 1 stored for a descriptor with no normal, and what a
             * GX_TG_NRM texgen then read back */
            vpi(b, "MOV nr, {0.0, 0.0, 1.0, 1.0};\n");
        }
        vpi(b, "MOV nr.w, 1.0;\n");
    }

    /* --- the colour channel (light_channel) */
    if (!k->lit) {
        /* chan_mode 0 and 1 both leave the final colour in the source vertex:
         * phase 1 already splatted the register material when it won.  This is
         * the case that has to come out *byte-identical* to the CPU path, and
         * it does, because the byte array is normalised the same way and
         * nothing else happens to it. */
        vpi(b, "MOV result.color, vertex.color;\n");
    } else {
        vpi(b, "MOV mt, %s;\n",
            k->mat_reg ? "program.env[6]" : "vertex.color");
        vpi(b, "MOV ac.xyz, %s;\n",
            k->amb_reg ? "program.env[7]" : "vertex.color");
        for (i = 0; i < k->nlights; i++) {
            int lp = VPE_LIGHT + VPE_LSTRIDE * i;
            if (k->hilite && !(k->l0mask & (1u << i))) {
                continue; /* a light only channel 1 reads */
            }
            vpi(b, "SUB t0.xyz, program.env[%d], vp;\n", lp);
            vpi(b, "DP3 t0.w, t0, t0;\n");                 /* d2            */
            vpi(b, "MAX t0.w, t0.w, 1.0e-30;\n");
            vpi(b, "RSQ t1.w, t0.w;\n");                   /* 1/d           */
            vpi(b, "MUL t0.xyz, t0, t1.w;\n");             /* unit direction */
            vpi(b, "MUL t1.x, t0.w, t1.w;\n");             /* d = d2 * (1/d) */
            /* exactly light_channel's three-way, in its order: CLAMP, then
             * SIGN, then "anything else means one" */
            if (k->diff_fn == GX_DF_CLAMP) {
                vpi(b, "DP3 t1.y, nr, t0;\n");
                vpi(b, "MAX t1.y, t1.y, 0.0;\n");
            } else if (k->diff_fn == GX_DF_SIGN) {
                vpi(b, "DP3 t1.y, nr, t0;\n");
            } else {
                vpi(b, "MOV t1.y, 1.0;\n");
            }
            if (k->attn_fn != GX_AF_NONE) {
                vpi(b, "MAD t1.z, program.env[%d].y, t1.x, program.env[%d].x;\n",
                    lp + 2, lp + 2);
                vpi(b, "MAD t1.z, program.env[%d].z, t0.w, t1.z;\n", lp + 2);
                vpi(b, "MAX t1.z, t1.z, 1.0e-20;\n");
                vpi(b, "RCP t1.z, t1.z;\n");
                vpi(b, "MIN t1.z, t1.z, 1.0;\n");
                vpi(b, "MUL t1.y, t1.y, t1.z;\n");
            }
            vpi(b, "MAD ac.xyz, program.env[%d], t1.y, ac;\n", lp + 1);
        }
        vpi(b, "MUL t0.xyz, ac, mt;\n");
        vpi(b, "MAX t0.xyz, t0, 0.0;\n");
        if (!k->hilite) {
            vpi(b, "MIN result.color.xyz, t0, 1.0;\n");
        } else {
            /* M21: the specular channel, GX_DF_NONE + GX_AF_SPEC exactly as
             * the hardware (and Dolphin's LightingShaderGen) compute it:
             *   ldir = normalize(lpos - pos)
             *   nh   = (N . ldir >= 0) ? max(0, N . H) : 0      H = the light's dir
             *   attn = max(0, a . (1, nh, nh^2)) / (k . (1, nh, nh^2))
             *   c1   = mat1 * clamp(amb1 + sum(attn * lcol), 0, 1)
             * then the fold (gx_internal.h): primary = c0 * (1 - c1),
             * secondary = c1, and GL_COLOR_SUM adds it after the units. */
            vpi(b, "MIN t0.xyz, t0, 1.0;\n"); /* t0 = c0 */
            if (k->amb1_reg) {
                vpi(b, "MOV ac.xyz, program.env[%d];\n", VPE_AMB1);
            } else {
                vpi(b, "MOV ac.xyz, vertex.color;\n");
            }
            for (i = 0; i < k->nlights; i++) {
                int lp = VPE_LIGHT + VPE_LSTRIDE * i;
                if (!(k->l1mask & (1u << i))) {
                    continue;
                }
                vpi(b, "SUB t1.xyz, program.env[%d], vp;\n", lp);
                vpi(b, "DP3 t1.w, t1, t1;\n");
                vpi(b, "MAX t1.w, t1.w, 1.0e-30;\n");
                vpi(b, "RSQ t1.w, t1.w;\n");
                vpi(b, "MUL t1.xyz, t1, t1.w;\n");              /* ldir          */
                vpi(b, "DP3 t1.w, nr, t1;\n");                   /* N . ldir      */
                vpi(b, "SGE t1.w, t1.w, 0.0;\n");                /* the gate      */
                vpi(b, "DP3 t1.x, nr, program.env[%d];\n", lp + 4); /* N . H      */
                vpi(b, "MAX t1.x, t1.x, 0.0;\n");
                vpi(b, "MUL t1.x, t1.x, t1.w;\n");               /* nh            */
                vpi(b, "MUL t1.y, t1.x, t1.x;\n");               /* nh^2          */
                vpi(b, "MAD t1.w, program.env[%d].y, t1.x, program.env[%d].x;\n", lp + 3, lp + 3);
                vpi(b, "MAD t1.w, program.env[%d].z, t1.y, t1.w;\n", lp + 3); /* numerator */
                vpi(b, "MAX t1.w, t1.w, 0.0;\n");
                vpi(b, "MAD t1.z, program.env[%d].y, t1.x, program.env[%d].x;\n", lp + 2, lp + 2);
                vpi(b, "MAD t1.z, program.env[%d].z, t1.y, t1.z;\n", lp + 2); /* denominator */
                vpi(b, "MAX t1.z, t1.z, 1.0e-20;\n");
                vpi(b, "RCP t1.z, t1.z;\n");
                vpi(b, "MUL t1.w, t1.w, t1.z;\n");               /* attn          */
                vpi(b, "MAD ac.xyz, program.env[%d], t1.w, ac;\n", lp + 1);
            }
            vpi(b, "MAX ac.xyz, ac, 0.0;\n");
            vpi(b, "MIN ac.xyz, ac, 1.0;\n");
            if (k->mat1_reg) {
                vpi(b, "MUL ac.xyz, ac, program.env[%d];\n", VPE_MAT1);
            } else {
                vpi(b, "MUL ac.xyz, ac, vertex.color;\n");
            }
            if (k->hilite == 2) {
                /* M22: the textured highlight rides the primary alpha */
                vpi(b, "MOV result.color.xyz, t0;\n");
                vpi(b, "DP3 result.color.w, ac, {0.299, 0.587, 0.114, 0.0};\n");
            } else {
                vpi(b, "MOV result.color.secondary, ac;\n");
                vpi(b, "SUB t1.xyz, 1.0, ac;\n");
                vpi(b, "MUL result.color.xyz, t0, t1;\n");
            }
        }
        if (k->hilite != 2) {
            vpi(b, "MOV result.color.w, mt.w;\n");
        }
    }

    /* --- texgen, one GL unit at a time (the mapping draw_run makes) */
    for (u = 0; u < GX_TEX_UNITS; u++) {
        const char* in;
        int tt;
        if (!k->unit_on[u]) {
            continue;
        }
        tt = k->unit_tg[u];
        if (k->tg_kind[tt] == 1 && !k->viewtg) {
            /* GX_TG_POS is the RAW position (M26, PLAN.md 41): the XF unit
             * multiplies the texgen matrix into the *input* row, and the
             * game's shadow and projection matrices are built object->texture
             * (hsfdraw.c FaceDrawShadow: shadowCam * invCamera * model).
             * Every M3..M25 build fed the view-space position here
             * (`--viewtexgen`), so every projected shadow map landed where
             * the view-space point would have been in the world. */
            in = "vertex.position"; /* w is 1 for the 3-float arrays */
        } else if (k->tg_kind[tt] == 2 && !k->viewtg) {
            /* GX_TG_NRM likewise: the raw normal; hsfdraw's reflection and
             * hilite matrices carry the object->view rotation themselves. */
            vpi(b, "MOV t0, vertex.normal;\n");
            vpi(b, "MOV t0.w, 1.0;\n");
            in = "t0";
        } else if (k->tg_kind[tt] == 1) {
            in = "vp"; /* the view-space position, w already 1 */
        } else if (k->tg_kind[tt] == 2) {
            in = "nr"; /* w set to 1 above */
        } else {
            vpi(b, "MOV t0, vertex.texcoord[%d];\n", k->tg_k[tt]);
            vpi(b, "MOV t0.zw, {0.0, 0.0, 1.0, 1.0};\n");
            in = "t0";
        }
        if (!k->tg_mtx[tt]) {
            vpi(b, "MOV t1.xy, %s;\n", in);
        } else {
            int tm = VPE_TEXMTX + 3 * tt;
            vpi(b, "DP4 t1.x, program.env[%d], %s;\n", tm + 0, in);
            vpi(b, "DP4 t1.y, program.env[%d], %s;\n", tm + 1, in);
            if (k->tg_div[tt] && !k->vtxdiv) {
                /* GX_TG_MTX3x4 (M26, PLAN.md 41): the third row is q, and the
                 * hardware divides by it PER PIXEL.  Hand GL (s*su + q*0,
                 * t*sv + q*tv, 0, q) and let the rasteriser divide: the
                 * fold's offset rides on q so that (t*sv + q*tv)/q is the
                 * flipped coordinate.  Dividing at the vertex (the M3..M25
                 * path, `--vtxdivide`) interpolates s/q linearly across a
                 * polygon, which for a floor under a perspective shadow
                 * camera puts every shadow between the vertices in the
                 * wrong place. */
                vpi(b, "DP4 t1.w, program.env[%d], %s;\n", tm + 2, in);
                vpi(b, "MUL t0.xy, t1, program.env[%d];\n", VPE_TEXSCL + u);
                vpi(b, "MAD t0.xy, t1.w, program.env[%d].zwzw, t0;\n", VPE_TEXSCL + u);
                vpi(b, "MOV t0.z, 0.0;\n");
                vpi(b, "MOV t0.w, t1.w;\n");
                vpi(b, "MOV result.texcoord[%d], t0;\n", u);
                continue;
            }
            if (k->tg_div[tt]) {
                vpi(b, "DP4 t1.w, program.env[%d], %s;\n", tm + 2, in);
                vpi(b, "MAX t1.w, t1.w, 1.0e-30;\n");
                vpi(b, "RCP t1.w, t1.w;\n");
                vpi(b, "MUL t1.xy, t1, t1.w;\n");
            }
        }
        /* the NPOT fold (s*su, t*sv + tv: the offset is the EFB copy's
         * flip, M24b), and r/q, which nothing generates but the rasteriser
         * reads */
        vpi(b, "MOV result.texcoord[%d], {0.0, 0.0, 0.0, 1.0};\n", u);
        vpi(b, "MAD result.texcoord[%d].xy, t1, program.env[%d], program.env[%d].zwzw;\n", u,
            VPE_TEXSCL + u, VPE_TEXSCL + u);
    }

    vpb_add(b, "END\n");
}

/* ---- the variant cache -----------------------------------------------------
 *
 * Keyed like `gx_tev.c`'s configurations: hash the key, walk a short chain,
 * compile on a miss.  A dead variant -- one the driver would run in software --
 * stays in the table with `ok = 0` so it is diagnosed once and then costs a
 * lookup, not a compile. */
#define VP_BUCKETS 64
#define VP_MAX_VARIANTS 256

typedef struct VpVariant {
    struct VpVariant* next;
    VpKey key;
    unsigned id;          /* the GL program object                            */
    int instr, native;
    int ok;               /* 0 = every draw with this key goes to the CPU     */
    const char* why;      /* why it is dead                                   */
    unsigned draws, verts;
} VpVariant;

#ifndef PORT_NO_SDL
static VpVariant* vp_tab[VP_BUCKETS];
static int vp_nvariants;
static unsigned vp_bound;     /* the program currently bound, 0 = none        */
static int vp_enabled;        /* GL_VERTEX_PROGRAM_ARB is on                  */
static unsigned stat_gpu_draws, stat_gpu_verts;
static unsigned stat_cpu_draws, stat_cpu_verts;
static unsigned stat_compiles, stat_dead;
static unsigned stat_pal_batches, stat_pal_uploads, stat_pal_rows;
static unsigned frame_cpu_draws, frame_gpu_draws;
static unsigned worst_frame_cpu;

static u32 vp_hash(const VpKey* k) {
    const u8* p = (const u8*)k;
    size_t i;
    u32 h = 2166136261u;
    for (i = 0; i < sizeof(VpKey); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static VpVariant* vp_lookup(const VpKey* k) {
    u32 h = vp_hash(k) & (VP_BUCKETS - 1);
    VpVariant* v;
    VpBuf b;
    GLint errpos = -1;
    GLuint id = 0;

    for (v = vp_tab[h]; v; v = v->next) {
        if (memcmp(&v->key, k, sizeof(VpKey)) == 0) {
            return v;
        }
    }
    if (vp_nvariants >= VP_MAX_VARIANTS) {
        return NULL; /* pathological; the CPU path is always correct */
    }
    v = (VpVariant*)calloc(1, sizeof(VpVariant));
    if (!v) {
        return NULL;
    }
    v->key = *k;
    v->next = vp_tab[h];
    vp_tab[h] = v;
    vp_nvariants++;

    memset(&b, 0, sizeof(b));
    vp_gen(k, &b);
    if (!b.s) {
        v->why = "out of memory generating the text";
        stat_dead++;
        return v;
    }
    v->instr = b.instr;
    stat_compiles++;

    if (k->nlights > VPE_NLIGHTS) {
        v->why = "more lights than the parameter block holds (VPE_NLIGHTS)";
        stat_dead++;
        port_log("port> vprog: variant %d wants %d lights, the block holds %d -- CPU "
                 "fallback\n", vp_nvariants, k->nlights, VPE_NLIGHTS);
        free(b.s);
        return v;
    }
    /* Refuse before asking the driver, when the count alone settles it: the
     * generator knows how many instructions it wrote and the probe knows how
     * many the card has. */
    if (b.instr > vpl.max_native_instr) {
        v->why = "over the card's native instruction limit";
        stat_dead++;
        if (port_opt.vprogstats || port_opt.vproglog) {
            port_log("port> vprog: variant %d needs %d instructions, the card has "
                     "%d -- CPU fallback\n",
                     vp_nvariants, b.instr, vpl.max_native_instr);
        }
        free(b.s);
        return v;
    }

    vp_GenProgramsARB(1, &id);
    vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, id);
    vp_ProgramStringARB(VP_VERTEX_PROGRAM_ARB, VP_PROGRAM_FORMAT_ASCII_ARB,
                        (GLsizei)b.len, b.s);
    glGetIntegerv(VP_PROGRAM_ERROR_POSITION_ARB, &errpos);
    if (errpos != -1) {
        const char* msg = (const char*)glGetString(VP_PROGRAM_ERROR_STRING_ARB);
        port_log("port> vprog: variant %d REJECTED at char %d: %s\n", vp_nvariants,
                 (int)errpos, msg ? msg : "(no message)");
        port_log("%s", b.s);
        v->why = "the driver rejected the program text";
        stat_dead++;
        vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, 0);
        vp_DeleteProgramsARB(1, &id);
        free(b.s);
        return v;
    }
    v->native = vp_geti(VP_PROGRAM_NATIVE_INSTRUCTIONS_ARB);
    if (!vp_geti(VP_PROGRAM_UNDER_NATIVE_LIMITS_ARB)) {
        v->why = "the driver would run it in software";
        stat_dead++;
        vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, 0);
        vp_DeleteProgramsARB(1, &id);
        free(b.s);
        return v;
    }
    v->id = id;
    v->ok = 1;
    vp_bound = id;
    if (port_opt.vproglog) {
        port_log("port> vprog: variant %d, %d instructions (%d native):\n%s",
                 vp_nvariants, v->instr, v->native, b.s);
    } else if (port_opt.vprogstats) {
        port_log("port> vprog: variant %d compiled, %d instructions (%d native), "
                 "lit %d, %d lights, %d units\n",
                 vp_nvariants, v->instr, v->native, k->lit, k->nlights, k->nunits);
    }
    free(b.s);
    return v;
}
#endif /* !PORT_NO_SDL */

/* ---- the draw -------------------------------------------------------------- */

/* glc_invalidate's counterpart: the program binding, the enable and the whole
 * parameter shadow are GL state the cache cannot see, so they are forgotten
 * whenever it forgets everything else. */
void gx_vprog_invalidate(void) {
#ifndef PORT_NO_SDL
    memset(env_valid, 0, sizeof(env_valid));
    vp_bound = 0;
    vp_enabled = -1; /* neither on nor known off: the next draw states it */
#endif
}

void gx_vprog_disable(void) {
#ifndef PORT_NO_SDL
    if (vp_enabled != 0) {
        vp_enabled = 0;
        if (vpl.have) {
            glDisable(VP_VERTEX_PROGRAM_ARB);
        }
    }
#endif
}

void gx_vprog_frame_reset(void) {
#ifndef PORT_NO_SDL
    if (frame_cpu_draws > worst_frame_cpu) {
        worst_frame_cpu = frame_cpu_draws;
    }
    frame_cpu_draws = frame_gpu_draws = 0;
#endif
}

#ifndef PORT_NO_SDL
static void vp_build_key(const GxXfDesc* d, VpKey* k, int* nlights_out,
                         int lightidx[8]) {
    const GXChanCtrl* cc = &gx.chan[0];
    int i, u, nl = 0;

    memset(k, 0, sizeof(*k));
    k->have_nrm = (u8)(d->have_nrm != 0);
    k->lit = (u8)(d->chan_mode == 2);
    k->pal = (u8)(d->pal_n > 0 && !port_opt.palnoarl);
    k->fog = (u8)(gx.fog_type != GX_FOG_NONE);
    if (k->lit) {
        k->mat_reg = (u8)(cc->mat_src == GX_SRC_REG);
        k->amb_reg = (u8)(cc->amb_src == GX_SRC_REG);
        k->diff_fn = cc->diff_fn;
        k->attn_fn = cc->attn_fn;
        for (i = 0; i < 8; i++) {
            if ((cc->light_mask & (1u << i)) && gx.light[i].used) {
                lightidx[nl++] = i;
            }
        }
    }
    k->nlights = (u8)nl;
    *nlights_out = nl;
    if (k->lit && d->hilite) {
        /* M21: the packed light list is the union of both channels' lights
         * (hsfdraw.c gives both the same mask, so usually the same list),
         * channel 0's first; each channel reads its own subset by mask */
        const GXChanCtrl* c1 = &gx.chan[GX_COLOR1];
        int j;
        k->hilite = (u8)d->hilite;
        k->mat1_reg = (u8)(c1->mat_src == GX_SRC_REG);
        k->amb1_reg = (u8)(c1->amb_src == GX_SRC_REG);
        k->l0mask = (u8)((1u << nl) - 1u);
        for (i = 0; i < 8; i++) {
            if ((c1->light_mask & (1u << i)) && gx.light[i].used) {
                for (j = 0; j < nl; j++) {
                    if (lightidx[j] == i) {
                        break;
                    }
                }
                if (j == nl && nl < 8) {
                    lightidx[nl++] = i;
                }
                if (j < 8) {
                    k->l1mask |= (u8)(1u << j);
                }
            }
        }
        k->nlights = (u8)nl;
        *nlights_out = nl;
    }

    for (i = 0; i < d->ntexgen && i < GX_TEXCOORDS; i++) {
        k->tg_kind[i] = d->tg[i].src_kind;
        k->tg_k[i] = d->tg[i].src_k;
        k->tg_div[i] = d->tg[i].divide;
        k->vtxdiv = (u8)port_opt.vtxdivide;
        k->viewtg = (u8)port_opt.viewtexgen;
        k->tg_mtx[i] = (u8)(d->tg[i].mtx != NULL);
    }
    /* exactly draw_run's own unit -> texgen mapping, so the two paths bind
     * the same thing */
    for (u = 0; u < gl13_max_tex_units && u < GX_TEX_UNITS; u++) {
        int stage = u < gx.num_tev ? u : -1;
        if (stage >= 0 && gx.tev[stage].coord < d->ntexgen &&
            gx_bound_tex(gx.tev[stage].map) != NULL) {
            k->unit_on[u] = 1;
            k->unit_tg[u] = gx.tev[stage].coord;
            k->nunits++;
        }
    }
}
#endif

/* The decision, and only the decision.  It has to be made before phase 2 runs
 * -- not running phase 2 is the entire point -- but everything that *reads GL
 * state* has to wait, because `draw_run` has not applied the draw's texture
 * state yet.  `gx_vprog_bind` is the second half. */
static VpKey pending_key;
static VpVariant* pending_var;
static int pending_nl;
static int pending_lightidx[8];

int gx_vprog_draw(const GxXfDesc* d, int nverts) {
#ifdef PORT_NO_SDL
    (void)d;
    (void)nverts;
    return 0;
#else
    VpVariant* v;
    int nl = 0;

    if (!gx_vprog_available() || port_opt.cpuxf) {
        return 0;
    }
    vp_build_key(d, &pending_key, &nl, pending_lightidx);
    pending_nl = nl;
    v = vp_lookup(&pending_key);
    if (!v || !v->ok) {
        stat_cpu_draws++;
        stat_cpu_verts += (unsigned)nverts;
        frame_cpu_draws++;
        pending_var = NULL;
        return 0;
    }
    pending_var = v;
    v->draws++;
    v->verts += (unsigned)nverts;
    stat_gpu_draws++;
    stat_gpu_verts += (unsigned)nverts;
    frame_gpu_draws++;
    return 1;
#endif
}

/* The second half: run from `draw_run` *after* `gl13_apply_transform`,
 * `gl13_apply_raster_state` and `gx_tev_apply`, because two of the things it
 * needs are set by them.
 *
 * The one that cost a witness run: `glc_get_tex_scale` is the NPOT fold
 * `gx_tex.c` puts in each unit's fixed-function GL_TEXTURE matrix at *bind*
 * time, and the bind happens inside `gx_tev_apply`.  Read a draw too early it
 * is the previous draw's fold -- and before the first bind on a unit it is the
 * shadow's zero, which multiplies every texture coordinate by nothing and
 * samples one texel of the atlas for the whole primitive.  That is what the
 * first G4 witness of this path looked like: the 3D characters correct and
 * every 2D layer -- the sky, the logo's "4", the sprites -- flat or absent. */
void gx_vprog_bind(const GxXfDesc* d) {
#ifdef PORT_NO_SDL
    (void)d;
#else
    VpKey key = pending_key;
    VpVariant* v = pending_var;
    int nl = pending_nl;
    int i, u;
    const GXChanCtrl* cc = &gx.chan[0];

    if (!v) {
        return;
    }
    if (vp_bound != v->id) {
        vp_bound = v->id;
        vp_BindProgramARB(VP_VERTEX_PROGRAM_ARB, v->id);
    }
    if (vp_enabled != 1) {
        vp_enabled = 1;
        glEnable(VP_VERTEX_PROGRAM_ARB);
    }

    /* ---- the parameters.  env4 emits only what changed, which matters:
     * consecutive draws share the lights and the texgen matrices and usually
     * differ in nothing but the position matrix. */
    if (key.pal) {
        /* M18: the palette rows filled since the last upload, in one call
         * (or one per row without EXT_gpu_program_parameters).  The rows
         * persist on the card across batches; gx_draw.c's cache decides
         * what is resident. */
        if (d->pal_dirty_hi >= d->pal_dirty_lo) {
            int lo = d->pal_dirty_lo, n = d->pal_dirty_hi - d->pal_dirty_lo + 1;
            const f32* rows = &d->pal[lo][0];
            if (vp_ProgramEnvParameters4fvEXT) {
                vp_ProgramEnvParameters4fvEXT(VP_VERTEX_PROGRAM_ARB, (GLuint)(VPE_PAL + lo),
                                              (GLsizei)n, rows);
                stat_env_set++;
            } else {
                int r;
                for (r = 0; r < n; r++) {
                    vp_ProgramEnvParameter4fvARB(VP_VERTEX_PROGRAM_ARB, (GLuint)(VPE_PAL + lo + r),
                                                 rows + 4 * r);
                    stat_env_set++;
                }
            }
            stat_pal_rows += (unsigned)n;
            stat_pal_uploads++;
        }
        stat_pal_batches++;
    } else if (vp_ProgramEnvParameters4fvEXT && !port_opt.noenvbulk) {
        /* M21 (--envbulk): the six matrix rows as one upload.  The walk's
         * 1.94M batches emitted 2.94 env params each through env4; the rows
         * of a new object all differ, so the per-row elision bought nothing
         * there and the three (or six) calls were the cost.  Compared as a
         * block against the same shadow env4 keeps. */
        const f32* m = d->pos_mtx;
        const f32* nm = d->nrm_mtx;
        float rows[6][4];
        int n = key.have_nrm ? 6 : 3;
        int r, same = 1;
        rows[0][0] = m[0]; rows[0][1] = m[1]; rows[0][2] = m[2];  rows[0][3] = m[3];
        rows[1][0] = m[4]; rows[1][1] = m[5]; rows[1][2] = m[6];  rows[1][3] = m[7];
        rows[2][0] = m[8]; rows[2][1] = m[9]; rows[2][2] = m[10]; rows[2][3] = m[11];
        if (key.have_nrm) {
            rows[3][0] = nm[0]; rows[3][1] = nm[1]; rows[3][2] = nm[2]; rows[3][3] = 0.0f;
            rows[4][0] = nm[3]; rows[4][1] = nm[4]; rows[4][2] = nm[5]; rows[4][3] = 0.0f;
            rows[5][0] = nm[6]; rows[5][1] = nm[7]; rows[5][2] = nm[8]; rows[5][3] = 0.0f;
        }
        for (r = 0; r < n; r++) {
            if (!env_valid[VPE_POSMTX + r] ||
                memcmp(env_shadow[VPE_POSMTX + r], rows[r], sizeof(rows[r])) != 0) {
                same = 0;
                break;
            }
        }
        if (same) {
            stat_env_elided += (unsigned)n;
        } else {
            for (r = 0; r < n; r++) {
                env_valid[VPE_POSMTX + r] = 1;
                memcpy(env_shadow[VPE_POSMTX + r], rows[r], sizeof(rows[r]));
            }
            vp_ProgramEnvParameters4fvEXT(VP_VERTEX_PROGRAM_ARB, (GLuint)VPE_POSMTX, (GLsizei)n,
                                          &rows[0][0]);
            stat_env_set++;
            stat_env_bulk++;
        }
    } else {
        const f32* m = d->pos_mtx;
        env4(VPE_POSMTX + 0, m[0], m[1], m[2], m[3]);
        env4(VPE_POSMTX + 1, m[4], m[5], m[6], m[7]);
        env4(VPE_POSMTX + 2, m[8], m[9], m[10], m[11]);
        if (key.have_nrm) {
            const f32* nm = d->nrm_mtx;
            env4(VPE_NRMMTX + 0, nm[0], nm[1], nm[2], 0.0f);
            env4(VPE_NRMMTX + 1, nm[3], nm[4], nm[5], 0.0f);
            env4(VPE_NRMMTX + 2, nm[6], nm[7], nm[8], 0.0f);
        }
    }
    if (key.lit) {
        if (key.mat_reg) {
            env4(VPE_MAT, cc->mat.r / 255.0f, cc->mat.g / 255.0f,
                 cc->mat.b / 255.0f, cc->mat.a / 255.0f);
        }
        if (key.amb_reg) {
            env4(VPE_AMB, cc->amb.r / 255.0f, cc->amb.g / 255.0f,
                 cc->amb.b / 255.0f, 1.0f);
        }
        for (i = 0; i < nl && i < VPE_NLIGHTS; i++) {
            const GXLight* l = &gx.light[pending_lightidx[i]];
            int lp = VPE_LIGHT + VPE_LSTRIDE * i;
            env4(lp + 0, l->pos[0], l->pos[1], l->pos[2], 1.0f);
            env4(lp + 1, l->color.r / 255.0f, l->color.g / 255.0f,
                 l->color.b / 255.0f, 1.0f);
            env4(lp + 2, l->k[0], l->k[1], l->k[2], 0.0f);
            if (key.hilite) {
                env4(lp + 3, l->a[0], l->a[1], l->a[2], 0.0f);
                env4(lp + 4, l->dir[0], l->dir[1], l->dir[2], 0.0f);
            }
        }
        if (key.hilite) {
            const GXChanCtrl* c1 = &gx.chan[GX_COLOR1];
            if (key.mat1_reg) {
                env4(VPE_MAT1, c1->mat.r / 255.0f, c1->mat.g / 255.0f, c1->mat.b / 255.0f,
                     c1->mat.a / 255.0f);
            }
            if (key.amb1_reg) {
                env4(VPE_AMB1, c1->amb.r / 255.0f, c1->amb.g / 255.0f, c1->amb.b / 255.0f, 1.0f);
            }
        }
    }
    (void)nl;
    for (i = 0; i < d->ntexgen && i < GX_TEXCOORDS; i++) {
        const f32* tm = d->tg[i].mtx;
        int tp = VPE_TEXMTX + 3 * i;
        if (!tm) {
            continue;
        }
        env4(tp + 0, tm[0], tm[1], tm[2], tm[3]);
        env4(tp + 1, tm[4], tm[5], tm[6], tm[7]);
        if (d->tg[i].divide) {
            env4(tp + 2, tm[8], tm[9], tm[10], tm[11]);
        }
    }
    for (u = 0; u < GX_TEX_UNITS; u++) {
        float su = 1.0f, sv = 1.0f, tv = 0.0f;
        if (!key.unit_on[u]) {
            continue;
        }
        glc_get_tex_fold(u, &su, &sv, &tv);
        env4(VPE_TEXSCL + u, su, sv, 0.0f, tv);
    }

    /* ---- the arrays.  The *source* layout is the vertex format now: no
     * output buffer is written at all, which is the point. */
    glc_vertex_array(d->base, d->stride);
    glc_color_array(d->base + d->off_clr, d->stride);
    glc_normal_array(key.have_nrm ? d->base + d->off_nrm : NULL, d->stride);
    glc_fogcoord_array((d->pal_n > 0 && !port_opt.palnofog) ? d->base + d->off_skin : NULL,
                       d->stride);
    for (u = 0; u < gl13_max_tex_units && u < GX_TEX_UNITS; u++) {
        /* the program reads vertex.texcoord[k] for the raw coordinate a texgen
         * names, so the arrays are indexed by *source* slot, not by unit */
        if (u < d->ntex) {
            glc_coord_array(u, d->base + d->off_tex + 8 * u, d->stride);
        } else {
            glc_coord_array(u, NULL, 0);
        }
    }

#endif
}

void gx_vprog_report(void) {
#ifndef PORT_NO_SDL
    int h;
    double tot;
    if (!vpl.have) {
        port_log("port> vprog: not available on this driver\n");
        return;
    }
    tot = (double)stat_gpu_draws + (double)stat_cpu_draws;
    port_log("port> vprog: %u variants (%u compiled, %u dead), "
             "%u draws on the GPU path / %u on the CPU (%.2f%% GPU)\n",
             (unsigned)vp_nvariants, stat_compiles, stat_dead, stat_gpu_draws,
             stat_cpu_draws, tot > 0 ? 100.0 * stat_gpu_draws / tot : 0.0);
    tot = (double)stat_gpu_verts + (double)stat_cpu_verts;
    port_log("port> vprog: %u vertices on the GPU / %u on the CPU (%.2f%% GPU); "
             "worst frame %u CPU draws\n",
             stat_gpu_verts, stat_cpu_verts,
             tot > 0 ? 100.0 * stat_gpu_verts / tot : 0.0, worst_frame_cpu);
    port_log("port> vprog: env params %u emitted (%u of them M21 bulk matrix uploads), %u elided\n",
             stat_env_set, stat_env_bulk, stat_env_elided);
    if (stat_pal_batches) {
        port_log("port> vprog: palette on %u batches: %u uploads of %u rows (%.1f rows a "
                 "batch; %d slots of %d rows)\n",
                 stat_pal_batches, stat_pal_uploads, stat_pal_rows,
                 (double)stat_pal_rows / stat_pal_batches, vpl.pal_slots, GX_PAL_STRIDE);
    }
    for (h = 0; h < VP_BUCKETS; h++) {
        VpVariant* v;
        for (v = vp_tab[h]; v; v = v->next) {
            if (v->ok) {
                port_log("port>   variant lit=%d lights=%d units=%d texgen=%d: "
                         "%d instr (%d native), %u draws, %u verts\n",
                         v->key.lit, v->key.nlights, v->key.nunits,
                         v->key.tg_mtx[0], v->instr, v->native, v->draws,
                         v->verts);
            } else {
                port_log("port>   variant lit=%d lights=%d units=%d DEAD: %s "
                         "(%d instructions)\n",
                         v->key.lit, v->key.nlights, v->key.nunits,
                         v->why ? v->why : "?", v->instr);
            }
        }
    }
#endif
}
