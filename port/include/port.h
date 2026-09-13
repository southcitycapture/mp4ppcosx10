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

/* the game's own entry point, renamed by -Dmain=mp4_game_main */
void mp4_game_main(void);

#ifdef __cplusplus
}
#endif

#endif
