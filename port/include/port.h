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
    int noaudio;            /* --noaudio  HuAudInit/msm succeed as silent stubs */
    int glcheck;            /* --glcheck  assert no GL call outside GL 1.3    */
    int glinfo;             /* --glinfo   dump GL strings, limits, extensions */
    long long seed;         /* --seed N   deterministic clock origin (RNG seed) */
    int perf;               /* --perf     per-frame game/gx/present timing     */
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
    int turns;              /* --turns N  the board's turn count                */
    int status;             /* --status  one state line per second              */
    int stuckwatch;         /* seconds of no scene change before the watchdog
                             *   names the live screen (0 = off)                */
    int soak;               /* --soak  boot, walk in, play, restart, forever    */
    int depop;              /* --nodepop clears it: the voice cut-off ramp      */
    int resample4;          /* --resample1 clears it: the 4-tap resampler       */
    int clickstat;          /* --clickstat  count mix discontinuities in-process */
    /* ---- M9 ---- */
    int nodlcache;          /* --nodlcache  decode every display list every
                             *   frame, the way M8 did: the A/B for the cache  */
    const char* perfwin;    /* --perfwin A-B[:NAME][,...]  per-scene fps out of
                             *   one run's own per-frame samples, so a baseline
                             *   costs one boot instead of three                */
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
void port_fatal(const char* fmt, ...);
void port_shutdown(int code); /* the one exit path: report, flush, close SDL */

/* --perf, src/debug/perf.c */
void port_perf_gx_begin(void);
void port_perf_gx_end(void);
void port_perf_present_begin(void);
void port_perf_present_end(void);
void port_perf_slept(double seconds);
void port_perf_audio_begin(void);
void port_perf_audio_end(void);
void port_perf_frame(void);
void port_perf_report(void);
void port_perf_window(double* fps, double* aud_ms); /* --status's rolling second */

/* --scenelog, src/debug/scenelog.c */
void port_scenelog(void);
void port_nanwatch(void);
void port_ovllog(void);

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

/* ---- host loop ----------------------------------------------------------- */
void port_vi_init(void);
void port_host_service(void); /* called from VIWaitForRetrace, once per frame */

/* ---- subsystems ---------------------------------------------------------- */
void port_dvd_init(void);
void port_dvd_service(void);
void port_thp_report(void);
void port_card_report(void);
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

/* the output device, port/src/audio/audio_out_sdl.c */
int port_audio_out_init(void);
void port_audio_out_queue(const void* samples, unsigned bytes);
unsigned port_audio_out_queued(void);
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
