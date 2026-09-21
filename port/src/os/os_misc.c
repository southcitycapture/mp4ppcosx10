/* The rest of the OS: init, time, interrupts, cache, threads, the stopwatch,
 * reset, and OSLink.
 *
 * Almost all of it collapses.  The port is cooperatively single-threaded and
 * has no caches to keep coherent, so the 284 DC/IC call sites and the 20
 * interrupt-mask sites become no-ops -- but real, callable ones, because the
 * SDK calls some of them itself.  The game creates exactly one OSThread, the
 * soft-reset watcher; it and the reset button live in
 * port/src/os/sreset_poll.c.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dolphin/types.h>
#include <dolphin/os.h>

/* __OSBusClock / __OSCoreClock are tentative definitions in every translation
 * unit that includes <dolphin/os.h> (on the console they are fixed addresses
 * in low memory).  A REL bundle that merged its own copy would read zero, and
 * five modules use OS_BUS_CLOCK, so patches.txt makes the header declarations
 * `extern` and this is the one definition. */
u32 __OSBusClock;
u32 __OSCoreClock;

void OSInit(void) {
    __OSBusClock = PORT_BUS_CLOCK;
    __OSCoreClock = PORT_CORE_CLOCK;
    /* The two addresses are printed because a fault address is otherwise a
     * riddle.  M5 left a SIGBUS at 0x04800000 in an m425dll draw hook (§15.6)
     * and the single most useful thing to know about that number is whether
     * it is the first byte past the top of MEM1 -- a read one element off the
     * end of a small allocation that happened to sit at the top of the heap
     * looks exactly like that.  With the bounds in the log it is a comparison
     * rather than an inference. */
    port_log("port> OSInit: MEM1 %u MB [%p, %p), ARAM %u MB, bus %u MHz\n",
             PORT_MEM1_SIZE >> 20, port_mem1_lo(), port_mem1_hi(), PORT_ARAM_SIZE >> 20,
             PORT_BUS_CLOCK / 1000000);
    if (port_opt.rtc_set) {
        /* Printed, because the whole point of --rtc is that a second rig can
         * be given the same number, and a log that does not say what the
         * number was cannot be compared with anything. */
        time_t t = (time_t)port_opt.rtc;
        struct tm tm;
        char when[64];
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
        port_log("port> OSInit: RTC pinned to %lld (%s), OSGetTime origin "
                 "%lld ticks%s\n",
                 (long long)port_opt.rtc, when, (long long)port_opt.seed,
                 port_opt.rtc_offset != 0.0 ? " (shifted by --rtcoffset)" : "");
        if (port_opt.rtc_offset != 0.0) {
            port_log("port> OSInit: --rtcoffset %+.3f s (%+.1f frames) -- the "
                     "origin is moved so BoardRandInit reads what the console "
                     "reads, not so the boot does\n",
                     port_opt.rtc_offset, port_opt.rtc_offset * 60.0);
        }
    }
}

/* ---- time ---------------------------------------------------------------- */
/* OSGetTick counts the console's 40.5 MHz decrementer.  OSGetTime is the same
 * counter as a 64-bit value since the RTC epoch (2000-01-01).  Under
 * --deterministic both advance by exactly one 60 Hz frame per retrace, so a
 * replay is reproducible.
 *
 * `--seed N` is that same deterministic clock started at a different reading,
 * and it is the port's whole RNG-seed story.  The game has exactly two random
 * sources and **both of them seed from `OSGetTime` and from nothing else**:
 *
 *   src/game/frand.c:13    frandom(0) -> rand8() ^ (s64)OSGetTime() ^ 0xD826BC89,
 *                          reached once from init.c:77 (`rnd_temp = frand()`)
 *   src/game/board/main.c:1432  BoardRandInit() -> boardRandSeed = OSGetTime()
 *
 * and `rand8`'s own `rnd_seed` is a literal 0x0000D9ED in main.c:134.  So
 * moving the clock's origin moves both generators, together, through the
 * game's own seed sites -- no patch to game source, no second seeding path to
 * keep in step with the first, and the two RNGs stay in the same relationship
 * to each other that they have on the console. */

#define GC_EPOCH_UNIX PORT_GC_EPOCH_UNIX

static OSTime det_ticks;
static OSTime wall_origin;

void port_time_tick(void) { det_ticks += PORT_TIMER_CLOCK / 60; }

static OSTime host_ticks(void) {
    if (port_opt.deterministic) {
        return port_opt.seed + det_ticks;
    }
    /* 40.5 MHz exactly, as ns * 81/2000 -- integer, and it does not lose the
     * half.  The obvious `us * (PORT_TIMER_CLOCK / 1000000) / 1000` is wrong
     * twice: PORT_TIMER_CLOCK / 1000000 truncates 40.5 to 40, and the trailing
     * /1000 leaves the counter running at 40 kHz, a thousand times slow.  The
     * game notices.  `bootDll` waits out the Nintendo logo with
     * `while (OSTicksToMilliseconds(OSGetTick() - t0) < 3000)`, and at 40 kHz
     * that is fifty minutes, so the boot sat on the logo forever while the
     * frame loop ran perfectly at 60 fps.  (--deterministic advances 40.5e6/60
     * per retrace and was right all along, which is why it did not show there.) */
    /* port_now_ns() counts from the port's own first reading, not from the
     * epoch (see port/src/platform/clock.c -- the alternative overflows on
     * PowerPC every nine minutes of uptime).  So the *origin* comes from the
     * host's calendar clock instead, once, which is both what the console does
     * -- OSGetTime is ticks since 2000-01-01 -- and what keeps `frandom(0)`
     * and `BoardRandInit()` seeded differently from one run to the next.
     * --deterministic replaces the whole thing with --seed. */
    if (wall_origin == 0) {
        wall_origin = (OSTime)((s64)(time(NULL) - GC_EPOCH_UNIX) *
                               (s64)PORT_TIMER_CLOCK);
    }
    return wall_origin + (OSTime)((port_now_ns() * 81ULL) / 2000ULL);
}

/* A sanity line for the one clock the game reads directly.  `bootDll` and a
 * dozen other places pace themselves with
 * `while (OSTicksToMilliseconds(OSGetTick() - t0) < N)`, so a tick rate that
 * is wrong by a factor is not a small error -- it is a hang that looks like a
 * rendering problem.  Printed at shutdown next to the frame counts. */
static OSTime clock_t0;
static double clock_w0;

void port_clock_mark(void) {
    clock_t0 = host_ticks();
    clock_w0 = port_now_seconds();
}

void port_clock_report(void) {
    double w = port_now_seconds() - clock_w0;
    OSTime d = host_ticks() - clock_t0;
    if (w <= 0.0) {
        return;
    }
    port_log("port> OS clock: %.0f ticks in %.2f s = %.3f MHz (console 40.500)%s\n",
             (double)d, w, (double)d / w / 1e6,
             ((double)d / w / 1e6) > 40.0 && ((double)d / w / 1e6) < 41.0 ? ""
                                                                         : "  *** WRONG");
}

/* ---- the spin-loop clock ---------------------------------------------------
 *
 * On the console the decrementer runs whether or not the game is doing
 * anything, so a busy-wait like
 *
 *     tickStart = OSGetTick();
 *     while ((msmMusGetNumPlay(TRUE) || msmSeGetNumPlay(TRUE)) &&
 *            OSTicksToMilliseconds(OSGetTick() - tickStart) < 500) {}
 *
 * (`SNDGRP_WAIT`, src/game/audio.c:488) always ends: either the sounds stop or
 * half a second passes.  The port's deterministic clock advances one 60 Hz
 * frame per *retrace*, and a spin loop does not reach a retrace -- so under
 * `--rtc` / `--deterministic` neither half of that condition can ever change
 * and the loop is infinite.  It had never been reached, because the other
 * half of the condition was accidentally always false: `numPlay` is only
 * recomputed in `msmSePeriodicProc`, which never ran while the AI DMA
 * callback was a stub (22.4).  Wiring that callback up made this the first
 * thing the boot hit -- 102% of a CPU in `HuAudSndCharGrpSet`, five minutes
 * in, the frame counter frozen at 840.
 *
 * So `OSGetTick` gets a virtual advance of its own: a fixed amount per call,
 * which is deterministic (it is a pure function of how many times the game
 * has asked) and is only visible to code that asks repeatedly without letting
 * a frame go by -- which is exactly the spin loops it is for.  A millisecond
 * per thousand calls makes `SNDGRP_WAIT` give up after about 30,000
 * iterations, a few milliseconds of real time, and take the game's own
 * documented timeout path.
 *
 * `OSGetTime` deliberately does *not* get it.  That is the clock both of the
 * game's RNGs seed from (see above) and the one `--rtc` pins, and it must
 * stay a function of the retrace count alone. */
#define PORT_SPIN_TICKS (PORT_TIMER_CLOCK / 60 / 1000)
static OSTime spin_ticks;

OSTime OSGetTime(void) { return host_ticks(); }

OSTick OSGetTick(void) {
    if (port_opt.deterministic) {
        spin_ticks += PORT_SPIN_TICKS;
    }
    return (OSTick)(host_ticks() + spin_ticks);
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_sec = td->sec;
    tm.tm_min = td->min;
    tm.tm_hour = td->hour;
    tm.tm_mday = td->mday;
    tm.tm_mon = td->mon;
    tm.tm_year = td->year - 1900;
    return (OSTime)(timegm(&tm) - GC_EPOCH_UNIX) * PORT_TIMER_CLOCK;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    time_t t = (time_t)(ticks / PORT_TIMER_CLOCK) + GC_EPOCH_UNIX;
    OSTime rem = ticks % PORT_TIMER_CLOCK;
    struct tm tm;
    gmtime_r(&t, &tm);
    td->sec = tm.tm_sec;
    td->min = tm.tm_min;
    td->hour = tm.tm_hour;
    td->mday = tm.tm_mday;
    td->mon = tm.tm_mon;
    td->year = tm.tm_year + 1900;
    td->wday = tm.tm_wday;
    td->yday = tm.tm_yday;
    td->msec = (int)(rem * 1000 / PORT_TIMER_CLOCK);
    td->usec = (int)(rem * 1000000 / PORT_TIMER_CLOCK) % 1000;
}

/* ---- the stopwatch (perf.c) ---------------------------------------------- */

void OSInitStopwatch(OSStopwatch* sw, char* name) {
    memset(sw, 0, sizeof(*sw));
    sw->name = name;
    sw->min = (OSTime)0x7FFFFFFFFFFFFFFFLL;
}
void OSStartStopwatch(OSStopwatch* sw) {
    sw->running = TRUE;
    sw->last = OSGetTime();
}
void OSStopStopwatch(OSStopwatch* sw) {
    OSTime d;
    if (!sw->running) {
        return;
    }
    d = OSGetTime() - sw->last;
    sw->running = FALSE;
    sw->total += d;
    sw->hits++;
    if (d < sw->min) {
        sw->min = d;
    }
    if (d > sw->max) {
        sw->max = d;
    }
}
OSTime OSCheckStopwatch(OSStopwatch* sw) {
    return sw->running ? sw->total + (OSGetTime() - sw->last) : sw->total;
}
void OSResetStopwatch(OSStopwatch* sw) {
    sw->total = 0;
    sw->hits = 0;
    sw->min = (OSTime)0x7FFFFFFFFFFFFFFFLL;
    sw->max = 0;
    sw->running = FALSE;
}
void OSDumpStopwatch(OSStopwatch* sw) {
    port_log("port> stopwatch %s: %lld ticks over %u hits\n",
             sw->name ? sw->name : "?", (long long)sw->total, sw->hits);
}

/* ---- interrupts ---------------------------------------------------------- */
/* Cooperative and single-threaded: the mask is a value the game saves and
 * restores, nothing more. */

static BOOL int_enabled = TRUE;

BOOL OSDisableInterrupts(void) {
    BOOL old = int_enabled;
    int_enabled = FALSE;
    return old;
}
BOOL OSEnableInterrupts(void) {
    BOOL old = int_enabled;
    int_enabled = TRUE;
    return old;
}
BOOL OSRestoreInterrupts(BOOL level) {
    BOOL old = int_enabled;
    int_enabled = level;
    return old;
}

/* ---- caches -------------------------------------------------------------- */
/* 284 call sites, all no-ops: there is no incoherent DMA engine behind us. */

void DCInvalidateRange(void* addr, u32 n) { (void)addr; (void)n; }
void DCFlushRange(void* addr, u32 n) { (void)addr; (void)n; }
void DCStoreRange(void* addr, u32 n) { (void)addr; (void)n; }
void DCFlushRangeNoSync(void* addr, u32 n) { (void)addr; (void)n; }
void DCStoreRangeNoSync(void* addr, u32 n) { (void)addr; (void)n; }
void DCZeroRange(void* addr, u32 n) { memset(addr, 0, n); }
void ICInvalidateRange(void* addr, u32 n) { (void)addr; (void)n; }
void LCEnable(void) {}
void LCDisable(void) {}
void PPCSync(void) {}
void PPCHalt(void) { port_fatal("PPCHalt"); }

/* ---- threads ------------------------------------------------------------- */
/* The game creates one OSThread, the soft-reset watcher, and the port polls
 * its body once per retrace instead of running it on a thread.  That whole
 * surface -- OSCreateThread, OSResumeThread, OSSleepThread, OSWakeupThread,
 * OSInitMessageQueue and the reset button -- lives in
 * port/src/os/sreset_poll.c. */

/* ---- reset, sound and video mode ----------------------------------------- */

static u32 sound_mode = 1; /* stereo */
static u32 progressive;

u32 OSGetSoundMode(void) { return sound_mode; }
void OSSetSoundMode(u32 mode) { sound_mode = mode; }
u32 OSGetProgressiveMode(void) { return progressive; }
void OSSetProgressiveMode(u32 on) { progressive = on; }

/* ---- OSLink -------------------------------------------------------------- */
/* Nothing calls these any more: objdll.c's three OSLink/OSUnlink sites are the
 * seam the bundle loader replaced (port/src/os/dll_load.c), and no other
 * caller exists in the game.  They stay defined, and loud, because a REL
 * bundle could import them and because a future single-binary build mode
 * (PLAN.md §2.4a) would route through here. */

BOOL OSLink(OSModuleInfo* newModule, void* bss) {
    (void)bss;
    port_log("port> OSLink(%p) called directly: the port loads RELs as Mach-O "
             "bundles through omDLLLink, not by relocating the .rel\n",
             (void*)newModule);
    return FALSE;
}

BOOL OSLinkFixed(OSModuleInfo* newModule, void* bss) { return OSLink(newModule, bss); }

BOOL OSUnlink(OSModuleInfo* oldModule) {
    (void)oldModule;
    return TRUE;
}

/* ---- snapshots ------------------------------------------------------------
 * The deterministic clock *is* game state: `OSGetTime` seeds both of the RNGs
 * and `OSGetTick` paces a dozen wait loops, and both are pure functions of
 * these counters under --rtc/--deterministic.  A restored run that started
 * them from zero would seed differently and deal a different minigame, which
 * is exactly the divergence the milestone is meant to rule out. */
void os_misc_snap_register(void) {
    port_snap_register("os.det_ticks", &det_ticks, sizeof(det_ticks));
    port_snap_register("os.wall_origin", &wall_origin, sizeof(wall_origin));
    port_snap_register("os.spin_ticks", &spin_ticks, sizeof(spin_ticks));
    port_snap_register("os.clock_t0", &clock_t0, sizeof(clock_t0));
    port_snap_register("os.int_enabled", &int_enabled, sizeof(int_enabled));
    port_snap_register("os.sound_mode", &sound_mode, sizeof(sound_mode));
    port_snap_register("os.progressive", &progressive, sizeof(progressive));
}

/* M29: the game's process.c multiplies every coroutine stack by this (the
 * patch in port/patches.txt); PORT_PRC_STACK_MUL (4) unless --stackmul */
int port_prc_stack_mul(void) {
    return port_opt.stackmul > 0 ? port_opt.stackmul : PORT_PRC_STACK_MUL;
}
