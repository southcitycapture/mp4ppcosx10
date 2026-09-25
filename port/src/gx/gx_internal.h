/* The GX state machine's shared shape.
 *
 * Three layers, as PLAN.md §3.1 lays them out: `gx_state.c` holds the 128 GX
 * entry points, which do not draw -- they only write into `GXState`;
 * `gx_draw.c` turns a GXBegin..GXEnd (or a recorded display list) into decoded
 * vertices and a draw; `gx_tev.c` compiles the TEV stage chain into an
 * OpenGL 1.3 texture-environment chain; `gx_tex.c` decodes and caches
 * textures; `gl13.c` is the only file that touches GL.
 */
#ifndef PORT_GX_INTERNAL_H
#define PORT_GX_INTERNAL_H

#include "port.h"

#include <dolphin/types.h>
#include <dolphin/gx.h>

#define GX_MAX_ATTR 26 /* GX_VA_MAX_ATTR */
#define GX_TEV_STAGES 16
#define GX_TEX_UNITS 8
#define GX_TEXCOORDS 8

typedef struct GXVatFmt {
    u8 cnt;
    u8 type;
    u8 frac;
} GXVatFmt;

typedef struct GXArraySpec {
    const u8* base;
    u8 stride;
} GXArraySpec;

typedef struct GXTevStage {
    u8 coord, map, chan;      /* GXSetTevOrder                                */
    u8 cin[4], ain[4];        /* GXSetTevColorIn / GXSetTevAlphaIn (a,b,c,d)  */
    u8 cop, cbias, cscale, cclamp, creg;
    u8 aop, abias, ascale, aclamp, areg;
    u8 kcsel, kasel;          /* konst selects                                */
    u8 ras_swap, tex_swap;
    u8 direct;                /* GXSetTevDirect vs an indirect stage          */
} GXTevStage;

/* GXSetIndTexOrder / GXSetIndTexCoordScale: which texture an indirect stage
 * looks up, and by how much its coordinate is divided first. */
typedef struct GXIndStage {
    u8 coord, map, scale_s, scale_t;
} GXIndStage;

/* GXSetTevIndTile, the one indirect form the port reproduces exactly: a small
 * indirect texture whose texels name tiles of a larger one. */
typedef struct GXIndTile {
    u8 on;          /* 0 once GXSetTevDirect or GXSetNumIndStages(0) undoes it */
    u8 ind;         /* which GXIndTexStageID                                   */
    u8 fmt;         /* GXIndTexFormat                                          */
    u16 ts_s, ts_t;   /* tile size, in texels of the tile sheet                */
    u16 tsp_s, tsp_t; /* tile spacing, in texels of the tile sheet             */
} GXIndTile;

/* M35 (PLAN.md 50.13): the warp form, GXSetTevIndWarp -- the direct stage's
 * texel coordinate offset by an indirect texel through a 2x3 matrix.  Built
 * on GL_ATI_text_fragment_shader by gx_tfs.c; recorded here. */
typedef struct GXIndMtx {
    f32 m[2][3];      /* GXSetIndTexMtx's offset matrix                        */
    s8 exp;           /* its scale exponent                                    */
} GXIndMtx;
typedef struct GXIndWarp {
    u8 on;            /* 0 once GXSetTevDirect / GXSetNumIndStages(0) undoes it */
    u8 ind;           /* which GXIndTexStageID                                   */
    u8 mtx;           /* GX_ITM_0..2 (0 = GX_ITM_OFF: no offset)                 */
    u8 sgn;           /* signed offsets (the texel biased by -128)               */
    u8 rep;           /* replace mode: the offset alone, no direct coordinate    */
} GXIndWarp;

typedef struct GXTexGen {
    u8 func, src, mtx, normalize, postmtx;
} GXTexGen;

typedef struct GXChanCtrl {
    u8 enable, amb_src, mat_src, diff_fn, attn_fn;
    u32 light_mask;
    GXColor amb, mat;
} GXChanCtrl;

typedef struct GXLight {
    f32 pos[3], dir[3];
    GXColor color;
    f32 a[3]; /* angle attenuation  */
    f32 k[3]; /* distance attenuation */
    int used;
} GXLight;

/* Our own GXTexObj payload.  GXStruct.h reserves 22 u32 (88 bytes) for it
 * under TARGET_PC, which is the switch this port builds with, so the game's
 * own `GXTexObj` variables are big enough to hold this outright and no side
 * table is needed. */
typedef struct GXTexObjPort {
    u32 magic;
    const void* image;
    u16 width, height;
    u32 format;    /* GXTexFmt, or a GXCITexFmt for the CI variants */
    u8 wrap_s, wrap_t;
    u8 mipmap;
    u8 is_ci;
    u32 tlut_name;
    u8 min_filt, mag_filt;
    f32 min_lod, max_lod, lod_bias;
    u32 gl_name;   /* filled in by the cache at bind time */
    u32 content;   /* content hash of the last upload      */
} GXTexObjPort;

typedef struct GXTlutObjPort {
    u32 magic;
    const void* lut;
    u32 fmt;
    u16 n;
} GXTlutObjPort;

typedef struct GXState {
    /* vertex description */
    u8 vcd[GX_MAX_ATTR];                 /* GXAttrType per attribute        */
    GXVatFmt vat[GX_MAX_VTXFMT][GX_MAX_ATTR];
    GXArraySpec array[GX_MAX_ATTR];

    /* transform */
    f32 pos_mtx[10][12];
    f32 nrm_mtx[10][9];
    f32 tex_mtx[20][12];
    u32 cur_pnmtx;
    f32 proj[7];
    u8 proj_type;
    f32 vp[6];       /* left, top, wd, ht, nearz, farz */
    u32 scissor[4];
    u8 cull;
    u8 line_width;   /* M30: GXSetLineWidth, in 1/6 pixel (PLAN.md 45 cause H) */

    /* channels and lights */
    u8 num_chans;
    GXChanCtrl chan[4];
    GXLight light[8];

    /* texgen */
    u8 num_texgens;
    GXTexGen texgen[GX_TEXCOORDS];

    /* TEV */
    u8 num_tev;
    GXTevStage tev[GX_TEV_STAGES];
    GXColor tev_reg[4];   /* GX_TEVPREV, REG0..2 */
    GXColor kcolor[4];
    u8 swap_tbl[4][4];    /* GXSetTevSwapModeTable */
    u8 num_ind;
    /* The indirect stages the port can actually reproduce: the tile-map case
     * (GXSetTevIndTile).  See gx_tex.c's gx_tex_bind_tiled. */
    GXIndStage ind[4];
    GXIndTile ind_tile[GX_TEV_STAGES];
    GXIndMtx ind_mtx[3];               /* M35: GX_ITM_0..2 */
    GXIndWarp ind_warp[GX_TEV_STAGES]; /* M35: GXSetTevIndWarp per TEV stage */

    /* pixel */
    u8 z_enable, z_func, z_update, z_comploc;
    u8 blend_mode, blend_src, blend_dst, blend_logic;
    u8 alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    u8 color_update, alpha_update;
    u8 fog_type;
    f32 fog_startz, fog_endz, fog_nearz, fog_farz;
    GXColor fog_color;
    GXColor copy_clear;
    u32 copy_clear_z;

    /* textures */
    /* Bound textures are held BY VALUE, not by pointer.  GXLoadTexObj's
     * contract is the hardware's: it loads the object's contents into the
     * texture registers and the caller's GXTexObj is dead the instant it
     * returns.  The game leans on that -- `HuSprTexLoad` in src/game/sprput.c
     * builds its GXTexObj as a *stack local*, loads it, and returns before a
     * single vertex is emitted -- so a port that aliased the caller's object
     * would be reading a dead stack frame at draw time.  See PLAN.md §11.x. */
    GXTexObjPort bound[GX_TEX_UNITS];
    GXTlutObjPort tlut[64];

    /* copies */
    u16 disp_src[4], disp_dst[2];
    u16 tex_src[4], tex_dst[2];
    u32 tex_dst_fmt;
    u8 tex_dst_half; /* GXSetTexCopyDst's mipmap flag: the 2x2 box filter (M23) */
} GXState;

extern GXState gx;
extern const void* gx_last_posmtx_caller;
extern const void* gx_last_posmtx_arg;
extern int gx_ready;
/* incremented by every GXSetArray: the display-list cache memoises its
 * array-contents hashes for the length of one of these, not one frame */
extern unsigned gx_array_epoch;
/* M40 (PLAN.md 55): the vertex cache's memo of an array's hash lasts until
 * the frame ends, a known rewriter runs (the skin body, ShapeProc/
 * ClusterProc, the game's own EnvelopeProc) -- all bump gx_vc_epoch -- or
 * GXSetArray names that same array again
 * (gx_vc_array_set: a buffer refilled between two draws is re-set) */
extern unsigned gx_vc_epoch;
void gx_vc_array_set(const void* base);

/* gx_draw.c */
void gx_draw_reset(void);
void gx_draw_report(void);

/* M16 (PLAN.md 31): a display list's primitives -- and, since the game sends
 * no GX call at all between two faces of one material (hsfdraw.c FaceDraw,
 * `materialBak`), consecutive lists' -- stay batched until the state moves.
 * Every GX state setter starts with GX_STATE_TOUCH(), which submits the
 * pending batch under the state it was decoded under, *before* the setter
 * changes anything.  It is one load and a branch when nothing is pending. */
extern int gx_batch_pending;
extern int gx_palette_active; /* M18: batches carry a matrix palette (gx_draw.c) */
extern int gx_batch_spans;    /* M18/M22: a matrix load or a descriptor setter ends no
                               * batch -- the palette, the pre-transform or the lazy
                               * flush decides per primitive (gx_draw.c) */
int gx_palette_on(void);
void gx_batch_flush_from(const char* who);
/* M22 (PLAN.md 37): the lazy flush.  A state setter no longer submits the
 * pending batch.  The first one after the batch's last primitive *applies*
 * the batch's state to GL (the transform, raster state, TEV, binds and
 * program parameters -- everything a submit does before its draw calls)
 * while the state is still the batch's, and records what the submit read.
 * The next primitive compares the state against that record: the same --
 * hsfdraw.c's material setup passes through states no primitive is drawn
 * under and comes back, for every object -- and the batch goes on; changed,
 * and the batch's draws are issued now, under the GL state that is still
 * its own, before anything else reaches GL.  No copy of the state is ever
 * made.  --eagerflush is the pre-M22 shape: the setter submits at once. */
void gx_batch_touch(const char* who);
/* M44 (PLAN.md 59): bumped by every GX state setter (the three touch macros
 * below and gx_state.c's GX_STATE_TOUCH_DECODE; GXInit and a restore reset
 * the memos that read it).  A primitive whose setter-free predecessor had the
 * same generation sees the same state: begin_attr_order keeps its layout. */
extern unsigned gx_state_gen;
#define GX_STATE_TOUCH()                                                                 \
    do {                                                                                 \
        gx_state_gen++;                                                                  \
        if (gx_batch_pending) {                                                          \
            gx_batch_touch(__func__);                                                    \
        }                                                                                \
    } while (0)
/* for a copy or the present: the batch must be *drawn* now, lazily or not */
#define GX_FLUSH_NOW()                                                                   \
    do {                                                                                 \
        if (gx_batch_pending) {                                                          \
            gx_batch_flush_from(__func__);                                               \
        }                                                                                \
    } while (0)
/* The same, for a setter that has compared first: a value the state already
 * holds changes nothing, so it ends no batch.  hsfdraw.c's FaceDraw calls
 * GXSetBlendMode before *every* face, almost always with the value it set
 * for the previous one (55,678 of 68,389 batch ends on the board, M16). */
/* `group` is a bit of --cmpmask (default all set): a group whose bit is clear
 * flushes unconditionally, which is how a wrong compare is bisected. */
#define GX_STATE_TOUCH_IF(group, changed)                                                \
    do {                                                                                 \
        gx_state_gen++;                                                                  \
        if (gx_batch_pending && (!(port_opt.cmpmask & (group)) || (changed))) {          \
            gx_batch_touch(__func__);                                                    \
        }                                                                                \
    } while (0)
#define GX_CMP_RASTER 1
#define GX_CMP_MATRIX 2
#define GX_CMP_TEV 4
#define GX_CMP_CHAN 8
/* M37 (PLAN.md 52): the two classes of batch end the character select's
 * state did not need.  GX_CMP_M37 is the setters that were still flushing
 * unconditionally -- the texgens, GXSetTevOp and the TEV register/swap
 * table, the indirect setup, the fog, the colour/alpha update and the
 * light loads -- each of which hsfdraw.c's material setup and the sprite
 * path re-send with the value the state already holds.  GX_CMP_DESC is
 * the vertex descriptor, the attribute formats and the arrays: read by
 * the *decode* of the next primitive and by nothing in a pending batch,
 * whose layout batch_prepare compares anyway.  Clearing either bit of
 * --cmpmask is the A/B. */
#define GX_CMP_M37 256
#define GX_CMP_DESC 512
/* M37: a matrix load into a slot nothing in the pending batch draws with
 * (only gx.cur_pnmtx is read, gx_draw.c:795) ends no batch. */
#define GX_CMP_MSLOT 1024
/* M37: a texture loaded into a unit no TEV or indirect stage in use reads
 * ends no batch -- the same argument as M22's GXLoadTexObj compare, one
 * step further. */
#define GX_CMP_TEXU 2048
/* M37: an immediate-mode primitive (GXBegin/GXEnd, the sprite path) no
 * longer gets a batch of its own -- it joins the pending batch exactly as
 * a display list's primitive does, and batch_prepare's compare decides.
 * 115 of the character select's 249 batches are immediate primitives. */
#define GX_CMP_IMM 4096

/* gx_tev.c */
void gx_tev_apply(void);          /* GXState -> GL texture environment */
void gx_tev_report(void);
void gx_tev_cache_invalidate(void); /* M13: drop the TEV state cache (GL reset) */

/* gx_tex.c */
void gx_tex_init(void);
void gx_tex_bind(int unit, GXTexObjPort* obj);
/* ...through a GXSetTevSwapModeTable entry, packed two bits per output
 * channel: the cache holds a separately re-encoded copy per (texture, swap). */
void gx_tex_bind_swapped(int unit, GXTexObjPort* obj, u8 swap);
int gx_tex_bind_tiled(int unit, GXTexObjPort* sheet, GXTexObjPort* map,
                      const GXIndTile* tile);
GXTexObjPort* gx_bound_tex(unsigned id);
extern u8 gx_unit_alpha_min[8]; /* M33: the bound texture's smallest alpha, per unit */
const GXTexObjPort* gx_bound_tex_of(const GXState* st, unsigned id); /* M22 */ /* NULL unless the unit holds a real object */
void gx_tex_copy(void* dest, int clear);
void gx_tex_report(void);
void gx_tex_tile_report(void);
/* --texvalidate-every-bind: keep per-bind content validation (still using
 * the sampled hash) but bypass the per-frame epoch, for bisecting a
 * suspected staleness regression between "the epoch" and "the sampling". */
void gx_tex_set_validate_every_bind(int v);

/* gl13.c -- the only file that talks to GL */
int gl13_init(void);
void gl13_shutdown(void);
void gl13_begin_frame(void);
void gl13_present(void);
void gl13_apply_raster_state(void);
void gl13_apply_transform(void);
void gl13_clear(GXColor c, u32 z);
void gl13_clear_at_swap(GXColor c, u32 z); /* run it after the swap, not before */
int gl13_check(const char* fn);   /* --glcheck; returns 0, for the GL() macro */

/* The shadow of the GL state (M3 §1).  Everything that sets GL state goes
 * through these; they emit only the difference and count what they elided.
 * `glc_invalidate` forgets the lot, and must be called after anything that
 * changes GL behind the cache's back. */
void glc_invalidate(void);
void glc_forget_white(void); /* M43: gl13_shutdown (gl13_state.c) */
/* M43 (PLAN.md 58.2): --rtgx, src/gx/gx_rtgx.c */
struct GxXfDesc;
enum { RTGX_F_INVALIDATE = 1, RTGX_F_ACTIVE, RTGX_F_CLIENT_ACTIVE, RTGX_F_BIND, RTGX_F_NOTE_BIND,
       RTGX_F_ENABLE2D, RTGX_F_TEXMTX, RTGX_F_TEXMTX_FOLD, RTGX_F_VP_DISABLE, RTGX_F_VP_FORGET };
extern int rtgx_owner_rt;
void rtgx_fwd(int op, int a, int b, float x, float y, float z) __attribute__((cold, noinline));
void rtgx_unexpected(const char* who) __attribute__((cold, noinline));
extern int rtgx_frame_on; /* M43: this drawn frame's batches are the render thread's */
int gx_rtgx_batch(void);
void gx_rtgx_release(void);
int gx_rtgx_frame_on(void);
void gx_rtgx_frame_begin(void);
void gx_rtgx_record(const struct GxXfDesc* xfd, int on_gpu, int csum, const u8* ob, int out_stride,
                    int out_off_clr, int out_off_tex, int out_ntex);
int gx_rtgx_zp_maybe(void);
void gx_rtgx_zp_begin(void);
void gx_rtgx_zp_end(void);
void gx_rtgx_rt_time(double s);
void gx_rtgx_rt_present(void);
void gx_rtgx_game_time(double s);
void gx_rtgx_report(void);
void gx_tev_bind_textures(void);
void gx_vprog_forget_vbo(void);
void gx_vprog_forget_bound(void);
const GXTexObjPort* gx_bound_tex_of(const GXState* st, unsigned id);
void gl13_downsample_read(unsigned name, float su, float sv, int x, int y, int w, int h,
                          unsigned char* out_rgba); /* M23 */
void glc_stats(unsigned* emitted, unsigned* elided);
void glc_active_texture(int unit);
void glc_client_active_texture(int unit);
void glc_bind_texture(int unit, unsigned name);
unsigned glc_white_texture(void);
void glc_note_bind(int unit, unsigned name);
void glc_unit_enable_tex2d(int unit, int on);
void glc_texenvi(int unit, unsigned pname, int v);
void glc_texenvf(int unit, unsigned pname, float v);
void glc_texenv_color(int unit, const float* rgba);
void glc_tex_matrix(int unit, float su, float sv);
void glc_tex_matrix_fold(int unit, float su, float sv, float tv); /* M24b: t*sv + tv */
void glc_projection(const float* m16);
void glc_modelview_identity(void);
void glc_vertex_array(const void* p, int stride);
void glc_color_array(const void* p, int stride);
void glc_coord_array(int unit, const void* p, int stride); /* NULL turns it off */
void glc_normal_array(const void* p, int stride);          /* NULL turns it off */
void glc_fogcoord_array(const void* p, int stride);        /* M18: the palette slot */
int glc_fogcoord_available(void);
void glc_get_tex_scale(int unit, float* su, float* sv);
void glc_get_tex_fold(int unit, float* su, float* sv, float* tv);
/* M16: the vertex ring (PLAN.md 31).  gl13_var_setup hands back the ring --
 * DMA-visible when GL_APPLE_vertex_array_range is on, plain memory otherwise
 * -- and the three calls below are the fence discipline gx_draw.c follows:
 * enter before writing a span, flush before drawing it, left after the draws
 * so the chunks the writer has finished with get their fences. */
u8* gl13_var_setup(size_t bytes, size_t ring_bytes);
unsigned gl13_vc_vbo(void);     /* M40: the cache's buffer object, 0 for none */
void glc_arrays_forget(void);   /* M40 */
int gl13_var_active(void);
int gl13_have_multidraw(void);
void gl13_var_enter(size_t off, size_t len);
void gl13_var_flush(const void* p, size_t len);
void gl13_var_left(size_t from, size_t cursor, int wrapped);
void gl13_var_stats(unsigned* waits, unsigned* blocked, unsigned* sets, unsigned* flushes);
void gl13_multi_draw_arrays(unsigned mode, const int* first, const int* count, int n);
void gl13_draw_range_elements(unsigned mode, unsigned lo, unsigned hi, int n, int wide,
                              const void* idx); /* M21 */
int gl13_trace_armed(void);       /* --gltrace F: this is frame F */
int gl13_live(void);
/* --nodraw / --ffto (PLAN.md 24.1): the renderer is switched off under a live
 * context.  `gl13_live()` is 0 while it is, so every GL path already skips;
 * these two are for the paths that would decode something first. */
int gl13_draw_off(void);
void gl13_set_draw_off(int v);
int gl13_have_context(void); /* a window and a context, drawn into or not */
int gl13_fullscreen(void);   /* M25: the letterboxed present is on */
int port_framemode_active(void); /* src/platform/framemode.c */
/* Drop every decoded texture and every composed indirect tile, deleting their
 * GL names if there is a context.  Called when drawing comes back on after a
 * --nodraw stretch (the cache filled with entries that never got a GL name)
 * and after a --restore (the cache belongs to the process, the textures belong
 * to the snapshot's MEM1). */
void gx_tex_flush_all(void);
void gl13_write_ppm(const char* path);
void gl13_write_png(const char* path); /* M32: the F12 screenshot */
extern int gl13_have_combine3;
extern int gl13_have_crossbar;
extern int gl13_have_s3tc;
extern int gl13_have_blend_subtract;
extern int gl13_have_depth_texture;
extern int gl13_max_tex_units;

/* gx_vprog.c -- phase 2 as an ARB vertex program (PLAN.md 25).
 *
 * What a draw has to tell the generator.  It is exactly the subset of
 * gx_draw.c's per-primitive invariants (`PrimInv`) and source layout
 * (`Layout`) that the program text or the program parameters depend on;
 * `draw_run` fills one of these instead of exporting its own statics. */
typedef struct GxXfDesc {
    const f32* pos_mtx;      /* 3x4, row major                               */
    const f32* nrm_mtx;      /* 3x3, row major                               */
    int have_nrm;            /* the source layout carries a normal           */
    int chan_mode;           /* 0/1: the source colour is already final      */
    int ntexgen;
    /* the source layout, which becomes the vertex arrays */
    const u8* base;
    int vbo;                 /* M40: base is an offset in the cache's buffer object */
    int stride, off_nrm, off_clr, off_tex, ntex;
    struct {
        u8 src_kind;         /* 0 texcoord, 1 position, 2 normal             */
        u8 src_k;
        u8 divide;
        const f32* mtx;      /* NULL for the identity                        */
    } tg[GX_TEXCOORDS];
    /* M18: the batch's matrix palette (gx_draw.c pal[]); pal_n == 0 means the
     * pre-M18 shape, pos_mtx/nrm_mtx for the whole batch */
    const f32 (*pal)[4];
    int pal_n;               /* slots addressable (the palette's size)       */
    int pal_dirty_lo, pal_dirty_hi; /* rows written since the last upload    */
    int off_skin;            /* the slot float in the source layout          */
    int hilite;              /* M21: fold the specular channel (gx_draw.c)   */
} GxXfDesc;

/* M21 (PLAN.md 36): hsfdraw.c's hilite materials light a second colour
 * channel (GX_COLOR1, GX_DF_NONE + GX_AF_SPEC: the specular term) and add
 * it in a TEV stage of the shape lerp(CPREV, ONE, RASC[COLOR1A1]).  GL 1.3's
 * combiner has one per-vertex colour, so the stage is folded: the vertex
 * program writes primary = c0 * (1 - spec) and secondary = spec, the stage
 * passes PREV through, and GL_COLOR_SUM adds the secondary colour after the
 * units.  `gx_hilite_stage` is that stage's index for the primitive being
 * drawn, or -1. */
extern int gx_hilite_stage;
/* M22 (PLAN.md 37): 1 = the screen above, folded through the colour sum;
 * 2 = the *textured* highlight (hsfdraw.c:1374, TEXC * RASC1 + CPREV, the
 * results portraits): the program writes spec into the primary colour's
 * alpha -- which the matcher has checked no stage reads as RASA -- and the
 * stage is emitted as MODULATE_ADD_ATI(TEXTURE, PREVIOUS, PRIMARY.a).  A
 * coloured specular loses its tint that way (the alpha is its luminance);
 * the game's lights are white. */
extern int gx_hilite_mode;
u32 gx_tev_last_sig(void); /* M21: --submitstats' mergeable-batch count */
void gx_tev_stage_konst(const GXTevStage* s, int alpha, float* out); /* M22: --drawlog */
int gx_hilite_decide(void);
void glc_color_sum(int on);

void gx_vprog_probe(void);            /* needs a live GL context */
int gx_tev_unit_stage(int u); /* M30: the stage a GL unit samples for (gx_tev.c) */
void gx_unit_memo(int on);    /* M41: the unit layout and register shape decided once a draw */
/* M35: what GL unit `u` samples this draw -- the texgen slot and the texmap --
 * or 0 when it samples nothing.  The stage's own (gx_tev_unit_stage) on the
 * fixed-function path; on the fragment-shader path (gx_tfs.c) the units past
 * the stages carry the indirect maps.  Both vertex paths bind by this. */
int gx_tev_unit_source(int u, u8* coord, u8* map);

/* gx_tfs.c -- M35 (PLAN.md 50.13): the indirect warp on GL_ATI_text_fragment_shader */
extern int gl13_have_tfs;                /* the extension is on the card's list */
int gx_tfs_layout(int u, u8* coord, u8* map); /* 1: this draw is the shader's, and unit u's source; -1: not the shader's draw */
int gx_tfs_apply(int stages, int emit);  /* 1: the draw's TEV is bound as a fragment program */
void gx_tfs_off(void);                   /* the fixed-function draw that follows: shader off */
void gx_tfs_invalidate(void);            /* the GL context's state is unknown again */
void gx_tfs_probe(void);                 /* --tfsprobe: compile, draw, read back, print */
void gx_tfs_report(void);
int gx_vprog_available(void);
int gx_vprog_native_instr_limit(void);
/* 1 = this draw's vertices are on the GPU; the caller must skip phase 2 and
 * must NOT bind the CPU output arrays.  0 = fall back, and it has been
 * counted. */
int gx_vprog_draw(const GxXfDesc* d, int nverts);
/* the second half, after gl13_apply_transform / _raster_state / gx_tev_apply:
 * the parameters and the arrays, two of which read state those three set */
void gx_vprog_bind(const GxXfDesc* d);
void gx_vprog_disable(void);          /* back to fixed function for one draw  */
void gx_vprog_invalidate(void);       /* glc_invalidate's counterpart         */
void gx_vprog_report(void);           /* --vprogstats                         */
void gx_vprog_frame_reset(void);      /* per-frame fallback counters          */

/* ---- M29: the display-list decode as a record (PLAN.md 44) ----------------
 * The per-primitive decode plan (gx_draw.c build_decode_plan) and the job
 * the game thread hands the render thread: everything the eight specialised
 * loops and the general walker read, by value -- the arrays' bases and
 * strides, the byte tables (append-only statics), the destination in the
 * ring.  The render thread runs gx_decode_job on it and writes nothing but
 * the ring bytes. */
enum {
    DEC_NONE = 0,
    /* <type>_<components read>_<components written>; the written-but-unread
     * component is the zero GX pads a 2-component position or a 1-component
     * texcoord with. */
    DEC_F32_2_3, DEC_F32_3_3, DEC_F32_1_2, DEC_F32_2_2,
    DEC_S16_2_3, DEC_S16_3_3, DEC_S16_1_2, DEC_S16_2_2,
    DEC_U16_2_3, DEC_U16_3_3, DEC_U16_1_2, DEC_U16_2_2,
    DEC_S8_2_3,  DEC_S8_3_3,  DEC_S8_1_2,  DEC_S8_2_2,
    DEC_U8_2_3,  DEC_U8_3_3,  DEC_U8_1_2,  DEC_U8_2_2,
    DEC_CLR_RGBA8, DEC_CLR_RGBX8, DEC_CLR_RGB8,
    DEC_CLR_RGB565, DEC_CLR_RGBA4, DEC_CLR_RGBA6,
    /* the table forms of the four 8-bit ops (byte_table) */
    DEC_TS8_2_3, DEC_TS8_3_3, DEC_TS8_1_2, DEC_TS8_2_2,
    DEC_TU8_2_3, DEC_TU8_3_3, DEC_TU8_1_2, DEC_TU8_2_2
};

typedef struct DecStep {
    const u8* base;   /* indexed: the array; direct: NULL                     */
    f32 scale;        /* the VAT's fractional scale, folded in once           */
    const f32* tbl;   /* S8/U8: (f32)(s8)b * scale for every byte (M17)       */
    u16 dstoff;       /* byte offset into the vertex, or into `pending`       */
    u8 stride;        /* indexed: the array's stride                          */
    u8 idx;           /* 0 direct, 1 GX_INDEX8, 2 GX_INDEX16                  */
    u8 advance;       /* direct: bytes of payload this step eats              */
    u8 op;            /* DEC_*                                                */
    u8 to_pending;    /* destination is the staging vertex, not the packed one */
    u8 attr;          /* only for the display-list cache's index range        */
} DecStep;

#define GX_DEC_FILL_MAX 8
typedef struct GxDecJob {
    const u8* p;          /* the list bytes of the run (the game's memory)    */
    const u8* end;
    u32 count;            /* the primitive's vertex count                   */
    u8* dst;              /* the ring: src_buf + run_pos                    */
    u32 stride;
    int off_nrm, off_clr, off_tex;
    int clr_const;        /* splat `clr` into off_clr per vertex            */
    u32 clr;
    int prefetch;         /* !--noprefetch                                  */
    int fast;             /* which specialised loop (gx_draw.c), -1 general */
    int nplan;
    int nfill;
    /* M44 (--novcpos): fast == -2, a positions refresh -- the run's other
     * attributes copied from the vertex cache's stored run (`seed`), the
     * positions (F32 xyz, indexed; their index `pos_off` bytes into each of
     * the list's `pos_vb`-byte vertices) decoded again */
    const u8* seed;
    u32 pos_off, pos_vb;
    /* M44: the plan before the fill, so a job with no fill (nearly all) travels
     * as the header and the steps in use alone (rt_decode_record) */
    DecStep plan[GX_MAX_ATTR];
    struct { u16 dstoff; f32 s, t; } fill[GX_DEC_FILL_MAX];
} GxDecJob;
/* the render thread (rt.c): decode the job's run into the ring; returns the
 * vertices written */
u32 gx_decode_job(const GxDecJob* j);
/* the game thread (rt.c): append the run to the stream */
void rt_decode_record(const GxDecJob* j);

/* M44 (PLAN.md 59): the water -- the indirect warp evaluated at the vertices
 * (gx_water.c) */
enum { GX_WATER_OFF = 0, GX_WATER_CHEAP = 1, GX_WATER_FULL = 2 };
typedef struct GxWaterInd {
    u8 coord;             /* the indirect stage's texture coordinate (0xFF: unused) */
    const u8* rgba;       /* its map, decoded on the CPU */
    int tw, th;
    u8 wrap_s, wrap_t, linear;
    float div_s, div_t;   /* GXSetIndTexCoordScale */
    u8 same_as;           /* an earlier stage with the same map and coordinate, or 0xFF */
} GxWaterInd;
typedef struct GxWaterWarp {
    u8 coord;             /* the warped stage's texture coordinate */
    u8 ind;               /* its indirect stage */
    float f[2][3];        /* the matrix times 2^exp, over the direct map's size */
} GxWaterWarp;
typedef struct GxWaterPlan {
    int nwarp;
    GxWaterWarp w[GX_TEV_STAGES];
    GxWaterInd ind[4];
} GxWaterPlan;
int gx_water_level(void);
int gx_water_plan(GxWaterPlan* p);
void gx_water_offsets(const GxWaterPlan* p, u8* out, int n, int stride, int off_tex);
void gx_water_report(void);

/* one place for "the backend could not do this exactly", counted and named
 * once each by --gxwarn */
void gx_warn(const char* what);
void gx_warn_report(void);

/* --gxlog */
extern int gx_logging;
void gx_log_call(const char* name, const char* fmt, ...);
#define GXLOG(name, ...)                                                                 \
    do {                                                                                 \
        if (gx_logging) {                                                                \
            gx_log_call(name, __VA_ARGS__);                                              \
        }                                                                                \
    } while (0)

/* M31: --forceobj's flags for the draw being applied (gx_draw.c); 0 normally */
extern int gx_force_flags;
const char* port_drawobj_name(const void* mtx, int* model_index);

#endif
