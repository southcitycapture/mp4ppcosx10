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
    int used;
    /* The Radeon 9000 has no ARB_texture_non_power_of_two, so an NPOT texture
     * is uploaded into the next power of two up and these are the fractions of
     * it that hold real texels.  1.0 for the overwhelmingly common POT case. */
    float su, sv;
} CacheEntry;

#define CACHE_MAX 2048
static CacheEntry cache[CACHE_MAX];
static int cache_used;
static unsigned stat_hit, stat_miss, stat_evict, stat_bytes, stat_npot;

void gx_tex_init(void) {
    memset(cache, 0, sizeof(cache));
    cache_used = 0;
}

void gx_tex_report(void) {
    if (!stat_hit && !stat_miss) {
        return;
    }
    port_log("port> texture cache: %u hits, %u misses, %u re-uploads, %u entries, "
             "%u KB decoded, %u padded to a power of two\n",
             stat_hit, stat_miss, stat_evict, (unsigned)cache_used, stat_bytes / 1024,
             stat_npot);
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

void gx_tex_bind(int unit, GXTexObjPort* o) {
    const GXTlutObjPort* tlut = NULL;
    u32 content;
    int i, slot = -1;
    if (!o || o->magic != TEXOBJ_MAGIC) {
        return;
    }
    if (o->is_ci && o->tlut_name < 64 && gx.tlut[o->tlut_name].magic == TLUT_MAGIC) {
        tlut = &gx.tlut[o->tlut_name];
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

    content = fnv(&o->format, sizeof(o->format), 2166136261u);
    content = fnv(&o->width, sizeof(o->width), content);
    content = fnv(&o->height, sizeof(o->height), content);
    if (o->image) {
        size_t n = encoded_size(o->format, o->width, o->height);
        content = fnv(o->image, n, content);
    }
    if (tlut && tlut->lut) {
        content = fnv(tlut->lut, (size_t)tlut->n * 2, content);
    }

    for (i = 0; i < cache_used; i++) {
        if (cache[i].image == o->image && cache[i].format == o->format &&
            cache[i].w == o->width && cache[i].h == o->height &&
            cache[i].lut == (tlut ? tlut->lut : NULL)) {
            slot = i;
            break;
        }
    }
    if (slot >= 0 && cache[slot].content == content) {
        stat_hit++;
    } else {
        int w = 0, h = 0;
        u8* rgba = decode(o, tlut, &w, &h);
        if (slot < 0) {
            if (cache_used == CACHE_MAX) {
                slot = (int)(content % CACHE_MAX); /* an eviction, not a leak */
                if (gl13_live() && cache[slot].gl_name) {
                    GLuint n = cache[slot].gl_name;
                    GL(glDeleteTextures)(1, &n);
                }
                memset(&cache[slot], 0, sizeof(cache[slot]));
            } else {
                slot = cache_used++;
            }
            stat_miss++;
        } else {
            stat_evict++;
        }
        cache[slot].image = o->image;
        cache[slot].lut = tlut ? tlut->lut : NULL;
        cache[slot].format = o->format;
        cache[slot].w = o->width;
        cache[slot].h = o->height;
        cache[slot].content = content;
        cache[slot].su = cache[slot].sv = 1.0f;
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
                GL(glBindTexture)(GL_TEXTURE_2D, name);
                GL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, up);
            }
            if (up != rgba) {
                free(up);
            }
            free(rgba);
        }
    }
    o->gl_name = cache[slot].gl_name;
    if (!gl13_live() || !o->gl_name) {
        return;
    }
    GL(glActiveTexture)(GL_TEXTURE0 + unit);
    GL(glBindTexture)(GL_TEXTURE_2D, o->gl_name);
    /* Fold the NPOT padding into this unit's texture matrix, so the vertex
     * decoder keeps emitting the game's own 0..1 texcoords and knows nothing
     * about it.  GL_TEXTURE is otherwise unused by this backend -- texgen is
     * done on the CPU (PLAN.md §10.5) -- so the matrix is ours to spend. */
    {
        float m[16];
        memset(m, 0, sizeof(m));
        m[0] = cache[slot].su;
        m[5] = cache[slot].sv;
        m[10] = 1.0f;
        m[15] = 1.0f;
        GL(glMatrixMode)(GL_TEXTURE);
        GL(glLoadMatrixf)(m);
        GL(glMatrixMode)(GL_MODELVIEW);
    }
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)gl_wrap(o->wrap_s));
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)gl_wrap(o->wrap_t));
    /* No mip levels are uploaded, so a mipmapped min filter would make the
     * texture incomplete; fall back to its non-mipmapped equivalent. */
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        (GLint)(o->min_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR));
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        (GLint)gl_filter(o->mag_filt, 0));
}

/* ---- the GX texture-object entry points ----------------------------------- */

void GXInitTexObj(GXTexObj* obj, void* image, u16 w, u16 h, GXTexFmt fmt,
                  GXTexWrapMode ws, GXTexWrapMode wt, u8 mipmap) {
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

void GXInitTlutObj(GXTlutObj* obj, void* lut, GXTlutFmt fmt, u16 n) {
    GXTlutObjPort* t = (GXTlutObjPort*)obj;
    t->magic = TLUT_MAGIC;
    t->lut = lut;
    t->fmt = (u32)fmt;
    t->n = n;
}

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) {
    GXTlutObjPort* t = (GXTlutObjPort*)obj;
    if (tlut_name < 64 && t->magic == TLUT_MAGIC) {
        gx.tlut[tlut_name] = *t;
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
void GXInvalidateTexAll(void) {}
void GXInvalidateTexRegion(GXTexRegion* r) { (void)r; }

/* ---- EFB copies ----------------------------------------------------------- */
/* No FBO on this card, so an EFB copy is glCopyTexSubImage2D out of the back
 * buffer, which is what fixed-function-era engines did.  The formats the game
 * copies in are GX_CTF_R8, GX_CTF_A8 and the two depth ones. */

void gx_tex_copy(void* dest, int clear) {
    static unsigned copies;
    copies++;
    if (!gl13_live()) {
        return;
    }
    if (gx.tex_dst_fmt == GX_TF_Z24X8 || gx.tex_dst_fmt == GX_TF_Z8) {
        if (!gl13_have_depth_texture) {
            gx_warn("GXCopyTex of a depth format without ARB_depth_texture: dropped");
            return;
        }
    }
    /* The destination is an EFB-copy texture the game will bind through a
     * GXTexObj pointing at `dest`; the cache keys on that address, so the
     * copy is recorded as a decoded RGBA texture under the same key. */
    gx_warn("GXCopyTex: EFB copies are recorded but not yet read back into the "
            "texture cache (M3)");
    (void)dest;
    (void)clear;
}

void GXCopyTex(void* dest, GXBool clear) { gx_tex_copy(dest, clear ? 1 : 0); }
