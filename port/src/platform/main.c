/* The port's entry point.
 *
 * Bring up MEM1, ARAM, the disc and VI, then hand control to the game's own
 * `main()` -- renamed to `mp4_game_main` by -Dmain=mp4_game_main so the host's
 * entry point can keep the name -- and never come back: the game's frame loop
 * is `while (1)`, and the port leaves it through OSPanic, OSResetSystem or
 * --frames.
 *
 * The game runs on a stack the port allocated next to MEM1, not on the host's
 * thread stack, so that every stack pointer it truncates into a u32 lives in
 * one known 4 GB window.  See port/src/os/jmp_arm64.s.
 */
#include "port.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif

/* The process's resident set, in MB, for the status line (M18).  A number that
 * only grows across a soak names what the long-running process accumulates;
 * one that does not says the slowdown is somewhere else. */
unsigned port_rss_mb(void) {
#ifdef __APPLE__
    struct task_basic_info info;
    mach_msg_type_number_t n = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &n) ==
        KERN_SUCCESS) {
        return (unsigned)(info.resident_size / (1024 * 1024));
    }
#endif
    return 0;
}

PortOptions port_opt;
static int water_set; /* M44: --water given on the command line */
static int waterlook_set; /* M45: --waterlook given on the command line */

void port_log_open(const char* path);
void* port_game_stack_top(void);
void port_call_on_stack(void (*fn)(void), void* stack_top);
void port_dvd_stats(void);
void port_crash_handler_install(void);
void port_watchdog_arm(int seconds);
void gx_tex_set_validate_every_bind(int v);
void gx_tex_set_budget_mb(int mb);
void port_sincos_report(void);
void port_fast_sincos_report(void);
void port_mtx_memo_report(void);
void port_motion_exec_report(void);
void port_vtx_rewrite_report(void); /* M43: src/os/vtx_rewrite.c */
void port_wb_report(void);
void port_matwalk_report(void);
void port_curve_memo_report(void);
void port_sparse_report(void);
void port_fastsqrt_report(void);

#ifdef PORT_DEADCODE
/* M19 (PLAN.md 34.4): a function nothing calls, so that a second build
 * differs from the first only in code that never runs -- the control arm of
 * the cross-build .wav question.  Built with
 *   port/build-ppc.sh BUILD=build-ppc-dead TUNE="-mcpu=7450 -mtune=7450 -mno-altivec -DPORT_DEADCODE" */
void port_deadcode_probe(void) { port_log("port> dead code, never printed\n"); }
#endif

static void usage(const char* argv0) {
    fprintf(stderr,
            "Mario Party 4 PowerPC Edition " PORT_VERSION_STRING " (milestone " PORT_MILESTONE ")\n"
            "\n"
            "usage: %s --image <disc.iso | dir> [options]\n"
            "\n"
            "  --image PATH      the user's own disc image, or a directory holding an\n"
            "                    extracted files/ tree.  The game resolves all 138 data\n"
            "                    files at boot and panics on the first miss.  Without it,\n"
            "                    $MARIOPARTY4_IMAGE, then the .app's Contents/Resources,\n"
            "                    then ~/MarioParty4 are searched for an .iso or files/.\n"
            "  --log PATH        also write the OSReport narration to PATH\n"
            "  --frames N        stop after N retraces and print the stub report\n"
            "  --watchdog SEC    give up after SEC seconds and report where\n"
            "  --turbo           run the host loop flat out instead of at 60 Hz\n"
            "                    (every frame drawn: the renderer measurement)\n"
            "  --realtime        the default: the retrace at 60 Hz on the wall\n"
            "                    clock, the picture at most every 2nd retrace and\n"
            "                    at least every 6th, frames consumed but not\n"
            "                    drawn when the renderer is behind (PLAN.md 32)\n"
            "  --lockstep        every frame drawn and the game as slow as the\n"
            "                    renderer: the gate every build before M17 had\n"
            "  --maxskip N       at most N consumed frames between drawn ones (5)\n"
            "  --perfdump FILE   every per-frame --perf sample as CSV\n"
            "  --mpgl            Apple's multithreaded GL engine: the driver's own\n"
            "                    work on the second CPU (kCGLCEMPEngine)\n"
            "  --olddecode2      convert 8-bit vertex components in the loop instead\n"
            "                    of through the byte tables (pre-M17), for the A/B\n"
            "  --altivec         the AltiVec PSMTXROMultVecArray: bit-exact with the\n"
            "                    scalar body and measured 3.5%% slower, so off (PLAN.md 32)\n"
            "  --nodcbt          no dcbt of the next vertex's arrays in those loops\n"
            "  --noprefetch      M36: no read-ahead of a dealt minigame's files at the\n"
            "                    roulette (nor the board's at the results, the board\n"
            "                    set at the mode select); the game's own reads stay\n"
            "                    exactly where they were either way\n"
            "  --resident MB     M36: the resident set -- the files every game reads,\n"
            "                    held in memory and served from there; 0 = off, default\n"
            "                    by the installed RAM (0 under 768 MB, 128 at 1 GB, 256\n"
            "                    at 1.5 GB and up), never less than 384 MB left over\n"
            "  --dvdlog          M36: one line per DVD read (frame, file, range, ms, source)\n"
            "  --cpuskin         the game's own CPU skinning at every EnvelopeProc\n"
            "                    (M18 A/B; default: the same skinning, run only when\n"
            "                    a drawn frame first needs it -- never on a consumed\n"
            "                    frame)\n"
            "  --palette         the vertex-program matrix palette (batches span\n"
            "                    GXLoadPosMtxImm, the card skins); measured slower on\n"
            "                    this driver, so opt-in (PLAN.md 33.2)\n"
            "  --palsize N       palette slots per batch (default: what fits, 24)\n"
            "  --skinstats       per-mesh envelope shapes and per-frame skin counts\n"
            "  --noskinlifetime  the M18 skinning registry: entries not dropped at the\n"
            "                    game's frees, no guards (reproduces the M18 fault)\n"
            "  --skindeferall    defer the bone walk as well as the vertex skinning\n"
            "                    (not exact: the draw walk's matrix stack reaches game\n"
            "                    logic; PLAN.md 33.3)\n"
            "  --mixtrace FILE   write every voice's mixer inputs per DSP frame to FILE\n"
            "                    (diff two runs' files: the first line names the field)\n"
            "  --mixcheck        check the mixer's 32-bit arithmetic against the 64-bit\n"
            "                    form on every sample (M18 item 3)\n"
            "  --nosincos        PSMTXRotRad's sinf/cosf straight to libm instead of\n"
            "                    through the exact memo (M18 A/B)\n"
            "  --nomatwalk       M28 (a): hsfdraw's material setup decided on consumed\n"
            "                    frames as written (default: skipped, its two game-\n"
            "                    visible side effects kept; PLAN.md 43)\n"
            "  --curvememo       M28 (b): GetCurve memoised on (track, time, start);\n"
            "                    exact, measured slower, off (--nocurvememo)\n"
            "  --nosparsemtx     M28 (c): SetEnvelopMtx's four concats through the\n"
            "                    general PSMTXRotRad + PSMTXConcat (default: sparse)\n"
            "  --fastsqrt        M28 (d): VECMag/VECNormalize/VECDistance's sqrtf\n"
            "                    through frsqrte + Newton, correctly rounded (every\n"
            "                    float checked against libm); as fast as libm, so\n"
            "                    off (--nofastsqrt); --matwalk/--sparsemtx turn (a)\n"
            "                    and (c) back on\n"
            "  --snapsync        write snapshots synchronously on the game thread\n"
            "                    (default: copied at the retrace, written by a worker)\n"
            "  --threads N       M24: 0 = every job inline on the game thread (the\n"
            "                    single-core path, today's code); 1 = the workers on,\n"
            "                    on any machine; default: on when hw.ncpu > 1\n"
            "  --nomixthread     M24: the mixer stays on the game thread\n"
            "  --renderthread N  M27: 0 = the direct GL path; 1 = the GL stream replayed\n"
            "                    inline (the single-core twin); 2 = the render thread,\n"
            "                    joined every frame; 3 = overlapped (default with cpu 2)\n"
            "  --norenderthread  M27: --renderthread 0\n"
            "  --rtgate MS       M27: the gate waits this long for the render thread (4)\n"
            "  --rtdecode N      M29: the display-list decode 0 on the game thread, 1 on the\n"
            "                    render thread joined at once, 2 joined at the retrace, auto (M33:\n"
            "                    a share per drawn frame on the game thread so the two threads'\n"
            "                    frames balance; the default with a render thread)\n"
            "  --rtauto-fit MS   M33: auto moves nothing while the render thread's frame is under MS (30)\n"
            "  --rtauto-max F    M33: the largest share auto moves to the game thread (0.75)\n"
            "  --stackmul N      M29: the coroutine stacks' multiplier over the game's sizes (2; M31 soak)\n"
            "  --rtsplit         M27: the replay timed by record class (an instrument)\n"
            "  --predecode       M24: the texture decode staged on the worker from\n"
            "                    consumed frames (measured, off: PLAN.md 39.3)\n"
            "  --predecodelog    M24: a line per drawn frame that took a staged decode\n"
            "  --machinecheck    M25: print the machine inventory (CPU, RAM, OS, GL\n"
            "                    renderer, VRAM, every extension the port uses) and\n"
            "                    the verdict; exit 0 ok / 1 degraded / 2 unsupported\n"
            "  --force           M25: run on an `unsupported' verdict anyway\n"
            "  --fake-machine F  M25: override the probes from F (key=value lines:\n"
            "                    model ncpu mhz cputype cpusubtype altivec ram_mb os\n"
            "                    native gl gl_vendor gl_renderer gl_version\n"
            "                    gl_extensions vram_mb texunits maxtexsize)\n"
            "  --texbudget MB    GL texture bytes the cache may hold before it evicts\n"
            "                    least-recently-bound entries (default 40; 0 = never,\n"
            "                    the pre-M18 behaviour that paged the 64 MB card)\n"
            "  --olddecode3      the general plan walker for every primitive instead\n"
            "                    of the specialised loops for the common shapes\n"
            "  --decodestats     per decoded vertex: do its indexed attributes share\n"
            "                    one index, and was its tuple already decoded this frame\n"
            "  --audiolead MS    silence queued ahead of the mix when pacing\n"
            "                    starts, so an overrunning frame does not starve\n"
            "                    the device (100 under --realtime)\n"
            "  --gxlog           log every GX call, not just the first of each\n"
            "  --stub-trace      log every stub call, not just the first of each\n"
            "  --deterministic   fixed 60 Hz tick and no wall-clock pacing\n"
            "  --seed N          the deterministic clock's origin: the RNG seed\n"
            "  --rtc SECS        the same origin written as the console's real-time\n"
            "                    clock, in Unix seconds -- the number Dolphin calls\n"
            "                    CustomRTCValue.  `--rtc dolphin' is the pinned\n"
            "  --rtcoffset SECS  shift that origin, so the port reaches\n"
            "                    BoardRandInit at the console's reading rather\n"
            "                    than 300 frames earlier.  1 frame = 1/60 s.\n"
            "  --memmap          print the MEM1/ARAM/stack map and its guards\n"
            "  --guardtest WHERE mem1-hi|mem1-lo|aram-hi|aram-lo|stack-lo: write\n"
            "                    one byte past that edge and expect a fault\n"
            "                    reference value (1041472800, 2003-01-02).  Implies\n"
            "                    --deterministic, and both of the game's RNG seed\n"
            "                    sites read it through OSGetTime\n"
            "  --card FILE       use FILE as slot A's 512 KB card image\n"
            "  --freshcard       format the card image at boot, so the save file's\n"
            "                    played-minigame set does not carry between runs.\n"
            "                    With no --card it formats a scratch image, never\n"
            "                    the player's own save file\n"
            "  --verbose         print each stub the first time it is called\n"
            "\n"
            "  --reldir DIR      where the 99 REL bundles live (default: <exe dir>/rels)\n"
            "  --reltest         load and unload all 99 REL bundles twice and report\n"
            "  --gxdemo          draw the GX self-test frame instead of the game\n"
            "  --relzerobss      always zero a module's bss by hand on load\n"
            "  --nodatareset     do not put a re-opened module's .data back to its\n"
            "                    on-disc contents (the pre-M20 loader: m406dll's\n"
            "                    intro countdown then starts a second play below 0)\n"
            "  --reldlclose      really dlclose a REL when the game unlinks it.  The\n"
            "                    port keeps it mapped by default, because the game\n"
            "                    calls into bootDll after unlinking it and the\n"
            "                    console's freed heap is still executable\n"
            "  --noaudio         HuAudInit and msm succeed as silent stubs\n"
            "  --wav FILE        capture the 32 kHz stereo mix to a WAV file\n"
            "  --mute            mix and time it as usual; emit silence\n"
            "  --audiolog        narrate MusyX stream, voice and studio events\n"
            "  --headless        decode and log GX, but open no window\n"
            "  --glcheck         assert that no GL call leaves the GL 1.3 subset\n"
            "  --glinfo          dump the driver's GL strings, limits and extensions\n"
            "  --vprobe          dump the ARB_vertex_program limits and a trivial load\n"
            "  --cpuxf           phase 2 on the CPU (the pre-M11 path), for the A/B\n"
            "  --vprogstats      GPU-path vs CPU-fallback draws, and why a variant died\n"
            "  --perf            per-frame game/gx/present timing, both clocks\n"
            "  --gxsplit         ...and the gx time split into its regions (M21)\n"
            "  --drawlog N       explain the first N draws: geometry, texture, state\n"
            "  --drawlog-at F    ...but only on presented frame F, which is how you\n"
            "  --oldnulltev      drop a TEV stage that names no texture (the pre-M15\n"
            "                    path), for the A/B on the character eyes\n"
            "  --oldsubmit       one glDrawArrays per GX primitive from a plain\n"
            "                    client-memory buffer (the pre-M16 submit), for\n"
            "                    the A/B (PLAN.md 31)\n"
            "  --novar           batch and merge draws but keep the vertex ring\n"
            "                    out of GL_APPLE_vertex_array_range\n"
            "  --nomultidraw     one glDrawArrays per strip, no glMultiDrawArraysEXT\n"
            "  --nohilite        M21: the specular (hilite) channel unlit, as before\n"
            "  --noindexed       M21: strips/fans through multi-draw, not one\n"
            "                    glDrawRangeElements per batch (--indexed restores)\n"
            "  --noenvbulk       M21: matrix rows as separate env-param calls\n"
            "  --nofixbase       M21: vertex arrays based at the batch, not at the\n"
            "                    ring's start (--fixbase restores)\n"
            "  --submitstats     batches, merged draws, primitives per list,\n"
            "                    ring fence waits\n"
            "  --endlog F[,F..]  M37: name every batch end of the drawn frames listed:\n"
            "                    the setter that ended it, and hashes of the state, the\n"
            "                    layout and the matrices it was submitted under.\n"
            "                    port/tools/m37_ends.py reads it (PLAN.md 52)\n"
            "  --nohilitetex     M22: the textured highlight and the mask/reflect\n"
            "                    register triple (the results portraits) as before\n"
            "  --nocopyhalf      M23: a half-scale EFB copy (the shadow map) copies\n"
            "                    its bottom-left quarter at 1:1 as before PLAN.md 38\n"
            "  --copylog         M24b: one line per GXCopyTex (frame, rectangle, format,\n"
            "                    clear, front buffer): every copy consumer on a walk\n"
            "  --noefbflip       M24b: an EFB copy is sampled in GL's row order as\n"
            "                    before PLAN.md 39b (t = 0 the bottom of the region:\n"
            "                    every copy drawn back upside down, m416's mirror)\n"
            "  --oldfirsthash    M23: a texture's first sight stores the exhaustive hash\n"
            "                    the next frame's sampled one never matches (the\n"
            "                    pre-M23 double decode of every texture over 1 KB)\n"
            "  --norekey         M23: a texture the cache already holds under another\n"
            "                    address is decoded and uploaded again as before\n"
            "  --dumpcopy        M23: the first eight copy read-backs (m415's canvas)\n"
            "                    as copy-*.ppm (the GL texels) and canvas-*.pgm (the bytes)\n"
            "  --notint          M23: a tinted lerp-by-konst pair with K_c = 1 drops its\n"
            "                    tint as M22 did (the mode select's 0.95 grey)\n"
            "  --tintlog         M23: one line per frame that drew a tinted lerp-by-konst\n"
            "                    pair (the tint the M22 rewrite drops), with the tint\n"
            "  --texdecodelog    M23: a line for every frame that spent over 20 ms\n"
            "                    decoding textures (needs --perf/--status)\n"
            "  --lazyflush       M22: a state setter applies the pending batch's state\n"
            "                    to GL and the next primitive decides whether the\n"
            "                    batch ends (measured: slower, PLAN.md 37; off)\n"
            "  --premerge-max N  M22: an object of up to N vertices differing from\n"
            "                    the pending batch in its matrices alone is\n"
            "                    transformed on the CPU into the batch's model\n"
            "                    space and appended; implies --lazyflush\n"
            "                    (measured: slower, PLAN.md 37; default 0 = off)\n"
            "  --nospot          M30: GXInitLightSpot's cone ignored, every spot light\n"
            "                    lights all round as before (m427's headlamps)\n"
            "  --oldfog          M30: the pre-M30 fog: a degenerate GXSetFog range\n"
            "                    (end == start) still fogs (m414's cyan), and the\n"
            "                    exponential shape is GL's of |z| (PLAN.md 45)\n"
            "  --noregchain      M30: a TEV stage reading the register the previous\n"
            "                    stage wrote gets the register's constant, as before\n"
            "                    (m427's flooded cave), for the A/B\n"
            "  --nocarry         M31: no scalar-in-alpha fold (m417's pool: the water black)\n"
            "  --forceobj N[:f]  M31 diagnostic: object N's draws without cull 1 / z test 2 / alpha test 4\n"
            "  --zprepass N      M33: a depth-only pass before a z-writing draw whose alpha test\n"
            "                    kills fragments: 1 = under GXSetZCompLoc(TRUE), where the hardware\n"
            "                    writes their Z (the default since M34, PLAN.md 49.2), 2 = every\n"
            "                    such draw (the console disagrees: the title's ribbon), 0 = never\n"
            "  --skipobj N       M33 diagnostic: object N's draws are not issued at all\n"
            "  --probeobj N      M33 diagnostic: object N's draws bracketed by a framebuffer read-back\n"
            "  --probeverts N    M34: --probeobj prints up to N of a draw's vertices as bound (6)\n"
            "  --skipverts N     M34 diagnostic: draws of exactly N vertices are not issued (a\n"
            "                    hook's draw has no object name for --skipobj)\n"
            "                    (pixels changed) and a print of the GL state and vertices as issued\n"
            "                    (M32: blend off 8, colour+alpha update forced on 16)\n"
            "  --nolinewidth     M30: ignore GXSetLineWidth, every line one pixel\n"
            "                    wide as before (m428's rope), for the A/B\n"
            "  --noregfix        fold a TEV stage's GX_TEVREG write to PREV (the\n"
            "                    pre-M16 path; the board eyes), for the A/B\n"
            "  --oldkonst        claim a unit's GL constant whole instead of RGB\n"
            "                    and A separately (the pre-M16 path), for the A/B\n"
            "  --tfs             M35: the indirect warp (Mario Medley's caustic, Makin'\n"
            "                    Waves' and Cheep Cheep Sweep's ripples) as a fragment program\n"
            "                    on GL_ATI_text_fragment_shader (the Radeon 9000 has it); OFF:\n"
            "                    the Leopard driver samples two games' EFB copies wrong through\n"
            "                    it (PLAN.md 50.13); --notfs is the default\n"
            "  --tfsprobe        compile the warp programs, draw a read-back test, print, quit\n"
            "  --tfslog          every fragment program compiled: its text and constants\n"
            "  --tfsall N        a diagnostic: every draw of N or more TEV stages through the\n"
            "                    fragment shader, warp or not (the fixed-function chain's A/B)\n"
            "  --oldspec0        M35: GX_AF_SPEC on a colour channel read as the distance\n"
            "                    attenuation (The Great Deflate's Thwomps opaque), for the A/B\n"
            "  --nolitalpha      M35: the alpha channel unlit -- the material's alpha, not\n"
            "                    ambient + the lights' (the Thwomps' translucency), for the A/B\n"
            "  --oldczero        M35: lerp(a, b, 0) keeps b as an input (a four-input\n"
            "                    stage, the d term alone drawn: m425's sea), for the A/B\n"
            "  --clrasclr        M35: GXColor3u8's bytes filed as a colour whatever the\n"
            "                    next attribute is (the background quad after a shadow\n"
            "                    pass drew nothing), for the A/B\n"
            "  --nohoistmtx      M41: C_MTXMultVecArray / PSMTXROMultVecArray reload the\n"
            "                    matrix every vertex, as before, for the A/B\n"
            "  --nofastconcat    M41: C_MTXConcat is the SDK's own body, not the port's\n"
            "                    register-blocked one (the same arithmetic), for the A/B\n"
            "  --nostripes       M41: a texture the game rewrites is decoded and uploaded\n"
            "                    whole at every flush, not only its changed tile rows\n"
            "  --nounitmemo      M41: the TEV unit layout and register-fix shape decided\n"
            "                    at every call inside a draw (not once a draw), as before\n"
            "  --norotmemo       M41: C_MTXRotRad (every object walk's mtxRot) calls\n"
            "                    libm's sinf/cosf directly, not through the memo, as before\n"
            "  --nofastsin       M42: the sin/cos memo's misses call libm (no fast path\n"
            "                    that returns libm's own floats without it)\n"
            "  --nomtxmemo       M42: mtxRot/mtxRotCat run the game's bodies every call\n"
            "  --norotl          M42: the rotation builders' misses run the game's bodies\n"
            "                    (MTXRotRad + MTXConcat), not the sparse left product\n"
            "  --notexskip       M43: decode jobs with an unread TEX0 go to the general walker\n"
            "  --nostripepool    M43: the texture stripes allocate their buffers per update\n"
            "  --giveitem N,..   M45: on a board, hand each player with an empty slot item N\n"
            "  --waterlook L     M45: port (the user's tuning: m417's ripple x2.5, no sky) | console\n"
            "  --wavegain PCT    M45: the water warp's amplitude on every screen (100 = console)\n"
            "  --nowaterpt       M45: the water's CPU-path positions as M44 (view space, identity)\n"
            "  --affinetex       M45: the CPU path's projected texcoords divided at the vertex\n"
            "  --halfwatch N     M45: every Nth presented frame, count a half-black picture\n"
            "  --nodcbz          M44: no dcbz ahead of the render stream's / vertex ring's writes\n"
            "  --novpgen         M44: vertex-program parameters compared every draw (no generations)\n"
            "  --notevdirty      M44: the TEV signature hashed at every draw\n"
            "  --noattrmemo      M44: a primitive's layout and decode plan derived every primitive\n"
            "  --nowordhash      M44: the texture content hash byte by byte\n"
            "  --water L         M44: the water's indirect warp: off, cheap (at the game's vertices),\n"
            "                    full (subdivided), auto (the default: by machine and screen)\n"
            "  --watergrid N     M44: full's subdivision levels (default 1: four triangles each)\n"
            "  --novcpos         M44: no positions refresh of a cached run (the whole run decoded)\n"
            "  --norastermemo    M44: the transform and raster state applied at every draw\n"
            "  --nowbpart        M43: the write barrier arms only the pages wholly inside an\n"
            "                    array (M42's), not its partial end pages (the default since M43)\n"
            "  --oldvtxjoin      M43: the morph rewriters wait for the whole decode stream (M29)\n"
            "  --rtgx 0|1|auto   M43: the GX state translation on the render thread (the\n"
            "                    game thread records GX state; PLAN.md 58.2); auto per frame\n"
            "  --rtgxfit MS      M43: --rtgx auto only while the game thread's cycle is over MS\n"
            "  --vcarr A,B       M43: the vertex cache's served/missed vertices per display\n"
            "                    list, frames A..B from the minigame's entry, at exit\n"
            "  --pmcwin A,B      M43: --pmc counts frames A..B from the minigame's entry\n"
            "  --pmc N           M43: the G4's performance counters on the game thread,\n"
            "                    event set N (1..3), split by region, reported at exit\n"
            "  --nowb            M42: the vertex cache re-hashes every array each drawn\n"
            "                    frame (no write barrier on the pages it has read)\n"
            "  --nomotionexec    M42: Hu3DMotionExec is the game's own compiled body\n"
            "  --nopacklights    M41: a lit vertex-program variant over the card's\n"
            "                    instruction limit is drawn on the CPU path, as before\n"
            "                    (no program with the lights packed four to a register)\n"
            "  --nodirtyfilter   M41: every DCStoreRange/DCFlushRange scans the whole\n"
            "                    texture cache (no page filter first), for the A/B\n"
            "  --nodirty         M35: the game's DCStoreRange/DCFlushRange do not mark\n"
            "                    the texture cache (m415's stamps unseen, as before), for the A/B\n"
            "  --oldakonst       M35: an alpha read as a colour claims the unit's A\n"
            "                    half even when RGB is free (the M16..M34 claim: the\n"
            "                    shadow pass's darkness squared), for the A/B\n"
            "  --tlutlog         every GXLoadTlut and every CI texture bind: the\n"
            "                    palette address, count, format, TLUT name, swap and\n"
            "                    cache slot.  Scoped to --drawlog-at's frame if given\n"
            "  --scenelog F      the cameras and camera-bearing models on frame F\n"
            "  --ovllog          name the scene (omcurovl) every time it changes\n"
            "                    point --drawlog at a screen rather than at the boot\n"
            "  --nocard          both memory-card slots read empty.  The game then\n"
            "                    stops at SELECT A FILE, exactly as a console with no\n"
            "                    card does; without it slot A holds a 512 KB image in\n"
            "                    ~/Library/Application Support/MarioParty4/\n"
            "  --dumptex         write every decoded texture (colour + alpha) to shotdir\n"
            "  --texhash-full    hash whole textures on every bind, bypassing the\n"
            "                    validation epoch entirely (slow; a correctness check)\n"
            "  --texvalidate-every-bind\n"
            "                    keep the sampled content hash but check it on every\n"
            "                    bind instead of once per validation epoch -- for\n"
            "                    telling an epoch bug from a sampling bug\n"
            "  --gxwarn          name every GX feature the backend degraded\n"
            "  --dumpframe SPEC  write these presented frames as PPMs:\n"
            "                    N, or a,b,c, or first-last/step (e.g. 1-400/20)\n"
            "  --shotdir DIR     where --dumpframe writes (default: .)\n"
            "  --scale N         window scale over 640x480 (default 1)\n"
            "  --fullscreen      M25: the whole screen, the 640x480 picture scaled to\n"
            "                    fit and letterboxed; remembered in the config file\n"
            "  --windowed        M25: the 640x480 window (clears a remembered --fullscreen)\n"
            "  --noconfig        M25: neither read nor write ~/Library/Application\n"
            "                    Support/MarioParty4/config (the lab's runs; windowed\n"
            "                    unless --fullscreen, where a first run from the\n"
            "                    Finder is fullscreen and remembers it)\n"
            "  --defaults        M32: print every option's effective value (the block\n"
            "                    the log starts with) and exit -- for a bug report\n"
            "  --keys            M32: print the keyboard and pad table and exit\n"
            "\n"
            "  --nopad           no controller 1 at all, not even the keyboard\n"
            "  --kbport N        M34: the keyboard as controller N (1-4) of its own; the\n"
            "                    pads fill the other ports in order.  Without it the\n"
            "                    keyboard is controller 1 alone, or beside the pad on it\n"
            "  --paddbg          log raw pad reports/buttons/axes as they arrive\n"
            "  --play SCRIPT     scripted controller 1 input (port/src/pad/pad_play.c\n"
            "                    format); see port/tools/gecko2play.py to convert a\n"
            "                    port/ref/tools/mkgecko.py reference script\n"
            "  --record FILE     record controller 1's raw input in the --play format\n"
            "\n"
            "  self-play (M7):\n"
            "  --minigame N|NAME park the minigame roulette on this minigame; a\n"
            "                    comma-separated list parks on each in turn, moving\n"
            "                    on when the one in force has been played, and\n"
            "                    releasing the roulette when the list is done\n"
            "  --board N[+]      M39c: the party board the menus pick, 1-6 (Toad's,\n"
            "                    Goomba's, Shy Guy's, Boo's, Koopa's, Bowser's);\n"
            "                    N+ moves to the next board at each board a --soak\n"
            "                    chains.  The extras: --goto w10dll|w20dll|w21dll\n"
            "  --boarddump A,..  M39c: dump the frames A,.. counted from the frame the\n"
            "                    first board overlay (w01..w21) is entered\n"
            "  --com4            all four players are CPU\n"
            "  --mgdump A,B,..   M26: dump the frames A,B,.. counted from the frame the\n"
            "                    minigame module is entered (the gallery's four)\n"
            "  --mgend N         M26: quit N frames after that entry\n"
            "  --viewtexgen      M26: GX_TG_POS/NRM texgens read the view-space position\n"
            "                    and normal, as before M26 (every shadow map and\n"
            "                    reflection map was projected from the wrong space)\n"
            "  --nrmfrac0        M26: S8/S16 normals scaled by the VAT's frac (0) instead of\n"
            "                    the hardware's fixed 1.6/1.14 (raw normals 64x too long)\n"
            "  --vtxdivide       M26: projected texgens (GX_TG_MTX3x4: every shadow map)\n"
            "                    divided by q at the vertex, as before M26\n"
            "  --mghold          M25: inside a minigame's own overlay all four players\n"
            "                    are human with idle controllers (they hold still):\n"
            "                    a four-way DRAW on demand (Avalanche!, PLAN.md 35.2)\n"
            "  --cast a,b,c,d    the four characters a --com4 run plays: numbers\n"
            "                    0-7 or names (mario luigi peach yoshi wario\n"
            "                    donkey daisy waluigi).  PLAN.md 29.2\n"
            "  --dvdheap KB      resize HEAP_DVD away from the console's 5632 KB.\n"
            "                    A deliberate divergence, logged at boot; see\n"
            "                    PLAN.md 29.2 before using it for anything\n"
            "  --turns N         the board's turn count (10/20/30/50)\n"
            "  --status          one state line a second: screen, turn, minigame,\n"
            "                    coins and stars per player, aud ms, fps\n"
            "  --stuckwatch SEC  name the live screen if it has not changed in SEC\n"
            "  --soak            boot, walk into a four-CPU board, play it, and\n"
            "                    start another one when it ends, forever\n"
            "  --nodepop         switch off the voice cut-off ramp\n"
            "  --resample4       the 4-tap Catmull-Rom resampler instead of the\n"
            "                    default linear interpolation (PLAN.md 20.5)\n"
            "  --resample1       linear interpolation; the default, kept so that an\n"
            "                    A/B can name both sides\n"
            "  --clickstat       count mix discontinuities as they are produced\n"
            "  --nomovies        skip the THP movies (the opening, the mode select's,\n"
            "                    the story endings, the credits) as M2-M37 did\n"
            "  --thpyuv          draw a movie frame the game's way -- three I8\n"
            "                    planes through THPDraw's TEV -- not as the CPU's\n"
            "                    RGBA (an A/B; its colours are wrong, PLAN.md 53.4)\n"
            "  --thplog          one log line per movie frame drawn\n"
            "  --nothpslice      one CPU: no movie decode in the retrace's slack\n"
            "                    (an owed frame decoded whole at its draw, M38)\n"
            "  --thpguard MS     one CPU: a drawn frame decodes an owed movie frame\n"
            "                    only if the queued audio covers it + MS (60; 0 =\n"
            "                    always, M38) -- else the newest ready one shows\n"
            "  --foldxbar        the M30 three-texture fold reads across units with\n"
            "                    the crossbar, as before 0.9.7 (m448's felt black)\n"
            "  --foldcap N       cut the M30 three-texture fold after N units (9: N\n"
            "                    cycles 1..6 by frame; 10+N also forces the chain's\n"
            "                    last alpha to 1; 21/22: unit B's alpha from its\n"
            "                    crossbar read / its constant alone): the m448 bisect\n"
            "  --goto OVL[:EVT[:CHAR]]  boot straight into overlay OVL (a name as in\n"
            "                    the status line) at event EVT, four players set\n"
            "                    up with character CHAR first: mstory2dll:4:0 is\n"
            "                    Mario's story ending\n"
            "  --noaicb          do not call the game's AI DMA callback, which\n"
            "                    leaves msmSe/Mus/StreamPeriodicProc dead as\n"
            "                    the stub did before M9b (PLAN.md 22.4)\n"
            "  --olddecode       decode display-list vertices with the old\n"
            "                    call-per-attribute cursor instead of the\n"
            "                    per-primitive plan (PLAN.md 21.4); for A/B\n"
            "  --dlcache         replay a display list's cached decoded vertices\n"
            "                    when its bytes and the arrays it indexes have\n"
            "                    not moved.  Off: it is faster on the title and\n"
            "                    slower on the character select (PLAN.md 21.3)\n"
            "  --vcache off|count|on|auto  M40: the static-geometry cache (PLAN.md 55):\n"
            "                    a display list whose bytes, arrays and plan are\n"
            "                    unchanged since it was decoded is drawn from its\n"
            "                    decoded copy, no decode and no copy; count only\n"
            "                    measures the share; auto (the default) keys only\n"
            "                    the frames whose render thread needs it\n"
            "  --vcachefit MS    M40: auto's threshold (28 ms; --novcache off)\n"
            "  --vcachemb N      M40: the cache's region in the vertex range (8)\n"
            "  --vcachevbo       M40: the cache's region in a buffer object (VRAM);\n"
            "                    EXPERIMENTAL, wrong on the Leopard ATI driver: its\n"
            "                    batches raise GL_INVALID_OPERATION beside the\n"
            "                    vertex range and draw nothing (PLAN.md 55)\n"
            "  --cardwait        M40: a card flush waits for the running write (M29)\n"
            "  --synclog         M40: the log written on the calling thread\n"
            "  teleport to the bug (M10):\n"
            "  --oldtev          re-apply the texture environment on every draw\n"
            "  --tevstats        TEV state-cache hits and misses\n"
            "  --m444trace       log m444dll's ball, per frame, for the console diff\n"
            "  --nodraw          consume the game's GX command streams and emit\n"
            "                    no GL: no vertex decode, no texture decode, no\n"
            "                    present.  The game logic reads none of it, so\n"
            "                    the run is the same run, roughly three times\n"
            "                    faster.  The window keeps the last frame drawn\n"
            "  --ffto N          --nodraw until frame N, then switch drawing on\n"
            "                    and carry on at the normal pace: the way to be\n"
            "                    at frame N of a deterministic run in a fraction\n"
            "                    of the time.  --dumpframe N then still writes\n"
            "                    the byte-identical frame\n"
            "  --ffto-warm K     render K frames before N so the texture cache\n"
            "                    and the EFB are warm when N is drawn (default 1)\n"
            "  --snap-every K    write a snapshot every K frames\n"
            "  --snap-keep N     keep the newest N snapshots (default 3)\n"
            "  --snap-at N       one snapshot at frame N\n"
            "  --snap-dir DIR    where the ring lives (default ~/MarioParty4/snaps)\n"
            "  --restore-lax     with --restore: accept a snapshot whose build id differs\n"
            "                    -- for a re-link of the same source only (MEM1 holds\n"
            "                    code addresses; a rebuild that moves a function is\n"
            "                    unsound even when the snapmap matches)\n"
            "  --restore FILE    resume the run in FILE: same binary, same arena\n"
            "                    addresses, same modules.  Every other flag on\n"
            "                    the line still applies, so a restore can be\n"
            "                    --dumpframe'd or run under gdb\n"
            "  --snapdiff        print a per-region digest of the arenas every\n"
            "                    snapshot, to byte-diff a restored run against a\n"
            "                    straight one at the same frame\n"
            "\n"
            "  --perfwin SPEC    with --perf, also report fps over named frame\n"
            "                    windows: A-B[:NAME][,A-B[:NAME]...].  One boot\n"
            "                    then answers \"how fast is the title/the menu/the\n"
            "                    board\" instead of three\n",
            argv0);
}

/* Finding the disc without being told where it is.
 *
 * On the development Mac the image is always passed with --image.  On the G4
 * the game is launched by the isle-ppc-tools console runner, which hands the
 * bundle a fixed argument line, so the bundle has to be able to find its own
 * disc.  Three places are searched, in order, and the first `*.iso`/`*.nkit.iso`
 * or `files/` tree found wins:
 *
 *   1. $MARIOPARTY4_IMAGE          -- an explicit override, for scripts
 *   2. <the .app>/Contents/Resources  -- a self-contained bundle
 *   3. ~/MarioParty4               -- the shared folder, which survives
 *                                     replacing the .app (the same arrangement
 *                                     the Snowboard Kids ports use for ROMs)
 *
 * Nothing here is G4-specific in itself; it is just the only way a
 * double-clicked bundle can work on any Mac.
 */
static char default_image[1024];

static int dir_holds_disc(const char* dir) {
    DIR* d;
    struct dirent* e;
    struct stat st;
    char probe[1024];

    snprintf(probe, sizeof(probe), "%s/files", dir);
    if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(default_image, sizeof(default_image), "%s", dir);
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        const char* dot = strrchr(e->d_name, '.');
        if (dot && !strcasecmp(dot, ".iso")) {
            snprintf(default_image, sizeof(default_image), "%s/%s", dir, e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* M32: is this the disc?  A GameCube image starts with its game id -- GMPE01
 * for Mario Party 4 (USA) -- and carries the FST pointer at 0x424; an
 * extracted tree is a folder with files/ (or the files themselves) in it.
 * dvd_fs.c does the real open; this is the chooser's quick look. */
static int image_looks_right(const char* path, char* why, size_t n) {
    struct stat st;
    FILE* f;
    unsigned char hdr[8];
    if (stat(path, &st) != 0) {
        snprintf(why, n, "it cannot be read");
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        char probe[1100];
        snprintf(probe, sizeof(probe), "%s/files", path);
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 1;
        }
        snprintf(probe, sizeof(probe), "%s/data", path);
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 1;
        }
        snprintf(why, n, "a folder, but with no files/ tree in it");
        return 0;
    }
    if (st.st_size < 0x460) {
        snprintf(why, n, "too small to be a disc image");
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        snprintf(why, n, "it cannot be opened");
        return 0;
    }
    if (fread(hdr, 1, 6, f) != 6) {
        fclose(f);
        snprintf(why, n, "it cannot be read");
        return 0;
    }
    fclose(f);
    hdr[6] = '\0';
    if (memcmp(hdr, "GMPE01", 6) != 0) {
        if (memcmp(hdr, "GMP", 3) == 0) {
            snprintf(why, n, "a Mario Party 4 disc, but not the USA release (its id reads %s; "
                             "the port is built from GMPE01)", hdr);
        } else {
            snprintf(why, n, "not a GameCube disc image of Mario Party 4 (the first bytes read "
                             "\"%.6s\", not GMPE01)", hdr);
        }
        return 0;
    }
    return 1;
}

static const char* port_find_default_image(void) {
    const char* env = getenv("MARIOPARTY4_IMAGE");
    const char* home;
    char buf[1024];

    if (env && *env) {
        return env;
    }
#if defined(__APPLE__)
    {
        char exe[1024];
        uint32_t n = (uint32_t)sizeof(exe);
        if (_NSGetExecutablePath(exe, &n) == 0) {
            /* .../Foo.app/Contents/MacOS/isle -> .../Foo.app/Contents/Resources */
            char* slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                slash = strrchr(exe, '/'); /* strip MacOS */
                if (slash) {
                    *slash = '\0';
                    snprintf(buf, sizeof(buf), "%s/Resources", exe);
                    if (dir_holds_disc(buf)) {
                        return default_image;
                    }
                }
            }
        }
    }
#endif
    /* M25: the image the config remembers (from a --image or the dialog),
     * if it is still there */
    if (!port_opt.noconfig) {
        const char* remembered = port_config_get("image");
        if (remembered) {
            struct stat st;
            if (stat(remembered, &st) == 0) {
                snprintf(default_image, sizeof(default_image), "%s", remembered);
                port_log("port> config: disc image %s (remembered)\n", default_image);
                return default_image;
            }
            port_log("port> config: the remembered disc image %s is gone; searching\n",
                     remembered);
        }
    }
    home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, sizeof(buf), "%s/MarioParty4", home);
        if (dir_holds_disc(buf)) {
            return default_image;
        }
    }
    /* M25: nothing found: ask, once, and remember the answer.  Not for the
     * runs that never open a window (the answer would be a hang on a
     * dialog nobody can see). */
    if (!port_opt.headless && !port_opt.machinecheck && !port_opt.noconfig &&
        !port_opt.reltest && !port_opt.gxdemo) {
        /* M32: until the choice is a Mario Party 4 disc image (or cancelled);
         * a wrong file gets a notice and the chooser again, not a panic at
         * the first missing data file */
        while (port_dialog_choose_image(default_image, sizeof(default_image))) {
            char why[256];
            if (image_looks_right(default_image, why, sizeof(why))) {
                port_config_set("image", default_image);
                port_config_save();
                port_log("port> config: disc image %s (chosen; remembered)\n", default_image);
                return default_image;
            }
            port_log("port> chooser: %s is not the disc image: %s\n", default_image, why);
            {
                char text[1600];
                snprintf(text, sizeof(text),
                         "%s\n\n%s\n\nThe game wants your own dump of Mario Party 4 "
                         "(USA, Rev 1) as a .iso (an NKit .iso is fine), or a folder "
                         "holding its extracted files/ tree.  Choose again.",
                         default_image, why);
                port_dialog_notice("Mario Party 4: that is not the disc image", text);
            }
        }
    }
    return NULL;
}

int port_parse_args(int argc, char** argv) {
    int i;
    /* The two audio repairs are on by default and switched *off* by a flag, so
     * that every unadorned run -- including every run of the self-play soak --
     * exercises them, and an A/B is one word on the command line. */
    port_opt.depop = 1;
    port_opt.threads = -1; /* M24: the workers when the machine has the cores */
    port_opt.renderthread = -1; /* M27: overlapped with the workers on, inline without */
    /* M28 (PLAN.md 43.9): the material walk (a) and the sparse concats (c)
     * pay and are on; the curve memo (b) lost 0.1-0.4 ms a frame and the
     * frsqrte sqrtf (d) is exact and exactly as fast as libm's -- both off,
     * --curvememo / --fastsqrt turn them on */
    port_opt.nocurvememo = 1;
    port_opt.nofastsqrt = 1;
    port_opt.rtgate_ms = 4;
    port_opt.rtdecode = -1; /* M29: the decode on the render thread when there is one */
    port_opt.rtauto_fit_ms = 30.0; /* M33: auto's dead band, two retraces less a margin */
    port_opt.rtauto_max = 0.75;
    port_opt.stackmul = PORT_PRC_STACK_MUL;
    port_opt.zprepass = 1; /* M34: the ZCompLoc gate, the hardware's rule (PLAN.md 49.2) */
    port_opt.tfs = 0; /* M35: the indirect warp as a fragment program is built and OFF (PLAN.md 50.13:
                       * the EFB copies of two of the three games sample wrong through it on the
                       * Leopard driver; --tfs turns it on) */
    /* Linear, since the G4 measured both on the same walk (PLAN.md §20.5):
     * 1.75 ms mean against the 4-tap's 2.00, a worst frame of 10.92 ms against
     * 28.30, and fewer discontinuities, not more -- 33,002 against 36,328.
     * The 4-tap was there to buy quality and on this hardware it buys none, so
     * it is the flag now and linear is the default. */
    port_opt.resample4 = 0;
    port_opt.vcache = 3;    /* M40: the static-geometry cache, auto (PLAN.md 55) */
    port_opt.vcache_fit = 28.0;
    port_opt.rtgx = 0;       /* M43: the render thread's translation: measured, off (PLAN.md 58.2) */
    port_opt.wbpart = 1;     /* M43: the barrier on the partial end pages too (PLAN.md 58.4, 58.10) */
    port_opt.water = -1;     /* M44: auto (PLAN.md 59) */
    port_opt.watergrid = 1;
    port_opt.waterlook = 1;  /* M45: the user's tuning (PLAN.md 60) */
    port_opt.rtgx_fit = 29.0;
    port_opt.vcache_mb = 8;
    port_opt.resident = -1; /* M36: the resident set's budget by the installed RAM (machine.c) */
    port_opt.cmpmask = 8191; /* every compare-first group on; see gx_internal.h (M18:
                             * 15 left Z mode, Z comp loc, cull and alpha compare
                             * flushing unconditionally, PLAN.md 33.2) */
    /* M21 (PLAN.md 36): measured on the 9,000-frame walk and both *off*.
     * --indexed (one glDrawRangeElements per batch) is 4-12% slower than the
     * driver's multi-draw of strips and its triangle setup differs at the
     * rounding level; --fixbase and --envbulk are exact and were within noise
     * then (M44 turned both on, below). */
    port_opt.noindexed = 1;
    /* M44 (PLAN.md 59): both on.  M21 measured them level when every GL
     * call was the driver's (36.3); since M27 every call is a record the game
     * thread writes and the render thread replays, and the arrays' pointers
     * (the same base batch after batch) and the matrix rows (one record for
     * six) are records not written: m441's front end -0.28 / -0.34 M cycles
     * a drawn frame each (the counters, 59.3), and without --fixbase the GL
     * calls a drawn frame rise 30-55% where the vertex cache serves (m409,
     * m414, m418, m420 fell under the bar, 59.9).  The G4 locked up once, at a
     * board load, on a build with both on (candidate 11); --fixbase is the
     * lever that moves the addresses the card fetches, so it was taken off,
     * measured off, put back and run long in that very workload (59.9).
     * --nofixbase / --noenvbulk are the old shape. */
    port_opt.nofixbase = 0;
    port_opt.noenvbulk = 0;
    port_opt.premerge_max = 0; /* M22 (PLAN.md 37): the CPU pre-transform, measured and off */
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--image") && i + 1 < argc) {
            port_opt.image = argv[++i];
        } else if (!strcmp(a, "--log") && i + 1 < argc) {
            port_opt.log = argv[++i];
        } else if (!strcmp(a, "--watchdog") && i + 1 < argc) {
            port_opt.watchdog = atoi(argv[++i]);
        } else if (!strcmp(a, "--frames") && i + 1 < argc) {
            port_opt.max_frames = atoi(argv[++i]);
        } else if (!strcmp(a, "--turbo")) {
            port_opt.turbo = 1;
        } else if (!strcmp(a, "--realtime")) {
            port_opt.realtime = 1;
            port_opt.lockstep = 0;
        } else if (!strcmp(a, "--lockstep")) {
            port_opt.lockstep = 1;
        } else if (!strcmp(a, "--mpgl")) {
            port_opt.mpgl = 1;
        } else if (!strcmp(a, "--altivec")) {
            port_opt.altivec = 1;
        } else if (!strcmp(a, "--noaltivec")) {
            port_opt.altivec = 0;
        } else if (!strcmp(a, "--cpuskin")) {
            port_opt.cpuskin = 1;
        } else if (!strcmp(a, "--nopalette")) {
            port_opt.nopalette = 1;
        } else if (!strcmp(a, "--palette")) {
            port_opt.palette = 1;
        } else if (!strcmp(a, "--palsize") && i + 1 < argc) {
            port_opt.palsize = atoi(argv[++i]);
        } else if (!strcmp(a, "--skinstats")) {
            port_opt.skinstats = 1;
        } else if (!strcmp(a, "--palnoarl")) {
            port_opt.palnoarl = 1;
        } else if (!strcmp(a, "--palnofog")) {
            port_opt.palnofog = 1;
        } else if (!strcmp(a, "--noskinlifetime")) {
            port_opt.noskinlifetime = 1;
        } else if (!strcmp(a, "--restore-lax")) {
            port_opt.restore_lax = 1;
        } else if (!strcmp(a, "--skindeferall")) {
            port_opt.skindeferall = 1;
        } else if (!strcmp(a, "--mixtrace") && i + 1 < argc) {
            port_opt.mixtrace = argv[++i];
        } else if (!strcmp(a, "--mixcheck")) {
            port_opt.mixcheck = 1;
        } else if (!strcmp(a, "--nosincos")) {
            port_opt.nosincos = 1;
        } else if (!strcmp(a, "--nomatwalk")) {
            port_opt.nomatwalk = 1;
        } else if (!strcmp(a, "--matwalk")) {
            port_opt.nomatwalk = 0;
        } else if (!strcmp(a, "--nocurvememo")) {
            port_opt.nocurvememo = 1;
        } else if (!strcmp(a, "--curvememo")) {
            port_opt.nocurvememo = 0;
        } else if (!strcmp(a, "--nosparsemtx")) {
            port_opt.nosparsemtx = 1;
        } else if (!strcmp(a, "--sparsemtx")) {
            port_opt.nosparsemtx = 0;
        } else if (!strcmp(a, "--nofastsqrt")) {
            port_opt.nofastsqrt = 1;
        } else if (!strcmp(a, "--fastsqrt")) {
            port_opt.nofastsqrt = 0;
        } else if (!strcmp(a, "--snapsync")) {
            port_opt.snapsync = 1;
        } else if (!strcmp(a, "--threads") && i + 1 < argc) {
            port_opt.threads = atoi(argv[++i]);
        } else if (!strcmp(a, "--nomixthread")) {
            port_opt.nomixthread = 1;
        } else if (!strcmp(a, "--renderthread") && i + 1 < argc) {
            port_opt.renderthread = atoi(argv[++i]);
        } else if (!strcmp(a, "--norenderthread")) {
            port_opt.renderthread = 0;
        } else if (!strcmp(a, "--rtsplit")) {
            port_opt.rtsplit = 1;
        } else if (!strcmp(a, "--rtgate") && i + 1 < argc) {
            port_opt.rtgate_ms = atoi(argv[++i]);
        } else if (!strcmp(a, "--rtdecode") && i + 1 < argc) {
            i++;
            port_opt.rtdecode = !strcmp(argv[i], "auto") ? 3 : atoi(argv[i]);
        } else if (!strcmp(a, "--rtauto-fit") && i + 1 < argc) {
            port_opt.rtauto_fit_ms = atof(argv[++i]);
        } else if (!strcmp(a, "--rtauto-max") && i + 1 < argc) {
            port_opt.rtauto_max = atof(argv[++i]);
        } else if (!strcmp(a, "--stackmul") && i + 1 < argc) {
            port_opt.stackmul = atoi(argv[++i]);
            if (port_opt.stackmul < 1) {
                port_opt.stackmul = 1;
            }
        } else if (!strcmp(a, "--predecode")) {
            port_opt.predecode = 1;
        } else if (!strcmp(a, "--predecodelog")) {
            port_opt.predecodelog = 1;
        } else if (!strcmp(a, "--texbudget") && i + 1 < argc) {
            gx_tex_set_budget_mb(atoi(argv[++i]));
            port_opt.texbudget_set = 1;
        } else if (!strcmp(a, "--fake-machine") && i + 1 < argc) {
            port_opt.fake_machine = argv[++i];
        } else if (!strcmp(a, "--machinecheck")) {
            port_opt.machinecheck = 1;
        } else if (!strcmp(a, "--force")) {
            port_opt.force = 1;
        } else if (!strcmp(a, "--mgdump") && i + 1 < argc) {
            port_opt.mgdump = argv[++i];
        } else if (!strcmp(a, "--mgend") && i + 1 < argc) {
            port_opt.mgend = atoi(argv[++i]);
        } else if (!strcmp(a, "--viewtexgen")) {
            port_opt.viewtexgen = 1;
        } else if (!strcmp(a, "--nrmfrac0")) {
            port_opt.nrmfrac0 = 1;
        } else if (!strcmp(a, "--vtxdivide")) {
            port_opt.vtxdivide = 1;
        } else if (!strcmp(a, "--mghold")) {
            port_opt.mghold = 1;
        } else if (!strcmp(a, "--fullscreen")) {
            port_opt.fullscreen = 1;
            port_opt.fullscreen_set = 1;
        } else if (!strcmp(a, "--windowed")) {
            port_opt.fullscreen = 0;
            port_opt.fullscreen_set = 1;
        } else if (!strcmp(a, "--noconfig")) {
            port_opt.noconfig = 1;
        } else if (!strcmp(a, "--defaults")) {
            port_opt.print_defaults = 1;
        } else if (!strcmp(a, "--keys")) {
            port_opt.print_keys = 1;
        } else if (!strcmp(a, "--nodcbt")) {
            port_opt.nodcbt = 1;
        } else if (!strcmp(a, "--noprefetch")) {
            port_opt.noprefetch = 1;
        } else if (!strcmp(a, "--resident") && i + 1 < argc) {
            port_opt.resident = atoi(argv[++i]);
        } else if (!strcmp(a, "--dvdlog")) {
            port_opt.dvdlog = 1;
        } else if (!strcmp(a, "--olddecode3")) {
            port_opt.olddecode3 = 1;
        } else if (!strcmp(a, "--olddecode2")) {
            port_opt.olddecode2 = 1;
        } else if (!strcmp(a, "--decodestats")) {
            port_opt.decodestats = 1;
        } else if (!strcmp(a, "--perfdump") && i + 1 < argc) {
            port_opt.perfdump = argv[++i];
            port_opt.perf = 1;
        } else if (!strcmp(a, "--maxskip") && i + 1 < argc) {
            port_opt.maxskip = atoi(argv[++i]);
        } else if (!strcmp(a, "--audiolead") && i + 1 < argc) {
            port_opt.audiolead = atoi(argv[++i]);
            port_opt.audiolead_set = 1;
        } else if (!strcmp(a, "--gxlog")) {
            port_opt.gxlog = 1;
        } else if (!strcmp(a, "--stub-trace")) {
            port_opt.stub_trace = 1;
        } else if (!strcmp(a, "--deterministic")) {
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            /* --seed implies --deterministic: it *is* the deterministic
             * clock, started at a chosen reading.  See os_misc.c. */
            port_opt.seed = strtoll(argv[++i], NULL, 0);
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--rtc") && i + 1 < argc) {
            /* Dolphin pins CustomRTCValue and the game seeds both of its RNGs
             * from OSGetTime; --rtc is the same number on this side, converted
             * to the console's 40.5 MHz ticks since 2000-01-01.  It is --seed
             * in the units the reference rig is configured in. */
            const char* v = argv[++i];
            port_opt.rtc = !strcmp(v, "dolphin") ? PORT_RTC_DOLPHIN
                                                 : strtoll(v, NULL, 0);
            port_opt.rtc_seen = 1;
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--rtcoffset") && i + 1 < argc) {
            /* §17.3: --rtc gives the two rigs the same clock *origin* and they
             * still deal different minigames, because `BoardRandInit`
             * (board/main.c:1432) seeds from OSGetTime at board setup rather
             * than at boot, and the port arrives there some 300 frames earlier
             * than the console -- it skips the DVD seek and the opening movie.
             * This is that difference, in seconds, added to the origin.  One
             * frame is 1/60 s, so the 300-frame gap is --rtcoffset 5.0; the
             * exact number is a subtraction between the two rigs' --ovllog
             * timestamps at the board's first frame. */
            port_opt.rtc_offset = strtod(argv[++i], NULL);
        } else if (!strcmp(a, "--card") && i + 1 < argc) {
            port_opt.card = argv[++i];
        } else if (!strcmp(a, "--freshcard")) {
            port_opt.freshcard = 1;
        } else if (!strcmp(a, "--minigame") && i + 1 < argc) {
            port_opt.minigame = argv[++i];
        } else if (!strcmp(a, "--boarddump") && i + 1 < argc) {
            port_opt.boarddump = argv[++i];
        } else if (!strcmp(a, "--board") && i + 1 < argc) {
            const char* v = argv[++i];
            port_opt.board = atoi(v);
            port_opt.boardcycle = strchr(v, '+') != NULL;
            if (port_opt.board < 1 || port_opt.board > 6) {
                fprintf(stderr, "--board: 1-6 (w01..w06); the extras are --goto "
                                "w10dll / w20dll / w21dll\n");
                exit(2);
            }
        } else if (!strcmp(a, "--com4")) {
            port_opt.com4 = 1;
        } else if (!strcmp(a, "--cast") && i + 1 < argc) {
            port_opt.cast = argv[++i];
            port_opt.com4 = 1;
        } else if (!strcmp(a, "--dvdheap") && i + 1 < argc) {
            port_opt.dvdheap = atoi(argv[++i]);
            /* malloc.c is game code and cannot see port_opt; the env var is the
             * same seam --m444trace uses (PLAN.md 28.2). */
            setenv("MP4_DVDHEAP_KB", argv[i], 1);
        } else if (!strcmp(a, "--turns") && i + 1 < argc) {
            port_opt.turns = atoi(argv[++i]);
        } else if (!strcmp(a, "--status")) {
            port_opt.status = 1;
        } else if (!strcmp(a, "--stuckwatch") && i + 1 < argc) {
            port_opt.stuckwatch = atoi(argv[++i]);
        } else if (!strcmp(a, "--soak")) {
            port_opt.soak = 1;
            /* A Mega Mushroom is rare enough that a soak may run for hours
             * without drawing one, and 29.3's two `return 1`s have no witness
             * until it does.  Arm the trace for every soak: it costs one
             * comparison per Mega squish and it is the only way the overnight
             * run can be the witness. */
            setenv("MP4_MEGATRACE", "1", 1);
            port_opt.com4 = 1;
            port_opt.status = 1;
            if (!port_opt.stuckwatch) {
                port_opt.stuckwatch = 90;
            }
        } else if (!strcmp(a, "--nodepop")) {
            port_opt.depop = 0;
        } else if (!strcmp(a, "--resample1")) {
            port_opt.resample4 = 0;
        } else if (!strcmp(a, "--resample4")) {
            port_opt.resample4 = 1;
        } else if (!strcmp(a, "--dlcache")) {
            port_opt.dlcache = 1;
        } else if (!strcmp(a, "--novcache")) {
            port_opt.vcache = 0;
        } else if (!strcmp(a, "--vcache") && i + 1 < argc) {
            const char* v = argv[++i];
            port_opt.vcache = !strcmp(v, "off") || !strcmp(v, "0") ? 0
                              : !strcmp(v, "count") || !strcmp(v, "1") ? 1
                              : !strcmp(v, "auto") || !strcmp(v, "3") ? 3 : 2;
        } else if (!strcmp(a, "--vcachefit") && i + 1 < argc) {
            port_opt.vcache_fit = atof(argv[++i]);
        } else if (!strcmp(a, "--vcachemb") && i + 1 < argc) {
            port_opt.vcache_mb = atoi(argv[++i]);
        } else if (!strcmp(a, "--vcachevbo")) {
            port_opt.vcache_vbo = 1;
        } else if (!strcmp(a, "--cardwait")) {
            port_opt.cardwait = 1;
        } else if (!strcmp(a, "--synclog")) {
            port_opt.synclog = 1;
        } else if (!strcmp(a, "--olddecode")) {
            port_opt.olddecode = 1;
        } else if (!strcmp(a, "--noaicb")) {
            port_opt.noaicb = 1;
        } else if (!strcmp(a, "--nomovies")) {
            port_opt.nomovies = 1;
        } else if (!strcmp(a, "--thpyuv")) {
            port_opt.thpyuv = 1;
        } else if (!strcmp(a, "--thplog")) {
            port_opt.thplog = 1;
        } else if (!strcmp(a, "--nothpslice")) {
            port_opt.nothpslice = 1;
        } else if (!strcmp(a, "--thpguard") && i + 1 < argc) {
            port_opt.thpguard = atoi(argv[++i]);
            port_opt.thpguard_set = 1;
        } else if (!strcmp(a, "--foldxbar")) {
            port_opt.foldxbar = 1;
        } else if (!strcmp(a, "--foldcap") && i + 1 < argc) {
            port_opt.foldcap = atoi(argv[++i]);
        } else if (!strcmp(a, "--goto") && i + 1 < argc) {
            port_opt.gotoovl = argv[++i];
        } else if (!strcmp(a, "--perfwin") && i + 1 < argc) {
            port_opt.perfwin = argv[++i];
            port_opt.perf = 1;
        } else if (!strcmp(a, "--oldtev")) {
            port_opt.oldtev = 1;
        } else if (!strcmp(a, "--tevstats")) {
            port_opt.tevstats = 1;
        } else if (!strcmp(a, "--m444trace")) {
            /* The m444 ball trace lives inside the REL (port/patches.txt), so
             * it cannot see port_opt; it reads the environment instead, and
             * this flag is what sets it.  PLAN.md 28.2. */
            setenv("MP4_M444TRACE", "1", 1);
        } else if (!strcmp(a, "--nodraw")) {
            port_opt.nodraw = 1;
        } else if (!strcmp(a, "--ffto") && i + 1 < argc) {
            port_opt.ffto = atoi(argv[++i]);
        } else if (!strcmp(a, "--ffto-warm") && i + 1 < argc) {
            port_opt.ffto_warm = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-every") && i + 1 < argc) {
            port_opt.snap_every = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-keep") && i + 1 < argc) {
            port_opt.snap_keep = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-at") && i + 1 < argc) {
            port_opt.snap_now = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-dir") && i + 1 < argc) {
            port_opt.snap_dir = argv[++i];
        } else if (!strcmp(a, "--restore") && i + 1 < argc) {
            port_opt.restore = argv[++i];
        } else if (!strcmp(a, "--snapdiff")) {
            port_opt.snapdiff = 1;
        } else if (!strcmp(a, "--clickstat")) {
            port_opt.clickstat = 1;
        } else if (!strcmp(a, "--reldir") && i + 1 < argc) {
            port_opt.reldir = argv[++i];
        } else if (!strcmp(a, "--gxdemo")) {
            port_opt.gxdemo = 1;
        } else if (!strcmp(a, "--reltest")) {
            port_opt.reltest = 1;
        } else if (!strcmp(a, "--guardtest") && i + 1 < argc) {
            port_opt.guardtest = argv[++i];
        } else if (!strcmp(a, "--memmap")) {
            port_opt.memmap = 1;
        } else if (!strcmp(a, "--relzerobss")) {
            port_opt.relzerobss = 1;
        } else if (!strcmp(a, "--nodatareset")) {
            port_opt.nodatareset = 1;
        } else if (!strcmp(a, "--noaudio")) {
            port_opt.noaudio = 1;
        } else if (!strcmp(a, "--wav") && i + 1 < argc) {
            port_opt.wav = argv[++i];
        } else if (!strcmp(a, "--mute")) {
            port_opt.mute = 1;
        } else if (!strcmp(a, "--audiolog")) {
            port_opt.audiolog = 1;
        } else if (!strcmp(a, "--glcheck")) {
            port_opt.glcheck = 1;
        } else if (!strcmp(a, "--glinfo")) {
            port_opt.glinfo = 1;
        } else if (!strcmp(a, "--vprobe")) {
            port_opt.vprobe = 1;
        } else if (!strcmp(a, "--cpuxf")) {
            port_opt.cpuxf = 1;
        } else if (!strcmp(a, "--vprogstats")) {
            port_opt.vprogstats = 1;
        } else if (!strcmp(a, "--vproglog")) {
            port_opt.vproglog = 1;
        } else if (!strcmp(a, "--gxwarn")) {
            port_opt.gxwarn = 1;
        } else if (!strcmp(a, "--gxsplit")) {
            port_opt.gxsplit = 1;
            port_sub_on = 1; /* M44 */
            port_opt.perf = 1;
        } else if (!strcmp(a, "--perf")) {
            port_opt.perf = 1;
        } else if (!strcmp(a, "--drawlog") && i + 1 < argc) {
            port_opt.drawlog = atoi(argv[++i]);
        } else if (!strcmp(a, "--drawlog-at") && i + 1 < argc) {
            port_opt.drawlog_frame = atoi(argv[++i]);
        } else if (!strcmp(a, "--tlutlog")) {
            port_opt.tlutlog = 1;
        } else if (!strcmp(a, "--oldnulltev")) {
            port_opt.oldnulltev = 1;
        } else if (!strcmp(a, "--oldsubmit")) {
            port_opt.oldsubmit = 1;
        } else if (!strcmp(a, "--novar")) {
            port_opt.novar = 1;
        } else if (!strcmp(a, "--batchmax") && i + 1 < argc) {
            port_opt.batchmax = atoi(argv[++i]);
        } else if (!strcmp(a, "--segrebase") && i + 1 < argc) {
            port_opt.segrebase = atoi(argv[++i]); /* 1 + 2 tev + 4 vprog + 8 raster */
        } else if (!strcmp(a, "--gltrace") && i + 1 < argc) {
            port_opt.gltrace = atoi(argv[++i]);
        } else if (!strcmp(a, "--endlog") && i + 1 < argc) {
            port_opt.endlog = argv[++i];
        } else if (!strcmp(a, "--nomerge")) {
            port_opt.nomerge = 1;
        } else if (!strcmp(a, "--nohilite")) {
            port_opt.nohilite = 1;
        } else if (!strcmp(a, "--noindexed")) {
            port_opt.noindexed = 1;
        } else if (!strcmp(a, "--indexed")) {
            port_opt.noindexed = 0;
        } else if (!strcmp(a, "--noenvbulk")) {
            port_opt.noenvbulk = 1;
        } else if (!strcmp(a, "--envbulk")) {
            port_opt.noenvbulk = 0;
        } else if (!strcmp(a, "--nofixbase")) {
            port_opt.nofixbase = 1;
        } else if (!strcmp(a, "--fixbase")) {
            port_opt.nofixbase = 0;
        } else if (!strcmp(a, "--nomultidraw")) {
            port_opt.nomultidraw = 1;
        } else if (!strcmp(a, "--submitstats")) {
            port_opt.submitstats = 1;
        } else if (!strcmp(a, "--regfix2dbg")) {
            port_opt.regfix2dbg = 1;
        } else if (!strcmp(a, "--nohilitetex")) {
            port_opt.nohilitetex = 1;
        } else if (!strcmp(a, "--nocopyhalf")) {
            port_opt.nocopyhalf = 1;
        } else if (!strcmp(a, "--copylog")) {
            port_opt.copylog = 1;
        } else if (!strcmp(a, "--noefbflip")) {
            port_opt.noefbflip = 1;
        } else if (!strcmp(a, "--oldfirsthash")) {
            port_opt.oldfirsthash = 1;
        } else if (!strcmp(a, "--norekey")) {
            port_opt.norekey = 1;
        } else if (!strcmp(a, "--dumpcopy")) {
            port_opt.dumpcopy = 1;
        } else if (!strcmp(a, "--notint")) {
            port_opt.notint = 1;
        } else if (!strcmp(a, "--tintlog")) {
            port_opt.tintlog = 1;
        } else if (!strcmp(a, "--texdecodelog")) {
            port_opt.texdecodelog = 1;
        } else if (!strcmp(a, "--lazyflush")) {
            port_opt.lazyflush = 1;
        } else if (!strcmp(a, "--premerge-max") && i + 1 < argc) {
            port_opt.premerge_max = atoi(argv[++i]);
            port_opt.lazyflush = 1;
        } else if (!strcmp(a, "--mdposonly")) {
            port_opt.mdposonly = 1;
        } else if (!strcmp(a, "--mdmax") && i + 1 < argc) {
            port_opt.mdmax = atoi(argv[++i]);
        } else if (!strcmp(a, "--nospot")) {
            port_opt.nospot = 1;
        } else if (!strcmp(a, "--oldfog")) {
            port_opt.oldfog = 1;
        } else if (!strcmp(a, "--noregchain")) {
            port_opt.noregchain = 1;
        } else if (!strcmp(a, "--nocarry")) {
            port_opt.nocarry = 1;
        } else if (!strcmp(a, "--skipobj") && i + 1 < argc) {
            port_opt.skipobj = argv[++i];
        } else if (!strcmp(a, "--probeobj") && i + 1 < argc) {
            port_opt.probeobj = argv[++i];
        } else if (!strcmp(a, "--probeverts") && i + 1 < argc) {
            port_opt.probeverts = atoi(argv[++i]);
        } else if (!strcmp(a, "--skipverts") && i + 1 < argc) {
            port_opt.skipverts = atoi(argv[++i]);
        } else if (!strcmp(a, "--zprepass") && i + 1 < argc) {
            port_opt.zprepass = atoi(argv[++i]);
        } else if (!strcmp(a, "--probebox") && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d,%d,%d", &port_opt.probebox[0], &port_opt.probebox[1],
                   &port_opt.probebox[2], &port_opt.probebox[3]);
        } else if (!strcmp(a, "--forceobj") && i + 1 < argc) {
            char* colon;
            port_opt.forceobj = argv[++i];
            colon = strchr(port_opt.forceobj, ':');
            if (colon) {
                *colon = 0;
                port_opt.forceobj_flags = atoi(colon + 1);
            }
        } else if (!strcmp(a, "--nolinewidth")) {
            port_opt.nolinewidth = 1;
        } else if (!strcmp(a, "--noregfix")) {
            port_opt.noregfix = 1;
        } else if (!strcmp(a, "--oldkonst")) {
            port_opt.oldkonst = 1;
        } else if (!strcmp(a, "--oldczero")) {
            port_opt.oldczero = 1;
        } else if (!strcmp(a, "--tfs")) {
            port_opt.tfs = 1;
        } else if (!strcmp(a, "--notfs")) {
            port_opt.tfs = 0;
        } else if (!strcmp(a, "--tfsprobe")) {
            port_opt.tfsprobe = 1;
        } else if (!strcmp(a, "--tfslog")) {
            port_opt.tfslog = 1;
        } else if (!strcmp(a, "--tfsnorebind")) {
            port_opt.tfsnorebind = 1;
        } else if (!strcmp(a, "--tfscopycpu")) {
            port_opt.tfscopycpu = 1;
        } else if (!strcmp(a, "--tfssqcopy")) {
            port_opt.tfssqcopy = 1;
        } else if (!strcmp(a, "--tfsmtx")) {
            port_opt.tfsmtx = 1;
        } else if (!strcmp(a, "--tfsforcebind")) {
            port_opt.tfsforcebind = 1;
        } else if (!strcmp(a, "--tfsenvreset")) {
            port_opt.tfsenvreset = 1;
        } else if (!strcmp(a, "--tfsdump") && i + 1 < argc) {
            port_opt.tfsdump = atoi(argv[++i]);
        } else if (!strcmp(a, "--tfsdbg") && i + 1 < argc) {
            port_opt.tfsdbg = atoi(argv[++i]);
        } else if (!strcmp(a, "--tfsall") && i + 1 < argc) {
            port_opt.tfsall = atoi(argv[++i]);
        } else if (!strcmp(a, "--oldspec0")) {
            port_opt.oldspec0 = 1;
        } else if (!strcmp(a, "--nolitalpha")) {
            port_opt.nolitalpha = 1;
        } else if (!strcmp(a, "--clrasclr")) {
            port_opt.clrasclr = 1;
        } else if (!strcmp(a, "--nohoistmtx")) {
            port_opt.nohoistmtx = 1;
        } else if (!strcmp(a, "--nofastconcat")) {
            port_opt.nofastconcat = 1;
        } else if (!strcmp(a, "--nostripes")) {
            port_opt.nostripes = 1;
        } else if (!strcmp(a, "--nounitmemo")) {
            port_opt.nounitmemo = 1;
        } else if (!strcmp(a, "--norotmemo")) {
            port_opt.norotmemo = 1;
        } else if (!strcmp(a, "--nofastsin")) {
            port_opt.nofastsin = 1;
        } else if (!strcmp(a, "--nomtxmemo")) {
            port_opt.nomtxmemo = 1;
        } else if (!strcmp(a, "--norotl")) {
            port_opt.norotl = 1;
        } else if (!strcmp(a, "--notexskip")) {
            port_opt.notexskip = 1;
        } else if (!strcmp(a, "--nostripepool")) {
            port_opt.nostripepool = 1;
        } else if (!strcmp(a, "--giveitem") && i + 1 < argc) {
            port_opt.giveitem = argv[++i];
        } else if (!strcmp(a, "--waterlook") && i + 1 < argc) {
            i++;
            port_opt.waterlook = !strcmp(argv[i], "console") ? 0 : 1;
            waterlook_set = 1;
        } else if (!strcmp(a, "--wavegain") && i + 1 < argc) {
            port_opt.wavegain = atoi(argv[++i]);
        } else if (!strcmp(a, "--nowaterpt")) {
            port_opt.nowaterpt = 1;
        } else if (!strcmp(a, "--affinetex")) {
            port_opt.affinetex = 1;
        } else if (!strcmp(a, "--halfwatch") && i + 1 < argc) {
            port_opt.halfwatch = atoi(argv[++i]);
        } else if (!strcmp(a, "--nodcbz")) {
            port_opt.nodcbz = 1;
        } else if (!strcmp(a, "--novpgen")) {
            port_opt.novpgen = 1;
        } else if (!strcmp(a, "--notevdirty")) {
            port_opt.notevdirty = 1;
        } else if (!strcmp(a, "--noattrmemo")) {
            port_opt.noattrmemo = 1;
        } else if (!strcmp(a, "--nowordhash")) {
            port_opt.nowordhash = 1;
        } else if (!strcmp(a, "--water") && i + 1 < argc) {
            const char* v = argv[++i];
            port_opt.water = !strcmp(v, "off") ? 0 : !strcmp(v, "cheap") ? 1
                           : !strcmp(v, "full") ? 2 : -1;
            water_set = 1;
        } else if (!strcmp(a, "--watergrid") && i + 1 < argc) {
            port_opt.watergrid = atoi(argv[++i]);
        } else if (!strcmp(a, "--novcpos")) {
            port_opt.novcpos = 1;
        } else if (!strcmp(a, "--norastermemo")) {
            port_opt.norastermemo = 1;
        } else if (!strcmp(a, "--wbpart")) {
            port_opt.wbpart = 1;
        } else if (!strcmp(a, "--nowbpart")) {
            port_opt.wbpart = 0;
        } else if (!strcmp(a, "--oldvtxjoin")) {
            port_opt.oldvtxjoin = 1;
        } else if (!strcmp(a, "--rtgx") && i + 1 < argc) {
            const char* v = argv[++i];
            port_opt.rtgx = !strcmp(v, "auto") || !strcmp(v, "2") ? 2
                            : !strcmp(v, "3") ? 3 /* the hand-over test: alternate frames */
                            : atoi(v) ? 1 : 0;
        } else if (!strcmp(a, "--rtgxfit") && i + 1 < argc) {
            port_opt.rtgx_fit = atof(argv[++i]);
        } else if (!strcmp(a, "--vcarr") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &port_opt.vcarr_from, &port_opt.vcarr_to) != 2) {
                port_opt.vcarr_from = port_opt.vcarr_to = 0;
            }
        } else if (!strcmp(a, "--pmcwin") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &port_opt.pmcwin_from, &port_opt.pmcwin_to) != 2) {
                port_opt.pmcwin_from = port_opt.pmcwin_to = 0;
            }
        } else if (!strcmp(a, "--pmc") && i + 1 < argc) {
            port_opt.pmc = atoi(argv[++i]);
        } else if (!strcmp(a, "--nowb")) {
            port_opt.nowb = 1;
        } else if (!strcmp(a, "--nomotionexec")) {
            port_opt.nomotionexec = 1;
        } else if (!strcmp(a, "--nopacklights")) {
            port_opt.nopacklights = 1;
        } else if (!strcmp(a, "--nodirtyfilter")) {
            port_opt.nodirtyfilter = 1;
        } else if (!strcmp(a, "--nodirty")) {
            port_opt.nodirty = 1;
        } else if (!strcmp(a, "--oldakonst")) {
            port_opt.oldakonst = 1;
        } else if (!strcmp(a, "--cmpmask") && i + 1 < argc) {
            port_opt.cmpmask = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--ovllog")) {
            port_opt.ovllog = 1;
        } else if (!strcmp(a, "--nanwatch")) {
            port_opt.nanwatch = 1;
        } else if (!strcmp(a, "--scenelog") && i + 1 < argc) {
            port_opt.scenelog = argv[++i];
        } else if (!strcmp(a, "--nocard")) {
            port_opt.nocard = 1;
        } else if (!strcmp(a, "--reldlclose")) {
            port_opt.reldlclose = 1;
        } else if (!strcmp(a, "--dumptex")) {
            port_opt.dumptex = 1;
        } else if (!strcmp(a, "--texhash-full")) {
            port_opt.texhash_full = 1;
        } else if (!strcmp(a, "--texvalidate-every-bind")) {
            gx_tex_set_validate_every_bind(1);
        } else if (!strcmp(a, "--headless")) {
            port_opt.headless = 1;
        } else if (!strcmp(a, "--dumpframe") && i + 1 < argc) {
            port_opt.dumpframe = argv[++i];
        } else if (!strcmp(a, "--shotdir") && i + 1 < argc) {
            port_opt.shotdir = argv[++i];
        } else if (!strcmp(a, "--scale") && i + 1 < argc) {
            port_opt.scale = atoi(argv[++i]);
        } else if (!strcmp(a, "--nopad")) {
            port_opt.nopad = 1;
        } else if (!strcmp(a, "--kbport") && i + 1 < argc) {
            port_opt.kbport = atoi(argv[++i]);
        } else if (!strcmp(a, "--paddbg")) {
            port_opt.pad_debug = 1;
        } else if (!strcmp(a, "--play") && i + 1 < argc) {
            port_opt.pad_play = argv[++i];
        } else if (!strcmp(a, "--record") && i + 1 < argc) {
            port_opt.pad_record = argv[++i];
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            port_opt.verbose = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strncmp(a, "-psn_", 5)) {
            /* M32: LaunchServices hands a double-clicked application its
             * process serial number as an argument (-psn_0_2146828); the
             * first Finder launch of the bundle printed the usage and
             * exited on it */
            continue;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            usage(argv[0]);
            return 0;
        }
    }
    /* --soak implies the menu walk.
     *
     * `--soak` alone boots into the attract loop and stays there: nothing
     * presses Start, so the board it is supposed to soak never begins.  That
     * is not a hypothetical -- it cost the 2026-09-14 overnight run seven
     * hours of title screen and 280 STUCK lines (PLAN.md 21.8), and it is an
     * easy mistake to make because every other soak flag is self-contained.
     * The walk is shipped in the bundle next to the disc image, so naming it
     * here costs nothing and there is no case where a soak wants the attract
     * loop instead. An explicit --play still wins. */
    if (port_opt.soak && port_opt.pad_play == NULL) {
        port_opt.pad_play = "board-start-com4.play";
        fprintf(stderr, "port> --soak implies --play %s (the menu walk); "
                        "pass --play explicitly to override\n",
                port_opt.pad_play);
    }

    /* M10 defaults, after the loop so the flags may come in any order.
     * --ffto *is* --nodraw with an end: one frame of warm-up before N, because
     * the texture cache is flushed when drawing comes back on and the first
     * drawn frame has to re-upload what it binds (PLAN.md 24.1). */
    port_opt.nodraw_user = port_opt.nodraw; /* M28: --nodraw --ffto N stays off past N */
    if (port_opt.ffto) {
        port_opt.nodraw = 1;
    }
    if (port_opt.ffto_warm <= 0) {
        port_opt.ffto_warm = 1;
    }
    if (port_opt.snap_keep <= 0) {
        port_opt.snap_keep = 3;
    }
    {
        extern int port_mtx_noaltivec;
        port_mtx_noaltivec = !port_opt.altivec;
    }
    /* M17: frame mode is the default; --lockstep, --turbo, --nodraw and
     * --headless are the measurements it would distort (framemode.c). */
    if (!port_opt.lockstep) {
        port_opt.realtime = 1;
    }
    if (!port_opt.audiolead_set) {
        port_opt.audiolead = 100;
    }
    if (!port_opt.thpguard_set) {
        port_opt.thpguard = 60;
    }

    /* After the loop, so --rtc and --rtcoffset may be given in either order. */
    if (port_opt.rtc_seen) {
        port_opt.rtc_set = 1;
        port_opt.seed =
            (long long)((double)(port_opt.rtc - PORT_GC_EPOCH_UNIX) *
                            (double)PORT_TIMER_CLOCK +
                        port_opt.rtc_offset * (double)PORT_TIMER_CLOCK);
    }
    return 1;
}

void port_gx_shutdown(void);
void gl13_rt_start(void);
void port_gx_demo(void);
void gl13_write_ppm(const char* path);
void GXInit_demo_bootstrap(void);

/* One exit path, so a quit through the game's own reset, through --frames and
 * through the end of main() all report the same things in the same order. */
void port_shutdown(int code) {
    port_audio_shutdown(); /* first: it closes the WAV, which must be complete */
    port_workers_shutdown(); /* M24: after the audio's join, before the reports */
    port_dvd_cache_shutdown(); /* M36: the loader's thread, before its report */
    void gx_tev_report(void);
    void gx_water_report(void);
    void gx_tfs_report(void);
    gx_tev_report();
    gx_water_report(); /* M44 */
    gx_tfs_report(); /* M35 */
    port_perf_report();
    port_pmc_report(); /* M43: on the game thread, whose counters they are */
    port_framemode_report();
    port_audio_report();
    port_clock_report();
    port_gx_shutdown();
    port_dvd_stats();
    port_dvd_cache_report(); /* M36 */
    port_thp_report();
    port_card_report();
    port_dll_report();
    port_snap_report();
    port_workers_report();
    port_sincos_report();
    port_fast_sincos_report();
    port_mtx_memo_report();
    port_motion_exec_report();
    port_vtx_rewrite_report(); /* M43 */
    port_wb_report();
    port_matwalk_report();
    port_curve_memo_report();
    port_sparse_report();
    port_fastsqrt_report();
    port_reset_report();
    port_stub_report();
    exit(code);
}

void port_vtx_rewrite_report(void); /* src/os/vtx_rewrite.c */

static void run_game(void) {
    port_clock_mark();
    port_pmc_init(); /* M43: the counters are this thread's */
    port_log("port> entering the game's own main()\n\n");
    mp4_game_main();
    port_log("\nport> the game's main() returned\n");
    port_shutdown(0);
}

int main(int argc, char** argv) {
    if (!port_parse_args(argc, argv)) {
        return 1;
    }
    /* M25: the config file -- what --image, --fullscreen and the dialogs
     * remembered; the log's default home; see config.c for the format. */
    if (!port_opt.noconfig) {
        static char default_log[800];
        port_config_load();
        if (port_opt.image) {
            char abs[1024];
            /* an absolute path, so a run from another directory finds it */
            port_config_set("image", port_opt.image[0] == '/' ? port_opt.image
                            : realpath(port_opt.image, abs) ? abs : port_opt.image);
        }
        /* M44: the water's level, remembered as the fullscreen switch is (a
         * --water on the command line is stored; otherwise the config's) */
        if (water_set) {
            port_config_set("water", port_opt.water == 0 ? "off" : port_opt.water == 1 ? "cheap"
                                     : port_opt.water == 2 ? "full" : "auto");
        } else {
            const char* w = port_config_get("water");
            if (w) {
                port_opt.water = !strcmp(w, "off") ? 0 : !strcmp(w, "cheap") ? 1
                               : !strcmp(w, "full") ? 2 : -1;
            }
        }
        /* M45: the water's look, remembered the same way */
        if (waterlook_set) {
            port_config_set("waterlook", port_opt.waterlook ? "port" : "console");
        } else {
            const char* w = port_config_get("waterlook");
            if (w) {
                port_opt.waterlook = strcmp(w, "console") != 0;
            }
        }
        if (port_opt.fullscreen_set) {
            port_config_set("fullscreen", port_opt.fullscreen ? "1" : "0");
        } else {
            const char* fs = port_config_get("fullscreen");
            if (fs) {
                port_opt.fullscreen = atoi(fs) != 0;
            } else {
                /* M32: a first run from the Finder comes up fullscreen, as
                 * the Snowboard Kids apps do, and the config remembers it
                 * from then on (--windowed clears it).  The lab's config on
                 * the G4 carries `fullscreen = 0', so its runs stay windowed
                 * -- the md5s are windowed frames (PLAN.md 40.6). */
                port_opt.fullscreen = 1;
                port_config_set("fullscreen", "1");
            }
        }
        if (!port_opt.log) {
            snprintf(default_log, sizeof(default_log), "%s/MarioParty4.log",
                     port_app_support_dir());
            port_opt.log = default_log;
        }
        port_config_save();
    }
    if (port_opt.print_keys) {
        port_print_keys(stdout);
        return 0;
    }
    port_log_open(port_opt.log);
    port_log_async_start(); /* M40 (PLAN.md 55): the log's writes off the game thread */
    if (port_opt.log) {
        port_log("port> log: %s\n", port_opt.log);
    }
    port_crash_handler_install();
    if (port_opt.watchdog) {
        port_watchdog_arm(port_opt.watchdog);
    }
    port_log("Mario Party 4 -- native port, PowerPC Edition %s (milestone %s)\n",
             PORT_VERSION_STRING, PORT_MILESTONE);
    /* M25: the machine check, before anything opens a window: the inventory,
     * the verdict and the settings it implies (or the exit, under
     * --machinecheck / an unsupported machine without --force). */
    port_machine_check();
    if (port_opt.print_defaults) {
        port_print_effective(stdout, 0);
        return 0;
    }
    /* M32: the disc image after the check, so a player on an unsupported Mac
     * gets the refusal and not a file dialog; the chooser asks until it is
     * given a disc image or cancelled (port_find_default_image). */
    if (!port_opt.image) {
        port_opt.image = port_find_default_image();
        if (!port_opt.image && !port_opt.reltest && !port_opt.gxdemo && !port_opt.guardtest &&
            !port_opt.memmap) {
            port_log("port> no disc image: nothing chosen; quitting\n");
            if (!port_opt.headless) {
                port_dialog_notice("Mario Party 4: no disc image",
                                   "Mario Party 4 needs your own disc image of the game "
                                   "(Mario Party 4, USA, Rev 1, as a .iso) and none was "
                                   "chosen.\n\nOpen the game again to choose it, or put "
                                   "the .iso in a folder named MarioParty4 in your home "
                                   "folder, where it is found without asking.");
            }
            return 0;
        }
    }
    if (port_opt.reltest) {
        port_opt.reldlclose = 1; /* the self-test is *about* the unload path */
        return port_dll_selftest();
    }
    if (port_opt.gxdemo) {
        char path[1024];
        port_mem_init();
        port_vi_init();
        port_gx_init();
        GXInit_demo_bootstrap();
        port_gx_demo();
        snprintf(path, sizeof(path), "%s/gxdemo.ppm",
                 port_opt.shotdir ? port_opt.shotdir : ".");
        gl13_write_ppm(path);
        port_gx_present();
        port_shutdown(0);
    }
    port_reset_init();
    port_mem_init();
    if (port_opt.memmap || port_opt.guardtest) {
        port_mem_regions_dump();
    }
    if (port_opt.guardtest) {
        port_guard_selftest(port_opt.guardtest);
        port_shutdown(0);
    }
    port_vi_init();
    port_dvd_init();
    port_gx_init();
    port_workers_init(); /* M24: the second core, if there is one */
    port_dvd_cache_init(); /* M36: the loader -- after the FST and the thread decision */
    gl13_rt_start();     /* M27: GL to the render thread (or the inline replay) */
    port_print_effective(NULL, 1); /* M32: the block a bug report is asked for */
    /* After port_gx_init, which is what brings SDL up.  --noaudio keeps the
     * whole path switched off, including the tick, so the boot behaves exactly
     * as it did before M6 -- which is what makes an audio regression bisectable
     * against a silent run of the same seed. */
    if (port_opt.noaudio) {
        port_audio_enabled = 0;
    } else {
        port_audio_out_init();
        if (port_opt.wav) {
            port_audio_wav_start(port_opt.wav);
        }
    }
    port_selfplay_init();
    /* After every subsystem, because --ffto asks the GX backend to switch the
     * renderer off and the snapshot registry has to see the buffers the audio
     * and card layers just allocated. */
    port_ffto_init();
    port_framemode_init();
    if (port_framemode_active() && port_opt.audiolead > 0) {
        port_audio_out_prime((unsigned)port_opt.audiolead);
    }
    port_snap_init();
    if (port_snap_restore_pending()) {
        /* Does not return: it copies the snapshot over this process's arenas,
         * globals and modules and longjmps into the saved retrace.  The boot
         * above was only ever there to build the host side -- window, GL
         * context, audio device, disc, pad -- that a snapshot deliberately
         * does not carry. */
        port_snap_restore();
    }
    port_call_on_stack(run_game, port_game_stack_top());
    return 0;
}
