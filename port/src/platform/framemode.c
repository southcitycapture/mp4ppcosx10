/* --realtime: the game at console speed, the picture at what the card can do.
 *
 * Until M17 the retrace gate waited for the renderer: the game asked for a
 * retrace, the port drew the frame it had just been handed, presented it, and
 * only then let the game build the next one.  At 23 fps on the board that is
 * also 39% game speed -- the animations, the music tempo against the clock,
 * the COM's thinking time, all of it stretched by the same factor -- and the
 * --perf report has been saying so since M5 ("WALL CLOCK IS BEHIND: the gate
 * is hiding an overrun").
 *
 * This file makes the two clocks separate things:
 *
 *   - the game's retrace runs at 60 Hz on the WALL clock.  Each call to
 *     VIWaitForRetrace is still exactly 1/59.94 s of game time (the
 *     deterministic clock under --rtc is still a function of the retrace
 *     count, the pad is still sampled once per retrace, the audio mix is
 *     still 3.34 DSP frames per retrace), so a --play script or a --seed run
 *     sees the same inputs at the same retrace numbers as before;
 *
 *   - the renderer presents when it can.  Before the game builds a frame the
 *     gate decides whether that frame will be *drawn* or merely *consumed*
 *     (the GX interpreter in its --nodraw shape, which M10 built: the state
 *     setters still run, the display lists are read and dropped, nothing is
 *     decoded, nothing reaches GL).  A drawn frame costs what it always did;
 *     a consumed one costs the game logic alone, which is what lets the
 *     average land on 16.7 ms.
 *
 * The policy, in the order the rules are applied:
 *
 *   1. never draw two consecutive frames (the cap: at most every 2nd retrace,
 *      so 30 fps when the card could do more);
 *   2. if the gate is late against the wall clock by more than half a
 *      period, do not draw -- run the game flat out until it has caught up;
 *   3. but never let rule 2 starve the picture: after MAXSKIP consecutive
 *      consumed frames the next one is drawn whatever the clock says;
 *   4. a frame --dumpframe asks for is always drawn (the md5 contract), and so
 *      is the frame after a pending F12 shot.
 *
 * What is measured: game seconds against wall seconds (the speed, in %),
 * presented frames per wall second, and the share of frames consumed rather
 * than drawn.  Every one of those goes into --perfwin's per-scene line and
 * into --status's once-a-second line, next to the numbers that were already
 * there.
 *
 * `--lockstep` is the pre-M17 gate on the same binary: every frame drawn,
 * the game as slow as the renderer.  --turbo and --nodraw imply it (they are
 * measurements of the renderer and of the game respectively, and a frame
 * skip would make either number a lie).
 */
#include "port.h"

#include <stdio.h>

#include <dolphin/types.h>
#include <dolphin/vi.h>

int gl13_draw_off(void);
void gl13_set_draw_off(int v);
unsigned gl13_frame_number(void);
int gl13_frame_wanted(unsigned n);
int gl13_shot_pending(void);
void gl13_begin_frame(void);
int port_ffto_active(void);

#define PERIOD (1.0 / 59.94)

static int active;           /* the mode is on for this run */
static int paused;           /* --ffto owns draw_off for now */
static unsigned since_draw;  /* retraces since the last drawn frame began */
static int next_drawn = 1;   /* what the gate decided for the frame the game
                              * is building now */

/* the whole-run tally, printed at shutdown */
static unsigned long n_retraces, n_drawn, n_forced, n_dump_forced, n_resync;
static double lost_seconds, max_late, t_pace0;
static unsigned long r_pace0;

/* --status's rolling second */
static unsigned win_retraces, win_drawn;

void port_framemode_init(void) {
    /* --turbo is a renderer measurement (every frame drawn, no pacing) and
     * --nodraw is a game measurement (no frame drawn, no pacing); a skip
     * under either would change what the number means.  --ffto is --nodraw
     * with an end, and frame mode takes over when it ends. */
    if (port_opt.lockstep || port_opt.turbo || (port_opt.nodraw && !port_opt.ffto) ||
        port_opt.headless) {
        port_opt.realtime = 0;
    }
    active = port_opt.realtime;
    if (port_opt.maxskip <= 0) {
        port_opt.maxskip = 5;
    }
    if (!active) {
        port_log("port> frame mode: lockstep (%s)\n",
                 port_opt.lockstep ? "--lockstep"
                 : port_opt.turbo  ? "--turbo"
                 : port_opt.nodraw ? "--nodraw"
                                   : "--headless");
        return;
    }
    port_log("port> frame mode: --realtime (retrace at 60 Hz on the wall clock, "
             "present at most every 2nd retrace, at least every %dth; "
             "--lockstep for the old gate)\n",
             port_opt.maxskip + 1);
}

int port_framemode_active(void) { return active && !paused; }

/* Called by the gate once per retrace, after the present and after the pacing
 * sleep, with how late the gate is against its own schedule (0 when it slept
 * to be on time).  Decides whether the frame the game is about to build is
 * drawn, and switches the renderer accordingly. */
void port_framemode_decide(double late, double now) {
    unsigned next;
    int draw;

    if (!active) {
        return;
    }
    if (port_ffto_active()) {
        /* --ffto is running the game flat out with the renderer off, and it
         * will switch it back on itself.  Nothing here until then. */
        paused = 1;
        return;
    }
    if (paused) {
        /* ffto just finished: it turned drawing on for its own reasons, so
         * the frame the game is about to build is drawn, and the tally starts
         * now rather than at boot. */
        paused = 0;
        since_draw = 0;
        next_drawn = 1;
        n_retraces = n_drawn = n_forced = n_dump_forced = n_resync = 0;
        lost_seconds = max_late = 0.0;
        t_pace0 = now;
        r_pace0 = VIGetRetraceCount();
        gl13_begin_frame();
        return;
    }
    if (t_pace0 == 0.0) {
        t_pace0 = now;
        r_pace0 = VIGetRetraceCount();
    }
    n_retraces++;
    win_retraces++;
    if (late > max_late) {
        max_late = late;
    }

    /* The frame the game builds next is presented as this number (the present
     * at the top of this retrace has already numbered the one just built). */
    next = gl13_frame_number() + 1;

    if (gl13_frame_wanted(next) || gl13_shot_pending()) {
        draw = 1;
        n_dump_forced++;
    } else if (since_draw < 1) {
        draw = 0; /* rule 1: the cap */
    } else if (since_draw >= (unsigned)port_opt.maxskip) {
        draw = 1; /* rule 3: never starve */
        if (late > 0.5 * PERIOD) {
            n_forced++;
        }
    } else {
        draw = late <= 0.5 * PERIOD; /* rule 2 */
    }

    if (draw) {
        since_draw = 0;
        n_drawn++;
        win_drawn++;
    } else {
        since_draw++;
    }
    next_drawn = draw;
    if (gl13_draw_off() == draw) {
        gl13_set_draw_off(!draw);
    }
    if (draw) {
        /* The clear the game asked for at the end of the last frame (drawn or
         * not) happens now, on the buffer this frame is drawn into. */
        gl13_begin_frame();
    }
}

int port_framemode_next_drawn(void) { return active ? next_drawn : !gl13_draw_off(); }

/* The gate fell more than a quarter second behind and re-based its schedule
 * rather than spin through the backlog: that much game time did not happen
 * at real speed.  Counted so the report can say so. */
void port_framemode_resync(double behind) {
    if (!active || paused) {
        return;
    }
    n_resync++;
    lost_seconds += behind;
    port_log("port> realtime: resync at retrace %u, %.0f ms behind dropped (%lu so far)\n",
             (unsigned)VIGetRetraceCount(), behind * 1000.0, n_resync);
    /* The ring drained during the stall and, paced to real time from here,
     * would never fill again: every later overrun would underrun.  Re-queue
     * the lead; the gap is already there. */
    if (port_opt.audiolead > 0) {
        port_audio_out_prime((unsigned)port_opt.audiolead);
    }
}

/* How far behind its schedule the gate may fall before it re-bases rather
 * than catches up.  In lockstep a quarter second (as before M17).  Under
 * --realtime a full second: a scene load or a cold texture cache stalls a
 * frame for 200-300 ms, and catching that up costs ~150 ms of consumed
 * frames while dropping it costs the game time and the audio's lead. */
double port_framemode_resync_limit(void) { return (active && !paused) ? 1.0 : 0.25; }

void port_framemode_window(double* speed_pct, double* presented_fps, double dt) {
    if (!active || dt <= 0.0) {
        *speed_pct = 0.0;
        *presented_fps = 0.0;
    } else {
        *speed_pct = 100.0 * (double)win_retraces / dt / 59.94;
        *presented_fps = (double)win_drawn / dt;
    }
    win_retraces = 0;
    win_drawn = 0;
}

void port_framemode_report(void) {
    double wall, game;
    if (!active || n_retraces == 0) {
        return;
    }
    wall = port_now_seconds() - t_pace0;
    game = (double)n_retraces / 59.94;
    port_log("\n---- --realtime: %lu paced retraces ----\n", n_retraces);
    port_log("  clocks   game %.2f s vs wall %.2f s -- speed %.1f%%%s\n", game, wall,
             wall > 0.0 ? 100.0 * game / wall : 0.0,
             lost_seconds > 0.0 ? "  (before the resyncs below)" : "");
    port_log("  present  %lu drawn of %lu (%.1f%% skipped), %.1f presented fps, "
             "%lu forced by the skip cap, %lu by --dumpframe\n",
             n_drawn, n_retraces, 100.0 * (double)(n_retraces - n_drawn) / (double)n_retraces,
             wall > 0.0 ? (double)n_drawn / wall : 0.0, n_forced, n_dump_forced);
    port_log("  late     worst %.1f ms behind the schedule; %lu resync(s) dropping %.2f s "
             "of game time\n",
             max_late * 1000.0, n_resync, lost_seconds);
}
