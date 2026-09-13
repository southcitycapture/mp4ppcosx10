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
    GXTexObjPort* bound[GX_TEX_UNITS];
    GXTlutObjPort tlut[64];

    /* copies */
    u16 disp_src[4], disp_dst[2];
    u16 tex_src[4], tex_dst[2];
    u32 tex_dst_fmt;
} GXState;

extern GXState gx;
extern int gx_ready;

/* gx_draw.c */
void gx_draw_reset(void);
void gx_draw_report(void);

/* gx_tev.c */
void gx_tev_apply(void);          /* GXState -> GL texture environment */
void gx_tev_report(void);

/* gx_tex.c */
void gx_tex_init(void);
void gx_tex_bind(int unit, GXTexObjPort* obj);
void gx_tex_copy(void* dest, int clear);
void gx_tex_report(void);

/* gl13.c -- the only file that talks to GL */
int gl13_init(void);
void gl13_shutdown(void);
void gl13_begin_frame(void);
void gl13_present(void);
void gl13_apply_raster_state(void);
void gl13_apply_transform(void);
void gl13_clear(GXColor c, u32 z);
int gl13_check(const char* fn);   /* --glcheck; returns 0, for the GL() macro */
int gl13_live(void);
void gl13_write_ppm(const char* path);
extern int gl13_have_combine3;
extern int gl13_have_crossbar;
extern int gl13_have_s3tc;
extern int gl13_have_blend_subtract;
extern int gl13_have_depth_texture;
extern int gl13_max_tex_units;

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

#endif
