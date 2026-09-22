/* THP movies, played (M38, PLAN.md 53).
 *
 * The game's own movie player runs unmodified: src/game/thpmain.c
 * (HuTHPSprCreateVol, THPTestProc, THPViewSprFunc, THPDecodeFunc),
 * src/game/THPSimple.c (the file, its read ring of ten frames, the audio ring
 * of four, the mixer chained into the AI callback) and src/game/THPDraw.c's
 * setup and restore.  What the port supplies is what Nintendo's SDK did:
 *
 *   - `THPInit`, and `THPVideoDecode` -- the JPEG decode.  THPDec.c is 352
 *     paired-single sites of IDCT and is not ported; thp_jpeg.c is a small
 *     baseline decoder in THP's shape.  `THPAudioDecode` is Nintendo's own
 *     src/dolphin/thp/THPAudio.c, plain C, compiled as it stands.
 *   - the idle function the decode loop runs in (os/sreset_poll.c), and the
 *     AI's DMA source the mixer re-points (audio/musyx_sal.c);
 *   - the picture: THPDraw.c converts Y/U/V to RGB in five TEV stages with a
 *     negative S10 register and a subtract, which the port's GL 1.3 combiner
 *     translation cannot draw (it clamps the register and draws SUB as ADD).
 *     So the frame is converted on the CPU, with THPDraw's own constants
 *     (thp_jpeg.c), and drawn as one RGBA texture through the port's GX: one
 *     stage, texture times the sprite's colour register, alpha from its
 *     alpha -- what the game's fifth stage does.  One exact-text patch at the
 *     top of THPGXYuv2RgbDraw hands the draw here; `--thpyuv` hands it back
 *     (the planes, the game's TEV), for the A/B of PLAN.md 53.4.
 *
 * The decode, and why a late frame is dropped and never slowed.
 * `THPSimpleDecode` (the idle pass, once a retrace) hands `THPVideoDecode`
 * one compressed frame and the tile buffers of the next of its two frame
 * sets.  Here that copies the compressed bytes into a slot keyed by the tile
 * buffer, publishes the decode to the worker (workers.c) and returns at once:
 * the game's frame counter, its audio ring and its schedule advance exactly as
 * on the console, whatever the decode costs.  The frame is needed at the next
 * draw -- the sprite's draw function, a frame later -- and the draw finishes
 * the slot there: joined if the worker has it, run inline if not (the inline
 * twin: with one CPU nothing is ever published, and a frame is decoded only
 * when a drawn frame needs it).  A consumed frame (frame mode, --realtime)
 * draws nothing and decodes nothing, and when the next decode arrives for a
 * slot whose frame was never drawn, that frame is dropped: cancelled if it
 * was still queued, freed if it was done.  The movie's clock is the game's,
 * and the game's is the console's; a slow machine shows fewer of the frames.
 *
 * One CPU (M39, PLAN.md 54.2).  With no worker a frame is owed, and the
 * draw that wants it used to decode it whole, ~15 ms inside a drawn frame
 * that on the mode select was already long: the output ring ran dry (335
 * underruns against 84 without movies).  Two rules now, both on the owed
 * slot only (the worker's path is unchanged):
 *   - the slack: at the retrace, after the frame's work and the audio tick
 *     and before the pacing sleep, the newest owed frame is decoded a MCU row
 *     (and then converted 32 rows) at a time for as long as the schedule has
 *     room (port_thp_slack, called from vi.c) -- time the game thread would
 *     otherwise sleep; `--thpslice 0` turns it off;
 *   - the guard: a drawn frame finishes what is left of the owed frame only
 *     if the audio already queued covers that work and `--thpguard MS` more
 *     (default 60); if not, it shows the newest frame that is ready and the
 *     owed one goes on in the slack -- audio beats video.  0 = always finish.
 * The slices are the same arithmetic as the whole decode, in pieces
 * (thpj_decode_rows, thpj_to_rgba_rows), so the pictures are the same bytes.
 *
 * Nothing below do_read calls port_log (PLAN.md 51): this file is called from
 * the game thread only, and the job logs nothing.
 */
#include "port.h"
#include "thp_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin.h>
#include <dolphin/gx.h>
#include "game/THPSimple.h"

int gl13_draw_off(void);
unsigned gl13_frame_number(void);
int gl13_frame_wanted(unsigned n);
int gl13_shot_pending(void);

/* The converter's byte order: A R G B on a big-endian machine, which with
 * GL_BGRA / GL_UNSIGNED_INT_8_8_8_8_REV is the Mac's native texel layout. */
#ifdef __BIG_ENDIAN__
#define THP_ARGB 1
#else
#define THP_ARGB 0
#endif
void port_gx_tex_dirty(const void* addr, unsigned long n);

#define NSLOT 2

enum { S_EMPTY = 0, S_OWED, S_QUEUED, S_DONE, S_SHOWN };

typedef struct ThpSlot {
    PortJob job; /* first: the worker hands the job back as a PortJob* */
    const void* key;             /* the game's Y tile buffer for this frame set */
    u8 *gy, *gu, *gv;            /* the game's three tile buffers (--thpyuv) */
    u8* comp;                    /* the compressed frame, copied at the publish */
    unsigned comp_cap, comp_len;
    u8 *py, *pu, *pv;            /* row-major planes, the port's */
    ThpjCtx* ctx;
    u8* rgba;                    /* the job's output buffer (the pool's), taken by the draw */
    volatile int* rgba_flag;     /* its pool flag: 1 again once the upload has read it */
    int w, h, tiled;
    int state;
    volatile int cancel;
    int err;
    double ms;                   /* decode + convert, on whichever thread ran it */
    double ms_dec;               /* the decode alone */
    s32 movie_frame;
    /* M39: an owed frame done in pieces on the game thread */
    int phase;                   /* 0 not begun, 1 decoding MCU rows, 2 converting, 3 done */
    int conv_y;                  /* the next row to convert */
    int pieces;                  /* slack calls that did some of it */
} ThpSlot;

static ThpSlot slots[NSLOT];

/* The frames' RGBA buffers, 1.1 MB each: a pool the render thread hands back
 * (rt_texsubimage2d_owned's done flag) rather than a malloc and free a
 * frame -- a fresh large malloc is fresh pages, and faulting 270 of them in
 * cost the worker 5 ms a frame (PLAN.md 53.4).  Game thread only, except
 * the flag, which the render thread raises after the upload. */
#define NPOOL 6
static struct {
    u8* p;
    size_t n;
    volatile int avail;
} pool[NPOOL];
static unsigned long stat_pool_miss;

static u8* pool_get(size_t n, volatile int** flag) {
    int i;
    for (i = 0; i < NPOOL; i++) {
        if (pool[i].p && pool[i].avail && pool[i].n == n) {
            pool[i].avail = 0;
            *flag = &pool[i].avail;
            return pool[i].p;
        }
    }
    for (i = 0; i < NPOOL; i++) {
        if (!pool[i].p) {
            pool[i].p = (u8*)malloc(n);
            if (!pool[i].p) {
                break;
            }
            pool[i].n = n;
            pool[i].avail = 0;
            *flag = &pool[i].avail;
            return pool[i].p;
        }
    }
    /* all in flight (or a size change): a plain buffer the upload frees */
    stat_pool_miss++;
    *flag = NULL;
    return (u8*)malloc(n);
}

static void pool_release(u8* p, volatile int* flag) {
    if (!p) {
        return;
    }
    if (flag) {
        *flag = 1;
    } else {
        free(p);
    }
}
static int next_victim;
static PortTexture ptex;

/* the movie in progress, for its log line */
static struct {
    int open;
    u32 frames_total;
    float fps;
    int w, h, audio;
    unsigned long published, worker, inline_, cancelled, dropped_done, drawn, errors;
    unsigned long draws, consumed_draws, audio_short;
    double dec_ms, dec_worst, join_ms, join_worst, dec_only_ms;
    unsigned long waited, shown_previous;
    double first_draw, last_draw;
    s32 last_drawn_frame, last_published;
    /* M39, one CPU */
    unsigned long guarded, slack_calls, slack_done, sliced_frames;
    double slack_ms;
    unsigned long underruns0, underruns;
} mv;

static unsigned long total_movies, total_frames_drawn, total_dropped;
static int skipped;
static char first_skipped[64];

/* ---- the policy ---------------------------------------------------------- */

/* Movies need the AI's address to survive a u32 both ways (musyx_sal.c's
 * AIInitDMA), which only a 32-bit machine gives; --nomovies is the M2-M37
 * behaviour: patches.txt's two thpmain.c patches skip the movie outright. */
int portTHPAvailable(void) { return sizeof(void*) == 4 && !port_opt.nomovies; }

void portTHPSkip(const char* path) {
    if (!skipped) {
        snprintf(first_skipped, sizeof(first_skipped), "%s", path ? path : "?");
    }
    skipped++;
    port_log("port> THP: %s skipped (%s)\n", path ? path : "?",
             port_opt.nomovies ? "--nomovies" : "no movies on a 64-bit host");
}

/* ---- the SDK surface ----------------------------------------------------- */

BOOL THPInit(void) {
    if (!portTHPAvailable()) {
        return FALSE;
    }
    thpj_init();
    /* THPSimpleInit registers its mixer next; mark it so the AI knows the
     * period's buffer may be re-pointed (musyx_sal.c) */
    port_ai_next_callback_is_thp();
    return TRUE;
}

/* M39: the owed frame, in pieces, until done or `deadline` (port_now_seconds
 * time; 0 = no deadline).  1 when the frame is complete. */
#define THP_BAND 32
static int slot_step(ThpSlot* s, double deadline) {
    double t = port_now_seconds();
    int did = 0;
    while (s->phase < 3 && (deadline == 0.0 || t < deadline)) {
        int ph = s->phase;
        double t1;
        if (ph == 0) {
            s->ms = s->ms_dec = 0.0;
            s->err = thpj_decode_begin(s->ctx, s->comp, s->comp_len,
                                       s->tiled ? s->gy : s->py, s->tiled ? s->gu : s->pu,
                                       s->tiled ? s->gv : s->pv, s->w, s->h, s->tiled, NULL);
            s->phase = s->err ? 3 : 1;
            s->conv_y = 0;
        } else if (ph == 1) {
            int r = thpj_decode_rows(s->ctx, 1);
            if (r != THPJ_MORE) {
                s->err = r;
                s->phase = r || s->tiled || !s->rgba ? 3 : 2;
            }
        } else {
            thpj_to_rgba_rows(s->ctx, s->py, s->pu, s->pv, s->w, s->h, s->rgba, s->w * 4,
                              THP_ARGB, s->conv_y, s->conv_y + THP_BAND);
            s->conv_y += THP_BAND;
            if (s->conv_y >= s->h) {
                s->phase = 3;
            }
        }
        t1 = port_now_seconds();
        s->ms += (t1 - t) * 1000.0;
        if (ph <= 1) {
            s->ms_dec += (t1 - t) * 1000.0;
        }
        t = t1;
        did = 1;
    }
    if (did) {
        s->pieces++;
    }
    return s->phase == 3;
}

static void slot_run(PortJob* j) {
    ThpSlot* s = (ThpSlot*)j;
    double t0 = port_now_seconds();
    if (s->cancel) {
        s->err = -1;
        return;
    }
    if (s->tiled) {
        s->err = thpj_decode(s->ctx, s->comp, s->comp_len, s->gy, s->gu, s->gv, s->w, s->h, 1,
                             NULL);
        s->ms_dec = (port_now_seconds() - t0) * 1000.0;
    } else {
        s->err = thpj_decode(s->ctx, s->comp, s->comp_len, s->py, s->pu, s->pv, s->w, s->h, 0,
                             NULL);
        s->ms_dec = (port_now_seconds() - t0) * 1000.0;
        if (!s->err && s->rgba) {
            thpj_to_rgba(s->ctx, s->py, s->pu, s->pv, s->w, s->h, s->rgba, s->w * 4, THP_ARGB);
        }
    }
    s->ms = (port_now_seconds() - t0) * 1000.0;
}

static int slot_alloc(ThpSlot* s, int w, int h) {
    if (s->ctx && s->w == w && s->h == h) {
        return 1;
    }
    free(s->py);
    free(s->ctx);
    s->ctx = (ThpjCtx*)malloc(thpj_ctx_size());
    s->py = (u8*)malloc((size_t)w * h * 3 / 2);
    if (!s->ctx || !s->py) {
        return 0;
    }
    s->pu = s->py + (size_t)w * h;
    s->pv = s->pu + (size_t)w * h / 4;
    s->w = w;
    s->h = h;
    return 1;
}

/* the frame the slot holds is done with: finished, or dropped */
static void slot_retire(ThpSlot* s) {
    if (s->state == S_QUEUED) {
        s->cancel = 1;
        port_worker_join(port_worker_decode(), &s->job);
        s->cancel = 0;
        if (s->err == -1) {
            mv.cancelled++;
        } else {
            mv.dropped_done++; /* the worker had begun it: done, and never drawn */
        }
    } else if (s->state == S_OWED) {
        if (s->phase == 3) {
            mv.dropped_done++; /* finished in the slack, and never drawn */
        } else {
            mv.cancelled++;
        }
    } else if (s->state == S_DONE) {
        mv.dropped_done++;
    }
    pool_release(s->rgba, s->rgba_flag);
    s->rgba = NULL;
    s->rgba_flag = NULL;
    s->state = S_EMPTY;
}

/* the draw needs the slot's frame now */
static void slot_finish(ThpSlot* s) {
    double t0 = port_now_seconds(), d;
    if (s->state == S_QUEUED) {
        port_worker_join(port_worker_decode(), &s->job);
        if (s->job.ran_inline) {
            mv.inline_++;
        } else {
            mv.worker++;
        }
    } else if (s->state == S_OWED) {
        /* what the slack left (all of it, with --nothpslice or no slack) */
        if (s->pieces) {
            mv.sliced_frames++;
        }
        slot_step(s, 0.0);
        mv.inline_++;
    } else {
        return;
    }
    d = (port_now_seconds() - t0) * 1000.0;
    mv.join_ms += d;
    if (d > mv.join_worst) {
        mv.join_worst = d;
    }
    mv.dec_ms += s->ms;
    mv.dec_only_ms += s->ms_dec;
    if (d > 0.5) {
        mv.waited++;
    }
    if (s->ms > mv.dec_worst) {
        mv.dec_worst = s->ms;
    }
    if (s->err) {
        if (mv.errors++ < 5) {
            port_log("port> THP: frame %ld: decode error %d\n", (long)s->movie_frame, s->err);
        }
    }
    s->state = S_DONE;
}

static ThpSlot* slot_find(const void* key) {
    int i;
    for (i = 0; i < NSLOT; i++) {
        if (slots[i].key == key) {
            return &slots[i];
        }
    }
    return NULL;
}

static void movie_begin(void) {
    const THPHeader* hd = &SimpleControl.unk3C;
    memset(&mv, 0, sizeof(mv));
    mv.open = 1;
    mv.frames_total = hd->mNumFrames;
    mv.fps = hd->mFrameRate;
    mv.w = (int)SimpleControl.unk80.unk00;
    mv.h = (int)SimpleControl.unk80.unk04;
    mv.audio = SimpleControl.unk9F;
    mv.last_drawn_frame = -1;
    mv.underruns0 = port_audio_out_underruns();
    total_movies++;
    port_log("port> THP: a movie opens: %dx%d, %.2f fps, %u frames (%.1f s), %s, "
             "buffer %u bytes; decode on the %s%s\n",
             mv.w, mv.h, (double)mv.fps, (unsigned)mv.frames_total,
             mv.fps > 0 ? mv.frames_total / mv.fps : 0.0,
             mv.audio ? "with audio" : "no audio", (unsigned)hd->mBufferSize,
             port_threads_on() ? "worker, one frame ahead" : "game thread, at the draw",
             port_opt.thpyuv ? " (--thpyuv: the game's TEV)" : "");
}

static void movie_end(void) {
    double wall = mv.last_draw - mv.first_draw;
    unsigned long decoded = mv.worker + mv.inline_;
    mv.underruns = port_audio_out_underruns() - mv.underruns0;
    port_log("port> THP: the movie closes: %lu of %u frames published, %lu drawn, "
             "%lu dropped (%lu never decoded, %lu decoded and superseded); "
             "decoded %lu (%lu on the worker, %lu inline), %lu errors; "
             "decode+convert %.2f ms a frame (decode %.2f, worst %.2f), the draw waited on %lu "
             "(%.2f ms a frame, worst %.2f) and found the newest frame not ready %lu times; "
             "%.2f frames a second presented over %.1f s; %lu audio periods short\n",
             mv.published, (unsigned)mv.frames_total, mv.drawn, mv.cancelled + mv.dropped_done,
             mv.cancelled, mv.dropped_done, decoded, mv.worker, mv.inline_, mv.errors,
             decoded ? mv.dec_ms / decoded : 0.0, decoded ? mv.dec_only_ms / decoded : 0.0,
             mv.dec_worst, mv.waited, decoded ? mv.join_ms / decoded : 0.0, mv.join_worst,
             mv.shown_previous,
             wall > 0 && mv.drawn > 1 ? (mv.drawn - 1) / wall : 0.0, wall, mv.audio_short);
    {
        char one[256] = "";
        if (!port_threads_on()) {
            snprintf(one, sizeof(one),
                     "; one CPU: the slack worked on %lu of the drawn frames and finished %lu "
                     "(%lu calls, %.1f ms), %lu draw(s) held back for the audio (--thpguard %d%s)",
                     mv.sliced_frames, mv.slack_done, mv.slack_calls, mv.slack_ms, mv.guarded,
                     port_opt.thpguard, port_opt.nothpslice ? ", --nothpslice" : "");
        }
        port_log("port> THP: while it played the output device ran dry %lu time(s)%s\n",
                 mv.underruns, one);
    }
    total_frames_drawn += mv.drawn;
    total_dropped += mv.cancelled + mv.dropped_done;
    mv.open = 0;
}

/* The compressed size of the component `file` points at: THPSimpleDecode
 * walks the frame at SimpleControl.unkCC[unkAC] -- two words, one size per
 * component, then the components in order -- and VideoDecode passes the
 * video one.  Bounded by the header's largest frame. */
static unsigned comp_size(const void* file) {
    const s32* fr = SimpleControl.unkCC[SimpleControl.unkAC].unk00;
    u32 n = SimpleControl.unk6C.mNumComponents, i;
    const u8* p;
    if (fr && n <= 16) {
        p = (const u8*)(fr + 2 + n);
        for (i = 0; i < n; i++) {
            if (p == (const u8*)file) {
                u32 sz = (u32)fr[2 + i];
                return sz <= SimpleControl.unk3C.mBufferSize ? sz : SimpleControl.unk3C.mBufferSize;
            }
            p += fr[2 + i];
        }
    }
    return SimpleControl.unk3C.mBufferSize;
}

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    ThpSlot* s;
    unsigned len;
    int w = (int)SimpleControl.unk80.unk00, h = (int)SimpleControl.unk80.unk04;
    (void)work;
    if (!file) {
        return 25;
    }
    if (!tileY || !tileU || !tileV) {
        return 27;
    }
    if (w <= 0 || h <= 0 || w > THPJ_MAX_W || (w & 15) || (h & 15)) {
        return 11;
    }
    if (!mv.open) {
        movie_begin();
    }
    s = slot_find(tileY);
    if (!s) {
        s = &slots[next_victim];
        next_victim = (next_victim + 1) % NSLOT;
        slot_retire(s);
        s->key = tileY;
    }
    slot_retire(s);
    if (!slot_alloc(s, w, h)) {
        return 6;
    }
    len = comp_size(file);
    if (len > s->comp_cap) {
        free(s->comp);
        s->comp = (u8*)malloc(len);
        s->comp_cap = s->comp ? len : 0;
        if (!s->comp) {
            return 6;
        }
    }
    memcpy(s->comp, file, len);
    s->comp_len = len;
    s->gy = (u8*)tileY;
    s->gu = (u8*)tileU;
    s->gv = (u8*)tileV;
    s->tiled = port_opt.thpyuv;
    s->movie_frame = SimpleControl.unkCC[SimpleControl.unkAC].unk04;
    if (s->movie_frame < mv.last_published) {
        mv.last_drawn_frame = -1; /* HuTHPRestart: the movie begins again */
    }
    mv.last_published = s->movie_frame;
    s->err = 0;
    s->rgba = NULL;
    s->rgba_flag = NULL;
    if (!s->tiled) {
        s->rgba = pool_get((size_t)w * h * 4, &s->rgba_flag);
    }
    s->job.run = slot_run;
    s->phase = 0;
    s->pieces = 0;
    s->state = S_OWED;
    mv.published++;
    if (port_worker_submit(port_worker_decode(), &s->job)) {
        s->state = S_QUEUED;
    }
    return 0;
}

/* ---- the draw ------------------------------------------------------------ */

static int must_wait(void) {
    return !port_framemode_active() || gl13_frame_wanted(gl13_frame_number() + 1) ||
           gl13_shot_pending();
}

/* M39 (one CPU): a frame the slack has finished, or the worker has */
static int slot_ready(ThpSlot* s) {
    return s->state == S_DONE || (s->state == S_OWED && s->phase == 3) ||
           (s->state == S_QUEUED && port_worker_done(&s->job));
}

/* M39: would finishing this owed frame now eat into the audio the device
 * needs?  The queued audio (the ring, 128 bytes a ms) must cover the work
 * left -- the frame's mean cost so far, by its progress -- and --thpguard
 * more, for the rest of the frame's own work. */
static int guard_holds(ThpSlot* s) {
    unsigned long decoded = mv.worker + mv.inline_;
    double est = decoded ? mv.dec_ms / decoded : 16.0, left, ring;
    if (port_opt.thpguard <= 0 || !port_audio_out_opened()) {
        return 0;
    }
    if (s->phase == 0) {
        left = est;
    } else if (s->phase == 1) {
        left = est - s->ms; /* the decode's share is the bigger half */
    } else {
        left = est * 0.47 * (1.0 - (double)s->conv_y / s->h);
    }
    if (left < 0.5) {
        left = 0.5;
    }
    ring = port_audio_out_queued() / 128.0;
    return ring < left + port_opt.thpguard;
}

/* THPGXYuv2RgbDraw's first line (patches.txt): 1 = drawn here, 0 = the
 * game's own path draws (--thpyuv, or a buffer this file never decoded). */
int portTHPDraw(void* yImage, s16 x, s16 y, s16 polyWidth, s16 polyHeight) {
    ThpSlot* s = slot_find(yImage);
    GXTexObj obj;
    if (!s || s->state == S_EMPTY) {
        return 0;
    }
    mv.draws++;
    if (gl13_draw_off()) {
        /* a consumed frame: nothing reaches GL, so nothing is decoded for it */
        mv.consumed_draws++;
        return 1;
    }
    if (!port_opt.thpyuv && s->state == S_OWED && s->phase < 3 && !must_wait() &&
        guard_holds(s)) {
        /* One CPU, and the audio is short: the newest ready frame shows and
         * this one goes on in the retrace's slack (port_thp_slack) */
        ThpSlot* o = &slots[s == &slots[0] ? 1 : 0];
        mv.guarded++;
        mv.shown_previous++;
        if (slot_ready(o) && o->movie_frame > mv.last_drawn_frame) {
            s = o;
        } else {
            if (!ptex.w) {
                return 1;
            }
            goto draw;
        }
    } else if (!port_opt.thpyuv && s->state == S_QUEUED && !port_worker_done(&s->job) &&
               !must_wait()) {
        /* Under --realtime the draw does not wait for a decode: it shows the
         * last frame that is ready, and this one at the next drawn frame if
         * it is still the current one -- late, never slowing the game's
         * frame (PLAN.md 53.6).  A frame --dumpframe or F12 wants waits, so
         * a named frame's picture is the same in every run. */
        ThpSlot* o = &slots[s == &slots[0] ? 1 : 0];
        mv.shown_previous++;
        if (slot_ready(o) && o->movie_frame > mv.last_drawn_frame) {
            /* the frame before this one is ready and was never shown: that
             * is the newest picture there is, so it goes up now */
            s = o;
        } else {
            if (!ptex.w) {
                return 1;
            }
            goto draw;
        }
    }
    slot_finish(s);
    if (port_opt.thpyuv) {
        /* the planes are the game's now; tell the texture cache they changed */
        if (s->state == S_DONE) {
            port_gx_tex_dirty(s->gy, (unsigned long)s->w * s->h);
            port_gx_tex_dirty(s->gu, (unsigned long)s->w * s->h / 4);
            port_gx_tex_dirty(s->gv, (unsigned long)s->w * s->h / 4);
            s->state = S_SHOWN;
            mv.drawn++;
        }
        return 0;
    }
    if (s->state == S_DONE) {
        if (s->rgba && !s->err) {
            /* a frame the bind never took is superseded */
            pool_release((u8*)ptex.pending, ptex.pending_done);
            ptex.w = s->w;
            ptex.h = s->h;
            ptex.argb = THP_ARGB;
            ptex.pending = s->rgba;
            ptex.pending_done = s->rgba_flag;
            s->rgba = NULL;
            s->rgba_flag = NULL;
        }
        s->state = S_SHOWN;
        mv.drawn++;
        {
            double t = port_now_seconds();
            if (!mv.first_draw) {
                mv.first_draw = t;
            }
            mv.last_draw = t;
        }
        mv.last_drawn_frame = s->movie_frame;
        if (port_opt.thplog) {
            port_log("port> THP: frame %ld drawn at retrace %lu (decode %.2f ms, %s)\n",
                     (long)s->movie_frame, (unsigned long)VIGetRetraceCount(), s->ms,
                     s->job.ran_inline ? "inline" : "worker");
        }
    }
    if (!ptex.w) {
        return 1; /* nothing ever decoded into this slot: draw nothing */
    }
draw:
    /* THPDraw's fifth stage is the whole of what survives the conversion:
     * colour = RGB * C1 (the sprite's colour, THPGXYuv2RgbSetup's
     * GXSetTevColor(GX_TEVREG1)), alpha = A1, blended SRCALPHA as set up */
    GXInitTexObj(&obj, &ptex, (u16)ptex.w, (u16)ptex.h, (GXTexFmt)GX_TF_PORT_RGBA, GX_CLAMP,
                 GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE,
                    GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE,
                      GX_PTIDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C1, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A1);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXBegin(GX_QUADS, GX_VTXFMT7, 4);
    GXPosition3s16(x, y, 0);
    GXTexCoord2s16(0, 0);
    GXPosition3s16(x + polyWidth, y, 0);
    GXTexCoord2s16(1, 0);
    GXPosition3s16(x + polyWidth, y + polyHeight, 0);
    GXTexCoord2s16(1, 1);
    GXPosition3s16(x, y + polyHeight, 0);
    GXTexCoord2s16(0, 1);
    GXEnd();
    return 1;
}

/* ---- the retrace, the status, the report --------------------------------- */

/* M39 (one CPU): vi.c calls this at the retrace, after the frame's work and
 * the audio tick, when the schedule has room before the next one: the newest
 * owed frame is worked on in pieces until `deadline` (a MCU row or 32
 * converted rows is ~0.3-0.5 ms on the G4), so the draw that wants it finds
 * less or nothing left.  Never past the deadline: the retrace is not late. */
void port_thp_slack(double deadline) {
    ThpSlot* s = NULL;
    int i;
    double t0;
    if (!mv.open || port_opt.nothpslice) {
        return;
    }
    for (i = 0; i < NSLOT; i++) {
        ThpSlot* c = &slots[i];
        if (c->state == S_OWED && c->phase < 3 && (!s || c->movie_frame > s->movie_frame)) {
            s = c;
        }
    }
    if (!s) {
        return;
    }
    t0 = port_now_seconds();
    if (t0 >= deadline) {
        return;
    }
    mv.slack_calls++;
    if (slot_step(s, deadline)) {
        mv.slack_done++;
    }
    mv.slack_ms += (port_now_seconds() - t0) * 1000.0;
}

void port_thp_retrace(void) {
    if (!mv.open) {
        return;
    }
    /* the audio the mixer will want before the next retrace: 3 or 4 periods
     * of 160 samples; short of that while frames are still coming is an
     * underrun of the movie's own ring (it is the decode that is late) */
    if (SimpleControl.unk98 && SimpleControl.unk9D == 1 && SimpleControl.unk9F &&
        SimpleControl.unk144[0].unk0C >= 0 &&
        (u32)SimpleControl.unk144[0].unk0C + 1 < mv.frames_total) {
        u32 have = 0;
        int i, k = SimpleControl.unk198;
        for (i = 0; i < 4; i++) {
            u32 n = SimpleControl.unk164[(k + i) & 3].unk08;
            if (!n) {
                break;
            }
            have += n;
        }
        if (have < 4 * 160) {
            mv.audio_short++;
        }
    }
    if (!SimpleControl.unk98) {
        int i;
        for (i = 0; i < NSLOT; i++) {
            slot_retire(&slots[i]);
            slots[i].key = NULL;
        }
        pool_release((u8*)ptex.pending, ptex.pending_done);
        ptex.pending = NULL;
        ptex.pending_done = NULL;
        movie_end();
    }
}

void port_thp_status(char* buf, unsigned long n) {
    if (!mv.open || !n) {
        if (n) {
            buf[0] = 0;
        }
        return;
    }
    snprintf(buf, n, "  thp %ld/%u drawn %lu drop %lu", (long)mv.last_drawn_frame,
             (unsigned)mv.frames_total, mv.drawn, mv.cancelled + mv.dropped_done);
}

void port_thp_report(void) {
    if (skipped) {
        port_log("port> THP: %d movie(s) skipped, first %s\n", skipped, first_skipped);
    }
    if (total_movies) {
        port_log("port> THP: %lu movie(s) played, %lu frames drawn, %lu dropped; %lu frame "
                 "buffers from outside the pool\n",
                 total_movies, total_frames_drawn + (mv.open ? mv.drawn : 0),
                 total_dropped + (mv.open ? mv.cancelled + mv.dropped_done : 0), stat_pool_miss);
    }
}
