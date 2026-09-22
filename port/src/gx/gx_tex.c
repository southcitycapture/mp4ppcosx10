/* Texture decode and the texture cache.
 *
 * Every GX texture is tiled and none of the ten formats the game uses is a
 * format any OpenGL 1.3 driver accepts directly, so all of them are de-tiled
 * and expanded to RGBA8 on the CPU, exactly as the two Snowboard Kids ports
 * do for the RDP's formats.  Tiles are 4x4 texels for the 16-bit formats,
 * 8x4 for the 8-bit ones, 8x8 for the 4-bit ones, and 8x8 for CMPR, whose
 * tile is a 2x2 arrangement of DXT1 blocks.
 *
 * The cache is keyed on **content**, not on the address.  That is not an
 * optimisation, it is a correctness requirement the SBK ports paid for twice:
 * Mario Party rewrites scratch textures and animated palettes in place at the
 * same address (`HuSprTexLoad` in the hilite path does exactly this), and a
 * pointer-keyed cache shows the previous frame's pixels -- which is what
 * flickering text looks like.  The key is the address, the format, the size,
 * the palette pointer and an FNV-1a hash of the texel and palette bytes.
 *
 * `GXInvalidateTexAll` is called once per frame from `HuSysBeforeRender`.  It
 * invalidates TMEM, which we do not have.  It must *not* drop this cache.
 */
#include "gx_internal.h"

#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef PORT_NO_SDL
#include <SDL_opengl.h>
#include "gx_rt.h" /* M27: every gl* below is the render thread's twin */
#endif

#define GL(fn) (port_opt.glcheck ? gl13_check(#fn) : 0, fn)

#define TEXOBJ_MAGIC 0x4D503454u /* 'MP4T' */
#define TLUT_MAGIC 0x4D50344Cu   /* 'MP4L' */

/* ---- little helpers ------------------------------------------------------- */

static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static void rgb565(u16 v, u8* out) {
    u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    out[0] = (u8)((r << 3) | (r >> 2));
    out[1] = (u8)((g << 2) | (g >> 4));
    out[2] = (u8)((b << 3) | (b >> 2));
    out[3] = 255;
}

static void rgb5a3(u16 v, u8* out) {
    if (v & 0x8000) { /* 1 rrrrr ggggg bbbbb  -- opaque */
        u32 r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        out[0] = (u8)((r << 3) | (r >> 2));
        out[1] = (u8)((g << 3) | (g >> 2));
        out[2] = (u8)((b << 3) | (b >> 2));
        out[3] = 255;
    } else { /* 0 aaa rrrr gggg bbbb */
        u32 a = (v >> 12) & 7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        out[0] = (u8)((r << 4) | r);
        out[1] = (u8)((g << 4) | g);
        out[2] = (u8)((b << 4) | b);
        out[3] = (u8)((a << 5) | (a << 2) | (a >> 1));
    }
}

static void tlut_lookup(const GXTlutObjPort* t, u32 idx, u8* out) {
    const u8* p;
    u16 v;
    if (!t || !t->lut) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 255;
        return;
    }
    p = (const u8*)t->lut + idx * 2;
    v = be16(p);
    switch (t->fmt) {
        case GX_TL_IA8: /* AAAAAAAA IIIIIIII, as the IA8 texel (M34) */
            out[0] = out[1] = out[2] = (u8)(v & 0xFF);
            out[3] = (u8)(v >> 8);
            break;
        case GX_TL_RGB565:
            rgb565(v, out);
            break;
        default:
            rgb5a3(v, out);
            break;
    }
}

/* GX's CMPR: an 8x8 tile of four DXT1 blocks in row-major order, each block
 * with big-endian colour endpoints and the two-bit selectors packed one byte
 * per row.  The three-colour mode's third colour is the midpoint and index 3
 * is transparent black, which is DXT1's own rule. */
static void cmpr_block(const u8* src, u8* out, int stride) {
    u16 c0 = be16(src), c1 = be16(src + 2);
    u8 pal[4][4];
    int y, x;
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1) {
        int i;
        for (i = 0; i < 3; i++) {
            pal[2][i] = (u8)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (u8)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        int i;
        for (i = 0; i < 3; i++) {
            pal[2][i] = (u8)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    for (y = 0; y < 4; y++) {
        u8 bits = src[4 + y];
        for (x = 0; x < 4; x++) {
            const u8* c = pal[(bits >> (6 - 2 * x)) & 3];
            u8* d = out + (size_t)y * stride + (size_t)x * 4;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
            d[3] = c[3];
        }
    }
}

/* Decode one GX texture into a freshly allocated RGBA8 buffer. */
static u8* decode(const GXTexObjPort* o, const GXTlutObjPort* tlut, int* out_w,
                  int* out_h) {
    int w = o->width, h = o->height;
    int bw, bh, x, y, tx, ty;
    const u8* src = (const u8*)o->image;
    u8* dst;
    size_t stride = (size_t)w * 4;
    if (w <= 0 || h <= 0 || !src) {
        return NULL;
    }
    dst = (u8*)calloc((size_t)w * h, 4);
    if (!dst) {
        return NULL;
    }
    *out_w = w;
    *out_h = h;

    switch (o->format) {
        case GX_TF_I4:
        case GX_TF_C4:
            bw = 8;
            bh = 8;
            for (ty = 0; ty < h; ty += bh) {
                for (tx = 0; tx < w; tx += bw) {
                    for (y = 0; y < bh; y++) {
                        for (x = 0; x < bw; x += 2) {
                            u8 b = *src++;
                            int k;
                            for (k = 0; k < 2; k++) {
                                u32 v = k ? (b & 0xF) : (b >> 4);
                                int px = tx + x + k, py = ty + y;
                                u8* d;
                                if (px >= w || py >= h) {
                                    continue;
                                }
                                d = dst + (size_t)py * stride + (size_t)px * 4;
                                if (o->format == GX_TF_C4) {
                                    tlut_lookup(tlut, v, d);
                                } else {
                                    u8 i8 = (u8)((v << 4) | v);
                                    d[0] = d[1] = d[2] = d[3] = i8;
                                }
                            }
                        }
                    }
                }
            }
            break;

        case GX_TF_I8:
        case GX_TF_C8:
        case GX_TF_IA4:
        case GX_TF_A8:
            bw = 8;
            bh = 4;
            for (ty = 0; ty < h; ty += bh) {
                for (tx = 0; tx < w; tx += bw) {
                    for (y = 0; y < bh; y++) {
                        for (x = 0; x < bw; x++) {
                            u8 v = *src++;
                            int px = tx + x, py = ty + y;
                            u8* d;
                            if (px >= w || py >= h) {
                                continue;
                            }
                            d = dst + (size_t)py * stride + (size_t)px * 4;
                            if (o->format == GX_TF_C8) {
                                tlut_lookup(tlut, v, d);
                            } else if (o->format == GX_TF_IA4) {
                                u8 i = (u8)((v & 0xF) << 4 | (v & 0xF));
                                u8 a = (u8)((v >> 4) << 4 | (v >> 4));
                                d[0] = d[1] = d[2] = i;
                                d[3] = a;
                            } else if (o->format == GX_TF_A8) {
                                d[0] = d[1] = d[2] = 255;
                                d[3] = v;
                            } else {
                                /* An intensity texel is (I,I,I,I), not
                                 * (I,I,I,1) -- the SBK ports got this wrong
                                 * once and every I8 sprite came out opaque. */
                                d[0] = d[1] = d[2] = d[3] = v;
                            }
                        }
                    }
                }
            }
            break;

        case GX_TF_IA8:
        case GX_TF_RGB565:
        case GX_TF_RGB5A3:
        case GX_TF_C14X2:
            bw = 4;
            bh = 4;
            for (ty = 0; ty < h; ty += bh) {
                for (tx = 0; tx < w; tx += bw) {
                    for (y = 0; y < bh; y++) {
                        for (x = 0; x < bw; x++) {
                            u16 v = be16(src);
                            int px = tx + x, py = ty + y;
                            src += 2;
                            if (px >= w || py >= h) {
                                continue;
                            }
                            {
                                u8* d = dst + (size_t)py * stride + (size_t)px * 4;
                                if (o->format == GX_TF_IA8) {
                                    /* M34: an IA8 texel is AAAAAAAA IIIIIIII -- the
                                     * alpha is the high byte, as IA4's is the high
                                     * nibble.  The port read it the other way round
                                     * since M3, which is why m435's flare passed its
                                     * alpha test only under the star (PLAN.md 49.2)
                                     * and m408's plug drew its intensity as alpha. */
                                    d[0] = d[1] = d[2] = (u8)(v & 0xFF);
                                    d[3] = (u8)(v >> 8);
                                } else if (o->format == GX_TF_RGB565) {
                                    rgb565(v, d);
                                } else if (o->format == GX_TF_RGB5A3) {
                                    rgb5a3(v, d);
                                } else {
                                    tlut_lookup(tlut, v & 0x3FFF, d);
                                }
                            }
                        }
                    }
                }
            }
            break;

        case GX_TF_RGBA8:
            /* a 4x4 tile is two 32-byte halves: sixteen AR pairs then sixteen
             * GB pairs */
            bw = 4;
            bh = 4;
            for (ty = 0; ty < h; ty += bh) {
                for (tx = 0; tx < w; tx += bw) {
                    for (y = 0; y < bh; y++) {
                        for (x = 0; x < bw; x++) {
                            int px = tx + x, py = ty + y;
                            const u8* ar = src + ((size_t)y * 4 + x) * 2;
                            const u8* gb = src + 32 + ((size_t)y * 4 + x) * 2;
                            if (px >= w || py >= h) {
                                continue;
                            }
                            {
                                u8* d = dst + (size_t)py * stride + (size_t)px * 4;
                                d[3] = ar[0];
                                d[0] = ar[1];
                                d[1] = gb[0];
                                d[2] = gb[1];
                            }
                        }
                    }
                    src += 64;
                }
            }
            break;

        case GX_TF_CMPR:
            for (ty = 0; ty < h; ty += 8) {
                for (tx = 0; tx < w; tx += 8) {
                    int by, bx;
                    for (by = 0; by < 8; by += 4) {
                        for (bx = 0; bx < 8; bx += 4) {
                            u8 tmp[4 * 4 * 4];
                            int yy, xx;
                            cmpr_block(src, tmp, 16);
                            src += 8;
                            for (yy = 0; yy < 4; yy++) {
                                for (xx = 0; xx < 4; xx++) {
                                    int px = tx + bx + xx, py = ty + by + yy;
                                    if (px >= w || py >= h) {
                                        continue;
                                    }
                                    memcpy(dst + (size_t)py * stride + (size_t)px * 4,
                                           tmp + (size_t)yy * 16 + (size_t)xx * 4, 4);
                                }
                            }
                        }
                    }
                }
            }
            break;

        default:
            gx_warn("texture: an undecoded GX format was bound (drawn white)");
            memset(dst, 0xFF, (size_t)w * h * 4);
            break;
    }
    return dst;
}

/* how many bytes the encoded texture occupies, for the content hash */
static size_t encoded_size(u32 fmt, int w, int h) {
    switch (fmt) {
        case GX_TF_I4:
        case GX_TF_C4:
        case GX_TF_CMPR:
            return (size_t)((w + 7) / 8 * 8) * ((h + 7) / 8 * 8) / 2;
        case GX_TF_I8:
        case GX_TF_C8:
        case GX_TF_IA4:
        case GX_TF_A8:
            return (size_t)((w + 7) / 8 * 8) * ((h + 3) / 4 * 4);
        case GX_TF_IA8:
        case GX_TF_RGB565:
        case GX_TF_RGB5A3:
        case GX_TF_C14X2:
            return (size_t)((w + 3) / 4 * 4) * ((h + 3) / 4 * 4) * 2;
        case GX_TF_RGBA8:
            return (size_t)((w + 3) / 4 * 4) * ((h + 3) / 4 * 4) * 4;
        default:
            return 0;
    }
}

static u32 fnv(const void* p, size_t n, u32 h) {
    const u8* b = (const u8*)p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

/* ---- the cache ------------------------------------------------------------ */

u8 gx_unit_alpha_min[8] = { 255, 255, 255, 255, 255, 255, 255, 255 }; /* M33: per texture unit at bind */

typedef struct CacheEntry {
    const void* image;
    const void* lut;
    u32 format;
    u16 w, h;
    u32 content;
    /* M23: the exhaustive first-sight hash of the source bytes, the key a
     * re-key by content matches on (cache_find_by_content); 0 once the
     * bytes were rewritten in place under the key. `content` itself is the
     * hash the per-epoch revalidation compares -- sampled for anything
     * over TEX_HASH_SAMPLE -- which until M23 was the exhaustive one on a
     * miss, so the next epoch's sampled hash never matched and every
     * texture over a kilobyte was decoded and uploaded twice. */
    u32 content_full;
    unsigned gl_name;
    u8 wrap_s, wrap_t, min_filt, mag_filt;
    u8 alpha_min;  /* M33: the smallest alpha the decode produced (255 for an
                    * opaque format) -- whether an alpha test can kill anything */
    /* GXSetTevSwapModeTable, packed two bits per output channel (see
     * GX_SWAP_IDENTITY).  It is part of the key, not of the bind: two stages
     * can sample the same texels through two different swap tables in the
     * same frame, and each wants its own re-encoded copy. */
    u8 swap;
    int used;
    /* The Radeon 9000 has no ARB_texture_non_power_of_two, so an NPOT texture
     * is uploaded into the next power of two up and these are the fractions of
     * it that hold real texels.  1.0 for the overwhelmingly common POT case. */
    float su, sv;
    /* glTexParameter belongs to the texture object, so what GL already holds
     * for this name is remembered here rather than in gl13.c's per-unit
     * shadow.  -1 is "unknown", which is what a fresh glGenTextures name and
     * a re-upload both leave behind. */
    int param_wrap_s, param_wrap_t, param_min, param_mag;
    /* An EFB copy: the texels never pass through main memory, so there is
     * nothing at `image` to decode or to hash.  The entry owns a GL texture
     * that GXCopyTex refills from the back buffer, and a bind uses it as it
     * stands. */
    int efb;
    /* M23: an EFB copy with the half-scale box filter (GXSetTexCopyDst's
     * mipmap flag; every shadow map, hsfman.c:2001) keeps the *source*
     * rectangle's texels, `copy_w` x `copy_h` of them, in a texture padded
     * from that size; `w`/`h` stay the destination the game named, so a
     * bind's 0..1 lands on the whole region through su/sv and a read-back
     * box-filters 2x2 down to what the copy unit would have written. */
    u16 copy_w, copy_h;
    u8 copy_half;
    /* M23: once the game has read this copy back (port_gx_copy_read), every
     * later copy also reads itself back at the destination size through
     * gl13_downsample_read into `cpu_rgba` (w*h*4 bytes), so the game's
     * read costs no GL at all and the bus carries a seventh of the bytes. */
    u8* cpu_rgba;
    u8 cpu_read;
    unsigned cpu_read_at; /* stat_efb at the game's last read: the read-back stops
                           * a few copies after the game stops asking (the intro
                           * reads every frame; the game after it never does) */
    /* O(1) lookup: every entry (efb or not) chains off a bucket of
     * `hash_head[]` keyed on `image` alone -- see `find_slot()`.  -1 ends a
     * chain. */
    int hash_next;
    /* The validation epoch (see below) this slot's `content` was last
     * verified against the real texel bytes.  A hit whose slot is already
     * current for this epoch costs a hash-table lookup and nothing else: no
     * FNV pass at all. */
    unsigned validated_epoch;
    /* bytes GL holds for this entry (the padded RGBA8 upload), so the cache's
     * resident size is a number and not a guess (M18: the 45-minute soak
     * slowed from 99% to 80% speed while a fresh process at the same frame
     * ran at 100%; the first suspect is what the long process accumulates) */
    unsigned gl_bytes;
    unsigned last_used;   /* the frame that last bound it (the budget's LRU) */
    /* M35 (PLAN.md 50): the game's own "I wrote these texels" signal.  The
     * console's GP reads main memory, so a CPU-written texture has to be
     * DCStoreRange'd / DCFlushRange'd before it is drawn, and the port's
     * DC* bodies (os_misc.c) hand that range here: every entry whose bytes
     * it overlaps is marked, and its next bind hashes the *whole* image
     * rather than the four sampled windows -- which is how Stamp Out!'s
     * stamps (fifty-texel squares the CPU paints into a 600x600 RGB5A3
     * canvas, m415Dll/main.c:1A60) were never seen at all. */
    u8 dirty;
    u32 enc_size;         /* encoded_size(format, w, h) at the last decode */
} CacheEntry;

#define CACHE_MAX 2048
static CacheEntry cache[CACHE_MAX];
static int cache_used;
static size_t cache_gl_bytes; /* sum of cache[].gl_bytes: what the driver holds */
static unsigned stat_dirty_calls, stat_dirty_marks, stat_dirty_clean, stat_dirty_redecode;

/* M35: DCStoreRange / DCFlushRange (os_misc.c) land here with the range the
 * game just wrote.  Every non-copy entry whose encoded bytes overlap it is
 * marked dirty; the next bind of a dirty entry hashes its whole image. */
void port_gx_tex_dirty(const void* addr, unsigned long n) {
    const u8* a = (const u8*)addr;
    int i;
    if (port_opt.nodirty || !a || n == 0) {
        return;
    }
    stat_dirty_calls++;
    for (i = 0; i < cache_used; i++) {
        CacheEntry* e = &cache[i];
        const u8* im = (const u8*)e->image;
        if (e->efb || !im || !e->enc_size || e->dirty) {
            continue;
        }
        if (im < a + n && a < im + e->enc_size) {
            e->dirty = 1;
            stat_dirty_marks++;
        }
    }
}
/* ---- the VRAM budget (M18, PLAN.md 33.0) -----------------------------------
 *
 * The cache was bounded by *entries* (2048, random replacement when full) and
 * not by bytes.  The M17 overnight soak slowed from 99% real-time speed at
 * turn 1 to 80% at turn 12, while a fresh process restored at the same frame
 * ran at 100%; the instrumented re-run showed the cache holding 130-142 MB of
 * GL textures on a card with 64 MB, from frame 60,000 on.  Past the card's
 * memory the driver pages textures over AGP every frame, and that is the
 * slowdown.  So the cache now has a byte budget: when an upload takes it over,
 * the entries that have not been bound for the longest go first, never one
 * bound this frame.  `--texbudget MB` (0 = the pre-M18 behaviour). */
static void hash_remove(int slot);
/* M24 (below): the decode on the second core */
typedef struct Staged Staged;
static int predecode_on(void);
static void predecode_request(const GXTexObjPort* o);
static Staged* predecode_take(const GXTexObjPort* o, const void* lut, u32 content_full);
static void tex_bind_upload_staged(int slot, int unit, const GXTexObjPort* o, Staged* e);
static unsigned frame_pre_unstaged;
static size_t tex_budget_bytes = (size_t)40 << 20;
static int free_slots[CACHE_MAX];
static int nfree_slots;
static unsigned stat_budget_evict;
static size_t stat_budget_evict_bytes;
static unsigned cache_frame;  /* the frame gx_tex_bind last saw */

void gx_tex_set_budget_mb(int mb) { tex_budget_bytes = (size_t)(mb > 0 ? mb : 0) << 20; }
int gx_tex_budget_mb(void) { return (int)(tex_budget_bytes >> 20); } /* M32: for --defaults */

static void cache_free_slot(int slot) {
    hash_remove(slot);
    free(cache[slot].cpu_rgba);
    if (gl13_live() && cache[slot].gl_name) {
        GLuint n = cache[slot].gl_name;
        GL(glDeleteTextures)(1, &n);
    }
    cache_gl_bytes -= cache[slot].gl_bytes;
    memset(&cache[slot], 0, sizeof(cache[slot]));
    cache[slot].hash_next = -1;
    free_slots[nfree_slots++] = slot;
}

/* Evict the least recently bound entries until the GL-resident bytes are
 * under budget.  An entry bound in this frame or the last is never taken:
 * the frame mode draws every other retrace, so "last frame" is the frame the
 * game built and the card did not see. */
static void cache_evict_to_budget(void) {
    int guard = 0;
    if (!tex_budget_bytes) {
        return;
    }
    while (cache_gl_bytes > tex_budget_bytes && guard++ < CACHE_MAX) {
        int i, oldest = -1;
        for (i = 0; i < cache_used; i++) {
            const CacheEntry* e = &cache[i];
            if (!e->gl_name || e->image == NULL) {
                continue;
            }
            if (e->last_used + 1 >= cache_frame) {
                continue;
            }
            if (oldest < 0 || e->last_used < cache[oldest].last_used) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            return; /* everything resident was bound this frame or the last */
        }
        stat_budget_evict++;
        stat_budget_evict_bytes += cache[oldest].gl_bytes;
        cache_free_slot(oldest);
    }
}
static unsigned stat_hit, stat_miss, stat_evict, stat_bytes, stat_npot;
static unsigned stat_hash_full, stat_hash_sampled;
static unsigned stat_efb;
static unsigned stat_copy_read, stat_copy_read_miss; /* port_gx_copy_read */
static unsigned stat_copy_read_gpu, stat_copy_read_tex; /* M23: read at the copy / on demand */
static double stat_copy_read_gpu_s, stat_copy_read_tex_s;
static unsigned stat_copy_front, stat_copy_region_front;
static unsigned stat_copy_kept; /* M21: clear-after copies on consumed frames, kept */
static unsigned stat_copy_half; /* M23: half-scale (box-filtered) copies, kept at source size */
/* How many times an already-cached slot's content hash was actually
 * recomputed to check for an in-place rewrite -- as opposed to a pure
 * epoch-cached hit, which touches none of the texel bytes at all.  This is
 * the number the per-frame epoch is supposed to shrink. */
static unsigned stat_revalidate;
/* M23 (PLAN.md 38): what a frame's cold decode costs, per frame and in
 * total.  The scene-change audio underruns (§32.5) are the first drawn
 * frame of a scene decoding every texture it binds; these say how many,
 * how many bytes, and how the time splits between the decode and the
 * upload.  `frame_*` are reset by gx_tex_frame_decode_take() once a frame. */
static unsigned frame_decodes, frame_src_bytes, frame_rgba_bytes, frame_rekeys;
static double frame_decode_s, frame_upload_s, frame_hash_s;
static unsigned stat_decodes, stat_rekeys;
static unsigned long stat_rekey_bytes;
static double stat_decode_s, stat_upload_s, stat_hash_s;
static unsigned stat_frames_over20;

/* `--texvalidate-every-bind`: keep the validation epoch out of the decision
 * (every bind re-checks content) but leave the sampled-vs-full hash choice
 * alone.  This isolates the epoch from the sampling for bisection, the way
 * `--texhash-full` isolates the sampling from the epoch. */
static int validate_every_bind;
void gx_tex_set_validate_every_bind(int v) { validate_every_bind = v; }

/* ---- the O(1) index: a hash table on `image`, chained through the cache
 * array itself ---------------------------------------------------------- */

#define TEX_HASH_BITS 12
#define TEX_HASH_SIZE (1u << TEX_HASH_BITS)
#define TEX_HASH_MASK (TEX_HASH_SIZE - 1u)
static int hash_head[TEX_HASH_SIZE];

static unsigned hash_key(const void* image) {
    uintptr_t p = (uintptr_t)image;
    /* A texture buffer's address is usually 4/8/16-byte aligned, so the low
     * bits alone are a poor key; fold the whole pointer through a
     * multiplicative mix before masking down to the table size.  Two
     * constants because `uintptr_t` is 64 bits on the development Mac and 32
     * on the G4, and a 64-bit literal truncated into a 32-bit multiply is a
     * compiler warning waiting to happen. */
#if UINTPTR_MAX > 0xFFFFFFFFu
    p ^= p >> 15;
    p *= (uintptr_t)0x2545F4914F6CDD1DULL; /* splitmix64's finalizer */
    p ^= p >> 13;
#else
    p ^= p >> 15;
    p *= (uintptr_t)0x85EBCA6Bu; /* murmur3's finalizer */
    p ^= p >> 13;
#endif
    return (unsigned)p & TEX_HASH_MASK;
}

static void hash_insert(int slot) {
    unsigned h = hash_key(cache[slot].image);
    cache[slot].hash_next = hash_head[h];
    hash_head[h] = slot;
}

/* Unlink `slot` from whichever bucket its *current* `image` chains through.
 * Must run before the slot's `image` is overwritten (an eviction reusing the
 * slot for a different key). */
static void hash_remove(int slot) {
    unsigned h = hash_key(cache[slot].image);
    int* pp = &hash_head[h];
    while (*pp >= 0) {
        if (*pp == slot) {
            *pp = cache[slot].hash_next;
            return;
        }
        pp = &cache[*pp].hash_next;
    }
}

/* The single O(1) replacement for both of the old O(cache_used) scans: the
 * EFB-copy scan (matched on `image` alone, and given priority, exactly as
 * the two separate loops used to -- an EFB entry at this address always wins
 * over a decoded one) and the regular (image, format, w, h, lut) scan.
 * `*is_efb` reports which kind was found. */
static int find_slot(const void* image, u32 format, u16 w, u16 h, const void* lut,
                      u8 swap, int* is_efb) {
    unsigned h0 = hash_key(image);
    int i, regular = -1;
    for (i = hash_head[h0]; i >= 0; i = cache[i].hash_next) {
        CacheEntry* e = &cache[i];
        if (e->image != image) {
            continue;
        }
        if (e->efb) {
            *is_efb = 1;
            return i;
        }
        if (regular < 0 && e->format == format && e->w == w && e->h == h &&
            e->lut == lut && e->swap == swap) {
            regular = i;
        }
    }
    *is_efb = 0;
    return regular;
}

/* The validation epoch: bumped the first time any bind observes a new frame
 * number.  A cache slot's content hash is only ever recomputed against the
 * real texel bytes once per epoch; every other bind of the same slot inside
 * that epoch is a hash-table lookup and a bind, nothing else.  See the long
 * comment above `gx_tex_bind`'s epoch-bump line for why the frame counter is
 * the right signal and `GXInvalidateTexAll`/`DCFlushRange` are not. */
static unsigned cache_epoch;
static unsigned cache_epoch_frame;
static int cache_epoch_started;
unsigned gl13_frame_number(void);

void gx_tex_init(void) {
    int i;
    memset(cache, 0, sizeof(cache));
    cache_used = 0;
    for (i = 0; i < (int)TEX_HASH_SIZE; i++) {
        hash_head[i] = -1;
    }
    cache_epoch = 0;
    cache_epoch_frame = 0;
    cache_epoch_started = 0;
}

/* the cache's resident size, for the status line (M18) */
void gx_tex_cache_stats(unsigned* entries, unsigned* kb) {
    *entries = (unsigned)cache_used;
    *kb = (unsigned)(cache_gl_bytes / 1024);
}

void gx_tex_predecode_report(void);
void gx_tex_report(void) {
    gx_tex_predecode_report();
    if (!stat_hit && !stat_miss) {
        return;
    }
    port_log("port> texture cache: %u hits, %u misses, %u re-uploads, %u entries, "
             "%u KB decoded, %u padded to a power of two, %u KB held by GL\n",
             stat_hit, stat_miss, stat_evict, (unsigned)cache_used, stat_bytes / 1024,
             stat_npot, (unsigned)(cache_gl_bytes / 1024));
    port_log("port> texture budget: %u MB; %u evictions (%u KB) to stay under it\n",
             (unsigned)(tex_budget_bytes >> 20), stat_budget_evict,
             (unsigned)(stat_budget_evict_bytes / 1024));
    port_log("port> texture decode (M23): %u decodes, %.0f ms decoding, %.0f ms uploading, "
             "%.0f ms hashing; %u frames over 20 ms of it; %u re-keyed by content "
             "(%lu KB not decoded again)%s\n",
             stat_decodes, stat_decode_s * 1000.0, stat_upload_s * 1000.0, stat_hash_s * 1000.0,
             stat_frames_over20, stat_rekeys, stat_rekey_bytes / 1024,
             port_opt.norekey ? " (--norekey)" : "");
    if (stat_dirty_calls) {
        port_log("port> texture dirty ranges (M35): %u DC store/flush calls, %u entries marked, "
                 "%u hashed clean, %u decoded again%s\n", stat_dirty_calls, stat_dirty_marks,
                 stat_dirty_clean, stat_dirty_redecode, port_opt.nodirty ? " (--nodirty)" : "");
    }
    port_log("port> texture hash: %u KB hashed in full, %u KB sampled, %u "
             "revalidations (of %u binds)%s%s\n",
             stat_hash_full / 1024, stat_hash_sampled / 1024, stat_revalidate,
             stat_hit + stat_miss + stat_evict,
             port_opt.texhash_full ? " (--texhash-full: epoch+sampling bypassed)" : "",
             validate_every_bind ? " (--texvalidate-every-bind: epoch bypassed)" : "");
    if (stat_efb) {
        port_log("port> EFB copies: %u colour copies into the cache (on consumed frames "
                 "from the front buffer: %u whole-screen, %u region; %u clear-after copies "
                 "kept from the last drawn frame, M21; %u half-scale copies kept at "
                 "source size, M23)\n",
                 stat_efb, stat_copy_front, stat_copy_region_front, stat_copy_kept,
                 stat_copy_half);
    }
    if (stat_copy_read || stat_copy_read_miss) {
        port_log("port> copy-read: %u copies read back by the game (m415's canvas), "
                 "%u answered with zeros; %u read at the copy through the back buffer "
                 "(%.0f ms), %u drawn and read on demand (%.0f ms) (M23)\n",
                 stat_copy_read, stat_copy_read_miss, stat_copy_read_gpu,
                 stat_copy_read_gpu_s * 1000.0, stat_copy_read_tex, stat_copy_read_tex_s * 1000.0);
    }
}

static GLenum gl_wrap(u8 w) {
    switch (w) {
        case GX_CLAMP: return GL_CLAMP_TO_EDGE;
        case GX_MIRROR: return GL_MIRRORED_REPEAT;
        default: return GL_REPEAT;
    }
}

static GLenum gl_filter(u8 f, int is_min) {
    switch (f) {
        case GX_NEAR: return GL_NEAREST;
        case GX_LINEAR: return GL_LINEAR;
        case GX_NEAR_MIP_NEAR: return is_min ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        case GX_LIN_MIP_NEAR: return is_min ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        case GX_NEAR_MIP_LIN: return is_min ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
        default: return is_min ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    }
}

/* How much of a large texture the sampled hash looks at, in bytes. */
/* The sampled content hash's budget, in bytes.  M2b set it at 4096 and M3's
 * profile put `gx_tex_bind` at 512 of 4,200 in-thread samples on the G4 --
 * 427 binds a frame times four kilobytes of FNV is four megabytes a frame of
 * pure pointer-chasing on a 1 GHz machine.  1024 is still a header, a tail and
 * a spread of interior points, still catches every in-place rewrite the boot
 * and the menus do, and costs a quarter as much.  `--texhash-full` is how to
 * prove a suspected staleness bug is or is not this. */
#define TEX_HASH_SAMPLE 1024

/* M5b: "has this buffer ever been hashed in full" used to be a linear scan
 * over up to 4096 remembered pointers (`seen_before()`), run on every bind of
 * anything bigger than TEX_HASH_SAMPLE.  It is now free: `slot < 0` at the
 * point a content hash is computed already means "no cache entry for this
 * (image, format, w, h, lut) key exists yet", which is exactly the condition
 * `seen_before()` was approximating -- and is in fact a tighter one, because
 * it is keyed on the whole cache key rather than on the raw pointer alone (a
 * buffer address reused later for a different format/size now gets its own
 * exhaustive first-sight hash instead of inheriting an unrelated sighting).
 * See `tex_bind_content_hash()`'s `first_sight` parameter. */

/* The validation epoch.  A cache slot's content hash is only ever recomputed
 * against the real texel bytes once per epoch; every other bind of the same
 * slot inside that epoch is a hash-table lookup and a bind, nothing else.
 *
 * The signal has to be both cheap and conservative -- never so coarse that an
 * in-place rewrite goes unnoticed for more than one epoch's worth of frames.
 * `GXInvalidateTexAll` cannot be it: the comment above and PLAN.md §3.6 both
 * say `HuSprDispInit` calls it once *per sprite pass*, several times a frame,
 * so treating it as the epoch boundary would mean re-validating everything
 * several times a frame -- no better than the old per-bind hash.
 * `DCFlushRange` is a no-op in this port (`port/src/os/os_misc.c`) and out of
 * this file's lane besides.  What is left, and is exactly right: the frame
 * counter `gl13.c` already keeps for `--drawlog-at`/`--scenelog`
 * (`gl13_frame_number()`).  The game's draw order inside a frame is what it
 * is regardless of how many invalidate-alls it issues, so "revalidate a slot
 * at most once per frame, the first time it is bound that frame" is exact:
 * every content change the sampled hash can see at all is caught the very
 * next time the changed texture is bound, which is always within the same or
 * the following frame relative to when the game wrote it. */

static int pot(int v) { return v > 0 && (v & (v - 1)) == 0; }

static int pot_up(int v) {
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/* Copy a decoded RGBA8 image into the next power of two up, replicating the
 * last row and column across the padding.  GX's texture unit takes the real
 * width and height and pads to its own tile size in hardware; GL 1.3 without
 * NPOT cannot, and an NPOT glTexImage2D makes the texture *incomplete*, which
 * silently disables texturing for that unit -- the failure looks like a
 * lighting bug thirty draws later, which is exactly how this one presented.
 * Edge replication rather than zero fill is what keeps GX_CLAMP and GX_LINEAR
 * from fringing along the two padded edges. */
static u8* pad_to_pot(const u8* src, int w, int h, int pw, int ph) {
    u8* dst = (u8*)malloc((size_t)pw * ph * 4);
    int y, x;
    if (!dst) {
        return NULL;
    }
    for (y = 0; y < ph; y++) {
        const u8* srow = src + (size_t)(y < h ? y : h - 1) * w * 4;
        u8* drow = dst + (size_t)y * pw * 4;
        memcpy(drow, srow, (size_t)w * 4);
        for (x = w; x < pw; x++) {
            memcpy(drow + (size_t)x * 4, srow + (size_t)(w - 1) * 4, 4);
        }
    }
    return dst;
}

/* The content hash: the same FNV walk as before the M5b rework, factored out
 * so both the miss path and the epoch-triggered revalidation path share it.
 * `first_sight` forces the exhaustive hash regardless of size, exactly as
 * `!seen_before(o->image)` used to -- see the note above `TEX_HASH_SAMPLE`.
 * `--texhash-full` still forces it unconditionally, on every call. */
static u32 tex_bind_content_hash_body(const GXTexObjPort* o, const GXTlutObjPort* tlut,
                                       int first_sight);
static u32 tex_bind_content_hash(const GXTexObjPort* o, const GXTlutObjPort* tlut,
                                  int first_sight) {
    double t0 = port_now_seconds();
    u32 c = tex_bind_content_hash_body(o, tlut, first_sight);
    frame_hash_s += port_now_seconds() - t0;
    return c;
}
static u32 tex_bind_content_hash_body(const GXTexObjPort* o, const GXTlutObjPort* tlut,
                                       int first_sight) {
    u32 content = fnv(&o->format, sizeof(o->format), 2166136261u);
    content = fnv(&o->width, sizeof(o->width), content);
    content = fnv(&o->height, sizeof(o->height), content);
    if (o->image) {
        size_t n = encoded_size(o->format, o->width, o->height);
        if (port_opt.texhash_full || first_sight || n <= TEX_HASH_SAMPLE) {
            content = fnv(o->image, n, content);
            stat_hash_full += (unsigned)n;
        } else {
            const u8* q = (const u8*)o->image;
            size_t chunk = TEX_HASH_SAMPLE / 4;
            size_t step = (n - chunk) / 3;
            int k;
            content = fnv(&n, sizeof(n), content);
            for (k = 0; k < 4; k++) {
                size_t at = (size_t)k * step;
                if (at + chunk > n) {
                    at = n - chunk;
                }
                content = fnv(q + at, chunk, content);
            }
            stat_hash_sampled += (unsigned)(chunk * 4);
        }
    }
    if (tlut && tlut->lut) {
        content = fnv(tlut->lut, (size_t)tlut->n * 2, content);
    }
    return content;
}

/* The identity swap table, packed: red->red, green->green, blue->blue,
 * alpha->alpha. */
#define GX_SWAP_IDENTITY ((u8)(0 | (1 << 2) | (2 << 4) | (3 << 6)))
static void swizzle_rgba(u8* rgba, int w, int h, u8 swap);

/* Decode fresh texels into `slot` and upload them: shared by a genuine miss
 * (a brand-new cache key) and a revalidation that found the bytes changed
 * under an existing key (an in-place rewrite, the old `stat_evict` case).
 * Everything about the slot except `content` (set by the caller) and the key
 * fields (already correct, either just-assigned or unchanged) is written
 * here. */
static void tex_bind_decode_and_upload(int slot, int unit, const GXTexObjPort* o,
                                        const GXTlutObjPort* tlut) {
    int w = 0, h = 0;
    double t0 = port_now_seconds(), t1;
    u8* rgba = decode(o, tlut, &w, &h);
    cache[slot].su = cache[slot].sv = 1.0f;
    cache[slot].alpha_min = 255;
    if (rgba) {
        size_t k, n = (size_t)w * h;
        u8 amin = 255;
        swizzle_rgba(rgba, w, h, cache[slot].swap);
        for (k = 0; k < n && amin; k++) {
            if (rgba[k * 4 + 3] < amin) {
                amin = rgba[k * 4 + 3];
            }
        }
        cache[slot].alpha_min = amin;
    }
    t1 = port_now_seconds();
    frame_decodes++;
    stat_decodes++;
    frame_decode_s += t1 - t0;
    frame_src_bytes += (unsigned)encoded_size(o->format, o->width, o->height);
    if (rgba) {
        frame_rgba_bytes += (unsigned)(w * h * 4);
    }
    /* --dumptex: every texture the decoder produces, as it produced it,
     * written out the first time it is decoded.  "The draw is right and
     * the screen is black" is nearly always the texture, and looking at
     * the texture is much faster than reasoning about the format. */
    if (rgba && port_opt.dumptex) {
        char path[1024];
        FILE* f;
        snprintf(path, sizeof(path), "%s/tex-%03d-%dx%d-fmt%u%s.ppm",
                 port_opt.shotdir ? port_opt.shotdir : ".", slot, w, h,
                 (unsigned)o->format, o->is_ci ? "-ci" : "");
        f = fopen(path, "wb");
        if (f) {
            int yy, xx;
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (yy = 0; yy < h; yy++) {
                for (xx = 0; xx < w; xx++) {
                    fwrite(rgba + ((size_t)yy * w + xx) * 4, 1, 3, f);
                }
            }
            fclose(f);
            port_log("port> --dumptex: wrote %s\n", path);
        }
        /* and the alpha, which is what an alpha test actually judges */
        snprintf(path, sizeof(path), "%s/tex-%03d-%dx%d-fmt%u%s-alpha.pgm",
                 port_opt.shotdir ? port_opt.shotdir : ".", slot, w, h,
                 (unsigned)o->format, o->is_ci ? "-ci" : "");
        f = fopen(path, "wb");
        if (f) {
            int yy, xx;
            fprintf(f, "P5\n%d %d\n255\n", w, h);
            for (yy = 0; yy < h; yy++) {
                for (xx = 0; xx < w; xx++) {
                    fwrite(rgba + ((size_t)yy * w + xx) * 4 + 3, 1, 1, f);
                }
            }
            fclose(f);
        }
    }
    if (rgba) {
        int pw = pot_up(w), ph = pot_up(h);
        u8* up = rgba;
        stat_bytes += (unsigned)(w * h * 4);
        if (pw != w || ph != h) {
            u8* padded = pad_to_pot(rgba, w, h, pw, ph);
            if (padded) {
                up = padded;
                cache[slot].su = (float)w / (float)pw;
                cache[slot].sv = (float)h / (float)ph;
                stat_npot++;
            } else {
                pw = w;
                ph = h;
            }
        }
        if (gl13_live()) {
            GLuint name = cache[slot].gl_name;
            if (!name) {
                GL(glGenTextures)(1, &name);
                cache[slot].gl_name = name;
            }
            /* An upload has to bind the name it is about to fill, and
             * it does that on whichever unit is current; tell the shadow
             * rather than let it guess.  `unit` is where this bind is
             * headed anyway, so the bind below usually elides. */
            glc_active_texture(unit);
            if (gl13_trace_armed()) {
                port_log("gltrace> upload unit %d name %u %dx%d img %p\n", unit, name, pw, ph,
                         o->image);
            }
            GL(glBindTexture)(GL_TEXTURE_2D, name);
            glc_note_bind(unit, name);
            /* M27: the upload owns the texels from here -- the render thread
             * frees them after the call (direct mode: the call, then free) */
            (port_opt.glcheck ? gl13_check("glTexImage2D") : 0);
            rt_teximage2d_owned(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, up);
            if (up == rgba) {
                rgba = NULL;
            }
            up = NULL;
            /* A fresh name has the GL default filter state, which is
             * mipmapped and therefore incomplete here; force the
             * parameters to be re-emitted for it. */
            cache[slot].param_wrap_s = -1;
            cache_gl_bytes -= cache[slot].gl_bytes;
            cache[slot].gl_bytes = (unsigned)(pw * ph * 4);
            cache_gl_bytes += cache[slot].gl_bytes;
            frame_upload_s += port_now_seconds() - t1;
        }
        if (up && up != rgba) {
            free(up);
        }
        free(rgba);
    }
}

/* M23: the frame's decode accounting, taken (and reset) by the perf frame
 * hook so the stall line can say what the frame spent on textures. */
void gx_tex_frame_decode_take(unsigned* n, unsigned* src_kb, unsigned* rgba_kb,
                              double* decode_ms, double* upload_ms, double* hash_ms,
                              unsigned* rekeys) {
    *n = frame_decodes;
    *src_kb = frame_src_bytes / 1024;
    *rgba_kb = frame_rgba_bytes / 1024;
    *decode_ms = frame_decode_s * 1000.0;
    *upload_ms = frame_upload_s * 1000.0;
    *hash_ms = frame_hash_s * 1000.0;
    *rekeys = frame_rekeys;
    stat_decode_s += frame_decode_s;
    stat_upload_s += frame_upload_s;
    stat_hash_s += frame_hash_s;
    if (frame_decode_s + frame_upload_s > 0.020) {
        stat_frames_over20++;
    }
    frame_decodes = frame_src_bytes = frame_rgba_bytes = frame_rekeys = 0;
    frame_decode_s = frame_upload_s = frame_hash_s = 0.0;
}

/* M23: a texture the cache already holds under another address.  The game
 * frees a scene's textures and the next scene's land elsewhere, so the
 * board's textures come back from every minigame at new addresses and
 * were decoded and uploaded again each time (the board-load resyncs of
 * §32.1).  The cache is content-hashed already; on a miss whose exhaustive
 * first-sight hash matches an entry that is not live (not bound this frame
 * or the last -- a live duplicate at another address keeps its own copy,
 * or two addresses would trade one entry back and forth), the entry is
 * re-keyed to the new address: no decode, no upload.  `--norekey` is the
 * pre-M23 miss. */
static int cache_find_by_content(const GXTexObjPort* o, u32 content_full, u8 swap,
                                 const void* lut, unsigned frame) {
    int i;
    if (port_opt.norekey) {
        return -1;
    }
    for (i = 0; i < cache_used; i++) {
        const CacheEntry* e = &cache[i];
        if (e->efb || !e->gl_name || e->image == NULL || !e->content_full ||
            e->content_full != content_full) {
            continue;
        }
        if (e->format != o->format || e->w != o->width || e->h != o->height ||
            e->swap != swap) {
            continue;
        }
        if (e->image == o->image && e->lut == lut) {
            continue; /* that is a hit, not a re-key; find_slot would have found it */
        }
        if (e->last_used + 1 >= frame) {
            continue; /* live under its own address */
        }
        return i;
    }
    return -1;
}

/* The tail of a bind, shared by every path that lands on a decoded (non-EFB)
 * slot: bind the GL name, fold NPOT padding into the texture matrix, and emit
 * glTexParameter only when this texture object's own parameters actually
 * changed since the last time this slot was bound. */
/* --tlutlog: is this call inside the window the log is scoped to?  With
 * --drawlog-at F the window is frame F, which is the only way to ask the
 * question about one screen rather than about a whole boot. */
static int tlutlog_armed(void) {
    if (!port_opt.tlutlog) {
        return 0;
    }
    if (port_opt.drawlog_frame &&
        gl13_frame_number() + 1 != (unsigned)port_opt.drawlog_frame) {
        return 0;
    }
    return 1;
}

static void tex_bind_finish(int unit, GXTexObjPort* o, int slot) {
    /* --tlutlog, the CI half.  The TL32 double-TLUT trick (hsfdraw.c:1823)
     * binds one C8 atlas twice with two palettes; if the two binds report the
     * same palette address, or the same palette hash, the second read gave
     * back the first palette and the eye's colour expression collapses
     * (PLAN.md 29.4).  Print enough to tell those apart: the TLUT name, the
     * palette pointer, its entry count and format, an FNV over the bytes the
     * decoder actually reads, the swap table, and the cache slot. */
    if (o->is_ci && tlutlog_armed()) {
        const GXTlutObjPort* t =
            (o->tlut_name < 64 && gx.tlut[o->tlut_name].magic == TLUT_MAGIC)
                ? &gx.tlut[o->tlut_name]
                : NULL;
        port_log("port> tlut bind unit %d map-tlut %u  img %p fmt %u %ux%u  "
                 "lut %p n %u fmt %u hash %08x  swap %02x  slot %d gl %u\n",
                 unit, (unsigned)o->tlut_name, o->image, (unsigned)o->format,
                 (unsigned)o->width, (unsigned)o->height, t ? t->lut : NULL,
                 t ? (unsigned)t->n : 0u, t ? (unsigned)t->fmt : 0u,
                 (t && t->lut) ? fnv(t->lut, (size_t)t->n * 2, 2166136261u) : 0u,
                 (unsigned)cache[slot].swap, slot, cache[slot].gl_name);
    }
    o->gl_name = cache[slot].gl_name;
    gx_unit_alpha_min[unit & 7] = cache[slot].alpha_min;
    if (!gl13_live() || !o->gl_name) {
        return;
    }
    glc_bind_texture(unit, o->gl_name);
    /* Fold the NPOT padding into this unit's texture matrix, so the vertex
     * decoder keeps emitting the game's own 0..1 texcoords and knows nothing
     * about it.  GL_TEXTURE is otherwise unused by this backend -- texgen is
     * done on the CPU (PLAN.md §10.5) -- so the matrix is ours to spend. */
    glc_tex_matrix(unit, cache[slot].su, cache[slot].sv);
    /* glTexParameter belongs to the texture *object*, not the unit, so the
     * remembered copy lives in the cache entry -- and a bind of a texture
     * whose parameters have not changed emits nothing at all.  This is four
     * calls a bind and 427 binds a frame; it was the third-largest block of
     * per-draw GL traffic after the TEV chain and the raster state. */
    {
        CacheEntry* e = &cache[slot];
        int ws = (int)gl_wrap(o->wrap_s);
        int wt = (int)gl_wrap(o->wrap_t);
        /* No mip levels are uploaded, so a mipmapped min filter would make the
         * texture incomplete; fall back to its non-mipmapped equivalent. */
        int mn = o->min_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR;
        int mg = (int)gl_filter(o->mag_filt, 0);
        if (e->param_wrap_s != ws || e->param_wrap_t != wt || e->param_min != mn ||
            e->param_mag != mg) {
            e->param_wrap_s = ws;
            e->param_wrap_t = wt;
            e->param_min = mn;
            e->param_mag = mg;
            /* glTexParameter acts on the texture bound to the *active* unit,
             * and glc_bind_texture above says nothing to GL -- not even
             * glActiveTexture -- when the unit already holds this name.  So
             * the parameters went to whichever unit happened to be active:
             * one unit's texture got another's wrap and filter, depending
             * on the order of binds before it.  M16 found it as a 150-pixel
             * disagreement in one eye between two submit shapes that were
             * otherwise identical (PLAN.md 31.3), a latent bug since M5b. */
            glc_active_texture(unit);
            if (gl13_trace_armed()) {
                port_log("gltrace> glTexParameteri unit %d name %u wrap %d %d filt %d %d\n", unit,
                         o->gl_name, ws, wt, mn, mg);
            }
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)ws);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wt);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)mn);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)mg);
        }
    }
}

/* Re-encode a decoded RGBA8 image through a GX texture swap table.
 *
 * This is the exact half of PLAN.md 21's swap-table work.  A GX TEV stage
 * routes the *texture* colour through a four-entry table before the combiner
 * sees it -- `out.r = in[tbl[0]]` and so on -- and the character eyes are
 * drawn by a stage that does exactly that.  GL 1.3 has no per-channel
 * swizzle, but there is nothing to express: the swap is a property of the
 * texels, so applying it to the texels once, at decode, is not an
 * approximation of the hardware, it *is* the hardware. */
static void swizzle_rgba(u8* rgba, int w, int h, u8 swap) {
    const int sel[4] = { swap & 3, (swap >> 2) & 3, (swap >> 4) & 3,
                         (swap >> 6) & 3 };
    size_t n = (size_t)w * (size_t)h, i;
    if (swap == GX_SWAP_IDENTITY) {
        return;
    }
    for (i = 0; i < n; i++) {
        u8* p = rgba + i * 4;
        u8 in[4];
        in[0] = p[0];
        in[1] = p[1];
        in[2] = p[2];
        in[3] = p[3];
        p[0] = in[sel[0]];
        p[1] = in[sel[1]];
        p[2] = in[sel[2]];
        p[3] = in[sel[3]];
    }
}

void gx_tex_bind(int unit, GXTexObjPort* o) { gx_tex_bind_swapped(unit, o, GX_SWAP_IDENTITY); }

static void tex_bind_body(int unit, GXTexObjPort* o, u8 swap);

void gx_tex_bind_swapped(int unit, GXTexObjPort* o, u8 swap) {
    if (!o || o->magic != TEXOBJ_MAGIC) {
        return;
    }
    port_perf_sub_enter(PERF_SUB_TEX);
    tex_bind_body(unit, o, swap);
    port_perf_sub_leave();
}

static void tex_bind_body(int unit, GXTexObjPort* o, u8 swap) {
    const GXTlutObjPort* tlut = NULL;
    int slot, is_efb;
    unsigned frame;
    if (gl13_draw_off()) {
        /* --nodraw: nothing will sample it, and decoding a texture is the
         * second most expensive thing this backend does.  The cache is
         * flushed when drawing comes back, so nothing stale survives. */
        return;
    }

    /* The validation epoch: bumped the first time any bind observes a new
     * frame number.  See the long comment above this function's old body,
     * now attached to `cache_epoch`. */
    frame = gl13_frame_number();
    if (!cache_epoch_started || frame != cache_epoch_frame) {
        cache_epoch_started = 1;
        cache_epoch_frame = frame;
        cache_epoch++;
    }
    cache_frame = frame;

    if (o->is_ci && o->tlut_name < 64 && gx.tlut[o->tlut_name].magic == TLUT_MAGIC) {
        tlut = &gx.tlut[o->tlut_name];
    }

    /* One hash-table lookup replaces both of the old O(cache_used) scans: the
     * EFB-copy scan (image only, and given priority) and the regular (image,
     * format, w, h, lut) scan. */
    slot = find_slot(o->image, o->format, o->width, o->height,
                      tlut ? tlut->lut : NULL, swap, &is_efb);

    if (slot >= 0 && is_efb) {
        /* An EFB copy is bound as it stands: there is nothing at `image` to
         * decode, and nothing to hash either -- what the game left in that
         * buffer is whatever it was before the copy, and the pixels live in a
         * GL texture the copy already filled. */
        o->gl_name = cache[slot].gl_name;
        cache[slot].last_used = frame;
        gx_unit_alpha_min[unit & 7] = 0; /* an EFB copy: unknown, assume it can */
        if (!gl13_live() || !o->gl_name) {
            return;
        }
        stat_hit++;
        glc_bind_texture(unit, o->gl_name);
        /* M24b (PLAN.md 39b): the copy was filled by glCopyTexSubImage2D in
         * GL's row order -- t = 0 is the *bottom* of the copied region --
         * where the texture the game thinks it bound has its first row at
         * the top of the region (the copy unit writes the EFB top-down, and
         * every decoded texture is uploaded that way, so t = 0 is its top).
         * So an EFB copy sampled with the game's own coordinates came out
         * upside down: m416's afterimage (a whole-screen copy drawn back
         * over itself every frame) mirrored the room about the middle of
         * the screen, the wipe's crossfade faded the old scene in upside
         * down, and every projected shadow map since M3 was mirrored in the
         * light's t.  The fold puts t = 0 at the top: t' = sv - t*sv.
         * `--noefbflip` is the old orientation. */
        if (port_opt.noefbflip) {
            glc_tex_matrix(unit, cache[slot].su, cache[slot].sv);
        } else {
            glc_tex_matrix_fold(unit, cache[slot].su, -cache[slot].sv, cache[slot].sv);
        }
        if (cache[slot].param_wrap_s != (int)gl_wrap(o->wrap_s)) {
            cache[slot].param_wrap_s = (int)gl_wrap(o->wrap_s);
            glc_active_texture(unit); /* same reason as in tex_bind_finish */
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                (GLint)cache[slot].param_wrap_s);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                (GLint)gl_wrap(o->wrap_t));
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        return;
    }

    /* GX textures are always power of two, and a non-POT one silently makes
     * the GL texture incomplete -- which disables texturing for that unit and
     * looks like a lighting bug thirty draws later. */
    if ((!pot(o->width) || !pot(o->height)) &&
        (o->wrap_s == GX_REPEAT || o->wrap_t == GX_REPEAT)) {
        /* Padding and GX_REPEAT disagree: the repeat would run over the
         * padding.  Every NPOT texture in the boot path clamps, so this is
         * reported rather than solved until something actually needs it. */
        gx_warn("texture: a non-power-of-two texture with GX_REPEAT is padded "
                "to a power of two and will repeat over the padding");
    }

    if (slot >= 0) {
        /* The cache is content-keyed because the game reuses one buffer for
         * different images and GX has no "this texture changed" call we
         * could trust (see `cache_epoch`'s comment).  A *hit* on an existing
         * slot only needs to touch the texel bytes at all once per
         * validation epoch: `--texhash-full` and `--texvalidate-every-bind`
         * both defeat that, independently, for bisecting a suspected
         * staleness bug between "the epoch" and "the sampling". */
        CacheEntry* e = &cache[slot];
        int need_validate = port_opt.texhash_full || validate_every_bind ||
                            e->validated_epoch != cache_epoch;
        e->last_used = frame;
        if (e->dirty) {
            /* M35: the game flushed these bytes since the last bind (see
             * port_gx_tex_dirty).  The exhaustive hash says whether they
             * changed; the sampled one (four 256-byte windows of a 720 KB
             * canvas) cannot. */
            u32 cf = tex_bind_content_hash(o, tlut, 1 /* exhaustive */);
            e->dirty = 0;
            e->validated_epoch = cache_epoch;
            if (e->content_full && cf == e->content_full) {
                stat_dirty_clean++;
                stat_hit++;
            } else {
                stat_dirty_redecode++;
                stat_evict++;
                e->content_full = cf;
                e->content = tex_bind_content_hash(o, tlut, 0); /* what the epoch compares */
                e->enc_size = (u32)encoded_size(o->format, o->width, o->height);
                tex_bind_decode_and_upload(slot, unit, o, tlut);
            }
            tex_bind_finish(unit, o, slot);
            return;
        }
        if (!need_validate) {
            stat_hit++;
        } else {
            u32 content = tex_bind_content_hash(o, tlut, 0 /* slot exists: not first sight */);
            stat_revalidate++;
            e->validated_epoch = cache_epoch;
            if (content == e->content) {
                stat_hit++;
            } else {
                /* An in-place rewrite: same key, new bytes (the SBK lesson --
                 * Mario Party rewrites scratch textures and animated palettes
                 * at the same address).  The key fields, and this slot's
                 * place in the hash table, do not change. */
                stat_evict++;
                e->content = content;
                e->content_full = 0; /* M23: no longer what a re-key could trust */
                tex_bind_decode_and_upload(slot, unit, o, tlut);
            }
        }
        tex_bind_finish(unit, o, slot);
        return;
    }

    /* A genuine miss: no entry for this (image, format, w, h, lut) key. */
    {
        u32 content_full = tex_bind_content_hash(o, tlut, 1 /* first sight: exhaustive */);
        /* M23: the hash the next epoch's revalidation will compute is the
         * sampled one for anything over TEX_HASH_SAMPLE; store that, or the
         * revalidation always misses and decodes the texture again
         * (--oldfirsthash keeps the pre-M23 double decode). */
        u32 content = port_opt.oldfirsthash ? content_full : tex_bind_content_hash(o, tlut, 0);
        int again = cache_find_by_content(o, content_full, swap, tlut ? tlut->lut : NULL, frame);
        if (again >= 0) {
            CacheEntry* e = &cache[again];
            hash_remove(again);
            e->image = o->image;
            e->lut = tlut ? tlut->lut : NULL;
            e->validated_epoch = cache_epoch;
            e->last_used = frame;
            e->enc_size = (u32)encoded_size(o->format, o->width, o->height);
            e->dirty = 0;
            hash_insert(again);
            frame_rekeys++;
            stat_rekeys++;
            stat_rekey_bytes += e->gl_bytes;
            stat_hit++;
            tex_bind_finish(unit, o, again);
            return;
        }
        if (nfree_slots) {
            slot = free_slots[--nfree_slots]; /* a slot the budget freed */
        } else if (cache_used == CACHE_MAX) {
            slot = (int)(content % CACHE_MAX); /* an eviction, not a leak */
            hash_remove(slot); /* unlink whatever key that slot held before */
            if (gl13_live() && cache[slot].gl_name) {
                GLuint n = cache[slot].gl_name;
                GL(glDeleteTextures)(1, &n);
            }
            cache_gl_bytes -= cache[slot].gl_bytes;
            free(cache[slot].cpu_rgba);
            memset(&cache[slot], 0, sizeof(cache[slot]));
        } else {
            slot = cache_used++;
        }
        stat_miss++;
        cache[slot].image = o->image;
        cache[slot].lut = tlut ? tlut->lut : NULL;
        cache[slot].format = o->format;
        cache[slot].w = o->width;
        cache[slot].h = o->height;
        cache[slot].swap = swap;
        cache[slot].content = content;
        cache[slot].content_full = content_full;
        cache[slot].enc_size = (u32)encoded_size(o->format, o->width, o->height);
        cache[slot].dirty = 0;
        cache[slot].validated_epoch = cache_epoch;
        cache[slot].last_used = frame;
        hash_insert(slot);
        {
            /* M24: the worker may have decoded exactly these bytes already */
            Staged* st = predecode_on() ? predecode_take(o, tlut ? tlut->lut : NULL, content_full)
                                        : NULL;
            if (st) {
                tex_bind_upload_staged(slot, unit, o, st);
            } else {
                if (predecode_on()) {
                    frame_pre_unstaged++;
                }
                tex_bind_decode_and_upload(slot, unit, o, tlut);
            }
        }
        tex_bind_finish(unit, o, slot);
        if (cache_gl_bytes > tex_budget_bytes) {
            cache_evict_to_budget();
        }
    }
}

/* ---- the GX texture-object entry points ----------------------------------- */

void GXInitTexObj(GXTexObj* obj, void* image, u16 w, u16 h, GXTexFmt fmt,
                  GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap) {
    /* No GX_STATE_TOUCH here or in the other GXInit* below (M22, PLAN.md
     * 37): these write the *game's* object -- hsfdraw.c LoadTexture's stack
     * local -- and nothing a pending batch reads.  The batch's textures are
     * the copies GXLoadTexObj made into gx.bound[], and that is where the
     * compare-first flush lives.  On the title, GXInitTexObj ended 3,922 of
     * 11,982 batches and 1,802 of those pairs had the same state. */
    GXTexObjPort* o = (GXTexObjPort*)obj;
    memset(o, 0, sizeof(*o));
    o->magic = TEXOBJ_MAGIC;
    o->image = image;
    o->width = w;
    o->height = h;
    o->format = (u32)fmt;
    o->wrap_s = (u8)ws;
    o->wrap_t = (u8)wt;
    o->mipmap = mipmap;
    o->min_filt = mipmap ? GX_LIN_MIP_LIN : GX_LINEAR;
    o->mag_filt = GX_LINEAR;
    o->max_lod = 0.0f;
}

void GXInitTexObjCI(GXTexObj* obj, void* image, u16 w, u16 h, GXCITexFmt fmt,
                    GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap, u32 tlut_name) {
    GXTexObjPort* o = (GXTexObjPort*)obj;
    GXInitTexObj(obj, image, w, h, (GXTexFmt)fmt, ws, wt, mipmap);
    o->is_ci = 1;
    o->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt,
                     f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                     GXBool do_edge_lod, GXAnisotropy aniso) {
    GXTexObjPort* o = (GXTexObjPort*)obj;
    (void)bias_clamp;
    (void)do_edge_lod;
    (void)aniso;
    if (o->magic != TEXOBJ_MAGIC) {
        return;
    }
    o->min_filt = (u8)min_filt;
    o->mag_filt = (u8)mag_filt;
    o->min_lod = min_lod;
    o->max_lod = max_lod;
    o->lod_bias = lod_bias;
}

void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t) {
    GXTexObjPort* o = (GXTexObjPort*)obj;
    if (o->magic == TEXOBJ_MAGIC) {
        o->wrap_s = (u8)s;
        o->wrap_t = (u8)t;
    }
}

/* M37: is this texture unit read by any stage the pending batch draws
 * under -- a TEV stage below GXSetNumTevStages, or an indirect stage below
 * GXSetNumIndStages?  Exactly the units submit_rec_capture walks. */
static int gx_unit_in_use(unsigned unit) {
    int i;
    for (i = 0; i < (int)gx.num_tev && i < GX_TEV_STAGES; i++) {
        if (gx.tev[i].map == unit) {
            return 1;
        }
    }
    for (i = 0; i < (int)gx.num_ind && i < 4; i++) {
        if (gx.ind[i].map == unit) {
            return 1;
        }
    }
    return 0;
}

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    /* Compare-first (M22): the same texture loaded again -- the next object
     * of the same material -- changes nothing the pending batch reads.  The
     * fields from gl_name on are the cache's, filled in at bind time in the
     * copy and zero in the game's object, so they are not compared. */
    /* M37 (PLAN.md 52): ...and a unit no stage in use reads is not read by
     * the submit either (the SubmitRec walks gx.tev[i].map for i < num_tev),
     * so loading it changes nothing the pending batch draws with.  A stage
     * that later names the unit must come through GXSetTevOrder or
     * GXSetNumTevStages, both compare-first. */
    GX_STATE_TOUCH_IF(GX_CMP_TEV, (unsigned)id >= GX_TEX_UNITS || !obj ||
                                      (memcmp(&gx.bound[id], obj,
                                              offsetof(GXTexObjPort, gl_name)) != 0 &&
                                       ((port_opt.cmpmask & GX_CMP_TEXU) == 0 ||
                                        gx_unit_in_use((unsigned)id))));
    /* Copy, do not alias: see the comment on GXState::bound.  This is what
     * the console's write to the texture registers is, and the game's sprite
     * path depends on it -- HuSprTexLoad's GXTexObj is a stack local. */
    if ((unsigned)id < GX_TEX_UNITS && obj) {
        gx.bound[id] = *(const GXTexObjPort*)obj;
        /* M24: on a consumed frame nothing binds, so this is the earliest
         * word that the next drawn frame wants this texture */
        if (gl13_draw_off() && ((const GXTexObjPort*)obj)->magic == TEXOBJ_MAGIC &&
            port_framemode_active() && predecode_on()) {
            predecode_request((const GXTexObjPort*)obj);
        }
    }
}

/* NULL unless the unit holds an object GXInitTexObj actually initialised. */
GXTexObjPort* gx_bound_tex(unsigned id) {
    if (id >= GX_TEX_UNITS || gx.bound[id].magic != TEXOBJ_MAGIC) {
        return NULL;
    }
    return &gx.bound[id];
}
/* the same question of a state that is not the current one (M22) */
const GXTexObjPort* gx_bound_tex_of(const GXState* st, unsigned id) {
    if (id >= GX_TEX_UNITS || st->bound[id].magic != TEXOBJ_MAGIC) {
        return NULL;
    }
    return &st->bound[id];
}

void GXInitTlutObj(GXTlutObj* obj, void* lut, GXTlutFmt fmt, u16 n) {
    GXTlutObjPort* t = (GXTlutObjPort*)obj; /* the game's object; no flush (above) */
    t->magic = TLUT_MAGIC;
    t->lut = lut;
    t->fmt = (u32)fmt;
    t->n = n;
}

/* M37: is this palette named by a CI texture on a unit in use? */
static int gx_tlut_in_use(u32 name) {
    int i;
    for (i = 0; i < (int)gx.num_tev && i < GX_TEV_STAGES; i++) {
        unsigned u = gx.tev[i].map;
        if (u < GX_TEX_UNITS && gx.bound[u].magic == TEXOBJ_MAGIC && gx.bound[u].is_ci &&
            gx.bound[u].tlut_name == name) {
            return 1;
        }
    }
    return 0;
}

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) {
    GXTlutObjPort* t = (GXTlutObjPort*)obj;
    /* M37: ...and a palette no CI texture the pending batch draws with
     * names is not read by the submit either (submit_rec_capture takes
     * gx.tlut[bound[u].tlut_name] only for a unit in use holding a CI
     * texture), so loading it ends no batch. */
    GX_STATE_TOUCH_IF(GX_CMP_TEV, tlut_name >= 64 || t->magic != TLUT_MAGIC ||
                                      (memcmp(&gx.tlut[tlut_name], t, sizeof(*t)) != 0 &&
                                       ((port_opt.cmpmask & GX_CMP_TEXU) == 0 ||
                                        gx_tlut_in_use(tlut_name))));
    if (tlut_name < 64 && t->magic == TLUT_MAGIC) {
        gx.tlut[tlut_name] = *t;
        /* --tlutlog, the load half: the game's own two calls, as the game
         * makes them.  The second of a TL32 pair should carry a palette
         * address (palSize+0xF)&0xFFF0 entries past the first. */
        if (tlutlog_armed()) {
            port_log("port> tlut load name %2u  lut %p n %u fmt %u hash %08x\n",
                     (unsigned)tlut_name, t->lut, (unsigned)t->n, (unsigned)t->fmt,
                     t->lut ? fnv(t->lut, (size_t)t->n * 2, 2166136261u) : 0u);
        }
    }
}

u32 GXGetTexBufferSize(u16 w, u16 h, u32 fmt, u8 mipmap, u8 max_lod) {
    u32 size = (u32)encoded_size(fmt, w, h);
    if (mipmap) {
        u32 total = size;
        int i;
        for (i = 1; i < max_lod && w > 1 && h > 1; i++) {
            w = (u16)(w / 2);
            h = (u16)(h / 2);
            total += (u32)encoded_size(fmt, w ? w : 1, h ? h : 1);
        }
        return total;
    }
    return size;
}

/* TMEM does not exist here; the cache is content keyed and must survive. */
/* neither touches anything the port has, so neither ends a batch (M22) */
void GXInvalidateTexAll(void) {}
void GXInvalidateTexRegion(GXTexRegion* r) { (void)r; }

/* ---- EFB copies ----------------------------------------------------------- */
/* No FBO on this card, so an EFB copy is glCopyTexSubImage2D out of the back
 * buffer, which is what fixed-function-era engines did.  The formats the game
 * copies in are GX_CTF_R8, GX_CTF_A8 and the two depth ones. */

int gl13_frame_wanted(unsigned n);
void gx_tex_copy(void* dest, int clear) {
    static unsigned copies, depth_dropped;
    int sl, st, sw, sh, dw, dh, cw, ch, pw, ph, i, slot = -1;
    int from_front = 0, half;
    copies++;
    if (!gl13_live()) {
        /* --realtime, a consumed frame (src/platform/framemode.c): nothing was
         * drawn, so there is no EFB to copy.  The copy is answered from the
         * front buffer, which is the last picture presented: one drawn frame
         * stale, and what the player is looking at.  The wipe's crossfade
         * takes a whole-screen copy at its first frame and blends it over the
         * next thirty; the mode-select bubbles copy the *region* of screen
         * behind each bubble once and draw it for as long as the bubble
         * lives -- M17's first witness dropped region copies as "the next
         * drawn frame redoes them", and every bubble was a yellow square
         * (screenshots/m17-modesel-bubbles-dropped.png).  A per-frame
         * render-to-texture (a shadow map, a water reflection) gets the
         * wrong region this way, but nothing draws on this frame and the
         * next drawn frame redoes it before reading it. */
        if (!gl13_have_context() || !gl13_draw_off() || !port_framemode_active()) {
            return;
        }
        /* M21 (PLAN.md 36): a copy the game *clears* after is an offscreen
         * pass -- the shadow map (hsfman.c:2007, m428, m439), never a picture
         * of the screen -- so the front buffer is the wrong answer for it and
         * the texture keeps what the last drawn frame's pass put there.  This
         * is what m415's canvas read back (port_gx_copy_read) when the last
         * intro frame was a consumed one: the presented picture, as the
         * paper's texture, for the rest of the minigame. */
        if (clear) {
            stat_copy_kept++;
            return;
        }
        from_front = 1;
        if (gx.tex_src[0] == 0 && gx.tex_src[1] == 0 && gx.tex_src[2] >= 640 &&
            gx.tex_src[3] >= 400) {
            stat_copy_front++;
        } else {
            stat_copy_region_front++;
        }
    }
    if (gx.tex_dst_fmt == GX_TF_Z24X8 || gx.tex_dst_fmt == GX_TF_Z8 ||
        gx.tex_dst_fmt == GX_TF_Z16 || gx.tex_dst_fmt == GX_CTF_Z8M ||
        gx.tex_dst_fmt == GX_CTF_Z8L || gx.tex_dst_fmt == GX_CTF_Z16L) {
        /* The Radeon 9000 has no ARB_depth_texture (docs/g4-glinfo.log says so
         * from the card itself, correcting what the development Mac reported),
         * so there is no GL 1.3 home for a depth copy at all: no FBO to read
         * from, no depth internal format to copy into, and no way to sample
         * one afterwards.  The game does four of these -- three Z24X8 and one
         * Z8 -- and they are a depth-of-field or shadow helper, so the
         * approximation is to skip them and say so.  The alternative, a colour
         * proxy, would put a picture of the scene where the shader expects a
         * depth ramp, which is worse than nothing: whatever reads it would
         * modulate by an arbitrary image rather than by a flat value.
         * Skipping leaves the texture at whatever it last held, which for
         * these sites is the neutral case. */
        depth_dropped++;
        gx_warn("GXCopyTex of a depth format: the Radeon 9000 has no "
                "ARB_depth_texture, so the copy is skipped (see gx_tex.c)");
        return;
    }

    sl = gx.tex_src[0];
    st = gx.tex_src[1];
    sw = gx.tex_src[2];
    sh = gx.tex_src[3];
    dw = gx.tex_dst[0] ? gx.tex_dst[0] : sw;
    dh = gx.tex_dst[1] ? gx.tex_dst[1] : sh;
    if (sw <= 0 || sh <= 0) {
        return;
    }
    /* M23 (PLAN.md 38): the half-scale copy.  GXSetTexCopyDst's mipmap flag
     * asks the copy unit for a 2x2 box filter -- the source rectangle is
     * twice the destination in each axis -- and every shadow map in the game
     * is one (hsfman.c:2001: a 384x384 pass copied to 192x192).  Until M23
     * this copied min(sw,dw) x min(sh,dh) texels at 1:1, i.e. the bottom-left
     * quarter of the pass at twice its size: Stamp Out!'s paper was a
     * magnified corner of its own shadow map (green or blue with bands, one
     * run in several white when the corner was blank), and every projected
     * shadow since M3 was the wrong quadrant of its pass.  There is no
     * FBO and no blit here, so the copy keeps the whole source rectangle at
     * full size and lets GL's bilinear sample it at 2:1 (within a texel of
     * the box filter); the read-back (port_gx_copy_read) does the 2x2 mean
     * exactly.  `--nocopyhalf` is the pre-M23 corner. */
    half = 0;
    cw = dw;
    ch = dh;
    if (!port_opt.nocopyhalf && (gx.tex_dst_half || (sw == 2 * dw && sh == 2 * dh)) &&
        sw > dw && sh > dh) {
        half = 1;
        cw = sw;
        ch = sh;
        if (!from_front) {
            stat_copy_half++;
        }
    }

    /* The cache entry is keyed on `dest`, the address the game will later wrap
     * in a GXTexObj.  It carries no decodable texels, so it is marked `efb`
     * and gx_tex_bind uses its GL name as it stands.  The same hash table
     * `find_slot()` looks up in `gx_tex_bind` covers this entry too -- the
     * format/w/h/lut arguments below are irrelevant for an `efb` match, which
     * is keyed on `image` alone (see `find_slot()`). */
    slot = find_slot(dest, 0, 0, 0, NULL, GX_SWAP_IDENTITY, &i);
    if (slot < 0) {
        if (nfree_slots) {
            slot = free_slots[--nfree_slots];
        } else if (cache_used == CACHE_MAX) {
            gx_warn("GXCopyTex: the texture cache is full; the copy is dropped");
            return;
        } else {
            slot = cache_used++;
        }
        memset(&cache[slot], 0, sizeof(cache[slot]));
        cache[slot].image = dest;
        cache[slot].efb = 1;
        hash_insert(slot);
        stat_miss++;
    }
    cache[slot].format = gx.tex_dst_fmt;
    cache[slot].w = (u16)dw;
    cache[slot].h = (u16)dh;

    pw = pot_up(cw);
    ph = pot_up(ch);
    if (port_opt.tfssqcopy) {
        /* M35 diagnostic: the copy's texture square (does the fragment
         * shader sample a 1024x512 image white?) */
        pw = ph = pw > ph ? pw : ph;
    }
    {
        GLuint name = cache[slot].gl_name;
        if (!name) {
            GL(glGenTextures)(1, &name);
            cache[slot].gl_name = name;
            cache[slot].param_wrap_s = -1;
        } else if (cache[slot].copy_w != (u16)cw || cache[slot].copy_h != (u16)ch) {
            /* the same buffer copied at another size (a shadow map resized
             * by Hu3DShadowSizeSet): the texture is sized again below */
            cache[slot].param_wrap_s = -1;
        }
        cache[slot].copy_w = (u16)cw;
        cache[slot].copy_h = (u16)ch;
        cache[slot].copy_half = (u8)half;
        glc_active_texture(0);
        GL(glBindTexture)(GL_TEXTURE_2D, name);
        glc_note_bind(0, name);
        /* glCopyTexImage2D would be one call, but it is not in the GL 1.3
         * subset this backend has written down and the destination has to be a
         * power of two anyway, so the texture is sized once with a null
         * glTexImage2D and refilled with glCopyTexSubImage2D thereafter --
         * which is also cheaper, because it does not reallocate. */
        if (cache[slot].param_wrap_s == -1) {
            /* M23 (PLAN.md 38): the texture is sized with *defined* texels,
             * not NULL.  The copied region fills its bottom-left `cw` x `ch`
             * and the padding beyond it was never written -- whatever the
             * driver's VRAM held -- and a shadow map is sampled *through a
             * projection* (SetShadow, GX_TG_MTX3x4 from position), whose
             * coordinates run past the region's edge on any receiver larger
             * than the shadow camera's view.  GX clamps at the copy's real
             * edge; GL clamps at the padded texture's, so the receiver read
             * a stretched column of VRAM garbage: Stamp Out!'s paper in
             * bands, green or blue or orange by card and by run, white on
             * the run where that memory was black.  Zero is the shadow
             * map's own edge (its clear colour, hsfman.c:1930, and the
             * two-pixel border its scissor leaves), so a clamped sample
             * past the region reads what the console's would. */
            u8* zero = (u8*)calloc((size_t)pw * ph, 4);
            (port_opt.glcheck ? gl13_check("glTexImage2D") : 0);
            rt_teximage2d_owned(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, zero); /* M27: freed after the call */
            cache[slot].param_wrap_s = 0;
            cache_gl_bytes -= cache[slot].gl_bytes;
            cache[slot].gl_bytes = (unsigned)(pw * ph * 4);
            cache_gl_bytes += cache[slot].gl_bytes;
            cache[slot].last_used = cache_frame;
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        /* GX's y runs down from the top of the EFB and GL's up from the
         * bottom, and the copy is written into the bottom-left of the padded
         * texture so the game's own 0..1 texcoords, folded by su/sv below,
         * land on it. */
        if (from_front && !gl13_fullscreen()) {
            /* under --fullscreen the back buffer's corner holds the last
             * drawn frame (gl13.c fs_blit_back); the front holds the scaled
             * picture */
            GL(glReadBuffer)(GL_FRONT);
        }
        GL(glCopyTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, sl, 480 - (st + sh),
                                sw < cw ? sw : cw, sh < ch ? sh : ch);
        if (from_front && !gl13_fullscreen()) {
            GL(glReadBuffer)(GL_BACK);
        }
        if (port_opt.tfscopycpu && !rt_on()) {
            /* M35 diagnostic: the copy round-tripped through the CPU into a
             * glTexImage2D-defined image (does the fragment shader read a
             * CopyTexSubImage'd texture at all?) */
            u8* px = (u8*)malloc((size_t)pw * ph * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            rt_teximage2d_owned(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
    }
    cache[slot].su = (float)cw / (float)pw;
    cache[slot].sv = (float)ch / (float)ph;
    stat_efb++;
    if (port_opt.copylog) {
        port_log("port> copy: frame %u  src %d,%d %dx%d -> dst %dx%d fmt %u%s%s%s  slot %d gl %u  dest %p\n",
                 gl13_frame_number() + 1, sl, st, sw, sh, dw, dh, (unsigned)gx.tex_dst_fmt,
                 half ? " half" : "", clear ? " clear" : "", from_front ? " front" : "", slot,
                 cache[slot].gl_name, dest);
    }
    if (cache[slot].cpu_read && !from_front && stat_efb - cache[slot].cpu_read_at <= 8) {
        /* the game reads this one back (M23): downsample it into the region
         * the clear below is about to wipe, and keep the bytes */
        if (!cache[slot].cpu_rgba) {
            cache[slot].cpu_rgba = (u8*)malloc((size_t)dw * dh * 4);
        }
        if (cache[slot].cpu_rgba) {
            double t0 = port_now_seconds();
            gl13_downsample_read(cache[slot].gl_name, cache[slot].su, cache[slot].sv, sl,
                                 480 - (st + sh), dw, dh, cache[slot].cpu_rgba);
            stat_copy_read_gpu++;
            stat_copy_read_gpu_s += port_now_seconds() - t0;
        }
    }
    /* --dumpcopy on a --dumpframe frame: the copy's texels as GL holds them
     * right after the copy, before the clear below (M23) */
    if (port_opt.dumpcopy && gl13_frame_wanted(gl13_frame_number() + 1) && !from_front) {
        static unsigned n;
        u8* rgba = (u8*)malloc((size_t)pw * ph * 4);
        if (rgba && n < 64) {
            char path[1024];
            FILE* f;
            GL(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
            GL(glGetTexImage)(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            snprintf(path, sizeof(path), "%s/efbcopy-%02u-f%05u-%dx%d.ppm",
                     port_opt.shotdir ? port_opt.shotdir : ".", n++, gl13_frame_number() + 1,
                     cw, ch);
            f = fopen(path, "wb");
            if (f) {
                int yy, xx;
                fprintf(f, "P6\n%d %d\n255\n", cw, ch);
                for (yy = 0; yy < ch; yy++) {
                    for (xx = 0; xx < cw; xx++) {
                        fwrite(rgba + ((size_t)(ch - 1 - yy) * pw + xx) * 4, 1, 3, f);
                    }
                }
                fclose(f);
            }
            port_log("port> --dumpcopy: wrote %s (src %d,%d %dx%d dst %dx%d fmt %u clear %d)\n", path,
                     sl, st, sw, sh, dw, dh, (unsigned)gx.tex_dst_fmt, clear);
        }
        free(rgba);
    }
    if (from_front) {
        return; /* nothing to clear: nothing was drawn */
    }

    /* GXCopyTex's clear applies to the EFB *after* the copy, and unlike
     * GXCopyDisp there is no swap in the way: the game is about to draw the
     * next thing into the same back buffer and expects the copied region to be
     * blank.  So it happens now, scissored to the region that was copied. */
    if (clear) {
        GL(glScissor)(sl, 480 - (st + sh), sw, sh);
        GL(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GL(glDepthMask)(GL_TRUE);
        GL(glClearColor)(gx.copy_clear.r / 255.0f, gx.copy_clear.g / 255.0f,
                         gx.copy_clear.b / 255.0f, gx.copy_clear.a / 255.0f);
        GL(glClearDepth)((double)gx.copy_clear_z / (double)0xFFFFFF);
        GL(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glc_invalidate();
    }
}

void GXCopyTex(void* dest, GXBool clear) { GX_FLUSH_NOW(); gx_tex_copy(dest, clear ? 1 : 0); }

/* ---- a copy the game reads with the CPU (M20, PLAN.md 35.3) ----------------
 * The copy above never writes MEM1: it lives in a GL texture keyed on `dest`,
 * and the bytes at `dest` stay whatever the heap held.  One game-side site
 * reads such a copy back: m415dll (Stamp Out!) memcpys the shadow map --
 * 192x192, one byte a texel, the stamps drawn by the shadow pass -- into its
 * canvas bitmap (an I8 ANIMDATA), then textures the paper with the canvas.
 * patches.txt turns those two memcpys into this: the copy's GL texture is
 * read back and encoded as GX would have written it at `dest`, one byte a
 * texel in 8x4 tiles.  The game copies the shadow map as GX_CTF_R8 (the red
 * channel) and binds it as GX_TF_I8; the tiles are the same, so the byte is
 * the copy format's channel (R8/G8/B8/A8) or GX's intensity for I8.  On a
 * consumed frame the texture holds the last drawn copy, which is what the
 * console's MEM1 would hold too.  A destination nothing was copied to (or
 * not in a one-byte format) is answered with zeros and one line in the log. */
static void dump_canvas(const u8* o, int w, int h, unsigned n) {
    char path[1024];
    FILE* f;
    snprintf(path, sizeof(path), "%s/canvas-%02u-f%05u-%dx%d.pgm",
             port_opt.shotdir ? port_opt.shotdir : ".", n, gl13_frame_number(), w, h);
    f = fopen(path, "wb");
    if (f) {
        int yy, xx;
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        for (yy = 0; yy < h; yy++) {
            for (xx = 0; xx < w; xx++) {
                fputc(o[(size_t)(yy >> 2) * (w >> 3) * 32 + (yy & 3) * 8 + (size_t)(xx >> 3) * 32 + (xx & 7)], f);
            }
        }
        fclose(f);
    }
}

void port_gx_copy_read(const void* dest, void* out, unsigned long nbytes) {
    int slot, is_efb = 0, w, h, cw, ch, x, y, chan, half;
    u8* o = (u8*)out;

    slot = find_slot(dest, 0, 0, 0, NULL, GX_SWAP_IDENTITY, &is_efb);
    if (slot < 0 || !is_efb || !cache[slot].gl_name || !gl13_have_context()) {
        memset(out, 0, nbytes);
        if (!stat_copy_read_miss++) {
            port_log("port> copy-read: nothing was copied to %p (slot %d); the game "
                     "reads zeros (m415's canvas)\n", dest, slot);
        }
        return;
    }
    w = cache[slot].w;
    h = cache[slot].h;
    half = cache[slot].copy_half ? 1 : 0;
    cw = cache[slot].copy_w ? cache[slot].copy_w : w;
    ch = cache[slot].copy_h ? cache[slot].copy_h : h;
    switch (cache[slot].format) {
        case GX_TF_I8: chan = -1; break;
        case GX_CTF_R8: chan = 0; break;
        case GX_CTF_G8: chan = 1; break;
        case GX_CTF_B8: chan = 2; break;
        case GX_CTF_A8: chan = 3; break;
        default: chan = -2; break;
    }
    if (chan == -2 || (unsigned long)w * (unsigned long)h != nbytes || (w & 7) || (h & 3)) {
        memset(out, 0, nbytes);
        if (!stat_copy_read_miss++) {
            port_log("port> copy-read: the copy at %p is format %u, %dx%d, but the game "
                     "reads %lu bytes as one-byte tiles; zeros\n", dest,
                     (unsigned)cache[slot].format, w, h, nbytes);
        }
        return;
    }
    if (!cache[slot].cpu_rgba) {
        /* M23: the first read of this copy (or a read on a consumed frame
         * before any copy has read itself back): draw the copy's texture
         * at the game's size into the bottom-left of the back buffer as
         * scratch and read that -- 147 KB through glReadPixels rather
         * than a megabyte through glGetTexImage (22 ms on the Radeon;
         * the M21 intro's 120 reads were two resyncs of it).  A consumed
         * frame's back buffer is nobody's picture, and a drawn one begins
         * with the game's clear. */
        cache[slot].cpu_rgba = (u8*)malloc((size_t)w * h * 4);
        if (!cache[slot].cpu_rgba) {
            memset(out, 0, nbytes);
            return;
        }
        {
            double t0 = port_now_seconds();
            gl13_downsample_read(cache[slot].gl_name, cache[slot].su, cache[slot].sv, 0, 0, w, h,
                                 cache[slot].cpu_rgba);
            stat_copy_read_tex++;
            stat_copy_read_tex_s += port_now_seconds() - t0;
        }
    }
    cache[slot].cpu_read = 1; /* from the next copy on, it reads itself back */
    cache[slot].cpu_read_at = stat_efb;
    {
        /* the copy read itself back at the game's size (gl13_downsample_read);
         * GL row 0 is the bottom of the region, so GX row y is GL row h-1-y,
         * one byte a texel in 8x4 tiles.  For I8, intensity as the copy unit
         * computes it (BT.601 luma with the 16 offset, the same as Dolphin's
         * I8 copy). */
        const u8* rgba_s = cache[slot].cpu_rgba;
        for (y = 0; y < h; y++) {
            u8* tile_row = o + (size_t)(y >> 2) * (w >> 3) * 32 + (y & 3) * 8;
            const u8* row = rgba_s + (size_t)(h - 1 - y) * w * 4;
            for (x = 0; x < w; x++) {
                const u8* p = row + (size_t)x * 4;
                unsigned v;
                if (chan >= 0) {
                    v = p[chan];
                } else {
                    v = ((66u * p[0] + 129u * p[1] + 25u * p[2] + 128u) >> 8) + 16u;
                }
                tile_row[(size_t)(x >> 3) * 32 + (x & 7)] = (u8)(v > 255u ? 255u : v);
            }
        }
    }
    if (port_opt.dumpcopy && stat_copy_read < 8) {
        dump_canvas(o, w, h, stat_copy_read);
        port_log("port> --dumpcopy: read-back %u at frame %u: %dx%d canvas from the %dx%d %s copy\n",
                 stat_copy_read, gl13_frame_number(), w, h, cw, ch, half ? "half-scale" : "1:1");
    }
    stat_copy_read++;
}

/* ---- indirect tiling, composed on the CPU --------------------------------- */

/* `HuSprDisp` draws every tiled window background as one quad with an
 * indirect texture stage (src/game/sprput.c:99):
 *
 *     GXSetTexCoordScaleManually(GX_TEXCOORD0, TRUE, mapW*16, mapH*16);
 *     GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP1);
 *     GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_16, GX_ITS_16);
 *     GXSetTevIndTile(GX_TEVSTAGE0, GX_INDTEXSTAGE0, 16,16, 16,16, GX_ITF_4, ...);
 *
 * which on the hardware means, for a texel (X, Y) of the background:
 *
 *     (vs, vt)  = the indirect texture's texel at (X / 16, Y / 16)
 *     result    = the tile sheet's texel at (vs*16 + X%16, vt*16 + Y%16)
 *
 * There is no dependent texture read in GL 1.3 and no fragment program on a
 * Radeon 9000, so §3.4 case 3 gave up on this and drew the direct stage
 * alone -- which is why every window background came out as a pale block of
 * whatever tile happened to be under the quad.
 *
 * But nothing in that formula is per-pixel *state*: both textures are
 * ordinary images in main memory, and the composition is a pure function of
 * their contents.  So it is done once on the CPU, into a cache keyed on the
 * content hash of both images plus the tile geometry, and the result is bound
 * as an ordinary texture.  It is exact rather than approximate, and after the
 * first frame it costs one hash lookup.  The fallback -- a warning and the
 * direct stage alone -- stays for every indirect form that is not this one.
 *
 * Both textures come out of the same `decode` the rest of the cache uses, so
 * this knows nothing about texture formats: an indirect lookup on the console
 * samples the texture unit like any other, and what it gets back is the
 * decoded colour.  GX_ITF_4 then takes the low four bits of each component,
 * component 0 addressing S and component 1 addressing T. */

typedef struct TileEntry {
    u32 key;         /* content hash of both images and the tile geometry */
    unsigned gl_name;
    int w, h, pw, ph;
    float su, sv;
    int param_wrap_s, param_wrap_t, param_min, param_mag;
} TileEntry;

#define TILE_MAX 64
static TileEntry tiles[TILE_MAX];
static int tiles_used;
static unsigned stat_tile_hit, stat_tile_build;

static u32 tex_content_hash(const GXTexObjPort* o, const GXTlutObjPort* tlut) {
    u32 c = fnv(&o->format, sizeof(o->format), 2166136261u);
    c = fnv(&o->width, sizeof(o->width), c);
    c = fnv(&o->height, sizeof(o->height), c);
    if (o->image) {
        c = fnv(o->image, encoded_size(o->format, o->width, o->height), c);
    }
    if (tlut && tlut->lut) {
        c = fnv(tlut->lut, (size_t)tlut->n * 2, c);
    }
    return c;
}

static const GXTlutObjPort* tlut_for(const GXTexObjPort* o) {
    if (o->is_ci && o->tlut_name < 64 && gx.tlut[o->tlut_name].magic == TLUT_MAGIC) {
        return &gx.tlut[o->tlut_name];
    }
    return NULL;
}

/* How many bits of each indirect component the format keeps. */
static u32 ind_mask(u8 fmt) {
    switch (fmt) {
        case GX_ITF_8: return 0xFFu;
        case GX_ITF_5: return 0x1Fu;
        case GX_ITF_4: return 0x0Fu;
        default: return 0x07u; /* GX_ITF_3 */
    }
}

/* Which two numbers a tile-map texel carries.
 *
 * The indirect unit reads its texture as raw texel bits and hands the offset
 * matrix three components; the decoder here has already turned those bits
 * into RGBA, so the components have to be read back out of it.  For an RGB
 * map that is components 0 and 1 -- red and green -- which is the ordinary
 * case and what an indirect *bump* map uses.
 *
 * `HuSprDisp`'s window backgrounds are not that.  Their maps are GX_TF_IA4,
 * one byte a tile, and the byte is `SSSS TTTT` -- so the S component is the
 * *alpha* nibble and the T component the intensity nibble, because that is
 * how IA4 splits a byte.  Measured, not guessed: the file-select window's map
 * is 17x6 and reads
 *
 *     0 7 7 ... 7 1        the nine-slice, S in the alpha nibble,
 *     4 8 8 ... 8 5        T zero throughout, against a 256x32 sheet
 *     2 6 6 ... 6 3        whose nine tiles are a single row.
 *
 * Reading red for S instead gives zero everywhere and one tile stretched over
 * the window, which is what the "pale blocks" of §13.10 were. */
static void ind_components(const GXTexObjPort* map, const u8* texel, u32 mask, int* vs,
                           int* vt) {
    switch (map->format) {
        case GX_TF_I4:
        case GX_TF_I8:
        case GX_TF_IA4:
        case GX_TF_IA8:
            *vs = (int)(((u32)texel[3] >> 4) & mask);
            *vt = (int)(((u32)texel[0] >> 4) & mask);
            return;
        default:
            *vs = (int)((u32)texel[0] & mask);
            *vt = (int)((u32)texel[1] & mask);
            return;
    }
}

int gx_tex_bind_tiled(int unit, GXTexObjPort* sheet, GXTexObjPort* map,
                      const GXIndTile* tile) {
    u32 key;
    int i, slot = -1;
    int ts = tile->ts_s, tt = tile->ts_t;
    int sps = tile->tsp_s, spt = tile->tsp_t;
    int mw, mh, sw, sh, w, h;
    u8 *mrgba = NULL, *srgba = NULL, *out = NULL;
    u32 mask;

    if (!sheet || !map || sheet->magic != TEXOBJ_MAGIC || map->magic != TEXOBJ_MAGIC) {
        return 0;
    }
    if (gl13_draw_off()) {
        return 0; /* --nodraw: composing a background nobody will see */
    }
    if (ts <= 0 || tt <= 0 || sps <= 0 || spt <= 0) {
        return 0;
    }
    mw = map->width;
    mh = map->height;
    w = mw * ts;
    h = mh * tt;
    /* A composed background bigger than this is not a window background, and
     * the point of doing it on the CPU is that it stays small. */
    if (w <= 0 || h <= 0 || (long)w * h > 1024L * 1024L) {
        gx_warn("GXSetTevIndTile: the composed tile map would be larger than "
                "1024x1024; the direct stage is drawn alone");
        return 0;
    }

    key = tex_content_hash(sheet, tlut_for(sheet));
    key = fnv(&map->image, sizeof(map->image), key);
    key = tex_content_hash(map, tlut_for(map)) ^ (key * 16777619u);
    key = fnv(tile, sizeof(*tile), key);

    for (i = 0; i < tiles_used; i++) {
        if (tiles[i].key == key) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        stat_tile_hit++;
    } else {
        int x, y;
        if (tiles_used == TILE_MAX) {
            slot = (int)(key % TILE_MAX);
            if (gl13_live() && tiles[slot].gl_name) {
                GLuint n = tiles[slot].gl_name;
                GL(glDeleteTextures)(1, &n);
            }
            memset(&tiles[slot], 0, sizeof(tiles[slot]));
        } else {
            slot = tiles_used++;
        }
        stat_tile_build++;
        mrgba = decode(map, tlut_for(map), &mw, &mh);
        srgba = decode(sheet, tlut_for(sheet), &sw, &sh);
        if (port_opt.dumptex && stat_tile_build <= 2 && mrgba && srgba) {
            int r, c;
            port_log("port> indtile: map %dx%d fmt %u ci %u, sheet %dx%d fmt %u ci %u,"
                     " tile %dx%d spacing %dx%d fmt %u -> %dx%d\n",
                     mw, mh, (unsigned)map->format, (unsigned)map->is_ci, sw, sh,
                     (unsigned)sheet->format, (unsigned)sheet->is_ci, ts, tt, sps, spt,
                     (unsigned)tile->fmt, w, h);
            {
                char path[1024];
                FILE* f;
                snprintf(path, sizeof(path), "%s/indsheet-%02u-%dx%d.ppm",
                         port_opt.shotdir ? port_opt.shotdir : ".",
                         stat_tile_build, sw, sh);
                f = fopen(path, "wb");
                if (f) {
                    int yy, xx;
                    fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                    for (yy = 0; yy < sh; yy++)
                        for (xx = 0; xx < sw; xx++)
                            fwrite(srgba + ((size_t)yy * sw + xx) * 4, 1, 3, f);
                    fclose(f);
                }
                snprintf(path, sizeof(path), "%s/indmap-%02u-%dx%d.raw",
                         port_opt.shotdir ? port_opt.shotdir : ".",
                         stat_tile_build, mw, mh);
                f = fopen(path, "wb");
                if (f) {
                    fwrite(mrgba, 4, (size_t)mw * mh, f);
                    fclose(f);
                }
            }
            for (r = 0; r < mh && r < 4; r++) {
                char line[512];
                int at = 0;
                for (c = 0; c < mw && c < 12; c++) {
                    const u8* q = mrgba + ((size_t)r * mw + c) * 4;
                    at += snprintf(line + at, sizeof(line) - at, " %02x%02x%02x%02x",
                                   q[0], q[1], q[2], q[3]);
                }
                port_log("port> indtile map row %d:%s\n", r, line);
            }
        }
        if (!mrgba || !srgba) {
            free(mrgba);
            free(srgba);
            memset(&tiles[slot], 0, sizeof(tiles[slot]));
            if (slot == tiles_used - 1) {
                tiles_used--;
            }
            return 0;
        }
        out = (u8*)calloc((size_t)w * h, 4);
        if (!out) {
            free(mrgba);
            free(srgba);
            return 0;
        }
        mask = ind_mask(tile->fmt);
        for (y = 0; y < h; y++) {
            int my = y / tt, iy = y % tt;
            const u8* mrow = mrgba + (size_t)my * mw * 4;
            u8* orow = out + (size_t)y * w * 4;
            for (x = 0; x < w; x++) {
                int mx = x / ts, ix = x % ts;
                const u8* mv = mrow + (size_t)mx * 4;
                int vs, vt, sx, sy;
                ind_components(map, mv, mask, &vs, &vt);
                sx = vs * sps + ix;
                sy = vt * spt + iy;
                {
                u8* o = orow + (size_t)x * 4;
                if (sx >= 0 && sx < sw && sy >= 0 && sy < sh) {
                    memcpy(o, srgba + ((size_t)sy * sw + sx) * 4, 4);
                }
                }
            }
        }
        free(mrgba);
        free(srgba);
        if (port_opt.dumptex && stat_tile_build <= 4) {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof(path), "%s/indtile-%02u-%dx%d.ppm",
                     port_opt.shotdir ? port_opt.shotdir : ".", stat_tile_build, w, h);
            f = fopen(path, "wb");
            if (f) {
                int yy, xx;
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (yy = 0; yy < h; yy++) {
                    for (xx = 0; xx < w; xx++) {
                        fwrite(out + ((size_t)yy * w + xx) * 4, 1, 3, f);
                    }
                }
                fclose(f);
                port_log("port> --dumptex: wrote %s\n", path);
            }
        }
        tiles[slot].key = key;
        tiles[slot].w = w;
        tiles[slot].h = h;
        tiles[slot].pw = pot_up(w);
        tiles[slot].ph = pot_up(h);
        tiles[slot].su = (float)w / (float)tiles[slot].pw;
        tiles[slot].sv = (float)h / (float)tiles[slot].ph;
        tiles[slot].param_wrap_s = -1;
        stat_bytes += (unsigned)(w * h * 4);
        if (gl13_live()) {
            u8* up = out;
            GLuint name = tiles[slot].gl_name;
            if (tiles[slot].pw != w || tiles[slot].ph != h) {
                u8* padded = pad_to_pot(out, w, h, tiles[slot].pw, tiles[slot].ph);
                if (padded) {
                    up = padded;
                    stat_npot++;
                } else {
                    tiles[slot].pw = w;
                    tiles[slot].ph = h;
                    tiles[slot].su = tiles[slot].sv = 1.0f;
                }
            }
            if (!name) {
                GL(glGenTextures)(1, &name);
                tiles[slot].gl_name = name;
            }
            glc_active_texture(unit);
            GL(glBindTexture)(GL_TEXTURE_2D, name);
            glc_note_bind(unit, name);
            (port_opt.glcheck ? gl13_check("glTexImage2D") : 0);
            rt_teximage2d_owned(GL_TEXTURE_2D, 0, GL_RGBA8, tiles[slot].pw,
                                tiles[slot].ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
            if (up == out) {
                out = NULL; /* M27: the upload owns it now */
            }
        }
        free(out);
    }
    if (!gl13_live() || !tiles[slot].gl_name) {
        return 1;
    }
    glc_bind_texture(unit, tiles[slot].gl_name);
    glc_tex_matrix(unit, tiles[slot].su, tiles[slot].sv);
    {
        TileEntry* e = &tiles[slot];
        int ws = (int)gl_wrap(sheet->wrap_s);
        int wt = (int)gl_wrap(sheet->wrap_t);
        int mn = sheet->min_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR;
        int mg = (int)gl_filter(sheet->mag_filt, 0);
        if (e->param_wrap_s != ws || e->param_wrap_t != wt || e->param_min != mn ||
            e->param_mag != mg) {
            e->param_wrap_s = ws;
            e->param_wrap_t = wt;
            e->param_min = mn;
            e->param_mag = mg;
            /* glTexParameter acts on the texture bound to the *active* unit,
             * and glc_bind_texture above says nothing to GL -- not even
             * glActiveTexture -- when the unit already holds this name.  So
             * the parameters went to whichever unit happened to be active:
             * one unit's texture got another's wrap and filter, depending
             * on the order of binds before it.  M16 found it as a 150-pixel
             * disagreement in one eye between two submit shapes that were
             * otherwise identical (PLAN.md 31.3), a latent bug since M5b. */
            glc_active_texture(unit);
            if (gl13_trace_armed()) {
                port_log("gltrace> glTexParameteri unit %d name %u wrap %d %d filt %d %d\n", unit,
                         tiles[slot].gl_name, ws, wt, mn, mg);
            }
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)ws);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wt);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)mn);
            GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)mg);
        }
    }
    return 1;
}

void gx_tex_tile_report(void) {
    if (!stat_tile_hit && !stat_tile_build) {
        return;
    }
    port_log("port> indirect tiling: %u composed backgrounds, %u cache hits\n",
             stat_tile_build, stat_tile_hit);
}

/* Drop the lot: every decoded texture, every composed indirect tile, and the
 * GL names both of them hold.  Two callers, and both of them need it to be
 * this brutal rather than clever:
 *
 *   - drawing coming back on after `--nodraw` (PLAN.md 24.1).  A bind taken
 *     while the renderer was off still *cached* the key -- the hash table does
 *     not know about GL -- but never generated a name or uploaded a texel.  A
 *     later bind would then hit that entry, find `gl_name == 0`, and draw
 *     untextured.  Flushing makes the first drawn frame take the cold-miss
 *     path for everything it binds, which is what makes it byte-identical to
 *     the same frame of a straight run.
 *   - `--restore` (PLAN.md 24.2).  The cache is keyed on addresses in MEM1
 *     and the snapshot has just replaced MEM1 wholesale; every content hash in
 *     it describes texels that are no longer there.
 *
 * It costs one frame of re-uploads, which is why it is never done per frame. */

/* ---- M24: the texture decode on the second core (PLAN.md 39.3) -------------
 *
 * A decode is pure -- source bytes and a palette in, RGBA texels out -- and
 * on a scene's first drawn frame it is 100-150 ms of the 250-350 the frame
 * costs (§38.3).  The drawn frame cannot show a placeholder (the md5s), so
 * the only decode that can move is one that can start EARLY: frame mode
 * drops the GL work of a consumed frame but the setters still run, so a
 * GXLoadTexObj on a consumed frame names a texture the next drawn frame
 * will very probably bind.  The request is noted here (a hash-table lookup
 * on the consumed frame, nothing else); at the retrace the game thread
 * copies the source bytes and the palette into a staging entry -- the
 * worker reads nothing of the game's -- and hands the batch to the decode
 * worker, which hashes and decodes the copies while the game runs on.  A
 * miss on a drawn frame computes the exhaustive hash of the live bytes as
 * it always did (§38.3) and, if a staged entry carries the same key and the
 * same hash, uploads the staged texels instead of decoding: the same
 * function on the same bytes, so the upload is the one the inline decode
 * would have made.  A staged entry the worker has not reached is decoded
 * inline as before (and marked so the worker skips it); one nobody binds
 * within PRE_STALE_FRAMES is dropped.  Built, measured on the walk
 * (PLAN.md 39.3: it reaches the decodes -- 624 of 790 taken -- and the
 * cold frame does not move, the upload of memory-cold texels giving a
 * third of it back and the copies costing the game thread as much as the
 * decode saved), and OFF: --predecode turns it on, --predecodelog names
 * every drawn frame that took or missed a staged decode. */
#define PRE_REQ_MAX 1024
#define PRE_STAGED_MAX 1024
#define PRE_BUDGET_BYTES ((size_t)32 << 20) /* source copies plus RGBA held */
#define PRE_STALE_FRAMES 30
#define PRE_JOBS 8

typedef struct PreReq {
    const void* image;
    const void* lut;
    u32 format;
    u16 w, h;
    u16 lut_n;
    u32 lut_fmt;
    u8 is_ci;
} PreReq;
static PreReq pre_req[PRE_REQ_MAX];
static unsigned pre_nreq;
static unsigned pre_req_dropped;
/* dedupe within a retrace: image pointers seen, a small open-address set */
#define PRE_SEEN 2048
static const void* pre_seen[PRE_SEEN];
static unsigned pre_seen_n;

enum { PRE_FREE = 0, PRE_PENDING, PRE_RUNNING, PRE_DONE, PRE_CLAIMED };
typedef struct Staged {
    volatile int state;
    u8 counted;        /* PRE_DONE seen by the game thread once (the stats) */
    const void* image; /* the key: the game's address, format, size, palette */
    const void* lut;
    u32 format;
    u16 w, h;
    u16 lut_n;
    u32 lut_fmt;
    u8 is_ci;
    u8* src;           /* the copies the worker decodes */
    size_t src_n;
    u8* lutcopy;
    size_t lut_bytes;
    u32 content_full;  /* the worker's exhaustive hash of the copies */
    u8* rgba;          /* decoded, padded to a power of two, unswizzled */
    int dw, dh, pw, ph;
    double decode_s;
    unsigned frame;    /* published at */
} Staged;
static Staged staged[PRE_STAGED_MAX];
static size_t pre_bytes_held;

typedef struct PreJob {
    PortJob job;
    int first, count; /* staged[] indices */
} PreJob;
static PreJob pre_jobs[PRE_JOBS];
static unsigned pre_job_next;

static unsigned stat_pre_requests, stat_pre_published, stat_pre_decoded, stat_pre_taken,
    stat_pre_claimed, stat_pre_stale, stat_pre_hash_miss, stat_pre_budget, stat_pre_nojob;
static unsigned long stat_pre_taken_bytes;
static double stat_pre_taken_s, stat_pre_worker_s, stat_pre_copy_s;
static unsigned frame_pre_taken, frame_pre_claimed;
static double frame_pre_taken_s;

static int predecode_on(void) { return port_threads_on() && port_opt.predecode; }

/* Textures a consumed frame names and no drawn frame binds -- the indirect
 * tiling's sheets and maps (bound through the tile cache, keyed by content),
 * a texture the game loads and never draws -- would be staged every retrace
 * and dropped stale every time.  An image that went stale is not asked for
 * again for PRE_STALE_HOLD frames. */
#define PRE_NEG 512
#define PRE_STALE_HOLD 1800
static struct {
    const void* image;
    unsigned frame;
} pre_neg[PRE_NEG];
static unsigned stat_pre_neg_hits;

static int predecode_neg_probe(const void* image, unsigned frame, int insert) {
    unsigned h = hash_key(image) & (PRE_NEG - 1), k;
    for (k = 0; k < 8; k++) {
        unsigned at = (h + k) & (PRE_NEG - 1);
        if (pre_neg[at].image == image) {
            if (insert) {
                pre_neg[at].frame = frame;
                return 1;
            }
            if (frame < pre_neg[at].frame + PRE_STALE_HOLD) {
                return 1;
            }
            pre_neg[at].image = NULL; /* expired */
            return 0;
        }
        if (insert && (!pre_neg[at].image || frame >= pre_neg[at].frame + PRE_STALE_HOLD)) {
            pre_neg[at].image = image;
            pre_neg[at].frame = frame;
            return 1;
        }
    }
    return 0;
}

/* A consumed frame's GXLoadTexObj: note the texture if the cache does not
 * hold it.  The palette is resolved now, as the bind would resolve it. */
static void predecode_request(const GXTexObjPort* o) {
    const GXTlutObjPort* tlut = NULL;
    int slot, is_efb;
    unsigned h, k;
    if (!o->image || o->width == 0 || o->height == 0) {
        return;
    }
    if (o->is_ci && o->tlut_name < 64 && gx.tlut[o->tlut_name].magic == TLUT_MAGIC) {
        tlut = &gx.tlut[o->tlut_name];
    }
    /* held under any swap table: the bind's key carries the stage's swap,
     * which a setter cannot know, and a staged decode serves every swap
     * (the swizzle is applied at the upload) */
    {
        const void* lut = tlut ? tlut->lut : NULL;
        int i;
        for (i = hash_head[hash_key(o->image)]; i >= 0; i = cache[i].hash_next) {
            const CacheEntry* e = &cache[i];
            if (e->image == o->image &&
                (e->efb || (e->format == o->format && e->w == o->width && e->h == o->height &&
                            e->lut == lut))) {
                return;
            }
        }
        (void)slot;
        (void)is_efb;
    }
    if (predecode_neg_probe(o->image, gl13_frame_number(), 0)) {
        stat_pre_neg_hits++;
        return;
    }
    h = hash_key(o->image) & (PRE_SEEN - 1);
    for (k = 0; k < PRE_SEEN; k++) {
        unsigned at = (h + k) & (PRE_SEEN - 1);
        if (!pre_seen[at]) {
            if (pre_seen_n >= PRE_SEEN / 2 || pre_nreq >= PRE_REQ_MAX) {
                pre_req_dropped++;
                return;
            }
            pre_seen[at] = o->image;
            pre_seen_n++;
            break;
        }
        if (pre_seen[at] == o->image) {
            return; /* already asked for this retrace */
        }
    }
    {
        PreReq* r = &pre_req[pre_nreq++];
        r->image = o->image;
        r->format = o->format;
        r->w = o->width;
        r->h = o->height;
        r->is_ci = o->is_ci;
        r->lut = tlut ? tlut->lut : NULL;
        r->lut_n = tlut ? tlut->n : 0;
        r->lut_fmt = tlut ? tlut->fmt : 0;
        stat_pre_requests++;
    }
}

static size_t staged_bytes(const Staged* e);
static void staged_free(Staged* e) {
    pre_bytes_held -= staged_bytes(e);
    free(e->src);
    free(e->lutcopy);
    free(e->rgba);
    memset(e, 0, sizeof(*e));
}

/* The worker: hash and decode the copies.  No GL, nothing of the game's. */
static void predecode_job_run(PortJob* pj) {
    PreJob* j = (PreJob*)pj;
    int i;
    for (i = j->first; i < j->first + j->count; i++) {
        Staged* e = &staged[i];
        GXTexObjPort o;
        GXTlutObjPort t;
        int w = 0, h = 0;
        u8* rgba;
        double t0;
        if (!__sync_bool_compare_and_swap(&e->state, PRE_PENDING, PRE_RUNNING)) {
            continue; /* the game thread claimed it: it decoded it itself */
        }
        t0 = port_now_seconds();
        memset(&o, 0, sizeof(o));
        o.magic = TEXOBJ_MAGIC;
        o.image = e->src;
        o.width = e->w;
        o.height = e->h;
        o.format = e->format;
        o.is_ci = e->is_ci;
        memset(&t, 0, sizeof(t));
        t.magic = TLUT_MAGIC;
        t.lut = e->lutcopy;
        t.fmt = e->lut_fmt;
        t.n = e->lut_n;
        e->content_full = tex_bind_content_hash_body(&o, e->lutcopy ? &t : NULL, 1);
        rgba = decode(&o, e->lutcopy ? &t : NULL, &w, &h);
        if (rgba) {
            int pw = pot_up(w), ph = pot_up(h);
            if (pw != w || ph != h) {
                u8* padded = pad_to_pot(rgba, w, h, pw, ph);
                if (padded) {
                    free(rgba);
                    rgba = padded;
                } else {
                    pw = w;
                    ph = h;
                }
            }
            e->rgba = rgba;
            e->dw = w;
            e->dh = h;
            e->pw = pw;
            e->ph = ph;
        }
        e->decode_s = port_now_seconds() - t0;
        __sync_synchronize();
        e->state = PRE_DONE;
    }
}

/* A batch of staged entries handed to the worker; if no job slot is free
 * (eight in flight is a stalled worker) they are marked claimed so the
 * binds decode them as before, and nothing leaks. */
static void predecode_publish(int first, int count) {
    PreJob* j = &pre_jobs[pre_job_next % PRE_JOBS];
    int i;
    if (count <= 0) {
        return;
    }
    if (j->job.state == PORT_JOB_IDLE) {
        j->first = first;
        j->count = count;
        j->job.run = predecode_job_run;
        pre_job_next++;
        if (port_worker_submit(port_worker_decode(), &j->job)) {
            return;
        }
    }
    stat_pre_nojob += (unsigned)count;
    for (i = first; i < first + count; i++) {
        if (staged[i].state == PRE_PENDING) {
            staged[i].state = PRE_CLAIMED;
        }
    }
}

/* what a staged entry holds against the budget: the copies and the padded
 * RGBA the worker will produce, known before it does */
static size_t staged_bytes(const Staged* e) {
    return e->src_n + e->lut_bytes + (size_t)pot_up(e->w) * (size_t)pot_up(e->h) * 4;
}

/* The retrace: reap finished jobs, drop stale entries, then publish the
 * consumed frames' requests as a job.  Never waits for the worker: a
 * decode that is not done is not done, and the bind will do it. */
void port_gx_predecode_join(void) {
    unsigned frame = gl13_frame_number();
    unsigned k;
    int i, first = -1, count = 0;
    double t0;
    if (!predecode_on()) {
        pre_nreq = 0;
        return;
    }
    for (k = 0; k < PRE_JOBS; k++) {
        PreJob* j = &pre_jobs[k];
        if (j->job.state == PORT_JOB_DONE) {
            stat_pre_worker_s += j->job.t_end - j->job.t_start;
            j->job.state = PORT_JOB_IDLE;
        }
    }
    for (i = 0; i < PRE_STAGED_MAX; i++) {
        Staged* e = &staged[i];
        if (e->state == PRE_DONE && !e->counted) {
            e->counted = 1;
            stat_pre_decoded++;
        }
        if ((e->state == PRE_DONE || e->state == PRE_CLAIMED) &&
            frame > e->frame + PRE_STALE_FRAMES) {
            if (e->state == PRE_DONE) {
                stat_pre_stale++;
                predecode_neg_probe(e->image, frame, 1);
            }
            staged_free(e);
        }
    }
    if (!pre_nreq) {
        memset(pre_seen, 0, sizeof(pre_seen));
        pre_seen_n = 0;
        return;
    }
    t0 = port_now_seconds();
    for (k = 0; k < pre_nreq; k++) {
        const PreReq* r = &pre_req[k];
        size_t n = encoded_size(r->format, r->w, r->h);
        size_t lb = r->lut ? (size_t)r->lut_n * 2 : 0;
        size_t need = n + lb + (size_t)pot_up(r->w) * (size_t)pot_up(r->h) * 4;
        int is_efb;
        Staged* e;
        /* the cache may have got it since (a drawn frame between) */
        {
            int c, held = 0;
            for (c = hash_head[hash_key(r->image)]; c >= 0; c = cache[c].hash_next) {
                const CacheEntry* ce = &cache[c];
                if (ce->image == r->image &&
                    (ce->efb || (ce->format == r->format && ce->w == r->w && ce->h == r->h &&
                                 ce->lut == r->lut))) {
                    held = 1;
                    break;
                }
            }
            (void)is_efb;
            if (held) {
                continue;
            }
        }
        if (n == 0 || pre_bytes_held + need > PRE_BUDGET_BYTES) {
            stat_pre_budget++;
            continue;
        }
        /* the next free slot; a batch is one contiguous range */
        i = first < 0 ? 0 : first + count;
        while (i < PRE_STAGED_MAX && staged[i].state != PRE_FREE) {
            i++;
        }
        if (i >= PRE_STAGED_MAX) {
            stat_pre_budget++;
            continue;
        }
        if (first >= 0 && i != first + count) {
            predecode_publish(first, count);
            first = -1;
            count = 0;
        }
        if (first < 0) {
            first = i;
        }
        e = &staged[i];
        e->src = (u8*)malloc(n);
        e->lutcopy = lb ? (u8*)malloc(lb) : NULL;
        if (!e->src || (lb && !e->lutcopy)) {
            free(e->src);
            free(e->lutcopy);
            memset(e, 0, sizeof(*e));
            stat_pre_budget++;
            if (count == 0) {
                first = -1;
            }
            continue;
        }
        memcpy(e->src, r->image, n);
        if (lb) {
            memcpy(e->lutcopy, r->lut, lb);
        }
        e->src_n = n;
        e->lut_bytes = lb;
        e->image = r->image;
        e->lut = r->lut;
        e->format = r->format;
        e->w = r->w;
        e->h = r->h;
        e->lut_n = r->lut_n;
        e->lut_fmt = r->lut_fmt;
        e->is_ci = r->is_ci;
        e->frame = frame;
        e->rgba = NULL;
        e->pw = e->ph = 0;
        e->counted = 0;
        pre_bytes_held += staged_bytes(e);
        e->state = PRE_PENDING;
        count++;
        stat_pre_published++;
    }
    predecode_publish(first, count);
    stat_pre_copy_s += port_now_seconds() - t0;
    pre_nreq = 0;
    memset(pre_seen, 0, sizeof(pre_seen));
    pre_seen_n = 0;
}

/* The miss path's question: is there a staged decode of exactly these
 * bytes?  Returns the entry (taken out of the table) or NULL; a pending
 * entry the worker has not reached is claimed so the worker skips it. */
static Staged* predecode_take(const GXTexObjPort* o, const void* lut, u32 content_full) {
    int i;
    for (i = 0; i < PRE_STAGED_MAX; i++) {
        Staged* e = &staged[i];
        int st = e->state;
        if (st == PRE_FREE || e->image != o->image || e->format != o->format || e->w != o->width ||
            e->h != o->height || e->lut != lut) {
            continue;
        }
        if (st == PRE_PENDING) {
            if (__sync_bool_compare_and_swap(&e->state, PRE_PENDING, PRE_CLAIMED)) {
                stat_pre_claimed++;
                frame_pre_claimed++;
                return NULL;
            }
            st = e->state;
        }
        if (st == PRE_RUNNING) {
            stat_pre_claimed++;
            frame_pre_claimed++;
            return NULL; /* the worker is on it; the bind does not wait */
        }
        if (st == PRE_DONE) {
            __sync_synchronize();
            if (e->content_full != content_full || !e->rgba) {
                stat_pre_hash_miss++;
                staged_free(e);
                return NULL;
            }
            return e;
        }
        return NULL;
    }
    return NULL;
}

/* The upload of a staged decode: what tex_bind_decode_and_upload does after
 * its decode, on the worker's texels (swizzled here if the stage asks). */
static void tex_bind_upload_staged(int slot, int unit, const GXTexObjPort* o, Staged* e) {
    double t1 = port_now_seconds();
    u8* up = e->rgba;
    int pw = e->pw, ph = e->ph, w = e->dw, h = e->dh;
    e->rgba = NULL;
    cache[slot].su = cache[slot].sv = 1.0f;
    if (cache[slot].swap != GX_SWAP_IDENTITY) {
        swizzle_rgba(up, pw, ph, cache[slot].swap);
    }
    frame_decodes++;
    stat_decodes++;
    frame_src_bytes += (unsigned)encoded_size(o->format, o->width, o->height);
    frame_rgba_bytes += (unsigned)(w * h * 4);
    stat_bytes += (unsigned)(w * h * 4);
    if (pw != w || ph != h) {
        cache[slot].su = (float)w / (float)pw;
        cache[slot].sv = (float)h / (float)ph;
        stat_npot++;
    }
    if (gl13_live()) {
        GLuint name = cache[slot].gl_name;
        if (!name) {
            GL(glGenTextures)(1, &name);
            cache[slot].gl_name = name;
        }
        glc_active_texture(unit);
        if (gl13_trace_armed()) {
            port_log("gltrace> upload unit %d name %u %dx%d img %p (staged)\n", unit, name, pw, ph,
                     o->image);
        }
        GL(glBindTexture)(GL_TEXTURE_2D, name);
        glc_note_bind(unit, name);
        (port_opt.glcheck ? gl13_check("glTexImage2D") : 0);
        rt_teximage2d_owned(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
        up = NULL; /* M27: the upload owns it now */
        cache[slot].param_wrap_s = -1;
        cache_gl_bytes -= cache[slot].gl_bytes;
        cache[slot].gl_bytes = (unsigned)(pw * ph * 4);
        cache_gl_bytes += cache[slot].gl_bytes;
        frame_upload_s += port_now_seconds() - t1;
    }
    stat_pre_taken++;
    stat_pre_taken_bytes += (unsigned long)pw * ph * 4;
    stat_pre_taken_s += e->decode_s;
    frame_pre_taken++;
    frame_pre_taken_s += e->decode_s;
    free(up);
    staged_free(e);
}

void gx_tex_predecode_frame_take(unsigned* taken, unsigned* claimed, unsigned* unstaged,
                                 double* saved_ms) {
    *taken = frame_pre_taken;
    *claimed = frame_pre_claimed;
    *unstaged = frame_pre_unstaged;
    *saved_ms = frame_pre_taken_s * 1000.0;
    frame_pre_taken = frame_pre_claimed = frame_pre_unstaged = 0;
    frame_pre_taken_s = 0.0;
}

void gx_tex_predecode_report(void) {
    if (!stat_pre_requests) {
        return;
    }
    port_log("port> predecode (M24): %u requests on consumed frames (%u dropped), %u published "
             "(%.0f ms of copies on the game thread), %u decoded by the worker (%.0f ms), "
             "%u taken at a bind (%lu KB, %.0f ms of decode saved), %u claimed before the "
             "worker reached them, %u stale (%u requests held back after one), %u hash "
             "mismatches, %u over budget, %u without a job\n",
             stat_pre_requests, pre_req_dropped, stat_pre_published, stat_pre_copy_s * 1000.0,
             stat_pre_decoded, stat_pre_worker_s * 1000.0, stat_pre_taken,
             stat_pre_taken_bytes / 1024, stat_pre_taken_s * 1000.0, stat_pre_claimed,
             stat_pre_stale, stat_pre_neg_hits, stat_pre_hash_miss, stat_pre_budget,
             stat_pre_nojob);
}

void gx_tex_flush_all(void) {
    int i;
    for (i = 0; i < (int)cache_used; i++) {
        if (gl13_live() && cache[i].gl_name) {
            GLuint n = cache[i].gl_name;
            GL(glDeleteTextures)(1, &n);
        }
    }
    for (i = 0; i < tiles_used; i++) {
        if (gl13_live() && tiles[i].gl_name) {
            GLuint n = tiles[i].gl_name;
            GL(glDeleteTextures)(1, &n);
        }
    }
    memset(cache, 0, sizeof(cache));
    memset(tiles, 0, sizeof(tiles));
    cache_used = 0;
    tiles_used = 0;
    cache_gl_bytes = 0;
    nfree_slots = 0;
    for (i = 0; i < (int)TEX_HASH_SIZE; i++) {
        hash_head[i] = -1;
    }
    cache_epoch = 0;
    cache_epoch_frame = 0;
    cache_epoch_started = 0;
    glc_invalidate();
}
