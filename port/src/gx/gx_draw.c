/* GXBegin..GXEnd: vertex decode, display lists, and the draw.
 *
 * Under `TARGET_PC` -- the switch this port builds with -- the SDK's
 * `GXPosition*`/`GXColor*`/`GXTexCoord*` writers are ordinary function calls
 * rather than stores into the write-gather pipe at 0xCC008000, so this file
 * receives the vertex stream directly.  Three things happen here.
 *
 * **Assembly.** A vertex is complete when the last attribute the current
 * vertex descriptor names has been written, which is well defined because GX
 * requires attributes to be written in descriptor order.  Direct attributes
 * carry their value; indexed ones (`GX_INDEX8`/`GX_INDEX16`, 150 of the game's
 * 260 descriptor sites) carry an index into the array `GXSetArray` last named,
 * and are fetched and converted here against the vertex-attribute table's
 * component count, type and fractional shift.
 *
 * **Transform, on the CPU.** Positions go through the loaded position matrix
 * and normals through the loaded normal matrix before they reach GL, and GL's
 * modelview stays identity.  That is not laziness: the game loads normal
 * matrices that are *not* the inverse transpose of the position matrix, which
 * is the one thing GL's fixed function will not let you say.  Per-vertex
 * lighting is done here too, for the same reason -- GX's `GX_DF_CLAMP` /
 * `GX_DF_SIGN` diffuse functions and its ratio-of-quadratics attenuation are
 * not GL's, and doing it in C costs a party game nothing.  There are no
 * per-vertex matrix indices anywhere in this game (PLAN.md §1.14), so there is
 * exactly one position matrix in play per draw.
 *
 * **Display lists.** `GXBeginDisplayList` puts the writers into record mode
 * and they append to the caller's buffer in the **real GX byte encoding** --
 * a one-byte opcode with the vertex format in its low three bits, a
 * big-endian u16 count, then packed attributes in descriptor order.  Encoding
 * it exactly rather than inventing a record format matters because the game
 * slices its display-list buffer by the size we return (`DrawData[].dlOfs`,
 * capped at 0x20000 in hsfdraw.c): a fatter encoding would overrun a buffer
 * the game sized for the console and show up as corruption a hundred frames
 * later.  `GXCallDisplayList` parses the stream back against the *current*
 * descriptor state, which is what the hardware did and what the game assumes
 * -- every one of its 42 call sites sets the state immediately before.
 */
#include <dlfcn.h>

#include "gx_internal.h"
#include "gx_math.h"
#include "gx_skin.h"

unsigned gl13_frame_number(void);

#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
#include "gx_rt.h" /* M27: the render thread's twins */
#endif

#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

/* ---- the vertex buffer ---------------------------------------------------- */

/* ---- two compact vertex layouts, and why the 96-byte one is gone -----------
 *
 * M5 built the batched AltiVec transform, measured it, and it bought nothing
 * (PLAN.md 15.6).  The reading it left behind is the one this file now acts
 * on: the vertex path is **memory bound**, not arithmetic bound.  A `Vtx` was
 * 96 bytes -- three floats of position, three of normal, *two* RGBA colours
 * and **eight** texcoord slots -- and 329 million vertices a run over a 133
 * MHz bus is about 31 GB of traffic.  Vectorising a memory-bound loop is free
 * and worth nothing, which is exactly what the measurement said.
 *
 * So there is no `Vtx` any more.  There are two layouts, both packed to
 * exactly what the primitive in hand uses:
 *
 *   **source** -- what the decode produces and the display-list cache stores:
 *   the *model-space* position, the model-space normal when the descriptor
 *   has one, the vertex colour, and the raw texcoords a texgen will read
 *   back.  `pos` 12 + `nrm` 0 or 12 + `clr0` 4 + 8 per copied texcoord.
 *
 *   **output** -- what GL's client arrays read: the transformed position, the
 *   final colour, and the generated texcoords.  16 + 8 per texcoord slot.
 *
 * A board vertex is typically 36 bytes of source and 24 of output against the
 * old 96 of both, which is the change the profile asked for.
 *
 * Two fields are simply gone rather than packed.  `clr1` was written by every
 * vertex and read by nobody: GL has one primary colour, `gx_tev.c` only ever
 * names `GL_PRIMARY_COLOR`, and the CPU lighting only runs channel 0.  And
 * the normal never reaches GL at all -- lighting is done here (see the file
 * header) -- so in the output layout it is a register, not a field.
 *
 * **The AltiVec path went with it.**  It was switched off by default because
 * it was not faster, and every one of its loads, stores and permutes assumed
 * `sizeof(Vtx) == 96` with the position quadword-aligned.  Keeping a dead
 * fast path that encodes a layout the port no longer has would be a lie in
 * the source; M9's answer to the same question is the layout itself. */
typedef struct Layout {
    int stride;
    int off_nrm; /* -1 when the descriptor has no normal */
    int off_clr;
    int off_skin; /* -1 without the matrix palette (M18); else one float,
                   * the vertex's palette slot times GX_PAL_STRIDE */
    int off_tex;
    int ntex;
} Layout;

#define MAX_VERTS 65536
#define SRC_MAX_STRIDE (12 + 12 + 4 + 4 + 8 * GX_TEXCOORDS)
#define OUT_MAX_STRIDE (16 + 8 * GX_TEXCOORDS)

/* The source vertices go into a ring (M16, PLAN.md 31).  Before M16 `src_buf`
 * was a static buffer every primitive and every list wrote from index zero,
 * which was fine while `glDrawArrays` copied the data out synchronously.  Now
 * the ring is a `GL_APPLE_vertex_array_range` the card reads by DMA *after*
 * the call returns, so a run is never overwritten until its chunk's fence
 * says the card is done with it -- gl13.c owns the fences, this file owns
 * the cursor.  Without the extension (or with --novar / --oldsubmit) it is
 * the same ring in ordinary memory and the driver copies as before.
 *
 * `out_buf` is only the CPU path's (--cpuxf) output and is not in the ring. */
#define VRING_BYTES ((size_t)8 << 20)
static u8* src_buf;            /* the ring, from gl13_var_setup              */
static size_t src_cap;         /* its size in bytes                          */
static size_t ring_cursor;     /* the next free byte                         */
static size_t fence_from;      /* where the writer stood at the last flush   */
static int ring_wrapped;       /* the writer wrapped since the last flush    */
static u8 out_buf[MAX_VERTS * OUT_MAX_STRIDE] __attribute__((aligned(16)));
/* Where a vertex goes when the run has already filled the ring: it is
 * decoded and thrown away, so the list still steps by the right number of
 * bytes.  The old path dropped it out of transform_and_store(). */
static u8 sink_vtx[SRC_MAX_STRIDE] __attribute__((aligned(16)));
static Layout sl;              /* the source layout of the primitive in hand  */
static int out_stride, out_off_clr, out_off_tex, out_ntex;
static int nverts;             /* vertices in the primitive being assembled   */
static u32 sv_first;           /* the primitive's first vertex, list-relative */
static size_t run_pos;         /* byte offset in the ring of the run in hand  */

/* The writers still assemble into one uncompressed staging vertex: they arrive
 * one attribute at a time and in descriptor order, so there is nothing to pack
 * until the vertex is complete.  It is one vertex, not 65,536, so its size
 * costs nothing. */
typedef struct Pending {
    float pos[3];
    float nrm[3];
    unsigned char clr[2][4];
    float tex[GX_TEXCOORDS][2];
} Pending;
static Pending pending;
static int active[GX_MAX_ATTR]; /* attributes in descriptor order */
static int nactive;
static int acur;                /* which of them the next writer fills */
static int tex_slots;
static int in_prim;

/* ---- what a primitive settles once, rather than once a vertex -------------
 *
 * M3's profile put `transform_and_store` at 1,727 of about 4,300 in-thread
 * samples on the G4 and §13.12 named the reason: the function re-derived, for
 * every single vertex, things that cannot change between `GXBegin` and
 * `GXEnd` -- which matrix slot is current, whether the colour channel is lit
 * at all, which texgen reads what through which matrix, how wide the texcoord
 * array has to be.  None of that is arithmetic the frame needs; it is
 * bookkeeping, paid 250,000 times a frame.
 *
 * There is one thing to be careful about and it is worth naming: this is only
 * sound because a GX primitive cannot change any of it mid-stream.  The
 * matrix index is a per-*vertex* attribute on real hardware (`GX_VA_PNMTXIDX`)
 * and a display list may carry XF register writes -- but Mario Party 4 uses
 * neither (PLAN.md §3.2), the port asserts as much by warning on the
 * descriptor, and `GXSetCurrentMtx` is a state call the game only makes
 * between primitives.  If that ever stops being true the symptom is a whole
 * primitive drawn with the previous primitive's matrix, which is loud. */
typedef struct PrimInv {
    const f32* pos_mtx;
    const f32* nrm_mtx;
    int have_nrm;
    int no_clr0;         /* CLR0 absent: the register material is the colour  */
    /* 0: the channel writes nothing;  1: it splats the register material;
     * 2: it is genuinely lit and light_channel has to run.  Case 2 is rare on
     * the menus and case 0/1 was paying for a function call and eleven struct
     * loads to do nothing. */
    int chan_mode;
    int ntexgen;
    /* How many raw texcoord slots phase 1 has to carry into the vertex for
     * phase 2 to read back: one past the highest texgen source. */
    int tex_copy_n;
    struct {
        u8 src_kind;     /* 0 = a texcoord, 1 = position, 2 = normal          */
        u8 src_k;        /* which texcoord, when src_kind is 0                */
        u8 divide;       /* GX_TG_MTX3x4: divide by q                         */
        u8 mtx_slot;     /* M22: gx.tex_mtx[] slot, resolved at submit time   */
        const f32* mtx;  /* NULL for the identity                             */
    } tg[GX_TEXCOORDS];
    /* the indexed reader's per-attribute constants */
    const GXArraySpec* arr[GX_MAX_ATTR];
    const GXVatFmt* vat[GX_MAX_ATTR];
    /* M18, the matrix palette (PLAN.md 33): every vertex names its slot.  A
     * plain primitive's vertices all name `slotf`; a skinned mesh's name
     * `mesh->ent_slotf[mesh->pos_ent[index]]`, the batch slot of the entry
     * their rest position is bound to. */
    SkinMesh* skin;
    f32 slotf;
} PrimInv;
/* M22: two of them, and `pi` is whichever the primitive in hand is settled
 * into.  A batch keeps the buffer its first primitive used (bctx_pi) and the
 * next primitive is settled into the other, so a deferred submit reads the
 * batch's own without a copy (batch_flush, batch_apply_now). */
static PrimInv pi_bufs[2];
static PrimInv* pi_cur = &pi_bufs[0];
static PrimInv* bctx_pi = &pi_bufs[1];
#define pi (*pi_cur)

/* M21: the hilite fold (gx_internal.h).  Decided per primitive from the
 * channel and TEV state, which a batch shares (every setter that could
 * change it ends the batch). */
int gx_hilite_stage = -1;
int gx_hilite_mode = 0;
static unsigned stat_hilite_prims, stat_hilite_unfoldable, stat_hilite_textured;

static int swap_is_identity(const u8* t) {
    return t[0] == GX_CH_RED && t[1] == GX_CH_GREEN && t[2] == GX_CH_BLUE && t[3] == GX_CH_ALPHA;
}

int gx_hilite_decide(void) {
    int k, stages = gx.num_tev;
    gx_hilite_stage = -1;
    gx_hilite_mode = 0;
    if (port_opt.nohilite || gx.num_chans < 2 || !gx.chan[GX_COLOR1].enable ||
        gx.chan[GX_COLOR1].attn_fn != GX_AF_SPEC) {
        return -1;
    }
    for (k = 0; k < stages && k < GX_TEV_STAGES; k++) {
        const GXTevStage* t = &gx.tev[k];
        if (t->chan != GX_COLOR1A1) {
            continue;
        }
        if (t->cin[0] == GX_CC_CPREV && t->cin[1] == GX_CC_ONE && t->cin[2] == GX_CC_RASC &&
            t->cin[3] == GX_CC_ZERO && t->cop == GX_TEV_ADD && t->cbias == GX_TB_ZERO &&
            t->cscale == GX_CS_SCALE_1 && t->creg == GX_TEVPREV) {
            /* hsfdraw.c:827 / 1370: the screen.  Exact when everything
             * before it is linear in RAS0 and nothing follows -- which is
             * the material setup's shape; a stage after it (a projection
             * map) would see the sum added after it and is counted. */
            gx_hilite_stage = k;
            gx_hilite_mode = 1;
            stat_hilite_prims++;
            if (k + 1 < stages) {
                stat_hilite_unfoldable++;
                gx_warn("TEV: a hilite stage with stages after it; the specular "
                        "sum lands after them");
            }
            return k;
        }
        /* hsfdraw.c:1374: TEXC * RASC1 + CPREV, the textured highlight (M22,
         * gx_internal.h): spec rides the primary colour's alpha, so it must
         * be the last stage, sample a texture, and no stage may read the
         * rasterised alpha or route it through a swap table */
        if (t->cin[0] == GX_CC_ZERO && t->cin[1] == GX_CC_TEXC && t->cin[2] == GX_CC_RASC &&
            t->cin[3] == GX_CC_CPREV && t->cop == GX_TEV_ADD && t->cbias == GX_TB_ZERO &&
            t->cscale == GX_CS_SCALE_1 && t->creg == GX_TEVPREV && k + 1 == stages &&
            !port_opt.nohilitetex && gl13_have_combine3 && gx_bound_tex(t->map) != NULL &&
            t->coord < GX_TEXCOORDS) {
            int j, ok = 1;
            for (j = 0; j < stages && ok; j++) {
                const GXTevStage* u = &gx.tev[j];
                int q;
                for (q = 0; q < 4; q++) {
                    if (u->cin[q] == GX_CC_RASA || u->ain[q] == GX_CA_RASA) {
                        ok = 0;
                    }
                }
                if (!swap_is_identity(gx.swap_tbl[u->ras_swap & 3]) ||
                    !swap_is_identity(gx.swap_tbl[u->tex_swap & 3])) {
                    ok = 0; /* the identity tables only; the emitter picks operands */
                }
            }
            if (ok) {
                gx_hilite_stage = k;
                gx_hilite_mode = 2;
                stat_hilite_prims++;
                stat_hilite_textured++;
                return k;
            }
        }
        stat_hilite_unfoldable++;
        gx_warn("TEV: a textured hilite stage (TEXC * RASC1 + CPREV) the alpha "
                "fold does not cover; drawn from channel 0 as before");
        return -1;
    }
    return -1;
}
static void batch_prepare(u32 count);
static void pal_place(void);
int gx_palette_active;     /* the batches carry a matrix palette (M18)      */
#define palette_on gx_palette_active
int gx_batch_spans;        /* M18/M22: matrix loads and descriptor setters end
                            * no batch (gx_state.c); settled with the palette */
static int premerge_on;    /* M22: the CPU pre-transform (PLAN.md 37)      */
static int lazy_on;        /* M22: the lazy flush (gx_internal.h)           */
static int pal_slots;      /* its capacity, from gx_vprog.c                  */
#define PAL_SLOTS_MAX GX_PAL_SLOTS_MAX
static f32 pal[PAL_SLOTS_MAX][GX_PAL_STRIDE][4]; /* the pending batch's palette */
static int pal_n;          /* slots used in the pending batch                */
static unsigned pal_batch_serial; /* bumped by pal_reset: the window's key    */
static void pal_reset(void);
typedef struct PalSlot {
    u8 kind;               /* 0 free, 1 plain object, 2 skinned entry */
    unsigned pinned;       /* == pal_batch_serial when the pending batch uses it */
    unsigned used;         /* LRU stamp */
    SkinMesh* mesh;
    int ent;
    unsigned pose;
    f32 posm[12];
    f32 nrmm[9];
} PalSlot;
static PalSlot pal_slot[PAL_SLOTS_MAX];
static unsigned pal_clock;
static int pal_dirty_lo, pal_dirty_hi; /* rows to upload for the pending batch */


static u8 prim;
static u8 vtxfmt;
static u16 want_verts;

/* (float)b / 255.0f for every byte value, built once with the real division
 * so the table is bit-for-bit what the divide produced.  It removes two
 * expensive things at a stroke: an `fdivs`, which on a 7450 is 14-21 cycles
 * and does not pipeline, and the integer-to-float conversion, which on
 * PowerPC before ISA 2.06 is a store-load-subtract dance.  CPU lighting does
 * up to thirteen of these per vertex -- four for the material, three for the
 * ambient and three per light -- and after the square roots went, they were
 * what was left. */
static float byte_scale[256];

static unsigned stat_prims, stat_verts, stat_draws, stat_dls;
static unsigned long stat_fast_verts; /* through the specialised loops (M17) */
static unsigned long stat_rtdec_runs, stat_rtdec_verts, stat_rtdec_refused; /* M29 */
static unsigned long stat_rtdec_here; /* M33: auto's runs decoded on the game thread */
static unsigned long stat_zprepass;   /* M33: draws issued twice for the Z of alpha-killed fragments */
int gl13_zprepass_wanted(void); void gl13_zprepass_begin(void); void gl13_zprepass_end(void);
static int rt_auto_on;                /* M33: --rtdecode auto, cached per list */
/* --submitstats (M16): what the batching actually found in the lists */
static unsigned stat_indexed_batches, stat_indexed_tris, stat_indexed_u32; /* M21 */
static unsigned stat_mergeable, stat_mergeable_small, stat_mergeable_verts; /* M21 */
static unsigned stat_mergeable_atlas, stat_mergeable_atlas_small, stat_mergeable_atlas_verts; /* M23 */
static unsigned stat_pm_objects, stat_pm_prims, stat_pm_verts;              /* M22 */
static unsigned stat_pm_refused_big, stat_pm_refused_singular, stat_pm_refused_wrap;
static unsigned stat_pm_hist[4]; /* vertices merged per source object: <=4, <=8, <=16, more */
static unsigned stat_lazy_snaps, stat_lazy_transient, stat_lazy_flushes;    /* M22 */
static unsigned stat_batch_hist[6]; /* vertices per batch: <=16, <=64, <=256, <=1024, <=4096, more */
static unsigned stat_fixbase_batches, stat_fixbase_miss;                   /* M21 */
static unsigned stat_batches, stat_merged, stat_multi_calls, stat_multi_prims,
    stat_wraps, stat_lists_drawn, stat_late_flush;
static unsigned stat_pal_flushes, stat_pal_plain, stat_pal_skin, stat_pal_reused;
static unsigned stat_pal_overflow, stat_pal_fills, stat_pal_scan_hist[8];
static unsigned stat_list_hist[5]; /* primitives per drawn list: 1, 2-4, 5-16, 17-64, 65+ */
#define FLUSHER_SLOTS 32
static struct {
    const char* who;
    unsigned n;
    unsigned transient; /* M22: ...and the batch after was of the same state */
} flushers[FLUSHER_SLOTS];         /* which state setters ended batches      */
static int last_flusher = -1;      /* the slot that ended the previous batch */
static int pending_flusher = -1;   /* the slot ending the batch being flushed  */

/* M37 (PLAN.md 52): --endlog F names every batch end of one drawn frame --
 * who ended it, and a hash of exactly the state the submit read (the
 * SubmitRec of M22), of the layout and of the batch's own matrices.  Two
 * consecutive batches whose three hashes agree could have been one batch
 * with the same draws in the same order: that is the "batch end the state
 * did not need" of PLAN.md 51.5, and the log says which setter made it. */
static const char* end_who = "?";
static u32 endlog_fnv(const void* p, size_t n, u32 h) {
    const u8* b = (const u8*)p;
    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}
static int endlog_armed(void) {
    const char* p = port_opt.endlog;
    unsigned f = gl13_frame_number() + 1;
    if (!p) {
        return 0;
    }
    while (*p) {
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p++ - '0');
        }
        if (v == f) {
            return 1;
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}
/* Draws whose position matrix puts the object sideways off the world.
 *
 * M4 had to bolt a throwaway instrument on to count these (1,264 of 1,756 on
 * the character select); it is two compares per primitive, so it stays.  Zero
 * is the answer PLAN.md 15.1's fix is judged by.
 *
 * **x and y only, not z.**  The modelview's z translation is the camera
 * distance, and the board's camera sits 14,000 units back with a far plane at
 * 23,000 -- so on Toad's Midway Madness a z of -15,000 is an ordinary distant
 * space, and counting it flagged 498,060 perfectly good primitives.  Nothing
 * in this game is ever ten thousand units to the side of its own camera, which
 * is what the bug did and what this counts. */
#define GX_OFFWORLD_LIMIT 10000.0f
static unsigned stat_offworld;
static int offworld_named;
static int dl_shown;

void gx_draw_reset(void) {
    int i;
    nverts = 0;
    in_prim = 0;
    nactive = 0;
    for (i = 0; i < 256; i++) {
        byte_scale[i] = (float)i / 255.0f;
    }
}

static void dlc_report(void);

void gx_draw_report(void) {
    if (!stat_prims) {
        return;
    }
    port_log("port> GX draw: %u primitives, %u vertices, %u glDrawArrays, "
             "%u display lists replayed\n",
             stat_prims, stat_verts, stat_draws, stat_dls);
    port_log("port> GX draw: %lu vertices through the specialised decode loops "
             "(%.1f%%)\n",
             stat_fast_verts, stat_verts ? 100.0 * (double)stat_fast_verts / stat_verts : 0.0);
    if (stat_rtdec_runs || stat_rtdec_refused || stat_rtdec_here) {
        port_log("port> GX draw: M29: %lu display-list runs (%lu vertices, %.1f%%) handed to the "
                 "render thread's decode, %lu runs decoded here instead, %lu kept here by "
                 "--rtdecode auto (M33)\n",
                 stat_rtdec_runs, stat_rtdec_verts,
                 stat_verts ? 100.0 * (double)stat_rtdec_verts / stat_verts : 0.0,
                 stat_rtdec_refused, stat_rtdec_here);
    }
    if (stat_zprepass) {
        port_log("port> GX draw: M33: %lu draw(s) issued twice, depth first, for the Z of "
                 "alpha-killed fragments (--zprepass %d)\n", stat_zprepass, port_opt.zprepass);
    }
    port_log("port> GX draw: %u primitive(s) off-world (|position matrix "
             "translation| over %.0f)\n",
             stat_offworld, (double)GX_OFFWORLD_LIMIT);
    if (port_opt.submitstats) {
        unsigned w, b, f, fl;
        gl13_var_stats(&w, &b, &f, &fl);
        port_log("port> submit: %u batches for %u primitives -> %u GL draws "
                 "(%u list primitives merged, %u strips in %u multi-draws), "
                 "%u ring wraps\n",
                 stat_batches, stat_prims, stat_draws, stat_merged, stat_multi_prims,
                 stat_multi_calls, stat_wraps);
        port_log("port> submit: M23 atlas: %u batches differ from the one before in the matrices "
                 "and the textures' identity alone (same format/wrap/filter; %u of them of 16 "
                 "vertices or fewer -- sprites), %u vertices\n",
                 stat_mergeable_atlas, stat_mergeable_atlas_small, stat_mergeable_atlas_verts);
        port_log("port> submit: M21 mergeable: %u batches differ from the one before in the "
                 "matrices alone (%u of them of 256 vertices or fewer), %u vertices; vertices "
                 "per batch <=16: %u  <=64: %u  <=256: %u  <=1024: %u  <=4096: %u  more: %u\n",
                 stat_mergeable, stat_mergeable_small, stat_mergeable_verts, stat_batch_hist[0],
                 stat_batch_hist[1], stat_batch_hist[2], stat_batch_hist[3], stat_batch_hist[4],
                 stat_batch_hist[5]);
        port_log("port> submit: M22 premerge (%s, max %d): %u objects (%u primitives, %u "
                 "vertices) transformed into the pending batch; refused: %u over the "
                 "limit, %u singular, %u ring wraps; vertices per merged object <=4: %u  "
                 "<=8: %u  <=16: %u  more: %u\n",
                 premerge_on ? "on" : "off", port_opt.premerge_max, stat_pm_objects,
                 stat_pm_prims, stat_pm_verts, stat_pm_refused_big, stat_pm_refused_singular,
                 stat_pm_refused_wrap, stat_pm_hist[0], stat_pm_hist[1], stat_pm_hist[2],
                 stat_pm_hist[3]);
        port_log("port> submit: M22 lazy flush (%s): %u batches applied early at a setter, %u "
                 "went on past it (the state came back), %u issued late\n",
                 lazy_on ? "on" : "off", stat_lazy_snaps, stat_lazy_transient, stat_lazy_flushes);
        port_log("port> submit: M21 hilite: %u primitives with the specular channel folded "
                 "(%u of them textured, M22), %u stages the fold does not cover\n",
                 stat_hilite_prims, stat_hilite_textured, stat_hilite_unfoldable);
        port_log("port> submit: M21 indexed: %u batches as one glDrawRangeElements "
                 "(%u triangles, %u with 32-bit indices); fixbase: %u batches based at "
                 "the ring, %u not aligned\n",
                 stat_indexed_batches, stat_indexed_tris, stat_indexed_u32,
                 stat_fixbase_batches, stat_fixbase_miss);
        port_log("port> submit: %u display lists drawn; primitives per list: "
                 "1: %u  2-4: %u  5-16: %u  17-64: %u  65+: %u\n",
                 stat_lists_drawn, stat_list_hist[0], stat_list_hist[1],
                 stat_list_hist[2], stat_list_hist[3], stat_list_hist[4]);
        port_log("port> submit: vertex ring %s: %u fence waits (%u blocked), "
                 "%u fences set, %u range flushes\n",
                 gl13_var_active() ? "on" : "off", w, b, f, fl);
        if (palette_on) {
            port_log("port> submit: batches ended by the palette      %u "
                     "(%u slot fills: %u plain objects placed, %u reused resident; %u "
                     "skinned primitives; %u late flushes, %u entries over the palette)\n",
                     stat_pal_flushes, stat_pal_fills, stat_pal_plain, stat_pal_reused,
                     stat_pal_skin, stat_late_flush, stat_pal_overflow);
            port_log("port> submit: distinct entries per skinned primitive: 1: %u  2: %u  "
                     "3: %u  4: %u  5-8: %u  9-16: %u  17-24: %u  25+: %u\n",
                     stat_pal_scan_hist[0], stat_pal_scan_hist[1], stat_pal_scan_hist[2],
                     stat_pal_scan_hist[3], stat_pal_scan_hist[4], stat_pal_scan_hist[5],
                     stat_pal_scan_hist[6], stat_pal_scan_hist[7]);
        }
        {
            int i;
            for (i = 0; i < FLUSHER_SLOTS && flushers[i].who; i++) {
                port_log("port> submit: batches ended by %-24s %u  (%u transient: the next "
                         "batch's state was the same)\n", flushers[i].who, flushers[i].n,
                         flushers[i].transient);
            }
        }
    }
    dlc_report();
}

/* ---- display-list record mode --------------------------------------------- */

static u8* dl_buf;
static u32 dl_size, dl_pos;
static int dl_recording;
static int dl_replaying;

static void dl_put(const void* p, size_t n) {
    if (dl_pos + n > dl_size) {
        gx_warn("GXBeginDisplayList: the game's buffer is too small for the "
                "recorded list; the tail is dropped");
        dl_pos = dl_size;
        return;
    }
    memcpy(dl_buf + dl_pos, p, n);
    dl_pos += (u32)n;
}

static void dl_u8(u8 v) { dl_put(&v, 1); }
static void dl_u16(u16 v) {
    u8 b[2];
    b[0] = (u8)(v >> 8);
    b[1] = (u8)v;
    dl_put(b, 2);
}
static void dl_u32(u32 v) {
    u8 b[4];
    b[0] = (u8)(v >> 24);
    b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);
    b[3] = (u8)v;
    dl_put(b, 4);
}
static void dl_f32(f32 v) {
    union { f32 f; u32 u; } c;
    c.f = v;
    dl_u32(c.u);
}

void GXBeginDisplayList(void* list, u32 size) {
    dl_buf = (u8*)list;
    dl_size = size;
    dl_pos = 0;
    dl_recording = 1;
}

u32 GXEndDisplayList(void) {
    u32 n;
    /* GX pads a list to a 32-byte boundary with NOPs, and the game's own
     * bookkeeping expects the padded size. */
    while (dl_pos & 0x1F) {
        dl_u8(0);
    }
    n = dl_pos;
    dl_recording = 0;
    dl_buf = NULL;
    return n;
}

/* ---- reading an attribute out of an array --------------------------------- */

/* 1 / (1 << n), exactly.  The fractional shift is a power of two, so the
 * reciprocal is exact and multiplying by it is the same number the divide
 * produced -- at a fifteenth of the cost on a 7450, where `fdivs` is 14-21
 * cycles and does not pipeline.  This reader runs up to eight times per
 * vertex and the profile put it, with `indexed`, at 995 of 4,300 samples. */
const float gx_frac_scale[32] = {
    1.0f / 1.0f,          1.0f / 2.0f,          1.0f / 4.0f,
    1.0f / 8.0f,          1.0f / 16.0f,         1.0f / 32.0f,
    1.0f / 64.0f,         1.0f / 128.0f,        1.0f / 256.0f,
    1.0f / 512.0f,        1.0f / 1024.0f,       1.0f / 2048.0f,
    1.0f / 4096.0f,       1.0f / 8192.0f,       1.0f / 16384.0f,
    1.0f / 32768.0f,      1.0f / 65536.0f,      1.0f / 131072.0f,
    1.0f / 262144.0f,     1.0f / 524288.0f,     1.0f / 1048576.0f,
    1.0f / 2097152.0f,    1.0f / 4194304.0f,    1.0f / 8388608.0f,
    1.0f / 16777216.0f,   1.0f / 33554432.0f,   1.0f / 67108864.0f,
    1.0f / 134217728.0f,  1.0f / 268435456.0f,  1.0f / 536870912.0f,
    1.0f / 1073741824.0f, 1.0f / 2147483648.0f,
};

/* On a big-endian machine the disc's own byte order is the machine's, so a
 * 16- or 32-bit attribute is a load rather than a shift-and-or.  PowerPC
 * handles the unaligned case in hardware.  The portable path stays for the
 * little-endian development host, which is the only place it is needed. */
/* GX_VA_NRM's fixed-point shift is not the VAT's: the hardware ignores the
 * `frac` of GXSetVtxAttrFmt for normals and reads S8 as 1.6 and S16 as 1.14
 * (GX manual, GXSetVtxAttrFmt: "for normals, frac is fixed").  hsfdraw.c sets
 * frac 0 for its S8 normals, so until M26 every raw normal was 64x too long --
 * invisible to the lighting (which normalises) and to nothing else until a
 * GX_TG_NRM texgen read the raw row (PLAN.md 41.4: the title's cake drawn as a
 * 64x-repeated hilite map).  `--nrmfrac0` keeps the VAT's value. */
static u8 nrm_frac(u8 type, u8 frac) {
    if (port_opt.nrmfrac0) {
        return frac;
    }
    return type == GX_S8 ? 6 : type == GX_S16 ? 14 : frac;
}

static f32 read_component(const u8* p, u8 type, u8 frac, int i) {
    const float sc = gx_frac_scale[frac & 31];
    switch (type) {
        case GX_U8: return (f32)p[i] * sc;
        case GX_S8: return (f32)(s8)p[i] * sc;
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        case GX_U16: return (f32)*(const u16*)(p + i * 2) * sc;
        case GX_S16: return (f32)*(const s16*)(p + i * 2) * sc;
        default: return *(const f32*)(p + i * 4);
#else
        case GX_U16: {
            u16 v = (u16)((p[i * 2] << 8) | p[i * 2 + 1]);
            return (f32)v * sc;
        }
        case GX_S16: {
            s16 v = (s16)((p[i * 2] << 8) | p[i * 2 + 1]);
            return (f32)v * sc;
        }
        default: {
            union { f32 f; u32 u; } c;
            const u8* q = p + i * 4;
            c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
            return c.f;
        }
#endif
    }
}

static void read_color(const u8* p, u8 type, unsigned char* out) {
    switch (type) {
        case GX_RGB565: {
            u16 v = (u16)((p[0] << 8) | p[1]);
            out[0] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
            out[1] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
            out[2] = (u8)((v & 0x1F) * 255 / 31);
            out[3] = 255;
            break;
        }
        case GX_RGB8:
        case GX_RGBX8:
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = 255;
            break;
        case GX_RGBA4: {
            u16 v = (u16)((p[0] << 8) | p[1]);
            out[0] = (u8)(((v >> 12) & 0xF) * 17);
            out[1] = (u8)(((v >> 8) & 0xF) * 17);
            out[2] = (u8)(((v >> 4) & 0xF) * 17);
            out[3] = (u8)((v & 0xF) * 17);
            break;
        }
        case GX_RGBA6: {
            u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            out[0] = (u8)(((v >> 18) & 0x3F) * 255 / 63);
            out[1] = (u8)(((v >> 12) & 0x3F) * 255 / 63);
            out[2] = (u8)(((v >> 6) & 0x3F) * 255 / 63);
            out[3] = (u8)((v & 0x3F) * 255 / 63);
            break;
        }
        default: /* GX_RGBA8 */
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = p[3];
            break;
    }
}

static int color_bytes(u8 type) {
    switch (type) {
        case GX_RGB565:
        case GX_RGBA4: return 2;
        case GX_RGB8: return 3;
        case GX_RGBA6: return 3;
        case GX_RGBX8:
        case GX_RGBA8: return 4;
        default: return 4;
    }
}

/* ---- assembling a vertex --------------------------------------------------- */

static void build_decode_plan(void);
static size_t ring_claim(size_t need);

/* Whether the batches carry a matrix palette (M18).  Decided once, at the
 * first primitive, when the vertex-program probe has run: the ARL shape must
 * have loaded native, the fog-coordinate array must exist, and none of the
 * levers that need one matrix per batch may be on. */
static int palette_settled;
int gx_palette_on(void) {
    if (!palette_settled) {
        palette_settled = 1;
        palette_on = 0;
        if (port_opt.palette && !port_opt.nopalette && !port_opt.cpuxf &&
            !port_opt.olddecode && !port_opt.dlcache && gl13_live() &&
            gx_vprog_palette_available() && glc_fogcoord_available()) {
            palette_on = 1;
            pal_slots = gx_skin_palette_slots();
            memset(pal_slot, 0, sizeof(pal_slot));
            pal_reset();
        }
        /* M22 (PLAN.md 37): the pre-transform wants what the palette wanted
         * of the batch -- one set of decoded vertices the matrix loads and
         * the descriptor setters do not end -- and nothing of the card.  It
         * is off wherever the batch is not the ring's (--cpuxf, --dlcache,
         * --oldsubmit) and under the palette, which spans objects itself. */
        premerge_on = port_opt.premerge_max > 0 && !palette_on &&
                      !port_opt.cpuxf && !port_opt.olddecode && !port_opt.dlcache &&
                      !port_opt.oldsubmit && gl13_live();
        lazy_on = port_opt.lazyflush && !palette_on && !port_opt.cpuxf && !port_opt.olddecode &&
                  !port_opt.dlcache && !port_opt.oldsubmit && gl13_live();
        gx_batch_spans = palette_on || premerge_on || lazy_on;
        port_log("port> lazy flush: %s; premerge: %s (max %d vertices an object) -- "
                 "both measured and off by default, PLAN.md 37\n",
                 lazy_on ? "on" : "off", premerge_on ? "on" : "off", port_opt.premerge_max);
        port_log("port> matrix palette: %s%s\n",
                 palette_on ? "on" : "off",
                 palette_on ? "" : !port_opt.palette ? " (opt-in: --palette; PLAN.md 33.2)"
                             : port_opt.nopalette ? " (--nopalette)"
                             : port_opt.cpuxf ? " (--cpuxf)"
                             : port_opt.olddecode ? " (--olddecode)"
                             : port_opt.dlcache ? " (--dlcache)"
                             : !gl13_live() ? " (no GL)"
                             : !gx_vprog_palette_available() ? " (the ARL program is not native here)"
                             : " (no GL_EXT_fog_coord)");
    }
    return palette_on;
}

static void begin_attr_order(void) {
    static const int order[] = { GX_VA_POS,  GX_VA_NRM,  GX_VA_CLR0, GX_VA_CLR1,
                                 GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3,
                                 GX_VA_TEX4, GX_VA_TEX5, GX_VA_TEX6, GX_VA_TEX7 };
    size_t i;
    if (!palette_settled) {
        gx_palette_on();
    }
    nactive = 0;
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (gx.vcd[order[i]] != GX_NONE) {
            active[nactive++] = order[i];
        }
    }
    acur = 0;

    /* How many texcoord slots this primitive's vertices must actually hold.
     * transform_and_store no longer copies the whole 96-byte Vtx, so a slot
     * nobody writes is stale rather than zero -- and a TEV stage is allowed to
     * name a coord the texgen state does not generate.  Writing the gap is a
     * couple of stores in the rare case and nothing in the common one. */
    {
        int t;
        tex_slots = gx.num_texgens;
        for (t = 0; t < gx.num_tev && t < GX_TEV_STAGES; t++) {
            int c = gx.tev[t].coord;
            if (c < GX_TEXCOORDS && c + 1 > tex_slots) {
                tex_slots = c + 1;
            }
        }
        if (tex_slots > GX_TEXCOORDS) {
            tex_slots = GX_TEXCOORDS;
        }
    }

    /* everything transform_and_store would otherwise re-derive per vertex */
    {
        u32 slot = gx.cur_pnmtx < 10 ? gx.cur_pnmtx : 0;
        const GXChanCtrl* cc = &gx.chan[0];
        int t;
        pi.pos_mtx = gx.pos_mtx[slot];
        pi.nrm_mtx = gx.nrm_mtx[slot];
        if (pi.pos_mtx[3] > GX_OFFWORLD_LIMIT || pi.pos_mtx[3] < -GX_OFFWORLD_LIMIT ||
            pi.pos_mtx[7] > GX_OFFWORLD_LIMIT || pi.pos_mtx[7] < -GX_OFFWORLD_LIMIT) {
            stat_offworld++;
            /* Name the first few, the same way --drawlog does: the matrix GX
             * was handed is a member of a HU3DDRAWOBJ, so the model and the
             * HSF object come back from the pointer alone. */
            if (port_opt.gxwarn && offworld_named < 8) {
                int mdl = -1;
                const char* nm = port_drawobj_name(gx_last_posmtx_arg, &mdl);
                offworld_named++;
                port_log("gxwarn> off-world draw: model %d object \"%s\" "
                         "translation %.1f %.1f %.1f\n",
                         mdl, nm ? nm : "?", pi.pos_mtx[3], pi.pos_mtx[7],
                         pi.pos_mtx[11]);
            }
        }
        pi.have_nrm = gx.vcd[GX_VA_NRM] != GX_NONE;
        pi.no_clr0 = gx.vcd[GX_VA_CLR0] == GX_NONE;
        if (gx.num_chans == 0) {
            pi.chan_mode = 0;
        } else if (!cc->enable) {
            pi.chan_mode = cc->mat_src == GX_SRC_REG ? 1 : 0;
        } else {
            pi.chan_mode = 2;
        }
        gx_hilite_decide();
        pi.ntexgen = gx.num_texgens < GX_TEXCOORDS ? gx.num_texgens : GX_TEXCOORDS;
        for (t = 0; t < pi.ntexgen; t++) {
            const GXTexGen* g = &gx.texgen[t];
            if (g->src >= GX_TG_TEX0 && g->src <= GX_TG_TEX7) {
                pi.tg[t].src_kind = 0;
                pi.tg[t].src_k = (u8)(g->src - GX_TG_TEX0);
            } else if (g->src == GX_TG_POS) {
                pi.tg[t].src_kind = 1;
                pi.tg[t].src_k = 0;
            } else if (g->src == GX_TG_NRM) {
                pi.tg[t].src_kind = 2;
                pi.tg[t].src_k = 0;
            } else {
                pi.tg[t].src_kind = 0;
                pi.tg[t].src_k = (u8)t;
            }
            if (g->mtx == GX_IDENTITY || g->mtx < GX_TEXMTX0) {
                pi.tg[t].mtx = NULL;
            } else {
                u32 ts = ((u32)g->mtx - GX_TEXMTX0) / 3;
                pi.tg[t].mtx_slot = (u8)(ts < 20 ? ts : 0);
                pi.tg[t].mtx = gx.tex_mtx[pi.tg[t].mtx_slot];
            }
            pi.tg[t].divide = (u8)(g->func == GX_TG_MTX3x4);
        }
        pi.tex_copy_n = 0;
        for (t = 0; t < pi.ntexgen; t++) {
            if (pi.tg[t].src_kind == 0 && pi.tg[t].src_k + 1 > pi.tex_copy_n) {
                pi.tex_copy_n = pi.tg[t].src_k + 1;
            }
        }
        for (i = 0; i < (size_t)nactive; i++) {
            int a = active[i];
            pi.arr[a] = &gx.array[a];
            pi.vat[a] = &gx.vat[vtxfmt][a];
        }
    }

    /* The two layouts, settled once per primitive with everything they depend
     * on already in hand. */
    {
        int o = 12;
        sl.ntex = pi.tex_copy_n;
        sl.off_nrm = -1;
        if (pi.have_nrm) {
            sl.off_nrm = o;
            o += 12;
        }
        sl.off_clr = o;
        o += 4;
        sl.off_skin = -1;
        if (palette_on) {
            sl.off_skin = o;
            o += 4;
        }
        sl.off_tex = o;
        o += 8 * sl.ntex;
        sl.stride = o;

        out_ntex = tex_slots;
        out_off_clr = 12;
        out_off_tex = 16;
        out_stride = 16 + 8 * out_ntex;
    }

    /* M18: is the position array a skinned mesh's buffer?  Then the decode
     * reads the rest pose and the palette does the skinning (gx_skin.c). */
    pi.skin = NULL;
    pi.slotf = 0.0f;
    if (palette_on && dl_replaying && gx.vcd[GX_VA_POS] == GX_INDEX16) {
        SkinMesh* m = gx_skin_lookup(gx.array[GX_VA_POS].base, gl13_frame_number());
        if (m) {
            gx_skin_pose(m);
            pi.skin = m;
        }
    }

    build_decode_plan();
}


/* ---- the per-primitive decode plan (PLAN.md 21.4) --------------------------
 *
 * M9 measured the vertex path three ways and ruled out arithmetic (the AltiVec
 * batch bought nothing), bandwidth (a four-fold smaller vertex bought nothing)
 * and the decode's input (not decoding at all bought 18% on one scene).  What
 * was left was the decode's *shape*: `indexed()` was reached through
 * `attr_written()` for every attribute of every vertex, re-read `cur_attr()`,
 * branched down a chain on the attribute id and then called `read_component()`
 * two or three times -- and that function switched on the component type and
 * indexed a scale table on every one of those calls.  Three of the top four
 * symbols on all three scenes were that structure, plus `saveGPR`/`restGPRx`
 * at 4-8% purely as the prologue traffic it dragged in: about 1,280 cycles a
 * vertex for something that is four loads and a multiply.
 *
 * None of it depends on the vertex.  Which attribute is next, where its array
 * is, how wide its index is, what type its components are, what the fractional
 * scale is, and which field of the packed source vertex it lands in are all
 * settled by the vertex descriptor, the VAT and `GXSetArray` -- i.e. once per
 * primitive.  So `begin_attr_order()` now settles them once into `plan[]`, and
 * the inner loop walks that: no call, no cursor, no attribute-id chain, and
 * one dense switch that GCC turns into a jump table.
 *
 * Exactness is the whole constraint, so three quiet behaviours of the old path
 * are reproduced deliberately rather than dropped:
 *
 *   * an attribute the *descriptor* carries but the layout does not store --
 *     a texcoord past the last one a texgen reads, or CLR1, or CLR0 when the
 *     register material wins -- was still decoded into the `pending` staging
 *     vertex by the old code.  Those steps keep a destination; it is just in
 *     `pending` instead of in the vertex (`to_pending`).
 *   * a texcoord slot the layout stores but the descriptor does *not* carry
 *     read whatever `pending` held from some earlier primitive.  Nothing
 *     writes it during this primitive, so it is a constant here: `fill[]`.
 *   * `GXSetArray` with a null base or a zero stride left `pending` untouched.
 *     That is `DEC_NONE`, which still steps the list pointer over the index.
 *
 * and `pending` is written back from the last decoded vertex at the end of the
 * primitive, so the next primitive's `fill[]` sees exactly what it used to. */

/* the DEC_* ops and DecStep: gx_internal.h (M29: the render thread runs the plan) */

static DecStep plan[GX_MAX_ATTR];
static int nplan;

/* An 8-bit component's conversion, as a table (M17, PLAN.md 32).  A 32-bit
 * PowerPC has no integer-to-float instruction: `(f32)(s8)b` is two stores, a
 * doubleword load that hits both of them (a load-hit-store stall the 7450
 * pays in full), a subtract and a round -- per component, three per S8
 * normal, on every vertex the board draws.  A 1 KB table per (type, scale)
 * turns that into one load, and it is exact by construction: every entry is
 * the very expression it replaces, evaluated once.  The game's HSF normals
 * are S8 (hsfdraw.c:511), so this is most of the decode's conversions.
 * `--olddecode2` keeps the arithmetic in the loop. */
#define BYTE_TBL_MAX 8
static struct {
    f32 scale;
    int is_signed;
    f32 tbl[256];
} byte_tbl[BYTE_TBL_MAX];
static int byte_tbl_n;

static const f32* byte_table(int is_signed, f32 scale) {
    int i;
    for (i = 0; i < byte_tbl_n; i++) {
        if (byte_tbl[i].scale == scale && byte_tbl[i].is_signed == is_signed) {
            return byte_tbl[i].tbl;
        }
    }
    if (byte_tbl_n == BYTE_TBL_MAX) {
        return NULL; /* the loop falls back to the arithmetic */
    }
    for (i = 0; i < 256; i++) {
        byte_tbl[byte_tbl_n].tbl[i] =
            is_signed ? (f32)(s8)(u8)i * scale : (f32)(u8)i * scale;
    }
    byte_tbl[byte_tbl_n].scale = scale;
    byte_tbl[byte_tbl_n].is_signed = is_signed;
    return byte_tbl[byte_tbl_n++].tbl;
}
static int plan_ok;          /* every step decodable: the fast loop may run   */
static union { u32 u; u8 b[4]; } plan_clr; /* the register material, when it wins */
static int plan_clr_const;
static struct { u16 dstoff; f32 s, t; } plan_fill[GX_TEXCOORDS];
static int plan_nfill;
/* which slots the plan writes into the vertex, so `pending` can be refreshed
 * from the last one at the end of the primitive */
static struct { u16 dstoff; u8 k; } plan_back[GX_TEXCOORDS];
static int plan_nback;

/* The two type tables, both indexed by GXCompType. */
static int dec_bytes_of(u8 type, int n) {
    switch (type) {
        case GX_U8:
        case GX_S8: return n;
        case GX_U16:
        case GX_S16: return n * 2;
        default: return n * 4;
    }
}

static int dec_op_of(u8 type, int nread, int nwrite) {
    int base;
    switch (type) {
        case GX_U8:  base = DEC_U8_2_3;  break;
        case GX_S8:  base = DEC_S8_2_3;  break;
        case GX_U16: base = DEC_U16_2_3; break;
        case GX_S16: base = DEC_S16_2_3; break;
        default:     base = DEC_F32_2_3; break;
    }
    if (nwrite == 3) {
        return base + (nread == 3 ? 1 : 0);
    }
    return base + (nread == 2 ? 3 : 2);
}

static int dec_clr_op_of(u8 type) {
    switch (type) {
        case GX_RGB565: return DEC_CLR_RGB565;
        case GX_RGB8:   return DEC_CLR_RGB8;
        case GX_RGBA4:  return DEC_CLR_RGBA4;
        case GX_RGBA6:  return DEC_CLR_RGBA6;
        case GX_RGBX8:  return DEC_CLR_RGBX8;
        default:        return DEC_CLR_RGBA8;
    }
}

/* Called at the end of begin_attr_order(), once everything it settles is in
 * hand: the layout, the texgen sources, and which colour wins. */
typedef const u8* (*DecodeFast)(const u8*, const u8*, u32);
static DecodeFast pick_fast(void);
static DecodeFast plan_fast;

static void build_decode_plan(void) {
    int i, k;
    int supplied[GX_TEXCOORDS];

    nplan = 0;
    plan_nfill = 0;
    plan_nback = 0;
    plan_ok = 1;
    plan_clr_const = pi.no_clr0 || pi.chan_mode == 1;
    /* in memory order, so the one store below is right on either endianness */
    plan_clr.b[0] = gx.chan[0].mat.r;
    plan_clr.b[1] = gx.chan[0].mat.g;
    plan_clr.b[2] = gx.chan[0].mat.b;
    plan_clr.b[3] = gx.chan[0].mat.a;
    for (k = 0; k < GX_TEXCOORDS; k++) {
        supplied[k] = 0;
    }

    for (i = 0; i < nactive; i++) {
        int a = active[i];
        u8 desc = gx.vcd[a];
        const GXVatFmt* f = &gx.vat[vtxfmt][a];
        const GXArraySpec* arr = &gx.array[a];
        DecStep* s = &plan[nplan++];
        int nread, nwrite;

        s->attr = (u8)a;
        s->base = NULL;
        s->stride = 0;
        s->scale = gx_frac_scale[(a == GX_VA_NRM ? nrm_frac(f->type, f->frac) : f->frac) & 31];
        s->tbl = NULL;
        s->to_pending = 0;
        s->dstoff = 0;
        s->op = DEC_NONE;
        s->advance = 0;
        s->idx = desc == GX_INDEX16 ? 2 : desc == GX_INDEX8 ? 1 : 0;
        if (s->idx) {
            if (!arr->base || !arr->stride) {
                /* the old indexed() bailed out here without touching anything;
                 * the index is still consumed by the caller */
                s->advance = s->idx;
                continue;
            }
            s->base = arr->base;
            s->stride = arr->stride;
            s->advance = s->idx;
            if (pi.skin) {
                /* the rest pose, in the same layout (float Vec, hsfdraw.c:508
                 * and :515 for a cenv model) at the file's addresses */
                if (a == GX_VA_POS) {
                    s->base = (const u8*)gx_skin_rest_pos(pi.skin);
                } else if (a == GX_VA_NRM) {
                    s->base = (const u8*)gx_skin_rest_nrm(pi.skin);
                }
            }
        }

        if (a == GX_VA_POS) {
            nread = f->cnt == GX_POS_XYZ ? 3 : 2;
            nwrite = 3;
            s->op = (u8)dec_op_of(f->type, nread, nwrite);
            s->dstoff = 0;
            if (!s->idx) {
                s->advance = (u8)dec_bytes_of(f->type, nread);
            }
        } else if (a == GX_VA_NRM) {
            nread = nwrite = 3;
            s->op = (u8)dec_op_of(f->type, nread, nwrite);
            if (sl.off_nrm >= 0) {
                s->dstoff = (u16)sl.off_nrm;
            } else {
                s->to_pending = 1;
                s->dstoff = (u16)offsetof(Pending, nrm);
            }
            /* M30 (PLAN.md 45): GX_NRM_NBT carries nine components a vertex
             * (normal, binormal, tangent) and GX_NRM_NBT3 three indices; the
             * port keeps the normal and steps over the rest */
            if (!s->idx) {
                s->advance = (u8)dec_bytes_of(f->type, f->cnt == GX_NRM_XYZ ? 3 : 9);
            } else if (f->cnt == GX_NRM_NBT3) {
                s->advance = (u8)(3 * s->idx);
            }
        } else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
            s->op = (u8)dec_clr_op_of(f->type);
            if (a == GX_VA_CLR0 && !plan_clr_const) {
                s->dstoff = (u16)sl.off_clr;
            } else {
                s->to_pending = 1;
                s->dstoff = (u16)(offsetof(Pending, clr) + 4 * (a - GX_VA_CLR0));
            }
            if (!s->idx) {
                s->advance = (u8)color_bytes(f->type);
            }
        } else if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) {
            k = a - GX_VA_TEX0;
            nread = f->cnt == GX_TEX_ST ? 2 : 1;
            nwrite = 2;
            s->op = (u8)dec_op_of(f->type, nread, nwrite);
            supplied[k] = 1;
            if (k < sl.ntex) {
                s->dstoff = (u16)(sl.off_tex + 8 * k);
                plan_back[plan_nback].dstoff = s->dstoff;
                plan_back[plan_nback].k = (u8)k;
                plan_nback++;
            } else {
                s->to_pending = 1;
                s->dstoff = (u16)(offsetof(Pending, tex) + 8 * k);
            }
            if (!s->idx) {
                s->advance = (u8)dec_bytes_of(f->type, nread);
            }
        } else {
            /* Not an attribute this port decodes (GX_VA_PNMTXIDX and the
             * per-vertex texture-matrix indices; the port warns about those
             * in GXSetVtxDesc).  Without a width the list cannot be stepped,
             * so the plan is unusable and the caller keeps the old path. */
            plan_ok = 0;
        }
    }

    if (!port_opt.olddecode2) {
        for (i = 0; i < nplan; i++) {
            DecStep* s = &plan[i];
            if (s->op >= DEC_S8_2_3 && s->op <= DEC_S8_2_2) {
                s->tbl = byte_table(1, s->scale);
                if (s->tbl) {
                    s->op = (u8)(s->op - DEC_S8_2_3 + DEC_TS8_2_3);
                }
            } else if (s->op >= DEC_U8_2_3 && s->op <= DEC_U8_2_2) {
                s->tbl = byte_table(0, s->scale);
                if (s->tbl) {
                    s->op = (u8)(s->op - DEC_U8_2_3 + DEC_TU8_2_3);
                }
            }
        }
    }

    for (k = 0; k < sl.ntex; k++) {
        if (!supplied[k]) {
            plan_fill[plan_nfill].dstoff = (u16)(sl.off_tex + 8 * k);
            plan_fill[plan_nfill].s = pending.tex[k][0];
            plan_fill[plan_nfill].t = pending.tex[k][1];
            plan_nfill++;
        }
    }
    plan_fast = pick_fast();
}

static void transform_and_store(void);
static void finish_vertices(const u8* s, int n);
int gx_force_flags; /* M31: --forceobj */

/* **A writer's name does not say which attribute it fills.**
 *
 * On the console `GXPosition2f32` and `GXTexCoord2f32` are the same two stores
 * into the write-gather pipe at 0xCC008000.  The pipe has no idea what an
 * attribute is: the *command processor* consumes whatever arrives, in the
 * order the vertex descriptor names, and the writers are named for readability
 * and nothing else.  So a game is free to reach for whichever one has the
 * right shape, and Mario Party 4 does -- `src/game/window.c`, the code that
 * draws every line of message text in the game, emits each glyph's texture
 * coordinate with **`GXPosition2f32`**, because a texcoord is two floats and so
 * is a 2D position.  `src/game/printfunc.c` and four minigame modules do the
 * same in their own places, 28 calls in all.
 *
 * The port had believed the names, so those texcoords went into the position
 * and the descriptor's real last attribute was never written, which means the
 * vertex never completed.  Every window in the game drew as a handful of
 * enormous untextured quads -- the white blocks over the file-select and
 * board-settings screens.  Naming, rather than the pipe's order, was also why
 * a second `GXTexCoord2f32` overwrote the first instead of filling TEX1.
 *
 * So the port keeps a cursor into the descriptor and each writer fills
 * whichever attribute is next, converting its payload to what that attribute
 * needs.  That is what the hardware does, it costs nothing, and it makes the
 * whole class of "the game called the wrong-named writer" impossible rather
 * than one bug at a time. */
static int cur_attr(void) {
    return (nactive && acur < nactive) ? active[acur] : -1;
}

static void draw_now(void);
static void attr_written(void) {
    if (!nactive) {
        return;
    }
    if (++acur >= nactive) {
        acur = 0;
        transform_and_store();
        /* M30 (PLAN.md 45, cause H): the hardware ends a primitive on its
         * n-th vertex -- the SDK's GXEnd is an empty inline (GXVert.h) and
         * m428's rope hook (player.c:2189) never calls it.  Until M30 the
         * port only submitted on GXEnd, and the next GXBegin reset the
         * count: every immediate primitive without a GXEnd was dropped. */
        if (in_prim && want_verts && nverts >= want_verts) {
            in_prim = 0;
            port_perf_gx_begin();
            draw_now();
            port_perf_gx_end();
            nverts = 0;
        }
    }
}

/* n floats into whatever attribute the cursor is on. */
static void put_f(const f32* v, int n) {
    int a = cur_attr();
    if (a == GX_VA_POS) {
        pending.pos[0] = v[0];
        pending.pos[1] = n > 1 ? v[1] : 0.0f;
        pending.pos[2] = n > 2 ? v[2] : 0.0f;
    } else if (a == GX_VA_NRM) {
        pending.nrm[0] = v[0];
        pending.nrm[1] = n > 1 ? v[1] : 0.0f;
        pending.nrm[2] = n > 2 ? v[2] : 0.0f;
    } else if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) {
        int k = a - GX_VA_TEX0;
        pending.tex[k][0] = v[0];
        pending.tex[k][1] = n > 1 ? v[1] : 0.0f;
    } else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
        int k = a - GX_VA_CLR0, i;
        for (i = 0; i < 4; i++) {
            f32 c = i < n ? v[i] : 1.0f;
            c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
            pending.clr[k][i] = (unsigned char)(c * 255.0f + 0.5f);
        }
    }
    attr_written();
}

/* n raw fixed-point components, scaled by the *target* attribute's fractional
 * shift rather than by the one the writer's name would have picked. */
static void put_fixed(const s32* v, int n) {
    int a = cur_attr();
    f32 f[3];
    f32 k;
    int i;
    if (a < 0) {
        attr_written();
        return;
    }
    k = gx_frac_scale[(a == GX_VA_NRM ? nrm_frac(gx.vat[vtxfmt][a].type, gx.vat[vtxfmt][a].frac)
                                      : gx.vat[vtxfmt][a].frac) & 31];
    for (i = 0; i < n && i < 3; i++) {
        f[i] = (f32)v[i] * k;
    }
    put_f(f, n);
}

static void put_color(u8 r, u8 g, u8 b, u8 a8) {
    int a = cur_attr();
    int k = (a == GX_VA_CLR1) ? 1 : 0;
    pending.clr[k][0] = r;
    pending.clr[k][1] = g;
    pending.clr[k][2] = b;
    pending.clr[k][3] = a8;
    attr_written();
}

/* CPU lighting, one colour channel.  GX's model: the material colour comes
 * either from the vertex or from a register, the ambient likewise, and each
 * enabled light contributes diffuse * attenuation. */
/* the alpha channel's sum (M35): amb.a + sum(attn * diff * lcol.a), clamped,
 * times mat.a -- io[3] on the way in is the vertex alpha */
static float chan_light_factor(const GXChanCtrl* cc, const GXLight* l, const float* wpos,
                               const float* wnrm);
static void light_alpha(int c, const float* wpos, const float* wnrm, unsigned char* io) {
    const GXChanCtrl* cc = &gx.chan[c];
    float acc, mat;
    int i;
    mat = cc->mat_src == GX_SRC_REG ? byte_scale[cc->mat.a] : byte_scale[io[3]];
    acc = cc->amb_src == GX_SRC_REG ? byte_scale[cc->amb.a] : byte_scale[io[3]];
    for (i = 0; i < 8; i++) {
        const GXLight* l = &gx.light[i];
        if (!(cc->light_mask & (1u << i)) || !l->used) {
            continue;
        }
        acc += byte_scale[l->color.a] * chan_light_factor(cc, l, wpos, wnrm);
    }
    acc = acc < 0.0f ? 0.0f : acc > 1.0f ? 1.0f : acc;
    io[3] = (unsigned char)(acc * mat * 255.0f + 0.5f);
}

/* one light's diffuse * attenuation for a channel, the same three-way as
 * light_channel's (which keeps its own copy for the colour's speed) */
static float chan_light_factor(const GXChanCtrl* cc, const GXLight* l, const float* wpos,
                               const float* wnrm) {
    float dx = l->pos[0] - wpos[0], dy = l->pos[1] - wpos[1], dz = l->pos[2] - wpos[2];
    float d2 = dx * dx + dy * dy + dz * dz, d, ndl, att = 1.0f;
    if (d2 > 0.0f) {
        float rd = gx_rsqrtf(d2);
        d = d2 * rd;
        dx *= rd;
        dy *= rd;
        dz *= rd;
    } else {
        d = 1.0f;
    }
    ndl = wnrm[0] * dx + wnrm[1] * dy + wnrm[2] * dz;
    if (cc->attn_fn == GX_AF_SPEC && !port_opt.oldspec0) {
        float nh = 0.0f, num, den, nk[3], nm2;
        if (ndl >= 0.0f) {
            nh = wnrm[0] * l->dir[0] + wnrm[1] * l->dir[1] + wnrm[2] * l->dir[2];
            nh = nh < 0.0f ? 0.0f : nh;
        }
        nk[0] = l->k[0];
        nk[1] = l->k[1];
        nk[2] = l->k[2];
        if (cc->diff_fn != GX_DF_NONE) {
            nm2 = nk[0] * nk[0] + nk[1] * nk[1] + nk[2] * nk[2];
            if (nm2 > 0.0f) {
                float rn = gx_rsqrtf(nm2);
                nk[0] *= rn;
                nk[1] *= rn;
                nk[2] *= rn;
            }
        }
        num = l->a[0] + l->a[1] * nh + l->a[2] * nh * nh;
        den = nk[0] + nk[1] * nh + nk[2] * nh * nh;
        num = num < 0.0f ? 0.0f : num;
        att = den != 0.0f ? num / den : 0.0f;
    } else if (cc->attn_fn != GX_AF_NONE) {
        float den = l->k[0] + l->k[1] * d + l->k[2] * d2;
        att = den > 0.0f ? 1.0f / den : 1.0f;
        if (att > 1.0f) {
            att = 1.0f;
        }
        if (cc->attn_fn == GX_AF_SPOT && !port_opt.nospot) {
            float cs = -(dx * l->dir[0] + dy * l->dir[1] + dz * l->dir[2]);
            float num;
            cs = cs < 0.0f ? 0.0f : cs;
            num = l->a[0] + l->a[1] * cs + l->a[2] * cs * cs;
            att *= num < 0.0f ? 0.0f : num;
        }
    }
    if (cc->diff_fn == GX_DF_CLAMP) {
        ndl = ndl < 0.0f ? 0.0f : ndl;
    } else if (cc->diff_fn != GX_DF_SIGN) {
        ndl = 1.0f;
    }
    return ndl * att;
}

static void light_channel(int c, const float* wpos, const float* wnrm,
                          unsigned char* io) {
    const GXChanCtrl* cc = &gx.chan[c];
    float acc[3];
    float mat[4];
    int i;
    if (!cc->enable) {
        if (cc->mat_src == GX_SRC_REG) {
            io[0] = cc->mat.r;
            io[1] = cc->mat.g;
            io[2] = cc->mat.b;
            io[3] = cc->mat.a;
        }
        return;
    }
    if (cc->mat_src == GX_SRC_REG) {
        mat[0] = byte_scale[cc->mat.r];
        mat[1] = byte_scale[cc->mat.g];
        mat[2] = byte_scale[cc->mat.b];
        mat[3] = byte_scale[cc->mat.a];
    } else {
        mat[0] = byte_scale[io[0]];
        mat[1] = byte_scale[io[1]];
        mat[2] = byte_scale[io[2]];
        mat[3] = byte_scale[io[3]];
    }
    if (cc->amb_src == GX_SRC_REG) {
        acc[0] = byte_scale[cc->amb.r];
        acc[1] = byte_scale[cc->amb.g];
        acc[2] = byte_scale[cc->amb.b];
    } else {
        acc[0] = byte_scale[io[0]];
        acc[1] = byte_scale[io[1]];
        acc[2] = byte_scale[io[2]];
    }
    for (i = 0; i < 8; i++) {
        const GXLight* l;
        float dx, dy, dz, d2, d, ndl, att;
        if (!(cc->light_mask & (1u << i))) {
            continue;
        }
        l = &gx.light[i];
        if (!l->used) {
            continue;
        }
        dx = l->pos[0] - wpos[0];
        dy = l->pos[1] - wpos[1];
        dz = l->pos[2] - wpos[2];
        d2 = dx * dx + dy * dy + dz * dz;
        /* One reciprocal square root does the normalisation and the distance
         * both, and the 74xx has no fsqrt to call anyway (gx_math.h). */
        if (d2 > 0.0f) {
            float rd = gx_rsqrtf(d2);
            d = d2 * rd;
            dx *= rd;
            dy *= rd;
            dz *= rd;
        } else {
            d = 1.0f;
        }
        ndl = wnrm[0] * dx + wnrm[1] * dy + wnrm[2] * dz;
        att = 1.0f;
        if (cc->attn_fn == GX_AF_SPEC && !port_opt.oldspec0) {
            /* M35 (PLAN.md 50.14): the specular attenuation on a colour (or
             * alpha) channel, as the hardware and Dolphin's LightingShaderGen
             * compute it -- what gx_vprog.c's hilite path does for channel 1:
             *   nh   = (N . ldir >= 0) ? max(0, N . H) : 0     H = the light's dir
             *   attn = max(0, a . (1, nh, nh^2)) / (k . (1, nh, nh^2))
             * with k normalised unless the diffuse function is NONE (Dolphin's
             * rule, the hardware's).  M3..M35 read AF_SPEC as the distance
             * attenuation with these k, which for m425's Thwomps (k = (2, 0,
             * -1), a = (0, 0, 1)) was 1 / (2 - d^2): opaque, unlit slabs. */
            float nh = 0.0f, num, den, nk[3], nm2;
            if (ndl >= 0.0f) {
                nh = wnrm[0] * l->dir[0] + wnrm[1] * l->dir[1] + wnrm[2] * l->dir[2];
                nh = nh < 0.0f ? 0.0f : nh;
            }
            nk[0] = l->k[0];
            nk[1] = l->k[1];
            nk[2] = l->k[2];
            if (cc->diff_fn != GX_DF_NONE) {
                nm2 = nk[0] * nk[0] + nk[1] * nk[1] + nk[2] * nk[2];
                if (nm2 > 0.0f) {
                    float rn = gx_rsqrtf(nm2);
                    nk[0] *= rn;
                    nk[1] *= rn;
                    nk[2] *= rn;
                }
            }
            num = l->a[0] + l->a[1] * nh + l->a[2] * nh * nh;
            den = nk[0] + nk[1] * nh + nk[2] * nh * nh;
            num = num < 0.0f ? 0.0f : num;
            att = den != 0.0f ? num / den : 0.0f;
        }
        if (cc->diff_fn == GX_DF_CLAMP) {
            ndl = ndl < 0.0f ? 0.0f : ndl;
        } else if (cc->diff_fn == GX_DF_SIGN) {
            /* signed: keep it */
        } else {
            ndl = 1.0f;
        }
        if (cc->attn_fn != GX_AF_NONE && !(cc->attn_fn == GX_AF_SPEC && !port_opt.oldspec0)) {
            float den = l->k[0] + l->k[1] * d + l->k[2] * d2;
            att = den > 0.0f ? 1.0f / den : 1.0f;
            if (att > 1.0f) {
                att = 1.0f;
            }
            if (cc->attn_fn == GX_AF_SPOT && !port_opt.nospot) {
                /* M30 (PLAN.md 45): the cone, as gx_vprog.c computes it */
                float cs = -(dx * l->dir[0] + dy * l->dir[1] + dz * l->dir[2]);
                float num;
                cs = cs < 0.0f ? 0.0f : cs;
                num = l->a[0] + l->a[1] * cs + l->a[2] * cs * cs;
                att *= num < 0.0f ? 0.0f : num;
            }
        }
        acc[0] += byte_scale[l->color.r] * ndl * att;
        acc[1] += byte_scale[l->color.g] * ndl * att;
        acc[2] += byte_scale[l->color.b] * ndl * att;
    }
    for (i = 0; i < 3; i++) {
        float v = acc[i] * mat[i];
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        io[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
    io[3] = (unsigned char)(mat[3] * 255.0f + 0.5f);
    if (c == 0 && gx.chan[GX_ALPHA0].enable && !port_opt.nolitalpha) {
        /* M35 (PLAN.md 50.14): the alpha channel is lit too, by its own
         * control -- GXSetChanCtrl(GX_COLOR0A0, ...) sets both -- the same
         * sum over the lights' alpha: m425's Thwomps carry ambient 0x40 and
         * a white material, and are translucent slabs with opaque specular
         * highlights on the console */
        light_alpha(GX_ALPHA0, wpos, wnrm, io);
    }
}

/* ---- the vertex path, in two phases ----------------------------------------
 *
 * M4's profile said `transform_and_store` was the hottest symbol in the port
 * and M4's hoist did not move it (PLAN.md 14.3); M5's says the same, 25.7% of
 * the character select's in-thread samples and 30.9% of the board's.  The
 * reason a per-vertex AltiVec rewrite could not help was structural: the
 * function was called once per vertex from the attribute cursor, so the
 * modelview and the normal matrix were reloaded from memory for every vertex
 * and nothing could stay in a vector register.
 *
 * So the path is split.  Phase 1, `store_vertex`, runs per vertex and does
 * only what does not need the matrices: the colours (including the two
 * constant-per-primitive overrides), the *model-space* position and normal,
 * and the raw texture coordinates any texgen will read back.  Phase 2,
 * `finish_vertices`, runs once per primitive from `draw_now` with the whole
 * run in hand, and is the only place a matrix is touched.
 *
 * That is worth doing on its own -- the matrices load once per primitive
 * instead of once per vertex -- and it is what makes AltiVec possible: the
 * four columns of the modelview and the three of the normal matrix are built
 * once and stay in vector registers for the whole run, and each vertex is
 * three `vec_madd`s instead of nine multiplies and six adds.
 *
 * **The scalar path is kept and is the default.**  Without `__ALTIVEC__` the
 * arithmetic is exactly what it was, in the same order, so the goldens are
 * unchanged.  With it, `vec_madd` is a fused multiply-add and `vec_rsqrte` is
 * a different reciprocal square root, so the rounding differs and the
 * reference md5s were re-based deliberately -- see PLAN.md 15.5.
 */

/* Phase 2: the whole run at once, source -> output.
 *
 * `s` is the run's first source vertex -- src_buf for an immediate primitive,
 * and the cached copy for a display-list replay that hit (see the cache
 * below), which is the whole point of storing the source rather than the
 * output: the matrices move every frame and the model-space vertices do not.
 *
 * M5 ran this as four passes over the array (position, normal, lighting,
 * texgen).  It is one pass now.  The arithmetic per vertex is unchanged and in
 * the same order -- there is no dependency between vertices, so the passes
 * were free to merge -- but a vertex is now read once, held in registers, and
 * written once, instead of being walked four times through a 96-byte stride.
 * The reference md5s do not move, and did not. */
/* The matrices a submit runs under: the batch's own copies for a batch from
 * the ring (M22: gx.pos_mtx[] may have moved since it was decoded), the
 * primitive's for a cached list (gx_batch_spans is off with --dlcache). */
static const f32* sub_posm;
static const f32* sub_nrmm;

static void finish_vertices(const u8* s, int n) {
    const f32* m = sub_posm;
    const f32* nm = sub_nrmm;
    const int sstride = sl.stride, ostride = out_stride;
    int i;

    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++, s += sstride) {
        u8* o = out_buf + (size_t)i * ostride;
        f32* op = (f32*)o;
        const f32* sp = (const f32*)s;
        float px = sp[0], py = sp[1], pz = sp[2];
        float nrm[3];
        int t;

        op[0] = m[0] * px + m[1] * py + m[2] * pz + m[3];
        op[1] = m[4] * px + m[5] * py + m[6] * pz + m[7];
        op[2] = m[8] * px + m[9] * py + m[10] * pz + m[11];

        if (sl.off_nrm >= 0) {
            const f32* sn = (const f32*)(s + sl.off_nrm);
            float nx = sn[0], ny = sn[1], nz = sn[2];
            float len2;
            nrm[0] = nm[0] * nx + nm[1] * ny + nm[2] * nz;
            nrm[1] = nm[3] * nx + nm[4] * ny + nm[5] * nz;
            nrm[2] = nm[6] * nx + nm[7] * ny + nm[8] * nz;
            len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 0.0f) {
                float rl = gx_rsqrtf(len2);
                nrm[0] *= rl;
                nrm[1] *= rl;
                nrm[2] *= rl;
            }
        } else {
            /* what the old phase 1 stored for a descriptor with no normal, and
             * what a GX_TG_NRM texgen then read back */
            nrm[0] = nrm[1] = 0.0f;
            nrm[2] = 1.0f;
        }

        {
            const u8* sc = s + sl.off_clr;
            u8* oc = o + out_off_clr;
            oc[0] = sc[0];
            oc[1] = sc[1];
            oc[2] = sc[2];
            oc[3] = sc[3];
            if (pi.chan_mode == 2) {
                light_channel(0, op, nrm, oc);
            }
        }

        {
            const f32* raw = (const f32*)(s + sl.off_tex);
            f32* ot = (f32*)(o + out_off_tex);
            for (t = 0; t < pi.ntexgen; t++) {
                float sc, tc, in[3];
                const f32* tm = pi.tg[t].mtx;
                if (pi.tg[t].src_kind == 0) {
                    const f32* r = raw + 2 * pi.tg[t].src_k;
                    in[0] = r[0];
                    in[1] = r[1];
                    in[2] = 1.0f;
                } else if (pi.tg[t].src_kind == 1) {
                    /* the RAW position (M26, PLAN.md 41; `--viewtexgen` is
                     * the M3..M25 view-space input): see gx_vprog.c */
                    in[0] = port_opt.viewtexgen ? op[0] : px;
                    in[1] = port_opt.viewtexgen ? op[1] : py;
                    in[2] = port_opt.viewtexgen ? op[2] : pz;
                } else if (port_opt.viewtexgen || sl.off_nrm < 0) {
                    in[0] = nrm[0];
                    in[1] = nrm[1];
                    in[2] = nrm[2];
                } else {
                    const f32* sn = (const f32*)(s + sl.off_nrm);
                    in[0] = sn[0];
                    in[1] = sn[1];
                    in[2] = sn[2];
                }
                if (!tm) {
                    sc = in[0];
                    tc = in[1];
                } else {
                    sc = tm[0] * in[0] + tm[1] * in[1] + tm[2] * in[2] + tm[3];
                    tc = tm[4] * in[0] + tm[5] * in[1] + tm[6] * in[2] + tm[7];
                    if (pi.tg[t].divide) {
                        float q =
                            tm[8] * in[0] + tm[9] * in[1] + tm[10] * in[2] + tm[11];
                        if (q != 0.0f) {
                            sc /= q;
                            tc /= q;
                        }
                    }
                }
                ot[2 * t] = sc;
                ot[2 * t + 1] = tc;
            }
            for (; t < out_ntex; t++) {
                ot[2 * t] = 0.0f;
                ot[2 * t + 1] = 0.0f;
            }
        }
    }
}

/* Phase 1: everything that does not need a matrix, packed into the source
 * layout.  One vertex, one store of each field it actually has. */
static void transform_and_store(void) {
    u8* v;
    int k;
    if (nverts >= MAX_VERTS || run_pos + (size_t)(nverts + 1) * sl.stride > src_cap) {
        gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");
        return;
    }
    v = src_buf + run_pos + (size_t)nverts * sl.stride;
    nverts++;
    {
        f32* p = (f32*)v;
        p[0] = pending.pos[0];
        p[1] = pending.pos[1];
        p[2] = pending.pos[2];
    }
    if (sl.off_nrm >= 0) {
        f32* np = (f32*)(v + sl.off_nrm);
        np[0] = pending.nrm[0];
        np[1] = pending.nrm[1];
        np[2] = pending.nrm[2];
    }
    {
        u8* c = v + sl.off_clr;
        if (pi.no_clr0 || pi.chan_mode == 1) {
            /* Both cases splat the register material, and both are constant for
             * the whole primitive, so the copy from `pending` is skipped. */
            c[0] = gx.chan[0].mat.r;
            c[1] = gx.chan[0].mat.g;
            c[2] = gx.chan[0].mat.b;
            c[3] = gx.chan[0].mat.a;
        } else {
            c[0] = pending.clr[0][0];
            c[1] = pending.clr[0][1];
            c[2] = pending.clr[0][2];
            c[3] = pending.clr[0][3];
        }
    }
    if (sl.off_skin >= 0) {
        *(f32*)(v + sl.off_skin) = pi.slotf; /* never a skinned mesh: those are lists */
    }
    {
        f32* t = (f32*)(v + sl.off_tex);
        for (k = 0; k < sl.ntex; k++) {
            t[2 * k] = pending.tex[k][0];
            t[2 * k + 1] = pending.tex[k][1];
        }
    }
}

/* ---- the writers ----------------------------------------------------------- */

static void indexed(u32 index) {
    int attr = cur_attr();
    const GXArraySpec* a;
    const GXVatFmt* f;
    const u8* p;
    if (attr < 0) {
        attr_written();
        return;
    }
    a = pi.arr[attr];
    f = pi.vat[attr];
    if (!a->base || !a->stride) {
        attr_written();
        return;
    }
    p = a->base + (size_t)index * a->stride;
    if (attr == GX_VA_POS) {
        int n = f->cnt == GX_POS_XYZ ? 3 : 2;
        pending.pos[0] = read_component(p, f->type, f->frac, 0);
        pending.pos[1] = read_component(p, f->type, f->frac, 1);
        pending.pos[2] = n == 3 ? read_component(p, f->type, f->frac, 2) : 0.0f;
    } else if (attr == GX_VA_NRM) {
        u8 nf = nrm_frac(f->type, f->frac);
        pending.nrm[0] = read_component(p, f->type, nf, 0);
        pending.nrm[1] = read_component(p, f->type, nf, 1);
        pending.nrm[2] = read_component(p, f->type, nf, 2);
    } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        read_color(p, f->type, pending.clr[attr - GX_VA_CLR0]);
    } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        int k = attr - GX_VA_TEX0;
        pending.tex[k][0] = read_component(p, f->type, f->frac, 0);
        pending.tex[k][1] =
            f->cnt == GX_TEX_ST ? read_component(p, f->type, f->frac, 1) : 0.0f;
    }
    attr_written();
}

/* The indexed writers.  Which attribute an index belongs to is the cursor's
 * business, not the function name's -- see attr_written above. */
#define IDX_WRITER(name, T, put)                                                         \
    void name(T index) {                                                                 \
        if (dl_recording) {                                                              \
            put;                                                                         \
            return;                                                                      \
        }                                                                                \
        indexed((u32)index);                                                             \
    }

IDX_WRITER(GXPosition1x16, u16, dl_u16(index))
IDX_WRITER(GXPosition1x8, u8, dl_u8(index))
IDX_WRITER(GXNormal1x16, u16, dl_u16(index))
IDX_WRITER(GXNormal1x8, u8, dl_u8(index))
IDX_WRITER(GXColor1x16, u16, dl_u16(index))
IDX_WRITER(GXColor1x8, u8, dl_u8(index))
IDX_WRITER(GXTexCoord1x16, u16, dl_u16(index))
IDX_WRITER(GXTexCoord1x8, u8, dl_u8(index))
/* M31 (PLAN.md 46): the decomp's name for a bare u16 store into the pipe
 * (GXVert.h:180), which m430's rope and sail lists (player.c:1513,
 * water.c:1151) use for every index; the same cursor as the named writers.
 * It was an untyped stub until M31 -- the lists recorded nothing but their
 * GXBegin, and the decoder read the heap after it as vertices. */
IDX_WRITER(GXUnknownu16, u16, dl_u16(index))

void GXPosition3f32(f32 x, f32 y, f32 z) {
    f32 v[3];
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        dl_f32(z);
        return;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
    put_f(v, 3);
}
void GXPosition2f32(f32 x, f32 y) {
    f32 v[2];
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        return;
    }
    v[0] = x;
    v[1] = y;
    put_f(v, 2);
}
void GXPosition3s16(s16 x, s16 y, s16 z) {
    s32 v[3];
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        dl_u16((u16)z);
        return;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
    put_fixed(v, 3);
}
void GXPosition2s16(s16 x, s16 y) {
    s32 v[2];
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        return;
    }
    v[0] = x;
    v[1] = y;
    put_fixed(v, 2);
}
void GXPosition2u16(u16 x, u16 y) {
    s32 v[2];
    if (dl_recording) {
        dl_u16(x);
        dl_u16(y);
        return;
    }
    v[0] = x;
    v[1] = y;
    put_fixed(v, 2);
}
void GXPosition3u8(u8 x, u8 y, u8 z) {
    s32 v[3];
    if (dl_recording) {
        dl_u8(x);
        dl_u8(y);
        dl_u8(z);
        return;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
    put_fixed(v, 3);
}
void GXNormal3f32(f32 x, f32 y, f32 z) {
    f32 v[3];
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        dl_f32(z);
        return;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
    put_f(v, 3);
}
void GXNormal3s16(s16 x, s16 y, s16 z) {
    s32 v[3];
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        dl_u16((u16)z);
        return;
    }
    v[0] = x;
    v[1] = y;
    v[2] = z;
    put_fixed(v, 3);
}
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
    if (dl_recording) {
        dl_u8(r);
        dl_u8(g);
        dl_u8(b);
        dl_u8(a);
        return;
    }
    put_color(r, g, b, a);
}
void GXColor3u8(u8 r, u8 g, u8 b) {
    int a;
    if (dl_recording) {
        dl_u8(r);
        dl_u8(g);
        dl_u8(b);
        return;
    }
    a = cur_attr();
    if (a >= 0 && a != GX_VA_CLR0 && a != GX_VA_CLR1 && !port_opt.clrasclr) {
        /* M35 (PLAN.md 50): three bytes on the write-gather pipe are three
         * bytes -- the hardware reads them by the vertex descriptor, not by
         * the name of the inline that wrote them.  hsfman.c:2031 writes the
         * full-screen background quad Hu3DShadowExec paints after every
         * shadow pass (GX_VA_POS direct, GX_U8 XYZ) with GXColor3u8, and
         * put_color filed the bytes as a colour: the quad's four positions
         * were whatever the last vertex left, nothing painted, and every
         * scene with a shadow pass kept the EFB's black clear wherever its
         * geometry did not cover -- m401's band across the sea, m406's black
         * distance.  `--clrasclr` is the M3..M34 reading. */
        s32 v[3];
        u8 t = gx.vat[vtxfmt][a].type;
        v[0] = t == GX_S8 ? (s8)r : r;
        v[1] = t == GX_S8 ? (s8)g : g;
        v[2] = t == GX_S8 ? (s8)b : b;
        put_fixed(v, 3);
        return;
    }
    put_color(r, g, b, 255);
}
void GXColor1u32(u32 c) {
    GXColor4u8((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c);
}
void GXTexCoord2f32(f32 s, f32 t) {
    f32 v[2];
    if (dl_recording) {
        dl_f32(s);
        dl_f32(t);
        return;
    }
    v[0] = s;
    v[1] = t;
    put_f(v, 2);
}
void GXTexCoord2s16(s16 s, s16 t) {
    s32 v[2];
    if (dl_recording) {
        dl_u16((u16)s);
        dl_u16((u16)t);
        return;
    }
    v[0] = s;
    v[1] = t;
    put_fixed(v, 2);
}
void GXTexCoord2u16(u16 s, u16 t) { GXTexCoord2s16((s16)s, (s16)t); }

/* ---- GXBegin / GXEnd ------------------------------------------------------- */

static GLenum gl_prim(u8 p) {
    switch (p) {
        case GX_QUADS: return GL_QUADS;
        case GX_TRIANGLES: return GL_TRIANGLES;
        case GX_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
        case GX_TRIANGLEFAN: return GL_TRIANGLE_FAN;
        case GX_LINES: return GL_LINES;
        case GX_LINESTRIP: return GL_LINE_STRIP;
        default: return GL_POINTS;
    }
}

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 n) {
    if (dl_recording) {
        dl_u8((u8)((u8)type | (u8)fmt));
        dl_u16(n);
        return;
    }
    prim = (u8)type;
    vtxfmt = (u8)fmt;
    want_verts = n;
    nverts = 0;
    sv_first = 0;
    in_prim = 1;
    begin_attr_order();
    batch_prepare(n);
    run_pos = ring_claim((size_t)n * (size_t)sl.stride);
    pal_place();
    GXLOG("GXBegin", "prim %02x fmt %d n %u, %d attrs", type, fmt, n, nactive);
}

/* --drawlog N: explain the first N draws in full -- the geometry after the CPU
 * transform, the raster colour, the texture that is bound and the pipeline
 * state that decides whether any of it survives to the framebuffer.  Written
 * because "the draw happens and the screen stays black" has too many possible
 * causes to reason about from the source, and each of them is one line here. */
static void draw_log(u32 first, u32 count, u8 dprim) {
    static int shown;
    int i;
    int nverts = (int)count; /* shadows the global on purpose: see draw_submit */
    u8 prim = dprim;
    const GXTevStage* s0 = &gx.tev[0];
    GXTexObjPort* t;
    if (!port_opt.drawlog || shown >= port_opt.drawlog) {
        return;
    }
    /* --drawlog-at: the draws worth explaining are almost never the first
     * ones.  The boot spends hundreds of frames on two logos, so an
     * unqualified --drawlog explains the Nintendo logo eight times and stops;
     * the question at M3 was about the title screen's character models, six
     * hundred frames later. */
    if (port_opt.drawlog_frame &&
        gl13_frame_number() + 1 != (unsigned)port_opt.drawlog_frame) {
        return;
    }
    shown++;
    port_log("---- draw %d: prim %02x, %d verts, %d tev stage(s), %d texgen(s), "
             "%d chan(s) ----\n",
             shown, prim, nverts, gx.num_tev, gx.num_texgens, gx.num_chans);
    for (i = 0; i < nverts && i < 4; i++) {
        const u8* o = out_buf + (size_t)(first + (u32)i) * out_stride;
        const f32* op = (const f32*)o;
        const u8* oc = o + out_off_clr;
        const f32* ot = (const f32*)(o + out_off_tex);
        port_log("  v%d pos %8.2f %8.2f %8.2f  clr %3u %3u %3u %3u  st %6.3f %6.3f\n",
                 i, op[0], op[1], op[2], oc[0], oc[1], oc[2], oc[3],
                 out_ntex ? ot[0] : 0.0f, out_ntex ? ot[1] : 0.0f);
    }
    {
        const f32* m = gx.pos_mtx[gx.cur_pnmtx < 10 ? gx.cur_pnmtx : 0];
        port_log("  posmtx%u  %8.3f %8.3f %8.3f %10.2f\n", gx.cur_pnmtx, m[0], m[1],
                 m[2], m[3]);
        port_log("           %8.3f %8.3f %8.3f %10.2f\n", m[4], m[5], m[6], m[7]);
        port_log("           %8.3f %8.3f %8.3f %10.2f\n", m[8], m[9], m[10], m[11]);
    }
    port_log("  proj %s [%g %g %g %g %g %g]  viewport %g %g %g %g z %g..%g\n",
             gx.proj_type == GX_PERSPECTIVE ? "persp" : "ortho", gx.proj[0],
             gx.proj[1], gx.proj[2], gx.proj[3], gx.proj[4], gx.proj[5], gx.vp[0],
             gx.vp[1], gx.vp[2], gx.vp[3], gx.vp[4], gx.vp[5]);
    /* Every stage, not only the first.  The TL32 eye material (PLAN.md 29.4)
     * is two stages over one atlas with two palettes, so a log that stops at
     * stage 0 cannot see the bug at all: the second stage's texmap and its
     * TLUT are the whole question. */
    for (i = 0; i < (int)gx.num_tev && i < GX_TEV_STAGES; i++) {
        const GXTevStage* s = &gx.tev[i];
        {
            float kc[4], ka[4];
            gx_tev_stage_konst(s, 0, kc);
            gx_tev_stage_konst(s, 1, ka);
            port_log("  stage%d coord %u map %u chan %u  cin %u %u %u %u  "
                     "ain %u %u %u %u  swap ras %u tex %u  creg %u areg %u  "
                     "konst %.2f %.2f %.2f / %.2f\n",
                     i, s->coord, s->map, s->chan, s->cin[0], s->cin[1], s->cin[2],
                     s->cin[3], s->ain[0], s->ain[1], s->ain[2], s->ain[3],
                     s->ras_swap, s->tex_swap, s->creg, s->areg, kc[0], kc[1], kc[2], ka[3]);
        }
        t = gx_bound_tex(s->map);
        if (t) {
            const GXTlutObjPort* tl =
                (t->is_ci && t->tlut_name < 64) ? &gx.tlut[t->tlut_name] : NULL;
            port_log("    texmap%u %ux%u fmt %u ci %u tlut %u gl %u  img %p"
                     "  lut %p n %u lfmt %u\n",
                     s->map, t->width, t->height, t->format, t->is_ci,
                     t->tlut_name, t->gl_name, t->image, tl ? tl->lut : NULL,
                     tl ? (unsigned)tl->n : 0u, tl ? (unsigned)tl->fmt : 0u);
        } else {
            port_log("    texmap%u NOT BOUND\n", s->map);
        }
    }
    /* The TEV registers and konst colours every stage above can name.  A
     * GX_TEXMAP_NULL stage reads nothing but these and CPREV, so when such a
     * stage draws the wrong colour these four numbers are the whole input. */
    port_log("  tevreg prev %3u %3u %3u %3u  c0 %3u %3u %3u %3u  "
             "c1 %3u %3u %3u %3u  c2 %3u %3u %3u %3u\n",
             gx.tev_reg[0].r, gx.tev_reg[0].g, gx.tev_reg[0].b, gx.tev_reg[0].a,
             gx.tev_reg[1].r, gx.tev_reg[1].g, gx.tev_reg[1].b, gx.tev_reg[1].a,
             gx.tev_reg[2].r, gx.tev_reg[2].g, gx.tev_reg[2].b, gx.tev_reg[2].a,
             gx.tev_reg[3].r, gx.tev_reg[3].g, gx.tev_reg[3].b, gx.tev_reg[3].a);

    /* The texgens and the texture matrices they name.  The eye and eyebrow
     * materials pick one blink frame out of an atlas as a 2x4 texture matrix
     * (hsfanim.c:255-258), so "which sub-rectangle of the atlas" is a pair of
     * numbers in this matrix and nowhere else; and the atlas is NPOT, so the
     * port's own padding fold rides in the same place (PLAN.md 30). */
    for (i = 0; i < (int)gx.num_texgens && i < GX_TEXCOORDS; i++) {
        const GXTexGen* g = &gx.texgen[i];
        port_log("  texgen%d func %u src %u mtx %u norm %u postmtx %u\n", i,
                 g->func, g->src, g->mtx, g->normalize, g->postmtx);
        if (g->mtx >= 30 && g->mtx < 30 + 20 * 3) {
            const f32* m = gx.tex_mtx[(g->mtx - 30) / 3];
            port_log("    texmtx%u  %8.4f %8.4f %8.4f %8.4f\n", (g->mtx - 30) / 3,
                     m[0], m[1], m[2], m[3]);
            port_log("             %8.4f %8.4f %8.4f %8.4f\n", m[4], m[5], m[6], m[7]);
        } else {
            port_log("    texmtx identity (mtx id %u)\n", g->mtx);
        }
        {
            float su = 1.0f, sv = 1.0f, tv = 0.0f;
            glc_get_tex_fold(i, &su, &sv, &tv);
            port_log("    npot fold unit %d  su %.6f sv %.6f tv %.6f%s\n", i, su, sv, tv,
                     tv != 0.0f ? "  (an EFB copy, flipped: M24b)" : "");
        }
    }
    (void)s0;
    port_log("  chan0 enable %u matsrc %u mat %u %u %u %u  ambsrc %u\n",
             gx.chan[0].enable, gx.chan[0].mat_src, gx.chan[0].mat.r,
             gx.chan[0].mat.g, gx.chan[0].mat.b, gx.chan[0].mat.a,
             gx.chan[0].amb_src);
    port_log("  alphacmp %u ref %u op %u / %u ref %u   zmode test %u fn %u write %u\n",
             gx.alpha_comp0, gx.alpha_ref0, gx.alpha_op, gx.alpha_comp1,
             gx.alpha_ref1, gx.z_enable, gx.z_func, gx.z_update);
    if (gl13_zprepass_wanted()) {
        /* M34: the depth-only pass this draw gets first (--zprepass 1: only under
         * GXSetZCompLoc(TRUE); 2: every alpha-tested z-writing draw) */
        port_log("  z pre-pass: issued depth-first (comploc %u, --zprepass %d)\n", gx.z_comploc,
                 port_opt.zprepass);
    }
    port_log("  blend mode %u src %u dst %u   cull %u   scissor %u %u %u %u   update colour %u alpha %u\n",
             gx.blend_mode, gx.blend_src, gx.blend_dst, gx.cull,
             gx.scissor[0], gx.scissor[1], gx.scissor[2], gx.scissor[3],
             gx.color_update, gx.alpha_update);
    /* M32: the alpha channel's control too -- a lit colour with an alpha of
     * zero blends to nothing, which reads exactly like a draw that never
     * landed (m435's sphere, PLAN.md 47.5) */
    port_log("  alpha0 enable %u matsrc %u ambsrc %u lights %02x   colour1 enable %u   alpha1 enable %u\n",
             gx.chan[2].enable, gx.chan[2].mat_src, gx.chan[2].amb_src, gx.chan[2].light_mask,
             gx.chan[1].enable, gx.chan[3].enable);
    /* M30 (PLAN.md 45): the fog and the copy clear, which a flat-colour
     * frame (m414's cyan quadrants) can only be read from */
    port_log("  fog type %u start %g end %g near %g far %g color %u %u %u %u   "
             "copyclear %u %u %u %u z %06x\n",
             gx.fog_type, gx.fog_startz, gx.fog_endz, gx.fog_nearz, gx.fog_farz,
             gx.fog_color.r, gx.fog_color.g, gx.fog_color.b, gx.fog_color.a,
             gx.copy_clear.r, gx.copy_clear.g, gx.copy_clear.b, gx.copy_clear.a,
             (unsigned)gx.copy_clear_z);
    /* M34: the object name and the caller last -- a hook's matrix (m404's
     * guide line) is a stack Mtx, not a DrawObjData entry, and the lookup
     * faulted the bench's drawlog before the stages were printed. */
    {
        int mdl = -1;
        const char* nm = port_drawobj_name(gx_last_posmtx_arg, &mdl);
        if (mdl >= 0) {
            port_log("           drawobj model %d object \"%s\"\n", mdl,
                     nm ? nm : "?");
        }
    }
    {
        Dl_info di;
        if (gx_last_posmtx_caller &&
            dladdr((void*)(uintptr_t)gx_last_posmtx_caller, &di) && di.dli_sname) {
            port_log("           loaded by %s+%u (%s)\n", di.dli_sname,
                     (unsigned)((const char*)gx_last_posmtx_caller -
                                (const char*)di.dli_saddr),
                     di.dli_fname ? di.dli_fname : "?");
        }
    }
    {
        GLenum e = GL(glGetError)();
        port_log("  glGetError %s (0x%04x)\n", e == GL_NO_ERROR ? "GL_NO_ERROR" : "SET",
                 (unsigned)e);
    }
}

/* The draw itself, once the source run is in hand.  `s` is where phase 2
 * reads from: src_buf for a primitive the writers just assembled, and the
 * cached copy for a display list that hit. */
/* What the vertex program needs to know about this primitive, which is the
 * subset of `pi` and `sl` its text and its parameters depend on.  Filled per
 * draw rather than per vertex, so it is a dozen stores against 54 vertices. */
static void fill_xf_desc(GxXfDesc* d, const u8* s) {
    int t;
    d->pos_mtx = sub_posm;
    d->nrm_mtx = sub_nrmm;
    d->have_nrm = sl.off_nrm >= 0;
    d->chan_mode = pi.chan_mode;
    d->ntexgen = pi.ntexgen;
    d->base = s;
    d->stride = sl.stride;
    d->off_nrm = sl.off_nrm >= 0 ? sl.off_nrm : 0;
    d->off_clr = sl.off_clr;
    d->off_tex = sl.off_tex;
    d->ntex = sl.ntex;
    d->off_skin = sl.off_skin;
    d->hilite = (gx_hilite_stage >= 0 && pi.chan_mode == 2) ? gx_hilite_mode : 0;
    d->pal = (const f32(*)[4])pal;
    d->pal_n = (palette_on && sl.off_skin >= 0) ? pal_slots : 0;
    d->pal_dirty_lo = pal_dirty_lo;
    d->pal_dirty_hi = pal_dirty_hi;
    for (t = 0; t < GX_TEXCOORDS; t++) {
        d->tg[t].src_kind = pi.tg[t].src_kind;
        d->tg[t].src_k = pi.tg[t].src_k;
        d->tg[t].divide = pi.tg[t].divide;
        /* through gx, not pi's pointer: a deferred submit (M22) reads the
         * batch's snapshot, and the live slot may hold the next object's */
        d->tg[t].mtx = pi.tg[t].mtx ? gx.tex_mtx[pi.tg[t].mtx_slot] : NULL;
    }
}

/* ---- the batch (M16, PLAN.md 31) ------------------------------------------
 *
 * Before M16 every GX primitive was its own `glDrawArrays`, preceded by the
 * whole per-draw state walk -- the vertex-program key, the transform, the
 * raster state, the TEV chain, the six units' array pointers -- even though a
 * display list cannot change any GX state between its primitives.  A list's
 * primitives now accumulate as *segments* over one contiguous span of the
 * ring, and the span is submitted once: one state walk, one set of array
 * pointers, one range flush, and then the segments are issued with as few GL
 * calls as their shapes allow.  Contiguous list primitives of one type
 * (triangles, quads, lines, points) merge into one call; strips and fans of
 * one type go down `glMultiDrawArraysEXT`.  Nothing about any triangle
 * changes: the same vertices in the same order with the same state, which is
 * why the frame md5s are expected to hold and are checked (PLAN.md 31).
 *
 * The batch is flushed at the end of the list, when the layout changes
 * inside one, when the ring wraps, and when it is full.  `--oldsubmit` flushes
 * after every primitive and issues one call per segment, which is the pre-M16
 * shape on the same ring. */
typedef struct Seg {
    u32 first;   /* vertex index from the batch's base */
    u32 count;
    u8 prim;
} Seg;
#define BATCH_MAX 1024
static Seg batch[BATCH_MAX];
static int batch_n;
int gx_batch_pending; /* == batch_n != 0, for GX_STATE_TOUCH's one-load test */
static size_t batch_pos;   /* ring offset of the batch's first vertex */
static u32 batch_verts;
static Layout batch_sl;

/* ---- the CPU pre-transform (M22, PLAN.md 37) --------------------------------
 *
 * M21 counted it (§36.4): 61% of the walk's batches differ from the batch
 * before them in nothing but the position and normal matrices, and half of
 * all batches carry sixteen vertices or fewer -- a quad, a sprite, a small
 * prop -- each costing the driver the same ~25 us of per-draw validation as
 * a thousand-vertex one.  `GXLoadPosMtxImm` (a new object) was what ended
 * them.  The palette (M18) would have spanned them on the card and is
 * software on this driver (§33.2).
 *
 * So the span is made on the CPU instead.  A batch keeps *its own copy* of
 * the matrices it was decoded under (`batch_posm`, `batch_nrmm`: the matrix
 * loads no longer end it, so gx.pos_mtx[] may move under a pending batch),
 * and an object that arrives with different matrices, the same layout and
 * the same state -- every other setter still ends the batch -- is, when it
 * is small enough, transformed after its decode from its own model space
 * into the batch's:
 *
 *     pos' = inv(M_batch) * M_obj * pos       nrm' = inv(N_batch) * N_obj * nrm
 *
 * so the card, applying M_batch to pos', lands where M_obj would have put
 * pos.  Everything downstream is view-space and unchanged: the lighting
 * (the light positions are view-space parameters), the specular fold, the
 * fog coordinate, and the position/normal texgens, which the program reads
 * from `vp`/`nr` after the transform.  The running batch's own vertices are
 * never touched, so a batch nothing merged into is submitted exactly as
 * before; only the merged object's vertices carry the three roundings
 * (the inverse, the composite, the CPU's fmadd against the card's DP4),
 * which the md5s of PLAN.md 37 are the account of.  The alternative --
 * both objects into view space under an identity modelview -- would have
 * touched the running batch retroactively, and it can be a thousand
 * vertices deep when a four-vertex sprite arrives.
 *
 * The limit is per *source object*, cumulative: an object's primitives keep
 * merging until the object has put `--premerge-max` vertices into the
 * batch; the next one flushes and the rest of the object is a batch of
 * its own, which can then be merged *into*.  A singular batch matrix (a
 * scale of zero, which the game uses to hide things) refuses the merge
 * rather than divide by it.
 *
 * Measured on the 9,000-frame walk (PLAN.md 37) and *off*: at 32 vertices it
 * merges 138K of the walk's 1.94M batches and takes back 57K of 2.81M GL
 * draw calls, at 256 it merges 161K for 67K calls and moves the board's
 * md5, and neither arm is faster than the eager control -- the driver's
 * cost is per draw call and per vertex, and a batch that ends without a
 * GL state change was never the expensive kind.  `--premerge-max N` turns
 * it on. */
static f32 batch_posm[12];    /* the pending batch's matrices, its own copies */
static f32 batch_nrmm[9];
static int batch_inv_state;   /* 0 not yet asked, 1 inverted, -1 singular      */
static f32 batch_inv_pos[12], batch_inv_nrm[9];
static int merge_this;        /* the primitive in hand joins through the pre-transform */
static f32 merge_c[12], merge_cn[9];       /* inv(batch) * object                 */
static f32 merge_src_posm[12], merge_src_nrmm[9]; /* the object merge_c was made for */
static int merge_src_valid;
static u32 merge_src_verts;   /* what that object has put into the batch so far */

/* ---- the lazy flush (M22, PLAN.md 37) -----------------------------------------
 *
 * The first M22 build let the matrix loads through and merged nothing: on
 * the title, of 2,465 batches that had the same state as the batch before
 * them, 1,802 had been ended by GXInitTexObj, and once that was silenced
 * the same pairs were ended by GXSetTexCoordGen2, then GXLoadTexMtxImm --
 * hsfdraw.c's material setup writes texgen 0 to the identity and then to
 * TEXMTX0, resets all sixteen konst selects and sets them again, and so
 * on: a sequence that passes through states no primitive is ever drawn
 * under.  A setter that flushes at once cannot know that the state is
 * coming back.
 *
 * The second build snapshotted the whole GXState at the first setter and
 * submitted under the snapshot: exact, and slower than the batches it
 * saved (6 KB copied 1.16M times on the walk).  So the setters do not
 * copy anything.  The first one after a batch's last primitive runs the
 * *state half* of the submit right then -- draw_apply: the transform, the
 * raster state, the TEV, the texture binds, the program and its
 * parameters, all under the state the batch was decoded under, which is
 * still the state -- and records the few hundred bytes the submit read
 * (`SubmitRec`).  The next primitive captures the same bytes again and
 * compares.  The same: the batch goes on, and nothing about GL has to
 * move.  Different: the batch's draw calls are issued now (draw_issue),
 * under the GL state that is still its own because no GL call has
 * happened since the apply, and the primitive starts a new batch.  The
 * only extra work over the eager flush is one capture and one compare
 * per touched batch; the state walk ran once either way. */
typedef struct BatchCtx {
    int hilite_stage, hilite_mode;
    int out_stride, out_off_clr, out_off_tex, out_ntex;
} BatchCtx;
static BatchCtx bctx;         /* the pending batch's, taken at its first primitive */

#define SUBMIT_REC_MAX 2048
typedef struct SubmitRec {
    u32 len;
    u8 bytes[SUBMIT_REC_MAX];
} SubmitRec;
static SubmitRec rec_batch;   /* what the pending batch's submit read       */
static SubmitRec rec_now;
static int batch_applied;     /* draw_apply has run for the pending batch   */
static const char* applied_who;

static void rec_put(SubmitRec* r, const void* p, size_t n) {
    if (r->len + n <= SUBMIT_REC_MAX) {
        memcpy(r->bytes + r->len, p, n);
    }
    r->len += (u32)n; /* past the end the record is "too long", never equal */
}

/* Everything draw_apply and its callees read of gx: not the descriptor,
 * the arrays or the position/normal matrices (the layout compare and the
 * pre-transform own those), not the copy setup, and of the indexed tables
 * only the entries in use. */
static void submit_rec_capture_body(SubmitRec* r, int with_tex) {
    int i;
    u8 lmask;
    r->len = 0;
#define PUT(f) rec_put(r, &gx.f, sizeof(gx.f))
    PUT(proj); PUT(proj_type); PUT(vp); PUT(scissor); PUT(cull);
    PUT(num_chans); PUT(chan);
    lmask = (u8)(gx.chan[0].light_mask | gx.chan[1].light_mask | gx.chan[2].light_mask |
                 gx.chan[3].light_mask);
    for (i = 0; i < 8; i++) {
        if (lmask & (1u << i)) {
            PUT(light[i]);
        }
    }
    PUT(num_texgens);
    for (i = 0; i < gx.num_texgens && i < GX_TEXCOORDS; i++) {
        u32 m = gx.texgen[i].mtx;
        PUT(texgen[i]);
        if (m >= GX_TEXMTX0 && m < GX_IDENTITY && (m - GX_TEXMTX0) / 3 < 20) {
            PUT(tex_mtx[(m - GX_TEXMTX0) / 3]);
        }
    }
    PUT(num_tev); PUT(num_ind);
    for (i = 0; i < gx.num_tev && i < GX_TEV_STAGES; i++) {
        unsigned u = gx.tev[i].map;
        PUT(tev[i]);
        PUT(ind_tile[i]);
        if (u < GX_TEX_UNITS && gx_bound_tex(u)) {
            if (with_tex) {
                rec_put(r, &gx.bound[u], offsetof(GXTexObjPort, gl_name));
                if (gx.bound[u].is_ci && gx.bound[u].tlut_name < 64) {
                    PUT(tlut[gx.bound[u].tlut_name]);
                }
            } else {
                /* M37: the atlas view -- what two draws must share for one
                 * page of a texture atlas to serve both (PLAN.md 52) */
                PUT(bound[u].format); PUT(bound[u].wrap_s); PUT(bound[u].wrap_t);
                PUT(bound[u].min_filt); PUT(bound[u].mag_filt); PUT(bound[u].is_ci);
            }
        } else {
            rec_put(r, &i, sizeof(i)); /* "unit has nothing" */
        }
    }
    PUT(tev_reg); PUT(kcolor); PUT(swap_tbl); PUT(ind);
    PUT(z_enable); PUT(z_func); PUT(z_update); PUT(z_comploc);
    PUT(blend_mode); PUT(blend_src); PUT(blend_dst); PUT(blend_logic);
    PUT(alpha_comp0); PUT(alpha_ref0); PUT(alpha_op); PUT(alpha_comp1); PUT(alpha_ref1);
    PUT(color_update); PUT(alpha_update);
    PUT(fog_type); PUT(fog_startz); PUT(fog_endz); PUT(fog_nearz); PUT(fog_farz); PUT(fog_color);
#undef PUT
}

static void submit_rec_capture(SubmitRec* r) { submit_rec_capture_body(r, 1); }

static int submit_rec_differs(void) {
    submit_rec_capture(&rec_now);
    return rec_now.len != rec_batch.len || rec_now.len > SUBMIT_REC_MAX ||
           memcmp(rec_now.bytes, rec_batch.bytes, rec_now.len) != 0;
}

static int flusher_slot(const char* who) {
    int i;
    for (i = 0; i < FLUSHER_SLOTS; i++) {
        if (flushers[i].who == who) {
            return i;
        }
        if (!flushers[i].who) {
            flushers[i].who = who;
            return i;
        }
    }
    return -1;
}

static int batch_apply_now(void);
static void batch_flush(void);

void gx_batch_touch(const char* who) {
    if (!lazy_on) {
        gx_batch_flush_from(who);
        return;
    }
    if (batch_applied) {
        return; /* the state is in GL and the record is taken */
    }
    port_perf_gx_begin();
    if (batch_apply_now()) {
        submit_rec_capture(&rec_batch);
        batch_applied = 1;
        applied_who = who;
        stat_lazy_snaps++;
    } else {
        /* a batch the apply could not hold (the CPU fallback needs its
         * final vertex count): the eager shape for this one */
        if (port_opt.submitstats) {
            int i = flusher_slot(who);
            if (i >= 0) {
                flushers[i].n++;
                pending_flusher = i;
            }
        }
        end_who = who;
        batch_flush();
    }
    port_perf_gx_end();
}

static void pm_source_done(void) {
    if (merge_src_valid && merge_src_verts) {
        stat_pm_hist[merge_src_verts <= 4 ? 0 : merge_src_verts <= 8 ? 1
                     : merge_src_verts <= 16 ? 2 : 3]++;
    }
    merge_src_valid = 0;
    merge_src_verts = 0;
}

/* The batch just started with the primitive in hand: its matrices are the
 * primitive's, and nothing is inverted or merged yet. */
static void pm_batch_start(void) {
    memcpy(batch_posm, pi.pos_mtx, sizeof(batch_posm));
    memcpy(batch_nrmm, pi.nrm_mtx, sizeof(batch_nrmm));
    batch_inv_state = 0;
    pm_source_done();
    batch_applied = 0;
    /* this primitive's PrimInv becomes the batch's; the next primitive is
     * settled into the other buffer */
    bctx_pi = pi_cur;
    pi_cur = (pi_cur == &pi_bufs[0]) ? &pi_bufs[1] : &pi_bufs[0];
    bctx.hilite_stage = gx_hilite_stage;
    bctx.hilite_mode = gx_hilite_mode;
    bctx.out_stride = out_stride;
    bctx.out_off_clr = out_off_clr;
    bctx.out_off_tex = out_off_tex;
    bctx.out_ntex = out_ntex;
}

/* a 3x4 affine inverse (rows of 4), in double, out in f32; 0 when singular */
static int pm_inv_affine(const f32* m, f32* out) {
    double a00 = m[0], a01 = m[1], a02 = m[2], t0 = m[3];
    double a10 = m[4], a11 = m[5], a12 = m[6], t1 = m[7];
    double a20 = m[8], a21 = m[9], a22 = m[10], t2 = m[11];
    double c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
    double c10 = a12 * a20 - a10 * a22, c11 = a00 * a22 - a02 * a20, c12 = a02 * a10 - a00 * a12;
    double c20 = a10 * a21 - a11 * a20, c21 = a01 * a20 - a00 * a21, c22 = a00 * a11 - a01 * a10;
    double det = a00 * c00 + a01 * c10 + a02 * c20;
    double r;
    if (!(det > 1e-30 || det < -1e-30)) {
        return 0; /* singular, or NaN */
    }
    r = 1.0 / det;
    c00 *= r; c01 *= r; c02 *= r;
    c10 *= r; c11 *= r; c12 *= r;
    c20 *= r; c21 *= r; c22 *= r;
    out[0] = (f32)c00; out[1] = (f32)c01; out[2] = (f32)c02;
    out[3] = (f32)-(c00 * t0 + c01 * t1 + c02 * t2);
    out[4] = (f32)c10; out[5] = (f32)c11; out[6] = (f32)c12;
    out[7] = (f32)-(c10 * t0 + c11 * t1 + c12 * t2);
    out[8] = (f32)c20; out[9] = (f32)c21; out[10] = (f32)c22;
    out[11] = (f32)-(c20 * t0 + c21 * t1 + c22 * t2);
    return 1;
}

static int pm_inv_3x3(const f32* m, f32* out) {
    double a00 = m[0], a01 = m[1], a02 = m[2];
    double a10 = m[3], a11 = m[4], a12 = m[5];
    double a20 = m[6], a21 = m[7], a22 = m[8];
    double c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
    double c10 = a12 * a20 - a10 * a22, c11 = a00 * a22 - a02 * a20, c12 = a02 * a10 - a00 * a12;
    double c20 = a10 * a21 - a11 * a20, c21 = a01 * a20 - a00 * a21, c22 = a00 * a11 - a01 * a10;
    double det = a00 * c00 + a01 * c10 + a02 * c20;
    double r;
    if (!(det > 1e-30 || det < -1e-30)) {
        return 0;
    }
    r = 1.0 / det;
    out[0] = (f32)(c00 * r); out[1] = (f32)(c01 * r); out[2] = (f32)(c02 * r);
    out[3] = (f32)(c10 * r); out[4] = (f32)(c11 * r); out[5] = (f32)(c12 * r);
    out[6] = (f32)(c20 * r); out[7] = (f32)(c21 * r); out[8] = (f32)(c22 * r);
    return 1;
}

/* c = a * b for 3x4 affines (a's rows of 4 applied after b's) */
static void pm_compose_affine(const f32* a, const f32* b, f32* c) {
    int r;
    for (r = 0; r < 3; r++) {
        const f32* ar = a + 4 * r;
        c[4 * r + 0] = (f32)((double)ar[0] * b[0] + (double)ar[1] * b[4] + (double)ar[2] * b[8]);
        c[4 * r + 1] = (f32)((double)ar[0] * b[1] + (double)ar[1] * b[5] + (double)ar[2] * b[9]);
        c[4 * r + 2] = (f32)((double)ar[0] * b[2] + (double)ar[1] * b[6] + (double)ar[2] * b[10]);
        c[4 * r + 3] = (f32)((double)ar[0] * b[3] + (double)ar[1] * b[7] + (double)ar[2] * b[11] + ar[3]);
    }
}

static void pm_compose_3x3(const f32* a, const f32* b, f32* c) {
    int r;
    for (r = 0; r < 3; r++) {
        const f32* ar = a + 3 * r;
        c[3 * r + 0] = (f32)((double)ar[0] * b[0] + (double)ar[1] * b[3] + (double)ar[2] * b[6]);
        c[3 * r + 1] = (f32)((double)ar[0] * b[1] + (double)ar[1] * b[4] + (double)ar[2] * b[7]);
        c[3 * r + 2] = (f32)((double)ar[0] * b[2] + (double)ar[1] * b[5] + (double)ar[2] * b[8]);
    }
}

/* batch_prepare found a pending batch of this layout whose matrices are not
 * the primitive's.  Merge, or not? */
static int pm_decide(u32 count) {
    int need_nrm = sl.off_nrm >= 0;
    if (count > (u32)port_opt.premerge_max || batch_verts + count > MAX_VERTS) {
        stat_pm_refused_big++;
        return 0;
    }
    if (batch_inv_state == 0) {
        batch_inv_state = (pm_inv_affine(batch_posm, batch_inv_pos) &&
                           (!need_nrm || pm_inv_3x3(batch_nrmm, batch_inv_nrm)))
                              ? 1
                              : -1;
    }
    if (batch_inv_state < 0) {
        stat_pm_refused_singular++;
        return 0;
    }
    if (!merge_src_valid || memcmp(merge_src_posm, pi.pos_mtx, sizeof(merge_src_posm)) != 0 ||
        (need_nrm && memcmp(merge_src_nrmm, pi.nrm_mtx, sizeof(merge_src_nrmm)) != 0)) {
        /* a new source object: its composite, and the last one's count */
        pm_source_done();
        memcpy(merge_src_posm, pi.pos_mtx, sizeof(merge_src_posm));
        memcpy(merge_src_nrmm, pi.nrm_mtx, sizeof(merge_src_nrmm));
        pm_compose_affine(batch_inv_pos, pi.pos_mtx, merge_c);
        if (need_nrm) {
            pm_compose_3x3(batch_inv_nrm, pi.nrm_mtx, merge_cn);
        }
        merge_src_valid = 1;
        merge_src_verts = 0;
    }
    if (merge_src_verts + count > (u32)port_opt.premerge_max) {
        stat_pm_refused_big++;
        return 0;
    }
    return 1;
}

/* the primitive's `n` decoded vertices at `v`, from its model space into the
 * batch's; the same arithmetic as finish_vertices' position row */
static void pm_apply(u8* v, u32 n) {
    const f32* c = merge_c;
    const f32* cn = merge_cn;
    const int stride = sl.stride, off_nrm = sl.off_nrm;
    u32 i;
    for (i = 0; i < n; i++, v += stride) {
        f32* p = (f32*)v;
        float x = p[0], y = p[1], z = p[2];
        p[0] = c[0] * x + c[1] * y + c[2] * z + c[3];
        p[1] = c[4] * x + c[5] * y + c[6] * z + c[7];
        p[2] = c[8] * x + c[9] * y + c[10] * z + c[11];
        if (off_nrm >= 0) {
            f32* q = (f32*)(v + off_nrm);
            float nx = q[0], ny = q[1], nz = q[2];
            q[0] = cn[0] * nx + cn[1] * ny + cn[2] * nz;
            q[1] = cn[3] * nx + cn[4] * ny + cn[5] * nz;
            q[2] = cn[6] * nx + cn[7] * ny + cn[8] * nz;
        }
    }
    if (merge_src_verts == 0) {
        stat_pm_objects++;
    }
    merge_src_verts += n;
    stat_pm_prims++;
    stat_pm_verts += n;
}

/* ---- the batch's matrix palette (M18, PLAN.md 33) --------------------------
 *
 * Before M18 a batch had one position matrix, so `GXLoadPosMtxImm` -- a new
 * object -- ended it: 693K of the walk's 1.96M batches (§31.2), and the
 * driver's per-batch validation was 17.5% of a drawn frame (§32.3).  Now the
 * matrices live in the vertex program's parameter block as a palette of
 * `pal_slots` (position 3x4, normal 3x3: GX_PAL_STRIDE params each), every
 * vertex carries its slot (`Layout.off_skin`), and a matrix load changes
 * nothing the pending batch depends on.  A batch ends when the palette is
 * full instead.
 *
 * A *skinned* mesh places all of its envelope entries at once, premultiplied
 * by the object's own matrices, so the card does one multiply per vertex --
 * the same shape as a plain object, whose one slot is its (pos, nrm) pair.
 *
 * Placement happens *before* a primitive is decoded and after every flush
 * that could precede its decode (layout, limits, ring wrap), so no vertex is
 * ever written with a slot of a palette that is then thrown away. */
/* ---- the palette as a cache ---------------------------------------------------
 *
 * The first M18 build gave every batch a fresh palette, uploaded whole: the
 * parameter uploads went from 340 a frame to 3,100 and the walk got slower
 * (PLAN.md 33.2).  The parameters *persist* on the card, so the palette is a
 * cache now: a slot keeps its matrix until something evicts it, a batch pins
 * the slots it uses (an eviction of a pinned slot would re-address vertices
 * already decoded), a primitive whose matrix is resident costs nothing, and
 * the upload per batch is one call over the dirty range
 * (GL_EXT_gpu_program_parameters).  A batch ends on the palette only when
 * every slot is pinned. */
static void pal_reset(void) {
    /* a new batch: nothing is pinned any more; the contents stay */
    pal_batch_serial++;
    pal_n = pal_slots; /* every slot is addressable now */
    pal_dirty_lo = GX_PAL_STRIDE * PAL_SLOTS_MAX;
    pal_dirty_hi = -1;
}

static void pal_mark_dirty(int slot) {
    int lo = GX_PAL_STRIDE * slot, hi = lo + GX_PAL_STRIDE - 1;
    if (lo < pal_dirty_lo) {
        pal_dirty_lo = lo;
    }
    if (hi > pal_dirty_hi) {
        pal_dirty_hi = hi;
    }
}

static void pal_rows_plain(int slot, const f32* pm, const f32* nm) {
    f32(*r)[4] = pal[slot];
    r[0][0] = pm[0]; r[0][1] = pm[1]; r[0][2] = pm[2]; r[0][3] = pm[3];
    r[1][0] = pm[4]; r[1][1] = pm[5]; r[1][2] = pm[6]; r[1][3] = pm[7];
    r[2][0] = pm[8]; r[2][1] = pm[9]; r[2][2] = pm[10]; r[2][3] = pm[11];
    r[3][0] = nm[0]; r[3][1] = nm[1]; r[3][2] = nm[2]; r[3][3] = 0.0f;
    r[4][0] = nm[3]; r[4][1] = nm[4]; r[4][2] = nm[5]; r[4][3] = 0.0f;
    r[5][0] = nm[6]; r[5][1] = nm[7]; r[5][2] = nm[8]; r[5][3] = 0.0f;
    pal_mark_dirty(slot);
}

/* slot = (object position matrix) x P_e,  (object normal matrix) x N_e */
static void pal_rows_skin(int slot, const f32* pm, const f32* nm, const Mtx P, const Mtx N) {
    f32(*r)[4] = pal[slot];
    Mtx out;
    int i, j;
    PSMTXConcat((const f32(*)[4])pm, P, out);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            r[i][j] = out[i][j];
        }
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            r[3 + i][j] = nm[i * 3 + 0] * N[0][j] + nm[i * 3 + 1] * N[1][j] +
                          nm[i * 3 + 2] * N[2][j];
        }
        r[3 + i][3] = 0.0f;
    }
    pal_mark_dirty(slot);
}

static void batch_flush(void);

/* A slot to fill: a free one, else the least recently used unpinned one;
 * -1 when every slot is pinned by the pending batch. */
static int pal_victim(void) {
    int i, best = -1;
    for (i = 0; i < pal_slots; i++) {
        PalSlot* ps = &pal_slot[i];
        if (ps->pinned == pal_batch_serial) {
            continue;
        }
        if (ps->kind == 0) {
            return i;
        }
        if (best < 0 || ps->used < pal_slot[best].used) {
            best = i;
        }
    }
    return best;
}

static void pal_flush_for_room(void) {
    Layout cur = sl;
    sl = batch_sl;
    end_who = "palette:room";
    batch_flush();
    sl = cur;
    stat_pal_flushes++;
}

/* The primitive about to be decoded, for the skinned pre-scan: its list bytes
 * and vertex count (set by GXCallDisplayList before pal_place). */
static const u8* pal_scan_p;
static u32 pal_scan_count;

/* Give the primitive in hand its slot(s).  May flush the pending batch (every
 * slot is pinned); the caller has decoded nothing yet.
 *
 * A skinned mesh's entries are placed *as the primitives need them* -- a
 * character's body has 43-50 entries (PLAN.md 33.1) and the palette 24 -- so
 * the mesh keeps a map entry -> slot (`ent_slotf`, checked against the slot's
 * own key since another object may have evicted it), the primitive's index
 * list is scanned for the entries it touches, and the ones not resident get
 * a slot. */
static void pal_place(void) {
    const f32* pm = pi.pos_mtx;
    const f32* nm = pi.nrm_mtx;
    int k;
    if (!palette_on) {
        return;
    }
    pal_clock++;
    if (pi.skin) {
        SkinMesh* m = pi.skin;
        int e, per = 0, need = 0, pass;
        const u8* p = pal_scan_p;
        u32 i;
        int needed[PAL_SLOTS_MAX + 1];
        int nneeded = 0;
        for (k = 0; k < nplan; k++) {
            per += plan[k].advance;
        }
        if (!p || per <= 0 || nplan < 1 || plan[0].attr != GX_VA_POS || plan[0].idx != 2) {
            /* not a shape the window can scan; should not happen (the lookup
             * requires the list shape) -- counted, drawn with slot 0 */
            stat_pal_overflow++;
            for (e = 0; e < m->nent; e++) {
                m->ent_slotf[e] = 0.0f;
            }
            return;
        }
        /* the map is per object matrix and pose: a different one means none
         * of the resident entries is this primitive's */
        if (m->map_pose != m->pose_serial || memcmp(m->map_posm, pm, 48) != 0 ||
            memcmp(m->map_nrmm, nm, 36) != 0) {
            for (e = 0; e < m->nent; e++) {
                m->ent_slotf[e] = -1.0f;
            }
            m->map_pose = m->pose_serial;
            memcpy(m->map_posm, pm, 48);
            memcpy(m->map_nrmm, nm, 36);
        }
        /* the distinct entries this primitive touches */
        for (i = 0; i < pal_scan_count; i++) {
            u32 ix = ((u32)p[i * per] << 8) | p[i * per + 1];
            int j, seen = 0;
            if (ix >= (u32)m->nvtx) {
                continue;
            }
            e = m->pos_ent[ix];
            for (j = 0; j < nneeded; j++) {
                if (needed[j] == e) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && nneeded <= PAL_SLOTS_MAX) {
                needed[nneeded++] = e;
            }
        }
        stat_pal_scan_hist[nneeded <= 4 ? (nneeded ? nneeded - 1 : 0)
                           : nneeded <= 8 ? 4 : nneeded <= 16 ? 5 : nneeded <= 24 ? 6 : 7]++;
        for (pass = 0; pass < 2; pass++) {
            /* pin what is resident, count what is not */
            need = 0;
            for (k = 0; k < nneeded; k++) {
                e = needed[k];
                if (m->ent_slotf[e] >= 0.0f) {
                    int slot = (int)m->ent_slotf[e] / GX_PAL_STRIDE;
                    PalSlot* ps = &pal_slot[slot];
                    if (ps->kind == 2 && ps->mesh == m && ps->ent == e &&
                        ps->pose == m->pose_serial && memcmp(ps->posm, pm, 48) == 0 &&
                        memcmp(ps->nrmm, nm, 36) == 0) {
                        ps->pinned = pal_batch_serial;
                        ps->used = pal_clock;
                        continue;
                    }
                    m->ent_slotf[e] = -1.0f; /* evicted under us */
                }
                need++;
            }
            if (need == 0) {
                break;
            }
            /* room: unpinned slots */
            {
                int room = 0;
                for (k = 0; k < pal_slots; k++) {
                    if (pal_slot[k].pinned != pal_batch_serial) {
                        room++;
                    }
                }
                if (room >= need || pass == 1) {
                    break;
                }
            }
            pal_flush_for_room(); /* unpins everything; contents stay */
        }
        for (k = 0; k < nneeded; k++) {
            int slot;
            e = needed[k];
            if (m->ent_slotf[e] >= 0.0f &&
                pal_slot[(int)m->ent_slotf[e] / GX_PAL_STRIDE].pinned == pal_batch_serial &&
                pal_slot[(int)m->ent_slotf[e] / GX_PAL_STRIDE].mesh == m &&
                pal_slot[(int)m->ent_slotf[e] / GX_PAL_STRIDE].ent == e) {
                continue; /* pinned above */
            }
            slot = pal_victim();
            if (slot < 0) {
                m->ent_slotf[e] = 0.0f; /* more entries than slots: wrong, counted */
                stat_pal_overflow++;
                continue;
            }
            {
                PalSlot* ps = &pal_slot[slot];
                if (ps->kind == 2 && ps->mesh && ps->mesh->ent_slotf &&
                    ps->ent < ps->mesh->nent &&
                    ps->mesh->ent_slotf[ps->ent] == (f32)(GX_PAL_STRIDE * slot)) {
                    ps->mesh->ent_slotf[ps->ent] = -1.0f; /* tell the evictee */
                }
                pal_rows_skin(slot, pm, nm, m->P[e].m, m->N[e].m);
                ps->kind = 2;
                ps->mesh = m;
                ps->ent = e;
                ps->pose = m->pose_serial;
                memcpy(ps->posm, pm, 48);
                memcpy(ps->nrmm, nm, 36);
                ps->pinned = pal_batch_serial;
                ps->used = pal_clock;
                m->ent_slotf[e] = (f32)(GX_PAL_STRIDE * slot);
                stat_pal_fills++;
            }
        }
        stat_pal_skin++;
        return;
    }
    /* a plain object: resident already? */
    for (k = 0; k < pal_slots; k++) {
        PalSlot* ps = &pal_slot[k];
        if (ps->kind == 1 && memcmp(ps->posm, pm, 48) == 0 && memcmp(ps->nrmm, nm, 36) == 0) {
            ps->pinned = pal_batch_serial;
            ps->used = pal_clock;
            pi.slotf = (f32)(GX_PAL_STRIDE * k);
            stat_pal_reused++;
            return;
        }
    }
    k = pal_victim();
    if (k < 0) {
        pal_flush_for_room();
        k = pal_victim();
    }
    {
        PalSlot* ps = &pal_slot[k];
        if (ps->kind == 2 && ps->mesh && ps->mesh->ent_slotf && ps->ent < ps->mesh->nent &&
            ps->mesh->ent_slotf[ps->ent] == (f32)(GX_PAL_STRIDE * k)) {
            ps->mesh->ent_slotf[ps->ent] = -1.0f;
        }
        pal_rows_plain(k, pm, nm);
        ps->kind = 1;
        ps->mesh = NULL;
        ps->ent = -1;
        memcpy(ps->posm, pm, 48);
        memcpy(ps->nrmm, nm, 36);
        ps->pinned = pal_batch_serial;
        ps->used = pal_clock;
        pi.slotf = (f32)(GX_PAL_STRIDE * k);
        stat_pal_plain++;
        stat_pal_fills++;
    }
}

static void draw_submit(const u8* s, int n, const Seg* segs, int nsegs, int in_ring);
static int draw_apply(const u8* s, int n, int in_ring);
static void draw_issue(const u8* s, int n, const Seg* segs, int nsegs, int in_ring);

static void ring_ensure(void) {
    if (!src_buf) {
        src_buf = gl13_var_setup(VRING_BYTES);
        src_cap = VRING_BYTES;
    }
}
/* M27: the ring before the render thread owns GL (gl13_var_setup probes
 * extensions and generates the fences on the main thread) */
void gx_draw_ring_ensure(void) { ring_ensure(); }

/* M21 (--submitstats): how many batches differ from the one before them in
 * nothing but the matrices -- the same layout, TEV config, textures, raster
 * state and channel colours -- and how many vertices they carry.  That is
 * the batch count a CPU pre-transform of small objects (or any other way of
 * spanning GXLoadPosMtxImm) could take back; PLAN.md 36. */
static u32 last_batch_sig, last_batch_sig_notex;
static Layout last_batch_sl;
/* M23 (PLAN.md 38, the atlas question): batches that differ from the one
 * before in the matrices *and the textures' identity* alone -- same format,
 * wrap and filter, another image -- which is what a runtime atlas of the
 * sprite cache could join, over and above the M21 count. */

static u32 batch_state_sig_body(int with_tex) {
    u32 h = gx_tev_last_sig();
    const u8* p;
    size_t n;
    int i;
#define SIG_MIX(ptr, len) do { p = (const u8*)(ptr); n = (size_t)(len); while (n--) { h = (h ^ *p++) * 16777619u; } } while (0)
    for (i = 0; i < GX_TEV_STAGES && i < gx.num_tev; i++) {
        /* M22: the object's *contents* (M21 mixed in gx_bound_tex's return,
         * which is &gx.bound[unit] -- one address per unit, whatever is
         * loaded -- so its count never saw a texture change: PLAN.md 37) */
        GXTexObjPort* t = gx_bound_tex(gx.tev[i].map);
        if (t) {
            if (with_tex) {
                SIG_MIX(t, offsetof(GXTexObjPort, gl_name));
                if (t->is_ci && t->tlut_name < 64) {
                    SIG_MIX(&gx.tlut[t->tlut_name], sizeof(gx.tlut[0]));
                }
            } else {
                /* the atlas view: what an atlas page must share */
                SIG_MIX(&t->format, sizeof(t->format));
                SIG_MIX(&t->wrap_s, sizeof(t->wrap_s));
                SIG_MIX(&t->wrap_t, sizeof(t->wrap_t));
                SIG_MIX(&t->min_filt, sizeof(t->min_filt));
                SIG_MIX(&t->mag_filt, sizeof(t->mag_filt));
                SIG_MIX(&t->is_ci, sizeof(t->is_ci));
            }
        } else {
            SIG_MIX(&i, sizeof(i));
        }
    }
    SIG_MIX(gx.tev_reg, sizeof(gx.tev_reg));
    SIG_MIX(gx.kcolor, sizeof(gx.kcolor));
    SIG_MIX(&gx.z_enable, sizeof(gx.z_enable));
    SIG_MIX(&gx.z_func, sizeof(gx.z_func));
    SIG_MIX(&gx.z_update, sizeof(gx.z_update));
    SIG_MIX(&gx.blend_mode, sizeof(gx.blend_mode));
    SIG_MIX(&gx.blend_src, sizeof(gx.blend_src));
    SIG_MIX(&gx.blend_dst, sizeof(gx.blend_dst));
    SIG_MIX(&gx.alpha_comp0, sizeof(gx.alpha_comp0));
    SIG_MIX(&gx.cull, sizeof(gx.cull));
    SIG_MIX(gx.chan, sizeof(gx.chan));
    SIG_MIX(&gx.num_chans, sizeof(gx.num_chans));
    SIG_MIX(&gx.num_texgens, sizeof(gx.num_texgens));
    SIG_MIX(gx.texgen, sizeof(gx.texgen));
#undef SIG_MIX
    return h;
}
static u32 batch_state_sig(void) { return batch_state_sig_body(1); }

static void batch_flush(void) {
    if (batch_n) {
        /* cleared before the submit, so a GX_STATE_TOUCH reached from inside
         * it (there is none today) cannot submit the same batch twice */
        int n = batch_n;
        u32 nv = batch_verts;
        /* M22: the submit runs under the batch's own context -- the last
         * primitive's may be the next object's (a deferred flush from
         * batch_prepare, or a flush from a state setter after
         * begin_attr_order ran for a primitive the batch never got) */
        PrimInv* pi_save = pi_cur;
        Layout sl_save = sl;
        int hs_save = gx_hilite_stage, hm_save = gx_hilite_mode;
        int os_save = out_stride, oc_save = out_off_clr, ot_save = out_off_tex, on_save = out_ntex;
        int applied = batch_applied;
        batch_n = 0;
        batch_verts = 0;
        gx_batch_pending = 0;
        batch_applied = 0;
        pi_cur = bctx_pi;
        sl = batch_sl;
        gx_hilite_stage = bctx.hilite_stage;
    gx_hilite_mode = bctx.hilite_mode;
        out_stride = bctx.out_stride;
        out_off_clr = bctx.out_off_clr;
        out_off_tex = bctx.out_off_tex;
        out_ntex = bctx.out_ntex;
        unsigned calls0 = stat_draws;
        if (applied) {
            /* the state walk ran at the setter (gx_batch_touch); only the
             * draw calls are left, and nothing has touched GL since */
            draw_issue(src_buf + batch_pos, (int)nv, batch, n, 1);
            stat_lazy_flushes++;
        } else {
            draw_submit(src_buf + batch_pos, (int)nv, batch, n, 1);
        }
        if (endlog_armed()) {
            /* the state is still the batch's: a setter touches before it
             * changes anything, and batch_prepare has not written yet */
            static unsigned endlog_n;
            u32 rec, notex, lay, mtx;
            const GXTexObjPort* t0 = gx.num_tev ? gx_bound_tex(gx.tev[0].map) : NULL;
            submit_rec_capture_body(&rec_now, 1);
            rec = endlog_fnv(rec_now.bytes,
                             rec_now.len < SUBMIT_REC_MAX ? rec_now.len : SUBMIT_REC_MAX,
                             2166136261u ^ rec_now.len);
            submit_rec_capture_body(&rec_now, 0);
            notex = endlog_fnv(rec_now.bytes,
                               rec_now.len < SUBMIT_REC_MAX ? rec_now.len : SUBMIT_REC_MAX,
                               2166136261u ^ rec_now.len);
            lay = endlog_fnv(&batch_sl, sizeof(batch_sl), 2166136261u);
            mtx = endlog_fnv(batch_posm, sizeof(batch_posm), 2166136261u);
            mtx = endlog_fnv(batch_nrmm, sizeof(batch_nrmm), mtx);
            port_log("endlog> b%-5u who=%-24s verts=%-5u segs=%-4d calls=%-3u prim=%02x "
                     "rec=%08x notex=%08x lay=%08x mtx=%08x tex=%ux%u/%u/%u",
                     ++endlog_n, end_who, (unsigned)nv, n, stat_draws - calls0,
                     n ? batch[0].prim : 0u, rec, notex, lay, mtx,
                     t0 ? (unsigned)t0->width : 0u, t0 ? (unsigned)t0->height : 0u,
                     t0 ? (unsigned)t0->format : 0u, t0 ? (unsigned)t0->gl_name : 0u);
            {
                /* every unit the batch draws under, for the atlas question:
                 * a page can only serve a draw whose texture is clamped on
                 * both axes and small enough to be a tile of one */
                int st;
                for (st = 0; st < (int)gx.num_tev && st < GX_TEV_STAGES; st++) {
                    const GXTexObjPort* t = gx_bound_tex(gx.tev[st].map);
                    if (t) {
                        port_log(" u%d=%ux%u/f%u/w%u%u/ci%u/g%u", (int)gx.tev[st].map,
                                 (unsigned)t->width, (unsigned)t->height,
                                 (unsigned)t->format, (unsigned)t->wrap_s,
                                 (unsigned)t->wrap_t, (unsigned)t->is_ci,
                                 (unsigned)t->gl_name);
                    }
                }
            }
            port_log("\n");
        }
        end_who = "?";
        pi_cur = pi_save;
        sl = sl_save;
        gx_hilite_stage = hs_save;
    gx_hilite_mode = hm_save;
        out_stride = os_save;
        out_off_clr = oc_save;
        out_off_tex = ot_save;
        out_ntex = on_save;
        stat_batches++;
        if (port_opt.submitstats) {
            u32 sig = batch_state_sig();
            u32 sig_notex = batch_state_sig_body(0);
            stat_batch_hist[nv <= 16 ? 0 : nv <= 64 ? 1 : nv <= 256 ? 2 : nv <= 1024 ? 3 : nv <= 4096 ? 4 : 5]++;
            if (sig != last_batch_sig && sig_notex == last_batch_sig_notex &&
                memcmp(&last_batch_sl, &batch_sl, sizeof(Layout)) == 0) {
                stat_mergeable_atlas++;
                stat_mergeable_atlas_verts += nv;
                if (nv <= 16) {
                    stat_mergeable_atlas_small++;
                }
            }
            last_batch_sig_notex = sig_notex;
            if (sig == last_batch_sig && memcmp(&last_batch_sl, &batch_sl, sizeof(Layout)) == 0) {
                stat_mergeable++;
                stat_mergeable_verts += nv;
                if (nv <= 256) {
                    stat_mergeable_small++;
                }
                if (last_flusher >= 0) {
                    flushers[last_flusher].transient++;
                }
            }
            last_batch_sig = sig;
            last_batch_sl = batch_sl;
            last_flusher = pending_flusher;
            pending_flusher = -1;
        }
    }
    pal_reset();
    /* the chunks the writer has finished with get their fences, after the
     * draws that read them */
    gl13_var_left(fence_from, ring_cursor, ring_wrapped);
    fence_from = ring_cursor;
    ring_wrapped = 0;
}

/* Claim `need` bytes of the ring for the run about to be decoded and return
 * its offset.  A run that does not fit before the end wraps to the start,
 * which breaks the batch (its vertices would no longer be contiguous). */
static size_t ring_claim(size_t need) {
    ring_ensure();
    if (need > src_cap) {
        need = src_cap; /* the decoder truncates to the sink past the end */
    }
    /* M21 (--fixbase): a batch's first vertex sits at a multiple of its
     * stride from the ring's start, so the batch can be addressed as an
     * index from a base that never moves (draw_submit).  A few bytes of
     * padding per batch; nothing inside a batch moves. */
    if (!batch_n && !port_opt.nofixbase && sl.stride > 0) {
        size_t rem = ring_cursor % (size_t)sl.stride;
        if (rem) {
            ring_cursor += (size_t)sl.stride - rem;
        }
    }
    if (ring_cursor + need > src_cap) {
        end_who = "ring:wrap";
        batch_flush();
        ring_cursor = 0;
        ring_wrapped = 1;
        stat_wraps++;
    }
    gl13_var_enter(ring_cursor, need);
    return ring_cursor;
}

/* M37 (PLAN.md 52): the batch's phase 2 runs for every vertex in it under
 * the *first* primitive's PrimInv and output layout (bctx / bctx_pi), so a
 * primitive whose own differ may not join it.  Until M37 that was
 * guaranteed indirectly -- every piece of state those are derived from has
 * a setter that ends the batch when it changes -- and the compare is the
 * net under the immediate-mode batching below.  An extra flush can never
 * move a pixel.
 *
 * Only the texgens below `ntexgen` are compared: begin_attr_order writes
 * `tg[]` up to that index and leaves the rest as the last primitive that
 * used this buffer left them, and `pi_cur` alternates between two buffers
 * whose stale tails differ -- a memcmp of the whole struct fails for every
 * primitive, which is what the first build of this did (972 batches on the
 * character select instead of 249). */
static int prim_ctx_differs(void) {
    int t;
    if (pi.pos_mtx != bctx_pi->pos_mtx || pi.nrm_mtx != bctx_pi->nrm_mtx ||
        pi.have_nrm != bctx_pi->have_nrm || pi.no_clr0 != bctx_pi->no_clr0 ||
        pi.chan_mode != bctx_pi->chan_mode || pi.ntexgen != bctx_pi->ntexgen ||
        pi.tex_copy_n != bctx_pi->tex_copy_n || out_stride != bctx.out_stride ||
        out_off_clr != bctx.out_off_clr || out_off_tex != bctx.out_off_tex ||
        out_ntex != bctx.out_ntex || gx_hilite_stage != bctx.hilite_stage ||
        gx_hilite_mode != bctx.hilite_mode) {
        return 1;
    }
    for (t = 0; t < pi.ntexgen && t < GX_TEXCOORDS; t++) {
        if (pi.tg[t].src_kind != bctx_pi->tg[t].src_kind ||
            pi.tg[t].src_k != bctx_pi->tg[t].src_k ||
            pi.tg[t].divide != bctx_pi->tg[t].divide ||
            pi.tg[t].mtx != bctx_pi->tg[t].mtx) {
            return 1;
        }
        /* `mtx_slot` is written only for a texgen that names a matrix, and
         * is stale otherwise -- reading it when `mtx` is NULL is what made
         * the first build of this compare fail 754 times a character-select
         * frame (the two PrimInv buffers carry different stale tails). */
        if (pi.tg[t].mtx && pi.tg[t].mtx_slot != bctx_pi->tg[t].mtx_slot) {
            return 1;
        }
    }
    return 0;
}

/* Before a primitive is decoded: if it cannot join the pending batch -- the
 * layout differs, the batch is full, --oldsubmit / --batchmax -- flush now,
 * while nothing of the primitive has been written.  M16 made this decision
 * *after* the decode, in batch_add; the palette (M18) needs it before, because
 * a vertex's slot is written during the decode and belongs to the batch the
 * primitive will end up in. */
static void batch_prepare(u32 count) {
    merge_this = 0;
    if (batch_n && batch_applied) {
        /* M22: setters ran since the batch's last primitive and its state
         * went to GL then.  Did any of them leave the state the submit
         * reads different? */
        if (submit_rec_differs()) {
            if (port_opt.submitstats) {
                pending_flusher = flusher_slot(applied_who);
                if (pending_flusher >= 0) {
                    flushers[pending_flusher].n++;
                }
            }
            end_who = applied_who; /* the setter whose change the compare found */
            batch_flush(); /* issues under the GL state that is still the batch's */
        } else {
            stat_lazy_transient++;
        }
    }
    if (batch_n && prim_ctx_differs()) {
        Layout cur = sl;
        end_who = "prepare:context";
        sl = batch_sl;
        batch_flush();
        sl = cur;
    }
    if (batch_n &&
        (port_opt.oldsubmit || batch_n >= BATCH_MAX ||
         (port_opt.batchmax && batch_n >= port_opt.batchmax) ||
         batch_verts + count > MAX_VERTS || memcmp(&batch_sl, &sl, sizeof(sl)) != 0 ||
         /* M22: the matrices moved under the batch (the loads no longer end
          * it): transform the object in, or flush under the batch's own */
         (gx_batch_spans && !palette_on &&
          (memcmp(batch_posm, pi.pos_mtx, sizeof(batch_posm)) != 0 ||
           (sl.off_nrm >= 0 && memcmp(batch_nrmm, pi.nrm_mtx, sizeof(batch_nrmm)) != 0)) &&
          !(premerge_on && (merge_this = pm_decide(count)) != 0)))) {
        /* The layout cannot change inside a list (it is a function of the
         * descriptor, the texgens and the TEV chain, none of which a list can
         * touch), so the pending batch was assembled under `sl` as it is now
         * -- except in the one case this guards, where it is restored for
         * the flush. */
        Layout cur = sl;
        end_who = memcmp(&batch_sl, &sl, sizeof(sl)) != 0 ? "prepare:layout"
                  : (batch_n >= BATCH_MAX || batch_verts + count > MAX_VERTS ||
                     port_opt.oldsubmit || port_opt.batchmax) ? "prepare:full"
                  : "prepare:matrix";
        sl = batch_sl;
        batch_flush();
        sl = cur;
    }
}

/* The run at `run_pos` has `nverts` vertices: advance the cursor and add it
 * to the batch.  batch_prepare and ring_claim have already made sure it can
 * join; the check here is the belt to their braces. */
static void batch_add(void) {
    u32 count = (u32)nverts; /* before anything below can flush */
    u8 p = prim;
    size_t bytes = (size_t)count * sl.stride;
    ring_cursor = run_pos + bytes;
    if (!count) {
        return;
    }
    if (batch_n &&
        (batch_n >= BATCH_MAX || batch_verts + count > MAX_VERTS ||
         memcmp(&batch_sl, &sl, sizeof(sl)) != 0 ||
         batch_pos + (size_t)batch_verts * batch_sl.stride != run_pos)) {
        Layout cur = sl;
        sl = batch_sl;
        end_who = "add:late";
        batch_flush();
        sl = cur;
        stat_late_flush++;
    }
    if (!batch_n) {
        batch_pos = run_pos;
        batch_sl = sl;
        batch_verts = 0;
        /* M22: the batch's matrices are this primitive's own; a merge decided
         * for a batch that a wrap or a late flush has since ended is off,
         * and the vertices stay in their own model space */
        pm_batch_start();
        if (merge_this) {
            stat_pm_refused_wrap++;
            merge_this = 0;
        }
    } else if (merge_this) {
        pm_apply(src_buf + run_pos, count);
        merge_this = 0;
    }
    batch[batch_n].first = batch_verts;
    batch[batch_n].count = count;
    batch[batch_n].prim = p;
    batch_n++;
    batch_verts += count;
    gx_batch_pending = 1;
}

/* GX_STATE_TOUCH's target: a state setter is about to change something the
 * pending batch was decoded under.  Timed as GX work, which it is.  With
 * --submitstats the setters that end batches are counted by name, which is
 * what says whether a batch ended because the material changed or because
 * the game re-sent something it already had. */

void gx_batch_flush_from(const char* who) {
    if (port_opt.submitstats) {
        int i = flusher_slot(who);
        if (i >= 0) {
            flushers[i].n++;
            pending_flusher = i;
        }
    }
    end_who = who;
    port_perf_gx_begin();
    batch_flush();
    port_perf_gx_end();
}

static int prim_is_list(GLenum mode) {
    return mode == GL_TRIANGLES || mode == GL_QUADS || mode == GL_LINES ||
           mode == GL_POINTS;
}

static int prim_unit(GLenum mode) {
    return mode == GL_QUADS ? 4 : mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : 1;
}

/* Issue the segments with as few calls as their shapes allow.  Merging a list
 * primitive is only exact when its count is whole -- a stray vertex at the
 * end of one GX_TRIANGLES run is dropped by GL and must stay dropped rather
 * than pair with the next run's first. */
static void issue_segments(const Seg* segs, int nsegs) {
    static int md_first[BATCH_MAX];
    static int md_count[BATCH_MAX];
    int i = 0;
    int multi = gl13_have_multidraw() && !port_opt.nomultidraw && !port_opt.oldsubmit;
    /* M30 (PLAN.md 45, cause B): a position-only layout -- no normal, no
     * texcoord array, every texgen from the position -- is the one shape
     * glMultiDrawArraysEXT drew nothing for, on the Radeon 9000's driver and
     * on the Intel HD 3000's alike, while the same strips through
     * glDrawArrays drew (m434's pond: 31 strips of 64, the only such batch
     * in the game).  Not understood; per-strip calls for that layout, and
     * --mdposonly keeps the multi-draw for the A/B. */
    if (multi && sl.ntex == 0 && sl.off_nrm < 0 && !port_opt.mdposonly) {
        multi = 0;
    }
    while (i < nsegs) {
        GLenum mode = gl_prim(segs[i].prim);
        int j = i + 1;
        if (port_opt.oldsubmit) {
            if (gl13_trace_armed()) {
                port_log("gltrace> glDrawArrays mode %04x first %u count %u\n", mode,
                         segs[i].first, segs[i].count);
            }
            GL(glDrawArrays)(mode, (GLint)segs[i].first, (GLsizei)segs[i].count);
            stat_draws++;
        } else if (prim_is_list(mode) && !port_opt.nomerge) {
            u32 first = segs[i].first, count = segs[i].count;
            int unit = prim_unit(mode);
            while (j < nsegs && gl_prim(segs[j].prim) == mode &&
                   segs[j].first == first + count && (count % (u32)unit) == 0) {
                count += segs[j].count;
                j++;
            }
            if (gl13_trace_armed()) {
                port_log("gltrace> glDrawArrays mode %04x first %u count %u\n", mode, first, count);
            }
            GL(glDrawArrays)(mode, (GLint)first, (GLsizei)count);
            stat_draws++;
            stat_merged += (unsigned)(j - i - 1);
        } else {
            while (j < nsegs && gl_prim(segs[j].prim) == mode) {
                j++;
            }
            if (multi && j - i >= 2) {
                /* M30 (PLAN.md 45, cause B): one glMultiDrawArraysEXT per run
                 * of segments, split so that no call carries more than
                 * --mdmax vertices (the diagnostic for m434's water: 31
                 * strips of 64 in one call drew nothing on two drivers). */
                int k = i;
                while (k < j) {
                    int m = 0, verts = 0;
                    while (k + m < j && (m == 0 || !port_opt.mdmax ||
                                         verts + (int)segs[k + m].count <= port_opt.mdmax)) {
                        md_first[m] = (int)segs[k + m].first;
                        md_count[m] = (int)segs[k + m].count;
                        verts += (int)segs[k + m].count;
                        m++;
                    }
                    gl13_multi_draw_arrays(mode, md_first, md_count, m);
                    stat_draws++;
                    stat_multi_calls++;
                    stat_multi_prims += (unsigned)m;
                    k += m;
                }
            } else {
                int k;
                for (k = i; k < j; k++) {
                    if (gl13_trace_armed()) {
                        port_log("gltrace> glDrawArrays mode %04x first %u count %u\n", mode,
                                 segs[k].first, segs[k].count);
                    }
                    GL(glDrawArrays)(mode, (GLint)segs[k].first, (GLsizei)segs[k].count);
                    stat_draws++;
                }
            }
        }
        i = j;
    }
}

/* ---- M21: one draw call per batch ------------------------------------------
 *
 * `glMultiDrawArraysEXT` is a loop inside the driver: every strip of a batch
 * is still its own draw with its own per-draw validation.  A batch whose
 * segments are all triangles, quads, strips and fans is instead expressed as
 * one triangle list through an index buffer and issued with one
 * `glDrawRangeElements`.  Nothing about any triangle changes: a strip's
 * triangle i is (i, i+1, i+2) for even i and (i+1, i, i+2) for odd i, which
 * is the order GL itself defines (so the winding, hence the culling, is the
 * same), a fan's is (0, i+1, i+2), and a quad is (0,1,2)(0,2,3), the split
 * the hardware makes of a GL_QUADS quad.  The shade model is GL_SMOOTH
 * everywhere (gl13.c), so no provoking vertex can differ.  The md5s are the
 * check; `--noindexed` is the A/B. */
static u32 idx_buf[3 * MAX_VERTS];

/* Build the batch's index list with `bias` added to every index.  Returns
 * the index count, or 0 when a segment is not a triangle-family primitive. */
static u32 build_indices(const Seg* segs, int nsegs, u32 bias, u32* lo, u32* hi) {
    u32 n = 0;
    int k;
    u32 mn = 0xffffffffu, mx = 0;
    for (k = 0; k < nsegs; k++) {
        u32 f = segs[k].first + bias, c = segs[k].count, i;
        if (c < 3) {
            continue;
        }
        switch (segs[k].prim) {
            case GX_TRIANGLES:
                c -= c % 3;
                for (i = 0; i < c; i++) {
                    idx_buf[n++] = f + i;
                }
                break;
            case GX_QUADS:
                c -= c % 4;
                for (i = 0; i < c; i += 4) {
                    idx_buf[n++] = f + i;
                    idx_buf[n++] = f + i + 1;
                    idx_buf[n++] = f + i + 2;
                    idx_buf[n++] = f + i;
                    idx_buf[n++] = f + i + 2;
                    idx_buf[n++] = f + i + 3;
                }
                break;
            case GX_TRIANGLESTRIP:
                for (i = 0; i + 2 < c; i++) {
                    if (i & 1) {
                        idx_buf[n++] = f + i + 1;
                        idx_buf[n++] = f + i;
                    } else {
                        idx_buf[n++] = f + i;
                        idx_buf[n++] = f + i + 1;
                    }
                    idx_buf[n++] = f + i + 2;
                }
                break;
            case GX_TRIANGLEFAN:
                for (i = 1; i + 1 < c; i++) {
                    idx_buf[n++] = f;
                    idx_buf[n++] = f + i;
                    idx_buf[n++] = f + i + 1;
                }
                break;
            default:
                return 0;
        }
        if (f < mn) {
            mn = f;
        }
        if (f + c - 1 > mx) {
            mx = f + c - 1;
        }
    }
    *lo = mn;
    *hi = mx;
    return n;
}

static void issue_indexed(u32 nidx, u32 lo, u32 hi) {
    if (hi <= 0xffffu) {
        /* 16-bit indices: the same buffer, packed in place (front to back,
         * so no element is overwritten before it is read) */
        u16* p16 = (u16*)idx_buf;
        u32 i;
        for (i = 0; i < nidx; i++) {
            p16[i] = (u16)idx_buf[i];
        }
        gl13_draw_range_elements(GL_TRIANGLES, lo, hi, (int)nidx, 0, p16);
    } else {
        gl13_draw_range_elements(GL_TRIANGLES, lo, hi, (int)nidx, 1, idx_buf);
        stat_indexed_u32++;
    }
    stat_draws++;
    stat_indexed_batches++;
    stat_indexed_tris += nidx / 3;
}

/* The draw itself, once the span is in hand.  `s` is where phase 2 reads
 * from: the ring for a batch the decoder just assembled, and the cached copy
 * for a display list that hit.  `n` is the span's vertex count; the segments
 * index into it. */
/* The submit, in two halves (M22): draw_apply puts the batch's state into
 * GL -- the program, the parameters, the arrays, the transform, the raster
 * state, the TEV and the binds -- and draw_issue makes the draw calls.
 * draw_submit is the two in a row; the lazy flush runs the first at the
 * setter that would have ended the batch and the second when the batch
 * really ends. */
/* M33 (PLAN.md 48): --skipobj / --probeobj, diagnostics for a draw that
 * the drawlog says is sane and the picture never shows (m435's sphere).
 * The probe reads the viewport back before and after the object's draw
 * calls (the twins are joins, so it works under the render thread too),
 * counts the pixels the draw changed and their box, and prints the GL
 * state the draw is issued under -- the enables, the program binding, the
 * vertex array as bound -- with the first vertices' bytes as GL sees them. */
static int obj_named(const char* want) {
    int mdl = -1;
    const char* nm;
    if (want && !strcmp(want, "*")) {
        return 1; /* every draw (with --probebox: only those that touch the box) */
    }
    nm = want ? port_drawobj_name(gx_last_posmtx_arg, &mdl) : NULL;
    return nm && !strcmp(nm, want);
}
static GxXfDesc app_xfd;  /* what draw_apply settled, for draw_issue */
static int app_on_gpu;
static u32 app_bias;
static int app_skip, app_probe; /* M33: --skipobj / --probeobj, named at the apply (the
                                 * lazy flush issues after the matrix pointer has moved on) */

static int draw_apply(const u8* s, int n, int in_ring) {
    GxXfDesc* xfd = &app_xfd;
    int on_gpu = 0;
    u32 bias = 0; /* M21 --fixbase: the batch's first vertex as an index from the ring's start */
    sub_posm = in_ring ? batch_posm : pi.pos_mtx;
    sub_nrmm = in_ring ? batch_nrmm : pi.nrm_mtx;
    /* M11: phase 2 on the GPU.  The decision has to be made *before* phase 2
     * runs, because the whole point is not to run it; `gx_vprog_draw` compiles
     * or finds the variant, uploads the parameters and binds the source
     * layout as the vertex arrays, and returns 0 for anything it cannot cover
     * -- having counted it.  `--cpuxf` makes it always return 0, which is the
     * A/B (PLAN.md 25). */
    if (!port_opt.cpuxf) {
        fill_xf_desc(xfd, s);
        on_gpu = gx_vprog_draw(xfd, n);
        /* M21 (--fixbase): the arrays are based at the ring, not at the
         * batch, so their pointers -- and whatever the driver rebuilds when
         * a pointer changes -- change only when the layout does.  The
         * batch's position becomes an index bias on every segment.  Only
         * for a batch the ring_claim alignment placed (a late flush in
         * batch_add can start a batch anywhere: then the old base). */
        if (on_gpu && in_ring && !port_opt.nofixbase && src_buf && s >= src_buf &&
            sl.stride > 0 && ((size_t)(s - src_buf) % (size_t)sl.stride) == 0) {
            bias = (u32)((size_t)(s - src_buf) / (size_t)sl.stride);
            xfd->base = src_buf;
            stat_fixbase_batches++;
        } else if (on_gpu && in_ring && !port_opt.nofixbase) {
            stat_fixbase_miss++;
        }
    }
    if (!on_gpu) {
        gx_vprog_disable();
        if (in_ring) {
            /* M29: the vertices may still be the render thread's to decode */
            rt_decode_join("cpu path");
        }
        port_perf_sub_enter(PERF_SUB_XF);
        finish_vertices(s, n);
        port_perf_sub_leave();
    }
    port_perf_sub_enter(PERF_SUB_STATE);
    /* M31 (PLAN.md 46): --forceobj NAME[:flags], a diagnostic -- the named
     * object's draws with the cull (1), the z test (2) and the alpha test
     * (4) switched off (all three by default), to tell which of them hides a
     * draw the drawlog says is sane (m435's pillar sphere). */
    gx_force_flags = 0;
    if (port_opt.forceobj) {
        int mdl = -1;
        const char* nm = port_drawobj_name(gx_last_posmtx_arg, &mdl);
        if (nm && !strcmp(nm, port_opt.forceobj)) {
            gx_force_flags = port_opt.forceobj_flags ? port_opt.forceobj_flags : 7;
        }
    }
    app_skip = port_opt.skipobj && obj_named(port_opt.skipobj);
    if (port_opt.skipverts && n == port_opt.skipverts) {
        app_skip = 1; /* M34: --skipverts N, a draw a hook issues (no object name) */
    }
    app_probe = port_opt.probeobj && obj_named(port_opt.probeobj);
    if (port_opt.probeobj && !strcmp(port_opt.probeobj, "?")) {
        /* the names the lookup sees this frame, once each per frame */
        static unsigned last_fr; static char seen[64][32]; static int nseen;
        int mdl = -1, k;
        const char* nm = port_drawobj_name(gx_last_posmtx_arg, &mdl);
        unsigned fr = gl13_frame_number();
        if (fr != last_fr) { last_fr = fr; nseen = 0; }
        for (k = 0; k < nseen; k++) { if (!strcmp(seen[k], nm ? nm : "(null)")) break; }
        if (k == nseen && nseen < 64) {
            snprintf(seen[nseen++], 32, "%s", nm ? nm : "(null)");
            port_log("port> probeobj: frame %u model %d name \"%s\" (%d verts)\n", fr, mdl, nm ? nm : "(null)", n);
        }
    }
    gl13_apply_transform();
    gl13_apply_raster_state();
    gx_tev_apply();
    /* M21: the specular colour sum, only where the vertex program wrote a
     * secondary colour for it (gx_internal.h, gx_hilite_decide) */
    glc_color_sum(on_gpu && !port_opt.cpuxf && xfd->hilite == 1);
    port_perf_sub_leave();

    /* On the GPU path the arrays are the *source* layout and gx_vprog_draw has
     * already bound them; there is no `out_buf` to point at, because phase 2
     * never ran.  On the CPU path: `out_buf` is a static buffer and every draw
     * reads it from index zero, so the base pointer never moves; the offsets
     * and the stride do, because the layout is packed to the primitive.
     * glc_* compares both. */
    port_perf_sub_enter(PERF_SUB_ISSUE);
    if (on_gpu) {
        /* the parameters and the arrays, now that the texture binds this draw
         * needs have happened: gx_vprog.c says why that ordering matters */
        gx_vprog_bind(xfd);
    } else {
        /* M27: out_buf is rewritten by the next CPU-path draw, so under the
         * render thread this draw's bytes go into the stream as a payload and
         * the arrays point at the copy (rt_stash returns out_buf itself when
         * the stream is off) */
        const u8* ob = (const u8*)rt_stash(out_buf, (size_t)n * (size_t)out_stride);
        glc_vertex_array(ob, out_stride);
        glc_color_array(ob + out_off_clr, out_stride);
        glc_normal_array(NULL, 0);
        {
            int i;
            for (i = 0; i < gl13_max_tex_units; i++) {
                u8 coord = 0, map = 0;
                if (gx_tev_unit_source(i, &coord, &map) && coord < out_ntex &&
                    gx_bound_tex(map) != NULL) {
                    glc_coord_array(i, ob + out_off_tex + 8 * coord, out_stride);
                } else {
                    glc_coord_array(i, NULL, 0);
                }
            }
        }
    }
    port_perf_sub_leave();
    app_on_gpu = on_gpu;
    app_bias = bias;
    return on_gpu;
}

static u8* probe_a;
static u8* probe_b;
static GLint probe_vp[4];
static void probe_begin(const u8* s, int n, const Seg* segs, int nsegs, int on_gpu) {
    GLint v[4];
    int i;
    glGetIntegerv(GL_VIEWPORT, probe_vp);
    if (!probe_a) {
        probe_a = (u8*)malloc(2048 * 2048 * 3);
        probe_b = (u8*)malloc(2048 * 2048 * 3);
    }
    {
        int mdl = -1;
        const char* nm = port_drawobj_name(gx_last_posmtx_arg, &mdl);
        port_log("port> probeobj: frame %u draw %lu: model %d \"%s\", %d tev stage(s), %d texgen(s); "
                 "z test %d fn %d write %d comploc %d, alpha %d/%d op %d %d/%d, blend %d %d %d, cull %d\n",
                 gl13_frame_number(), (unsigned long)stat_draws, mdl, nm ? nm : "?", gx.num_tev,
                 gx.num_texgens, gx.z_enable, gx.z_func, gx.z_update, gx.z_comploc, gx.alpha_comp0,
                 gx.alpha_ref0, gx.alpha_op, gx.alpha_comp1, gx.alpha_ref1, gx.blend_mode,
                 gx.blend_src, gx.blend_dst, gx.cull);
    }
    port_log("port> probeobj: draw of %d verts in %d segment(s), %s path, batch at ring+%ld "
             "stride %d (pos 0 nrm %d clr %d tex %d, %d texcoords); viewport %d %d %d %d\n",
             n, nsegs, on_gpu ? "vertex-program" : "CPU", src_buf ? (long)(s - src_buf) : -1L,
             sl.stride, sl.off_nrm, sl.off_clr, sl.off_tex, sl.ntex, probe_vp[0], probe_vp[1],
             probe_vp[2], probe_vp[3]);
    for (i = 0; i < nsegs; i++) {
        port_log("port> probeobj:   segment %d: prim 0x%02x first %u count %u\n", i, segs[i].prim,
                 segs[i].first, segs[i].count);
    }
#define PG(name, e) do { v[0] = v[1] = v[2] = v[3] = 0; glGetIntegerv(e, v); \
        port_log("port> probeobj:   %-28s %d %d %d %d\n", name, v[0], v[1], v[2], v[3]); } while (0)
    PG("VERTEX_PROGRAM_ARB", 0x8620);
    PG("PROGRAM_BINDING_ARB", 0x8677);
    PG("VERTEX_ARRAY", 0x8074);
    PG("VERTEX_ARRAY_SIZE/TYPE/STRIDE", 0x807A);
    PG("  type", 0x807B);
    PG("  stride", 0x807C);
    PG("NORMAL_ARRAY", 0x8075);
    PG("COLOR_ARRAY", 0x8076);
    PG("TEXTURE_COORD_ARRAY", 0x8078);
    PG("CULL_FACE", GL_CULL_FACE);
    PG("DEPTH_TEST", GL_DEPTH_TEST);
    PG("DEPTH_WRITEMASK", GL_DEPTH_WRITEMASK);
    PG("DEPTH_FUNC", GL_DEPTH_FUNC);
    PG("ALPHA_TEST", GL_ALPHA_TEST);
    PG("BLEND", GL_BLEND);
    PG("SCISSOR_TEST", GL_SCISSOR_TEST);
    PG("SCISSOR_BOX", GL_SCISSOR_BOX);
    PG("COLOR_WRITEMASK", GL_COLOR_WRITEMASK);
    PG("STENCIL_TEST", GL_STENCIL_TEST);
    PG("POLYGON_MODE", GL_POLYGON_MODE);
    PG("CLIP_PLANE0", 0x3000);
    PG("LIGHTING", GL_LIGHTING);
    PG("MATRIX_MODE", GL_MATRIX_MODE);
    PG("ACTIVE_TEXTURE", 0x84E0);
    PG("CLIENT_ACTIVE_TEXTURE", 0x84E1);
    PG("DRAW_BUFFER", GL_DRAW_BUFFER);
#undef PG
    {
        /* the first vertices as bound: the ring's bytes at the array base */
        int k, lim = n < 6 ? n : 6;
        if (port_opt.probeverts > lim && n > lim) {
            lim = n < port_opt.probeverts ? n : port_opt.probeverts; /* M34: --probeverts N */
        }
        for (k = 0; k < lim; k++) {
            const u8* vb = s + (size_t)k * sl.stride;
            const f32* pf = (const f32*)vb;
            port_log("port> probeobj:   v%d pos %.2f %.2f %.2f", k, pf[0], pf[1], pf[2]);
            if (sl.off_nrm >= 0) {
                const f32* nf = (const f32*)(vb + sl.off_nrm);
                port_log("  nrm %.3f %.3f %.3f", nf[0], nf[1], nf[2]);
            }
            if (sl.off_clr >= 0) {
                const u8* c = vb + sl.off_clr;
                port_log("  clr %d %d %d %d", c[0], c[1], c[2], c[3]);
            }
            if (sl.off_tex >= 0) {
                const f32* t = (const f32*)(vb + sl.off_tex);
                port_log("  st %.3f %.3f", t[0], t[1]);
            }
            port_log("\n");
        }
    }
    glFinish();
    glReadPixels(probe_vp[0], probe_vp[1], probe_vp[2], probe_vp[3], GL_RGB, GL_UNSIGNED_BYTE, probe_a);
}
static void probe_end(void) {
    GLint w = probe_vp[2], h = probe_vp[3];
    long i, npx = (long)w * h, changed = 0;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    GLenum err;
    glFinish();
    glReadPixels(probe_vp[0], probe_vp[1], w, h, GL_RGB, GL_UNSIGNED_BYTE, probe_b);
    for (i = 0; i < npx; i++) {
        if (probe_a[i * 3] != probe_b[i * 3] || probe_a[i * 3 + 1] != probe_b[i * 3 + 1] ||
            probe_a[i * 3 + 2] != probe_b[i * 3 + 2]) {
            int x = (int)(i % w), y = (int)(i / w);
            if (port_opt.probebox[2] > 0 && (x < port_opt.probebox[0] || x > port_opt.probebox[2] ||
                                             y < port_opt.probebox[1] || y > port_opt.probebox[3])) {
                continue;
            }
            changed++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    glGetIntegerv(GL_VIEWPORT, probe_vp); /* a join: every record before it is done */
    err = glGetError();
    if (port_opt.probebox[2] > 0) {
        /* the depth at the box's centre, after the draw */
        GLfloat z = -1.0f;
        glReadPixels((port_opt.probebox[0] + port_opt.probebox[2]) / 2,
                     (port_opt.probebox[1] + port_opt.probebox[3]) / 2, 1, 1, GL_DEPTH_COMPONENT,
                     GL_FLOAT, &z);
        port_log("port> probeobj:   depth at the box centre after the draw: %.6f\n", (double)z);
    }
    if (port_opt.probebox[2] > 0 && !changed) {
        port_log("port> probeobj:   -> nothing in the box\n");
        return;
    }
    port_log("port> probeobj:   -> %ld pixel(s) changed%s, glGetError 0x%x\n", changed,
             changed ? "" : " (the draw landed nothing)", (unsigned)err);
    if (changed) {
        /* how much: the mean and largest per-channel change, and the two
         * pictures (before, after, the difference x8) into --shotdir */
        long sum = 0; int mx = 0;
        static int nprobe;
        char path[512];
        FILE* f;
        for (i = 0; i < npx; i++) {
            int c;
            for (c = 0; c < 3; c++) {
                int d = abs((int)probe_a[i * 3 + c] - (int)probe_b[i * 3 + c]);
                sum += d;
                if (d > mx) mx = d;
            }
        }
        port_log("port> probeobj:      box x %d..%d y %d..%d (GL rows, bottom up); mean change over "
                 "the changed pixels %.1f levels, largest %d\n", x0, x1, y0, y1,
                 (double)sum / (double)(changed * 3), mx);
        if (port_opt.shotdir && nprobe < 40) {
            int pass;
            for (pass = 0; pass < 3; pass++) {
                snprintf(path, sizeof path, "%s/probe-%05u-%02d-%s.ppm", port_opt.shotdir,
                         gl13_frame_number(), nprobe, pass == 0 ? "before" : pass == 1 ? "after" : "diffx8");
                f = fopen(path, "wb");
                if (f) {
                    int y;
                    fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
                    for (y = (int)h - 1; y >= 0; y--) {
                        if (pass < 2) {
                            fwrite((pass == 0 ? probe_a : probe_b) + (size_t)y * w * 3, 1, (size_t)w * 3, f);
                        } else {
                            long k;
                            for (k = (long)y * w * 3; k < (long)(y + 1) * w * 3; k++) {
                                int d = abs((int)probe_a[k] - (int)probe_b[k]) * 8;
                                fputc(d > 255 ? 255 : d, f);
                            }
                        }
                    }
                    fclose(f);
                }
            }
            nprobe++;
        }
    }
}

static void draw_issue(const u8* s, int n, const Seg* segs, int nsegs, int in_ring) {
    int on_gpu = app_on_gpu;
    u32 bias = app_bias;
    int probing = 0;
    stat_prims += (unsigned)nsegs;
    stat_verts += (unsigned)n;
    if (app_skip) {
        return;
    }
    if (app_probe) {
        probing = 1;
        probe_begin(s, n, segs, nsegs, on_gpu);
    }
    /* After the state is applied, not before: the texture cache fills in
     * gl_name at bind time, so a log taken earlier reports a stale 0 and
     * sends you hunting for a texture upload that already happened. */
    /* Through parameters, not the globals: draw_submit runs from inside
     * batch_add when a batch has to make room, and batch_add still needs
     * `nverts` and `prim` for the segment it is adding.  The first M16 build
     * wrote `nverts = n` here and every segment added right after an in-add
     * flush was recorded with the *previous* batch's vertex count -- one
     * eye on the title, 136 pixels, PLAN.md 31.3. */
    if (port_opt.drawlog) {
        int i;
        for (i = 0; i < nsegs; i++) {
            draw_log(segs[i].first, segs[i].count, segs[i].prim);
        }
    }
    port_perf_sub_enter(PERF_SUB_ISSUE);
    if (on_gpu && in_ring) {
        /* the CPU cache, out ahead of the DMA (a no-op without VAR); over
         * the batch's final extent, which a lazy flush grew after the apply */
        gl13_var_flush(s, (size_t)n * sl.stride);
    }
    if (port_opt.segrebase && on_gpu) {
        /* diagnostic (PLAN.md 31.3): every segment from its own base pointer
         * with first = 0, i.e. the pre-M16 array shape inside one batch */
        int i;
        for (i = 0; i < nsegs; i++) {
            GxXfDesc x2 = app_xfd;
            x2.base = s + (size_t)segs[i].first * sl.stride;
            if (port_opt.segrebase & 2) {
                gx_tev_apply();
            }
            if (port_opt.segrebase & 4) {
                gx_vprog_draw(&x2, (int)segs[i].count);
            }
            if (port_opt.segrebase & 8) {
                gl13_apply_transform();
                gl13_apply_raster_state();
            }
            gx_vprog_bind(&x2);
            GL(glDrawArrays)(gl_prim(segs[i].prim), 0, (GLsizei)segs[i].count);
            stat_draws++;
        }
        port_perf_sub_leave();
        if (probing) probe_end();
        return;
    }
    if (!port_opt.noindexed && !port_opt.oldsubmit) {
        u32 lo = 0, hi = 0, nidx;
        port_perf_sub_enter(PERF_SUB_INDEX);
        nidx = build_indices(segs, nsegs, bias, &lo, &hi);
        port_perf_sub_leave();
        if (nidx) {
            issue_indexed(nidx, lo, hi);
            port_perf_sub_leave();
            if (probing) probe_end();
            return;
        }
    }
    if (bias) {
        /* the arrays are based at the ring: every segment starts `bias`
         * vertices in */
        static Seg biased[BATCH_MAX];
        int i;
        for (i = 0; i < nsegs; i++) {
            biased[i] = segs[i];
            biased[i].first += bias;
        }
        if (gl13_zprepass_wanted()) {
            gl13_zprepass_begin();
            issue_segments(biased, nsegs);
            gl13_zprepass_end();
            stat_zprepass++;
        }
        issue_segments(biased, nsegs);
    } else {
        if (gl13_zprepass_wanted()) {
            gl13_zprepass_begin();
            issue_segments(segs, nsegs);
            gl13_zprepass_end();
            stat_zprepass++;
        }
        issue_segments(segs, nsegs);
    }
    port_perf_sub_leave();
    if (probing) probe_end();
}

static void draw_submit(const u8* s, int n, const Seg* segs, int nsegs, int in_ring) {
    if (!n || !nsegs) {
        return;
    }
    if (!gl13_live()) {
        stat_prims += (unsigned)nsegs;
        stat_verts += (unsigned)n;
        return;
    }
    draw_apply(s, n, in_ring);
    draw_issue(s, n, segs, nsegs, in_ring);
}

/* The lazy flush's first half, for the pending batch (gx_batch_touch).
 * Returns 0 when the batch cannot be held open: the CPU fallback transforms
 * the vertices at the apply, and a batch that grows afterwards would leave
 * them behind. */
static int batch_apply_now(void) {
    PrimInv* pi_save = pi_cur;
    Layout sl_save = sl;
    int hs_save = gx_hilite_stage, hm_save = gx_hilite_mode;
    int os_save = out_stride, oc_save = out_off_clr, ot_save = out_off_tex, on_save = out_ntex;
    int ok;
    if (!batch_n || !gl13_live()) {
        return 0;
    }
    pi_cur = bctx_pi;
    sl = batch_sl;
    gx_hilite_stage = bctx.hilite_stage;
    gx_hilite_mode = bctx.hilite_mode;
    out_stride = bctx.out_stride;
    out_off_clr = bctx.out_off_clr;
    out_off_tex = bctx.out_off_tex;
    out_ntex = bctx.out_ntex;
    ok = draw_apply(src_buf + batch_pos, (int)batch_verts, 1);
    pi_cur = pi_save;
    sl = sl_save;
    gx_hilite_stage = hs_save;
    gx_hilite_mode = hm_save;
    out_stride = os_save;
    out_off_clr = oc_save;
    out_off_tex = ot_save;
    out_ntex = on_save;
    return ok;
}

/* An immediate-mode primitive.  Until M37 it was its own batch, flushed at
 * once; with GX_CMP_IMM it joins the pending batch exactly as a display
 * list's primitive does -- GXBegin has already run begin_attr_order,
 * batch_prepare and ring_claim for it, so the batch's contiguity, layout
 * and context have all been decided by the same code -- and the next state
 * setter or the next primitive ends the batch.  115 of the character
 * select's 249 batches were immediate primitives, 25 of them next to a
 * batch of exactly the same state and matrices (PLAN.md 52). */
static void draw_now(void) {
    batch_add();
    if (!(port_opt.cmpmask & GX_CMP_IMM)) {
        end_who = "immediate";
        batch_flush();
    }
}

void GXEnd(void) {
    if (dl_recording) {
        return;
    }
    if (!in_prim) {
        return;
    }
    in_prim = 0;
    port_perf_gx_begin();
    draw_now();
    port_perf_gx_end();
    nverts = 0;
}

/* ---- replaying a display list ---------------------------------------------- */

static u32 rd_be16(const u8* p) { return (u32)((p[0] << 8) | p[1]); }

/* ---- the display-list vertex cache -----------------------------------------
 *
 * 92% of the board's primitives arrive through `GXCallDisplayList`, and every
 * frame the port decoded the same lists again from scratch: parse the opcode
 * stream, follow every index into the game's own attribute arrays, convert
 * each component out of its fixed-point type, and stage a vertex.  M5's
 * profile put that decode -- `indexed` plus `read_component` -- at a quarter
 * to a third of the frame, as large again as the transform it feeds.
 *
 * None of it depends on the camera, the matrices, the lights or the material.
 * A display list is a fixed set of indices into arrays the game mostly writes
 * once at model load, so its **model-space** vertices are the same every
 * frame.  So they are decoded once and kept, and a replay skips straight to
 * phase 2.
 *
 * **What the key has to contain** is the whole of the argument, because a
 * cache that is wrong here draws last frame's geometry:
 *
 *   - the list's bytes (its indices and any direct attributes),
 *   - the vertex descriptor and the whole vertex-attribute table, since the
 *     same indices read differently through a different VAT,
 *   - each array's base pointer and stride,
 *   - **the contents of the array ranges the list actually reads** -- Mario
 *     Party 4 does animate geometry on the CPU (`ClusterExec.c` morphs and
 *     `EnvelopeExec.c` skins write back into the position array), so a key
 *     that trusted the base pointer would freeze every animated model,
 *   - and the handful of state bits phase 1 itself reads: whether the
 *     descriptor has a normal, whether the colour comes from the vertex or is
 *     splatted from the register material (and if so, that colour), and how
 *     many raw texcoords a texgen will read back.
 *
 * The array-contents check is why this pays rather than merely moves the cost.
 * An indexed position is six bytes in the array and thirty-six in the decoded
 * source vertex, and the check only *reads* the array while the decode reads
 * it, converts it and writes the vertex -- so validating is a small fraction
 * of decoding, and it is exact rather than sampled.
 *
 * **And on this game it does not pay, so it is off by default** -- `--dlcache`
 * turns it on.  PLAN.md 21.3 has the numbers: the title screen gains 18% and
 * the character select loses 14%, because Mario Party 4 animates almost
 * everything it draws and an animated model invalidates its entry every frame,
 * paying the validity check *and* the decode.  This is M5's AltiVec finding
 * again in a different place, and it is kept switched on a flag for the same
 * reason: the measurement is the result.
 */

#define DLC_BUCKETS 1024
#define DLC_MAX_BYTES (32u * 1024u * 1024u)
#define DLC_MAX_ENTRY_BYTES (2u * 1024u * 1024u)

typedef struct DlSeg {
    u8 op;     /* the list's own opcode: primitive | vertex format */
    u32 first; /* source vertex index within the entry              */
    u32 count;
} DlSeg;

typedef struct DlEntry {
    struct DlEntry* next;
    const void* list;
    u32 nbytes;
    u32 list_hash;
    u32 state_hash;
    u32 array_hash;
    /* The *window* of each array this list reads, in bytes from the array
     * base.  Not the prefix: a display list is one material's slice of a mesh
     * and its indices are a contiguous run, so [min..max] is its own vertices
     * and [0..max] is everybody's.  The first M9 cache hashed the prefix and
     * spent 13.7 MB a frame proving that 0.9 MB of vertices had not moved --
     * it was slower than the decode it replaced. */
    u32 arr_off[GX_MAX_ATTR];
    u32 arr_len[GX_MAX_ATTR];
    int stride;                 /* the source layout's stride              */
    u32 nverts;
    u8* src;
    DlSeg* segs;
    int nsegs;
    int dynamic; /* its arrays have been rewritten under it at least once */
    unsigned last_frame;
    size_t bytes;
} DlEntry;

static DlEntry* dlc[DLC_BUCKETS];
static size_t dlc_bytes;
static unsigned stat_dlc_hit, stat_dlc_new, stat_dlc_list, stat_dlc_array,
    stat_dlc_state, stat_dlc_evict, stat_dlc_big;
/* How the *entries* split, which is the static/dynamic question: an entry that
 * has ever been invalidated by its arrays moving is an animated model. */
static unsigned stat_dlc_entries, stat_dlc_dynamic;

static u32 hash_bytes(const void* pv, size_t n, u32 h) {
    const u8* p = (const u8*)pv;
    size_t i = 0;
    /* PowerPC loads unaligned words in hardware, and GCC turns this memcpy
     * into the single `lwz` it is. */
    for (; i + 4 <= n; i += 4) {
        u32 w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Everything phase 1 reads that is not the list's own bytes.
 *
 * Only the attributes the descriptor actually names are hashed.  The whole
 * vertex-attribute table is 624 bytes and the whole array table 208, and this
 * runs on every one of the ~850 `GXCallDisplayList` calls a board frame makes
 * -- hashing all of it would have cost most of what the cache saves.  Four
 * live attributes come to about 150 bytes. */
static u32 dl_state_hash(void) {
    const GXChanCtrl* cc = &gx.chan[0];
    u32 h = 2166136261u;
    u8 k[8];
    int a, cm, t, copy_n = 0, splat;
    h = hash_bytes(gx.vcd, sizeof(gx.vcd), h);
    for (a = 0; a < GX_MAX_ATTR; a++) {
        if (gx.vcd[a] != GX_NONE) {
            int f;
            for (f = 0; f < GX_MAX_VTXFMT; f++) {
                h = hash_bytes(&gx.vat[f][a], sizeof(GXVatFmt), h);
            }
            h = hash_bytes(&gx.array[a].base, sizeof(gx.array[a].base), h);
            h = hash_bytes(&gx.array[a].stride, sizeof(gx.array[a].stride), h);
        }
    }
    if (gx.num_chans == 0) {
        cm = 0;
    } else if (!cc->enable) {
        cm = cc->mat_src == GX_SRC_REG ? 1 : 0;
    } else {
        cm = 2;
    }
    for (t = 0; t < gx.num_texgens && t < GX_TEXCOORDS; t++) {
        const GXTexGen* g = &gx.texgen[t];
        int sk = -1;
        if (g->src >= GX_TG_TEX0 && g->src <= GX_TG_TEX7) {
            sk = (int)g->src - GX_TG_TEX0;
        } else if (g->src != GX_TG_POS && g->src != GX_TG_NRM) {
            sk = t;
        }
        if (sk >= 0 && sk + 1 > copy_n) {
            copy_n = sk + 1;
        }
    }
    splat = (cm == 1 || gx.vcd[GX_VA_CLR0] == GX_NONE);
    k[0] = (u8)cm;
    k[1] = (u8)copy_n;
    k[2] = (u8)splat;
    k[3] = splat ? cc->mat.r : 0;
    k[4] = splat ? cc->mat.g : 0;
    k[5] = splat ? cc->mat.b : 0;
    k[6] = splat ? cc->mat.a : 0;
    k[7] = 0;
    return hash_bytes(k, sizeof(k), h);
}

/* The array-contents check, memoised for the frame.
 *
 * A model is one mesh and several display lists over the same position,
 * normal, colour and texcoord arrays, so a board frame checks the same few
 * hundred kilobytes over and over.  Hashing each (base, length) once a frame
 * turns that back into one pass.  The memo is dropped at every frame boundary,
 * so a model the CPU morphs between frames is caught; what it cannot see is an
 * array rewritten *between two draws inside one frame*, which would need the
 * same model animated and drawn twice in a frame from the same buffer, and the
 * frame md5s say it does not happen. */
/* 8,192 slots with a four-way probe, not 512 with none.  A board frame calls
 * GXCallDisplayList about 850 times over a few thousand distinct (array,
 * length) pairs; at 512 slots the memo thrashed, every thrash re-hashed
 * kilobytes of vertex array, and the "cache" was slower than the decode it
 * replaced -- which is what the first M9 measurement said, and is why
 * `stat_arr_bytes` exists. */
#define ARRHASH_SLOTS 8192
#define ARRHASH_PROBE 4
typedef struct ArrHash {
    const u8* base;
    u32 len;
    u32 hash;
    unsigned frame;
} ArrHash;
static ArrHash arr_memo[ARRHASH_SLOTS];
static u32 arr_memo_next;
/* Bytes actually walked, so the report can say whether the validity check or
 * the decode it avoids is the expensive half. */
static double stat_arr_bytes, stat_list_bytes;

static u32 array_range_hash(const u8* base, u32 len, unsigned epoch) {
    unsigned h0 = (((unsigned)(uintptr_t)base >> 4) ^ (len * 2654435761u)) &
                  (ARRHASH_SLOTS - 1);
    unsigned i;
    ArrHash* m;
    for (i = 0; i < ARRHASH_PROBE; i++) {
        m = &arr_memo[(h0 + i) & (ARRHASH_SLOTS - 1)];
        if (m->base == base && m->len == len && m->frame == epoch) {
            return m->hash;
        }
    }
    /* Take the first slot that is not already current for this frame, so a
     * probe run never evicts a neighbour that is still being asked for. */
    m = &arr_memo[h0];
    for (i = 0; i < ARRHASH_PROBE; i++) {
        ArrHash* c = &arr_memo[(h0 + i) & (ARRHASH_SLOTS - 1)];
        if (c->frame != epoch) {
            m = c;
            break;
        }
    }
    if (i == ARRHASH_PROBE) {
        m = &arr_memo[(h0 + (arr_memo_next++ & (ARRHASH_PROBE - 1))) &
                      (ARRHASH_SLOTS - 1)];
    }
    m->base = base;
    m->len = len;
    m->frame = epoch;
    m->hash = hash_bytes(base, len, 2166136261u);
    stat_arr_bytes += (double)len;
    return m->hash;
}

static u32 dlc_array_hash(const u32* arr_off, const u32* arr_len) {
    u32 h = 2166136261u;
    int a;
    for (a = 0; a < GX_MAX_ATTR; a++) {
        if (arr_len[a] && gx.array[a].base) {
            u32 r = array_range_hash(gx.array[a].base + arr_off[a], arr_len[a],
                                     gx_array_epoch);
            h = (h ^ r) * 16777619u;
        }
    }
    return h;
}

static void dlc_free(DlEntry* e) {
    dlc_bytes -= e->bytes;
    free(e->src);
    free(e->segs);
    free(e);
}

/* Evict anything that has not been replayed for a couple of frames.  A scene
 * change swaps every model at once, so this is a cliff rather than a trickle
 * and an LRU list would be bookkeeping for nothing. */
static unsigned dlc_last_sweep_frame;

static void dlc_sweep_age(unsigned now, unsigned age) {
    int b;
    for (b = 0; b < DLC_BUCKETS; b++) {
        DlEntry** pp = &dlc[b];
        while (*pp) {
            DlEntry* e = *pp;
            if (e->last_frame + age < now) {
                *pp = e->next;
                stat_dlc_evict++;
                stat_dlc_entries--;
                dlc_free(e);
            } else {
                pp = &e->next;
            }
        }
    }
}

static void dlc_sweep(unsigned now) { dlc_sweep_age(now, 2); }

static void dlc_report(void) {
    unsigned tot = stat_dlc_hit + stat_dlc_new + stat_dlc_list + stat_dlc_array +
                   stat_dlc_state + stat_dlc_big;
    if (!tot) {
        return;
    }
    port_log("port> DL cache: %u calls, %u hits (%.1f%%), %u first-sight, "
             "%u arrays rewritten, %u list changed, %u state changed, %u too big\n",
             tot, stat_dlc_hit, 100.0 * stat_dlc_hit / tot, stat_dlc_new,
             stat_dlc_array, stat_dlc_list, stat_dlc_state, stat_dlc_big);
    port_log("port> DL cache: validity cost %.0f MB of array hashing and %.0f MB "
             "of list hashing over the run\n",
             stat_arr_bytes / 1048576.0, stat_list_bytes / 1048576.0);
    port_log("port> DL cache: %u live entries, %u KB, %u evicted; %u of them "
             "animated (their arrays moved), %u static\n",
             stat_dlc_entries, (unsigned)(dlc_bytes / 1024), stat_dlc_evict,
             stat_dlc_dynamic,
             stat_dlc_entries > stat_dlc_dynamic ? stat_dlc_entries - stat_dlc_dynamic
                                                 : 0);
}

/* Per-list decode bookkeeping: how far into each array the indices reached, so
 * the validity check knows exactly which bytes to look at. */
static u32 dl_minidx[GX_MAX_ATTR], dl_maxidx[GX_MAX_ATTR];
static DlSeg dl_segs[1024];
static int dl_nsegs;

static void dlc_store(const void* list, u32 nbytes, u32 lh, u32 sh, u32 total,
                      size_t list_pos, unsigned frame, int born_dynamic) {
    DlEntry* e;
    size_t sbytes = (size_t)total * (size_t)sl.stride;
    size_t gbytes = (size_t)dl_nsegs * sizeof(DlSeg);
    int a;
    if (!total || dl_nsegs <= 0 || dl_nsegs > (int)(sizeof(dl_segs) / sizeof(dl_segs[0]))) {
        return;
    }
    if (sbytes + gbytes > DLC_MAX_ENTRY_BYTES) {
        stat_dlc_big++;
        return;
    }
    if (dlc_bytes + sbytes + gbytes > DLC_MAX_BYTES) {
        dlc_sweep(frame);
        if (dlc_bytes + sbytes + gbytes > DLC_MAX_BYTES) {
            stat_dlc_big++;
            return;
        }
    }
    e = (DlEntry*)calloc(1, sizeof(*e));
    if (!e) {
        return;
    }
    e->src = (u8*)malloc(sbytes);
    e->segs = (DlSeg*)malloc(gbytes);
    if (!e->src || !e->segs) {
        free(e->src);
        free(e->segs);
        free(e);
        return;
    }
    memcpy(e->src, src_buf + list_pos, sbytes);
    memcpy(e->segs, dl_segs, gbytes);
    e->list = list;
    e->nbytes = nbytes;
    e->list_hash = lh;
    e->state_hash = sh;
    e->stride = sl.stride;
    e->nverts = total;
    e->nsegs = dl_nsegs;
    e->last_frame = frame;
    e->dynamic = born_dynamic;
    e->bytes = sbytes + gbytes + sizeof(*e);
    for (a = 0; a < GX_MAX_ATTR; a++) {
        if (dl_maxidx[a] == 0xFFFFFFFFu || !gx.array[a].stride) {
            e->arr_off[a] = 0;
            e->arr_len[a] = 0;
        } else {
            u32 st = (u32)gx.array[a].stride;
            e->arr_off[a] = dl_minidx[a] * st;
            e->arr_len[a] = (dl_maxidx[a] - dl_minidx[a] + 1) * st;
        }
    }
    e->array_hash = dlc_array_hash(e->arr_off, e->arr_len);
    {
        unsigned b = ((unsigned)(uintptr_t)list >> 5) & (DLC_BUCKETS - 1);
        e->next = dlc[b];
        dlc[b] = e;
    }
    dlc_bytes += e->bytes;
    stat_dlc_entries++;
    if (born_dynamic) {
        stat_dlc_dynamic++;
    }
}

/* The display-list cache needs the range of each array a list actually read.
 * It is only reached from the tracked instantiation of the decode loop. */
/* --decodestats (M17, PLAN.md 32): what an indexed submit could skip.  Per
 * vertex of the tracked decode: do all of its indexed attributes carry the
 * same index (then the game's own arrays could be bound as they stand), and
 * has this exact (arrays, indices) tuple been decoded already this frame
 * (then a vertex cache could hand back a GL index instead of decoding it
 * again)?  A 64K-entry stamp table keyed on the frame answers the second
 * without a clear per frame. */
static u32 ds_ix[GX_MAX_ATTR];
static int ds_n;
static unsigned long ds_verts, ds_same, ds_unique, ds_multi;
#define DS_HASH 65536
static u32 ds_tab[DS_HASH];
static u32 ds_stamp[DS_HASH];
/* the same question scoped to the batch (M16's unit of submit): what a cache
 * that lives as long as the batch could skip */
static u32 ds_btab[DS_HASH];
static u32 ds_bstamp[DS_HASH];
static unsigned long ds_unique_batch;

static void ds_vertex(void) {
    u32 h = 2166136261u, epoch;
    int i, same = 1;
    ds_verts++;
    if (ds_n < 2) {
        return;
    }
    ds_multi++;
    for (i = 1; i < ds_n; i++) {
        if (ds_ix[i] != ds_ix[0]) {
            same = 0;
            break;
        }
    }
    ds_same += same;
    for (i = 0; i < nplan; i++) {
        h = (h ^ (u32)(uintptr_t)plan[i].base) * 16777619u;
    }
    for (i = 0; i < ds_n; i++) {
        h = (h ^ ds_ix[i]) * 16777619u;
    }
    i = (int)((h ^ (h >> 16)) & (DS_HASH - 1));
    epoch = stat_batches + 1;
    if (!(ds_bstamp[i] == epoch && ds_btab[i] == h)) {
        ds_bstamp[i] = epoch;
        ds_btab[i] = h;
        ds_unique_batch++;
    }
    epoch = gl13_frame_number() + 1;
    if (ds_stamp[i] == epoch && ds_tab[i] == h) {
        return; /* seen this frame */
    }
    ds_stamp[i] = epoch;
    ds_tab[i] = h;
    ds_unique++;
}

/* --decodestats, part two: which decode plans (attribute, index width, op)
 * the vertices actually go through, so a specialised loop is written for the
 * shapes that matter and not the 28 the sources declare. */
#define DS_PLANS 32
static struct {
    u32 key;
    unsigned long verts, prims;
    char desc[96];
} ds_plan[DS_PLANS];
static int ds_nplan;
static int ds_cur_plan = -1;

static const char* ds_opname(int op) {
    static const char* n[] = { "none", "f32x2", "f32x3", "f32x1", "f32x2",
                               "s16x2", "s16x3", "s16x1", "s16x2", "u16x2", "u16x3",
                               "u16x1", "u16x2", "s8x2",  "s8x3",  "s8x1",  "s8x2",
                               "u8x2",  "u8x3",  "u8x1",  "u8x2",  "rgba8", "rgbx8",
                               "rgb8",  "rgb565", "rgba4", "rgba6", "s8x2t", "s8x3t",
                               "s8x1t", "s8x2t", "u8x2t", "u8x3t", "u8x1t", "u8x2t" };
    return (op >= 0 && op < (int)(sizeof(n) / sizeof(n[0]))) ? n[op] : "?";
}

static void ds_plan_note(u32 count) {
    u32 h = 2166136261u;
    int i;
    for (i = 0; i < nplan; i++) {
        h = (h ^ ((u32)plan[i].attr << 16 | (u32)plan[i].idx << 8 | plan[i].op)) * 16777619u;
    }
    h = (h ^ (u32)prim) * 16777619u;
    if (ds_cur_plan < 0 || ds_plan[ds_cur_plan].key != h) {
        for (i = 0; i < ds_nplan; i++) {
            if (ds_plan[i].key == h) {
                break;
            }
        }
        if (i == ds_nplan) {
            if (ds_nplan == DS_PLANS) {
                return;
            }
            ds_nplan++;
            ds_plan[i].key = h;
            ds_plan[i].verts = ds_plan[i].prims = 0;
            {
                int n = snprintf(ds_plan[i].desc, sizeof(ds_plan[i].desc), "prim %02x:", prim);
                int j;
                for (j = 0; j < nplan && n < (int)sizeof(ds_plan[i].desc) - 12; j++) {
                    n += snprintf(ds_plan[i].desc + n, sizeof(ds_plan[i].desc) - (size_t)n,
                                  " %d%s%s", plan[j].attr,
                                  plan[j].idx == 2 ? "/i16 " : plan[j].idx == 1 ? "/i8 " : "/d ",
                                  ds_opname(plan[j].op));
                }
            }
        }
        ds_cur_plan = i;
    }
    ds_plan[ds_cur_plan].verts += count;
    ds_plan[ds_cur_plan].prims++;
}

void gx_decodestats_report(void) {
    int i;
    if (!ds_verts) {
        return;
    }
    for (i = 0; i < ds_nplan; i++) {
        port_log("port> decodestats: %10lu verts %8lu prims  %s\n", ds_plan[i].verts,
                 ds_plan[i].prims, ds_plan[i].desc);
    }
    port_log("port> decodestats: %lu vertices decoded, %lu with 2+ indexed attributes, "
             "of which %lu (%.1f%%) share one index; %lu (%.1f%%) were the first "
             "occurrence of their (arrays, indices) tuple in their frame, "
             "%lu (%.1f%%) in their batch\n",
             ds_verts, ds_multi, ds_same, ds_multi ? 100.0 * ds_same / ds_multi : 0.0,
             ds_unique, ds_multi ? 100.0 * ds_unique / ds_multi : 0.0,
             ds_unique_batch, ds_multi ? 100.0 * ds_unique_batch / ds_multi : 0.0);
}

static void dl_track_index(u8 attr, u32 ix) {
    ds_ix[ds_n++] = ix;
    if (dl_maxidx[attr] == 0xFFFFFFFFu) {
        dl_maxidx[attr] = dl_minidx[attr] = ix;
    } else if (ix > dl_maxidx[attr]) {
        dl_maxidx[attr] = ix;
    } else if (ix < dl_minidx[attr]) {
        dl_minidx[attr] = ix;
    }
}

/* Big-endian is the disc's byte order and the G4's, so a 16- or 32-bit
 * component is a load; PowerPC does the unaligned case in hardware.  The
 * portable spelling is for the little-endian development host and is the same
 * arithmetic read_component() does there. */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define DEC_U16(q, i) ((f32) * (const u16*)((q) + (i) * 2))
#define DEC_S16(q, i) ((f32) * (const s16*)((q) + (i) * 2))
#define DEC_F32(q, i) (*(const f32*)((q) + (i) * 4))
#else
#define DEC_U16(q, i) ((f32)(u16)(((q)[(i) * 2] << 8) | (q)[(i) * 2 + 1]))
#define DEC_S16(q, i) ((f32)(s16)(((q)[(i) * 2] << 8) | (q)[(i) * 2 + 1]))
static f32 dec_f32_portable(const u8* q) {
    union { f32 f; u32 u; } c;
    c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
    return c.f;
}
#define DEC_F32(q, i) dec_f32_portable((q) + (i) * 4)
#endif
#define DEC_U8(q, i) ((f32)(q)[i])
#define DEC_S8(q, i) ((f32)(s8)(q)[i])

/* The two shapes every non-colour op has: read n, write n or n+1 with the
 * extra component zeroed, scaling by the VAT's fraction. */
#define DEC_CASE_TYPE(TAG, RD)                                                           \
    case DEC_##TAG##_2_3:                                                                \
        dp[0] = RD(q, 0) * sc;                                                           \
        dp[1] = RD(q, 1) * sc;                                                           \
        dp[2] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_##TAG##_3_3:                                                                \
        dp[0] = RD(q, 0) * sc;                                                           \
        dp[1] = RD(q, 1) * sc;                                                           \
        dp[2] = RD(q, 2) * sc;                                                           \
        break;                                                                           \
    case DEC_##TAG##_1_2:                                                                \
        dp[0] = RD(q, 0) * sc;                                                           \
        dp[1] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_##TAG##_2_2:                                                                \
        dp[0] = RD(q, 0) * sc;                                                           \
        dp[1] = RD(q, 1) * sc;                                                           \
        break;

/* f32 is the same four cases without the multiply: the VAT's fraction does not
 * apply to a float component, and read_component() did not apply it either. */
#define DEC_CASE_F32                                                                     \
    case DEC_F32_2_3:                                                                    \
        dp[0] = DEC_F32(q, 0);                                                           \
        dp[1] = DEC_F32(q, 1);                                                           \
        dp[2] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_F32_3_3:                                                                    \
        dp[0] = DEC_F32(q, 0);                                                           \
        dp[1] = DEC_F32(q, 1);                                                           \
        dp[2] = DEC_F32(q, 2);                                                           \
        break;                                                                           \
    case DEC_F32_1_2:                                                                    \
        dp[0] = DEC_F32(q, 0);                                                           \
        dp[1] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_F32_2_2:                                                                    \
        dp[0] = DEC_F32(q, 0);                                                           \
        dp[1] = DEC_F32(q, 1);                                                           \
        break;

/* The table form: one load per component, no multiply (the scale is in the
 * table), no integer-to-float dance. */
#define DEC_CASE_TBL(TAG)                                                                \
    case DEC_##TAG##_2_3:                                                                \
        dp[0] = tb[q[0]];                                                                \
        dp[1] = tb[q[1]];                                                                \
        dp[2] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_##TAG##_3_3:                                                                \
        dp[0] = tb[q[0]];                                                                \
        dp[1] = tb[q[1]];                                                                \
        dp[2] = tb[q[2]];                                                                \
        break;                                                                           \
    case DEC_##TAG##_1_2:                                                                \
        dp[0] = tb[q[0]];                                                                \
        dp[1] = 0.0f;                                                                    \
        break;                                                                           \
    case DEC_##TAG##_2_2:                                                                \
        dp[0] = tb[q[0]];                                                                \
        dp[1] = tb[q[1]];                                                                \
        break;

#define DEC_CASE_COLOUR                                                                  \
    case DEC_CLR_RGBA8:                                                                  \
        memcpy(d, q, 4);                                                                 \
        break;                                                                           \
    case DEC_CLR_RGBX8:                                                                  \
    case DEC_CLR_RGB8:                                                                   \
        d[0] = q[0];                                                                     \
        d[1] = q[1];                                                                     \
        d[2] = q[2];                                                                     \
        d[3] = 255;                                                                      \
        break;                                                                           \
    case DEC_CLR_RGB565: {                                                               \
        u32 c = (u32)((q[0] << 8) | q[1]);                                               \
        d[0] = (u8)(((c >> 11) & 0x1F) * 255 / 31);                                      \
        d[1] = (u8)(((c >> 5) & 0x3F) * 255 / 63);                                       \
        d[2] = (u8)((c & 0x1F) * 255 / 31);                                              \
        d[3] = 255;                                                                      \
        break;                                                                           \
    }                                                                                    \
    case DEC_CLR_RGBA4: {                                                                \
        u32 c = (u32)((q[0] << 8) | q[1]);                                               \
        d[0] = (u8)(((c >> 12) & 0xF) * 17);                                             \
        d[1] = (u8)(((c >> 8) & 0xF) * 17);                                              \
        d[2] = (u8)(((c >> 4) & 0xF) * 17);                                              \
        d[3] = (u8)((c & 0xF) * 17);                                                     \
        break;                                                                           \
    }                                                                                    \
    case DEC_CLR_RGBA6: {                                                                \
        u32 c = ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];                             \
        d[0] = (u8)(((c >> 18) & 0x3F) * 255 / 63);                                      \
        d[1] = (u8)(((c >> 12) & 0x3F) * 255 / 63);                                      \
        d[2] = (u8)(((c >> 6) & 0x3F) * 255 / 63);                                       \
        d[3] = (u8)((c & 0x3F) * 255 / 63);                                              \
        break;                                                                           \
    }

/* One primitive's vertices, straight from the list into the packed source
 * layout.  TRACK is the display-list cache's index-range bookkeeping, which is
 * off in the shipped build; instantiating the loop twice keeps it out of the
 * hot one entirely rather than paying a branch per attribute for it. */
#define DECODE_RUN(NAME, TRACK)                                                          \
    static const u8* NAME(const u8* p, const u8* end, u32 count) {                       \
        u32 i;                                                                           \
        u8* lastv = NULL;                                                                \
        for (i = 0; i < count && p < end; i++) {                                         \
            const DecStep* st = plan;                                                    \
            u8* v;                                                                       \
            int j;                                                                       \
            u32 pos_ix = 0, nrm_ix = 0;                                                  \
            if (nverts >= MAX_VERTS ||                                                   \
                run_pos + (size_t)(nverts + 1) * sl.stride > src_cap) {                  \
                gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");\
                v = sink_vtx; /* decoded and dropped, so the list still steps */         \
            } else {                                                                     \
                v = src_buf + run_pos + (size_t)nverts * sl.stride;                      \
                nverts++;                                                                \
                lastv = v;                                                               \
            }                                                                            \
            if (TRACK) {                                                                 \
                ds_n = 0;                                                                \
            }                                                                            \
            if (plan_clr_const) {                                                        \
                *(u32*)(v + sl.off_clr) = plan_clr.u;                                    \
            }                                                                            \
            for (j = 0; j < plan_nfill; j++) {                                           \
                f32* t = (f32*)(v + plan_fill[j].dstoff);                                \
                t[0] = plan_fill[j].s;                                                   \
                t[1] = plan_fill[j].t;                                                   \
            }                                                                            \
            for (j = nplan; j > 0; j--, st++) {                                          \
                const u8* q;                                                             \
                u8* d;                                                                   \
                f32* dp;                                                                 \
                f32 sc;                                                                  \
                const f32* tb;                                                           \
                if (st->idx == 2) {                                                      \
                    u32 ix = ((u32)p[0] << 8) | p[1];                                     \
                    q = st->base + (size_t)ix * st->stride;                              \
                    if (TRACK) {                                                         \
                        dl_track_index(st->attr, ix);                                    \
                    }                                                                    \
                    if (st->attr == GX_VA_POS) {                                         \
                        pos_ix = ix;                                                     \
                    } else if (st->attr == GX_VA_NRM) {                                  \
                        nrm_ix = ix;                                                     \
                    }                                                                    \
                    p += 2;                                                              \
                } else if (st->idx == 1) {                                               \
                    u32 ix = p[0];                                                       \
                    q = st->base + (size_t)ix * st->stride;                              \
                    if (TRACK) {                                                         \
                        dl_track_index(st->attr, ix);                                    \
                    }                                                                    \
                    if (st->attr == GX_VA_POS) {                                         \
                        pos_ix = ix;                                                     \
                    } else if (st->attr == GX_VA_NRM) {                                  \
                        nrm_ix = ix;                                                     \
                    }                                                                    \
                    p += 1;                                                              \
                } else {                                                                 \
                    q = p;                                                               \
                    p += st->advance;                                                    \
                }                                                                        \
                d = st->to_pending ? (u8*)&pending + st->dstoff : v + st->dstoff;        \
                dp = (f32*)d;                                                            \
                sc = st->scale;                                                          \
                tb = st->tbl;                                                            \
                switch (st->op) {                                                        \
                    DEC_CASE_F32                                                         \
                    DEC_CASE_TYPE(S16, DEC_S16)                                          \
                    DEC_CASE_TYPE(U16, DEC_U16)                                          \
                    DEC_CASE_TYPE(S8, DEC_S8)                                            \
                    DEC_CASE_TYPE(U8, DEC_U8)                                            \
                    DEC_CASE_TBL(TS8)                                                    \
                    DEC_CASE_TBL(TU8)                                                    \
                    DEC_CASE_COLOUR                                                      \
                    default: break; /* DEC_NONE: a null array, as before */              \
                }                                                                        \
            }                                                                            \
            if (sl.off_skin >= 0) {                                                      \
                f32 sf = pi.slotf;                                                       \
                if (pi.skin) {                                                           \
                    sf = pos_ix < (u32)pi.skin->nvtx                                     \
                             ? pi.skin->ent_slotf[pi.skin->pos_ent[pos_ix]] : 0.0f;      \
                    if (TRACK && port_opt.skinstats) {                                   \
                        gx_skin_count_vertex(pi.skin, pos_ix, nrm_ix, sl.off_nrm >= 0);  \
                    }                                                                    \
                }                                                                        \
                *(f32*)(v + sl.off_skin) = sf;                                           \
            }                                                                            \
            if (TRACK && port_opt.decodestats) {                                         \
                ds_vertex();                                                             \
            }                                                                            \
        }                                                                                \
        /* Leave `pending` holding the last vertex's texcoords, which is what the        \
         * old path left behind and what the next primitive's fill[] reads. */           \
        if (lastv) {                                                                     \
            for (i = 0; i < (u32)plan_nback; i++) {                                      \
                const f32* t = (const f32*)(lastv + plan_back[i].dstoff);                \
                pending.tex[plan_back[i].k][0] = t[0];                                   \
                pending.tex[plan_back[i].k][1] = t[1];                                   \
            }                                                                            \
        }                                                                                \
        return p;                                                                        \
    }

DECODE_RUN(decode_run, 0)
DECODE_RUN(decode_run_tracked, 1)

/* ---- the specialised loops (M17, PLAN.md 32) -------------------------------
 *
 * The plan walker above pays, per attribute of every vertex, a switch on the
 * op (an indirect branch the 7450 predicts badly) and a load of every step
 * field.  --decodestats says the game's display lists go through a handful
 * of plan shapes -- HSF models are POS f32 / NRM s8 / CLR0 rgba8 / TEX0 f32,
 * all GX_INDEX16 (hsfdraw.c:506-600), with the colour or the texcoord
 * absent for some materials -- so those shapes get a loop of their own with
 * the sequence fixed at compile time: four index loads, the loads and
 * stores, nothing else.  The stores are the same bytes in the same places
 * as the general loop's, which is why the md5s hold.
 *
 * NRM: 0 none, 1 s8 through the byte table, 2 f32.  CLR: 0 not in the
 * descriptor, 1 rgba8 into the vertex, 2 rgba8 into `pending` (the register
 * material wins, but the index is still consumed and the old path still
 * stored it).  TEX: 0 none, 1 f32 s/t into slot 0.  Every step GX_INDEX16. */
#define DECODE_FAST(NAME, NRM, CLR, TEX, SKIN)                                           \
    static const u8* NAME(const u8* p, const u8* end, u32 count) {                       \
        const int PREFETCH = !port_opt.nodcbt;                                       \
        const int off_skin = sl.off_skin;                                                \
        const f32 slotf = pi.slotf;                                                      \
        const f32* entf = (SKIN == 2) ? pi.skin->ent_slotf : NULL;                       \
        const u16* pent = (SKIN == 2) ? pi.skin->pos_ent : NULL;                         \
        const u32 nvtx = (SKIN == 2) ? (u32)pi.skin->nvtx : 0;                           \
        const u8* pb = plan[0].base;                                                     \
        const u32 ps = plan[0].stride;                                                   \
        const u8* nb = NRM ? plan[1].base : NULL;                                        \
        const u32 ns = NRM ? plan[1].stride : 0;                                         \
        const f32* nt = (NRM == 1) ? plan[1].tbl : NULL;                                 \
        const int ci = NRM ? 2 : 1;                                                      \
        const u8* cb = CLR ? plan[ci].base : NULL;                                       \
        const u32 cs = CLR ? plan[ci].stride : 0;                                        \
        const int ti = ci + (CLR ? 1 : 0);                                               \
        const u8* tb = TEX ? plan[ti].base : NULL;                                       \
        const u32 ts = TEX ? plan[ti].stride : 0;                                        \
        const int off_nrm = sl.off_nrm, off_clr = sl.off_clr, off_tex = sl.off_tex;      \
        const int per = 2 * (1 + (NRM ? 1 : 0) + (CLR ? 1 : 0) + (TEX ? 1 : 0));         \
        u8* lastv = NULL;                                                                \
        u32 i;                                                                           \
        if (plan_clr_const) {                                                            \
            /* the register material, splatted once per vertex below */                 \
        }                                                                                \
        for (i = 0; i < count && p + per <= end; i++) {                                  \
            u8* v;                                                                       \
            const u8* q;                                                                 \
            u32 ix;                                                                      \
            if (nverts >= MAX_VERTS ||                                                   \
                run_pos + (size_t)(nverts + 1) * sl.stride > src_cap) {                  \
                gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");\
                v = sink_vtx;                                                            \
            } else {                                                                     \
                v = src_buf + run_pos + (size_t)nverts * sl.stride;                      \
                nverts++;                                                                \
                lastv = v;                                                               \
            }                                                                            \
            if (plan_clr_const) {                                                        \
                *(u32*)(v + off_clr) = plan_clr.u;                                       \
            }                                                                            \
            if (PREFETCH && p + 2 * per <= end) {                                         \
                /* the next vertex's array entries: the indices are right     \
                 * there in the list, and the arrays are the random reads     \
                 * this loop waits on (dcbt on the 7450) */                   \
                const u8* pn = p + per;                                                  \
                __builtin_prefetch(pb + (size_t)(((u32)pn[0] << 8) | pn[1]) * ps);       \
                if (NRM) {                                                               \
                    __builtin_prefetch(nb + (size_t)(((u32)pn[2] << 8) | pn[3]) * ns);   \
                }                                                                        \
                if (TEX) {                                                               \
                    __builtin_prefetch(tb + (size_t)(((u32)pn[per - 2] << 8) |           \
                                                     pn[per - 1]) * ts);                 \
                }                                                                        \
            }                                                                            \
            ix = ((u32)p[0] << 8) | p[1];                                                \
            q = pb + (size_t)ix * ps;                                                    \
            ((f32*)v)[0] = DEC_F32(q, 0);                                                \
            ((f32*)v)[1] = DEC_F32(q, 1);                                                \
            ((f32*)v)[2] = DEC_F32(q, 2);                                                \
            if (SKIN == 1) {                                                             \
                *(f32*)(v + off_skin) = slotf;                                           \
            } else if (SKIN == 2) {                                                      \
                *(f32*)(v + off_skin) = ix < nvtx ? entf[pent[ix]] : 0.0f;               \
            }                                                                            \
            p += 2;                                                                      \
            if (NRM) {                                                                   \
                f32* dp = (f32*)(v + off_nrm);                                           \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = nb + (size_t)ix * ns;                                                \
                if (NRM == 1) {                                                          \
                    dp[0] = nt[q[0]];                                                    \
                    dp[1] = nt[q[1]];                                                    \
                    dp[2] = nt[q[2]];                                                    \
                } else {                                                                 \
                    dp[0] = DEC_F32(q, 0);                                               \
                    dp[1] = DEC_F32(q, 1);                                               \
                    dp[2] = DEC_F32(q, 2);                                               \
                }                                                                        \
                p += 2;                                                                  \
            }                                                                            \
            if (CLR) {                                                                   \
                u8* d = (CLR == 1) ? v + off_clr : pending.clr[0];                       \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = cb + (size_t)ix * cs;                                                \
                memcpy(d, q, 4);                                                         \
                p += 2;                                                                  \
            }                                                                            \
            if (TEX) {                                                                   \
                f32* dp = (f32*)(v + off_tex);                                           \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = tb + (size_t)ix * ts;                                                \
                dp[0] = DEC_F32(q, 0);                                                   \
                dp[1] = DEC_F32(q, 1);                                                   \
                p += 2;                                                                  \
            }                                                                            \
        }                                                                                \
        if (lastv && TEX) {                                                              \
            const f32* t = (const f32*)(lastv + off_tex);                                \
            pending.tex[0][0] = t[0];                                                    \
            pending.tex[0][1] = t[1];                                                    \
        }                                                                                \
        return p;                                                                        \
    }

/* The eight shapes --decodestats found on the 9,000-frame walk (PLAN.md 32):
 * 49% of all vertices are POS/NRM f32/TEX0, 39% POS/NRM s8/TEX0, 7% and 1%
 * the same two without a texcoord, and the four with a vertex colour share
 * the last 4%.  No shape stores a colour in `pending` (the register
 * material wins with no CLR0 in the descriptor), so CLR 2 is not built. */
#define DECODE_FAST3(NAME, NRM, CLR, TEX)                                                \
    DECODE_FAST(NAME##s0, NRM, CLR, TEX, 0)                                              \
    DECODE_FAST(NAME##s1, NRM, CLR, TEX, 1)                                              \
    DECODE_FAST(NAME##s2, NRM, CLR, TEX, 2)
DECODE_FAST3(decode_fast_n2c0t1, 2, 0, 1)
DECODE_FAST3(decode_fast_n1c0t1, 1, 0, 1)
DECODE_FAST3(decode_fast_n1c0t0, 1, 0, 0)
DECODE_FAST3(decode_fast_n2c0t0, 2, 0, 0)
DECODE_FAST3(decode_fast_n1c1t1, 1, 1, 1)
DECODE_FAST3(decode_fast_n2c1t1, 2, 1, 1)
DECODE_FAST3(decode_fast_n0c1t1, 0, 1, 1)
DECODE_FAST3(decode_fast_n1c1t0, 1, 1, 0)
/* the SKIN axis (M18): 0 no palette, 1 a plain primitive's one slot, 2 a
 * skinned mesh's per-vertex entry */
#define PICK3(NAME)                                                                      \
    (skin == 0 ? NAME##s0 : skin == 1 ? NAME##s1 : NAME##s2)

/* Match the plan against the shapes above; NULL when none fits and the
 * general walker runs.  Called once per primitive from build_decode_plan. */
static DecodeFast pick_fast(void) {
    int i = 0, nrm = 0, clr = 0, tex = 0;
    const int skin = sl.off_skin < 0 ? 0 : pi.skin ? 2 : 1;
    if (port_opt.olddecode3 || !plan_ok || plan_nfill != 0 || nplan < 2 || nplan > 4) {
        return NULL;
    }
    for (i = 0; i < nplan; i++) {
        if (plan[i].idx != 2 || !plan[i].base) {
            return NULL;
        }
    }
    i = 0;
    if (plan[i].attr != GX_VA_POS || plan[i].op != DEC_F32_3_3 || plan[i].to_pending ||
        plan[i].dstoff != 0) {
        return NULL;
    }
    i++;
    if (i < nplan && plan[i].attr == GX_VA_NRM) {
        if (plan[i].to_pending || (int)plan[i].dstoff != sl.off_nrm) {
            return NULL;
        }
        if (plan[i].op == DEC_TS8_3_3 && plan[i].tbl) {
            nrm = 1;
        } else if (plan[i].op == DEC_F32_3_3) {
            nrm = 2;
        } else {
            return NULL;
        }
        i++;
    }
    if (i < nplan && plan[i].attr == GX_VA_CLR0) {
        if (plan[i].op != DEC_CLR_RGBA8) {
            return NULL;
        }
        if (plan[i].to_pending) {
            if (plan[i].dstoff != (u16)offsetof(Pending, clr)) {
                return NULL;
            }
            clr = 2;
        } else {
            if ((int)plan[i].dstoff != sl.off_clr) {
                return NULL;
            }
            clr = 1;
        }
        i++;
    }
    if (i < nplan && plan[i].attr == GX_VA_TEX0) {
        if (plan[i].op != DEC_F32_2_2 || plan[i].to_pending ||
            (int)plan[i].dstoff != sl.off_tex || sl.ntex != 1) {
            return NULL;
        }
        tex = 1;
        i++;
    }
    if (i != nplan) {
        return NULL;
    }
    if (tex && plan_nback != 1) {
        return NULL;
    }
    if (!tex && (plan_nback != 0 || sl.ntex != 0)) {
        return NULL;
    }
    if (nrm == 2 && clr == 0 && tex == 1) return PICK3(decode_fast_n2c0t1);
    if (nrm == 1 && clr == 0 && tex == 1) return PICK3(decode_fast_n1c0t1);
    if (nrm == 1 && clr == 0 && tex == 0) return PICK3(decode_fast_n1c0t0);
    if (nrm == 2 && clr == 0 && tex == 0) return PICK3(decode_fast_n2c0t0);
    if (nrm == 1 && clr == 1 && tex == 1) return PICK3(decode_fast_n1c1t1);
    if (nrm == 2 && clr == 1 && tex == 1) return PICK3(decode_fast_n2c1t1);
    if (nrm == 0 && clr == 1 && tex == 1) return PICK3(decode_fast_n0c1t1);
    if (nrm == 1 && clr == 1 && tex == 0) return PICK3(decode_fast_n1c1t0);
    return NULL;
}

/* ---- M29: the decode as a job for the render thread (PLAN.md 44) ----------
 *
 * The same eight shapes and the same general walk as above, reading the
 * plan from the job instead of the file's statics and writing nothing but
 * the ring bytes: no `pending`, no `nverts`, no counters.  The stores are
 * the same bytes in the same places as the loops above, which is what the
 * md5s check.  The game thread keeps `pending` exact with a one-vertex pass
 * over the run's last vertex (decode_pending_last). */
#define DECODE_FAST_JOB(NAME, NRM, CLR, TEX)                                             \
    static u32 NAME(const GxDecJob* j) {                                                 \
        const int PREFETCH = j->prefetch;                                                \
        const u8* p = j->p;                                                              \
        const u8* end = j->end;                                                          \
        const u32 count = j->count;                                                      \
        const u32 stride = j->stride;                                                    \
        const u8* pb = j->plan[0].base;                                                  \
        const u32 ps = j->plan[0].stride;                                                \
        const u8* nb = NRM ? j->plan[1].base : NULL;                                     \
        const u32 ns = NRM ? j->plan[1].stride : 0;                                      \
        const f32* nt = (NRM == 1) ? j->plan[1].tbl : NULL;                              \
        const int ci = NRM ? 2 : 1;                                                      \
        const u8* cb = CLR ? j->plan[ci].base : NULL;                                    \
        const u32 cs = CLR ? j->plan[ci].stride : 0;                                     \
        const int ti = ci + (CLR ? 1 : 0);                                               \
        const u8* tb = TEX ? j->plan[ti].base : NULL;                                    \
        const u32 ts = TEX ? j->plan[ti].stride : 0;                                     \
        const int off_nrm = j->off_nrm, off_clr = j->off_clr, off_tex = j->off_tex;      \
        const int clr_const = j->clr_const;                                              \
        const u32 clr = j->clr;                                                          \
        const int per = 2 * (1 + (NRM ? 1 : 0) + (CLR ? 1 : 0) + (TEX ? 1 : 0));         \
        u8* v = j->dst;                                                                  \
        u32 i;                                                                           \
        for (i = 0; i < count && p + per <= end; i++, v += stride) {                     \
            const u8* q;                                                                 \
            u32 ix;                                                                      \
            if (clr_const) {                                                             \
                *(u32*)(v + off_clr) = clr;                                              \
            }                                                                            \
            if (PREFETCH && p + 2 * per <= end) {                                         \
                const u8* pn = p + per;                                                  \
                __builtin_prefetch(pb + (size_t)(((u32)pn[0] << 8) | pn[1]) * ps);       \
                if (NRM) {                                                               \
                    __builtin_prefetch(nb + (size_t)(((u32)pn[2] << 8) | pn[3]) * ns);   \
                }                                                                        \
                if (TEX) {                                                               \
                    __builtin_prefetch(tb + (size_t)(((u32)pn[per - 2] << 8) |           \
                                                     pn[per - 1]) * ts);                 \
                }                                                                        \
            }                                                                            \
            ix = ((u32)p[0] << 8) | p[1];                                                \
            q = pb + (size_t)ix * ps;                                                    \
            ((f32*)v)[0] = DEC_F32(q, 0);                                                \
            ((f32*)v)[1] = DEC_F32(q, 1);                                                \
            ((f32*)v)[2] = DEC_F32(q, 2);                                                \
            p += 2;                                                                      \
            if (NRM) {                                                                   \
                f32* dp = (f32*)(v + off_nrm);                                           \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = nb + (size_t)ix * ns;                                                \
                if (NRM == 1) {                                                          \
                    dp[0] = nt[q[0]];                                                    \
                    dp[1] = nt[q[1]];                                                    \
                    dp[2] = nt[q[2]];                                                    \
                } else {                                                                 \
                    dp[0] = DEC_F32(q, 0);                                               \
                    dp[1] = DEC_F32(q, 1);                                               \
                    dp[2] = DEC_F32(q, 2);                                               \
                }                                                                        \
                p += 2;                                                                  \
            }                                                                            \
            if (CLR) {                                                                   \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = cb + (size_t)ix * cs;                                                \
                memcpy(v + off_clr, q, 4);                                               \
                p += 2;                                                                  \
            }                                                                            \
            if (TEX) {                                                                   \
                f32* dp = (f32*)(v + off_tex);                                           \
                ix = ((u32)p[0] << 8) | p[1];                                            \
                q = tb + (size_t)ix * ts;                                                \
                dp[0] = DEC_F32(q, 0);                                                   \
                dp[1] = DEC_F32(q, 1);                                                   \
                p += 2;                                                                  \
            }                                                                            \
        }                                                                                \
        return i;                                                                        \
    }
DECODE_FAST_JOB(decj_n2c0t1, 2, 0, 1)
DECODE_FAST_JOB(decj_n1c0t1, 1, 0, 1)
DECODE_FAST_JOB(decj_n1c0t0, 1, 0, 0)
DECODE_FAST_JOB(decj_n2c0t0, 2, 0, 0)
DECODE_FAST_JOB(decj_n1c1t1, 1, 1, 1)
DECODE_FAST_JOB(decj_n2c1t1, 2, 1, 1)
DECODE_FAST_JOB(decj_n0c1t1, 0, 1, 1)
DECODE_FAST_JOB(decj_n1c1t0, 1, 1, 0)
typedef u32 (*DecodeJobFn)(const GxDecJob*);
static const DecodeJobFn dec_fast_job[8] = {
    decj_n2c0t1, decj_n1c0t1, decj_n1c0t0, decj_n2c0t0,
    decj_n1c1t1, decj_n2c1t1, decj_n0c1t1, decj_n1c1t0,
};
/* the index of the shape plan_fast names, or -1: the job carries the number
 * so the render thread never reads this file's statics */
static int fast_index_of(DecodeFast f) {
    if (f == decode_fast_n2c0t1s0) return 0;
    if (f == decode_fast_n1c0t1s0) return 1;
    if (f == decode_fast_n1c0t0s0) return 2;
    if (f == decode_fast_n2c0t0s0) return 3;
    if (f == decode_fast_n1c1t1s0) return 4;
    if (f == decode_fast_n2c1t1s0) return 5;
    if (f == decode_fast_n0c1t1s0) return 6;
    if (f == decode_fast_n1c1t0s0) return 7;
    return -1;
}

/* one vertex of a job's run through the plan: into `v` (stride bytes) and,
 * for the to_pending steps, into `pend`.  The general walker of the job and
 * the game thread's last-vertex pass share it. */
static const u8* decode_job_vertex(const GxDecJob* j, const u8* p, u8* v, Pending* pend) {
    const DecStep* st = j->plan;
    int k;
    if (j->clr_const) {
        *(u32*)(v + j->off_clr) = j->clr;
    }
    for (k = 0; k < j->nfill; k++) {
        f32* t = (f32*)(v + j->fill[k].dstoff);
        t[0] = j->fill[k].s;
        t[1] = j->fill[k].t;
    }
    for (k = j->nplan; k > 0; k--, st++) {
        const u8* q;
        u8* d;
        f32* dp;
        f32 sc;
        const f32* tb;
        if (st->idx == 2) {
            u32 ix = ((u32)p[0] << 8) | p[1];
            q = st->base + (size_t)ix * st->stride;
            p += 2;
        } else if (st->idx == 1) {
            u32 ix = p[0];
            q = st->base + (size_t)ix * st->stride;
            p += 1;
        } else {
            q = p;
            p += st->advance;
        }
        d = st->to_pending ? (u8*)pend + st->dstoff : v + st->dstoff;
        dp = (f32*)d;
        sc = st->scale;
        tb = st->tbl;
        switch (st->op) {
            DEC_CASE_F32
            DEC_CASE_TYPE(S16, DEC_S16)
            DEC_CASE_TYPE(U16, DEC_U16)
            DEC_CASE_TYPE(S8, DEC_S8)
            DEC_CASE_TYPE(U8, DEC_U8)
            DEC_CASE_TBL(TS8)
            DEC_CASE_TBL(TU8)
            DEC_CASE_COLOUR
            default: break;
        }
    }
    return p;
}

u32 gx_decode_job(const GxDecJob* j) {
    if (j->fast >= 0) {
        return dec_fast_job[j->fast](j);
    }
    {
        Pending scratch; /* the to_pending steps' values are the game thread's business */
        const u8* p = j->p;
        u8* v = j->dst;
        u32 i;
        for (i = 0; i < j->count && p < j->end; i++, v += j->stride) {
            p = decode_job_vertex(j, p, v, &scratch);
        }
        return i;
    }
}

/* the bytes of list one vertex of the plan eats, and the vertices a run of
 * `count` will decode from the bytes left -- the loops' own conditions
 * (`p + per <= end` for the specialised ones, `p < end` for the walker),
 * without decoding */
static u32 job_vertex_bytes(const GxDecJob* j) {
    u32 b = 0;
    int k;
    for (k = 0; k < j->nplan; k++) {
        b += j->plan[k].advance;
    }
    return b;
}
static u32 job_vertices(const GxDecJob* j, u32 vbytes) {
    size_t left = j->end > j->p ? (size_t)(j->end - j->p) : 0;
    u32 n;
    if (!vbytes) {
        return 0;
    }
    if (j->fast >= 0) {
        n = (u32)(left / vbytes);
    } else {
        n = (u32)((left + vbytes - 1) / vbytes);
    }
    return n < j->count ? n : j->count;
}

/* The game thread's share of a deferred run: what the loops above left in
 * `pending` -- the to_pending steps' values and plan_back's texcoords, both
 * from the last vertex decoded -- from that one vertex alone. */
static void decode_pending_last(const GxDecJob* j, u32 n, u32 vbytes) {
    u8 scratch[SRC_MAX_STRIDE] __attribute__((aligned(16)));
    const u8* pl;
    int i;
    if (!n) {
        return;
    }
    pl = j->p + (size_t)(n - 1) * vbytes;
    decode_job_vertex(j, pl, scratch, &pending);
    for (i = 0; i < plan_nback; i++) {
        const f32* t = (const f32*)(scratch + plan_back[i].dstoff);
        pending.tex[plan_back[i].k][0] = t[0];
        pending.tex[plan_back[i].k][1] = t[1];
    }
}

/* Build the job for the primitive in hand, or return 0 when the run must be
 * decoded here: a shape the job cannot carry (the palette's per-vertex slot,
 * the display-list cache's index tracking, --decodestats, the premerge),
 * or a run that would not fit the ring (the sink). */
static int rtdec_build(GxDecJob* j, const u8* p, const u8* end, u32 count) {
    if (!rt_decode_want(count) || sl.off_skin >= 0 || premerge_on || port_opt.decodestats ||
        nplan > GX_MAX_ATTR || plan_nfill > GX_DEC_FILL_MAX ||
        run_pos + (size_t)count * sl.stride > src_cap || count >= MAX_VERTS) {
        return 0;
    }
    j->p = p;
    j->end = end;
    j->count = count;
    j->dst = src_buf + run_pos;
    j->stride = (u32)sl.stride;
    j->off_nrm = sl.off_nrm;
    j->off_clr = sl.off_clr;
    j->off_tex = sl.off_tex;
    j->clr_const = plan_clr_const;
    j->clr = plan_clr.u;
    j->prefetch = !port_opt.nodcbt;
    j->fast = plan_fast ? fast_index_of(plan_fast) : -1;
    if (plan_fast && j->fast < 0) {
        return 0; /* a palette shape: the loops above */
    }
    j->nplan = nplan;
    memcpy(j->plan, plan, (size_t)nplan * sizeof(DecStep));
    j->nfill = plan_nfill;
    memcpy(j->fill, plan_fill, (size_t)plan_nfill * sizeof(plan_fill[0]));
    return 1;
}

void GXCallDisplayList(const void* list, u32 nbytes) {
    const u8* p = (const u8*)list;
    const u8* end = p + nbytes;
    u32 state_h = 0, list_h = 0, total = 0;
    unsigned frame;
    DlEntry* hit = NULL;
    DlEntry* found = NULL;
    int caching;
    unsigned nops = 0;    /* primitives in this list, for --submitstats */
    size_t list_pos = 0;  /* ring offset of the list's first run          */
    static GxDecJob rtjob; /* M29: the run handed to the render thread   */
    /* 0 = this (buffer, state) pair has never been seen, 2 = the list's bytes
     * changed, 3 = the arrays the list reads were rewritten under it (an
     * animated model, which is the split the M9 log reports) */
    int miss_reason = 0;
    if (dl_recording) {
        gx_warn("GXCallDisplayList inside a display list is not supported");
        return;
    }
    stat_dls++;
    if (gl13_draw_off()) {
        /* --nodraw / --ffto (PLAN.md 24.1).  A display list is the game
         * handing GX a block of *its own* memory to read; the decode writes
         * nothing the game can observe -- not one byte outside the port's own
         * staging buffers -- so when nothing is going to be drawn there is
         * nothing to do at all.  92% of the board's primitives arrive here,
         * and this early return is most of what makes a fast-forward fast. */
        return;
    }
    dl_replaying = 1;
    port_perf_gx_begin();
    frame = gl13_frame_number();
    caching = port_opt.dlcache && nbytes > 0;
    rt_auto_on = rt_recording && rt_decode_mode() == 3;
    /* A scene change strands every entry it had; sweep on a slow cadence so
     * the table does not grow to hold every model the walk has ever passed. */
    if (caching && frame != dlc_last_sweep_frame && (frame & 255) == 0) {
        dlc_last_sweep_frame = frame;
        dlc_sweep_age(frame, 240);
    }

    if (caching) {
        unsigned b = ((unsigned)(uintptr_t)list >> 5) & (DLC_BUCKETS - 1);
        DlEntry* e;
        state_h = dl_state_hash();
        /* Keyed on the state as well as the buffer.  The game calls the same
         * list with different state -- a model drawn twice with two different
         * register materials, most obviously -- and an entry that could only
         * hold one of them thrashed: the first M9 measurement had a quarter of
         * all calls missing with "state changed", each one a full re-decode.
         * They are separate entries now and the sweep is what bounds them. */
        for (e = dlc[b]; e; e = e->next) {
            if (e->list == list && e->nbytes == nbytes && e->state_hash == state_h) {
                found = e;
                break;
            }
        }
        if (found) {
            list_h = hash_bytes(list, nbytes, 2166136261u);
            stat_list_bytes += (double)nbytes;
            if (found->list_hash != list_h) {
                miss_reason = 2;
            } else if (found->array_hash !=
                       dlc_array_hash(found->arr_off, found->arr_len)) {
                miss_reason = 3;
            } else {
                hit = found;
            }
        } else {
            miss_reason = 0;
        }
        if (hit) {
            /* The replay.  Everything the miss path does *outside* the decode
             * still happens, including begin_attr_order -- which is what
             * settles the matrices, the texgens and the layout for phase 2,
             * and what counts an off-world draw.  The cached span is one
             * batch, submitted from the cache's own memory (M16). */
            int i;
            hit->last_frame = frame;
            stat_dlc_hit++;
            end_who = "dlcache:hit";
            batch_flush();
            for (i = 0; i < hit->nsegs && i < BATCH_MAX; i++) {
                const DlSeg* g = &hit->segs[i];
                batch[i].first = g->first;
                batch[i].count = g->count;
                batch[i].prim = (u8)(g->op & 0xF8);
            }
            prim = (u8)(hit->segs[0].op & 0xF8);
            vtxfmt = (u8)(hit->segs[0].op & 0x07);
            in_prim = 0;
            nverts = 0;
            begin_attr_order();
            if (nactive != 0 && sl.stride == hit->stride) {
                draw_submit(hit->src, (int)hit->nverts, batch, i, 0);
                stat_batches++;
            }
            nverts = 0;
            port_perf_gx_end();
            dl_replaying = 0;
            return;
        }
        /* A miss that was not a first sight has already been counted by the
         * reason it failed; drop the stale entry so the fresh decode can take
         * its place. */
        if (found) {
            DlEntry** pp = &dlc[b];
            while (*pp) {
                if (*pp == found) {
                    *pp = found->next;
                    stat_dlc_entries--;
                    if (found->dynamic && stat_dlc_dynamic) {
                        stat_dlc_dynamic--;
                    }
                    dlc_free(found);
                    break;
                }
                pp = &(*pp)->next;
            }
            found = NULL;
        }
        switch (miss_reason) {
            case 2: stat_dlc_list++; break;
            case 3: stat_dlc_array++; break;
            default: stat_dlc_new++; break;
        }
        if (!list_h) {
            list_h = hash_bytes(list, nbytes, 2166136261u);
            stat_list_bytes += (double)nbytes;
        }
        {
            int a;
            for (a = 0; a < GX_MAX_ATTR; a++) {
                dl_maxidx[a] = 0xFFFFFFFFu;
                dl_minidx[a] = 0xFFFFFFFFu;
            }
        }
        dl_nsegs = 0;
    }

    /* --drawlog also explains display-list replays: the list's size and, per
     * opcode, the primitive, the vertex count and how many attributes the
     * current descriptor says each vertex carries.  `nactive == 0` is the
     * dangerous one -- the parser then cannot know the stride, so it consumes
     * nothing and the list never advances. */
    if (port_opt.drawlog && dl_shown < port_opt.drawlog &&
        (!port_opt.drawlog_frame ||
         gl13_frame_number() + 1 == (unsigned)port_opt.drawlog_frame)) {
        dl_shown++;
        port_log("---- display list %d: %u bytes at %p ----\n", dl_shown,
                 (unsigned)nbytes, list);
    }
    while (p < end) {
        u8 op = *p++;
        u32 count;
        u32 i;
        int k;
        if (op == 0) {
            continue; /* NOP padding */
        }
        if ((op & 0x80) == 0) {
            gx_warn("GXCallDisplayList: an unexpected opcode in a recorded list");
            caching = 0;
            break;
        }
        if (p + 2 > end) {
            break;
        }
        count = rd_be16(p);
        p += 2;
        prim = (u8)(op & 0xF8);
        vtxfmt = (u8)(op & 0x07);
        nverts = 0;
        sv_first = total;
        in_prim = 1;
        begin_attr_order();
        batch_prepare(count);
        run_pos = ring_claim((size_t)count * (size_t)sl.stride);
        pal_scan_p = p;
        pal_scan_count = count;
        pal_place();
        pal_scan_p = NULL;
        if (nops == 0) {
            list_pos = run_pos;
        } else if (ring_wrapped) {
            caching = 0; /* the list's span is no longer one piece to copy */
        }
        nops++;
        if (port_opt.drawlog && dl_shown < port_opt.drawlog &&
        (!port_opt.drawlog_frame ||
         gl13_frame_number() + 1 == (unsigned)port_opt.drawlog_frame)) {
            port_log("   op %02x prim %02x fmt %d count %u nactive %d, %u bytes left\n",
                     op, prim, vtxfmt, count, nactive, (unsigned)(end - p));
        }
        if (nactive == 0) {
            /* No attribute in the descriptor means no bytes per vertex, so the
             * parser cannot step over this primitive's data and the outer loop
             * would re-read the same opcode forever.  This is always a bug
             * upstream of here -- the game sets the descriptor immediately
             * before every one of its 42 GXCallDisplayList sites -- so name it
             * and abandon the list rather than hang. */
            gx_warn("GXCallDisplayList: the vertex descriptor is empty, so the "
                    "list cannot be stepped through; it is abandoned");
            caching = 0;
            break;
        }
        /* The per-primitive decode plan (see build_decode_plan above): no
         * cursor, no call per attribute, no switch on the attribute id, and
         * the component type resolved once instead of two or three times a
         * vertex.  `plan_ok` is false only for a descriptor carrying an
         * attribute the port does not decode, which it also warns about; the
         * old cursor path is still what GXBegin/GXEnd immediate mode uses. */
        if (plan_ok && !port_opt.olddecode) {
            if (port_opt.decodestats) {
                ds_plan_note(count);
            }
            port_perf_sub_enter(PERF_SUB_DECODE);
            if (!caching && rtdec_build(&rtjob, p, end, count)) {
                /* M29: the run is the render thread's (PLAN.md 44); here only
                 * the list pointer, the vertex count and `pending` advance */
                u32 vb = job_vertex_bytes(&rtjob);
                u32 n = job_vertices(&rtjob, vb);
                rt_decode_record(&rtjob);
                gx_skin_stamp_decode(rt_pos()); /* the skin body's join (PLAN.md 44.1) */
                decode_pending_last(&rtjob, n, vb);
                rt_decode_there(n);
                nverts = (int)n;
                p += (size_t)n * vb;
                stat_rtdec_runs++;
                stat_rtdec_verts += n;
                if (plan_fast) {
                    stat_fast_verts += count;
                }
            } else if (plan_fast && !caching && !port_opt.decodestats) {
                /* M33: under --rtdecode auto this is the game thread's share
                 * (timed for the next frame's plan); under 1/2 a refusal */
                double t0 = rt_auto_on ? port_now_seconds() : 0.0;
                stat_fast_verts += count;
                stat_rtdec_refused += (rt_decode_on() && !rt_auto_on) ? 1 : 0;
                p = plan_fast(p, end, count);
                if (rt_auto_on) {
                    rt_decode_here((unsigned)nverts, port_now_seconds() - t0);
                    stat_rtdec_here++;
                }
            } else {
                double t0 = rt_auto_on ? port_now_seconds() : 0.0;
                stat_rtdec_refused += (rt_decode_on() && !rt_auto_on) ? 1 : 0;
                p = (caching || port_opt.decodestats) ? decode_run_tracked(p, end, count)
                                                      : decode_run(p, end, count);
                if (rt_auto_on) {
                    rt_decode_here((unsigned)nverts, port_now_seconds() - t0);
                    stat_rtdec_here++;
                }
            }
            port_perf_sub_leave();
        } else {
            for (i = 0; i < count && p < end; i++) {
                for (k = 0; k < nactive; k++) {
                    int attr = active[k];
                    u8 desc = gx.vcd[attr];
                    const GXVatFmt* f = &gx.vat[vtxfmt][attr];
                    if (desc == GX_INDEX16) {
                        u32 ix = rd_be16(p);
                        if (caching) {
                            dl_track_index((u8)attr, ix);
                        }
                        indexed(ix);
                        p += 2;
                    } else if (desc == GX_INDEX8) {
                        u32 ix = *p;
                        if (caching) {
                            dl_track_index((u8)attr, ix);
                        }
                        indexed(ix);
                        p += 1;
                    } else { /* GX_DIRECT */
                        int comps, bytes;
                        if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                            bytes = color_bytes(f->type);
                            read_color(p, f->type, pending.clr[attr - GX_VA_CLR0]);
                            p += bytes;
                            attr_written();
                            continue;
                        }
                        comps = attr == GX_VA_POS   ? (f->cnt == GX_POS_XYZ ? 3 : 2)
                                : attr == GX_VA_NRM ? 3
                                                    : (f->cnt == GX_TEX_ST ? 2 : 1);
                        bytes = f->type == GX_U8 || f->type == GX_S8     ? 1
                                : f->type == GX_U16 || f->type == GX_S16 ? 2
                                                                         : 4;
                        if (attr == GX_VA_POS) {
                            pending.pos[0] = read_component(p, f->type, f->frac, 0);
                            pending.pos[1] = read_component(p, f->type, f->frac, 1);
                            pending.pos[2] =
                                comps == 3 ? read_component(p, f->type, f->frac, 2) : 0.0f;
                        } else if (attr == GX_VA_NRM) {
                            u8 nf = nrm_frac(f->type, f->frac);
                            pending.nrm[0] = read_component(p, f->type, nf, 0);
                            pending.nrm[1] = read_component(p, f->type, nf, 1);
                            pending.nrm[2] = read_component(p, f->type, nf, 2);
                            if (f->cnt != GX_NRM_XYZ) {
                                comps = 9; /* M30: NBT, the binormal and tangent stepped over */
                            }
                        } else {
                            int t = attr - GX_VA_TEX0;
                            pending.tex[t][0] = read_component(p, f->type, f->frac, 0);
                            pending.tex[t][1] =
                                comps == 2 ? read_component(p, f->type, f->frac, 1) : 0.0f;
                        }
                        p += (size_t)comps * bytes;
                        attr_written();
                    }
                }
            }
        }
        in_prim = 0;
        if (caching) {
            if (dl_nsegs < (int)(sizeof(dl_segs) / sizeof(dl_segs[0]))) {
                dl_segs[dl_nsegs].op = op;
                dl_segs[dl_nsegs].first = sv_first;
                dl_segs[dl_nsegs].count = (u32)nverts;
                dl_nsegs++;
            } else {
                caching = 0;
            }
        }
        batch_add();
        total = sv_first + (u32)nverts;
        nverts = 0;
    }
    /* The batch outlives the list: the next GX state call flushes it
     * (GX_STATE_TOUCH), and until then the next list's primitives join it --
     * which is every face of a material in hsfdraw.c's FaceDraw, since the
     * game itself skips the material setup when `materialBak` matches.
     * --oldsubmit keeps the pre-M16 scope, one list at most. */
    if (nops && gl13_live()) {
        stat_lists_drawn++;
        stat_list_hist[nops == 1 ? 0 : nops <= 4 ? 1 : nops <= 16 ? 2 : nops <= 64 ? 3 : 4]++;
    }
    if (port_opt.oldsubmit) {
        end_who = "list:end";
        batch_flush();
    }
    if (caching) {
        dlc_store(list, nbytes, list_h, state_h, total, list_pos, frame,
                  miss_reason == 3);
    }
    sv_first = 0;
    port_perf_gx_end();
    dl_replaying = 0;
}

/* ---- the display buffer ----------------------------------------------------- */

void GXCopyDisp(void* dest, GXBool clear) {
    (void)dest;
    GX_FLUSH_NOW(); /* the frame's last batch, before the swap */
    /* The XFB does not exist here: the game draws into GL's back buffer and
     * the swap happens at the retrace gate, so the double-buffer discipline
     * the game expects is preserved (PLAN.md §3.7).  The clear it asks for is
     * the *next* frame's clear and must not touch this one -- on the console
     * the copy to the XFB has already happened by the time the EFB is cleared,
     * and here the "copy" is the swap, which has not happened yet.  So it is
     * queued and run immediately after the swap. */
    if (clear) {
        gl13_clear_at_swap(gx.copy_clear, gx.copy_clear_z);
    }
}

/* ---- the port's own hooks --------------------------------------------------- */

void port_gx_init(void) {
    /* The port's own lookup tables belong to the *process*, not to the game's
     * `GXInit`.  A restored run never calls GXInit -- the game did that in the
     * process that took the snapshot -- so a table that only `gx_draw_reset`
     * filled was all zeroes, `byte_scale[255]` was 0.0, and every lit vertex
     * came out black: the restored board rendered the right scene with the
     * characters as silhouettes (PLAN.md 24.4).  Filling them here costs
     * nothing and removes the whole class. */
    gx_draw_reset();
    gx_tex_init();
    gl13_init();
    gx_logging = port_opt.gxlog;
}

void port_gx_present(void) {
    GX_FLUSH_NOW();
    gl13_present();
}

void gl13_state_report(void);

void port_gx_shutdown(void) {
    gx_draw_report();
    gx_decodestats_report();
    gx_skin_report();
    if (port_opt.vprogstats) {
        gx_vprog_report();
    }
    gl13_state_report();
    gx_tex_report();
    gx_tex_tile_report();
    gx_warn_report();
    gl13_shutdown();
    rt_report(); /* M27: after the stream is drained and the context taken back */
}

void port_gx_frame_number(unsigned n) { (void)n; }
