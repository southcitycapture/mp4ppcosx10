/* M32: what a bug report needs -- every option's effective value, and the key
 * table -- printed on the log at every boot (after the machine check and the
 * thread decisions, so the values are the ones the run has) and on stdout by
 * `--defaults` / `--keys`, which then exit.
 *
 * The field list is generated from PortOptions (tools/gen_optfields.py ->
 * opt_fields.h), so a new option is in the print the day it is added.  The
 * handful of settings that live outside port_opt -- the texture budget, the
 * workers' and the render thread's resolved modes, the coroutine stack
 * multiplier, the paths -- are printed by name first. */
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int port_threads_on(void);
int rt_mode(void);
int rt_decode_mode(void);
int gx_tex_budget_mb(void);
const char* port_machine_summary(void);
int port_ncpu(void);

static void emit(FILE* f, const char* line) {
    if (f) {
        fputs(line, f);
    } else {
        port_log("%s", line);
    }
}

static void emitf(FILE* f, const char* fmt, ...) {
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(f, buf);
}

void port_print_effective(FILE* f, int live) {
    char line[1400];
    int col = 0;
    int threads, rtm, dec;
    const char* pre = f ? "" : "port> options: ";

    emitf(f, "%sMario Party 4 PowerPC Edition %s (%s), the effective options\n", pre,
          PORT_VERSION_STRING, PORT_MILESTONE);
    emitf(f, "%smachine       %s\n", pre, port_machine_summary());
    emitf(f, "%sdisc image    %s\n", pre, port_opt.image ? port_opt.image : "(none)");
    emitf(f, "%sdata folder   %s  (card image, config, log)\n", pre, port_app_support_dir());
    emitf(f, "%slog           %s\n", pre, port_opt.log ? port_opt.log : "(stdout only)");
    emitf(f, "%sfullscreen    %d  (%s)\n", pre, port_opt.fullscreen,
          port_opt.noconfig ? "--noconfig: not remembered"
          : port_opt.fullscreen_set ? "from the command line, remembered"
                                     : "from the config file");
    /* the three -1 defaults are decided by workers.c and rt.c at start; before
     * that (--defaults exits after the machine check) the same rules apply */
    if (live) {
        threads = port_threads_on();
        rtm = rt_mode();
        dec = rt_decode_mode();
    } else {
        threads = port_opt.threads < 0 ? port_ncpu() > 1 : port_opt.threads > 0;
        rtm = port_opt.renderthread < 0 ? (threads ? 3 : 1) : port_opt.renderthread;
        if (rtm >= 2 && !threads) {
            rtm = 1;
        }
        dec = port_opt.rtdecode < 0 ? (rtm >= 3 ? 3 : rtm >= 2 ? 2 : 0) : port_opt.rtdecode;
    }
    emitf(f, "%scores         %d; workers %s (--threads %d); render thread mode %d "
          "(--renderthread %d: 0 direct, 1 inline twin, 2 joined, 3 overlapped); "
          "decode on it %d (--rtdecode; 3 = auto)%s\n",
          pre, port_ncpu(), threads ? "on" : "off", port_opt.threads, rtm,
          port_opt.renderthread, dec, live ? "" : "  [as they will be decided at start]");
    emitf(f, "%spacing        %s%s; stack multiplier x%d; texture budget %d MB; resampler %s, "
          "depop %s\n",
          pre, port_opt.turbo ? "turbo" : port_opt.lockstep ? "lockstep" : "realtime",
          port_opt.deterministic ? " (deterministic clock)" : "", port_prc_stack_mul(),
          gx_tex_budget_mb(), port_opt.resample4 ? "4-tap" : "linear",
          port_opt.depop ? "on" : "off");
    /* M36: the loader's two settings, as the machine check resolved them */
    emitf(f, "%sloading       roulette prefetch %s (--noprefetch); resident set %d MB "
          "(--resident MB; the machine check's rule by the installed RAM)\n",
          pre, port_opt.noprefetch ? "off" : "on", port_dvd_cache_budget_mb());
    emitf(f, "%severy field of PortOptions (0/NULL = off or unset):\n", pre);
    line[0] = '\0';
#define OPT_ADD(text)                                                          \
    do {                                                                       \
        if (col && col + (int)strlen(text) > 88) {                             \
            emitf(f, "%s  %s\n", pre, line);                                   \
            line[0] = '\0';                                                    \
            col = 0;                                                           \
        }                                                                      \
        strncat(line, text, sizeof(line) - strlen(line) - 1);                  \
        strncat(line, "  ", sizeof(line) - strlen(line) - 1);                  \
        col += (int)strlen(text) + 2;                                          \
    } while (0)
#define OPT_INT(n)                                                             \
    do {                                                                       \
        char t[160];                                                           \
        snprintf(t, sizeof(t), "%s=%d", #n, port_opt.n);                       \
        OPT_ADD(t);                                                            \
    } while (0);
#define OPT_UNS(n)                                                             \
    do {                                                                       \
        char t[160];                                                           \
        snprintf(t, sizeof(t), "%s=%u", #n, port_opt.n);                       \
        OPT_ADD(t);                                                            \
    } while (0);
#define OPT_LL(n)                                                              \
    do {                                                                       \
        char t[160];                                                           \
        snprintf(t, sizeof(t), "%s=%lld", #n, port_opt.n);                     \
        OPT_ADD(t);                                                            \
    } while (0);
#define OPT_DBL(n)                                                             \
    do {                                                                       \
        char t[160];                                                           \
        snprintf(t, sizeof(t), "%s=%g", #n, port_opt.n);                       \
        OPT_ADD(t);                                                            \
    } while (0);
#define OPT_STR(n)                                                             \
    do {                                                                       \
        char t[1200];                                                          \
        snprintf(t, sizeof(t), "%s=%s", #n, port_opt.n ? port_opt.n : "NULL"); \
        OPT_ADD(t);                                                            \
    } while (0);
#include "opt_fields.h"
#undef OPT_INT
#undef OPT_UNS
#undef OPT_LL
#undef OPT_DBL
#undef OPT_STR
#undef OPT_ADD
    if (col) {
        emitf(f, "%s  %s\n", pre, line);
    }
}

/* The keys, as pad_sdl.c reads them and pad_xone.c maps the pad. */
void port_print_keys(FILE* f) {
    const char* pre = f ? "" : "port> keys: ";
    emitf(f, "%sKeyboard (controller 1; beside a pad on port 1 too; --kbport N makes it\n", pre);
    emitf(f, "%s          controller N of its own)\n", pre);
    emitf(f, "%s  arrow keys or W A S D   control stick\n", pre);
    emitf(f, "%s  I J K L                 C stick\n", pre);
    emitf(f, "%s  T F G H                 D-pad up / left / down / right\n", pre);
    emitf(f, "%s  Z  X  C  V              A  B  X  Y\n", pre);
    emitf(f, "%s  Q  E                    L  R\n", pre);
    emitf(f, "%s  Shift or Tab            Z\n", pre);
    emitf(f, "%s  Return                  Start (the game's own pause, in a minigame)\n", pre);
    emitf(f, "%s  F5 (or F12)             screenshot -> ~/Desktop/Mario Party 4 NNNNN.png\n", pre);
    emitf(f, "%s                          (Leopard gives F12 to Dashboard unless you change it)\n", pre);
    emitf(f, "%s  Escape, Cmd-Q           quit (through the game's reset: the save is written)\n", pre);
    emitf(f, "%sXbox One pads over USB (no driver needed), then any pads SDL knows,\n", pre);
    emitf(f, "%s  controllers 1-4 in that order (M34)\n", pre);
    emitf(f, "%s  left stick / right stick   control stick / C stick\n", pre);
    emitf(f, "%s  A B X Y                    A B X Y\n", pre);
    emitf(f, "%s  LT / RT                    L / R (analogue, with the click past 80%%)\n", pre);
    emitf(f, "%s  LB                         Z\n", pre);
    emitf(f, "%s  Menu (Start)               Start\n", pre);
    emitf(f, "%s  D-pad                      D-pad\n", pre);
    emitf(f, "%s  rumble                     on, when the game asks\n", pre);
}
