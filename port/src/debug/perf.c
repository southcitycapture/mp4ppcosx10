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

static double t_gx, t_present, t_audio, t_frame_start, t_sleep;
static double win_audio, win_t0;
static int win_frames;
static double gx_open, present_open, audio_open;
static int gx_depth;

/* per-frame samples, so the report can give a median and a worst case rather
 * than only a mean -- a port's stutter lives in the tail. */
#define PERF_MAX 20000
static float s_frame[PERF_MAX], s_gx[PERF_MAX], s_present[PERF_MAX], s_game[PERF_MAX];
static float s_audio[PERF_MAX];
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

/* The MusyX mix, from salCtrlDsp.  It is called about 3.34 times a frame (one
 * 160-sample DSP frame per 5 ms of audio, against a 16.68 ms video frame), so
 * this accumulates several regions per frame rather than bracketing one.
 * It is on the game thread on purpose -- see the header of
 * port/src/audio/audio_out_sdl.c -- which is exactly why it has to be counted
 * here and subtracted from `game` rather than left invisible. */
void port_perf_audio_begin(void) {
    if (port_opt.perf || port_opt.status) {
        audio_open = port_now_seconds();
    }
}

void port_perf_audio_end(void) {
    if (port_opt.perf || port_opt.status) {
        double d = port_now_seconds() - audio_open;
        t_audio += d;
        win_audio += d;
    }
}

/* A one-second rolling window, for --status.  --perf keeps whole-run
 * distributions and prints them once at the end; a soak that runs all night
 * needs the same two numbers *now*, so they are accumulated separately and
 * consumed by the reader rather than by the reporter. */
void port_perf_window(double* fps, double* aud_ms) {
    double now = port_now_seconds();
    double dt;
    if (win_t0 == 0.0) {
        win_t0 = now;
    }
    dt = now - win_t0;
    *fps = dt > 0.0 ? (double)win_frames / dt : 0.0;
    *aud_ms = win_frames ? win_audio * 1000.0 / (double)win_frames : 0.0;
    win_audio = 0.0;
    win_frames = 0;
    win_t0 = now;
}

void port_perf_slept(double seconds) {
    if (port_opt.perf) {
        t_sleep += seconds;
    }
}

/* Called from the retrace gate, once per frame, after the sleep. */
void port_perf_frame(void) {
    double now, wall, game;
    win_frames++;
    if (!port_opt.perf) {
        return;
    }
    now = port_now_seconds();
    if (t_frame_start == 0.0) {
        t_frame_start = now;
        t_first = now;
        t_gx = t_present = t_audio = t_sleep = 0.0;
        return;
    }
    wall = now - t_frame_start - t_sleep;
    game = wall - t_gx - t_present - t_audio;
    if (game < 0.0) {
        game = 0.0;
    }
    if (n_samples < PERF_MAX) {
        s_frame[n_samples] = (float)(wall * 1000.0);
        s_gx[n_samples] = (float)(t_gx * 1000.0);
        s_present[n_samples] = (float)(t_present * 1000.0);
        s_audio[n_samples] = (float)(t_audio * 1000.0);
        s_game[n_samples] = (float)(game * 1000.0);
        n_samples++;
    }
    t_frame_start = now;
    t_gx = t_present = t_audio = t_sleep = 0.0;
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

/* --perfwin A-B[:NAME][,...]: fps and the frame's split over a frame range,
 * out of the samples this run already collected.
 *
 * A port is judged scene by scene -- the title is not the board and neither is
 * the character select -- and M5 had to measure that by running the same walk
 * three times to three different `--frames N` and subtracting, which costs
 * three boots and about twenty minutes for three numbers.  The samples were
 * there the whole time; only the report was missing.  Frame numbers are the
 * sample index, which is the retrace count the gate has passed, so they are
 * the same numbers `--ovllog` and `--dumpframe` print.
 *
 * `sum(frame)` is work with the pacing sleep already subtracted, so under
 * `--turbo` -- which is how every measurement in PLAN.md 21 is taken -- the
 * window's fps is what the machine can actually do on that scene. */
static void perf_windows(void) {
    const char* p = port_opt.perfwin;
    if (!p || !*p) {
        return;
    }
    port_log("  ---- --perfwin ----\n");
    while (*p) {
        char name[32];
        long a = 0, b = 0;
        int i, n = 0;
        double sum = 0.0, gx = 0.0, pres = 0.0, game = 0.0, aud = 0.0;
        char* e;
        a = strtol(p, &e, 10);
        if (e == p) {
            break;
        }
        p = e;
        if (*p == '-') {
            b = strtol(p + 1, &e, 10);
            p = e;
        } else {
            b = a;
        }
        name[0] = 0;
        if (*p == ':') {
            int k = 0;
            p++;
            while (*p && *p != ',' && k < (int)sizeof(name) - 1) {
                name[k++] = *p++;
            }
            name[k] = 0;
        }
        for (i = (int)a; i <= (int)b && i < n_samples; i++) {
            if (i < 0) {
                continue;
            }
            sum += s_frame[i];
            gx += s_gx[i];
            pres += s_present[i];
            game += s_game[i];
            aud += s_audio[i];
            n++;
        }
        if (n > 0 && sum > 0.0) {
            port_log("  %-14s frames %ld-%ld (%d)  %6.2f ms/frame  %5.2f fps  "
                     "[game %5.2f gx %5.2f present %5.2f aud %5.2f]\n",
                     name[0] ? name : "window", a, b, n, sum / n, n * 1000.0 / sum,
                     game / n, gx / n, pres / n, aud / n);
        } else {
            port_log("  %-14s frames %ld-%ld: no samples\n", name[0] ? name : "window", a,
                     b);
        }
        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }
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
    line("aud", s_audio, n_samples);
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
    perf_windows();
}
