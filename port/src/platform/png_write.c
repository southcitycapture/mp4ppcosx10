/* M32: a PNG writer with no zlib -- the F12 screenshot lands on the player's
 * Desktop as a file every Mac can open, where --dumpframe's PPMs are for the
 * lab.  The image data goes into stored (uncompressed) deflate blocks, so the
 * only arithmetic is PNG's CRC-32 and zlib's Adler-32; a 640x480 frame is
 * 922 KB, a compression ratio nobody at the Desktop will notice and one
 * library fewer to carry across two toolchains.  Rows are handed in GL's
 * order (bottom row first), as glReadPixels returns them. */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long crc_table[256];

static void crc_init(void) {
    unsigned long c;
    int n, k;
    if (crc_table[1]) {
        return;
    }
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static unsigned long crc_update(unsigned long crc, const unsigned char* p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

static void put32(FILE* f, unsigned long v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
    fwrite(b, 1, 4, f);
}

static void chunk(FILE* f, const char* type, const unsigned char* data, size_t n) {
    unsigned long crc;
    put32(f, (unsigned long)n);
    fwrite(type, 1, 4, f);
    if (n) {
        fwrite(data, 1, n, f);
    }
    crc = crc_update(0xffffffffUL, (const unsigned char*)type, 4);
    crc = crc_update(crc, data, n) ^ 0xffffffffUL;
    put32(f, crc);
}

int port_write_png(const char* path, int w, int h, const unsigned char* rgb_bottom_up) {
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    unsigned char* z;
    size_t row = (size_t)w * 3 + 1; /* the filter byte, then the pixels */
    size_t raw = row * (size_t)h;
    size_t blocks = (raw + 65534) / 65535;
    size_t zn = 2 + raw + blocks * 5 + 4;
    size_t o = 0, done = 0;
    unsigned long a = 1, b = 0;
    int y;
    FILE* f;

    crc_init();
    z = (unsigned char*)malloc(zn);
    if (!z) {
        return 0;
    }
    z[o++] = 0x78; /* zlib header: deflate, 32 KB window, no dictionary */
    z[o++] = 0x01;
    for (y = h - 1; y >= 0; y--) {
        const unsigned char* src = rgb_bottom_up + (size_t)y * w * 3;
        size_t i;
        unsigned char filt = 0;
        /* each row is one filter byte + w*3 bytes of the pixels, split into
         * stored blocks of up to 65535 bytes as it goes */
        for (i = 0; i < row; i++) {
            unsigned char c = i == 0 ? filt : src[i - 1];
            if (done % 65535 == 0) {
                size_t left = raw - done;
                size_t len = left > 65535 ? 65535 : left;
                z[o++] = (unsigned char)(len == left ? 1 : 0); /* BFINAL on the last block */
                z[o++] = (unsigned char)(len & 0xff);
                z[o++] = (unsigned char)(len >> 8);
                z[o++] = (unsigned char)(~len & 0xff);
                z[o++] = (unsigned char)((~len >> 8) & 0xff);
            }
            z[o++] = c;
            done++;
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
    }
    z[o++] = (unsigned char)(b >> 8);
    z[o++] = (unsigned char)b;
    z[o++] = (unsigned char)(a >> 8);
    z[o++] = (unsigned char)a;

    f = fopen(path, "wb");
    if (!f) {
        free(z);
        return 0;
    }
    fwrite(sig, 1, 8, f);
    ihdr[0] = (unsigned char)(w >> 24);
    ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);
    ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24);
    ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);
    ihdr[7] = (unsigned char)h;
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* truecolour */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* filter method 0 */
    ihdr[12] = 0; /* no interlace */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}
