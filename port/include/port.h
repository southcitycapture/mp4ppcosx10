/* Shared internals of the Mario Party 4 port layer.
 *
 * Nothing in here is visible to game code: the game sees only the SDK's own
 * headers, and the port supplies the symbols behind them.
 */
#ifndef PORT_PORT_H
#define PORT_PORT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* M32: FILE in the --defaults printers */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- console geometry ---------------------------------------------------- */
#define PORT_MEM1_SIZE 0x01800000u /* 24 MB, retail  */
#define PORT_ARAM_SIZE 0x01000000u /* 16 MB          */
#define PORT_BUS_CLOCK 162000000u
#define PORT_CORE_CLOCK 486000000u
#define PORT_TIMER_CLOCK (PORT_BUS_CLOCK / 4) /* 40.5 MHz, what OSGetTick counts */
/* The console's RTC epoch: OSGetTime counts 40.5 MHz ticks from here.  Dolphin's
 * CustomRTCValue is a Unix time, so --rtc converts through this constant and the
 * two rigs then seed their RNGs from the same number. */
#define PORT_GC_EPOCH_UNIX 946684800LL /* 2000-01-01T00:00:00Z */
/* What port/ref/dolphin-user/Config/Dolphin.ini pins CustomRTCValue to. */
#define PORT_RTC_DOLPHIN 1041472800LL /* 2003-01-02T00:00:00Z */

/* ---- settings, from argv ------------------------------------------------- */
/* M32: the shipped version (the dmg's name, the plist, the --defaults header)
 * and the milestone that built it. */
#define PORT_VERSION_STRING "0.9.12"
#define PORT_MILESTONE "M43"

typedef struct PortOptions {
    const char* image;      /* --image  disc image or extracted files/ tree   */
    const char* log;        /* --log    copy of the OSReport narration        */
    int gxlog;              /* --gxlog  print every GX call, not just firsts  */
    int deterministic;      /* --deterministic  fixed tick, fixed clock       */
    int stub_trace;         /* --stub-trace     print every stub call         */
    int max_frames;         /* --frames N       quit after N retraces         */
    int turbo;              /* --turbo          do not pace to 60 Hz          */
    int watchdog;           /* --watchdog SEC   report and quit if it hangs   */
    int verbose;
    /* ---- M2 ---- */
    const char* reldir;     /* --reldir  where the 99 REL bundles live        */
    int gxdemo;             /* --gxdemo  draw the GX self-test frame          */
    int reltest;            /* --reltest load and unload all 99 twice, report */
    int memmap;             /* --memmap  print the region table at boot */
    const char* guardtest;  /* --guardtest WHERE: prove the guards fault */
    int relzerobss;         /* --relzerobss  always zero a module's bss by
                             *   hand on load, as if dlclose never unloaded   */
    int nodatareset;        /* --nodatareset  M20: keep a re-entered module's
                             *   .data as the last play left it (the pre-M20
                             *   loader, for the m406 reproduction)           */
    int noaudio;            /* --noaudio  HuAudInit/msm succeed as silent stubs */
    int glcheck;            /* --glcheck  assert no GL call outside GL 1.3    */
    int glinfo;             /* --glinfo   dump GL strings, limits, extensions */
    long long seed;         /* --seed N   deterministic clock origin (RNG seed) */
    int perf;               /* --perf     per-frame game/gx/present timing     */
    int gxsplit;            /* --gxsplit  M21: the gx time split into exclusive
                             *   regions (decode, CPU transform, state, texture
                             *   bind, issue) per drawn frame                  */
    int drawlog;            /* --drawlog N  explain the first N draws in full  */
    int dumptex;            /* --dumptex  write every decoded texture as a PPM */
    int texhash_full;       /* --texhash-full  hash whole textures every bind */
    int gxwarn;             /* --gxwarn   name every degraded GX feature      */
    int headless;           /* --headless no window; still decodes and logs   */
    const char* dumpframe;  /* --dumpframe SPEC  frames to write: N, a,b, a-b/s */
    const char* shotdir;    /* --shotdir  where --dumpframe writes            */
    int scale;              /* --scale N  window scale over 640x480           */
    /* ---- M3 ---- */
    int drawlog_frame;      /* --drawlog-at F  only explain draws on frame F  */
    int nocard;             /* --nocard   both memory-card slots read empty   */
    int reldlclose;         /* --reldlclose  really dlclose an unlinked REL   */
    /* ---- M3: PAD ---- */
    int nopad;              /* --nopad  no controller 1 at all (keyboard too) */
    int pad_debug;          /* --paddbg  log raw pad reports/buttons/axes     */
    int kbport;             /* --kbport N  the keyboard as controller N (M34) */
    const char* pad_play;   /* --play SCRIPT  scripted controller 1 input     */
    const char* pad_record; /* --record FILE  record controller 1's raw input */
    /* ---- M4 ---- */
    const char* scenelog;   /* --scenelog F[,F...]  the 3D scene's own state */
    int ovllog;             /* --ovllog  name the scene every time it changes  */
    /* ---- M5 ---- */
    int nanwatch;           /* --nanwatch  name the first frame each camera,
                             *   model or board-camera field turns NaN         */
    /* ---- M6: audio ---- */
    const char* wav;        /* --wav FILE  capture the mix to a WAV, because
                             *   nobody can listen to the G4 over SSH          */
    int mute;               /* --mute   mix and time it, emit silence          */
    int audiolog;           /* --audiolog  narrate voice/stream/studio events  */
    /* ---- M7 ---- */
    long long rtc;          /* --rtc SECS  the console RTC as Unix seconds:
                             *   the deterministic clock's origin expressed the
                             *   way Dolphin's CustomRTCValue is                */
    int rtc_set;            /* --rtc was given (0 is a legal RTC)              */
    int rtc_seen;           /* --rtc parsed; the origin is computed after the
                             * loop so --rtcoffset may come before or after   */
    double rtc_offset;      /* --rtcoffset SECS: shift the clock's origin so the
                             * port reaches BoardRandInit at the same OSGetTime
                             * the console does.  §17.3: sharing --rtc is not
                             * enough, because BoardRandInit reads the clock at
                             * board setup and the port gets there sooner.     */
    const char* card;       /* --card FILE  use this 512 KB card image          */
    int freshcard;          /* --freshcard  format the card image at boot       */
    const char* minigame;   /* --minigame NAME|ID  park the roulette here       */
    int com4;               /* --com4  all four players are CPU                 */
    const char* cast;       /* --cast a,b,c,d  the four characters --com4 parks
                             *   (0 Mario 1 Luigi 2 Peach 3 Yoshi 4 Wario
                             *    5 Donkey 6 Daisy 7 Waluigi), or names       */
    int dvdheap;            /* --dvdheap KB  HEAP_DVD's size, overriding the
                             *   console's 5,632 KB.  A deliberate, logged
                             *   divergence; 0 = the console's own number.    */
    int turns;              /* --turns N  the board's turn count                */
    int status;             /* --status  one state line per second              */
    int stuckwatch;         /* seconds of no scene change before the watchdog
                             *   names the live screen (0 = off)                */
    int soak;               /* --soak  boot, walk in, play, restart, forever    */
    int depop;              /* --nodepop clears it: the voice cut-off ramp      */
    int resample4;          /* --resample1 clears it: the 4-tap resampler       */
    int clickstat;          /* --clickstat  count mix discontinuities in-process */
    /* ---- M9 ---- */
    int dlcache;            /* --dlcache  replay cached display-list vertices
                             *   instead of decoding every frame.  Built,
                             *   measured and OFF by default: PLAN.md 21.3     */
    int vcache;             /* M40 (PLAN.md 55): the static-geometry cache --
                             *   0 off (--novcache), 1 count only (--vcache
                             *   count: the static share, nothing cached),
                             *   2 on (the decoded vertices kept in the vertex
                             *   range, replayed with no decode, no copy)      */
    int vcache_mb;          /* --vcachemb N: the cache's region (default 8)    */
    double vcache_fit;      /* --vcachefit MS: auto (3, the default) keys a
                             *   drawn frame when the render thread's replay +
                             *   the frame's whole decode would pass this (28) */
    int vcache_vbo;         /* --vcachevbo: the cache's region in a
                             *   GL_STATIC_DRAW_ARB buffer object (the card's
                             *   memory) instead of the vertex range (M40)     */
    int cardwait;           /* --cardwait: a card flush waits for the running
                             *   write (M29); default: it leaves its image for
                             *   the writer (M40, PLAN.md 55)                  */
    int synclog;            /* --synclog: port_log writes on the calling thread
                             *   (pre-M40); default: a writer thread drains a
                             *   ring (M40, PLAN.md 55)                        */
    /* ---- M9b ---- */
    int noaicb;             /* --noaicb  do not run the game's AI DMA callback,
                             *   i.e. leave the three msm periodic services
                             *   dead the way the stub did (PLAN.md 22.4)     */
    int olddecode;          /* --olddecode  walk the old call-per-attribute
                             *   cursor instead of the per-primitive decode
                             *   plan.  Kept so the two can be A/B'd on the
                             *   same hardware and the same frame md5s
                             *   (PLAN.md 21.4 / 22.1)                         */
    const char* perfwin;    /* --perfwin A-B[:NAME][,...]  per-scene fps out of
                             *   one run's own per-frame samples, so a baseline
                             *   costs one boot instead of three                */
    /* ---- M10: teleport to the bug ---- */
    int nodraw;             /* --nodraw  the GX interpreter consumes the
                             *   command streams and emits no GL and decodes
                             *   no vertices.  The game logic never reads any
                             *   of it, so the frame costs ~30% of itself     */
    int ffto;               /* --ffto N  run to frame N with drawing off and
                             *   the pacing gate open, then switch drawing
                             *   back on and carry on normally (PLAN.md 24.1) */
    int ffto_warm;          /* --ffto-warm K  frames rendered before N so the
                             *   texture cache and the EFB are warm; 1        */
    int snap_every;         /* --snap-every K  write a snapshot every K frames */
    int snap_keep;          /* --snap-keep N   keep the newest N (ring)        */
    const char* snap_dir;   /* --snap-dir DIR  where the ring lives            */
    const char* restore;    /* --restore FILE  resume from this snapshot       */
    int snap_now;           /* --snap-at N  one snapshot at frame N, then go on */
    int snapdiff;           /* --snapdiff  dump arena digests per region, to
                             *   byte-diff a restored run against a straight
                             *   one at the same frame                        */
    /* ---- M11: the vertex program ---- */
    int vprobe;             /* --vprobe  print the ARB_vertex_program limits
                             *   the generator is allowed to spend, compile a
                             *   trivial program and say whether it is native */
    int cpuxf;              /* --cpuxf  keep phase 2 (transform, CPU lighting,
                             *   texgen) on the CPU, the way every build before
                             *   M11 did.  The A/B lever, like --olddecode     */
    int vprogstats;         /* --vprogstats  draws and vertices on the GPU path
                             *   against the CPU fallback, and why each dead
                             *   variant died                                 */
    int vproglog;           /* --vproglog  print every generated program once  */

    /* ---- M13: the TEV state cache ---- */
    int oldtev;             /* --oldtev  re-apply the whole texture environment
                             *   on every draw, the way every build before M13
                             *   did.  The A/B lever (PLAN.md 28.5)           */
    int tevstats;           /* --tevstats  hits and misses of the TEV cache   */

    /* ---- M15: the eyes ---- */
    int oldnulltev;         /* --oldnulltev  drop a TEV stage that names no
                             *   texture, the way every build before M15 did.
                             *   The A/B lever for the eyes (PLAN.md 30)      */
    int tlutlog;            /* --tlutlog  every GXLoadTlut and every CI bind:
                             *   the palette address, its entry count and
                             *   format, the TLUT name, the swap table and the
                             *   cache slot the bind resolved to.  Scoped to
                             *   --drawlog-at's frame when that is given, or
                             *   the whole run when it is not (PLAN.md 30)    */

    /* ---- M16: the per-draw submit ---- */
    int oldsubmit;          /* --oldsubmit  one glDrawArrays per GX primitive
                             *   from a plain client-memory buffer, the way
                             *   every build before M16 did.  The A/B lever
                             *   (PLAN.md 31)                                 */
    int novar;              /* --novar  batch and merge, but keep the vertex
                             *   ring in ordinary memory (no
                             *   GL_APPLE_vertex_array_range), to price the
                             *   copy separately from the batching           */
    int batchmax;           /* --batchmax N  at most N segments per batch (the
                             *   flush is still deferred); 0 = BATCH_MAX     */
    int segrebase;          /* --segrebase  diagnostic: each segment from its
                             *   own base with first = 0                     */
    int gltrace;            /* --gltrace F  log every GL call the shadow lets
                             *   through during frame F, with arguments      */
    const char* endlog;     /* --endlog F[,F...]  M37: name every batch end of
                             *   the drawn frames listed -- who ended each
                             *   batch and the state, the layout and the
                             *   matrices it was submitted under (PLAN.md 52) */
    int nomerge;            /* --nomerge  do not merge contiguous list
                             *   primitives (triangles/quads) into one call */
    int nomultidraw;        /* --nomultidraw  one glDrawArrays per strip
                             *   instead of glMultiDrawArraysEXT              */
    /* ---- M21: the drawn frame ---- */
    int noindexed;          /* --noindexed  M21: issue a batch's strips and
                             *   fans through glMultiDrawArraysEXT as M16 did,
                             *   instead of one glDrawRangeElements over an
                             *   index list built for the batch (--indexed) */
    int nohilite;           /* --nohilite  M21: the pre-M21 picture for a
                             *   hilite (specular) material: channel 1 not
                             *   lit, its stage fed from channel 0, the
                             *   light's position not moved by
                             *   GXInitSpecularDir (PLAN.md 36)            */
    int noenvbulk;          /* --noenvbulk  M21: the position/normal matrix
                             *   rows as three-to-six glProgramEnvParameter
                             *   calls instead of one bulk upload            */
    int nofixbase;          /* --nofixbase  M21: the vertex arrays based at
                             *   each batch's ring position as M16 did,
                             *   instead of at the ring's start with the
                             *   batch's offset in `first` (--fixbase)      */
    int submitstats;        /* --submitstats  batches, merges, primitives per
                             *   list, fence waits                            */
    /* ---- M22: the CPU pre-transform (PLAN.md 37) ---- */
    int nohilitetex;        /* --nohilitetex  M22: the textured highlight
                             *   (the results portraits) drawn from channel
                             *   0 as before, and the mask/reflect register
                             *   triple folded to PREV as before            */
    int nocopyhalf;         /* --nocopyhalf  M23: a half-scale EFB copy takes
                             *   the bottom-left quarter at 1:1 as before   */
    int copylog;            /* --copylog  M24b: one line per GXCopyTex (the
                             *   frame, the rectangle, the format, clear,
                             *   from the front buffer), to find every
                             *   consumer of a copy on a walk              */
    int noefbflip;          /* --noefbflip  M24b: an EFB copy sampled with
                             *   GL's row order (t = 0 the bottom of the
                             *   copied region) as before PLAN.md 39b: every
                             *   copy drawn back upside down (m416)         */
    int oldfirsthash;       /* --oldfirsthash  M23: a miss stores the exhaustive
                             *   hash, so the next epoch decodes it again   */
    int norekey;            /* --norekey  M23: a texture the cache holds under
                             *   another address is decoded again as before */
    int dumpcopy;           /* --dumpcopy  M23: the first eight copy read-backs
                             *   (m415's canvas) as .ppm/.pgm in --shotdir   */
    int notint;             /* --notint  M23: a tinted lerp-by-konst pair with
                             *   K_c = 1 drops its tint as M22 did          */
    int tintlog;            /* --tintlog  M23: one line per frame that drew a
                             *   tinted lerp-by-konst pair (the dropped tint) */
    int texdecodelog;       /* --texdecodelog  M23: a line for every frame that
                             *   spent over 20 ms decoding textures          */
    /* ---- M24: the second core (PLAN.md 39) ---- */
    int threads;            /* --threads N  -1: the workers run when hw.ncpu > 1
                             *   (the default); 0: every job inline on the game
                             *   thread, today's code; 1: the workers on, on
                             *   any machine, for measurement                 */
    int nomixthread;        /* --nomixthread  M24: the mixer stays on the game
                             *   thread even with the workers on               */
    /* ---- M27: the render thread (PLAN.md 42) ---- */
    int renderthread;       /* --renderthread N  -1: 3 with the workers on, 1
                             *   without (the default); 0: the direct GL path
                             *   (--norenderthread); 1: the stream replayed inline
                             *   on the game thread; 2: the thread, joined at every
                             *   frame's end; 3: overlapped                    */
    int rtsplit;            /* --rtsplit  the replay timed by record class (state /
                             *   draw / tex / present); two timer reads a record */
    int rtgate_ms;          /* --rtgate MS  how long the gate waits for the render
                             *   thread to drain before the frame is consumed (4) */
    /* ---- M29: the decode on the render thread (PLAN.md 44) ---- */
    int stackmul;           /* --stackmul N  the coroutine stacks' multiplier over
                             *   the game's own sizes (PORT_PRC_STACK_MUL = 2 since M31; 4 before) */
    int rtdecode;           /* --rtdecode N  -1: auto (3) with the overlapped render thread, 2 joined, else 0
                             *   (the default); 0: the display lists decoded on the
                             *   game thread (the inline twin); 1: decoded by the
                             *   render thread, the game thread joined right after
                             *   each record (stage 1, no overlap); 2: joined at the
                             *   retrace (stage 2, the overlap); 3 (`auto`): M33, a
                             *   share of each drawn frame's decode kept on the game
                             *   thread so the two threads' frames balance (PLAN.md 48) */
    double rtauto_fit_ms;   /* --rtauto-fit MS  auto leaves the whole decode on the
                             *   render thread while its frame (replay + decode) is
                             *   under this (30: two retraces with margin)      */
    double rtauto_max;      /* --rtauto-max F  the largest share auto moves (0.75) */
    /* ---- M25: the machine check (src/platform/machine.c, PLAN.md 40) ---- */
    const char* fake_machine; /* --fake-machine FILE  key=value overrides of the
                             *   probes: argue another machine on this one   */
    int machinecheck;       /* --machinecheck  print the inventory and the
                             *   verdict, exit 0 ok / 1 degraded / 2 unsupported */
    int force;              /* --force  run on an `unsupported' verdict        */
    const char* mgdump;     /* --mgdump A,B,..  M26: --dumpframe offsets from the
                             *   frame the minigame module is entered (the
                             *   gallery's card / +400 / +1200 / +2300)        */
    int mgend;              /* --mgend N  M26: quit N frames after that entry  */
    int viewtexgen;         /* --viewtexgen  M26: GX_TG_POS/GX_TG_NRM texgens read
                             *   the view-space position/normal (the M3..M25
                             *   picture) instead of the raw input row       */
    int nrmfrac0;           /* --nrmfrac0  M26: decode S8/S16 normals with the VAT's
                             *   frac (0) instead of the hardware's fixed 6/14 */
    int vtxdivide;          /* --vtxdivide  M26: GX_TG_MTX3x4's q divided at the
                             *   vertex (the M3..M25 picture) instead of per
                             *   pixel by the rasteriser                     */
    int mghold;             /* --mghold  inside a minigame all four players are
                             *   human with idle pads: they hold still (M25)  */
    int fullscreen;         /* --fullscreen  the desktop's size, the 640x480
                             *   picture scaled to fit and letterboxed; --windowed
                             *   clears it; remembered in the config (M25)  */
    int fullscreen_set;     /* --fullscreen or --windowed was given          */
    int noconfig;           /* --noconfig  neither read nor write the config  */
    int print_defaults;     /* --defaults: print every effective option and exit (M32) */
    int print_keys;         /* --keys: print the key/pad table and exit (M32) */
    int texbudget_set;      /* --texbudget was given: the check leaves it alone */
    int predecode;          /* --predecode  M24: the texture decode staged on the
                             *   worker from consumed frames; built, measured
                             *   (the cold frame did not move) and OFF         */
    int predecodelog;       /* --predecodelog  M24: a line per drawn frame that
                             *   took or missed a staged decode                */
    int regfix2dbg;         /* --regfix2dbg  M22: unit C of the mask/reflect
                             *   triple shows one input per frame (frame%4)  */
    int lazyflush;          /* --lazyflush  a state setter applies the pending
                             *   batch's state and the next primitive decides
                             *   whether the batch ends (M22, measured, off) */
    int premerge_max;       /* --premerge-max N  an object of up to N vertices
                             *   that differs from the pending batch in its
                             *   matrices alone is transformed on the CPU into
                             *   the batch's model space and appended (M22,
                             *   measured, off: 0)                            */
    unsigned cmpmask;       /* --cmpmask N  which compare-first setter groups
                             *   may keep a batch alive (1 raster, 2 matrix,
                             *   4 tev, 8 chan; default 15).  0 = every
                             *   setter ends the batch.  For bisecting     */
    int oldkonst;           /* --oldkonst  claim a unit's GL constant whole
                             *   (RGB and A together) the way every build
                             *   before M16 did.  The A/B lever (PLAN.md 31.4) */
    int tfs;                /* --tfs / --notfs  M35: the indirect warp (GXSetTevIndWarp) as a
                             *   GL_ATI_text_fragment_shader program (PLAN.md 50.13); off
                             *   = the direct stage drawn unwarped, as M3..M35 */
    int tfsprobe;           /* --tfsprobe  compile the warp programs and a read-back test
                             *   on the card, print, quit */
    int tfslog;             /* --tfslog  every compiled program's text and constants */
    int tfsdbg;             /* --tfsdbg N  the warp draw's program replaced by a diagnostic (gx_tfs.c) */
    int tfsnorebind;        /* --tfsnorebind  the program bound once per change, not per draw (the A/B) */
    int tfscopycpu;         /* --tfscopycpu  diagnostic: EFB copies round-tripped through the CPU (needs --norenderthread) */
    int tfssqcopy;          /* --tfssqcopy  diagnostic: EFB copy textures padded square */
    int tfsmtx;             /* --tfsmtx  diagnostic: the units' GL texture matrices at the identity under the shader
                             *   (the vertex program keeps the fold through gx_tfs_fold) -- no effect seen */
    int tfsforcebind;       /* --tfsforcebind  diagnostic: the shader draw's texture binds issued past the shadow */
    int tfsenvreset;        /* --tfsenvreset  diagnostic: every unit's texture env reset to MODULATE under the shader
                             *   -- no effect seen */
    int tfsdump;            /* --tfsdump  the first warp draws' unit textures read back (needs --norenderthread) */
    int tfsall;             /* --tfsall N  a diagnostic: every draw of N or more TEV stages
                             *   through the fragment shader too, warp or not (m448's felt:
                             *   is the Radeon's fixed-function chain the cause?) */
    int oldspec0;           /* --oldspec0  M35: GX_AF_SPEC on channel 0 read as the distance
                             *   attenuation (M3..M35: m425's Thwomps opaque) */
    int nolitalpha;         /* --nolitalpha  M35: the alpha channel unlit (the material's
                             *   alpha), as M3..M35 */
    int oldczero;           /* --oldczero  M35: lerp(a, b, 0) keeps b as a live input
                             *   (m425's sea stage PREV + TEXC drawn as TEXC), as M3..M34 */
    int clrasclr;           /* --clrasclr  M35: GXColor3u8's three bytes are filed as a
                             *   colour whatever the descriptor's next attribute is
                             *   (hsfman.c's background quad, written as U8 positions
                             *   through it, drew nothing), as M3..M34 did */
    int nohoistmtx;         /* --nohoistmtx  M41: the vertex-array loops reload the
                             *   matrix every vertex, as before */
    int nofastconcat;       /* --nofastconcat  M41: C_MTXConcat is the SDK body (the
                             *   operands reloaded after every store) */
    int nostripes;          /* --nostripes  M41: a rewritten texture is decoded and
                             *   uploaded whole at every flush (no tile-row stripes) */
    int nounitmemo;         /* --nounitmemo  M41: the TEV unit layout and register
                             *   shape decided afresh at every call in a draw */
    int norotmemo;          /* --norotmemo  M41: C_MTXRotRad calls libm's sinf/cosf
                             *   directly (not through the sin/cos memo), as before */
    int nopacklights;       /* --nopacklights  M41: a lit variant over the card's
                             *   instruction limit goes to the CPU path, as before
                             *   (no packed-lights program) */
    int nofastsin;          /* --nofastsin  M42: the sin/cos memo's misses call libm's
                             *   sinf/cosf (no libm-identical fast path first) */
    int nomtxmemo;          /* --nomtxmemo  M42: mtxRot/mtxRotCat run the game's bodies
                             *   at every call (no memo on the angles' bits) */
    int norotl;             /* --norotl  M42: mtxRot/mtxRotCat's miss path is the game's
                             *   body (MTXRotRad + MTXConcat), not the sparse left product */
    int nowb;               /* --nowb  M42: the vertex cache re-hashes every array at
                             *   every epoch (no write barrier on MEM1 pages) */
    int notexskip;          /* --notexskip  M43: a render-thread decode job whose TEX0 no texgen
                             *   reads goes to the general walker, as before (PLAN.md 58.8) */
    int nostripepool;       /* --nostripepool  M43: the texture stripes' decode and upload
                             *   buffers allocated and freed each update, as before */
    /* M44 (PLAN.md 59): the GX front end, built for the 7455 -- each lever's old path */
    int nodcbz;             /* --nodcbz  M44: no dcbz ahead of the render stream's and the
                             *   vertex ring's writes (a store miss reads the line first) */
    int novpgen;            /* --novpgen  M44: the vertex program's parameters compared value by
                             *   value every draw, not skipped by the lights'/channels'/
                             *   texture matrices' generations */
    int notevdirty;         /* --notevdirty  M44: the TEV signature hashed every draw, not only
                             *   after a TEV setter changed something */
    int noattrmemo;         /* --noattrmemo  M44: a primitive's layout and decode plan derived
                             *   every primitive, not kept while nothing they read has changed */
    int nowordhash;         /* --nowordhash  M44: the texture content hash byte by byte (FNV) */
    int water;              /* --water off|cheap|full|auto  M44: the indirect warp at the
                             *   vertices (src/gx/gx_water.c); -1 auto (the default: by the
                             *   machine's class and the screen) */
    int watergrid;          /* --watergrid N  M44: full's subdivision levels (default 1) */
    int novcpos;            /* --novcpos  M44: a stored run whose position array alone moved is
                             *   decoded whole again (and the list goes animated), not refreshed */
    int wbpart;             /* --nowbpart  M43: the write barrier arms the partial pages at an
                             *   array's (a list's) ends too, the default (src/gx/gx_wb.c;
                             *   PLAN.md 58.4); --nowbpart is M42's interior pages alone */
    int oldvtxjoin;         /* --oldvtxjoin  M43: ClusterProc/ShapeProc wait for the whole
                             *   decode stream (M29), not only the records that read the
                             *   buffers they rewrite (src/os/vtx_rewrite.c) */
    int rtgx;               /* --rtgx 0|1|auto  M43: the translation on the render thread
                             *   (src/gx/gx_rtgx.c; PLAN.md 58.2): 0 the game thread's, 1 every
                             *   drawn frame, 2 = auto by both threads' last cycles */
    double rtgx_fit;        /* --rtgxfit MS  M43: auto hands over only while the game
                             *   thread's cycle is over this (29 ms) */
    int vcarr_from, vcarr_to; /* --vcarr A,B  M43: the vertex cache's reasons per display
                             *   list, frames A..B from the minigame's entry (PLAN.md 58.5) */
    int pmcwin_from, pmcwin_to; /* --pmcwin A,B  M43: --pmc counts frames A..B from the
                             *   minigame's entry (or retrace frames A..B without one) */
    int pmc;                /* --pmc N  M43: the G4's performance counters on the game
                             *   thread, event set N (1..3), split by region
                             *   (src/debug/pmc.c; PLAN.md 58.4) */
    int nomotionexec;       /* --nomotionexec  M42: Hu3DMotionExec is the game's body
                             *   (not the port's compile of the same statements) */
    int nodirtyfilter;      /* --nodirtyfilter  M41: every DC store/flush scans the whole
                             *   texture cache (no page filter first), for the A/B */
    int nodirty;            /* --nodirty  M35: DCStoreRange/DCFlushRange do not mark
                             *   the texture cache; a CPU-rewritten texture is seen
                             *   only by the sampled hash, as before (m415's stamps) */
    int oldakonst;          /* --oldakonst  M35: an alpha read as a colour (A0-2,
                             *   the alpha konsts) claims the unit's A half even
                             *   when the RGB half is free, and lerp(a,0,0)+0 is
                             *   an ADD with a black constant, as M16..M34 did
                             *   (the shadow pass's A1^2 maps; PLAN.md 50) */
    int mdposonly;          /* --mdposonly  M30: multi-draw a position-only layout too (m434's pond vanishes) */
    int mdmax;              /* --mdmax N  M30: at most N vertices per glMultiDrawArraysEXT call (0 = no cap) */
    int nospot;             /* --nospot  M30: spot lights without their cone, as before (PLAN.md 45 B) */
    int oldfog;             /* --oldfog  M30: GL_EXP by 1/(end-start+1) of |z|, and a degenerate range fogs (PLAN.md 45 C) */
    int noregchain;         /* --noregchain  M30: a register chain's reads as the constants, as before (PLAN.md 45 B) */
    int nocarry;            /* --nocarry  M31: no scalar-in-alpha fold for m417's pool shape (PLAN.md 46) */
    const char* forceobj;   /* --forceobj NAME[:flags]  M31 diagnostic: the object's draws without cull (1) / z test (2) / alpha test (4) */
    int forceobj_flags;
    const char* skipobj;    /* --skipobj NAME  M33 diagnostic: the object's draws not issued */
    int zprepass;           /* --zprepass N  M33: a depth-only pass before a draw whose alpha
                             *   test kills fragments the hardware still writes Z for: 1 = with
                             *   GXSetZCompLoc(TRUE) only (the default since M34), 2 = every
                             *   alpha-tested z-writing draw (the title says no), 0 = never */
    int probebox[4];        /* --probebox X0,Y0,X1,Y1  M33: --probeobj counts only that box (GL rows) */
    int probeverts;         /* --probeverts N  M34: --probeobj prints up to N vertices (6) */
    int skipverts;          /* --skipverts N  M34 diagnostic: draws of exactly N vertices not issued */
    const char* probeobj;   /* --probeobj NAME  M33 diagnostic: the object's draws bracketed by a
                             *   read-back of the framebuffer (pixels changed, their box) and a
                             *   print of the GL state and the first vertices as issued */
    int nolinewidth;        /* --nolinewidth  M30: GXSetLineWidth ignored, every line 1 px (PLAN.md 45 H) */
    int noregfix;           /* --noregfix  fold a stage's GX_TEVREG write to
                             *   PREV the way every build before M16 did (the
                             *   board eyes).  The A/B lever (PLAN.md 31.3)  */

    /* ---- M17: frame mode ---- */
    int realtime;           /* --realtime (default)  the retrace at 60 Hz on the
                             *   wall clock, frames drawn when the renderer
                             *   can (src/platform/framemode.c, PLAN.md 32)  */
    int lockstep;           /* --lockstep  every frame drawn and the game as
                             *   slow as the renderer: the pre-M17 gate.
                             *   --turbo and --nodraw imply it               */
    int maxskip;            /* --maxskip N  at most N consumed frames between
                             *   two drawn ones (default 5: a present at
                             *   least every 6th retrace)                     */
    const char* perfdump;   /* --perfdump FILE  every --perf sample as CSV  */
    int mpgl;               /* --mpgl  Apple's multithreaded GL engine
                             *   (kCGLCEMPEngine): the driver on the second
                             *   CPU.  Off until measured (PLAN.md 32)       */
    int olddecode2;         /* --olddecode2  convert 8-bit components with
                             *   the arithmetic in the loop instead of the
                             *   byte tables (the pre-M17 decode); the A/B
                             *   lever (PLAN.md 32)                           */
    int altivec;            /* --altivec  the AltiVec PSMTXROMultVecArray.
                             *   Exact (tests/mtx_test.c) and 3.5% slower on
                             *   consumed board frames, so off (PLAN.md 32) */
    int nodcbt;             /* --nodcbt  no dcbt of the next vertex's array
                             *   entries in the specialised loops (M17's
                             *   --noprefetch; renamed in M36 for the loader) */
    int olddecode3;         /* --olddecode3  never take the specialised loops
                             *   for the common plan shapes; the A/B lever  */
    int decodestats;        /* --decodestats  shared-index and repeated-tuple
                             *   counts per decoded vertex (PLAN.md 32)     */
    int audiolead_set;
    int audiolead;          /* --audiolead MS  silence queued ahead of the mix
                             *   at the first paced retrace, so a drawn frame
                             *   that overruns does not starve the device
                             *   (default 100 under --realtime, else 0)       */
    /* M18 (PLAN.md 33): the skinning and the matrix palette */
    int cpuskin;            /* --cpuskin   the game's own EnvelopeProc every
                             *   frame, CPU skinning as on the console; the
                             *   A/B lever for the GPU palette path          */
    int palette;            /* --palette   the vertex-program matrix palette:
                             *   batches span GXLoadPosMtxImm and skinned
                             *   meshes are skinned by the card.  Measured
                             *   SLOWER on this driver (ARL runs in software,
                             *   PLAN.md 33.2), so opt-in                    */
    int nopalette;          /* --nopalette (the default; kept for scripts)  */
    int skinstats;          /* --skinstats per-mesh envelope shape and the
                             *   per-frame counts                            */
    int palsize;            /* --palsize N  palette slots per batch (max 24) */
    int palnoarl;           /* --palnoarl  diagnostic: palette batches, arrays and
                             *   uploads as usual, but the program reads env[0..5]
                             *   (no ARL) -- wrong picture, times the ARL     */
    int palnofog;           /* --palnofog  diagnostic: no fog-coordinate array
                             *   bound (every vertex reads slot 0) -- wrong
                             *   picture, times the array                     */
    int noskinlifetime;     /* --noskinlifetime  the M18 registry: no drop at the
                             *   game's frees, no guards before a draw-time read
                             *   (the reproduction of the M18 fault; PLAN.md 34) */
    int restore_lax;        /* --restore-lax  accept a snapshot whose build id
                             *   differs (the id hashes the executable's size and
                             *   mtime): for a re-link of the SAME source only --
                             *   MEM1 holds code addresses, so a rebuild that
                             *   moves any function is unsound even with an
                             *   identical snapmap; PLAN.md 34                 */
    int skindeferall;       /* --skindeferall  defer the bone walk too, not just
                             *   the vertex skinning: +2 fps on the board and a
                             *   rounding-level game-state divergence through
                             *   hsfdraw.c's MTXBuf (PLAN.md 33.3); off      */
    const char* mixtrace;   /* --mixtrace FILE  every voice's mixer inputs, one
                             *   line per voice per DSP frame: diff two builds'
                             *   files and the first line names the field that
                             *   diverged (PLAN.md 34)                       */
    int mixcheck;           /* --mixcheck  the mixer's 32-bit gain/accumulate
                             *   checked against the 64-bit form per sample */
    int nosincos;           /* --nosincos  PSMTXRotRad straight to libm (M18) */
    int snapsync;           /* --snapsync   write snapshots on the game thread
                             *   (pre-M18: a 3 s stall each) instead of on a
                             *   worker from a copy taken at the retrace     */
    /* ---- M28: the experiments (PLAN.md 43), each an exact rewrite ---- */
    int nomatwalk;          /* --nomatwalk  FaceDraw's material setup runs on
                             *   consumed frames as written (gx_matwalk.c)   */
    int nocurvememo;        /* --nocurvememo  GetCurve to the game's body every
                             *   call (curve_memo.c)                          */
    int nosparsemtx;        /* --nosparsemtx  SetEnvelopMtx's concats through
                             *   PSMTXRotRad + PSMTXConcat (psmtx_c.c)       */
    int nofastsqrt;         /* --nofastsqrt  VECMag/VECNormalize/VECDistance's
                             *   sqrtf through libm (psmtx_c.c port_sqrtf)   */
    int nodraw_user;        /* --nodraw was on the command line (--ffto sets
                             *   nodraw itself and used to switch drawing back
                             *   on at its end whatever the user asked)     */
    /* ---- M36: the loader (PLAN.md 51) ---- */
    int noprefetch;         /* --noprefetch  no read-ahead of a dealt minigame's
                             *   files (the REL, its bundle, its data) at the
                             *   roulette, of the board's at the results, of
                             *   the board set at the mode select          */
    int resident;           /* --resident MB  the resident set's budget; -1 =
                             *   the machine check's rule (0 under 768 MB of
                             *   RAM, 128 at 1 GB, 256 at 1.5 GB+), 0 = off  */
    int dvdlog;             /* --dvdlog  one line per DVD read: the frame, the
                             *   file, the range, the time, where it came from */
    /* ---- M38: the movies (PLAN.md 53) ---- */
    int nomovies;           /* --nomovies  skip every THP movie the way M2-M37
                             *   did (patches.txt's two thpmain.c patches) */
    int thpyuv;             /* --thpyuv  draw a movie frame the game's way --
                             *   three I8 planes and THPDraw.c's five TEV
                             *   stages through the port's GX -- instead of
                             *   the CPU's RGBA (the A/B of PLAN.md 53.4)    */
    const char* gotoovl;    /* --goto OVL[:EVT[:CHAR]]  boot straight into an
                             *   overlay at an event (omMasterInit's first
                             *   call, patches.txt), four players set up with
                             *   CHAR first -- e.g. mstory2dll:4:0, the story
                             *   ending (PLAN.md 53.8) */
    int board;              /* --board N[+]  M39c: the party board the menus
                             *   pick, 1-6 (Toad's .. Bowser's), written into
                             *   GWSystem.board at mentDll's BoardSaveInit
                             *   (patches.txt) and parked there while mentDll
                             *   runs; 0 = the game's own cursor.  "N+" moves
                             *   on to the next board at every board a --soak
                             *   chains (PLAN.md 54b.1)                      */
    int boardcycle;         /* the "+" of --board N+                           */
    const char* boarddump;  /* --boarddump A,B,..  M39c: dump the frames A,B,..
                             *   counted from the frame a board overlay
                             *   (w01..w21) is first entered -- the board
                             *   gallery's twin of --mgdump                   */
    int foldxbar;           /* --foldxbar  the M30 three-texture fold's units A and
                             *   B read across with the crossbar, as M30-M37 did
                             *   (m448's felt is black on the Radeon that way) */
    int foldcap;            /* --foldcap N  cut the M30 three-texture fold after
                             *   N units (9: N = 1..6 by frame) -- m448's
                             *   felt bisect, PLAN.md 53.10                   */
    int thplog;             /* --thplog  one line per movie frame: decoded,
                             *   drawn, dropped, the decode's ms              */
    /* ---- M39: the movies on one CPU (PLAN.md 54.2) ---- */
    int nothpslice;         /* --nothpslice  no decode in the retrace's slack:
                             *   an owed frame is decoded whole at its draw,
                             *   as in M38                                    */
    int thpguard_set;
    int thpguard;           /* --thpguard MS  a drawn frame finishes an owed
                             *   movie frame only if the queued audio covers
                             *   the work left + MS (default 60; 0 = always,
                             *   M38)                                         */
} PortOptions;

extern PortOptions port_opt;

int port_parse_args(int argc, char** argv);

/* ---- the loud-stub table ------------------------------------------------- */
/* Every generated stub calls this.  The first call for a name prints it; all
 * calls are counted, in first-call order, which is exactly the "SDK surface
 * the boot actually hit" that M1 is after. */
void port_stub(const char* name);
void port_stub_report(void);

/* ---- diagnostics --------------------------------------------------------- */
void port_log(const char* fmt, ...);
void port_logv(const char* fmt, va_list ap);
void port_log_async_start(void); /* M40: the writer thread (after the options, --synclog) */
void port_log_sync(void);        /* M40: drain the ring and log in place from now (a fault) */
void port_fatal(const char* fmt, ...);
void port_shutdown(int code); /* the one exit path: report, flush, close SDL */
int port_write_png(const char* path, int w, int h, const unsigned char* rgb_bottom_up); /* M32 */
void port_print_effective(FILE* f, int live); /* M32: every option's effective value (f NULL = the log;
                                               * live = the workers and render thread have started) */
void port_print_keys(FILE* f);      /* M32: the key and pad table */

/* M25: the machine check (src/platform/machine.c).  Runs once at boot before
 * the window opens; the title is what the window shows until the first drawn
 * frame, the summary is the one-line inventory + verdict. */
void port_machine_check(void);
int port_prc_stack_mul(void);                /* os_misc.c: the coroutine stacks' multiplier (M29/M31) */
const char* port_machine_title(void);
const char* port_machine_summary(void);
const char* port_machine_verdict(void);   /* "ok" / "degraded" / "unsupported" */
int port_machine_degraded(void);
int port_machine_reasons(const char** out, int cap);

/* M25: the config file and the dialogs (src/platform/config.c) */
const char* port_app_support_dir(void);      /* ~/Library/Application Support/MarioParty4 */
void port_config_load(void);
const char* port_config_get(const char* key); /* NULL when absent */
void port_config_set(const char* key, const char* val);
void port_config_save(void);                 /* only when something changed */
int port_dialog_notice(const char* title, const char* text);
int port_dialog_choose_image(char* out, size_t n);

/* --perf, src/debug/perf.c */
void port_perf_gx_begin(void);
void port_perf_gx_end(void);
void port_perf_present_begin(void);
void port_perf_present_end(void);
void port_perf_slept(double seconds);
/* --gxsplit (M21): exclusive sub-regions of the gx time.  Nested enters
 * charge the inner region only; every enter is paired with a leave. */
enum {
    PERF_SUB_DECODE = 0, /* the display-list / immediate decode into the ring */
    PERF_SUB_XF,         /* finish_vertices: the CPU transform path            */
    PERF_SUB_STATE,      /* gl13_apply_transform/raster + gx_tev_apply         */
    PERF_SUB_TEX,        /* gx_tex_bind: hash, decode, upload, bind            */
    PERF_SUB_ISSUE,      /* gx_vprog_bind + the VAR flush + the draw calls     */
    PERF_SUB_INDEX,      /* M21: building the batch's index list               */
    /* M44 (PLAN.md 59): finer, for --pmc's per-function counts */
    PERF_SUB_TEV,        /* gx_tev_apply (its binds are texbind)               */
    PERF_SUB_VPDRAW,     /* fill_xf_desc + gx_vprog_draw: the variant, params  */
    PERF_SUB_VPBIND,     /* gx_vprog_bind: the arrays and the parameters       */
    PERF_SUB_ATTR,       /* begin_attr_order: a primitive's layout and plan    */
    PERF_SUB_VCKEY,      /* vc_list_begin / vc_decide: the vertex cache's keys */
    PERF_SUB_PRIM,       /* batch_prepare / batch_add / ring_claim / pal_place  */
    PERF_SUB_JOBB,       /* rtdec_build: the render thread's job for a run      */
    PERF_SUB_PEND,       /* decode_pending_last: the run's last vertex, here    */
    PERF_SUB_JREC,       /* rt_decode_record: the job into the stream          */
    PERF_SUB_FLUSH,      /* batch_flush's own work (a batch's submit, less the above) */
    PERF_SUB_N
};
/* M44: the sub-regions are entered only when something reads them (--gxsplit
 * or --pmc): one load and a branch otherwise */
extern int port_sub_on;
#define PORT_SUB_ENTER(w)                                                                \
    do {                                                                                 \
        if (__builtin_expect(port_sub_on, 0)) {                                          \
            port_perf_sub_enter(w);                                                      \
        }                                                                                \
    } while (0)
#define PORT_SUB_LEAVE()                                                                 \
    do {                                                                                 \
        if (__builtin_expect(port_sub_on, 0)) {                                          \
            port_perf_sub_leave();                                                       \
        }                                                                                \
    } while (0)
void gx_tex_frame_decode_take(unsigned* n, unsigned* src_kb, unsigned* rgba_kb,
                              double* decode_ms, double* upload_ms, double* hash_ms,
                              unsigned* rekeys); /* M23 */
void gx_tex_predecode_frame_take(unsigned* taken, unsigned* claimed, unsigned* unstaged,
                                 double* saved_ms); /* M24 */
void port_perf_sub_enter(int which);
void port_perf_sub_leave(void);
void port_perf_audio_begin(void);
void port_perf_audio_end(void);
void port_perf_frame(int drawn);
void port_perf_report(void);
/* --pmc (M43), src/debug/pmc.c: the counters split by region, exclusive */
enum {
    PMC_R_REST = 0, PMC_R_PRC, PMC_R_3DEXEC, PMC_R_SHADOW, PMC_R_MOTION, PMC_R_DRAW,
    PMC_R_DRAWPOST, PMC_R_OBJMTX, PMC_R_GX, PMC_R_AUDIO, PMC_R_PRESENT,
    PMC_R_GXSUB, /* M44: + PERF_SUB_*, the GX region's sub-regions */
    PMC_R_N = PMC_R_GXSUB + 16
};
extern int pmc_on;
void port_pmc_init(void);
void port_pmc_enter(int region);
void port_pmc_leave(void);
void port_pmc_frame(int drawn);
void port_pmc_report(void);
void port_perf_window(double* fps, double* aud_ms, double* speed_pct,
                      double* presented_fps); /* --status's rolling second */

/* what the long-running process holds (M18): the texture cache's GL-resident
 * size (gx_tex.c) and the process's resident set (src/platform/main.c) */
void gx_tex_cache_stats(unsigned* entries, unsigned* kb);
unsigned port_rss_mb(void);

/* --realtime, src/platform/framemode.c */
void port_framemode_init(void);
int port_framemode_active(void);
void port_framemode_decide(double late, double now);
int port_framemode_next_drawn(void);
void port_framemode_resync(double behind);
double port_framemode_resync_limit(void);
void port_framemode_window(double* speed_pct, double* presented_fps, double dt);
void port_framemode_report(void);

/* --scenelog, src/debug/scenelog.c */
void port_scenelog(void);
void port_nanwatch(void);
void port_ovllog(void);

/* The self-play harness's own controller, port/src/pad/pad.c.
 *
 * A press the harness makes has to enter the game at the same seam a real pad
 * does -- `PADRead`'s raw `PortPadRaw`, which is where a `--play` script sits.
 * Writing `HuPadBtnDown[]` from the retrace callback does nothing at all:
 * src/game/main.c:101 calls `HuPadRead()` at the top of every frame, which
 * overwrites both `HuPadBtn[]` and `HuPadBtnDown[]` from `_PadBtn*` before
 * `HuPrcCall(1)` dispatches a single coroutine.  PLAN.md 29.1.
 *
 * `port_pad_inject(buttons)` arms one frame's worth of digital buttons; the
 * next `PADRead` ORs them into port 1's raw state and disarms.  The game's own
 * edge and repeat logic then derives `BtnDown`/`DStkRep` exactly as it would
 * from a human thumb. */
void port_pad_inject(unsigned short buttons);

/* the self-play harness, port/src/debug/selfplay.c */
void port_selfplay_init(void);
void port_selfplay_tick(unsigned frame);
const char* port_drawobj_name(const void* mtx, int* model_index);
void port_clock_mark(void);
void port_clock_report(void);

/* ---- the clock ----------------------------------------------------------- */
/* mach_absolute_time, because clock_gettime is not in the 10.4 SDK. */
unsigned long long port_now_ns(void);
double port_now_seconds(void);

/* ---- MEM1 / ARAM --------------------------------------------------------- */
void port_mem_init(void);
void* port_mem1_lo(void);
int port_in_game_region(const void* p); /* M34: the game stack, MEM1 or a guard */
void* port_mem1_hi(void);
void* port_aram(void);
/* Name the port-owned region an address falls in ("MEM1", "the guard above
 * MEM1", ...), or NULL if the port does not own it.  The crash handler turns a
 * fault address into a sentence with these. */
const char* port_mem_region_name(const void* addr, long* off, const void** base,
                                 const void** end);
const char* port_mem_guard_of(const void* addr, const void** rlo,
                              const void** rhi);
void port_mem_regions_dump(void);
/* --guardtest WHERE: deliberately step one byte outside a region, so that the
 * guards and the crash handler's region line can be proved on a machine that
 * is not the one the bug was found on.  WHERE is mem1-hi, mem1-lo, aram-hi,
 * aram-lo or stack-lo. */
void port_guard_selftest(const char* where);

/* The high 32 bits of every address the game may truncate into a u32 field.
 * On the G4 these are zero and every reconstruction is the identity; on a
 * 64-bit host they are what makes `jmp_buf.lr` / `jmp_buf.sp` work.  See
 * port/src/os/jmp_host.c. */
extern uintptr_t port_text_base_hi;
extern uintptr_t port_stack_base_hi;

/* ---- M10: fast-forward and snapshots (src/debug/snapshot.c) -------------- */
/* --ffto: drawing is off until frame N, so a deterministic run reaches N in a
 * fraction of the wall clock.  Called at the top of every retrace, before the
 * present that would number the next frame. */
void port_ffto_init(void);
void port_ffto_tick(void);
/* The snapshot ring.  Taken at the top of the retrace, outside any GX or
 * audio call, which is the only point at which the game's own state is
 * quiescent and the port's coroutine is the game's. */
void port_snap_init(void);
void port_snap_tick(void);
void port_snap_report_existing(void); /* the fault handler asks for this */
/* Port-side state that a snapshot must carry because it is not in the arenas:
 * counters, clocks, replay positions.  Everything else the port owns is a host
 * resource and is re-derived on restore (PLAN.md 24.2). */
void port_snap_register(const char* name, void* p, unsigned long size);
int port_snap_restore_pending(void); /* --restore was given and not yet done */
void port_snap_restore(void);        /* ...do it; does not return */
void port_snap_report(void);

/* ---- host loop ----------------------------------------------------------- */
void port_vi_init(void);
void port_host_service(void); /* called from VIWaitForRetrace, once per frame */

/* ---- subsystems ---------------------------------------------------------- */
void port_dvd_init(void);
void port_dvd_service(void);
/* M36: the loader -- the roulette prefetch and the resident set
 * (src/dvd/dvd_cache.c, PLAN.md 51) */
int port_dvd_entry_count(void);
const char* port_dvd_entry_path(int n);
unsigned port_dvd_entry_length(int n);
unsigned port_dvd_entry_offset(int n);
const char* port_dvd_image_path(void);
const char* port_dvd_tree_root(void);
void port_dvd_cache_init(void);
void port_dvd_cache_shutdown(void);
void port_dvd_cache_service(void);
void port_dvd_cache_report(void);
int port_dvd_cache_serve(int entry, unsigned offset, void* dst, unsigned len);
void port_dvd_cache_disk_read(int entry, unsigned offset, const void* data, unsigned got, double dt);
void port_dvd_cache_status(char* buf, size_t n);
int port_dvd_cache_budget_mb(void);
void port_mg_dealt(int mg_index); /* the patch hook: mg_setup.c's roulette */
double port_vi_slack_seconds(void); /* vi.c: the pacing sleep this retrace would take */
const char* port_dll_bundle_path(const char* relpath);
void port_thp_report(void);
/* M38 (PLAN.md 53): the movies, port/src/thp */
void port_thp_retrace(void);
void port_thp_slack(double deadline); /* M39: one CPU, the owed frame in the slack */
void port_idle_tick(void);          /* os/sreset_poll.c: the idle function's pass */
void port_idle_trap(void);          /* the top of VIWaitForRetrace */
void port_idle_snap_register(void);
void port_ai_next_callback_is_thp(void); /* musyx_sal.c: THPInit's mark */        /* VIWaitForRetrace: the game's idle function, once */
void port_thp_status(char* buf, unsigned long n); /* the --status field, "" when no movie */
int port_thp_audio_active(void);    /* THPSimple's mixer is chained into the AI callback */
/* the AI's DMA source for the current 160-sample period (musyx_sal.c) */
void port_ai_dma_period(void* musyx_buf);
void* port_ai_dma_take(void);

/* M38: a port-owned RGBA8 image drawn through GX (gx_tex.c's tex_bind_port):
 * GXInitTexObj with format GX_TF_PORT_RGBA and image = a PortTexture*.  The
 * owner puts a malloc'd w*h*4 row-major frame in `pending`; the next bind
 * uploads it as a sub-image of a power-of-two texture made once and passes
 * the buffer to the render thread to free. */
#define GX_TF_PORT_RGBA 0x7E
typedef struct PortTexture {
    unsigned gl_name;
    int w, h, pw, ph;
    void* pending;
    volatile int* pending_done; /* NULL: the upload frees pending; else it sets *pending_done = 1 */
    int argb;          /* pending is A R G B bytes: GL_BGRA / 8_8_8_8_REV, the Mac's own */
    unsigned long uploads;
} PortTexture;
void port_card_report(void);
void port_card_service(void);        /* M29: reap the image writer (per retrace) */
void port_arq_service(void);

/* ---- audio (port/src/audio, PLAN.md §16) --------------------------------- */
/* The SAL's deterministic tick: one call per retrace, from the gate.  It is
 * the only thing that makes MusyX advance, and it advances by an exact
 * integer number of 160-sample frames, never by the wall clock. */
void port_audio_tick(void);
void port_audio_shutdown(void);
void port_audio_report(void);
/* MusyX's own ARAM allocator, ported off its stubbed PC arm (musyx_aram.c) */
void port_musyx_aram_report(void);
extern int port_audio_enabled; /* cleared by --noaudio */
void port_audio_join(void);    /* M24: finish the retrace's mixer job (game thread) */

/* ---- M24: the workers (port/src/platform/workers.c) ------------------------
 * Plain pthreads for work that has no dependency on the game's frame.  The
 * game is single-threaded and stays so: a job is published at the retrace
 * boundary with a copy of everything it reads, runs while the game runs the
 * next frame, and is joined at the next retrace, where the game thread
 * finishes it itself if the worker never picked it up.  With one CPU or
 * --threads 0 no job is ever built and every path is today's. */
typedef struct PortJob {
    void (*run)(struct PortJob* j);
    volatile int state;   /* PORT_JOB_IDLE / QUEUED / RUNNING / DONE */
    double t_queued, t_start, t_end;
    double fpscr;         /* the publishing thread's FPU control register (workers.c) */
    int ran_inline;       /* finished by the game thread at the join */
} PortJob;
enum { PORT_JOB_IDLE = 0, PORT_JOB_QUEUED, PORT_JOB_RUNNING, PORT_JOB_DONE };
typedef struct PortWorker PortWorker;
int port_ncpu(void);
int port_threads_on(void);          /* the workers are up */

/* ---- M27: the render thread (src/gx/rt.c, PLAN.md 42) ----
 * One GL thread; the game thread records every GL call into a stream the
 * render thread replays in order.  --renderthread 0 = the direct path, 1 =
 * the same stream replayed inline (the single-core twin), 2 = the thread
 * joined at every frame's end, 3 = overlapped (the default on two cores). */
typedef struct {
    const char* text;
    int len;
    unsigned target;   /* 0 = GL_VERTEX_PROGRAM_ARB; M35: 0x8200 = GL_TEXT_FRAGMENT_SHADER_ATI */
    unsigned id;       /* 0 = refused (errpos != -1, or not under the native limits) */
    int errpos;
    int native;        /* GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB */
    int under_native;
    char msg[200];
} RtCompile;
void rt_compile_vprog(RtCompile* c);  /* a join */
void rt_start(void* sdl_window, void* sdl_glcontext);
void rt_stop(void);
int rt_on(void);                      /* a render thread exists (mode >= 2) */
int rt_mode(void);
void rt_present(unsigned frame);
void rt_join(const char* why);        /* drain the stream; counted by name */
int rt_gate(double max_s);            /* drained, or drained within max_s */
void rt_ring_enter(int chunk);      /* the ring writer reuses a chunk: the join at reuse */
void rt_report(void);
void rt_status(char* buf, size_t n);
double rt_last_frame_ms(void);
/* M29 (PLAN.md 44): the decode records.  rt_decode_on says whether the
 * display-list decode goes into the stream; rt_pos is the writer's position
 * (a stamp); rt_decode_join waits until the render thread's decode cursor has
 * passed everything recorded so far, counted by name; rt_last_dec_ms is the
 * decode time the render thread spent on the last presented frame. */
int rt_decode_on(void);
unsigned rt_pos(void);
void rt_decode_join(const char* why);
void rt_decode_join_pos(unsigned pos, const char* why); /* up to a stamped position */
double rt_last_dec_ms(void);
/* M40 (PLAN.md 55): the records the game thread has written (every GL call,
 * for --perfdump), and a join that leaves the GPU idle -- every record
 * replayed, then glFinish -- for the vertex cache's reset */
unsigned long rt_records_written(void);
void rt_finish_join(const char* why);
void gx_draw_counters(unsigned long* calls, unsigned long* verts, unsigned long* vchit);
/* M33 (PLAN.md 48): --rtdecode auto.  rt_decode_want says whether this run is
 * the render thread's (the frame's share, spread by vertices); a run decoded
 * on the game thread is reported with rt_decode_here (timed), one handed over
 * with rt_decode_there; vi.c tells the planner each frame's end (drawn or
 * consumed, the game thread's seconds on it) and a drawn frame's start. */
int rt_decode_mode(void);           /* 0/1/2/3 as decided at start */
int rt_decode_want(unsigned verts);
void rt_decode_here(unsigned verts, double seconds);
void rt_decode_there(unsigned verts);
void rt_auto_frame_end(int drawn, double seconds);
void rt_auto_frame_begin(void);
double rt_auto_last_share(void);
double rt_auto_frame_gdec_ms(void);
int rt_vcache_inputs(double* replay_ms, double* dec_ms, double* rate_ms, double* game_ms); /* M40 */
void port_vtx_rewrite(const char* who); /* ShapeProc/ClusterProc (patches.txt): the join */
extern int rt_recording;
void port_workers_init(void);       /* after the options: reads --threads and hw.ncpu */
void port_workers_shutdown(void);
void port_workers_report(void);
PortWorker* port_worker_mixer(void);
PortWorker* port_worker_decode(void);
int port_worker_submit(PortWorker* w, PortJob* j);  /* 0: workers off, run it yourself */
void port_worker_join(PortWorker* w, PortJob* j);   /* game thread: it is done when this returns */
int port_worker_done(const PortJob* j);
void port_worker_stats(PortWorker* w, unsigned long* jobs, unsigned long* late_inline,
                       unsigned long* late_wait, double* wait_ms, double* busy_ms);
void port_workers_retrace_join(void); /* the top of VIWaitForRetrace: every retrace-bound job finished */

/* the output device, port/src/audio/audio_out_sdl.c */
int port_audio_out_init(void);
void port_audio_out_queue(const void* samples, unsigned bytes);
unsigned port_audio_out_queued(void);
int port_audio_out_opened(void);           /* M39: the device is playing the ring */
unsigned long port_audio_out_underruns(void); /* M39: the count so far */
void port_audio_out_prime(unsigned ms); /* --audiolead: silence ahead of the mix */
void port_audio_out_shutdown(void);
void port_audio_out_report(void);
int port_audio_wav_start(const char* path);
void port_audio_wav_write(const void* samples, unsigned bytes);
void port_audio_wav_finish(void);

/* ---- REL modules (port/src/os/dll_load.c) -------------------------------- */
void* portDLLOpen(const char* relpath);
void* portDLLReenter(const char* relpath, void* handle);
int port_dll_selftest(void);
void port_dll_report(void);

/* ---- the soft-reset watcher (port/src/os/sreset_poll.c) ------------------- */
void port_reset_init(void);
void port_reset_thread_tick(void);
void port_reset_report(void);
int port_reset_requested(void);
void port_request_reset(void);

/* ---- GX / the window (port/src/gx, port/src/platform/window_sdl.c) -------- */
void port_gx_init(void);
void port_gx_present(void);   /* called at the retrace gate when a swap is due */
void port_gx_frame_number(unsigned n);

/* the game's own entry point, renamed by -Dmain=mp4_game_main */
void mp4_game_main(void);

#ifdef __cplusplus
}
#endif

#endif
