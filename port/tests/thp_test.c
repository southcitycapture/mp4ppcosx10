/* thp_test: decode every video frame of a THP file with port/src/thp's
 * decoder, time it, and write chosen frames as PPM (PLAN.md 53).
 *   thp_test FILE.thp [frame ...]   -> thp-NNNNN.ppm in the cwd
 * Host or G4; reads the big-endian THP header byte by byte. */
#include "../src/thp/thp_jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static unsigned be32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char** argv) {
    FILE* f;
    long n;
    unsigned char *d, *Y, *U, *V, *rgba;
    unsigned nf, first, ffs, ci, nc, off, sz, i, w, h, vo;
    double t0, tdec = 0, tcvt = 0, worst = 0;
    int bad = 0;
    if (argc < 2) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    nf = be32(d + 0x14);
    ffs = be32(d + 0x18);
    ci = be32(d + 0x20);
    first = be32(d + 0x28);
    nc = be32(d + ci);
    vo = ci + 20;
    w = be32(d + vo);
    h = be32(d + vo + 4);
    Y = malloc(w * h);
    U = malloc(w * h / 4);
    V = malloc(w * h / 4);
    rgba = malloc(w * h * 4);
    ThpjCtx* ctx = malloc(thpj_ctx_size());
    /* M39: the same frame decoded a row at a time and converted in bands of
     * 32 rows must be byte-identical to the whole-frame path */
    ThpjCtx* ctx2 = malloc(thpj_ctx_size());
    unsigned char *Y2 = malloc(w * h * 3 / 2), *rgba2 = malloc(w * h * 4);
    unsigned sliced_diff = 0;
    thpj_init();
    off = first;
    sz = ffs;
    for (i = 0; i < nf; i++) {
        const unsigned char* fr = d + off;
        unsigned vsz = be32(fr + 8);
        const unsigned char* vid = fr + 8 + 4 * nc;
        int r, k, want = 0;
        double t;
        t0 = now();
        r = thpj_decode(ctx, vid, vsz, Y, U, V, (int)w, (int)h, 0, NULL);
        t = now() - t0;
        tdec += t;
        if (t > worst) worst = t;
        if (r) {
            if (bad++ < 5) fprintf(stderr, "frame %u: error %d\n", i, r);
        }
        for (k = 2; k < argc; k++)
            if ((unsigned)atoi(argv[k]) == i) want = 1;
        t0 = now();
        thpj_to_rgba(ctx, Y, U, V, (int)w, (int)h, rgba, (int)w * 4, 0);
        tcvt += now() - t0;
        {
            int r2 = thpj_decode_begin(ctx2, vid, vsz, Y2, Y2 + w * h, Y2 + w * h * 5 / 4,
                                       (int)w, (int)h, 0, NULL), yb;
            if (r2 == 0) {
                do r2 = thpj_decode_rows(ctx2, 1);
                while (r2 == THPJ_MORE);
            }
            for (yb = 0; yb < (int)h; yb += 32)
                thpj_to_rgba_rows(ctx2, Y2, Y2 + w * h, Y2 + w * h * 5 / 4, (int)w, (int)h,
                                  rgba2, (int)w * 4, 0, yb, yb + 32);
            if (r2 != r || memcmp(Y2, Y, w * h) || memcmp(Y2 + w * h, U, w * h / 4) ||
                memcmp(Y2 + w * h * 5 / 4, V, w * h / 4) || memcmp(rgba2, rgba, w * h * 4)) {
                if (sliced_diff++ < 5) fprintf(stderr, "frame %u: the sliced decode differs\n", i);
            }
        }
        if (want) {
            char name[64];
            FILE* o;
            unsigned p;
            snprintf(name, sizeof name, "thp-%05u.ppm", i);
            o = fopen(name, "wb");
            fprintf(o, "P6\n%u %u\n255\n", w, h);
            for (p = 0; p < w * h; p++) fwrite(rgba + p * 4, 1, 3, o);
            fclose(o);
        }
        off += sz;
        sz = be32(fr);
    }
    printf("%s: %u frames %ux%u, %d errors, decode %.2f ms/frame mean (worst %.2f), "
           "to-RGBA %.2f ms/frame\n",
           argv[1], nf, w, h, bad, tdec * 1000 / nf, worst * 1000, tcvt * 1000 / nf);
    printf("sliced decode (one MCU row a call, 32-row conversion bands): %u of %u frames "
           "differ from the whole-frame path\n", sliced_diff, nf);
    return bad != 0 || sliced_diff != 0;
}
