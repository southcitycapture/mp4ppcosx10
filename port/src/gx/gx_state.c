/* The GX entry points that only record state.
 *
 * Nothing here draws.  Every one of these writes into `gx` and returns, and
 * `gx_draw.c` resolves the whole struct into a GL configuration once per
 * GXBegin..GXEnd.  That is the shape the N64 ports' `gfx_pc.c` has and the
 * reason the 5,679 GX call sites in this game cost almost nothing: the
 * overwhelming majority of them are stores.
 *
 * The GX->GL translations that are one-liners live here (viewport, scissor,
 * cull, z, blend, alpha test, fog, matrices); the two that are not -- the TEV
 * chain and texture decoding -- are gx_tev.c and gx_tex.c.
 */
#include "gx_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

GXState gx;
int gx_ready;
int gx_logging;

/* ---- diagnostics ---------------------------------------------------------- */

void gx_log_call(const char* name, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    port_log("gx> %s(%s)\n", name, buf);
}

typedef struct Warn {
    const char* what;
    unsigned count;
} Warn;

static Warn warns[64];
static int warn_count;

void gx_warn(const char* what) {
    int i;
    for (i = 0; i < warn_count; i++) {
        if (warns[i].what == what || strcmp(warns[i].what, what) == 0) {
            warns[i].count++;
            return;
        }
    }
    if (warn_count == 64) {
        return;
    }
    warns[warn_count].what = what;
    warns[warn_count].count = 1;
    warn_count++;
    if (port_opt.gxwarn) {
        port_log("gxwarn> %s\n", what);
    }
}

void gx_warn_report(void) {
    int i;
    if (!warn_count) {
        return;
    }
    port_log("\n---- GX features the backend degraded (%d distinct) ----\n", warn_count);
    for (i = 0; i < warn_count; i++) {
        port_log("  %-58s %8u\n", warns[i].what, warns[i].count);
    }
}

/* ---- init ----------------------------------------------------------------- */

static GXFifoObj fifo_obj;

GXFifoObj* GXInit(void* base, u32 size) {
    int i, j;
    memset(&gx, 0, sizeof(gx));
    for (i = 0; i < GX_MAX_ATTR; i++) {
        gx.vcd[i] = GX_NONE;
        for (j = 0; j < GX_MAX_VTXFMT; j++) {
            gx.vat[j][i].cnt = 1;
            gx.vat[j][i].type = GX_F32;
            gx.vat[j][i].frac = 0;
        }
    }
    for (i = 0; i < 10; i++) {
        gx.pos_mtx[i][0] = gx.pos_mtx[i][5] = gx.pos_mtx[i][10] = 1.0f;
        gx.nrm_mtx[i][0] = gx.nrm_mtx[i][4] = gx.nrm_mtx[i][8] = 1.0f;
    }
    for (i = 0; i < 20; i++) {
        gx.tex_mtx[i][0] = gx.tex_mtx[i][5] = gx.tex_mtx[i][10] = 1.0f;
    }
    for (i = 0; i < 4; i++) {
        gx.swap_tbl[i][0] = GX_CH_RED;
        gx.swap_tbl[i][1] = GX_CH_GREEN;
        gx.swap_tbl[i][2] = GX_CH_BLUE;
        gx.swap_tbl[i][3] = GX_CH_ALPHA;
    }
    gx.num_tev = 1;
    gx.num_chans = 0;
    gx.z_enable = 1;
    gx.z_func = GX_LEQUAL;
    gx.z_update = 1;
    gx.color_update = 1;
    gx.alpha_update = 1;
    gx.blend_mode = GX_BM_NONE;
    gx.blend_src = GX_BL_ONE;
    gx.blend_dst = GX_BL_ZERO;
    gx.alpha_comp0 = GX_ALWAYS;
    gx.alpha_comp1 = GX_ALWAYS;
    gx.alpha_op = GX_AOP_AND;
    gx.vp[2] = 640.0f;
    gx.vp[3] = 480.0f;
    gx.vp[5] = 1.0f;
    gx.scissor[2] = 640;
    gx.scissor[3] = 480;
    gx_draw_reset();
    gx_ready = 1;
    port_log("port> GXInit: FIFO %p, %u bytes (the port keeps no FIFO; the "
             "write-gather writers are ordinary calls under TARGET_PC)\n",
             base, size);
    return &fifo_obj;
}

/* ---- vertex description --------------------------------------------------- */

void GXClearVtxDesc(void) {
    GX_STATE_TOUCH();
    int i;
    for (i = 0; i < GX_MAX_ATTR; i++) {
        gx.vcd[i] = GX_NONE;
    }
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    GX_STATE_TOUCH();
    if ((unsigned)attr < GX_MAX_ATTR) {
        gx.vcd[attr] = (u8)type;
    }
}

void GXSetVtxDescv(GXVtxDescList* list) {
    GX_STATE_TOUCH();
    for (; list && list->attr != GX_VA_NULL; list++) {
        GXSetVtxDesc(list->attr, list->type);
    }
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type,
                     u8 frac) {
    GX_STATE_TOUCH();
    if ((unsigned)vtxfmt < GX_MAX_VTXFMT && (unsigned)attr < GX_MAX_ATTR) {
        gx.vat[vtxfmt][attr].cnt = (u8)cnt;
        gx.vat[vtxfmt][attr].type = (u8)type;
        gx.vat[vtxfmt][attr].frac = frac;
    }
}

/* Bumped by every GXSetArray, and used by the display-list cache in
 * gx_draw.c as the lifetime of its array-contents memo.
 *
 * The frame was the wrong lifetime and the frame md5s said so.  The game
 * points GX at an object's arrays and then calls that object's display lists,
 * so within one such epoch the same array is read by several lists and hashing
 * it once is exactly right -- but *across* a frame the CPU animates geometry
 * (ClusterExec morphs, EnvelopeExec skins) and a model rewritten and drawn a
 * second time in the same frame would have been validated against the hash
 * taken before the rewrite.  That is one character on the character select
 * drawing a frame late, which is what the M9 run that hashed per frame
 * produced. */
unsigned gx_array_epoch;

void GXSetArray(GXAttr attr, const void* data, u8 stride) {
    GX_STATE_TOUCH();
    if ((unsigned)attr < GX_MAX_ATTR) {
        gx.array[attr].base = (const u8*)data;
        gx.array[attr].stride = stride;
        gx_array_epoch++;
    }
}

/* ---- matrices and the viewport -------------------------------------------- */

/* Who loaded the matrix.
 *
 * "The stage geometry is transformed clean off the side of the world" is a
 * question about *which* of the game's matrix builders produced it, and the
 * port has one fact the game does not: the return address.  Every REL and the
 * executable are Mach-O images, so `dladdr` names the caller -- the same trick
 * the fault handler's hand-walked backtrace uses.  Captured only under
 * --drawlog, and only as a pointer; the symbol lookup happens when a draw is
 * actually explained. */
const void* gx_last_posmtx_caller;
const void* gx_last_posmtx_arg;

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    u32 slot = id / 3;
    GX_STATE_TOUCH_IF(slot >= 10 || memcmp(gx.pos_mtx[slot], mtx, 48) != 0);
    if (port_opt.drawlog) {
        gx_last_posmtx_caller = __builtin_return_address(0);
        gx_last_posmtx_arg = mtx;
    }
    if (slot < 10) {
        memcpy(gx.pos_mtx[slot], mtx, 48);
    }
}

void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    /* the SDK takes a 3x3 written as the top-left of a 3x4 */
    u32 slot = id / 3;
    if (slot < 10) {
        const f32* m = (const f32*)mtx;
        int r;
        /* bit-exact, like GXLoadPosMtxImm's memcmp: a state call that
         * changes nothing ends no batch, and "nothing" means the bytes */
        GX_STATE_TOUCH_IF(memcmp(&gx.nrm_mtx[slot][0], m, 12) != 0 ||
                          memcmp(&gx.nrm_mtx[slot][3], m + 4, 12) != 0 ||
                          memcmp(&gx.nrm_mtx[slot][6], m + 8, 12) != 0);
        for (r = 0; r < 3; r++) {
            gx.nrm_mtx[slot][r * 3 + 0] = m[r * 4 + 0];
            gx.nrm_mtx[slot][r * 3 + 1] = m[r * 4 + 1];
            gx.nrm_mtx[slot][r * 3 + 2] = m[r * 4 + 2];
        }
    }
}

void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    u32 slot;
    if (id >= GX_PTTEXMTX0) {
        slot = (id - GX_PTTEXMTX0) / 3 + 10;
    } else {
        slot = (id - GX_TEXMTX0) / 3;
    }
    if (slot < 20) {
        GX_STATE_TOUCH_IF(memcmp(gx.tex_mtx[slot], mtx, type == GX_MTX2x4 ? 32 : 48) != 0);
        memcpy(gx.tex_mtx[slot], mtx, type == GX_MTX2x4 ? 32 : 48);
        if (type == GX_MTX2x4) {
            gx.tex_mtx[slot][8] = 0.0f;
            gx.tex_mtx[slot][9] = 0.0f;
            gx.tex_mtx[slot][10] = 1.0f;
            gx.tex_mtx[slot][11] = 0.0f;
        }
    }
}

void GXSetCurrentMtx(u32 id) { GX_STATE_TOUCH_IF(gx.cur_pnmtx != id / 3); gx.cur_pnmtx = id / 3; }

/* GX does not keep the 4x4 it is handed: it keeps six of its elements, which
 * is exactly what the fixed-function projection has degrees of freedom for.
 * The decomp's own src/dolphin/gx/GXTransform.c is the specification. */
void GXSetProjection(const void* mtxp, GXProjectionType type) {
    const f32(*mtx)[4] = (const f32(*)[4])mtxp;
    f32 p[6];
    p[0] = mtx[0][0];
    p[2] = mtx[1][1];
    p[4] = mtx[2][2];
    p[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        p[1] = mtx[0][3];
        p[3] = mtx[1][3];
    } else {
        p[1] = mtx[0][2];
        p[3] = mtx[1][2];
    }
    GX_STATE_TOUCH_IF(gx.proj_type != (u8)type || memcmp(gx.proj, p, sizeof(p)) != 0);
    gx.proj_type = (u8)type;
    memcpy(gx.proj, p, sizeof(p));
}

void GXGetProjectionv(f32* p) {
    p[0] = (f32)gx.proj_type;
    memcpy(p + 1, gx.proj, 6 * sizeof(f32));
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    f32 v[6];
    v[0] = left;
    v[1] = top;
    v[2] = wd;
    v[3] = ht;
    v[4] = nearz;
    v[5] = farz;
    GX_STATE_TOUCH_IF(memcmp(gx.vp, v, sizeof(v)) != 0);
    gx.vp[0] = left;
    gx.vp[1] = top;
    gx.vp[2] = wd;
    gx.vp[3] = ht;
    gx.vp[4] = nearz;
    gx.vp[5] = farz;
}

/* Only used when field_rendering is on, which GXNtsc480IntDf does not ask
 * for; the jitter is half a scanline and a progressive window has nowhere to
 * put it. */
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz,
                         u32 field) {
    GX_STATE_TOUCH();
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* v) { memcpy(v, gx.vp, sizeof(gx.vp)); }

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    GX_STATE_TOUCH_IF(gx.scissor[0] != left || gx.scissor[1] != top || gx.scissor[2] != wd ||
                      gx.scissor[3] != ht);
    gx.scissor[0] = left;
    gx.scissor[1] = top;
    gx.scissor[2] = wd;
    gx.scissor[3] = ht;
}

void GXSetScissorBoxOffset(s32 x, s32 y) {
    GX_STATE_TOUCH();
    (void)x;
    (void)y;
}

void GXSetCullMode(GXCullMode mode) { GX_STATE_TOUCH_IF(gx.cull != (u8)mode); gx.cull = (u8)mode; }

/* ---- channels, lights ----------------------------------------------------- */

void GXSetNumChans(u8 n) { GX_STATE_TOUCH_IF(gx.num_chans != n); gx.num_chans = n; }

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src,
                   GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn,
                   GXAttnFn attn_fn) {
    int i, n = 1, first = (int)chan;
    int changed = 0;
    if (chan == GX_COLOR0A0) {
        first = GX_COLOR0;
        n = 3; /* COLOR0 and ALPHA0 */
    } else if (chan == GX_COLOR1A1) {
        first = GX_COLOR1;
        n = 3;
    }
    for (i = 0; i < n; i += 2) {
        int c = first + i;
        if (c < 0 || c > 3) {
            continue;
        }
        if (gx.chan[c].enable != (u8)(enable ? 1 : 0) || gx.chan[c].amb_src != (u8)amb_src ||
            gx.chan[c].mat_src != (u8)mat_src || gx.chan[c].light_mask != light_mask ||
            gx.chan[c].diff_fn != (u8)diff_fn || gx.chan[c].attn_fn != (u8)attn_fn) {
            changed = 1;
        }
    }
    GX_STATE_TOUCH_IF(changed);
    for (i = 0; i < n; i += 2) {
        int c = first + i;
        if (c < 0 || c > 3) {
            continue;
        }
        gx.chan[c].enable = (u8)(enable ? 1 : 0);
        gx.chan[c].amb_src = (u8)amb_src;
        gx.chan[c].mat_src = (u8)mat_src;
        gx.chan[c].light_mask = light_mask;
        gx.chan[c].diff_fn = (u8)diff_fn;
        gx.chan[c].attn_fn = (u8)attn_fn;
    }
}

static void chan_color(GXChannelID chan, GXColor c, int ambient) {
    int i, first = (int)chan, n = 1;
    if (chan == GX_COLOR0A0) {
        first = GX_COLOR0;
        n = 3;
    } else if (chan == GX_COLOR1A1) {
        first = GX_COLOR1;
        n = 3;
    }
    for (i = 0; i < n; i += 2) {
        int k = first + i;
        if (k < 0 || k > 3) {
            continue;
        }
        if (ambient) {
            gx.chan[k].amb = c;
        } else {
            gx.chan[k].mat = c;
        }
    }
}

/* Compare first (M16): FaceDraw re-sends the material's ambient and material
 * colours for every material change, and consecutive materials of one model
 * usually share them.  The channel index math is chan_color's, repeated. */
static int chan_color_same(GXChannelID chan, GXColor c, int ambient) {
    int k = chan == GX_COLOR0A0 ? GX_COLOR0 : chan == GX_COLOR1A1 ? GX_COLOR1 : (int)chan;
    int n = (chan == GX_COLOR0A0 || chan == GX_COLOR1A1) ? 2 : 1;
    int i;
    for (i = 0; i < n; i++, k += 2) {
        const GXColor* have;
        if (k < 0 || k > 3) {
            continue;
        }
        have = ambient ? &gx.chan[k].amb : &gx.chan[k].mat;
        if (memcmp(have, &c, sizeof(c)) != 0) {
            return 0;
        }
    }
    return 1;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor c) {
    GX_STATE_TOUCH_IF(!chan_color_same(chan, c, 1));
    chan_color(chan, c, 1);
}
void GXSetChanMatColor(GXChannelID chan, GXColor c) {
    GX_STATE_TOUCH_IF(!chan_color_same(chan, c, 0));
    chan_color(chan, c, 0);
}

static GXLight* light_of(GXLightObj* o) { return (GXLight*)o; }

void GXInitLightPos(GXLightObj* o, f32 x, f32 y, f32 z) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    l->pos[0] = x;
    l->pos[1] = y;
    l->pos[2] = z;
    l->used = 1;
}
void GXInitLightDir(GXLightObj* o, f32 x, f32 y, f32 z) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    l->dir[0] = x;
    l->dir[1] = y;
    l->dir[2] = z;
    l->used = 1;
}
void GXInitLightColor(GXLightObj* o, GXColor c) { GX_STATE_TOUCH(); light_of(o)->color = c; }
void GXInitLightAttn(GXLightObj* o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    l->a[0] = a0;
    l->a[1] = a1;
    l->a[2] = a2;
    l->k[0] = k0;
    l->k[1] = k1;
    l->k[2] = k2;
}
void GXInitLightAttnK(GXLightObj* o, f32 k0, f32 k1, f32 k2) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    l->k[0] = k0;
    l->k[1] = k1;
    l->k[2] = k2;
}
void GXInitLightSpot(GXLightObj* o, f32 cutoff, GXSpotFn fn) {
    GX_STATE_TOUCH();
    /* GX's seven spot functions are polynomials in cos(theta); GL has one,
     * cos^exponent.  GX_SP_FLAT and GX_SP_COS map exactly; the rest are
     * approximated by the same cutoff with exponent 1. */
    GXLight* l = light_of(o);
    (void)l;
    if (fn != GX_SP_OFF && fn != GX_SP_FLAT && fn != GX_SP_COS) {
        gx_warn("GXInitLightSpot: a spot function GL cannot express exactly");
    }
    (void)cutoff;
}
void GXInitLightDistAttn(GXLightObj* o, f32 ref_distance, f32 ref_brightness,
                         GXDistAttnFn fn) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    f32 k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (fn != GX_DA_OFF && ref_distance > 0.0f && ref_brightness > 0.0f &&
        ref_brightness < 1.0f) {
        f32 t = (1.0f - ref_brightness) / ref_brightness;
        if (fn == GX_DA_GENTLE) {
            k1 = t / ref_distance;
        } else if (fn == GX_DA_MEDIUM) {
            k1 = 0.5f * t / ref_distance;
            k2 = 0.5f * t / (ref_distance * ref_distance);
        } else {
            k2 = t / (ref_distance * ref_distance);
        }
    }
    l->k[0] = k0;
    l->k[1] = k1;
    l->k[2] = k2;
}
void GXInitSpecularDir(GXLightObj* o, f32 x, f32 y, f32 z) {
    GX_STATE_TOUCH();
    GXLight* l = light_of(o);
    l->dir[0] = x;
    l->dir[1] = y;
    l->dir[2] = z;
    gx_warn("GXInitSpecularDir: specular is approximated by the diffuse term");
}

void GXLoadLightObjImm(GXLightObj* o, GXLightID id) {
    GX_STATE_TOUCH();
    int i;
    for (i = 0; i < 8; i++) {
        if (id & (1u << i)) {
            gx.light[i] = *light_of(o);
            gx.light[i].used = 1;
        }
    }
}

/* ---- texgen --------------------------------------------------------------- */

void GXSetNumTexGens(u8 n) { GX_STATE_TOUCH_IF(gx.num_texgens != n); gx.num_texgens = n; }

void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx,
                       GXBool normalize, u32 postmtx) {
    GX_STATE_TOUCH();
    if ((unsigned)dst < GX_TEXCOORDS) {
        gx.texgen[dst].func = (u8)func;
        gx.texgen[dst].src = (u8)src;
        gx.texgen[dst].mtx = (u8)mtx;
        gx.texgen[dst].normalize = (u8)(normalize ? 1 : 0);
        gx.texgen[dst].postmtx = (u8)postmtx;
    }
    if (func >= GX_TG_BUMP0 && func <= GX_TG_BUMP7) {
        gx_warn("GXSetTexCoordGen2: bump texgen is dropped, base material kept");
    }
}

void GXSetTexCoordScaleManually(GXTexCoordID coord, u8 enable, u16 ss, u16 ts) {
    GX_STATE_TOUCH();
    (void)coord;
    (void)enable;
    (void)ss;
    (void)ts;
    /* GX needs this because the hardware scales texcoords by the bound
     * texture's size; the port's decoder normalises to [0,1] itself. */
}

/* ---- TEV ------------------------------------------------------------------ */

void GXSetNumTevStages(u8 n) { GX_STATE_TOUCH_IF(gx.num_tev != n); gx.num_tev = n; }

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map,
                   GXChannelID color) {
    if ((unsigned)stage < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[stage].coord != (u8)coord || gx.tev[stage].map != (u8)map ||
                          gx.tev[stage].chan != (u8)color);
        gx.tev[stage].coord = (u8)coord;
        gx.tev[stage].map = (u8)map;
        gx.tev[stage].chan = (u8)color;
    }
}

void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                     GXTevColorArg d) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].cin[0] != (u8)a || gx.tev[s].cin[1] != (u8)b ||
                          gx.tev[s].cin[2] != (u8)c || gx.tev[s].cin[3] != (u8)d);
        gx.tev[s].cin[0] = (u8)a;
        gx.tev[s].cin[1] = (u8)b;
        gx.tev[s].cin[2] = (u8)c;
        gx.tev[s].cin[3] = (u8)d;
    }
}

void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                     GXTevAlphaArg d) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].ain[0] != (u8)a || gx.tev[s].ain[1] != (u8)b ||
                          gx.tev[s].ain[2] != (u8)c || gx.tev[s].ain[3] != (u8)d);
        gx.tev[s].ain[0] = (u8)a;
        gx.tev[s].ain[1] = (u8)b;
        gx.tev[s].ain[2] = (u8)c;
        gx.tev[s].ain[3] = (u8)d;
    }
}

void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].cop != (u8)op || gx.tev[s].cbias != (u8)bias ||
                          gx.tev[s].cscale != (u8)scale ||
                          gx.tev[s].cclamp != (u8)(clamp ? 1 : 0) || gx.tev[s].creg != (u8)out);
        gx.tev[s].cop = (u8)op;
        gx.tev[s].cbias = (u8)bias;
        gx.tev[s].cscale = (u8)scale;
        gx.tev[s].cclamp = (u8)(clamp ? 1 : 0);
        gx.tev[s].creg = (u8)out;
    }
}

void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].aop != (u8)op || gx.tev[s].abias != (u8)bias ||
                          gx.tev[s].ascale != (u8)scale ||
                          gx.tev[s].aclamp != (u8)(clamp ? 1 : 0) || gx.tev[s].areg != (u8)out);
        gx.tev[s].aop = (u8)op;
        gx.tev[s].abias = (u8)bias;
        gx.tev[s].ascale = (u8)scale;
        gx.tev[s].aclamp = (u8)(clamp ? 1 : 0);
        gx.tev[s].areg = (u8)out;
    }
}

/* GXSetTevOp is the SDK's own shorthand; the decomp's src/dolphin/gx/GXTev.c
 * spells out exactly which in/op pair each mode is, and this is that table. */
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GX_STATE_TOUCH();
    GXTevColorArg c = GX_CC_RASC;
    GXTevAlphaArg a = GX_CA_RASA;
    if (id != GX_TEVSTAGE0) {
        c = GX_CC_CPREV;
        a = GX_CA_APREV;
    }
    switch (mode) {
        case GX_MODULATE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, c, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, a, GX_CA_ZERO);
            break;
        case GX_DECAL:
            GXSetTevColorIn(id, c, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, a);
            break;
        case GX_BLEND:
            GXSetTevColorIn(id, c, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, a, GX_CA_ZERO);
            break;
        case GX_REPLACE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
            break;
        default: /* GX_PASSCLR */
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, c);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, a);
            break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColor(GXTevRegID id, GXColor c) {
    if ((unsigned)id < 4) {
        GX_STATE_TOUCH_IF(memcmp(&gx.tev_reg[id], &c, sizeof(c)) != 0);
        gx.tev_reg[id] = c;
    }
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 c) {
    GX_STATE_TOUCH();
    GXColor o;
    o.r = (u8)(c.r < 0 ? 0 : c.r > 255 ? 255 : c.r);
    o.g = (u8)(c.g < 0 ? 0 : c.g > 255 ? 255 : c.g);
    o.b = (u8)(c.b < 0 ? 0 : c.b > 255 ? 255 : c.b);
    o.a = (u8)(c.a < 0 ? 0 : c.a > 255 ? 255 : c.a);
    if ((unsigned)id < 4) {
        gx.tev_reg[id] = o;
    }
    if (c.r > 255 || c.g > 255 || c.b > 255 || c.r < 0 || c.g < 0 || c.b < 0) {
        gx_warn("GXSetTevColorS10: a TEV register outside 0..255 is clamped");
    }
}

void GXSetTevKColor(GXTevKColorID id, GXColor c) {
    if ((unsigned)id < 4) {
        GX_STATE_TOUCH_IF(memcmp(&gx.kcolor[id], &c, sizeof(c)) != 0);
        gx.kcolor[id] = c;
    }
}
void GXSetTevKColorSel(GXTevStageID s, GXTevKColorSel sel) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].kcsel != (u8)sel);
        gx.tev[s].kcsel = (u8)sel;
    }
}
void GXSetTevKAlphaSel(GXTevStageID s, GXTevKAlphaSel sel) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].kasel != (u8)sel);
        gx.tev[s].kasel = (u8)sel;
    }
}

void GXSetTevSwapMode(GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex) {
    if ((unsigned)s < GX_TEV_STAGES) {
        GX_STATE_TOUCH_IF(gx.tev[s].ras_swap != (u8)ras || gx.tev[s].tex_swap != (u8)tex);
        gx.tev[s].ras_swap = (u8)ras;
        gx.tev[s].tex_swap = (u8)tex;
    }
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g,
                           GXTevColorChan b, GXTevColorChan a) {
    GX_STATE_TOUCH();
    if ((unsigned)table < 4) {
        gx.swap_tbl[table][0] = (u8)r;
        gx.swap_tbl[table][1] = (u8)g;
        gx.swap_tbl[table][2] = (u8)b;
        gx.swap_tbl[table][3] = (u8)a;
    }
}

void GXSetTevDirect(GXTevStageID s) {
    GX_STATE_TOUCH();
    if ((unsigned)s < GX_TEV_STAGES) {
        gx.tev[s].direct = 1;
        gx.ind_tile[s].on = 0;
    }
}

void GXSetNumIndStages(u8 n) {
    GX_STATE_TOUCH();
    gx.num_ind = n;
    if (!n) {
        int i;
        for (i = 0; i < GX_TEV_STAGES; i++) {
            gx.ind_tile[i].on = 0;
        }
    }
}

void GXSetIndTexOrder(GXIndTexStageID s, GXTexCoordID c, GXTexMapID m) {
    GX_STATE_TOUCH();
    if ((unsigned)s < 4) {
        gx.ind[s].coord = (u8)c;
        gx.ind[s].map = (u8)m;
    }
}
void GXSetIndTexCoordScale(GXIndTexStageID s, GXIndTexScale ss, GXIndTexScale ts) {
    GX_STATE_TOUCH();
    if ((unsigned)s < 4) {
        gx.ind[s].scale_s = (u8)ss;
        gx.ind[s].scale_t = (u8)ts;
    }
}
void GXSetIndTexMtx(GXIndTexMtxID id, const void* offset, s8 scale_exp) {
    GX_STATE_TOUCH();
    (void)id;
    (void)offset;
    (void)scale_exp;
}
void GXSetTevIndWarp(GXTevStageID tev, GXIndTexStageID ind, GXBool signed_offset,
                     GXBool replace_mode, GXIndTexMtxID mtx) {
    GX_STATE_TOUCH();
    (void)tev;
    (void)ind;
    (void)signed_offset;
    (void)replace_mode;
    (void)mtx;
    gx_warn("GXSetTevIndWarp: dropped; --gxshader (M8) is where this comes back");
}
/* The one indirect form the port reproduces exactly.
 *
 * `HuSprDisp` draws every tiled window background with it (src/game/sprput.c
 * :99): a small indirect texture whose texels name 16x16 tiles of a larger
 * sheet.  There is no dependent texture read in GL 1.3 and no fragment
 * program on a Radeon 9000, but there does not have to be -- both textures
 * are ordinary images in main memory, so the composition the hardware would
 * do per pixel can be done once on the CPU and cached.  gx_tex.c does the
 * work; this only records what was asked for. */
void GXSetTevIndTile(GXTevStageID tev, GXIndTexStageID ind, u16 ts_s, u16 ts_t,
                     u16 tsp_s, u16 tsp_t, GXIndTexFormat fmt, GXIndTexMtxID mtx,
                     GXIndTexBiasSel bias, GXIndTexAlphaSel alpha) {
    GX_STATE_TOUCH();
    (void)mtx;
    (void)bias;
    (void)alpha;
    if ((unsigned)tev >= GX_TEV_STAGES || (unsigned)ind >= 4) {
        gx_warn("GXSetTevIndTile: stage out of range; dropped");
        return;
    }
    gx.tev[tev].direct = 0;
    gx.ind_tile[tev].on = 1;
    gx.ind_tile[tev].ind = (u8)ind;
    gx.ind_tile[tev].fmt = (u8)fmt;
    gx.ind_tile[tev].ts_s = ts_s;
    gx.ind_tile[tev].ts_t = ts_t;
    gx.ind_tile[tev].tsp_s = tsp_s;
    gx.ind_tile[tev].tsp_t = tsp_t;
}

/* ---- the pixel pipeline --------------------------------------------------- */

void GXSetZMode(GXBool compare, GXCompare func, GXBool update) {
    GX_STATE_TOUCH_IF(gx.z_enable != (u8)(compare ? 1 : 0) || gx.z_func != (u8)func ||
                      gx.z_update != (u8)(update ? 1 : 0));
    gx.z_enable = (u8)(compare ? 1 : 0);
    gx.z_func = (u8)func;
    gx.z_update = (u8)(update ? 1 : 0);
}

void GXSetZCompLoc(GXBool before_tex) { GX_STATE_TOUCH_IF(gx.z_comploc != (u8)(before_tex ? 1 : 0)); gx.z_comploc = (u8)(before_tex ? 1 : 0); }

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst,
                    GXLogicOp op) {
    GX_STATE_TOUCH_IF(gx.blend_mode != (u8)type || gx.blend_src != (u8)src ||
                      gx.blend_dst != (u8)dst || gx.blend_logic != (u8)op);
    gx.blend_mode = (u8)type;
    gx.blend_src = (u8)src;
    gx.blend_dst = (u8)dst;
    gx.blend_logic = (u8)op;
    if (type == GX_BM_SUBTRACT && !gl13_have_blend_subtract) {
        gx_warn("GX_BM_SUBTRACT without EXT_blend_subtract: drawn as a plain blend");
    }
    if (type == GX_BM_LOGIC && op != GX_LO_COPY) {
        gx_warn("GX_BM_LOGIC: logic ops are not translated");
    }
}

void GXSetAlphaCompare(GXCompare c0, u8 r0, GXAlphaOp op, GXCompare c1, u8 r1) {
    GX_STATE_TOUCH_IF(gx.alpha_comp0 != (u8)c0 || gx.alpha_ref0 != r0 || gx.alpha_op != (u8)op ||
                      gx.alpha_comp1 != (u8)c1 || gx.alpha_ref1 != r1);
    gx.alpha_comp0 = (u8)c0;
    gx.alpha_ref0 = r0;
    gx.alpha_op = (u8)op;
    gx.alpha_comp1 = (u8)c1;
    gx.alpha_ref1 = r1;
}

void GXSetColorUpdate(GXBool e) { GX_STATE_TOUCH(); gx.color_update = (u8)(e ? 1 : 0); }
void GXSetAlphaUpdate(GXBool e) { GX_STATE_TOUCH(); gx.alpha_update = (u8)(e ? 1 : 0); }
void GXSetDither(GXBool e) { (void)e; }

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    GX_STATE_TOUCH();
    gx.fog_type = (u8)type;
    gx.fog_startz = startz;
    gx.fog_endz = endz;
    gx.fog_nearz = nearz;
    gx.fog_farz = farz;
    gx.fog_color = color;
}

void GXSetFogRangeAdj(GXBool enable, u16 center, const GXFogAdjTable* table) {
    (void)enable;
    (void)center;
    (void)table;
}

void GXSetPixelFmt(GXPixelFmt pix, GXZFmt16 z) {
    (void)pix;
    (void)z;
}

/* ---- copies --------------------------------------------------------------- */

void GXSetCopyClear(GXColor c, u32 z) {
    gx.copy_clear = c;
    gx.copy_clear_z = z;
}
void GXSetDispCopySrc(u16 l, u16 t, u16 w, u16 h) {
    gx.disp_src[0] = l;
    gx.disp_src[1] = t;
    gx.disp_src[2] = w;
    gx.disp_src[3] = h;
}
void GXSetDispCopyDst(u16 w, u16 h) {
    gx.disp_dst[0] = w;
    gx.disp_dst[1] = h;
}
void GXSetTexCopySrc(u16 l, u16 t, u16 w, u16 h) {
    gx.tex_src[0] = l;
    gx.tex_src[1] = t;
    gx.tex_src[2] = w;
    gx.tex_src[3] = h;
}
void GXSetTexCopyDst(u16 w, u16 h, GXTexFmt fmt, GXBool mipmap) {
    (void)mipmap;
    gx.tex_dst[0] = w;
    gx.tex_dst[1] = h;
    gx.tex_dst_fmt = (u32)fmt;
}
u32 GXSetDispCopyYScale(f32 vscale) {
    /* The deflicker/scale path only matters for an interlaced XFB. */
    u32 lines = (u32)((f32)gx.disp_src[3] * vscale);
    return lines;
}
void GXSetCopyFilter(GXBool aa, u8 sample[12][2], GXBool vf, u8 vfilter[7]) {
    (void)aa;
    (void)sample;
    (void)vf;
    (void)vfilter;
}
void GXSetDispCopyGamma(GXGamma g) { (void)g; }
void GXAdjustForOverscan(GXRenderModeObj* in, GXRenderModeObj* out, u16 hor, u16 ver) {
    *out = *in;
    (void)hor;
    (void)ver;
}

/* ---- synchronisation, metrics, and the rest of the no-ops ----------------- */

void GXFlush(void) {}
void GXDrawDone(void) {}
void GXSetDrawDone(void) {}
void GXWaitDrawDone(void) {}
void GXPixModeSync(void) {}
void GXInvalidateVtxCache(void) {}
void GXResetWriteGatherPipe(void) {}
void GXSetDrawSync(u16 token) { (void)token; }

static GXDrawSyncCallback draw_sync_cb;
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback old = draw_sync_cb;
    draw_sync_cb = cb;
    return old;
}

void GXSetGPMetric(GXPerf0 a, GXPerf1 b) { (void)a; (void)b; }
void GXClearGPMetric(void) {}
void GXSetVCacheMetric(GXVCachePerf a) { (void)a; }
void GXClearVCacheMetric(void) {}
void GXClearPixMetric(void) {}
void GXClearMemMetric(void) {}
void GXReadGPMetric(u32* a, u32* b) { *a = *b = 0; }
void GXReadVCacheMetric(u32* a, u32* b, u32* c) { *a = *b = *c = 0; }
void GXReadPixMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f) {
    *a = *b = *c = *d = *e = *f = 0;
}
void GXReadMemMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f, u32* g, u32* h,
                     u32* i, u32* j) {
    *a = *b = *c = *d = *e = *f = *g = *h = *i = *j = 0;
}
void __GXAbortWaitPECopyDone(void) {}

/* ---- snapshots ------------------------------------------------------------
 * GX state is the one piece of port memory the game can *feel* without ever
 * reading it back: it is set once and then relied on for frames at a time (the
 * projection, the viewport, the TEV chain, the vertex descriptor and the
 * attribute array bases), so a restored run whose `gx` were reset would draw a
 * different first frame than the run it continues.  It is plain data plus
 * pointers into MEM1 and into the game's own text, both of which come back at
 * the same addresses, so it is carried verbatim. */
void gx_state_snap_register(void) {
    port_snap_register("gx.state", &gx, sizeof(gx));
    port_snap_register("gx.ready", &gx_ready, sizeof(gx_ready));
    port_snap_register("gx.fifo_obj", &fifo_obj, sizeof(fifo_obj));
    port_snap_register("gx.draw_sync_cb", &draw_sync_cb, sizeof(draw_sync_cb));
}
