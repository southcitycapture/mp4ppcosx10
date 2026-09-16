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

unsigned gl13_frame_number(void);

#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
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
    int off_tex;
    int ntex;
} Layout;

#define MAX_VERTS 65536
#define SRC_MAX_STRIDE (12 + 12 + 4 + 8 * GX_TEXCOORDS)
#define OUT_MAX_STRIDE (16 + 8 * GX_TEXCOORDS)

/* One primitive's worth of output, and -- for the display-list path -- a whole
 * list's worth of source, because the cache stores the list in one piece. */
static u8 src_buf[MAX_VERTS * SRC_MAX_STRIDE] __attribute__((aligned(16)));
static u8 out_buf[MAX_VERTS * OUT_MAX_STRIDE] __attribute__((aligned(16)));
/* Where a vertex goes when the primitive has already filled src_buf: it is
 * decoded and thrown away, so the list still steps by the right number of
 * bytes.  The old path dropped it out of transform_and_store(). */
static u8 sink_vtx[SRC_MAX_STRIDE] __attribute__((aligned(16)));
static Layout sl;              /* the source layout of the primitive in hand  */
static int out_stride, out_off_clr, out_off_tex, out_ntex;
static int nverts;             /* vertices in the primitive being assembled   */
static u32 sv_first;           /* where in src_buf this primitive starts      */

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
        const f32* mtx;  /* NULL for the identity                             */
    } tg[GX_TEXCOORDS];
    /* the indexed reader's per-attribute constants */
    const GXArraySpec* arr[GX_MAX_ATTR];
    const GXVatFmt* vat[GX_MAX_ATTR];
} PrimInv;
static PrimInv pi;
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
    port_log("port> GX draw: %u primitive(s) off-world (|position matrix "
             "translation| over %.0f)\n",
             stat_offworld, (double)GX_OFFWORLD_LIMIT);
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

static void begin_attr_order(void) {
    static const int order[] = { GX_VA_POS,  GX_VA_NRM,  GX_VA_CLR0, GX_VA_CLR1,
                                 GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3,
                                 GX_VA_TEX4, GX_VA_TEX5, GX_VA_TEX6, GX_VA_TEX7 };
    size_t i;
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
                pi.tg[t].mtx = gx.tex_mtx[ts < 20 ? ts : 0];
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
        sl.off_tex = o;
        o += 8 * sl.ntex;
        sl.stride = o;

        out_ntex = tex_slots;
        out_off_clr = 12;
        out_off_tex = 16;
        out_stride = 16 + 8 * out_ntex;
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
    DEC_CLR_RGB565, DEC_CLR_RGBA4, DEC_CLR_RGBA6
};

typedef struct DecStep {
    const u8* base;   /* indexed: the array; direct: NULL                     */
    f32 scale;        /* the VAT's fractional scale, folded in once           */
    u16 dstoff;       /* byte offset into the vertex, or into `pending`       */
    u8 stride;        /* indexed: the array's stride                          */
    u8 idx;           /* 0 direct, 1 GX_INDEX8, 2 GX_INDEX16                  */
    u8 advance;       /* direct: bytes of payload this step eats              */
    u8 op;            /* DEC_*                                                */
    u8 to_pending;    /* destination is the staging vertex, not the packed one */
    u8 attr;          /* only for the display-list cache's index range        */
} DecStep;

static DecStep plan[GX_MAX_ATTR];
static int nplan;
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
        s->scale = gx_frac_scale[f->frac & 31];
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
            if (!s->idx) {
                s->advance = (u8)dec_bytes_of(f->type, 3);
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

    for (k = 0; k < sl.ntex; k++) {
        if (!supplied[k]) {
            plan_fill[plan_nfill].dstoff = (u16)(sl.off_tex + 8 * k);
            plan_fill[plan_nfill].s = pending.tex[k][0];
            plan_fill[plan_nfill].t = pending.tex[k][1];
            plan_nfill++;
        }
    }
}

static void transform_and_store(void);
static void finish_vertices(const u8* s, int n);

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

static void attr_written(void) {
    if (!nactive) {
        return;
    }
    if (++acur >= nactive) {
        acur = 0;
        transform_and_store();
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
    k = gx_frac_scale[gx.vat[vtxfmt][a].frac & 31];
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
        if (cc->diff_fn == GX_DF_CLAMP) {
            ndl = ndl < 0.0f ? 0.0f : ndl;
        } else if (cc->diff_fn == GX_DF_SIGN) {
            /* signed: keep it */
        } else {
            ndl = 1.0f;
        }
        att = 1.0f;
        if (cc->attn_fn != GX_AF_NONE) {
            float den = l->k[0] + l->k[1] * d + l->k[2] * d2;
            att = den > 0.0f ? 1.0f / den : 1.0f;
            if (att > 1.0f) {
                att = 1.0f;
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
static void finish_vertices(const u8* s, int n) {
    const f32* m = pi.pos_mtx;
    const f32* nm = pi.nrm_mtx;
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
                    in[0] = op[0];
                    in[1] = op[1];
                    in[2] = op[2];
                } else {
                    in[0] = nrm[0];
                    in[1] = nrm[1];
                    in[2] = nrm[2];
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
    if (nverts >= MAX_VERTS || sv_first + (u32)nverts >= MAX_VERTS) {
        gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");
        return;
    }
    v = src_buf + (size_t)(sv_first + (u32)nverts) * sl.stride;
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
        pending.nrm[0] = read_component(p, f->type, f->frac, 0);
        pending.nrm[1] = read_component(p, f->type, f->frac, 1);
        pending.nrm[2] = read_component(p, f->type, f->frac, 2);
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
void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
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
    GXLOG("GXBegin", "prim %02x fmt %d n %u, %d attrs", type, fmt, n, nactive);
}

/* --drawlog N: explain the first N draws in full -- the geometry after the CPU
 * transform, the raster colour, the texture that is bound and the pipeline
 * state that decides whether any of it survives to the framebuffer.  Written
 * because "the draw happens and the screen stays black" has too many possible
 * causes to reason about from the source, and each of them is one line here. */
static void draw_log(void) {
    static int shown;
    int i;
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
        const u8* o = out_buf + (size_t)i * out_stride;
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
    port_log("  proj %s [%g %g %g %g %g %g]  viewport %g %g %g %g z %g..%g\n",
             gx.proj_type == GX_PERSPECTIVE ? "persp" : "ortho", gx.proj[0],
             gx.proj[1], gx.proj[2], gx.proj[3], gx.proj[4], gx.proj[5], gx.vp[0],
             gx.vp[1], gx.vp[2], gx.vp[3], gx.vp[4], gx.vp[5]);
    port_log("  stage0 coord %u map %u chan %u  cin %u %u %u %u  ain %u %u %u %u\n",
             s0->coord, s0->map, s0->chan, s0->cin[0], s0->cin[1], s0->cin[2],
             s0->cin[3], s0->ain[0], s0->ain[1], s0->ain[2], s0->ain[3]);
    t = gx_bound_tex(s0->map);
    if (t) {
        port_log("  texmap%u %ux%u fmt %u ci %u tlut %u gl %u\n", s0->map, t->width,
                 t->height, t->format, t->is_ci, t->tlut_name, t->gl_name);
    } else {
        port_log("  texmap%u NOT BOUND\n", s0->map);
    }
    port_log("  chan0 enable %u matsrc %u mat %u %u %u %u  ambsrc %u\n",
             gx.chan[0].enable, gx.chan[0].mat_src, gx.chan[0].mat.r,
             gx.chan[0].mat.g, gx.chan[0].mat.b, gx.chan[0].mat.a,
             gx.chan[0].amb_src);
    port_log("  alphacmp %u ref %u op %u / %u ref %u   zmode test %u fn %u write %u\n",
             gx.alpha_comp0, gx.alpha_ref0, gx.alpha_op, gx.alpha_comp1,
             gx.alpha_ref1, gx.z_enable, gx.z_func, gx.z_update);
    port_log("  blend mode %u src %u dst %u   cull %u   scissor %u %u %u %u\n",
             gx.blend_mode, gx.blend_src, gx.blend_dst, gx.cull,
             gx.scissor[0], gx.scissor[1], gx.scissor[2], gx.scissor[3]);
    {
        GLenum e = GL(glGetError)();
        port_log("  glGetError %s (0x%04x)\n", e == GL_NO_ERROR ? "GL_NO_ERROR" : "SET",
                 (unsigned)e);
    }
}

/* The draw itself, once the source run is in hand.  `s` is where phase 2
 * reads from: src_buf for a primitive the writers just assembled, and the
 * cached copy for a display list that hit. */
static void draw_run(const u8* s, int n) {
    if (!n) {
        return;
    }
    stat_prims++;
    stat_verts += (unsigned)n;
    if (!gl13_live()) {
        return;
    }
    nverts = n; /* draw_log reads it */
    finish_vertices(s, n);
    gl13_apply_transform();
    gl13_apply_raster_state();
    gx_tev_apply();
    /* After the state is applied, not before: the texture cache fills in
     * gl_name at bind time, so a log taken earlier reports a stale 0 and
     * sends you hunting for a texture upload that already happened. */
    draw_log();

    /* `out_buf` is a static buffer and every draw reads it from index zero, so
     * the base pointer never moves; the offsets and the stride do, because the
     * layout is now packed to the primitive.  glc_* compares both. */
    glc_vertex_array(out_buf, out_stride);
    glc_color_array(out_buf + out_off_clr, out_stride);
    {
        int i;
        for (i = 0; i < gl13_max_tex_units; i++) {
            int stage = i < gx.num_tev ? i : -1;
            if (stage >= 0 && gx.tev[stage].coord < out_ntex &&
                gx_bound_tex(gx.tev[stage].map) != NULL) {
                glc_coord_array(i,
                                out_buf + out_off_tex + 8 * gx.tev[stage].coord,
                                out_stride);
            } else {
                glc_coord_array(i, NULL, 0);
            }
        }
    }
    GL(glDrawArrays)(gl_prim(prim), 0, n);
    stat_draws++;
}

static void draw_now(void) {
    draw_run(src_buf + (size_t)sv_first * sl.stride, nverts);
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
                      unsigned frame, int born_dynamic) {
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
    memcpy(e->src, src_buf, sbytes);
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
static void dl_track_index(u8 attr, u32 ix) {
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
            if (sv_first + (u32)nverts >= MAX_VERTS) {                                   \
                gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");\
                v = sink_vtx; /* decoded and dropped, so the list still steps */         \
            } else {                                                                     \
                v = src_buf + (size_t)(sv_first + (u32)nverts) * sl.stride;               \
                nverts++;                                                                \
                lastv = v;                                                               \
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
                if (st->idx == 2) {                                                      \
                    u32 ix = ((u32)p[0] << 8) | p[1];                                     \
                    q = st->base + (size_t)ix * st->stride;                              \
                    if (TRACK) {                                                         \
                        dl_track_index(st->attr, ix);                                    \
                    }                                                                    \
                    p += 2;                                                              \
                } else if (st->idx == 1) {                                               \
                    u32 ix = p[0];                                                       \
                    q = st->base + (size_t)ix * st->stride;                              \
                    if (TRACK) {                                                         \
                        dl_track_index(st->attr, ix);                                    \
                    }                                                                    \
                    p += 1;                                                              \
                } else {                                                                 \
                    q = p;                                                               \
                    p += st->advance;                                                    \
                }                                                                        \
                d = st->to_pending ? (u8*)&pending + st->dstoff : v + st->dstoff;        \
                dp = (f32*)d;                                                            \
                sc = st->scale;                                                          \
                switch (st->op) {                                                        \
                    DEC_CASE_F32                                                         \
                    DEC_CASE_TYPE(S16, DEC_S16)                                          \
                    DEC_CASE_TYPE(U16, DEC_U16)                                          \
                    DEC_CASE_TYPE(S8, DEC_S8)                                            \
                    DEC_CASE_TYPE(U8, DEC_U8)                                            \
                    DEC_CASE_COLOUR                                                      \
                    default: break; /* DEC_NONE: a null array, as before */              \
                }                                                                        \
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

void GXCallDisplayList(const void* list, u32 nbytes) {
    const u8* p = (const u8*)list;
    const u8* end = p + nbytes;
    u32 state_h = 0, list_h = 0, total = 0;
    unsigned frame;
    DlEntry* hit = NULL;
    DlEntry* found = NULL;
    int caching;
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
             * still happens per segment, including begin_attr_order -- which
             * is what settles the matrices, the texgens and the layout for
             * phase 2, and what counts an off-world draw. */
            int i;
            hit->last_frame = frame;
            stat_dlc_hit++;
            for (i = 0; i < hit->nsegs; i++) {
                const DlSeg* g = &hit->segs[i];
                prim = (u8)(g->op & 0xF8);
                vtxfmt = (u8)(g->op & 0x07);
                in_prim = 0;
                nverts = 0;
                begin_attr_order();
                if (nactive == 0 || sl.stride != hit->stride) {
                    break;
                }
                draw_run(hit->src + (size_t)g->first * hit->stride, (int)g->count);
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
            p = caching ? decode_run_tracked(p, end, count)
                        : decode_run(p, end, count);
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
                            pending.nrm[0] = read_component(p, f->type, f->frac, 0);
                            pending.nrm[1] = read_component(p, f->type, f->frac, 1);
                            pending.nrm[2] = read_component(p, f->type, f->frac, 2);
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
        draw_now();
        total = sv_first + (u32)nverts;
        nverts = 0;
    }
    if (caching) {
        dlc_store(list, nbytes, list_h, state_h, total, frame,
                  miss_reason == 3);
    }
    sv_first = 0;
    port_perf_gx_end();
    dl_replaying = 0;
}

/* ---- the display buffer ----------------------------------------------------- */

void GXCopyDisp(void* dest, GXBool clear) {
    (void)dest;
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
    gx_tex_init();
    gl13_init();
    gx_logging = port_opt.gxlog;
}

void port_gx_present(void) { gl13_present(); }

void gl13_state_report(void);

void port_gx_shutdown(void) {
    gx_draw_report();
    gl13_state_report();
    gx_tex_report();
    gx_tex_tile_report();
    gx_warn_report();
    gl13_shutdown();
}

void port_gx_frame_number(unsigned n) { (void)n; }
