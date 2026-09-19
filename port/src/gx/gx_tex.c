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
        case GX_TL_IA8:
            out[0] = out[1] = out[2] = (u8)(v >> 8);
            out[3] = (u8)(v & 0xFF);
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
                                    d[0] = d[1] = d[2] = (u8)(v >> 8);
                                    d[3] = (u8)(v & 0xFF);
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

typedef struct CacheEntry {
    const void* image;
    const void* lut;
    u32 format;
    u16 w, h;
    u32 content;
    unsigned gl_name;
    u8 wrap_s, wrap_t, min_filt, mag_filt;
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
} CacheEntry;

#define CACHE_MAX 2048
static CacheEntry cache[CACHE_MAX];
static int cache_used;
static size_t cache_gl_bytes; /* sum of cache[].gl_bytes: what the driver holds */
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
static size_t tex_budget_bytes = (size_t)40 << 20;
static int free_slots[CACHE_MAX];
static int nfree_slots;
static unsigned stat_budget_evict;
static size_t stat_budget_evict_bytes;
static unsigned cache_frame;  /* the frame gx_tex_bind last saw */

void gx_tex_set_budget_mb(int mb) { tex_budget_bytes = (size_t)(mb > 0 ? mb : 0) << 20; }

static void cache_free_slot(int slot) {
    hash_remove(slot);
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
static unsigned stat_copy_front, stat_copy_region_front;
static unsigned stat_copy_kept; /* M21: clear-after copies on consumed frames, kept */
/* How many times an already-cached slot's content hash was actually
 * recomputed to check for an in-place rewrite -- as opposed to a pure
 * epoch-cached hit, which touches none of the texel bytes at all.  This is
 * the number the per-frame epoch is supposed to shrink. */
static unsigned stat_revalidate;

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

void gx_tex_report(void) {
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
    port_log("port> texture hash: %u KB hashed in full, %u KB sampled, %u "
             "revalidations (of %u binds)%s%s\n",
             stat_hash_full / 1024, stat_hash_sampled / 1024, stat_revalidate,
             stat_hit + stat_miss + stat_evict,
             port_opt.texhash_full ? " (--texhash-full: epoch+sampling bypassed)" : "",
             validate_every_bind ? " (--texvalidate-every-bind: epoch bypassed)" : "");
    if (stat_efb) {
        port_log("port> EFB copies: %u colour copies into the cache (on consumed frames "
                 "from the front buffer: %u whole-screen, %u region; %u clear-after copies "
                 "kept from the last drawn frame, M21)\n",
                 stat_efb, stat_copy_front, stat_copy_region_front, stat_copy_kept);
    }
    if (stat_copy_read || stat_copy_read_miss) {
        port_log("port> copy-read: %u copies read back by the game (m415's canvas), "
                 "%u answered with zeros\n", stat_copy_read, stat_copy_read_miss);
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
static u32 tex_bind_content_hash(const GXTexObjPort* o, const GXTlutObjPort* tlut,
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
    u8* rgba = decode(o, tlut, &w, &h);
    cache[slot].su = cache[slot].sv = 1.0f;
    if (rgba) {
        swizzle_rgba(rgba, w, h, cache[slot].swap);
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
            GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, up);
            /* A fresh name has the GL default filter state, which is
             * mipmapped and therefore incomplete here; force the
             * parameters to be re-emitted for it. */
            cache[slot].param_wrap_s = -1;
            cache_gl_bytes -= cache[slot].gl_bytes;
            cache[slot].gl_bytes = (unsigned)(pw * ph * 4);
            cache_gl_bytes += cache[slot].gl_bytes;
        }
        if (up != rgba) {
            free(up);
        }
        free(rgba);
    }
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
        if (!gl13_live() || !o->gl_name) {
            return;
        }
        stat_hit++;
        glc_bind_texture(unit, o->gl_name);
        glc_tex_matrix(unit, cache[slot].su, cache[slot].sv);
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
                tex_bind_decode_and_upload(slot, unit, o, tlut);
            }
        }
        tex_bind_finish(unit, o, slot);
        return;
    }

    /* A genuine miss: no entry for this (image, format, w, h, lut) key. */
    {
        u32 content = tex_bind_content_hash(o, tlut, 1 /* first sight: exhaustive */);
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
        cache[slot].validated_epoch = cache_epoch;
        cache[slot].last_used = frame;
        hash_insert(slot);
        tex_bind_decode_and_upload(slot, unit, o, tlut);
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

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    /* Compare-first (M22): the same texture loaded again -- the next object
     * of the same material -- changes nothing the pending batch reads.  The
     * fields from gl_name on are the cache's, filled in at bind time in the
     * copy and zero in the game's object, so they are not compared. */
    GX_STATE_TOUCH_IF(GX_CMP_TEV, (unsigned)id >= GX_TEX_UNITS || !obj ||
                                      memcmp(&gx.bound[id], obj, offsetof(GXTexObjPort, gl_name)) != 0);
    /* Copy, do not alias: see the comment on GXState::bound.  This is what
     * the console's write to the texture registers is, and the game's sprite
     * path depends on it -- HuSprTexLoad's GXTexObj is a stack local. */
    if ((unsigned)id < GX_TEX_UNITS && obj) {
        gx.bound[id] = *(const GXTexObjPort*)obj;
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

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) {
    GXTlutObjPort* t = (GXTlutObjPort*)obj;
    GX_STATE_TOUCH_IF(GX_CMP_TEV, tlut_name >= 64 || t->magic != TLUT_MAGIC ||
                                      memcmp(&gx.tlut[tlut_name], t, sizeof(*t)) != 0);
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

void gx_tex_copy(void* dest, int clear) {
    static unsigned copies, depth_dropped;
    int sl, st, sw, sh, dw, dh, pw, ph, i, slot = -1;
    int from_front = 0;
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

    pw = pot_up(dw);
    ph = pot_up(dh);
    {
        GLuint name = cache[slot].gl_name;
        if (!name) {
            GL(glGenTextures)(1, &name);
            cache[slot].gl_name = name;
            cache[slot].param_wrap_s = -1;
        }
        glc_active_texture(0);
        GL(glBindTexture)(GL_TEXTURE_2D, name);
        glc_note_bind(0, name);
        /* glCopyTexImage2D would be one call, but it is not in the GL 1.3
         * subset this backend has written down and the destination has to be a
         * power of two anyway, so the texture is sized once with a null
         * glTexImage2D and refilled with glCopyTexSubImage2D thereafter --
         * which is also cheaper, because it does not reallocate. */
        if (cache[slot].param_wrap_s == -1) {
            GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, NULL);
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
        if (from_front) {
            GL(glReadBuffer)(GL_FRONT);
        }
        GL(glCopyTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, sl, 480 - (st + sh),
                                sw < dw ? sw : dw, sh < dh ? sh : dh);
        if (from_front) {
            GL(glReadBuffer)(GL_BACK);
        }
    }
    cache[slot].su = (float)dw / (float)pw;
    cache[slot].sv = (float)dh / (float)ph;
    stat_efb++;
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
void port_gx_copy_read(const void* dest, void* out, unsigned long nbytes) {
    int slot, is_efb = 0, w, h, pw, ph, x, y, chan;
    u8* rgba;
    u8* o = (u8*)out;
    GLuint name;

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
    pw = pot_up(w);
    ph = pot_up(h);
    rgba = (u8*)malloc((size_t)pw * ph * 4);
    if (!rgba) {
        memset(out, 0, nbytes);
        return;
    }
    name = cache[slot].gl_name;
    glc_active_texture(0);
    GL(glBindTexture)(GL_TEXTURE_2D, name);
    glc_note_bind(0, name);
    GL(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
    GL(glGetTexImage)(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* gx_tex_copy wrote the region into the bottom-left of the padded
     * texture, GL row 0 = the bottom of the copied region, so GX row y is GL
     * row h-1-y.  For I8, intensity as the copy unit computes it (BT.601 luma
     * with the 16 offset, the same as Dolphin's I8 copy). */
    for (y = 0; y < h; y++) {
        const u8* row = rgba + ((size_t)(h - 1 - y) * pw) * 4;
        u8* tile_row = o + (size_t)(y >> 2) * (w >> 3) * 32 + (y & 3) * 8;
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
    free(rgba);
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
            GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, tiles[slot].pw,
                             tiles[slot].ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
            if (up != out) {
                free(up);
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
