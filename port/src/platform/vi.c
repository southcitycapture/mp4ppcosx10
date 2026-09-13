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

    if (port_opt.max_frames && (int)retrace_count >= port_opt.max_frames) {
        double wall = now_seconds() - first_retrace_at;
        port_log("\nport> --frames %d reached: %u retraces in %.2f s wall "
                 "(game clock %.2f s)\n",
                 port_opt.max_frames, retrace_count, wall, retrace_count / 59.94);
        port_shutdown(0);
    }
}
