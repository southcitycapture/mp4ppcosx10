/* A baseline JPEG decoder in the shape THP needs (M38, PLAN.md 53).
 *
 * THP video is motion JPEG with a few Nintendo simplifications, all of which
 * this decoder relies on and checks:
 *
 *   - baseline sequential Huffman (SOF0), 8-bit samples, three components,
 *     Y at 2x2 and Cb/Cr at 1x1 (4:2:0), one interleaved scan;
 *   - **no byte stuffing**: the entropy-coded segment is read as raw bits,
 *     exactly as `__THPPrepBitStream` / `__THPHuffDecodeTab` in
 *     src/dolphin/thp/THPDec.c do -- a 0xFF in the scan is data, not a
 *     marker (ffmpeg's mjpegdec special-cases AV_CODEC_ID_THP the same way);
 *   - a restart interval (DRI) is honoured the way THPDec.c honours it:
 *     every Ri MCUs the bit position moves to the next byte boundary and the
 *     DC predictors reset (no RST marker bytes are skipped, because THPDec.c
 *     skips none; none of Mario Party 4's twelve movies has a DRI at all);
 *   - width and height multiples of 16.
 *
 * The output is what `THPVideoDecode` hands the game: three **GX I8 tiled**
 * planes (8x4 texel tiles of 32 bytes, tile rows `width*4` bytes apart), Y
 * at full size and U/V at half size in each axis.  An 8x8 IDCT block is two
 * vertically adjacent tiles, so each block is written as two contiguous
 * 32-byte runs -- the tiling costs nothing.
 *
 * The inverse DCT is the integer one of stb_image (Sean Barrett, public
 * domain / MIT, itself after the IJG's jidctint "islow" -- see
 * port/docs/licences/stb_image.txt).  THPDec.c's is a float AAN; the two
 * round differently by a level here and there (PLAN.md 53 measures it).
 *
 * Nothing here allocates, calls the SDK, or logs: it runs on the decode
 * worker (workers.c) as well as on the game thread.
 */
#include "thp_jpeg.h"

#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;
typedef signed int s32;

#define FAST_BITS 9

typedef struct {
    u8 fast[1 << FAST_BITS];  /* index into values, 255 = slow path */
    u16 code[256];
    u8 values[256];
    u8 size[257];
    u32 maxcode[18];
    int delta[17];
    /* AC fast path (stb's fast_ac): run, size and the extended value folded
     * into one entry for the codes that fit FAST_BITS with their magnitude */
    s16 fast_ac[1 << FAST_BITS];
    int valid;
} Huff;

typedef struct {
    const u8* p;
    const u8* end;
    u32 buf;   /* bits left-aligned */
    int n;     /* valid bits in buf */
} Bits;

static const u8 dezigzag[64 + 15] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* runs past the end land here */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

static const u32 bmask[17] = {0,    1,    3,     7,     15,    31,    63,    127,  255,
                              511,  1023, 2047,  4095,  8191,  16383, 32767, 65535};
static const int jbias[16] = {0,    -1,   -3,    -7,    -15,   -31,   -63,   -127,
                              -255, -511, -1023, -2047, -4095, -8191, -16383, -32767};

static int huff_build(Huff* h, const u8* counts) {
    int i, j, k = 0;
    u32 code;
    for (i = 0; i < 16; ++i) {
        for (j = 0; j < counts[i]; ++j) {
            if (k >= 256) return 0;
            h->size[k++] = (u8)(i + 1);
        }
    }
    h->size[k] = 0;
    code = 0;
    k = 0;
    for (j = 1; j <= 16; ++j) {
        h->delta[j] = k - (int)code;
        if (h->size[k] == j) {
            while (h->size[k] == j) h->code[k++] = (u16)(code++);
            if (code - 1 >= (1u << j)) return 0;
        }
        h->maxcode[j] = code << (16 - j);
        code <<= 1;
    }
    h->maxcode[j] = 0xffffffff;
    memset(h->fast, 255, sizeof(h->fast));
    for (i = 0; i < k; ++i) {
        int s = h->size[i];
        if (s <= FAST_BITS) {
            int c = h->code[i] << (FAST_BITS - s);
            int m = 1 << (FAST_BITS - s);
            for (j = 0; j < m; ++j) h->fast[c + j] = (u8)i;
        }
    }
    return 1;
}

static void huff_build_fast_ac(Huff* h) {
    int i;
    for (i = 0; i < (1 << FAST_BITS); ++i) {
        u8 fast = h->fast[i];
        h->fast_ac[i] = 0;
        if (fast < 255) {
            int rs = h->values[fast];
            int run = (rs >> 4) & 15;
            int magbits = rs & 15;
            int len = h->size[fast];
            if (magbits && len + magbits <= FAST_BITS) {
                int k = ((i << len) & ((1 << FAST_BITS) - 1)) >> (FAST_BITS - magbits);
                int m = 1 << (magbits - 1);
                if (k < m) k += (~0U << magbits) + 1;
                if (k >= -128 && k <= 127)
                    h->fast_ac[i] = (s16)((k * 256) + (run * 16) + (len + magbits));
            }
        }
    }
}

/* ---- bits: raw, no unstuffing (THP) ---- */
static inline void bits_fill(Bits* b) {
    while (b->n <= 24) {
        u32 c = b->p < b->end ? *b->p++ : 0;
        b->buf |= c << (24 - b->n);
        b->n += 8;
    }
}

static inline int huff_decode(Bits* b, const Huff* h) {
    unsigned c;
    int s, k;
    if (b->n < 16) bits_fill(b);
    c = b->buf >> (32 - FAST_BITS);
    k = h->fast[c];
    if (k < 255) {
        s = h->size[k];
        b->buf <<= s;
        b->n -= s;
        return h->values[k];
    }
    {
        u32 t = b->buf >> 16;
        for (k = FAST_BITS + 1;; ++k)
            if (t < h->maxcode[k]) break;
        if (k == 17) {
            b->n -= 16;
            return -1;
        }
        c = ((b->buf >> (32 - k)) & bmask[k]) + h->delta[k];
        b->buf <<= k;
        b->n -= k;
        return h->values[c];
    }
}

static inline int extend_receive(Bits* b, int n) {
    unsigned k;
    int sgn;
    if (b->n < n) bits_fill(b);
    sgn = (s32)b->buf >> 31;
    k = (b->buf << n) | (b->buf >> (32 - n)); /* rotl */
    b->buf = k & ~bmask[n];
    k &= bmask[n];
    b->n -= n;
    return (int)k + (jbias[n] & ~sgn);
}

static int decode_block(Bits* b, short data[64], const Huff* hdc, const Huff* hac, int* pred,
                        const u16* dq) {
    int diff, dc, k, t;
    if (b->n < 16) bits_fill(b);
    t = huff_decode(b, hdc);
    if (t < 0 || t > 15) return 0;
    memset(data, 0, 64 * sizeof(data[0]));
    diff = t ? extend_receive(b, t) : 0;
    dc = *pred + diff;
    *pred = dc;
    data[0] = (short)(dc * dq[0]);
    k = 1;
    do {
        unsigned zig;
        int c, r, s;
        if (b->n < 16) bits_fill(b);
        c = b->buf >> (32 - FAST_BITS);
        r = hac->fast_ac[c];
        if (r) {
            k += (r >> 4) & 15;
            s = r & 15;
            b->buf <<= s;
            b->n -= s;
            zig = dezigzag[k];
            data[zig] = (short)((r >> 8) * dq[k]);
            k++;
        } else {
            int rs = huff_decode(b, hac);
            if (rs < 0) return 0;
            s = rs & 15;
            r = rs >> 4;
            if (s == 0) {
                if (rs != 0xf0) break; /* end of block */
                k += 16;
            } else {
                k += r;
                zig = dezigzag[k];
                data[zig] = (short)(extend_receive(b, s) * dq[k]);
                k++;
            }
        }
    } while (k < 64);
    return 1;
}

/* ---- the inverse DCT (stb_image's, after jidctint) ---- */
#define f2f(x) ((int)(((x)*4096 + 0.5)))
#define fsh(x) ((x)*4096)

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)       \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2;                                         \
    p3 = s6;                                         \
    p1 = (p2 + p3) * f2f(0.5411961f);                \
    t2 = p1 + p3 * f2f(-1.847759065f);               \
    t3 = p1 + p2 * f2f(0.765366865f);                \
    p2 = s0;                                         \
    p3 = s4;                                         \
    t0 = fsh(p2 + p3);                               \
    t1 = fsh(p2 - p3);                               \
    x0 = t0 + t3;                                    \
    x3 = t0 - t3;                                    \
    x1 = t1 + t2;                                    \
    x2 = t1 - t2;                                    \
    t0 = s7;                                         \
    t1 = s5;                                         \
    t2 = s3;                                         \
    t3 = s1;                                         \
    p3 = t0 + t2;                                    \
    p4 = t1 + t3;                                    \
    p1 = t0 + t3;                                    \
    p2 = t1 + t2;                                    \
    p5 = (p3 + p4) * f2f(1.175875602f);              \
    t0 = t0 * f2f(0.298631336f);                     \
    t1 = t1 * f2f(2.053119869f);                     \
    t2 = t2 * f2f(3.072711026f);                     \
    t3 = t3 * f2f(1.501321110f);                     \
    p1 = p5 + p1 * f2f(-0.899976223f);               \
    p2 = p5 + p2 * f2f(-2.562915447f);               \
    p3 = p3 * f2f(-1.961570560f);                    \
    p4 = p4 * f2f(-0.390180644f);                    \
    t3 += p1 + p4;                                   \
    t2 += p2 + p3;                                   \
    t1 += p2 + p4;                                   \
    t0 += p1 + p3;

static inline u8 clamp8(int x) {
    if ((unsigned)x > 255) return x < 0 ? 0 : 255;
    return (u8)x;
}

/* out0: rows 0-3, out1: rows 4-7, `stride` bytes between rows (8 when the
 * plane is GX-tiled: a tile is four 8-byte rows, the block's lower half is
 * the next tile row down) */
static void idct_block(u8* out0, u8* out1, int stride, short data[64]) {
    int i, val[64], *v = val;
    short* d = data;
    for (i = 0; i < 8; ++i, ++d, ++v) {
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 && d[40] == 0 && d[48] == 0 &&
            d[56] == 0) {
            int dcterm = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dcterm;
        } else {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            x0 += 512;
            x1 += 512;
            x2 += 512;
            x3 += 512;
            v[0] = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
        }
    }
    for (i = 0, v = val; i < 8; ++i, v += 8) {
        u8* o = i < 4 ? out0 + i * stride : out1 + (i - 4) * stride;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        /* +128 and truncate, not round: the console stores the IDCT's
         * floats through a u8 quantised psq_st, which truncates (THPDec.c;
         * PLAN.md 53.9 measured round-to-nearest a level bright) */
        x0 += (128 << 17);
        x1 += (128 << 17);
        x2 += (128 << 17);
        x3 += (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17);
        o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17);
        o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17);
        o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17);
        o[4] = clamp8((x3 - t0) >> 17);
    }
}

/* ---- the frame ---- */
typedef struct {
    int w, h;
    u16 q[4][64];
    int qvalid;
    Huff huff[4]; /* DC0, AC0, DC1, AC1 as THPDec.c orders them: (sel<<1)+ac */
    int comp_q[3], comp_dc[3], comp_ac[3];
    int ri;
} Frame;

static inline int be16(const u8* p) { return (p[0] << 8) | p[1]; }

struct ThpjCtx {
    Frame fr;
    int urow[THPJ_MAX_W], vrow[THPJ_MAX_W], vtmp[THPJ_MAX_W / 2];
    /* M39 (PLAN.md 54.2): the scan in progress, so a decode can stop after
     * any MCU row and go on later (thpj_decode_rows) -- the one-CPU path
     * spends the schedule's slack on it a row at a time */
    Bits b;
    const u8* src;
    u8 *ty, *tu, *tv;
    int pred[3], togo, my, tiled;
    ThpjStats* stats;
};

unsigned thpj_ctx_size(void) { return (unsigned)sizeof(ThpjCtx); }

int thpj_decode_begin(ThpjCtx* ctx, const unsigned char* src, unsigned size,
                      unsigned char* tileY, unsigned char* tileU, unsigned char* tileV,
                      int want_w, int want_h, int tiled, ThpjStats* stats) {
    /* ~12 KB of tables: in the caller's context, never on a coroutine stack */
    Frame* fr = &ctx->fr;
    const u8* p = src;
    const u8* end = src + size;
    int sof = 0;
    fr->ri = 0;
    fr->huff[0].valid = fr->huff[1].valid = fr->huff[2].valid = fr->huff[3].valid = 0;
    for (;;) {
        int m, len;
        if (p + 2 > end) return THPJ_ERR_TRUNC;
        if (*p++ != 0xFF) return THPJ_ERR_SYNTAX;
        while (p < end && *p == 0xFF) p++;
        if (p >= end) return THPJ_ERR_TRUNC;
        m = *p++;
        if (m == 0xD8) continue;     /* SOI */
        if (m == 0xD9) return THPJ_ERR_SYNTAX;
        if (p + 2 > end) return THPJ_ERR_TRUNC;
        len = be16(p);
        if (p + len > end) return THPJ_ERR_TRUNC;
        if (m == 0xDB) { /* DQT */
            const u8* q = p + 2;
            while (q < p + len) {
                int pq = *q >> 4, tq = *q & 15, i;
                q++;
                if (pq != 0 || tq > 3) return THPJ_ERR_UNSUPPORTED;
                for (i = 0; i < 64; i++) fr->q[tq][i] = q[i];
                q += 64;
                fr->qvalid |= 1 << tq;
            }
        } else if (m == 0xC4) { /* DHT */
            const u8* q = p + 2;
            while (q < p + len) {
                int tc = *q >> 4, th = *q & 15, i, n = 0;
                Huff* h;
                if (tc > 1 || th > 1) return THPJ_ERR_UNSUPPORTED;
                h = &fr->huff[(th << 1) + tc];
                for (i = 0; i < 16; i++) n += q[1 + i];
                if (n > 256) return THPJ_ERR_SYNTAX;
                if (!huff_build(h, q + 1)) return THPJ_ERR_SYNTAX;
                memcpy(h->values, q + 17, n);
                if (tc) huff_build_fast_ac(h);
                h->valid = 1;
                q += 17 + n;
            }
        } else if (m == 0xC0) { /* SOF0 */
            const u8* q = p + 2;
            int i;
            if (q[0] != 8 || q[5] != 3) return THPJ_ERR_UNSUPPORTED;
            fr->h = be16(q + 1);
            fr->w = be16(q + 3);
            for (i = 0; i < 3; i++) {
                int hv = q[7 + i * 3];
                if (hv != (i == 0 ? 0x22 : 0x11)) return THPJ_ERR_UNSUPPORTED;
                fr->comp_q[i] = q[8 + i * 3] & 3;
            }
            sof = 1;
        } else if (m == 0xDD) { /* DRI */
            fr->ri = be16(p + 2);
        } else if (m == 0xDA) { /* SOS: the tables are complete */
            const u8* q = p + 2;
            int i;
            if (!sof || q[0] != 3) return THPJ_ERR_UNSUPPORTED;
            for (i = 0; i < 3; i++) {
                fr->comp_dc[i] = (q[2 + i * 2] >> 4) & 1;
                fr->comp_ac[i] = q[2 + i * 2] & 1;
            }
            p += len;
            break;
        } else if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE) {
            /* APPn / COM */
        } else {
            return THPJ_ERR_UNSUPPORTED;
        }
        p += len;
    }
    if (fr->w != want_w || fr->h != want_h || (fr->w & 15) || (fr->h & 15) || fr->w > THPJ_MAX_W)
        return THPJ_ERR_SIZE;
    {
        int i;
        for (i = 0; i < 3; i++) {
            if (!fr->huff[fr->comp_dc[i] << 1].valid || !fr->huff[(fr->comp_ac[i] << 1) + 1].valid)
                return THPJ_ERR_SYNTAX;
            if (!(fr->qvalid & (1 << fr->comp_q[i]))) return THPJ_ERR_SYNTAX;
        }
    }
    ctx->b.p = p;
    ctx->b.end = end;
    ctx->b.buf = 0;
    ctx->b.n = 0;
    ctx->src = src;
    ctx->ty = tileY;
    ctx->tu = tileU;
    ctx->tv = tileV;
    ctx->pred[0] = ctx->pred[1] = ctx->pred[2] = 0;
    ctx->togo = fr->ri;
    ctx->my = 0;
    ctx->tiled = tiled;
    ctx->stats = stats;
    return 0;
}

int thpj_decode_rows(ThpjCtx* ctx, int rows) {
    Frame* fr = &ctx->fr;
    {
        Bits b = ctx->b;
        short blk[64];
        int pred[3];
        int mx, my, mcux = fr->w / 16, mcuy = fr->h / 16, myend;
        int ypitch = fr->w * 4, cpitch = fr->w * 2; /* one tile row */
        int togo = ctx->togo, tiled = ctx->tiled;
        u8 *tileY = ctx->ty, *tileU = ctx->tu, *tileV = ctx->tv;
        const Huff *ydc = &fr->huff[fr->comp_dc[0] << 1], *yac = &fr->huff[(fr->comp_ac[0] << 1) + 1];
        const Huff *udc = &fr->huff[fr->comp_dc[1] << 1], *uac = &fr->huff[(fr->comp_ac[1] << 1) + 1];
        const Huff *vdc = &fr->huff[fr->comp_dc[2] << 1], *vac = &fr->huff[(fr->comp_ac[2] << 1) + 1];
        const u16 *yq = fr->q[fr->comp_q[0]], *uq = fr->q[fr->comp_q[1]], *vq = fr->q[fr->comp_q[2]];
        pred[0] = ctx->pred[0];
        pred[1] = ctx->pred[1];
        pred[2] = ctx->pred[2];
        myend = rows <= 0 || ctx->my + rows > mcuy ? mcuy : ctx->my + rows;
        for (my = ctx->my; my < myend; my++) {
            for (mx = 0; mx < mcux; mx++) {
                u8 *y0, *y1, *y2, *y3, *u0, *u1, *v0, *v1;
                int ys, cs;
                if (tiled) {
                    /* Y block row 2my = tile rows 4my, 4my+1; 2my+1 = 4my+2, +3 */
                    y0 = tileY + (my * 4) * ypitch + mx * 64;
                    y1 = y0 + ypitch;
                    y2 = y0 + 2 * ypitch;
                    y3 = y0 + 3 * ypitch;
                    u0 = tileU + (my * 2) * cpitch + mx * 32;
                    v0 = tileV + (my * 2) * cpitch + mx * 32;
                    u1 = u0 + cpitch;
                    v1 = v0 + cpitch;
                    ys = cs = 8;
                } else {
                    y0 = tileY + (my * 16) * fr->w + mx * 16;
                    y1 = y0 + 4 * fr->w;
                    y2 = y0 + 8 * fr->w;
                    y3 = y0 + 12 * fr->w;
                    u0 = tileU + (my * 8) * (fr->w / 2) + mx * 8;
                    v0 = tileV + (my * 8) * (fr->w / 2) + mx * 8;
                    u1 = u0 + 2 * fr->w;
                    v1 = v0 + 2 * fr->w;
                    ys = fr->w;
                    cs = fr->w / 2;
                }
                if (!decode_block(&b, blk, ydc, yac, &pred[0], yq)) return THPJ_ERR_DATA;
                idct_block(y0, y1, ys, blk);
                if (!decode_block(&b, blk, ydc, yac, &pred[0], yq)) return THPJ_ERR_DATA;
                idct_block(y0 + (tiled ? 32 : 8), y1 + (tiled ? 32 : 8), ys, blk);
                if (!decode_block(&b, blk, ydc, yac, &pred[0], yq)) return THPJ_ERR_DATA;
                idct_block(y2, y3, ys, blk);
                if (!decode_block(&b, blk, ydc, yac, &pred[0], yq)) return THPJ_ERR_DATA;
                idct_block(y2 + (tiled ? 32 : 8), y3 + (tiled ? 32 : 8), ys, blk);
                if (!decode_block(&b, blk, udc, uac, &pred[1], uq)) return THPJ_ERR_DATA;
                idct_block(u0, u1, cs, blk);
                if (!decode_block(&b, blk, vdc, vac, &pred[2], vq)) return THPJ_ERR_DATA;
                idct_block(v0, v1, cs, blk);
                if (fr->ri && --togo == 0) {
                    /* THPDec.c: to the next byte boundary, predictors reset */
                    int drop = b.n & 7;
                    b.buf <<= drop;
                    b.n -= drop;
                    pred[0] = pred[1] = pred[2] = 0;
                    togo = fr->ri;
                }
            }
        }
        ctx->b = b;
        ctx->pred[0] = pred[0];
        ctx->pred[1] = pred[1];
        ctx->pred[2] = pred[2];
        ctx->togo = togo;
        ctx->my = myend;
        if (myend < mcuy) {
            return THPJ_MORE;
        }
        if (ctx->stats) {
            ctx->stats->bytes_used = (unsigned)(b.p - ctx->src) - (unsigned)(b.n / 8);
        }
    }
    return 0;
}

int thpj_decode(ThpjCtx* ctx, const unsigned char* src, unsigned size, unsigned char* tileY,
                unsigned char* tileU, unsigned char* tileV, int want_w, int want_h, int tiled,
                ThpjStats* stats) {
    int e = thpj_decode_begin(ctx, src, size, tileY, tileU, tileV, want_w, want_h, tiled, stats);
    return e ? e : thpj_decode_rows(ctx, 0);
}

/* The picture the game's TEV makes of the three planes, on the CPU.
 *
 * THPDraw.c's THPGXYuv2RgbSetup is a colour matrix in five TEV stages with
 * its coefficients in konst colours and one signed register (PLAN.md 53.4
 * works it through).  In 0..255 units, with U and V the raw texel values:
 *
 *     R = Y + 2 (V*0xB3/255 - 90)            = Y + 1.4039 V - 180
 *     G = Y + 135 - U*0x58/255 - V*0xB6/255  = Y - 0.3451 U - 0.7137 V + 135
 *     B = Y + 2 (U*0xE2/255 - 114)           = Y + 1.7725 U - 228
 *
 * which is BT.601 full range to within the konst colours' 8 bits -- and it
 * is the game's numbers, not the textbook's, that are used here.  The
 * chroma planes are half size and sampled GX_LINEAR at the pixel centres,
 * so the console upsamples chroma with a triangle filter (3/4, 1/4 in each
 * axis, clamped at the edges); so does this.
 *
 * The planes are linear here (thpj_decode with tiled = 0). */
#define CR_R 92006  /* 2*0xB3/255 in 16.16 */
#define CG_U 22616  /* 0x58/255 */
#define CG_V 46774  /* 0xB6/255 */
#define CB_B 116164 /* 2*0xE2/255 */

/* A clamp by sum (index value + 384), 1 KB: stays in the L1 cache.  The
 * chroma terms are multiplies -- the M38 first cut looked them up in four
 * 16 KB tables, twice the G4's 32 KB L1 between them, and the conversion cost
 * 12.2 ms a frame against the decode's 7.8 (PLAN.md 53.4). */
static u8 tab_clamp[1024];
static volatile int tab_ready;

static void tables_build(void) {
    int i;
    for (i = 0; i < 1024; i++) tab_clamp[i] = clamp8(i - 384);
    tab_ready = 1;
}

void thpj_init(void) {
    if (!tab_ready) tables_build();
}

/* chroma row cy blended with its neighbour toward output row y, 4x units */
static void chroma_vrow(const u8* c, int cw, int ch, int y, int* v) {
    /* output row y samples chroma row (y - 0.5)/2: the nearer row at 3/4 */
    int cy = y >> 1;
    int cn = (y & 1) ? cy + 1 : cy - 1;
    const u8 *a, *b;
    int x;
    if (cn < 0) cn = 0;
    if (cn >= ch) cn = ch - 1;
    a = c + cy * cw;
    b = c + cn * cw;
    for (x = 0; x < cw; x++) v[x] = 3 * a[x] + b[x];
}

/* the three chroma terms of one pixel from 16x-unit u and v, in 1/2^20 */
/* floored, as the TEV's stages floor (PLAN.md 53.9) */
#define TERMS(u, v)                                   \
    r_ = (CR_R * (v) + (-180 << 20)) >> 20;             \
    g_ = ((135 << 20) - CG_U * (u) - CG_V * (v)) >> 20; \
    b_ = (CB_B * (u) + (-228 << 20)) >> 20;

void thpj_to_rgba(ThpjCtx* ctx, const unsigned char* planeY, const unsigned char* planeU,
                  const unsigned char* planeV, int w, int h, unsigned char* rgba, int pitch,
                  int argb) {
    thpj_to_rgba_rows(ctx, planeY, planeU, planeV, w, h, rgba, pitch, argb, 0, h);
}

void thpj_to_rgba_rows(ThpjCtx* ctx, const unsigned char* planeY, const unsigned char* planeU,
                       const unsigned char* planeV, int w, int h, unsigned char* rgba, int pitch,
                       int argb, int y0, int y1) {
    int y, cw = w / 2, ch = h / 2;
    int *vu = ctx->urow, *vv = ctx->vrow;
    const u8* cl = tab_clamp + 384;
    /* one 32-bit store a pixel: the shifts put R, G, B, A at the byte
     * offsets the caller asked for (RGBA, or ARGB), on either endianness */
#ifdef __BIG_ENDIAN__
    const int sR = argb ? 16 : 24, sG = argb ? 8 : 16, sB = argb ? 0 : 8;
    const u32 A = argb ? 0xFF000000u : 0xFFu;
#else
    const int sR = argb ? 8 : 0, sG = argb ? 16 : 8, sB = argb ? 24 : 16;
    const u32 A = argb ? 0xFFu : 0xFF000000u;
#endif
    if (w > THPJ_MAX_W || (w & 1)) return;
    thpj_init();
    if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++) {
        const u8* yp = planeY + y * w;
        u32* o = (u32*)(void*)(rgba + y * pitch);
        int cx;
        chroma_vrow(planeU, cw, ch, y, vu);
        chroma_vrow(planeV, cw, ch, y, vv);
        for (cx = 0; cx < cw; cx++) {

            /* the pair of pixels over chroma column cx: the left takes its
             * left neighbour at 1/4, the right its right one (clamped) */
            int ul = cx > 0 ? vu[cx - 1] : vu[cx], ur = cx < cw - 1 ? vu[cx + 1] : vu[cx];
            int vl = cx > 0 ? vv[cx - 1] : vv[cx], vr = cx < cw - 1 ? vv[cx + 1] : vv[cx];
            int u0 = 3 * vu[cx] + ul, u1 = 3 * vu[cx] + ur; /* 16x */
            int v0 = 3 * vv[cx] + vl, v1 = 3 * vv[cx] + vr;
            int r_, g_, b_, Y;
            TERMS(u0, v0)
            Y = yp[0];
            o[0] = A | ((u32)cl[Y + r_] << sR) | ((u32)cl[Y + g_] << sG) | ((u32)cl[Y + b_] << sB);
            TERMS(u1, v1)
            Y = yp[1];
            o[1] = A | ((u32)cl[Y + r_] << sR) | ((u32)cl[Y + g_] << sG) | ((u32)cl[Y + b_] << sB);
            yp += 2;
            o += 2;
        }
    }
}
