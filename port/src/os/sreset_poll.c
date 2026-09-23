/* The one OSThread in the whole game, polled once per retrace.
 *
 * `src/game/sreset.c` creates exactly one thread -- `ToeThreadFunc`, the
 * power/reset button and disc-error watcher -- and its shape is
 *
 *     while (1) { OSSleepThread(&ToeMessageQueue.queueSend); <body> }
 *
 * woken once per field by `HuDvdErrDispIntFunc`, which the game installs as
 * VI's pre-retrace callback.  The port has no threads (PLAN.md §2.3: "run
 * ToeThreadFunc as a callback once per frame"), and the obvious way to run a
 * `while (1)` as a callback is to give it its own stack and a context switch.
 * It does not need one.
 *
 * The loop body carries no state from one iteration to the next -- its only
 * local, `hide_disp`, is assigned before it is read -- so re-entering the
 * function from the top once per retrace is indistinguishable from letting it
 * come round the loop.  All the port has to do is make the *first*
 * `OSSleepThread` of each tick return normally, so the body runs, and the
 * *second* one (the loop coming back around) jump out.  One `setjmp` per
 * retrace, no second stack, no ABI surface, and nothing that behaves
 * differently on PowerPC than on the host.
 *
 * The reset button itself: `--reset` from a hotkey, or SIGINT, sets the same
 * flag `OSGetResetButtonState` reports, so the quit goes through the game's
 * own reset path -- `H_ResetReady`, `HuSoftResetPostProc`, `HuRestartSystem`,
 * `OSResetSystem` -- and the port exits from there, having let the game save,
 * fade and tear down in the order it expects, rather than being killed.
 */
#include "port.h"

#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <dolphin/types.h>
#include <dolphin/os.h>

typedef void* (*OSThreadFunc)(void*);

static OSThread* toe_thread;
static OSThreadFunc toe_func;
static int toe_resumed;
static int toe_awake;   /* OSWakeupThread was called since the last tick */
static int in_tick;     /* we are inside the polled body                 */
static int sleep_depth; /* OSSleepThread calls so far this tick          */
static jmp_buf tick_out;
static int reset_requested;
static unsigned long ticks;

/* ---- the OSThread surface the game uses ---------------------------------- */

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack,
                    u32 stackSize, OSPriority prio, u16 attr) {
    (void)param;
    (void)stack;
    (void)stackSize;
    (void)prio;
    (void)attr;
    memset(thread, 0, sizeof(*thread));
    toe_thread = thread;
    toe_func = func;
    port_log("port> OSCreateThread %p func %p (the soft-reset watcher; polled "
             "once per retrace, not run on a thread)\n",
             (void*)thread, (void*)func);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
    if (thread == toe_thread) {
        toe_resumed = 1;
    }
    return 0;
}

/* M38: the idle function's stand-in thread (below) */
static OSThread idle_thread;
static OSIdleFunction idle_fn;
static void* idle_param;

void OSCancelThread(OSThread* thread) {
    if (thread == toe_thread) {
        toe_resumed = 0;
    }
    if (thread == &idle_thread && idle_fn) {
        port_log("port> OSCancelThread: the idle function %p is cancelled\n", (void*)idle_fn);
        idle_fn = NULL;
    }
}

void OSYieldThread(void) {}

void OSSleepThread(OSThreadQueue* queue) {
    (void)queue;
    if (!in_tick) {
        return; /* nothing else in this game ever sleeps */
    }
    if (sleep_depth++ == 0) {
        return; /* the wake this tick delivers: fall through into the body */
    }
    longjmp(tick_out, 1); /* the loop came round again; that is the tick over */
}

void OSWakeupThread(OSThreadQueue* queue) {
    (void)queue;
    toe_awake = 1;
}

/* M38 (PLAN.md 53.3): the idle function, polled the way the soft-reset
 * watcher is.
 *
 * The one caller is the movie player: `THPTestProc` (src/game/thpmain.c)
 * makes `THPDecodeFunc` the idle function, and its body is
 *
 *     while (1) { if (THPStat == 2) break;
 *                 if (THPSimpleDecode() == 1) OSReport(...);
 *                 VIWaitForRetrace(); }
 *
 * On the console the idle thread runs while every other thread waits -- in
 * this game, while the game thread waits for the retrace -- so it gets one
 * pass of that loop per retrace, after the frame's work.  The body keeps no
 * state across its own `VIWaitForRetrace`, so one pass is exactly one call
 * from the top: `port_idle_tick` calls the function under a setjmp at the
 * top of the port's VIWaitForRetrace, and the function's own call to
 * VIWaitForRetrace (`port_idle_trap`) jumps back out -- the retrace is the
 * game thread's, not the idle thread's.  A function that returns is over.
 * The stack the game offers is not used: the pass runs on the game thread's,
 * a few hundred bytes deep. */
static int in_idle;
static jmp_buf idle_out;
static unsigned long idle_passes;

OSThread* OSSetIdleFunction(OSIdleFunction f, void* param, void* stack, u32 size) {
    (void)stack;
    (void)size;
    idle_fn = f;
    idle_param = param;
    port_log("port> OSSetIdleFunction %p (one pass per retrace, on the game thread)\n",
             (void*)f);
    return f ? &idle_thread : NULL;
}

void port_idle_tick(void) {
    if (!idle_fn || in_idle) {
        return;
    }
    in_idle = 1;
    idle_passes++;
    if (setjmp(idle_out) == 0) {
        idle_fn(idle_param);
        port_log("port> the idle function %p returned after %lu passes\n", (void*)idle_fn,
                 idle_passes);
        idle_fn = NULL;
    }
    in_idle = 0;
}

void port_idle_trap(void) {
    if (in_idle) {
        longjmp(idle_out, 1);
    }
}

void port_idle_snap_register(void) {
    port_snap_register("os.idle_fn", &idle_fn, sizeof(idle_fn));
    port_snap_register("os.idle_param", &idle_param, sizeof(idle_param));
}

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    memset(mq, 0, sizeof(*mq));
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}

/* ---- the poll ------------------------------------------------------------ */

void port_reset_thread_tick(void) {
    if (!toe_resumed || !toe_func || !toe_awake) {
        return;
    }
    toe_awake = 0;
    sleep_depth = 0;
    in_tick = 1;
    ticks++;
    if (setjmp(tick_out) == 0) {
        toe_func(NULL);
        /* ToeThreadFunc is `while (1)`; it cannot get here. */
    }
    in_tick = 0;
}

/* ---- the reset button ---------------------------------------------------- */

/* The game's reset path wants a press *and* a release: `ToeThreadFunc` sets
 * `H_ResetReady` when it first sees the button down, and only acts when it
 * then sees it up.  So a request is a pulse of a few polls, not a level. */
static int reset_hold;
/* M40: the SIGINT handler only sets flags -- port_log takes the log ring's
 * lock, which the interrupted thread may hold -- and the game thread's next
 * poll says what happened */
static volatile int sigint_note;

BOOL OSGetResetButtonState(void) {
    if (sigint_note) {
        sigint_note = 0;
        port_log("port> reset requested: handing it to the game's own reset path "
                 "(press ctrl-C again, or --watchdog, to leave without it)\n");
    }
    if (reset_hold > 0) {
        reset_hold--;
        return TRUE;
    }
    return FALSE;
}
u32 OSGetResetCode(void) { return 0; }

int port_reset_requested(void) { return reset_requested; }

void port_request_reset(void) {
    if (!reset_requested) {
        port_log("port> reset requested: handing it to the game's own reset path "
                 "(press ctrl-C again, or --watchdog, to leave without it)\n");
    }
    reset_requested = 1;
    reset_hold = 3;
}

static void on_sigint(int sig) {
    (void)sig;
    if (reset_requested) {
        port_log_sync(); /* M40: the ring out first (a try-lock: never waits) */
        _exit(130); /* asked twice: the game is not getting there, leave now */
    }
    if (!reset_requested) {
        sigint_note = 1;
    }
    reset_requested = 1;
    reset_hold = 3;
}

void port_reset_init(void) { signal(SIGINT, on_sigint); }

void port_reset_report(void) {
    if (!toe_func) {
        return;
    }
    port_log("port> soft-reset watcher: %s, body polled %lu times%s\n",
             toe_resumed ? "running" : "created but never resumed", ticks,
             reset_requested ? ", a reset was requested" : "");
}

void OSResetSystem(BOOL reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)resetCode;
    (void)forceMenu;
    port_log("port> OSResetSystem: the game finished its reset path after %lu "
             "watcher ticks; quitting cleanly\n", ticks);
    port_shutdown(0);
}

/* ---- snapshots ------------------------------------------------------------
 * The soft-reset watcher is the game's one OSThread, polled here instead of
 * run (PLAN.md §10.4).  Its state is which thread the game created, whether it
 * has been resumed and whether it is waiting -- all of it decides whether the
 * game's own watcher body runs on the next retrace, so it belongs to the run.
 * `tick_out` is a jmp_buf into *this* tick's frame and is deliberately not
 * carried: a snapshot is only ever taken at the top of a retrace, outside the
 * polled body. */
void port_sreset_snap_register(void) {
    port_snap_register("sreset.toe_thread", &toe_thread, sizeof(toe_thread));
    port_snap_register("sreset.toe_func", &toe_func, sizeof(toe_func));
    port_snap_register("sreset.toe_resumed", &toe_resumed, sizeof(toe_resumed));
    port_snap_register("sreset.toe_awake", &toe_awake, sizeof(toe_awake));
    port_snap_register("sreset.ticks", &ticks, sizeof(ticks));
    port_snap_register("sreset.reset_hold", &reset_hold, sizeof(reset_hold));
}
