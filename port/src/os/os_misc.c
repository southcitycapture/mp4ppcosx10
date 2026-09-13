/* The rest of the OS: init, time, interrupts, cache, threads, the stopwatch,
 * reset, and OSLink.
 *
 * Almost all of it collapses.  The port is cooperatively single-threaded and
 * has no caches to keep coherent, so the 284 DC/IC call sites and the 20
 * interrupt-mask sites become no-ops -- but real, callable ones, because the
 * SDK calls some of them itself.  The game creates exactly one OSThread, the
 * soft-reset watcher, and its very first act is to block on OSSleepThread, so
 * never starting it is behaviour-preserving for as long as nothing posts to
 * its queue.
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
 * in low memory).  The port compiles game code with -fcommon so they merge,
 * and fills them in here. */
extern u32 __OSBusClock;
extern u32 __OSCoreClock;

void OSInit(void) {
    __OSBusClock = PORT_BUS_CLOCK;
    __OSCoreClock = PORT_CORE_CLOCK;
    port_log("port> OSInit: MEM1 %u MB, ARAM %u MB, bus %u MHz\n",
             PORT_MEM1_SIZE >> 20, PORT_ARAM_SIZE >> 20, PORT_BUS_CLOCK / 1000000);
}

/* ---- time ---------------------------------------------------------------- */
/* OSGetTick counts the console's 40.5 MHz decrementer.  OSGetTime is the same
 * counter as a 64-bit value since the RTC epoch (2000-01-01).  Under
 * --deterministic both advance by exactly one 60 Hz frame per retrace, so a
 * replay is reproducible. */

#define GC_EPOCH_UNIX 946684800LL /* 2000-01-01T00:00:00Z */

static OSTime det_ticks;

void port_time_tick(void) { det_ticks += PORT_TIMER_CLOCK / 60; }

static OSTime host_ticks(void) {
    if (port_opt.deterministic) {
        return det_ticks;
    }
    return (OSTime)(port_now_ns() / 1000ULL) * (PORT_TIMER_CLOCK / 1000000) / 1000;
}

OSTime OSGetTime(void) { return host_ticks(); }
OSTick OSGetTick(void) { return (OSTick)host_ticks(); }

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
/* One thread in the whole game (sreset.c's reset watcher), and it blocks on
 * OSSleepThread before doing anything.  Recording it is enough for M1; M2's
 * host loop will poll it. */

static OSThread* reset_thread;

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack,
                    u32 stackSize, OSPriority prio, u16 attr) {
    (void)param;
    (void)stack;
    (void)stackSize;
    (void)prio;
    (void)attr;
    memset(thread, 0, sizeof(*thread));
    reset_thread = thread;
    port_log("port> OSCreateThread %p func %p (the soft-reset watcher; not started)\n",
             (void*)thread, (void*)func);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) { (void)thread; return 0; }
void OSCancelThread(OSThread* thread) { (void)thread; }
void OSYieldThread(void) {}
void OSSleepThread(OSThreadQueue* queue) { (void)queue; }
void OSWakeupThread(OSThreadQueue* queue) { (void)queue; }
OSThread* OSSetIdleFunction(OSIdleFunction f, void* param, void* stack, u32 size) {
    (void)f;
    (void)param;
    (void)stack;
    (void)size;
    return NULL;
}

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    memset(mq, 0, sizeof(*mq));
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}

/* ---- reset, sound and video mode ----------------------------------------- */

static u32 sound_mode = 1; /* stereo */
static u32 progressive;

u32 OSGetSoundMode(void) { return sound_mode; }
void OSSetSoundMode(u32 mode) { sound_mode = mode; }
u32 OSGetProgressiveMode(void) { return progressive; }
void OSSetProgressiveMode(u32 on) { progressive = on; }
BOOL OSGetResetButtonState(void) { return FALSE; }
u32 OSGetResetCode(void) { return 0; }

void OSResetSystem(BOOL reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)resetCode;
    (void)forceMenu;
    port_log("port> OSResetSystem: quitting\n");
    port_stub_report();
    exit(0);
}

/* ---- OSLink -------------------------------------------------------------- */
/* M1 stops here on purpose.  The real loader is one dlopen'ed Mach-O bundle
 * per REL, which maps 1:1 onto objdll.c's five-point contract; that is M2.
 * Returning FALSE makes objdll.c report the failure through its own OSReport
 * and carry on as far as it can. */

BOOL OSLink(OSModuleInfo* newModule, void* bss) {
    (void)bss;
    port_log("port> OSLink(%p): REL relocation is M2 (one dlopen'ed bundle per "
             "module); returning FALSE\n", (void*)newModule);
    return FALSE;
}

BOOL OSLinkFixed(OSModuleInfo* newModule, void* bss) { return OSLink(newModule, bss); }

BOOL OSUnlink(OSModuleInfo* oldModule) {
    (void)oldModule;
    return TRUE;
}
