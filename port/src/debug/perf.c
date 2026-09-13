/* --perf: where a frame goes on the G4.
 *
 * The Snowboard Kids ports learned this the hard way (PLAN.md §2.5): an
 * idle-gated retrace *hides overruns*.  The game asks for a retrace, the port
 * hands it one, and the game clock reads a serene 60 fps while the wall clock
 * falls further behind every frame.  So every number here is reported against
 * **both** clocks, and the headline is the ratio between them.
 *
 * A frame is split three ways, measured at the only three places that can
 * honestly be told apart:
 *
 *   gx       inside GXBegin..GXEnd and GXCallDisplayList -- vertex decode,
 *            CPU transform and lighting, texture decode, the TEV compile and
 *            the glDrawArrays.  Everything the GL backend costs.
 *   present  inside gl13_present() -- the buffer swap, and with it whatever
 *            the driver had queued and deferred until someone asked.
 *   game     the remainder of the gate-to-gate interval: the engine, the
 *            HUPROCESS coroutines, DVD reads, ARAM traffic, msm.
 *
 * The pacing sleep is subtracted, so `game+gx+present` is work, not waiting,
 * and `wall` is what the frame actually took.  Timing calls are two
 * mach_absolute_time()s per region and the whole thing is behind
 * port_opt.perf, so an un-instrumented run pays one predictable branch.
 */
#include "port.h"

#include <stdlib.h>
#include <string.h>

static double t_gx, t_present, t_frame_start, t_sleep;
static double gx_open, present_open;
static int gx_depth;

/* per-frame samples, so the report can give a median and a worst case rather
 * than only a mean -- a port's stutter lives in the tail. */
#define PERF_MAX 20000
static float s_frame[PERF_MAX], s_gx[PERF_MAX], s_present[PERF_MAX], s_game[PERF_MAX];
static int n_samples;
static double t_first;

void port_perf_gx_begin(void) {
    if (!port_opt.perf) {
        return;
    }
    if (gx_depth++ == 0) {
        gx_open = port_now_seconds();
    }
}

void port_perf_gx_end(void) {
    if (!port_opt.perf) {
        return;
    }
    if (--gx_depth == 0) {
        t_gx += port_now_seconds() - gx_open;
    }
    if (gx_depth < 0) {
        gx_depth = 0;
    }
}

void port_perf_present_begin(void) {
    if (port_opt.perf) {
        present_open = port_now_seconds();
    }
}

void port_perf_present_end(void) {
    if (port_opt.perf) {
        t_present += port_now_seconds() - present_open;
    }
}

void port_perf_slept(double seconds) {
    if (port_opt.perf) {
        t_sleep += seconds;
    }
}

/* Called from the retrace gate, once per frame, after the sleep. */
void port_perf_frame(void) {
    double now, wall, game;
    if (!port_opt.perf) {
        return;
    }
    now = port_now_seconds();
    if (t_frame_start == 0.0) {
        t_frame_start = now;
        t_first = now;
        t_gx = t_present = t_sleep = 0.0;
        return;
    }
    wall = now - t_frame_start - t_sleep;
    game = wall - t_gx - t_present;
    if (game < 0.0) {
        game = 0.0;
    }
    if (n_samples < PERF_MAX) {
        s_frame[n_samples] = (float)(wall * 1000.0);
        s_gx[n_samples] = (float)(t_gx * 1000.0);
        s_present[n_samples] = (float)(t_present * 1000.0);
        s_game[n_samples] = (float)(game * 1000.0);
        n_samples++;
    }
    t_frame_start = now;
    t_gx = t_present = t_sleep = 0.0;
}

static int cmpf(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void line(const char* name, float* v, int n) {
    double sum = 0.0;
    float* c = (float*)malloc((size_t)n * sizeof(float));
    int i;
    if (!c) {
        return;
    }
    memcpy(c, v, (size_t)n * sizeof(float));
    for (i = 0; i < n; i++) {
        sum += c[i];
    }
    qsort(c, (size_t)n, sizeof(float), cmpf);
    port_log("  %-8s mean %6.2f  median %6.2f  p95 %6.2f  worst %6.2f ms\n", name,
             sum / n, c[n / 2], c[(int)(n * 0.95)], c[n - 1]);
    free(c);
}

void port_perf_report(void) {
    double wall, gameclock;
    int i, over = 0;
    if (!port_opt.perf || n_samples == 0) {
        return;
    }
    wall = port_now_seconds() - t_first;
    gameclock = n_samples / 59.94;
    for (i = 0; i < n_samples; i++) {
        if (s_frame[i] > 1000.0f / 59.94f) {
            over++;
        }
    }
    port_log("\n---- --perf: %d frames ----\n", n_samples);
    line("game", s_game, n_samples);
    line("gx", s_gx, n_samples);
    line("present", s_present, n_samples);
    line("frame", s_frame, n_samples);
    port_log("  budget   16.68 ms/frame at 59.94 Hz; %d of %d frames over it (%.1f%%)\n",
             over, n_samples, 100.0 * over / n_samples);
    /* The number that cannot lie: what the game thinks elapsed against what
     * actually elapsed.  A ratio above 1.0 is an overrun the retrace gate hid. */
    port_log("  clocks   game %.2f s vs wall %.2f s -- ratio %.3f%s\n", gameclock, wall,
             wall / gameclock,
             wall / gameclock > 1.02 ? "  (WALL CLOCK IS BEHIND: the gate is hiding an overrun)"
                                     : "  (keeping up)");
    port_log("  fps      %.1f effective\n", n_samples / wall);
}
