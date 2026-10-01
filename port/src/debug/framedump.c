/* M51 (PLAN.md 66): --framedump FILE -- every presented frame of a run,
 * deflated, one after another in FILE: the G4 video (tools/session_video.sh
 * reads it on littlejelly while it grows, tools/framedump_read.py turns it
 * into raw frames for ffmpeg).  The frame is read back where --dumpframe
 * reads it (gl13_present: the game's picture, before the overlay menu), so
 * a replay in lockstep -- every frame drawn -- gives the recording's every
 * frame.  Written by the game thread; a run that wants its speed does not
 * ask for it.
 *
 *   "MP4FD2\n", then per frame: "FRM2", u32 frame, u32 retrace, u16 w, u16 h,
 *   u32 n, n bytes of zlib (RGB, top row first); at the end "END!", u32
 *   count -- all big endian.  The retrace is the game's frame (the video's
 *   clock: 59.94 a second, the --wavdump's 32,000 samples a second). */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static FILE* fd;
static unsigned long fd_count;
static unsigned char* fd_rgb;
static unsigned char* fd_z;
static uLongf fd_zcap;

static void be32(unsigned char* p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

void port_framedump_close(void) {
    unsigned char t[8];
    if (!fd) {
        return;
    }
    memcpy(t, "END!", 4);
    be32(t + 4, fd_count);
    fwrite(t, 1, 8, fd);
    fclose(fd);
    fd = NULL;
    port_log("port> framedump (M51): %lu frames in %s\n", fd_count, port_opt.framedump);
}

/* `rgb` is w*h*3 bottom-up (glReadPixels); the file's frames are top-down */
void port_framedump_frame(unsigned frame, int w, int h, const unsigned char* rgb_bottom_up) {
    unsigned char hdr[20];
    size_t row = (size_t)w * 3;
    int y;
    uLongf zn;
    extern unsigned int VIGetRetraceCount(void);
    if (!port_opt.framedump || VIGetRetraceCount() < (unsigned)port_opt.framedump_from) {
        return; /* --framedumpfrom counts retraces (the recording's frames) */
    }
    if (!fd) {
        fd = fopen(port_opt.framedump, "wb");
        if (!fd) {
            port_log("port> framedump (M51): cannot create %s\n", port_opt.framedump);
            port_opt.framedump = NULL;
            return;
        }
        setvbuf(fd, NULL, _IOFBF, 1 << 18);
        fwrite("MP4FD2\n", 1, 7, fd);
        atexit(port_framedump_close);
        fd_rgb = (unsigned char*)malloc(row * (size_t)h);
        fd_zcap = compressBound((uLong)(row * (size_t)h));
        fd_z = (unsigned char*)malloc(fd_zcap);
        port_log("port> framedump (M51): every presented frame from %u to %s\n", frame, port_opt.framedump);
    }
    for (y = 0; y < h; y++) {
        memcpy(fd_rgb + (size_t)y * row, rgb_bottom_up + (size_t)(h - 1 - y) * row, row);
    }
    zn = fd_zcap;
    if (compress2(fd_z, &zn, fd_rgb, (uLong)(row * (size_t)h), 1) != Z_OK) {
        return;
    }
    memcpy(hdr, "FRM2", 4);
    be32(hdr + 4, frame);
    be32(hdr + 8, VIGetRetraceCount());
    hdr[12] = (unsigned char)(w >> 8);
    hdr[13] = (unsigned char)w;
    hdr[14] = (unsigned char)(h >> 8);
    hdr[15] = (unsigned char)h;
    be32(hdr + 16, zn);
    fwrite(hdr, 1, 20, fd);
    fwrite(fd_z, 1, zn, fd);
    fd_count++;
    if ((fd_count % 60u) == 0u) {
        fflush(fd);
    }
}
