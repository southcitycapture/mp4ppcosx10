/* `--ffto N`: be at frame N of a deterministic run without waiting for it.
 *
 * The renderer is ~70% of every frame on the G4 (PLAN.md §21.1: `gx` is 86% of
 * the frame before M9b, 60-70% after it), and the game logic never reads a
 * single byte of it back.  GX on this console is a write-only command stream:
 * the game builds display lists in its own memory, hands them to
 * `GXCallDisplayList`, sets state through `GXSet*`, and the only things it ever
 * reads back are the draw-sync token and the fifo status -- both of which live
 * in `gx_state.c` and are pure bookkeeping that costs nothing.  So a run with
 * the renderer switched off is *the same run*: same input, same clock, same
 * mix, same RNG, same everything, only faster.
 *
 * That is the whole trick.  `--nodraw` switches the renderer off and leaves it
 * off; `--ffto N` switches it off until frame N, turns it back on a frame or
 * two early so the texture cache and the EFB are warm, and then carries on at
 * the normal pace.  `--ffto 7000 --dumpframe 7000` is therefore a way of
 * asking for the §21.1 reference frame in a couple of minutes rather than ten.
 *
 * What is deliberately *not* switched off:
 *
 *   - the audio mix.  MusyX runs on the game thread from the retrace gate and
 *     the game reads voice and stream state back (`sndStreamStatus`,
 *     `numPlay`, the SE channel limit that M9b spent an evening on), so the
 *     mix is part of the determinism contract (§22.5) and it keeps running
 *     exactly as it does in a normal run;
 *   - the pad, the DVD service, the ARAM DMA service and the reset watcher,
 *     all of which are events the game sees;
 *   - display-list *recording*.  `GXBeginDisplayList`/`GXEndDisplayList` write
 *     into the game's memory and return a size the game stores; that path is
 *     untouched (gx_draw.c checks `dl_recording` before it checks anything).
 *
 * The one thing that does change is the wall clock, and under `--rtc` /
 * `--deterministic` the game cannot see the wall clock at all.
 */
#include "port.h"

#include <stdio.h>

/* gx_internal.h lives next to the backend; these three are all this file
 * wants from it, and declaring them here keeps the debug tree independent of
 * the GX tree's include path. */
int gl13_draw_off(void);
void gl13_set_draw_off(int v);
void gx_tex_flush_all(void);
unsigned gl13_frame_number(void);

static int active;         /* the skip is running */
static int done;           /* ...and has finished */
static int saved_turbo;
static double t0;
static unsigned f0;

void port_ffto_init(void) {
    if (port_opt.nodraw) {
        gl13_set_draw_off(1);
        port_log("port> --nodraw: the GX command streams are consumed and "
                 "nothing is drawn\n");
    }
    if (!port_opt.ffto) {
        return;
    }
    active = 1;
    /* Flat out for the skip, whatever the run asked for.  Under --rtc the
     * pacing gate is already open (the deterministic clock does not sleep),
     * so this only matters for a wall-clock run; it is restored at N so that
     * `--ffto N` on its own leaves a normally paced game on the screen. */
    saved_turbo = port_opt.turbo;
    port_opt.turbo = 1;
    t0 = port_now_seconds();
    f0 = gl13_frame_number();
    port_log("port> --ffto %d: drawing is off until frame %d (warm-up %d)\n",
             port_opt.ffto, port_opt.ffto, port_opt.ffto_warm);
}

/* Called at the top of every retrace, before the present that numbers the
 * frame the game has just built.  At this point `gl13_frame_number()` is the
 * count of frames already presented, the frame about to be presented is that
 * plus one, and the next frame the game will build is that plus two -- which
 * is the one this decision affects. */
int port_ffto_active(void) { return active && !done; }
/* The run's own --turbo, before the skip borrowed the flag (M19: frame mode
 * reads this, so a plain `--ffto N --realtime` is real time after N). */
int port_ffto_user_turbo(void) { return active ? saved_turbo : port_opt.turbo; }

void port_ffto_tick(void) {
    unsigned now;
    double dt;

    if (!active || done) {
        return;
    }
    now = gl13_frame_number();
    if ((int)now + 2 + port_opt.ffto_warm < port_opt.ffto) {
        return;
    }
    done = 1;
    active = 0;
    dt = port_now_seconds() - t0;
    /* The texture cache filled up with keys that never got a GL name while the
     * renderer was off; drop it so the first drawn frame takes the cold path
     * for everything it binds and comes out byte-identical to a straight run
     * (gx_tex.c: gx_tex_flush_all). */
    gx_tex_flush_all();
    gl13_set_draw_off(0);
    port_opt.turbo = saved_turbo;
    port_log("port> ffto: reached frame %d in %.1f s (%u frames, %.1f fps, "
             "drawing back on at frame %u)\n",
             port_opt.ffto, dt, now - f0,
             dt > 0.0 ? (double)(now - f0) / dt : 0.0, now + 2);
}
