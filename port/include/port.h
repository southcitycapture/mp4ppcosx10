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
    int relzerobss;         /* --relzerobss  always zero a module's bss by
                             *   hand on load, as if dlclose never unloaded   */
    int noaudio;            /* --noaudio  HuAudInit/msm succeed as silent stubs */
    int glcheck;            /* --glcheck  assert no GL call outside GL 1.3    */
    int glinfo;             /* --glinfo   dump GL strings, limits, extensions */
    long long seed;         /* --seed N   deterministic clock origin (RNG seed) */
    int perf;               /* --perf     per-frame game/gx/present timing     */
    int drawlog;            /* --drawlog N  explain the first N draws in full  */
    int dumptex;            /* --dumptex  write every decoded texture as a PPM */
    int gxwarn;             /* --gxwarn   name every degraded GX feature      */
    int headless;           /* --headless no window; still decodes and logs   */
    const char* dumpframe;  /* --dumpframe SPEC  frames to write: N, a,b, a-b/s */
    const char* shotdir;    /* --shotdir  where --dumpframe writes            */
    int scale;              /* --scale N  window scale over 640x480           */
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
void port_perf_frame(void);
void port_perf_report(void);

/* ---- the clock ----------------------------------------------------------- */
/* mach_absolute_time, because clock_gettime is not in the 10.4 SDK. */
unsigned long long port_now_ns(void);
double port_now_seconds(void);

/* ---- MEM1 / ARAM --------------------------------------------------------- */
void port_mem_init(void);
void* port_mem1_lo(void);
void* port_mem1_hi(void);
void* port_aram(void);

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
void port_arq_service(void);

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
