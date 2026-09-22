/* A baseline JPEG decoder in THP's shape (thp_jpeg.c, PLAN.md 53). */
#ifndef PORT_THP_JPEG_H
#define PORT_THP_JPEG_H

enum {
    THPJ_OK = 0,
    THPJ_ERR_TRUNC = 1,       /* ran off the end of the frame's bytes */
    THPJ_ERR_SYNTAX = 3,      /* not a marker where one must be, bad table */
    THPJ_ERR_UNSUPPORTED = 11 /* progressive, 12-bit, not 4:2:0, ... */,
    THPJ_ERR_SIZE = 12,       /* SOF disagrees with the THP header */
    THPJ_ERR_DATA = 13,       /* a Huffman code that is in no table */
    THPJ_MORE = -1            /* thpj_decode_rows: rows are left */
};

#define THPJ_MAX_W 1024

/* One decoder's tables and scratch (~25 KB): one per concurrent decode --
 * the worker and the game thread's inline twin can run two at once. */
typedef struct ThpjCtx ThpjCtx;
unsigned thpj_ctx_size(void);
void thpj_init(void); /* the colour tables; idempotent, call once on the game thread */

typedef struct {
    unsigned bytes_used; /* entropy-coded bytes read, for the tests */
} ThpjStats;

/* Decode one THP video component into three planes: Y w x h, U and V
 * (w/2) x (h/2); GX I8 tiled when `tiled` (what THPVideoDecode hands the
 * game), row-major otherwise (what thpj_to_rgba reads).  0 on success, else a THPJ_ERR_*; the planes are
 * partly written on an error. */
int thpj_decode(ThpjCtx* ctx, const unsigned char* src, unsigned size, unsigned char* tileY,
                unsigned char* tileU, unsigned char* tileV, int w, int h, int tiled,
                ThpjStats* stats);

/* The same decode in pieces (M39, PLAN.md 54.2): _begin reads the headers
 * (0 or a THPJ_ERR_*), then each _rows call decodes up to `rows` MCU rows
 * (16 picture rows; 0 = all that are left) and returns THPJ_MORE until the
 * last, then 0 or an error.  The output is byte-identical to thpj_decode's,
 * which is exactly _begin + _rows(0). */
int thpj_decode_begin(ThpjCtx* ctx, const unsigned char* src, unsigned size,
                      unsigned char* tileY, unsigned char* tileU, unsigned char* tileV,
                      int w, int h, int tiled, ThpjStats* stats);
int thpj_decode_rows(ThpjCtx* ctx, int rows);

/* Three row-major planes to RGBA8 (`pitch` bytes a row, alpha 255) -- or,
 * with `argb`, bytes A R G B -- by the colour matrix THPDraw.c's TEV applies. */
void thpj_to_rgba(ThpjCtx* ctx, const unsigned char* tileY, const unsigned char* tileU,
                  const unsigned char* tileV, int w, int h, unsigned char* rgba, int pitch,
                  int argb);
/* the same, output rows y0 <= y < y1 only (the conversion in bands) */
void thpj_to_rgba_rows(ThpjCtx* ctx, const unsigned char* tileY, const unsigned char* tileU,
                       const unsigned char* tileV, int w, int h, unsigned char* rgba, int pitch,
                       int argb, int y0, int y1);

#endif
