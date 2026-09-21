/* --gxdemo: exercise the GX slice without the game.
 *
 * The host build cannot reach the Nintendo/Hudson logos, because those come
 * out of a 32-bit big-endian sprite bank the host cannot parse (see
 * port/src/dvd/host_data.c).  That leaves the GX layer unverified on the
 * machine where it is being written, which is exactly the position the
 * Snowboard Kids ports were in when their worst rendering bugs went
 * unnoticed for weeks.
 *
 * So the port drives GX itself.  `--gxdemo` calls the same public GX entry
 * points the game calls, with data the port builds in memory, and writes the
 * result as a PPM.  It covers, in one frame:
 *
 *   - an orthographic projection, a viewport and a scissor;
 *   - direct `GX_QUADS` with `GXPosition3f32` + `GXColor4u8` (the sprite path);
 *   - indexed `GX_TRIANGLESTRIP` through `GXSetArray` + `GXPosition1x16` /
 *     `GXTexCoord1x16` with an `S16` position array and a fractional shift
 *     (the HSF path);
 *   - a texture in each of the ten formats the game uses, built here and
 *     decoded through gx_tex.c, including a C4 and a C8 through a TLUT and a
 *     CMPR block;
 *   - a one-stage `GX_MODULATE` TEV chain and a two-stage chain;
 *   - a display list recorded with `GXBeginDisplayList` and replayed with
 *     `GXCallDisplayList`, which checks the byte encoding round-trips;
 *   - alpha blending and the alpha test.
 *
 * If this frame is right, the parts of §9.4's 55-call boot list that draw
 * anything are right, and what is left for the G4 is the game's own data.
 */
#include "port.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/types.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

void gl13_write_ppm(const char* path);
int gl13_live(void);

/* ---- little texture builders, in the console's own tiled layouts ---------- */

static u8 tex_rgb565[32 * 32 * 2];
static u8 tex_rgb5a3[32 * 32 * 2];
static u8 tex_i4[32 * 32 / 2];
static u8 tex_i8[32 * 32];
static u8 tex_ia8[32 * 32 * 2];
static u8 tex_rgba8[32 * 32 * 4];
static u8 tex_c8[32 * 32];
static u8 tlut_data[256 * 2];
static u8 tex_cmpr[32 * 32 / 2];

static void put_be16(u8* p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/* index of texel (x,y) inside a tiled image of width w with bw x bh tiles */
static int tile_index(int x, int y, int w, int bw, int bh) {
    int tx = x / bw, ty = y / bh;
    int tiles_across = w / bw;
    return ((ty * tiles_across + tx) * bw * bh) + (y % bh) * bw + (x % bw);
}

static void build_textures(void) {
    int x, y, i;
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            int checker = ((x >> 2) ^ (y >> 2)) & 1;
            u8 r = (u8)(x * 8), g = (u8)(y * 8), b = (u8)(checker ? 255 : 0);
            put_be16(tex_rgb565 + tile_index(x, y, 32, 4, 4) * 2,
                     (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
            put_be16(tex_rgb5a3 + tile_index(x, y, 32, 4, 4) * 2,
                     (u16)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)));
            put_be16(tex_ia8 + tile_index(x, y, 32, 4, 4) * 2, /* AAAAAAAA IIIIIIII (M34) */
                     (u16)(((checker ? 0xFF : 0x40) << 8) | r));
            tex_i8[tile_index(x, y, 32, 8, 4)] = (u8)(x * 8);
            tex_c8[tile_index(x, y, 32, 8, 4)] = (u8)((x >> 2) + 8 * (y >> 3));
            {
                int idx = tile_index(x, y, 32, 8, 8);
                u8* p = &tex_i4[idx / 2];
                u8 v = (u8)(y >> 1);
                if ((idx & 1) == 0) {
                    *p = (u8)((*p & 0x0F) | (v << 4));
                } else {
                    *p = (u8)((*p & 0xF0) | v);
                }
            }
            {
                /* RGBA8: 4x4 tiles of sixteen AR pairs then sixteen GB pairs */
                int tx = x / 4, ty = y / 4, tiles = 32 / 4;
                u8* t = tex_rgba8 + ((size_t)(ty * tiles + tx) * 64);
                int k = (y % 4) * 4 + (x % 4);
                t[k * 2 + 0] = 255;
                t[k * 2 + 1] = r;
                t[32 + k * 2 + 0] = g;
                t[32 + k * 2 + 1] = b;
            }
        }
    }
    for (i = 0; i < 256; i++) {
        put_be16(tlut_data + i * 2, (u16)(0x8000 | ((i & 0x1F) << 10) | ((i >> 3) << 5) | 0x1F));
    }
    /* CMPR: 8x8 tiles of four DXT1 blocks; make every block a two-colour ramp */
    for (i = 0; i < (int)sizeof(tex_cmpr); i += 8) {
        put_be16(tex_cmpr + i, 0xF800);     /* red   */
        put_be16(tex_cmpr + i + 2, 0x001F); /* blue  */
        tex_cmpr[i + 4] = 0x00;
        tex_cmpr[i + 5] = 0x55;
        tex_cmpr[i + 6] = 0xAA;
        tex_cmpr[i + 7] = 0xFF;
    }
}

/* ---- the frame ------------------------------------------------------------ */

static void ortho(void) {
    Mtx44 p;
    memset(p, 0, sizeof(p));
    p[0][0] = 2.0f / 640.0f;
    p[0][3] = -1.0f;
    p[1][1] = -2.0f / 480.0f;
    p[1][3] = 1.0f;
    p[2][2] = -1.0f;
    p[3][3] = 1.0f;
    GXSetProjection(p, GX_ORTHOGRAPHIC);
}

static void identity_pos(void) {
    Mtx m;
    memset(m, 0, sizeof(m));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    GXLoadPosMtxImm(m, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

static void plain_state(void) {
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetNumChans(0);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
}

static void quad(f32 x, f32 y, f32 w, f32 h, u8 r, u8 g, u8 b, u8 a) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x, y, 0.0f);
    GXColor4u8(r, g, b, a);
    GXPosition3f32(x + w, y, 0.0f);
    GXColor4u8(r, g, b, a);
    GXPosition3f32(x + w, y + h, 0.0f);
    GXColor4u8(r, g, b, a);
    GXPosition3f32(x, y + h, 0.0f);
    GXColor4u8(r, g, b, a);
    GXEnd();
}

/* one textured quad, in whatever format the object was made with */
static void tex_quad(GXTexObj* obj, f32 x, f32 y, f32 s) {
    GXLoadTexObj(obj, GX_TEXMAP0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE,
                      GX_PTIDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x, y, 0.0f);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(x + s, y, 0.0f);
    GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(x + s, y + s, 0.0f);
    GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(x, y + s, 0.0f);
    GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
}

/* the indexed path, and a display list round-trip */
static s16 pos_array[8 * 3];
static u8 dl_storage[4096];

static void indexed_strip_through_a_display_list(void) {
    u32 size;
    int i;
    for (i = 0; i < 8; i++) {
        /* a strip of four quads' worth of vertices, S16 with 4 fraction bits */
        f32 fx = 40.0f + (f32)(i / 2) * 60.0f;
        f32 fy = (i & 1) ? 400.0f : 340.0f;
        /* Vertex arrays are read big-endian, because on the G4 and on the
         * disc that is what they are; the demo writes them the same way so
         * the host exercises the target's code path. */
        put_be16((u8*)&pos_array[i * 3 + 0], (u16)(s16)(fx * 16.0f));
        put_be16((u8*)&pos_array[i * 3 + 1], (u16)(s16)(fy * 16.0f));
        put_be16((u8*)&pos_array[i * 3 + 2], 0);
    }
    plain_state();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 4);
    GXSetArray(GX_VA_POS, pos_array, 6);
    GXSetNumChans(1);
    {
        GXColor c = { 80, 220, 120, 255 };
        GXSetChanMatColor(GX_COLOR0A0, c);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE,
                      GX_AF_NONE);
    }
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GXBeginDisplayList(dl_storage, sizeof(dl_storage));
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 8);
    for (i = 0; i < 8; i++) {
        GXPosition1x16((u16)i);
    }
    GXEnd();
    size = GXEndDisplayList();
    port_log("port> --gxdemo: display list recorded, %u bytes (1 opcode + 2 count + "
             "8 indices = 19, padded to 32)\n", size);
    GXCallDisplayList(dl_storage, size);
}

/* GXInit's own arguments are a FIFO the port does not keep, so the demo can
 * call it with nothing. */
void GXInit_demo_bootstrap(void) { GXInit(NULL, 0); }

void port_gx_demo(void) {
    GXTexObj t_rgb565, t_rgb5a3, t_rgba8, t_i8, t_i4, t_ia8, t_cmpr, t_c8;
    GXTlutObj tlut;
    GXColor clear = { 24, 24, 40, 255 };

    build_textures();
    GXSetCopyClear(clear, 0xFFFFFF);
    GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 480);
    ortho();
    identity_pos();
    /* Clear now, not through GXCopyDisp.  GXCopyDisp queues its clear until
     * after the swap, because on the console the EFB-to-XFB copy happens first
     * and the clear is for the *next* frame -- see gl13_clear_at_swap.  The
     * demo renders exactly one frame and then dumps it, so it has no previous
     * frame to have been cleared by, and asks for the clear directly. */
    { void gl13_clear(GXColor, u32); gl13_clear(clear, 0xFFFFFF); }

    plain_state();
    quad(20.0f, 20.0f, 200.0f, 60.0f, 220, 60, 60, 255);
    quad(240.0f, 20.0f, 200.0f, 60.0f, 60, 220, 60, 255);
    /* blended, over both */
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    quad(120.0f, 40.0f, 200.0f, 60.0f, 60, 60, 240, 128);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);

    GXInitTexObj(&t_rgb565, tex_rgb565, 32, 32, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_rgb5a3, tex_rgb5a3, 32, 32, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_rgba8, tex_rgba8, 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_i8, tex_i8, 32, 32, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_i4, tex_i4, 32, 32, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_ia8, tex_ia8, 32, 32, GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&t_cmpr, tex_cmpr, 32, 32, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTlutObj(&tlut, tlut_data, GX_TL_RGB5A3, 256);
    GXLoadTlut(&tlut, 0);
    GXInitTexObjCI(&t_c8, tex_c8, 32, 32, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, 0);

    tex_quad(&t_rgb565, 20.0f, 120.0f, 70.0f);
    tex_quad(&t_rgb5a3, 100.0f, 120.0f, 70.0f);
    tex_quad(&t_rgba8, 180.0f, 120.0f, 70.0f);
    tex_quad(&t_i8, 260.0f, 120.0f, 70.0f);
    tex_quad(&t_i4, 340.0f, 120.0f, 70.0f);
    tex_quad(&t_ia8, 420.0f, 120.0f, 70.0f);
    tex_quad(&t_cmpr, 500.0f, 120.0f, 70.0f);
    tex_quad(&t_c8, 20.0f, 210.0f, 70.0f);

    /* a two-stage chain: texture modulated by a konst colour */
    {
        GXColor k = { 255, 200, 0, 255 };
        GXSetTevKColor(GX_KCOLOR0, k);
        GXSetNumTevStages(2);
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
        GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
        GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
        GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                        GX_TEVPREV);
        GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                        GX_TEVPREV);
        tex_quad(&t_rgb565, 100.0f, 210.0f, 70.0f);
        GXSetNumTevStages(1);
    }

    indexed_strip_through_a_display_list();

    GXDrawDone();
}
