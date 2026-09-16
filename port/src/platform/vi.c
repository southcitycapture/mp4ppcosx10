/* VI, and with it the whole host loop.
 *
 * The game blocks in `VIWaitForRetrace()` -- twice per frame, from
 * `HuSysDoneRender`'s pacing loop and from `SwapBuffers` -- and that is the
 * port's idle gate.  Everything the console delivered by interrupt (pad
 * sampling, DVD completion, ARAM DMA completion, audio DMA, the reset
 * watcher) is delivered here instead, once per retrace, at a point the game
 * itself chose.  Given the same input stream the game therefore sees the same
 * events at the same points in its own logic, which is what makes replay
 * reproducible.
 *
 * The Snowboard Kids lesson applies: an idle-gated retrace hides overruns, so
 * the port must always be able to print the game clock and the wall clock side
 * by side.  --frames N is M1's version of that.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <dolphin/types.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

/* The SDK's standard render modes.  The game names four of them; the values
 * are the SDK's own published constants. */
#define VFILTER_DF { 8, 8, 10, 12, 10, 8, 8 }
#define VFILTER_SF { 0, 0, 21, 22, 21, 0, 0 }
#define SP_POINT                                                                         \
    { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }

GXRenderModeObj GXNtsc480IntDf = { 0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
                                   SP_POINT, VFILTER_DF };
GXRenderModeObj GXNtsc480Prog = { 2, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0,
                                  SP_POINT, VFILTER_SF };
GXRenderModeObj GXMpal480IntDf = { 8, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
                                   SP_POINT, VFILTER_DF };
GXRenderModeObj GXPal528IntDf = { 4, 640, 528, 528, 40, 23, 640, 528, 1, 0, 0,
                                  SP_POINT, VFILTER_DF };

static u32 retrace_count;
static u32 field;
static void* next_fb;
static void* current_fb;
static int swap_pending;
static int black = 1;
static VIRetraceCallback pre_cb;
static VIRetraceCallback post_cb;
static const GXRenderModeObj* mode;

void port_time_tick(void);

void port_vi_init(void) {
    retrace_count = 0;
    field = 0;
}

/* What a snapshot has to carry out of this file (PLAN.md §24.2).  The retrace
 * count is the game's clock -- `VIGetRetraceCount` is read all over the game
 * and the deterministic clock is a function of it -- and the two framebuffer
 * pointers and the callbacks point into MEM1 and into the game's own text, so
 * they are the same addresses in the restored process.  `next_retrace_at` and
 * `first_retrace_at` are wall-clock pacing and are deliberately *not* carried:
 * a restored run starts pacing from now. */
void port_vi_snap_register(void) {
    port_snap_register("vi.retrace_count", &retrace_count, sizeof(retrace_count));
    port_snap_register("vi.field", &field, sizeof(field));
    port_snap_register("vi.next_fb", &next_fb, sizeof(next_fb));
    port_snap_register("vi.current_fb", &current_fb, sizeof(current_fb));
    port_snap_register("vi.swap_pending", &swap_pending, sizeof(swap_pending));
    port_snap_register("vi.black", &black, sizeof(black));
    port_snap_register("vi.pre_cb", &pre_cb, sizeof(pre_cb));
    port_snap_register("vi.post_cb", &post_cb, sizeof(post_cb));
    port_snap_register("vi.mode", &mode, sizeof(mode));
}

void VIInit(void) { port_log("port> VIInit\n"); }

void VIConfigure(const GXRenderModeObj* rm) {
    mode = rm;
    port_log("port> VIConfigure: %ux%u xfb, %ux%u efb, tv mode %d\n", rm->fbWidth,
             rm->xfbHeight, rm->fbWidth, rm->efbHeight, (int)rm->viTVmode);
}

void VIConfigurePan(u16 x, u16 y, u16 w, u16 h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void VIFlush(void) {
    current_fb = next_fb;
    swap_pending = 1;
}

void VISetNextFrameBuffer(void* fb) { next_fb = fb; }
void VISetBlack(BOOL b) { black = b; }
u32 VIGetTvFormat(void) { return 0; } /* VI_NTSC */
u32 VIGetDTVStatus(void) { return 0; }
u32 VIGetNextField(void) { return field; }
u32 VIGetRetraceCount(void) { return retrace_count; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = pre_cb;
    pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = post_cb;
    post_cb = cb;
    return old;
}

void __VIGetCurrentPosition(s16* x, s16* y) {
    *x = 0;
    *y = 0;
}

/* ---- the gate ------------------------------------------------------------ */

static double now_seconds(void) { return port_now_seconds(); }

static double next_retrace_at;
static double first_retrace_at;

void VIWaitForRetrace(void) {
    /* Before the present, because it decides whether the *next* frame is
     * drawn: --ffto's switch, and the snapshot ring's safe point (the top of
     * the retrace is the one moment in the frame at which no GX call and no
     * audio call is in progress).  See PLAN.md §24. */
    port_ffto_tick();
    port_snap_tick();
    if (swap_pending) {
        port_perf_present_begin();
        port_gx_present();
        port_perf_present_end();
        swap_pending = 0;
    }
    if (pre_cb) {
        pre_cb(retrace_count);
    }

    port_dvd_service();
    port_arq_service();
    /* The AI interrupt, simulated.  On the console this was the audio
     * hardware's own 200 Hz interrupt; here it is an exact integer number of
     * 160-sample frames per retrace, spent before the game runs, so MusyX's
     * sequencer advances by a fixed function of the frame number and not by
     * the wall clock.  See port/src/audio/musyx_sal.c. */
    port_audio_tick();
    port_reset_thread_tick();

    {
        double now = now_seconds();
        if (first_retrace_at == 0.0) {
            first_retrace_at = now;
            next_retrace_at = now;
        }
        next_retrace_at += 1.0 / 59.94;
        if (next_retrace_at < now - 0.25) {
            next_retrace_at = now; /* resync rather than spin through a backlog */
        } else if (!port_opt.turbo && !port_opt.deterministic && next_retrace_at > now) {
            struct timespec req;
            double d = next_retrace_at - now;
            req.tv_sec = (time_t)d;
            req.tv_nsec = (long)((d - (double)req.tv_sec) * 1e9);
            nanosleep(&req, NULL);
            port_perf_slept(d);
        }
    }
    port_time_tick();
    port_perf_frame();

    retrace_count++;
    field ^= 1;
    if (post_cb) {
        post_cb(retrace_count);
    }
    /* The self-play harness parks game state here, after the game's own
     * PadReadVSync post-callback, so that what it writes is the last word on
     * the frame the game is about to run. */
    port_selfplay_tick(retrace_count);

    if (port_opt.max_frames && (int)retrace_count >= port_opt.max_frames) {
        double wall = now_seconds() - first_retrace_at;
        port_log("\nport> --frames %d reached: %u retraces in %.2f s wall "
                 "(game clock %.2f s)\n",
                 port_opt.max_frames, retrace_count, wall, retrace_count / 59.94);
        port_shutdown(0);
    }
}
