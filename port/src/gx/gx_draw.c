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
#include "gx_internal.h"
#include "gx_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
#endif

#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

/* ---- the vertex buffer ---------------------------------------------------- */

typedef struct Vtx {
    float pos[3];
    float nrm[3];
    unsigned char clr[2][4];
    float tex[GX_TEXCOORDS][2];
} Vtx;

#define MAX_VERTS 65536
static Vtx verts[MAX_VERTS];
static int nverts;

static Vtx pending;
static u8 have[GX_MAX_ATTR];
static int active[GX_MAX_ATTR]; /* attributes in descriptor order */
static int nactive;
static int last_active;
static int tex_slots;
static int in_prim;
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

void gx_draw_report(void) {
    if (!stat_prims) {
        return;
    }
    port_log("port> GX draw: %u primitives, %u vertices, %u glDrawArrays, "
             "%u display lists replayed\n",
             stat_prims, stat_verts, stat_draws, stat_dls);
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
    last_active = nactive ? active[nactive - 1] : -1;
    memset(have, 0, sizeof(have));

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
}

static void transform_and_store(void);

static void attr_done(int attr) {
    have[attr] = 1;
    if (attr == last_active) {
        transform_and_store();
        memset(have, 0, sizeof(have));
    }
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

static void transform_and_store(void) {
    Vtx* v;
    const f32* m = gx.pos_mtx[gx.cur_pnmtx < 10 ? gx.cur_pnmtx : 0];
    const f32* n = gx.nrm_mtx[gx.cur_pnmtx < 10 ? gx.cur_pnmtx : 0];
    float px, py, pz;
    int i;
    if (nverts >= MAX_VERTS) {
        gx_warn("GXBegin: more than 65536 vertices in one primitive; truncated");
        return;
    }
    v = &verts[nverts++];
    /* Not `*v = pending`.  A Vtx is 96 bytes and every one of its fields is
     * either overwritten below or unread by the draw; copying the whole thing
     * per vertex was pure memory traffic in the hottest loop in the port.
     * Only the two colours are carried across verbatim. */
    v->clr[0][0] = pending.clr[0][0];
    v->clr[0][1] = pending.clr[0][1];
    v->clr[0][2] = pending.clr[0][2];
    v->clr[0][3] = pending.clr[0][3];
    v->clr[1][0] = pending.clr[1][0];
    v->clr[1][1] = pending.clr[1][1];
    v->clr[1][2] = pending.clr[1][2];
    v->clr[1][3] = pending.clr[1][3];

    px = pending.pos[0];
    py = pending.pos[1];
    pz = pending.pos[2];
    v->pos[0] = m[0] * px + m[1] * py + m[2] * pz + m[3];
    v->pos[1] = m[4] * px + m[5] * py + m[6] * pz + m[7];
    v->pos[2] = m[8] * px + m[9] * py + m[10] * pz + m[11];

    if (gx.vcd[GX_VA_NRM] != GX_NONE) {
        float nx = pending.nrm[0], ny = pending.nrm[1], nz = pending.nrm[2];
        float len2;
        v->nrm[0] = n[0] * nx + n[1] * ny + n[2] * nz;
        v->nrm[1] = n[3] * nx + n[4] * ny + n[5] * nz;
        v->nrm[2] = n[6] * nx + n[7] * ny + n[8] * nz;
        len2 = v->nrm[0] * v->nrm[0] + v->nrm[1] * v->nrm[1] + v->nrm[2] * v->nrm[2];
        if (len2 > 0.0f) {
            float rl = gx_rsqrtf(len2);
            v->nrm[0] *= rl;
            v->nrm[1] *= rl;
            v->nrm[2] *= rl;
        }
    } else {
        v->nrm[0] = v->nrm[1] = 0.0f;
        v->nrm[2] = 1.0f;
    }

    if (gx.vcd[GX_VA_CLR0] == GX_NONE) {
        v->clr[0][0] = gx.chan[0].mat.r;
        v->clr[0][1] = gx.chan[0].mat.g;
        v->clr[0][2] = gx.chan[0].mat.b;
        v->clr[0][3] = gx.chan[0].mat.a;
    }
    if (gx.num_chans > 0) {
        light_channel(0, v->pos, v->nrm, v->clr[0]);
    }

    /* texgen: the only forms the game uses are a 2x4/3x4 matrix over a
     * texcoord or over the position (PLAN.md §1.14 -- there are no bump or
     * SRTG texgens outside the handful already warned about). */
    for (i = 0; i < gx.num_texgens && i < GX_TEXCOORDS; i++) {
        const GXTexGen* g = &gx.texgen[i];
        float s, t, in[3];
        u32 slot;
        if (g->src >= GX_TG_TEX0 && g->src <= GX_TG_TEX7) {
            int k = g->src - GX_TG_TEX0;
            in[0] = pending.tex[k][0];
            in[1] = pending.tex[k][1];
            in[2] = 1.0f;
        } else if (g->src == GX_TG_POS) {
            in[0] = v->pos[0];
            in[1] = v->pos[1];
            in[2] = v->pos[2];
        } else if (g->src == GX_TG_NRM) {
            in[0] = v->nrm[0];
            in[1] = v->nrm[1];
            in[2] = v->nrm[2];
        } else {
            in[0] = pending.tex[i][0];
            in[1] = pending.tex[i][1];
            in[2] = 1.0f;
        }
        if (g->mtx == GX_IDENTITY || g->mtx < GX_TEXMTX0) {
            s = in[0];
            t = in[1];
        } else {
            const f32* tm;
            slot = ((u32)g->mtx - GX_TEXMTX0) / 3;
            tm = gx.tex_mtx[slot < 20 ? slot : 0];
            s = tm[0] * in[0] + tm[1] * in[1] + tm[2] * in[2] + tm[3];
            t = tm[4] * in[0] + tm[5] * in[1] + tm[6] * in[2] + tm[7];
            if (g->func == GX_TG_MTX3x4) {
                float q = tm[8] * in[0] + tm[9] * in[1] + tm[10] * in[2] + tm[11];
                if (q != 0.0f) {
                    s /= q;
                    t /= q;
                }
            }
        }
        v->tex[i][0] = s;
        v->tex[i][1] = t;
    }
    for (; i < tex_slots; i++) {
        v->tex[i][0] = 0.0f;
        v->tex[i][1] = 0.0f;
    }
}

/* ---- the writers ----------------------------------------------------------- */

static void set_pos(f32 x, f32 y, f32 z) {
    pending.pos[0] = x;
    pending.pos[1] = y;
    pending.pos[2] = z;
    attr_done(GX_VA_POS);
}

static void indexed(int attr, u32 index) {
    const GXArraySpec* a = &gx.array[attr];
    const GXVatFmt* f = &gx.vat[vtxfmt][attr];
    const u8* p;
    if (!a->base || !a->stride) {
        attr_done(attr);
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
    attr_done(attr);
}

/* Which attribute a writer belongs to depends only on its name, so the
 * indexed writers each know theirs. */
#define IDX_WRITER(name, attr, T, put)                                                   \
    void name(T index) {                                                                 \
        if (dl_recording) {                                                              \
            put;                                                                         \
            return;                                                                      \
        }                                                                                \
        indexed(attr, (u32)index);                                                       \
    }

IDX_WRITER(GXPosition1x16, GX_VA_POS, u16, dl_u16(index))
IDX_WRITER(GXPosition1x8, GX_VA_POS, u8, dl_u8(index))
IDX_WRITER(GXNormal1x16, GX_VA_NRM, u16, dl_u16(index))
IDX_WRITER(GXNormal1x8, GX_VA_NRM, u8, dl_u8(index))
IDX_WRITER(GXColor1x16, GX_VA_CLR0, u16, dl_u16(index))
IDX_WRITER(GXColor1x8, GX_VA_CLR0, u8, dl_u8(index))
IDX_WRITER(GXTexCoord1x16, GX_VA_TEX0, u16, dl_u16(index))
IDX_WRITER(GXTexCoord1x8, GX_VA_TEX0, u8, dl_u8(index))

void GXPosition3f32(f32 x, f32 y, f32 z) {
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        dl_f32(z);
        return;
    }
    set_pos(x, y, z);
}
void GXPosition2f32(f32 x, f32 y) {
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        return;
    }
    set_pos(x, y, 0.0f);
}
void GXPosition3s16(s16 x, s16 y, s16 z) {
    u8 frac = gx.vat[vtxfmt][GX_VA_POS].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        dl_u16((u16)z);
        return;
    }
    set_pos(x * k, y * k, z * k);
}
void GXPosition2s16(s16 x, s16 y) {
    u8 frac = gx.vat[vtxfmt][GX_VA_POS].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        return;
    }
    set_pos(x * k, y * k, 0.0f);
}
void GXPosition2u16(u16 x, u16 y) {
    u8 frac = gx.vat[vtxfmt][GX_VA_POS].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u16(x);
        dl_u16(y);
        return;
    }
    set_pos(x * k, y * k, 0.0f);
}
void GXPosition3u8(u8 x, u8 y, u8 z) {
    u8 frac = gx.vat[vtxfmt][GX_VA_POS].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u8(x);
        dl_u8(y);
        dl_u8(z);
        return;
    }
    set_pos(x * k, y * k, z * k);
}
void GXNormal3f32(f32 x, f32 y, f32 z) {
    if (dl_recording) {
        dl_f32(x);
        dl_f32(y);
        dl_f32(z);
        return;
    }
    pending.nrm[0] = x;
    pending.nrm[1] = y;
    pending.nrm[2] = z;
    attr_done(GX_VA_NRM);
}
void GXNormal3s16(s16 x, s16 y, s16 z) {
    u8 frac = gx.vat[vtxfmt][GX_VA_NRM].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u16((u16)x);
        dl_u16((u16)y);
        dl_u16((u16)z);
        return;
    }
    pending.nrm[0] = x * k;
    pending.nrm[1] = y * k;
    pending.nrm[2] = z * k;
    attr_done(GX_VA_NRM);
}
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
    if (dl_recording) {
        dl_u8(r);
        dl_u8(g);
        dl_u8(b);
        dl_u8(a);
        return;
    }
    pending.clr[0][0] = r;
    pending.clr[0][1] = g;
    pending.clr[0][2] = b;
    pending.clr[0][3] = a;
    attr_done(GX_VA_CLR0);
}
void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
void GXColor1u32(u32 c) {
    GXColor4u8((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c);
}
void GXTexCoord2f32(f32 s, f32 t) {
    if (dl_recording) {
        dl_f32(s);
        dl_f32(t);
        return;
    }
    pending.tex[0][0] = s;
    pending.tex[0][1] = t;
    attr_done(GX_VA_TEX0);
}
void GXTexCoord2s16(s16 s, s16 t) {
    u8 frac = gx.vat[vtxfmt][GX_VA_TEX0].frac;
    f32 k = 1.0f / (f32)(1u << frac);
    if (dl_recording) {
        dl_u16((u16)s);
        dl_u16((u16)t);
        return;
    }
    pending.tex[0][0] = s * k;
    pending.tex[0][1] = t * k;
    attr_done(GX_VA_TEX0);
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
    shown++;
    port_log("---- draw %d: prim %02x, %d verts, %d tev stage(s), %d texgen(s), "
             "%d chan(s) ----\n",
             shown, prim, nverts, gx.num_tev, gx.num_texgens, gx.num_chans);
    for (i = 0; i < nverts && i < 4; i++) {
        const Vtx* v = &verts[i];
        port_log("  v%d pos %8.2f %8.2f %8.2f  clr %3u %3u %3u %3u  st %6.3f %6.3f\n",
                 i, v->pos[0], v->pos[1], v->pos[2], v->clr[0][0], v->clr[0][1],
                 v->clr[0][2], v->clr[0][3], v->tex[0][0], v->tex[0][1]);
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

static void draw_now(void) {
    if (!nverts) {
        return;
    }
    stat_prims++;
    stat_verts += (unsigned)nverts;
    if (!gl13_live()) {
        return;
    }
    gl13_apply_transform();
    gl13_apply_raster_state();
    gx_tev_apply();
    /* After the state is applied, not before: the texture cache fills in
     * gl_name at bind time, so a log taken earlier reports a stale 0 and
     * sends you hunting for a texture upload that already happened. */
    draw_log();

    /* `verts` is a static buffer and every draw reads it from index zero, so
     * these three pointers never change for the life of the process; the
     * cache turns 18 client-state calls a draw into none. */
    glc_vertex_array(&verts[0].pos[0], (int)sizeof(Vtx));
    glc_color_array(&verts[0].clr[0][0], (int)sizeof(Vtx));
    {
        int i;
        for (i = 0; i < gl13_max_tex_units; i++) {
            int stage = i < gx.num_tev ? i : -1;
            if (stage >= 0 && gx.tev[stage].coord < GX_TEXCOORDS &&
                gx_bound_tex(gx.tev[stage].map) != NULL) {
                glc_coord_array(i, &verts[0].tex[gx.tev[stage].coord][0],
                                (int)sizeof(Vtx));
            } else {
                glc_coord_array(i, NULL, 0);
            }
        }
    }
    GL(glDrawArrays)(gl_prim(prim), 0, nverts);
    stat_draws++;
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

void GXCallDisplayList(const void* list, u32 nbytes) {
    const u8* p = (const u8*)list;
    const u8* end = p + nbytes;
    if (dl_recording) {
        gx_warn("GXCallDisplayList inside a display list is not supported");
        return;
    }
    dl_replaying = 1;
    stat_dls++;
    port_perf_gx_begin();
    /* --drawlog also explains display-list replays: the list's size and, per
     * opcode, the primitive, the vertex count and how many attributes the
     * current descriptor says each vertex carries.  `nactive == 0` is the
     * dangerous one -- the parser then cannot know the stride, so it consumes
     * nothing and the list never advances. */
    if (port_opt.drawlog && dl_shown < port_opt.drawlog) {
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
        in_prim = 1;
        begin_attr_order();
        if (port_opt.drawlog && dl_shown < port_opt.drawlog) {
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
            break;
        }
        for (i = 0; i < count && p < end; i++) {
            for (k = 0; k < nactive; k++) {
                int attr = active[k];
                u8 desc = gx.vcd[attr];
                const GXVatFmt* f = &gx.vat[vtxfmt][attr];
                if (desc == GX_INDEX16) {
                    indexed(attr, rd_be16(p));
                    p += 2;
                } else if (desc == GX_INDEX8) {
                    indexed(attr, *p);
                    p += 1;
                } else { /* GX_DIRECT */
                    int comps, bytes;
                    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                        bytes = color_bytes(f->type);
                        read_color(p, f->type, pending.clr[attr - GX_VA_CLR0]);
                        p += bytes;
                        attr_done(attr);
                        continue;
                    }
                    comps = attr == GX_VA_POS   ? (f->cnt == GX_POS_XYZ ? 3 : 2)
                            : attr == GX_VA_NRM ? 3
                                                : (f->cnt == GX_TEX_ST ? 2 : 1);
                    bytes = f->type == GX_U8 || f->type == GX_S8   ? 1
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
                    attr_done(attr);
                }
            }
        }
        in_prim = 0;
        draw_now();
        nverts = 0;
    }
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
    gx_warn_report();
    gl13_shutdown();
}

void port_gx_frame_number(unsigned n) { (void)n; }
