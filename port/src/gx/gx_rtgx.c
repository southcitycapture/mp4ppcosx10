/* M43 (PLAN.md 58.2): --rtgx, the translation on the render thread.
 *
 * M27's seam is the GL call: the game thread translates GX state into GL
 * (gl13_state.c's raster state and projection, gx_tev.c's units, gx_vprog.c's
 * parameters and arrays) through one shadow of GL's state, and the render
 * thread replays the calls.  On the screens whose game thread is the wall
 * with room on the render thread (PLAN.md 57.10 class 2) that translation is
 * 1-3 ms of the game thread's cycle, and this moves it.
 *
 * The rule that makes it exact: **one shadow, one owner, at every position
 * of the stream.**  The four translation files are compiled twice (port/
 * Makefile RTI_OBJS, gx_rti.h): the game thread's instance, as always, and
 * the render thread's (rti_*), with its own shadow and caches, reading
 * `gx_rti` -- a replica of GXState the records below keep equal, byte for
 * byte, to every field the translation reads.  `rtgx_owner_rt` says whose
 * shadow describes GL at the end of the stream:
 *
 *  - a batch the render thread translates (owner RT): the game thread makes
 *    the decisions that read the game's memory or its own paths -- the
 *    vertex program's variant (gx_vprog_draw), the texture binds (the cache's
 *    content hash, decode and upload: gx_tev_bind_textures), the CPU
 *    transform -- and records one call: the GX state that changed since the
 *    last record (deltas against `gx_sent`, which mirrors the replica), the
 *    batch's context (the matrices by value, the variant, the arrays), and
 *    the render thread's instance runs the same translation from it;
 *  - every call the game thread makes into the shadow while the render
 *    thread owns it (a bind, an upload's bind, the invalidation after a
 *    clear, a copy, the present; the program disable) is forwarded into the
 *    stream in order (gl13_state.c GLC_FWD) and made there;
 *  - a batch the render thread cannot take (the fragment shader's warp: its
 *    binds live inside its own unit loop) takes the shadow back: the game
 *    thread's instance forgets everything (glc_invalidate) and translates
 *    it as before; the next eligible batch hands it over again, the render
 *    thread's instance forgetting everything first.  A forgotten shadow
 *    re-states GL; it never elides a call it should have made.
 *
 * The same GL calls reach the driver in the same order within a batch, but
 * the texture binds of a batch now come before its unit enables and
 * environment (the unit loop interleaved them): the state at the draw is
 * the same state, set by the same values -- exact by construction, and the
 * md5s and the minigame frame chain say so (PLAN.md 58.2).
 *
 * `--rtgx 0` is the old path, `1` every drawn frame, `auto` per drawn frame
 * from the last cycles of both threads, like --rtdecode auto: on only where
 * the game thread is the longer pole by more than the translation it would
 * hand over, so a screen whose render thread is the pole does not pay. */
#include "gx_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the render thread's instance (gx_rti.h renamed these) ---------------- */
void rti_glc_invalidate(void);
void rti_glc_active_texture(int unit);
void rti_glc_client_active_texture(int unit);
void rti_glc_bind_texture(int unit, unsigned name);
void rti_glc_note_bind(int unit, unsigned name);
void rti_glc_unit_enable_tex2d(int unit, int on);
void rti_glc_tex_matrix(int unit, float su, float sv);
void rti_glc_tex_matrix_fold(int unit, float su, float sv, float tv);
void rti_glc_color_sum(int on);
void rti_glc_vertex_array(const void* p, int stride);
void rti_glc_color_array(const void* p, int stride);
void rti_glc_normal_array(const void* p, int stride);
void rti_glc_coord_array(int unit, const void* p, int stride);
void rti_gl13_apply_transform(void);
void rti_gl13_apply_raster_state(void);
int rti_gl13_zprepass_wanted(void);
void rti_gl13_zprepass_begin(void);
void rti_gl13_zprepass_end(void);
void rti_gx_tev_apply(void);
void rti_gx_unit_memo(int on);
int rti_gx_tev_unit_source(int u, u8* coord, u8* map);
void rti_gx_vprog_disable(void);
void rti_gx_vprog_bind(const GxXfDesc* d);
void rti_gx_vprog_pending_set(const void* in);
void rti_gx_vprog_forget_vbo(void);
void rti_gx_vprog_forget_bound(void);
void rti_gx_tev_report(void);
void rti_gx_tfs_report(void);
void rti_glc_stats(unsigned* emitted, unsigned* elided);
void rti_gx_vprog_report(void);

/* the game thread's own */
void gx_vprog_forget_vbo(void);
size_t gx_vprog_pending_size(void);
void gx_vprog_pending_get(void* out);
int rt_threaded(void);
int rt_is_render_thread(void);
void rt_set_skip_draws(int on);
void rt_call(void (*fn)(void*), const void* args, size_t n, int sync); /* gx_rt.h */

/* ---- what the render thread's instance reads instead of the game's -------- */
GXState gx_rti;                    /* the replica                                  */
int rti_gx_hilite_stage = -1;      /* the batch's, as recorded                     */
int rti_gx_hilite_mode;
int rti_gx_force_flags;
u8 rti_gx_unit_alpha_min[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
static int rti_live = 1;
int rti_gl13_live(void) { return rti_live; }
GXTexObjPort* rti_gx_bound_tex(unsigned id) {
    return (GXTexObjPort*)gx_bound_tex_of(&gx_rti, id);
}
/* gx_warn's table is the game thread's: the render thread keeps its own */
void rti_gx_warn(const char* what) {
    static const char* seen[64];
    static int nseen;
    int i;
    for (i = 0; i < nseen; i++) {
        if (seen[i] == what) {
            return;
        }
    }
    if (nseen < 64) {
        seen[nseen++] = what;
    }
    if (port_opt.gxwarn) {
        port_log("port> gx (render thread's translation): %s\n", what);
    }
}
/* the fragment shader's binds are never the render thread's (the batch is
 * the game thread's); a call here is a bug, said once */
void rti_tex_bind_refused(int unit, GXTexObjPort* o, u8 swap) {
    static int told;
    (void)unit; (void)o; (void)swap;
    if (!told++) {
        port_log("*** rtgx: a texture bind reached the render thread's instance -- a bug\n");
    }
}
int rti_tex_bind_tiled_refused(int unit, GXTexObjPort* sheet, GXTexObjPort* map, const GXIndTile* t) {
    rti_tex_bind_refused(unit, sheet, 0);
    (void)map; (void)t;
    return 1;
}

/* ---- the game thread's side ------------------------------------------------ */
int rtgx_owner_rt;          /* the render thread's shadow describes GL at the stream's end */
static int frame_on;        /* this drawn frame's batches are the render thread's          */
static int mode_resolved = -1;
static GXState gx_sent;     /* the replica, as the records built it                        */
static unsigned long st_rt_batches, st_game_batches, st_to_rt, st_to_game, st_fwd,
    st_frames_on, st_frames_off, st_chunks, st_zp;
static double st_rec_bytes;
static unsigned long st_unexpected;
/* the auto decision's inputs: the translation's cost a drawn frame, each side */
static double tg_frame_s, tg_ms;        /* game thread (frames it owned)         */
static volatile double tr_last_ms;      /* render thread, published at present   */
static double tr_ms;
static int au_streak;

static int rtgx_mode(void) {
    if (mode_resolved < 0) {
        mode_resolved = port_opt.rtgx;
    }
    return mode_resolved;
}

/* ---- the forwarders ---------------------------------------------------------- */
typedef struct FwdArgs {
    int op, a, b;
    float x, y, z;
} FwdArgs;
static void fwd_fn(void* p) {
    const FwdArgs* f = (const FwdArgs*)p;
    switch (f->op) {
        case RTGX_F_INVALIDATE: rti_glc_invalidate(); break;
        case RTGX_F_ACTIVE: rti_glc_active_texture(f->a); break;
        case RTGX_F_CLIENT_ACTIVE: rti_glc_client_active_texture(f->a); break;
        case RTGX_F_BIND: rti_glc_bind_texture(f->a, (unsigned)f->b); break;
        case RTGX_F_NOTE_BIND: rti_glc_note_bind(f->a, (unsigned)f->b); break;
        case RTGX_F_ENABLE2D: rti_glc_unit_enable_tex2d(f->a, f->b); break;
        case RTGX_F_TEXMTX: rti_glc_tex_matrix(f->a, f->x, f->y); break;
        case RTGX_F_TEXMTX_FOLD: rti_glc_tex_matrix_fold(f->a, f->x, f->y, f->z); break;
        case RTGX_F_VP_DISABLE: rti_gx_vprog_disable(); break;
        case RTGX_F_VP_FORGET: rti_gx_vprog_forget_bound(); break;
        default: break;
    }
}
void rtgx_fwd(int op, int a, int b, float x, float y, float z) {
    FwdArgs f;
    f.op = op;
    f.a = a;
    f.b = b;
    f.x = x;
    f.y = y;
    f.z = z;
    st_fwd++;
    rt_call(fwd_fn, &f, sizeof(f), 0);
}
void rtgx_unexpected(const char* who) {
    static const char* seen[32];
    static int nseen;
    int i;
    st_unexpected++;
    for (i = 0; i < nseen; i++) {
        if (seen[i] == who) {
            return;
        }
    }
    if (nseen < 32) {
        seen[nseen++] = who;
    }
    port_log("*** rtgx: %s on the game thread while the render thread owns the shadow -- a bug\n",
             who);
}

/* ---- the ownership --------------------------------------------------------- */
static void take_fn(void* p) {
    (void)p;
    rti_glc_invalidate(); /* the game thread's instance moved GL: forget everything */
    rti_gx_vprog_forget_vbo();
}
static void take_rt(void) {
    rtgx_owner_rt = 1;
    rt_call(take_fn, NULL, 0, 0);
    st_to_rt++;
}
static void take_game(void) {
    rtgx_owner_rt = 0;
    glc_invalidate(); /* the render thread's instance moved GL: forget everything */
    gx_vprog_forget_vbo();
    st_to_game++;
}
/* a batch about to be translated: 1 = the render thread's (the shadow is
 * handed over first if need be), 0 = the game thread's (likewise taken back) */
int gx_rtgx_batch(void) {
    u8 c, m;
    int want = frame_on && gx_tfs_layout(0, &c, &m) < 0;
    if (want && !rtgx_owner_rt) {
        take_rt();
    } else if (!want && rtgx_owner_rt) {
        take_game();
    }
    if (want) {
        st_rt_batches++;
    } else {
        st_game_batches++;
    }
    return want;
}
/* an ownership that must end: the render thread stopping, a snapshot */
void gx_rtgx_release(void) {
    if (rtgx_owner_rt) {
        take_game();
    }
    frame_on = 0;
}

/* ---- the record -------------------------------------------------------------- */
typedef struct RecHdr {
    GxXfDesc xfd;
    f32 posm[12], nrmm[9];
    f32 tgm[GX_TEXCOORDS][12];
    u8 tg_has[GX_TEXCOORDS];
    const u8* ob;
    int out_stride, out_off_clr, out_off_tex, out_ntex;
    int on_gpu, csum, live, unitmemo;
    int hilite_stage, hilite_mode, force_flags;
    u32 vp_size, nchunks;
} RecHdr;
#define REC_MAX 16384
static u8 recbuf[REC_MAX] __attribute__((aligned(8)));
static u32 reclen;

static inline void chunk(size_t off, size_t len) {
    const u8* a = (const u8*)&gx + off;
    u8* b = (u8*)&gx_sent + off;
    if (memcmp(a, b, len) != 0) {
        u8* q = recbuf + reclen;
        u32 need = 4 + (u32)((len + 3) & ~(size_t)3);
        if (reclen + need > REC_MAX) {
            port_fatal("rtgx: a record over %u bytes", (unsigned)REC_MAX);
        }
        ((u16*)q)[0] = (u16)off;
        ((u16*)q)[1] = (u16)len;
        memcpy(q + 4, a, len);
        memcpy(b, a, len);
        reclen += need;
        ((RecHdr*)recbuf)->nchunks++;
        st_chunks++;
    }
}
#define CH(field) chunk(offsetof(GXState, field), sizeof(gx.field))
#define CHR(first, last) \
    chunk(offsetof(GXState, first), offsetof(GXState, last) + sizeof(gx.last) - offsetof(GXState, first))

/* everything the render thread's instance reads of GXState: the raster
 * state and the projection, the channels and the lights they name, the TEV
 * stages in use (and one past: the three-texture fold reads it), their
 * indirect state, the pixel state, and the texture objects the stages and
 * the indirect stages name */
static void deltas(void) {
    int i, stages = gx.num_tev ? gx.num_tev : 1;
    u32 lmask;
    CHR(proj, line_width);
    CH(num_chans);
    CH(chan);
    lmask = gx.chan[0].light_mask | gx.chan[1].light_mask | gx.chan[2].light_mask |
            gx.chan[3].light_mask;
    for (i = 0; i < 8; i++) {
        if (lmask & (1u << i)) {
            CH(light[i]);
        }
    }
    CH(num_texgens);
    CH(num_tev);
    if (stages < GX_TEV_STAGES) {
        stages++;
    }
    chunk(offsetof(GXState, tev), sizeof(gx.tev[0]) * (size_t)stages);
    chunk(offsetof(GXState, ind_tile), sizeof(gx.ind_tile[0]) * (size_t)stages);
    chunk(offsetof(GXState, ind_warp), sizeof(gx.ind_warp[0]) * (size_t)stages);
    CH(tev_reg);
    CH(kcolor);
    CH(swap_tbl);
    CH(num_ind);
    if (gx.num_ind) {
        CH(ind);
        CH(ind_mtx);
    }
    CHR(z_enable, fog_color);
    for (i = 0; i < stages; i++) {
        unsigned m = gx.tev[i].map;
        if (m < GX_TEX_UNITS) {
            CH(bound[m]);
        }
    }
    for (i = 0; i < gx.num_ind && i < 4; i++) {
        unsigned m = gx.ind[i].map;
        if (m < GX_TEX_UNITS) {
            CH(bound[m]);
        }
    }
}

static void apply_fn(void* p) {
    const RecHdr* h = (const RecHdr*)p;
    const u8* q = (const u8*)(h + 1);
    GxXfDesc x;
    u32 k;
    int i;
    double t0 = port_now_seconds();
    const u8* vp = q;
    q += (h->vp_size + 7u) & ~7u;
    for (k = 0; k < h->nchunks; k++) {
        u32 off = ((const u16*)q)[0], len = ((const u16*)q)[1];
        memcpy((u8*)&gx_rti + off, q + 4, len);
        q += 4 + ((len + 3u) & ~3u);
    }
    rti_gx_hilite_stage = h->hilite_stage;
    rti_gx_hilite_mode = h->hilite_mode;
    rti_gx_force_flags = h->force_flags;
    rti_live = h->live;
    x = h->xfd;
    x.pos_mtx = h->posm;
    x.nrm_mtx = h->nrmm;
    for (i = 0; i < GX_TEXCOORDS; i++) {
        x.tg[i].mtx = h->tg_has[i] ? h->tgm[i] : NULL;
    }
    if (h->unitmemo) {
        rti_gx_unit_memo(1);
    }
    if (!h->on_gpu) {
        rti_gx_vprog_disable();
    }
    rti_gl13_apply_transform();
    rti_gl13_apply_raster_state();
    rti_gx_tev_apply();
    rti_glc_color_sum(h->csum);
    if (h->on_gpu) {
        rti_gx_vprog_pending_set(vp);
        rti_gx_vprog_bind(&x);
    } else {
        const u8* ob = h->ob;
        rti_glc_vertex_array(ob, h->out_stride);
        rti_glc_color_array(ob + h->out_off_clr, h->out_stride);
        rti_glc_normal_array(NULL, 0);
        for (i = 0; i < gl13_max_tex_units; i++) {
            u8 coord = 0, map = 0;
            if (rti_gx_tev_unit_source(i, &coord, &map) && coord < h->out_ntex &&
                rti_gx_bound_tex(map) != NULL) {
                rti_glc_coord_array(i, ob + h->out_off_tex + 8 * coord, h->out_stride);
            } else {
                rti_glc_coord_array(i, NULL, 0);
            }
        }
    }
    if (h->unitmemo) {
        rti_gx_unit_memo(0);
    }
    gx_rtgx_rt_time(port_now_seconds() - t0);
}

/* the batch's state, recorded (the game thread; gx_draw.c draw_apply) */
void gx_rtgx_record(const GxXfDesc* xfd, int on_gpu, int csum, const u8* ob, int out_stride,
                    int out_off_clr, int out_off_tex, int out_ntex) {
    RecHdr* h = (RecHdr*)recbuf;
    size_t vps = on_gpu ? gx_vprog_pending_size() : 0;
    int i;
    memset(h, 0, sizeof(*h));
    h->xfd = *xfd;
    memcpy(h->posm, xfd->pos_mtx, sizeof(h->posm));
    if (xfd->nrm_mtx) {
        memcpy(h->nrmm, xfd->nrm_mtx, sizeof(h->nrmm));
    }
    for (i = 0; i < xfd->ntexgen && i < GX_TEXCOORDS; i++) {
        if (xfd->tg[i].mtx) {
            memcpy(h->tgm[i], xfd->tg[i].mtx, sizeof(h->tgm[i]));
            h->tg_has[i] = 1;
        }
    }
    h->xfd.pal = NULL; /* never with the palette (gx_rtgx_frame_begin) */
    h->ob = ob;
    h->out_stride = out_stride;
    h->out_off_clr = out_off_clr;
    h->out_off_tex = out_off_tex;
    h->out_ntex = out_ntex;
    h->on_gpu = on_gpu;
    h->csum = csum;
    h->live = gl13_live();
    h->unitmemo = !port_opt.nounitmemo;
    h->hilite_stage = gx_hilite_stage;
    h->hilite_mode = gx_hilite_mode;
    h->force_flags = gx_force_flags;
    h->vp_size = (u32)vps;
    reclen = sizeof(RecHdr);
    if (vps) {
        gx_vprog_pending_get(recbuf + reclen);
        reclen += (u32)((vps + 7u) & ~7u);
    }
    deltas();
    st_rec_bytes += reclen;
    rt_call(apply_fn, recbuf, reclen, 0);
}

/* ---- the z pre-pass (gx_draw.c draw_issue) ------------------------------------ */
static int zp_on; /* the render thread's */
static void zp_begin_fn(void* p) {
    memcpy(rti_gx_unit_alpha_min, p, 8);
    if (rti_gl13_zprepass_wanted()) {
        rti_gl13_zprepass_begin();
        zp_on = 1;
    } else {
        rt_set_skip_draws(1); /* declined: the pre-pass's draws are not made */
        zp_on = 0;
    }
}
static void zp_end_fn(void* p) {
    (void)p;
    if (zp_on) {
        rti_gl13_zprepass_end();
    } else {
        rt_set_skip_draws(0);
    }
    zp_on = 0;
}
/* could this draw want a pre-pass at all (gl13_zprepass_wanted's own first
 * test, of the game's state): only then are the pre-pass's draws recorded,
 * between the two calls that let the render thread decide */
int gx_rtgx_zp_maybe(void) {
    return port_opt.zprepass && gx.z_enable && gx.z_update &&
           (port_opt.zprepass != 1 || gx.z_comploc);
}
void gx_rtgx_zp_begin(void) {
    rt_call(zp_begin_fn, gx_unit_alpha_min, 8, 0);
    st_zp++;
}
void gx_rtgx_zp_end(void) { rt_call(zp_end_fn, NULL, 0, 0); }

/* ---- the frame: on or off, and the auto balance ------------------------------------ */
static double rt_frame_s;          /* the render thread's apply time this frame */
void gx_rtgx_rt_time(double s) { rt_frame_s += s; }
void gx_rtgx_rt_present(void) {    /* the render thread, at a present */
    tr_last_ms = rt_frame_s * 1000.0;
    rt_frame_s = 0.0;
}
/* the game thread's translation time, timed by draw_apply when it owns a batch */
void gx_rtgx_game_time(double s) { tg_frame_s += s; }

int gx_rtgx_frame_on(void) { return frame_on; }

void gx_rtgx_frame_begin(void) {
    int m = rtgx_mode();
    int want = 0;
    double g = 0, r = 0, d = 0, rate = 0;
    /* last frame's costs, each side's own */
    if (frame_on) {
        if (tr_last_ms > 0.0) {
            tr_ms = tr_ms > 0.0 ? 0.75 * tr_ms + 0.25 * tr_last_ms : tr_last_ms;
        }
    } else if (tg_frame_s > 0.0) {
        double ms = tg_frame_s * 1000.0;
        tg_ms = tg_ms > 0.0 ? 0.75 * tg_ms + 0.25 * ms : ms;
    }
    tg_frame_s = 0.0;
    if (m && rt_threaded() && !port_opt.palette && !port_opt.premerge_max && !port_opt.drawlog &&
        !port_opt.probeobj && !port_opt.segrebase && !port_opt.submitstats &&
        !port_opt.decodestats && !port_opt.lazyflush) {
        if (m == 1) {
            want = 1;
        } else if (m == 3) {
            want = !frame_on; /* the hand-over test: every other drawn frame */
        } else if (rt_vcache_inputs(&r, &d, &rate, &g)) {
            /* auto: g is the game thread's cycle (drawn + consumed), r + d the
             * render thread's; the translation moves t from one to the other.
             * On while the game thread stays the longer pole with t moved;
             * off once the render thread is the pole by more than t. */
            double R = r + d;
            double t_on = tr_ms > 0.0 ? tr_ms : tg_ms;
            double t_off = tg_ms > 0.0 ? tg_ms : tr_ms;
            int cond;
            if (!frame_on) {
                cond = g > port_opt.rtgx_fit && g > R + t_on + 1.0;
            } else {
                cond = !(R > g + t_off + 1.0) && g + t_off > port_opt.rtgx_fit - 3.0;
            }
            /* two frames in a row before a change */
            if (cond != frame_on) {
                if (++au_streak >= 2) {
                    want = cond;
                    au_streak = 0;
                } else {
                    want = frame_on;
                }
            } else {
                au_streak = 0;
                want = frame_on;
            }
        }
    }
    frame_on = want;
    if (want) {
        st_frames_on++;
    } else {
        st_frames_off++;
    }
}

void gx_rtgx_report(void) {
    unsigned e = 0, l = 0;
    if (!rtgx_mode() && !st_rt_batches) {
        return;
    }
    port_log("port> rtgx (M43, --rtgx %s): %lu drawn frames on, %lu off; batches %lu translated "
             "on the render thread, %lu on the game thread; %lu hand-overs to the render thread, "
             "%lu back\n",
             rtgx_mode() == 3 ? "3 (alternate)" : rtgx_mode() == 2 ? "auto" : rtgx_mode() == 1 ? "1" : "0", st_frames_on,
             st_frames_off, st_rt_batches, st_game_batches, st_to_rt, st_to_game);
    port_log("port> rtgx: %.1f MB of records (%.0f B a batch), %lu state chunks; %lu shadow calls "
             "forwarded; %lu z pre-pass decisions left to the render thread; translation "
             "%.2f ms a drawn frame on the game thread, %.2f on the render thread (the auto's "
             "last estimates)\n",
             st_rec_bytes / 1048576.0, st_rt_batches ? st_rec_bytes / (double)st_rt_batches : 0.0,
             st_chunks, st_fwd, st_zp, tg_ms, tr_ms);
    if (st_unexpected) {
        port_log("*** rtgx: %lu call(s) into the shadow the render thread owned -- a bug\n",
                 st_unexpected);
    }
    rti_glc_stats(&e, &l);
    port_log("port> rtgx: the render thread's shadow emitted %u GL state calls, elided %u\n", e, l);
}
