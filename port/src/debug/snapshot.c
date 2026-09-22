/* Snapshots: a ring of restore points, so that a crash twelve hours into a
 * soak can be reproduced in minutes with a debugger already attached.
 *
 * The port is in an unusually good position to do this.  The game is
 * single-threaded (PLAN.md §1.7: HUPROCESS coroutines over gcsetjmp/gclongjmp,
 * never a real thread), it does not call the host for anything the port cannot
 * re-derive, and all of its own memory is in two arenas the port mapped
 * itself.  So a snapshot is:
 *
 *   1. **MEM1 and ARAM**, which hold the heaps, every HUPROCESS stack, every
 *      loaded model, the sound data and the framebuffers -- 40 MB, copied
 *      wholesale because picking through it would be guesswork;
 *   2. **the used part of the host game stack**, which is where the *main*
 *      process's frames live (the coroutines' own stacks are inside MEM1);
 *   3. **the game's writable globals in the main binary**.  On the console
 *      these were the DOL's .data/.bss inside MEM1; here they are ordinary
 *      globals linked next to the port's own, and telling the two apart is
 *      what `<exe>.snapmap` (port/tools/gen_snapmap.py, from the link map) is
 *      for.  The port's own globals are emphatically NOT restored: they hold
 *      this process's window, GL names and file handles;
 *   4. **each loaded REL bundle's `__DATA` segment**, which is that module's
 *      .data and .bss -- the module's entire state;
 *   5. **the port's own determinism-bearing state**, which is not memory the
 *      game owns but is part of the run: the retrace and frame counters, the
 *      deterministic clock, the GX register state the game set frames ago, the
 *      audio SAL's DMA cursor, the memory card image, the self-play harness.
 *      That is an explicit, named registry (`port_snap_register`) rather than
 *      a blanket copy, exactly because a blanket copy would also bring the
 *      dangling host pointers;
 *   6. **the coroutine context** -- one `gcsetjmp` taken at the top of the
 *      retrace.  Restoring is a `gclongjmp` into it once everything else is
 *      back, so the restored run resumes in the middle of `VIWaitForRetrace`
 *      exactly where the snapshot was taken.
 *
 * What is deliberately left out, because it is re-derived:
 *
 *   - GL textures and the texture cache: flushed on restore, and the cache
 *     rebuilds lazily from the game's own memory, which has just been
 *     restored;
 *   - the GL state shadow: invalidated, so the next draw re-emits everything;
 *   - the disc: `dvd_fs.c` seeks per read and keeps no file position of its
 *     own; the game's `DVDFileInfo`s are in MEM1 and come back with it;
 *   - the pad replay: `pad_play_step` is a pure function of the frame number.
 *
 * The two things a restore cannot paper over, and therefore checks and refuses
 * on: a different build (the binary's size and its snapmap are hashed into a
 * build id), and the arenas or the REL bundles landing at different addresses
 * than the run that took the snapshot.  Both produce a clear message rather
 * than a mysterious corruption.
 */
#include "port.h"

#include <dolphin/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* From the GX backend and the loader; declared here so the debug tree does not
 * need their private headers. */
void gx_tex_flush_all(void);
void* port_game_stack_top(void);
unsigned gl13_frame_number(void);
void gl13_set_frame_number(unsigned n);
int port_dll_snap_count(void);
int port_dll_snap_get(int i, const char** name, void** handle, void** image,
                      void** data_lo, unsigned long* data_size, int* open,
                      int* stuck);
int port_dll_snap_reopen(const char* name, void** handle, void** image,
                         void** data_lo, unsigned long* data_size, int open,
                         int stuck);

/* The port modules that own a piece of the run.  Each one registers its own
 * statics; a file's statics are only visible inside it. */
void port_vi_snap_register(void);
void gl13_snap_register(void);
void gx_state_snap_register(void);
void os_misc_snap_register(void);
void musyx_sal_snap_register(void);
void port_card_snap_register(void);
void port_selfplay_snap_register(void);
void port_aram_snap_register(void);
void port_sreset_snap_register(void);
void port_pad_snap_register(void);
void port_musyx_mix_snap_register(void);
void port_aram_musyx_snap_register(void);

extern int gcsetjmp(void* jump);
extern int gclongjmp(void* jump, int status);

#define SNAP_MAGIC "MP4SNAP1"
#define SNAP_VERSION 1u
#define SNAP_CTX_BYTES 256u
#define SNAP_NAME_MAX 48
#define SNAP_MOD_NAME 64
#define SNAP_REG_MAX 128
#define SNAP_STACK_MARGIN (64u * 1024u)

typedef struct {
    char magic[8];
    u32 version;
    u32 build_id;
    u32 frame;
    u32 mem1_addr, mem1_size;
    u32 aram_addr, aram_size;
    u32 stack_lo, stack_size;
    u32 nranges, ranges_bytes;
    u32 nmods;
    u32 nregs;
    u32 flags;
    u8 ctx[SNAP_CTX_BYTES];
} SnapHeader;

typedef struct {
    u32 addr;
    u32 size;
} SnapRange;

typedef struct {
    char name[SNAP_MOD_NAME];
    u32 handle;
    u32 image;
    u32 data_lo;
    u32 data_size;
    u32 open;  /* the game has it linked right now */
    u32 stuck; /* ...and whether its bss needs zeroing by hand on re-entry */
} SnapMod;

typedef struct {
    char name[SNAP_NAME_MAX];
    u32 size;
} SnapRegHdr;

typedef struct {
    const char* name;
    void* p;
    unsigned long size;
} RegEntry;

static RegEntry reg[SNAP_REG_MAX];
static int reg_count;

static SnapRange* ranges;
static int nranges;
static unsigned long ranges_bytes;
static u32 build_id;
static int snap_ready;

static char snap_dir[1024];
static unsigned long stat_written, stat_bytes;
static double stat_last_cost;

/* The deepest the host game stack has been used.  The main process runs on it;
 * every HUPROCESS runs on a stack inside MEM1, so at any given retrace the
 * live host frames may be far above the current stack pointer. */
static u8* stack_low_water;

static u8 snap_ctx[SNAP_CTX_BYTES];

void port_snap_register(const char* name, void* p, unsigned long size) {
    if (reg_count == SNAP_REG_MAX || !p || !size) {
        port_log("port> snapshot: registry full or empty entry (%s)\n",
                 name ? name : "?");
        return;
    }
    reg[reg_count].name = name;
    reg[reg_count].p = p;
    reg[reg_count].size = size;
    reg_count++;
}

/* ---- the build id --------------------------------------------------------
 * A snapshot is only meaningful to the binary that took it: the game's globals
 * are restored by *address*, and a rebuild moves them.  The id is a hash of
 * the snapmap (which changes whenever any global moves) together with the
 * executable's size and mtime. */
static u32 fnv(const void* pv, size_t n, u32 h) {
    const u8* p = (const u8*)pv;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static void exe_path(char* out, size_t n) {
    uint32_t sz = (uint32_t)n;
#if defined(__APPLE__)
    if (_NSGetExecutablePath(out, &sz) != 0) {
        snprintf(out, n, "marioparty4");
    }
#else
    snprintf(out, n, "marioparty4");
#endif
}

/* ---- the snapmap ---------------------------------------------------------
 * `<exe>.snapmap`, written by the build from the link map: the address and
 * size of every writable global that came from a game or MusyX object file.
 * Without it a snapshot cannot be taken at all, which is the right failure:
 * silently skipping the game's globals would produce restores that diverge. */
static void load_snapmap(void) {
    char path[1024];
    char line[256];
    FILE* f;
    struct stat st;
    u32 h = 2166136261u;
    int i = 0;

    exe_path(path, sizeof(path) - 16);
    strncat(path, ".snapmap", sizeof(path) - strlen(path) - 1);
    f = fopen(path, "r");
    if (!f) {
        port_log("port> snapshot: no %s -- snapshots are unavailable (the "
                 "build writes it; see port/tools/gen_snapmap.py)\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        h = fnv(line, strlen(line), h);
        if (!strncmp(line, "ranges ", 7)) {
            nranges = atoi(line + 7);
            if (nranges > 0) {
                ranges = (SnapRange*)calloc((size_t)nranges, sizeof(*ranges));
            }
        } else if (line[0] == 'r' && line[1] == ' ' && ranges && i < nranges) {
            unsigned a = 0, s = 0;
            sscanf(line + 2, "%x %x", &a, &s);
            ranges[i].addr = a;
            ranges[i].size = s;
            ranges_bytes += s;
            i++;
        }
    }
    fclose(f);
    nranges = i;
    exe_path(path, sizeof(path));
    if (stat(path, &st) == 0) {
        h = fnv(&st.st_size, sizeof(st.st_size), h);
        h = fnv(&st.st_mtime, sizeof(st.st_mtime), h);
    }
    build_id = h;
    snap_ready = (nranges > 0);
    port_log("port> snapshot: %d game data ranges, %lu KB, build id %08x\n",
             nranges, ranges_bytes / 1024, build_id);
}

/* The snapmap's addresses are link-time; on this target the main executable is
 * not position-independent and the slide is zero, but asking costs nothing. */
static long image_slide(void) {
#if defined(__APPLE__)
    return (long)_dyld_get_image_vmaddr_slide(0);
#else
    return 0;
#endif
}

/* ---- where the ring lives ------------------------------------------------ */
static void snap_dir_init(void) {
    const char* home = getenv("HOME");
    if (port_opt.snap_dir) {
        snprintf(snap_dir, sizeof(snap_dir), "%s", port_opt.snap_dir);
    } else {
        snprintf(snap_dir, sizeof(snap_dir), "%s/MarioParty4/snaps",
                 home ? home : ".");
    }
    if (mkdir(snap_dir, 0755) != 0) {
        /* EEXIST is the normal case; anything else is reported when a write
         * fails, with the errno that actually matters. */
    }
}

/* ---- the ring ------------------------------------------------------------ */
/* Snapshots are named by frame, so the ring needs no index file: the port
 * keeps the list of the ones *this run* wrote and deletes its own oldest.  A
 * snapshot from an earlier run is never swept out from under a debugging
 * session that is about to restore it. */

/* The written list, used by ring_trim and by the fault handler's report. */
static char written[16][1024];
static int nwritten;

static void snap_note_written(const char* path) {
    int i;
    if (nwritten == 16) {
        for (i = 1; i < 16; i++) {
            memcpy(written[i - 1], written[i], sizeof(written[0]));
        }
        nwritten--;
    }
    snprintf(written[nwritten], sizeof(written[0]), "%s", path);
    nwritten++;
    while (nwritten > port_opt.snap_keep) {
        if (unlink(written[0]) == 0) {
            port_log("port> snapshot: dropped %s (--snap-keep %d)\n", written[0],
                     port_opt.snap_keep);
        }
        for (i = 1; i < nwritten; i++) {
            memcpy(written[i - 1], written[i], sizeof(written[0]));
        }
        nwritten--;
    }
}

void port_snap_report_existing(void) {
    int i;
    if (!nwritten) {
        return;
    }
    port_log("port> snapshots on disk, newest last:\n");
    for (i = 0; i < nwritten; i++) {
        port_log("port>   %s\n", written[i]);
    }
    port_log("port> restore one with --restore FILE (add --dumpframe or run it "
             "under gdb)\n");
}

/* ---- --snapdiff ---------------------------------------------------------- */
/* Digest the arenas region by region.  Two runs that are meant to be the same
 * run print the same table; the first line that differs says which megabyte to
 * look at, which is how a missing piece of state is found (PLAN.md §24.2). */
static void snap_diff_dump(unsigned frame) {
    const u8* m = (const u8*)port_mem1_lo();
    const u8* a = (const u8*)port_aram();
    u32 i;
    int r;
    long slide = image_slide();

    /* 64 KB granularity: coarse enough to be one log line, fine enough that
     * the offset it names can be looked up against the heap bases the boot
     * prints (OSCreateHeap) and turned into "which heap". */
    port_log("port> snapdiff f%u mem1:", frame);
    for (i = 0; i < PORT_MEM1_SIZE; i += 0x00010000u) {
        port_log(" %08x", fnv(m + i, 0x00010000u, 2166136261u));
    }
    port_log("\nport> snapdiff f%u aram:", frame);
    for (i = 0; i < PORT_ARAM_SIZE; i += 0x00400000u) {
        port_log(" %08x", fnv(a + i, 0x00400000u, 2166136261u));
    }
    port_log("\nport> snapdiff f%u gdata:", frame);
    for (r = 0; r < nranges; r++) {
        port_log(" %08x", fnv((const void*)((char*)(long)ranges[r].addr + slide),
                              ranges[r].size, 2166136261u));
    }
    /* The port's own registered state, entry by entry and by name: if a
     * restored run diverges, the first line that differs from the straight
     * run's names the thing that was not carried. */
    port_log("\nport> snapdiff f%u regs:", frame);
    for (r = 0; r < reg_count; r++) {
        port_log(" %s=%08x", reg[r].name, fnv(reg[r].p, reg[r].size, 2166136261u));
    }
    port_log("\n");
}

/* ---- taking one ---------------------------------------------------------- */

/* ---- the write, off the game thread (M18, PLAN.md 33.5) --------------------
 *
 * M17's leave-behind soak paid 3.1 s for every snapshot -- 40 MB through
 * fwrite on the game thread -- which at real time is a resync every 83 s of
 * game (§32.5).  The image is now *serialised into memory* at the retrace
 * boundary (the same instant, the same bytes: the restore path is untouched)
 * and a worker thread writes and renames it while the game runs on.  The
 * game thread's cost is one 40 MB copy.  The worker touches nothing but its
 * own buffer and the file; the bookkeeping (the ring of kept files, the log
 * line) is done by the game thread when it next finds the job finished.
 * `--snapsync` is the old synchronous write, for comparison. */
typedef struct SnapBuf {
    u8* p;
    size_t len, cap;
    int ok;
} SnapBuf;

static int wr(SnapBuf* b, const void* p, size_t n) {
    if (!b->ok) {
        return 0;
    }
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : (size_t)1 << 20;
        u8* np;
        while (nc < b->len + n) {
            nc *= 2;
        }
        np = (u8*)realloc(b->p, nc);
        if (!np) {
            b->ok = 0;
            return 0;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 1;
}

typedef struct SnapJob {
    volatile int state;      /* 0 idle, 1 writing, 2 done, 3 failed */
    pthread_t thread;
    int threaded;
    SnapBuf buf;
    char path[1100];
    char tmp[1100];
    double t0, t_copy;
    unsigned frame, nmods, stack_kb;
    unsigned long mb;
} SnapJob;
static SnapJob job;
static unsigned stat_skipped_busy;

static int snap_job_write_file(SnapJob* j) {
    FILE* f = fopen(j->tmp, "wb");
    int ok;
    if (!f) {
        return 0;
    }
    ok = fwrite(j->buf.p, 1, j->buf.len, f) == j->buf.len;
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        unlink(j->tmp);
        return 0;
    }
    if (rename(j->tmp, j->path) != 0) {
        unlink(j->tmp);
        return 0;
    }
    return 1;
}

static void* snap_job_main(void* arg) {
    SnapJob* j = (SnapJob*)arg;
    int ok = snap_job_write_file(j);
    j->state = ok ? 2 : 3;
    return NULL;
}

/* Called by the game thread: reap a finished write (log it, keep the ring). */
static void snap_job_finish(void) {
    if (job.state == 1 || job.state == 0) {
        return;
    }
    if (job.threaded) {
        pthread_join(job.thread, NULL);
        job.threaded = 0;
    }
    if (job.state == 3) {
        port_log("port> snapshot: write failed, %s left alone\n", job.path);
    } else {
        stat_last_cost = port_now_seconds() - job.t0;
        stat_written++;
        stat_bytes += (unsigned long)job.buf.len;
        port_log("port> snapshot: %s at frame %u -- %lu MB, %.0f ms on the game thread, "
                 "written in %.2f s behind it (stack %u KB, %u modules)\n",
                 job.path, job.frame, job.mb, job.t_copy * 1000.0, stat_last_cost,
                 job.stack_kb, job.nmods);
        snap_note_written(job.path);
    }
    free(job.buf.p);
    memset(&job.buf, 0, sizeof(job.buf));
    job.state = 0;
}

static void snap_write(const char* path) {
    SnapBuf* f;
    SnapHeader h;
    double t0 = port_now_seconds();
    int i, ok = 1;
    u8* stack_top = (u8*)port_game_stack_top();
    u8* stack_lo;
    long slide = image_slide();

    snap_job_finish();
    if (job.state == 1) {
        stat_skipped_busy++;
        port_log("port> snapshot: skipped at frame %u, the previous one is still being "
                 "written\n", gl13_frame_number());
        return;
    }

    stack_lo = stack_low_water ? stack_low_water : stack_top - 0x1000;
    if (stack_lo > stack_top) {
        stack_lo = stack_top;
    }
    stack_lo -= SNAP_STACK_MARGIN;

    memset(&job, 0, sizeof(job));
    snprintf(job.path, sizeof(job.path), "%s", path);
    snprintf(job.tmp, sizeof(job.tmp), "%s.tmp", path);
    job.t0 = t0;
    f = &job.buf;
    f->ok = 1;
    f->cap = (size_t)PORT_MEM1_SIZE + PORT_ARAM_SIZE + ranges_bytes + ((size_t)2 << 20);
    f->p = (u8*)malloc(f->cap);
    if (!f->p) {
        port_log("port> snapshot: cannot allocate %lu MB for %s\n",
                 (unsigned long)(f->cap >> 20), path);
        return;
    }
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, SNAP_MAGIC, 8);
    h.version = SNAP_VERSION;
    h.build_id = build_id;
    h.frame = gl13_frame_number();
    h.mem1_addr = (u32)(uintptr_t)port_mem1_lo();
    h.mem1_size = PORT_MEM1_SIZE;
    h.aram_addr = (u32)(uintptr_t)port_aram();
    h.aram_size = PORT_ARAM_SIZE;
    h.stack_lo = (u32)(uintptr_t)stack_lo;
    h.stack_size = (u32)(stack_top - stack_lo);
    h.nranges = (u32)nranges;
    h.ranges_bytes = (u32)ranges_bytes;
    h.nmods = (u32)port_dll_snap_count();
    h.nregs = (u32)reg_count;
    memcpy(h.ctx, snap_ctx, SNAP_CTX_BYTES);
    ok &= wr(f, &h, sizeof(h));

    /* 1. the game's writable globals, table then bytes */
    ok &= wr(f, ranges, sizeof(SnapRange) * (size_t)nranges);
    for (i = 0; i < nranges && ok; i++) {
        ok &= wr(f, (const void*)((char*)(long)ranges[i].addr + slide),
                 ranges[i].size);
    }

    /* 2. the REL bundles, in load order */
    for (i = 0; i < (int)h.nmods && ok; i++) {
        const char* name = NULL;
        void *handle = NULL, *image = NULL, *lo = NULL;
        unsigned long size = 0;
        int open = 0, stuck = 0;
        SnapMod m;
        if (!port_dll_snap_get(i, &name, &handle, &image, &lo, &size, &open,
                               &stuck)) {
            continue;
        }
        memset(&m, 0, sizeof(m));
        snprintf(m.name, sizeof(m.name), "%s", name ? name : "?");
        m.handle = (u32)(uintptr_t)handle;
        m.image = (u32)(uintptr_t)image;
        m.data_lo = (u32)(uintptr_t)lo;
        m.data_size = (u32)size;
        m.open = (u32)open;
        m.stuck = (u32)stuck;
        ok &= wr(f, &m, sizeof(m));
        if (lo && size) {
            ok &= wr(f, lo, size);
        }
    }

    /* 3. the port's registered state */
    for (i = 0; i < reg_count && ok; i++) {
        SnapRegHdr rh;
        memset(&rh, 0, sizeof(rh));
        snprintf(rh.name, sizeof(rh.name), "%s", reg[i].name);
        rh.size = (u32)reg[i].size;
        ok &= wr(f, &rh, sizeof(rh));
        ok &= wr(f, reg[i].p, reg[i].size);
    }

    /* 4. the arenas and the host game stack */
    ok &= wr(f, port_mem1_lo(), PORT_MEM1_SIZE);
    ok &= wr(f, port_aram(), PORT_ARAM_SIZE);
    ok &= wr(f, stack_lo, (size_t)h.stack_size);

    if (!ok || !f->ok) {
        port_log("port> snapshot: could not serialise %s\n", path);
        free(f->p);
        memset(&job, 0, sizeof(job));
        return;
    }
    job.t_copy = port_now_seconds() - t0;
    job.frame = h.frame;
    job.nmods = h.nmods;
    job.stack_kb = h.stack_size >> 10;
    job.mb = (unsigned long)(f->len >> 20);
    job.state = 1;
    /* Atomic either way: the file is written as .tmp and renamed, so a
     * snapshot only ever appears complete (a crash mid-write leaves a .tmp
     * that no restore would trust). */
    if (port_opt.snapsync || pthread_create(&job.thread, NULL, snap_job_main, &job) != 0) {
        job.threaded = 0;
        job.state = snap_job_write_file(&job) ? 2 : 3;
        snap_job_finish();
        return;
    }
    job.threaded = 1;
}

/* The safe point.  `gcsetjmp` here and the restore's `gclongjmp` lands back in
 * this function, on a stack that has just been restored byte for byte. */
static void snap_take(unsigned frame) {
    char path[1100];
    rt_join("snapshot"); /* M27: the stream drained before the game's memory is serialised */
    if (gcsetjmp(snap_ctx) != 0) {
        port_log("port> snapshot: resumed\n");
        return;
    }
    snprintf(path, sizeof(path), "%s/f%06u.snap", snap_dir, frame);
    snap_write(path);
}

/* ---- the tick ------------------------------------------------------------ */

void port_snap_tick(void) {
    unsigned frame;
    u8 probe;
    u8* sp = &probe;
    u8* stack_top = (u8*)port_game_stack_top();

    /* Track how deep the host game stack has ever been used, so a snapshot
     * saves the live frames and not 8 MB of untouched pages. */
    if (sp < stack_top && sp > stack_top - 0x00800000) {
        if (!stack_low_water || sp < stack_low_water) {
            stack_low_water = sp;
        }
    }
    if (!snap_ready) {
        return;
    }
    if (job.state >= 2) {
        snap_job_finish();
    }
    frame = gl13_frame_number();
    if (port_opt.snapdiff) {
        /* Every 50 frames, and independent of the snapshot cadence: the point
         * of a diff run is to find the *first* frame at which a restored run
         * stops agreeing with a straight one. */
        static unsigned last_diff;
        if (frame && (frame % 50) == 0 && frame != last_diff) {
            last_diff = frame;
            snap_diff_dump(frame);
        }
    }
    if (port_opt.snap_now && frame == (unsigned)port_opt.snap_now) {
        static int done_one;
        if (!done_one) {
            done_one = 1;
            snap_take(frame);
        }
        return;
    }
    if (port_opt.snap_every > 0 && frame &&
        (frame % (unsigned)port_opt.snap_every) == 0) {
        static unsigned last_taken;
        if (frame != last_taken) {
            last_taken = frame;
            snap_take(frame);
        }
    }
}

/* ---- restoring ----------------------------------------------------------- */

int port_snap_restore_pending(void) { return port_opt.restore != NULL; }

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

void port_snap_restore(void) {
    FILE* f = fopen(port_opt.restore, "rb");
    SnapHeader h;
    SnapRange* rr;
    int i;
    long slide = image_slide();
    double t0 = port_now_seconds();

    if (!f) {
        port_fatal("--restore: cannot open %s", port_opt.restore);
    }
    if (!rd(f, &h, sizeof(h)) || memcmp(h.magic, SNAP_MAGIC, 8) != 0 ||
        h.version != SNAP_VERSION) {
        port_fatal("--restore: %s is not a snapshot of this format",
                   port_opt.restore);
    }
    if (!snap_ready) {
        port_fatal("--restore: this build has no snapmap, so it cannot place "
                   "the game's globals");
    }
    if (h.build_id != build_id) {
        if (!port_opt.restore_lax) {
            port_fatal("--restore: %s was taken by a different build (%08x, this "
                       "one is %08x).  A snapshot restores the game's globals by "
                       "address; a rebuild moves them (--restore-lax only for a "
                       "re-link of the same source)",
                       port_opt.restore, h.build_id, build_id);
        }
        /* M19: the id also hashes the executable's size and mtime, so a
         * re-link of the same source (a bundle rebuilt, a file touched)
         * orphans a whole snapshot library.  The range table below still
         * checks every global's address and size, and the modules their
         * addresses -- but MEM1 also holds code addresses (process callbacks,
         * coroutine LRs), which nothing here can check, so this is for the
         * same source only, and it says so out loud. */
        port_log("port> --restore-lax: %s was taken by build %08x, this one is %08x; "
                 "continuing because the range table is checked below\n",
                 port_opt.restore, h.build_id, build_id);
    }
    if (h.mem1_addr != (u32)(uintptr_t)port_mem1_lo() ||
        h.aram_addr != (u32)(uintptr_t)port_aram() ||
        h.mem1_size != PORT_MEM1_SIZE || h.aram_size != PORT_ARAM_SIZE) {
        port_fatal("--restore: the arenas are at different addresses this run "
                   "(MEM1 %08x vs %08x, ARAM %08x vs %08x)",
                   (u32)(uintptr_t)port_mem1_lo(), h.mem1_addr,
                   (u32)(uintptr_t)port_aram(), h.aram_addr);
    }
    if ((u32)nranges != h.nranges) {
        port_fatal("--restore: the snapmap has %d ranges, the snapshot %u",
                   nranges, h.nranges);
    }

    rr = (SnapRange*)calloc((size_t)h.nranges, sizeof(*rr));
    if (!rd(f, rr, sizeof(*rr) * (size_t)h.nranges)) {
        port_fatal("--restore: truncated at the range table");
    }
    for (i = 0; i < (int)h.nranges; i++) {
        if (rr[i].addr != ranges[i].addr || rr[i].size != ranges[i].size) {
            port_fatal("--restore: range %d moved (%08x+%x vs %08x+%x)", i,
                       rr[i].addr, rr[i].size, ranges[i].addr, ranges[i].size);
        }
        if (!rd(f, (void*)((char*)(long)ranges[i].addr + slide), ranges[i].size)) {
            port_fatal("--restore: truncated in the game's globals");
        }
    }

    /* The modules, in the order the snapshot loaded them.  Their addresses
     * have to come back the same, because the game keeps pointers into them
     * (and `omDllData` keeps the dlopen handle itself) in memory this restore
     * is about to overwrite. */
    for (i = 0; i < (int)h.nmods; i++) {
        SnapMod m;
        void *handle = NULL, *image = NULL, *lo = NULL;
        unsigned long size = 0;
        if (!rd(f, &m, sizeof(m))) {
            port_fatal("--restore: truncated at module %d", i);
        }
        if (!port_dll_snap_reopen(m.name, &handle, &image, &lo, &size,
                                  (int)m.open, (int)m.stuck)) {
            port_fatal("--restore: cannot load REL bundle %s again", m.name);
        }
        /* Only the image address is compared.  dyld's own handle is a
         * malloc'd object and moves from run to run, which is exactly why the
         * game is handed the mach_header as its token instead (dll_load.c). */
        (void)handle;
        if ((u32)(uintptr_t)image != m.image) {
            port_fatal("--restore: %s came back at %08x, not %08x.  The module "
                       "bundles are linked at fixed addresses (-seg1addr), so "
                       "this means a different build or a different loader",
                       m.name, (u32)(uintptr_t)image, m.image);
        }
        if ((u32)(uintptr_t)lo != m.data_lo || (u32)size != m.data_size) {
            port_fatal("--restore: %s __DATA moved (%08x+%x vs %08x+%x)", m.name,
                       (u32)(uintptr_t)lo, (u32)size, m.data_lo, m.data_size);
        }
        if (m.data_size && !rd(f, lo, m.data_size)) {
            port_fatal("--restore: truncated in %s's data", m.name);
        }
    }

    /* The port's own registered state, matched by name: some of it lives in
     * buffers this process allocated at a different address (the card image,
     * the audio DMA buffers), so the bytes go where *this* run put them. */
    for (i = 0; i < (int)h.nregs; i++) {
        SnapRegHdr rh;
        int j, placed = 0;
        if (!rd(f, &rh, sizeof(rh))) {
            port_fatal("--restore: truncated at registry entry %d", i);
        }
        for (j = 0; j < reg_count; j++) {
            if (!strcmp(reg[j].name, rh.name) && reg[j].size == rh.size) {
                if (!rd(f, reg[j].p, rh.size)) {
                    port_fatal("--restore: truncated in registry entry %s", rh.name);
                }
                placed = 1;
                break;
            }
        }
        if (!placed) {
            /* Skip it rather than refuse: a registry that has gained an entry
             * is a build change, which the build id has already caught; this
             * is the belt to that braces. */
            if (fseek(f, (long)rh.size, SEEK_CUR) != 0) {
                port_fatal("--restore: cannot skip registry entry %s", rh.name);
            }
            port_log("port> --restore: registry entry %s is not in this build, "
                     "skipped\n", rh.name);
        }
    }

    if (!rd(f, port_mem1_lo(), PORT_MEM1_SIZE) ||
        !rd(f, port_aram(), PORT_ARAM_SIZE) ||
        !rd(f, (void*)(uintptr_t)h.stack_lo, h.stack_size)) {
        port_fatal("--restore: truncated in the arenas");
    }
    fclose(f);

    /* Re-derived, not restored: the texture cache is keyed on addresses in the
     * MEM1 that has just been replaced, and its GL names belong to this
     * process's context. */
    gx_tex_flush_all();
    gl13_set_frame_number(h.frame);
    stack_low_water = (u8*)(uintptr_t)h.stack_lo + SNAP_STACK_MARGIN;

    port_log("port> --restore: %s, frame %u, %u modules, %u registry entries, "
             "%.2f s\n",
             port_opt.restore, h.frame, h.nmods, h.nregs,
             port_now_seconds() - t0);
    port_log("port> --restore: resuming the game\n\n");
    memcpy(snap_ctx, h.ctx, SNAP_CTX_BYTES);
    gclongjmp(snap_ctx, 1);
    port_fatal("--restore: gclongjmp returned, which cannot happen");
}

/* ---- init ---------------------------------------------------------------- */

void port_snap_init(void) {
    if (!port_opt.snap_every && !port_opt.snap_now && !port_opt.restore &&
        !port_opt.snapdiff) {
        return;
    }
    load_snapmap();
    snap_dir_init();
    port_vi_snap_register();
    gl13_snap_register();
    gx_state_snap_register();
    os_misc_snap_register();
    musyx_sal_snap_register();
    port_idle_snap_register();
    port_card_snap_register();
    port_selfplay_snap_register();
    port_aram_snap_register();
    port_sreset_snap_register();
    port_pad_snap_register();
    port_musyx_mix_snap_register();
    port_aram_musyx_snap_register();
    port_log("port> snapshot: %d registry entries, ring in %s%s\n", reg_count,
             snap_dir, port_opt.restore ? " (restoring)" : "");
    if (port_opt.dlcache) {
        port_log("port> snapshot: --dlcache holds decoded vertices in host "
                 "memory keyed on MEM1 addresses; it is dropped for this run\n");
        port_opt.dlcache = 0;
    }
}

void port_snap_report(void) {
    /* a write still in flight at shutdown is finished, not abandoned */
    if (job.state == 1 && job.threaded) {
        pthread_join(job.thread, NULL);
        job.threaded = 0;
    }
    snap_job_finish();
    if (!stat_written) {
        return;
    }
    port_log("port> snapshots: %lu written, %lu MB, last one cost %.2f s%s; %u skipped "
             "with the previous write still in flight\n",
             stat_written, stat_bytes >> 20, stat_last_cost,
             port_opt.snapsync ? " (--snapsync)" : " behind the game thread",
             stat_skipped_busy);
}
