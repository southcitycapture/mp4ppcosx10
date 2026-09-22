/* The loader (M36, PLAN.md 51): the roulette prefetch and the resident set.
 *
 * The G4's disk is a shingled 2.5" drive on the ATA bus that reads 13 MB/s
 * when the page cache has dropped a file (PLAN.md 46.1), and every module
 * link is a synchronous read of that disk on the game thread: the REL out
 * of the disc image, the Mach-O bundle through dlopen, the module's data
 * directory image (1-3 MB).  A cold minigame load is a 300-800 ms `DVD:
 * read of ...` line and a resync (soak 21: m415.bin 811 ms).  Two answers,
 * both of them enhancements the machine check sizes, neither of them a
 * change to *when* a read completes -- dvd_fs.c still answers every read
 * inline, before DVDReadAsync returns (PLAN.md 32.1's rule); these only
 * make the answer come from RAM:
 *
 *   1. The prefetch.  When the roulette deals a minigame (mg_setup.c's
 *      `mgNext = ...`, the game's own moment, through the patch hook
 *      port_mg_dealt -- 350 frames before instDll links, and the other
 *      dealers (Bowser, battle, fortune, the story mode) through the
 *      GWSystem.mg_next watch at the retrace), a low-priority thread reads
 *      the module's REL, its bundle and its data file whole, so the game's
 *      own reads of them a few seconds later come out of the page cache.
 *      The same at the results screen for the board the game is about to
 *      re-enter, and at the mode select for the board set.  On one CPU
 *      (workers off) the same reads run inline at the retrace in 64 KB
 *      slices, only inside the schedule's slack (vi.c), so a slice never
 *      delays a retrace -- or not at all when there is no slack.
 *
 *   2. The resident set.  A budget of the files every game reads (the list
 *      in resident_list.h, derived from a --dvdlog run's counts: the
 *      board's data, instDll's, resultDll's, the characters', the mode
 *      select's), read once into memory -- mlock'd where the OS allows it
 *      to an ordinary user -- and served to the game's DVDRead from there
 *      (memcpy; the bytes are the disc's).  The list is filled in order
 *      until the budget is spent; a prefetched file joins the set when it
 *      fits and the least recently used file leaves when it does not.  The
 *      budget is the machine check's: 0 under 768 MB of RAM, 128 MB at
 *      1 GB, 256 MB at 1.5 GB and up, and never so much that the OS and the
 *      process have less than 584 MB (machine.c).  The file tree is read
 *      only, so nothing here can ever be stale.
 *
 * What is never changed: the order and timing of the game's own reads, the
 * bytes they receive, the retrace schedule.  --noprefetch and --resident 0
 * are the A/B levers; --dvdlog names every read with where it came from.
 */
#include "port.h"

#include "game/gamework_data.h"
#include "game/object.h"
#include "game/objsub.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

unsigned gl13_frame_number(void);
int port_threads_on(void);

/* ---- the game's two tables, read the way data.c and ovllist.c read them -- */

#define DATADIR(name, path) path,
static const char* const datadir_path[] = {
#include "datadir_table.h"
    NULL
};
#undef DATADIR
#define DATADIR_COUNT ((int)(sizeof(datadir_path) / sizeof(datadir_path[0])) - 1)

#define DLL(name) #name,
static const char* const ovl_name[] = {
#include "ovl_table.h"
    NULL
};
#undef DLL
#define OVL_COUNT ((int)(sizeof(ovl_name) / sizeof(ovl_name[0])) - 1)

/* the resident set's list, most-read first (PLAN.md 51.3) */
static const char* const resident_list[] = {
#include "resident_list.h"
    NULL
};

/* ---- the resident set ---------------------------------------------------- */

typedef struct Res {
    u8* buf;                /* the whole file, or NULL */
    u32 len;
    unsigned long last_use; /* the use clock at the last serve */
    unsigned long hits;
    int pinned;             /* on the list: evicted after everything else */
    int queued;             /* a job for it is in the queue or running */
    int locked;             /* mlock succeeded */
} Res;

static Res* res;
static int nres;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long budget_bytes;
static unsigned long total_bytes;
static unsigned long use_clock;
static int mlock_ok = 1; /* until the first refusal */
static int budget_mb_resolved;

/* stats */
static unsigned long stat_served, stat_served_bytes, stat_disk, stat_disk_bytes;
static unsigned long stat_adopted, stat_adopted_bytes, stat_evicted, stat_evicted_bytes;
static unsigned long stat_locked_bytes;
static double stat_disk_s;
static unsigned long stat_pre_jobs, stat_pre_files, stat_pre_bytes, stat_pre_skipped;
static double stat_pre_s, stat_pre_worst_s;
static unsigned long stat_slices;
static double stat_slice_s;
static unsigned long stat_disk_after_pre, stat_disk_after_pre_over100;

/* ---- the queue: the game thread fills it, the thread (or the slice) drains it */

#define QMAX 96
typedef struct Job {
    int entry;         /* an FST entry, or -1 for a bundle */
    char path[1024];   /* the bundle's file when entry < 0 */
    const char* why;   /* "roulette", "results", "modesel", "list" */
    int keep;          /* hold it resident (else read and drop: the page cache) */
    unsigned frame;    /* when it was asked for */
} Job;
static Job q[QMAX];
static unsigned qh, qt;
static pthread_cond_t cv_job = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int thread_up, quit;
static int fd = -1;         /* the thread's own descriptor on the image */
static u8* scratch;         /* the read-and-drop buffer */
#define SCRATCH (256 * 1024)
#define SLICE (64 * 1024)   /* the one-CPU inline read per retrace */

/* one line per finished job, written by the game thread at the retrace so
 * the log's order is the game's */
typedef struct Done {
    char name[96];
    const char* why;
    unsigned long bytes;
    double ms;
    int kept;
    unsigned frame;
} Done;
#define DMAX 32
static Done done[DMAX];
static unsigned dh, dt_;

/* the "was it prefetched" mark per entry, for the report */
static unsigned char* prefetched;

/* ---- --dvdlog's ring ------------------------------------------------------
 *
 * A read is answered wherever the game asks for it, and that is not always
 * the game thread on its own stack: MusyX's stream update runs from the
 * mixer's job, and `msmStreamDvdCallback` issues the *next* read from
 * inside the completion of the last one (the port's completions are inline,
 * PLAN.md 51.2), so the read path is re-entered several levels deep at a
 * stream's loop point.  A `port_log` there puts a `vfprintf` -- a kilobyte
 * or two of stack -- on every level of that chain, and the bench's 20-turn
 * soak died at the same frame twice with `--dvdlog` on and never without it
 * (PLAN.md 51.7).  So the read path only *records* its line, in a fixed
 * ring, and the retrace prints it: no formatting, no allocation and no
 * stdio below `do_read`, and the log comes out in the game's own order. */
typedef struct DvdLine {
    unsigned frame;
    int entry;
    unsigned offset, len;
    float ms;      /* < 0: the resident set answered */
    unsigned char pre;
} DvdLine;
#define DVDLOG_RING 512
static DvdLine dvdlog_ring[DVDLOG_RING];
static volatile unsigned dvdlog_head, dvdlog_tail; /* tail written by any thread */
static unsigned long stat_dvdlog_lost;

static void dvdlog_note(int entry, unsigned offset, unsigned len, float ms, int pre) {
    unsigned t;
    if (!port_opt.dvdlog) {
        return;
    }
    pthread_mutex_lock(&mu);
    t = dvdlog_tail;
    if (t - dvdlog_head >= DVDLOG_RING) {
        stat_dvdlog_lost++;
    } else {
        DvdLine* d = &dvdlog_ring[t % DVDLOG_RING];
        d->frame = gl13_frame_number();
        d->entry = entry;
        d->offset = offset;
        d->len = len;
        d->ms = ms;
        d->pre = (unsigned char)pre;
        dvdlog_tail = t + 1;
    }
    pthread_mutex_unlock(&mu);
}

/* the game thread, at the retrace */
static void dvdlog_drain(void) {
    for (;;) {
        DvdLine d;
        pthread_mutex_lock(&mu);
        if (dvdlog_head == dvdlog_tail) {
            pthread_mutex_unlock(&mu);
            return;
        }
        d = dvdlog_ring[dvdlog_head % DVDLOG_RING];
        dvdlog_head++;
        pthread_mutex_unlock(&mu);
        if (d.ms < 0.0f) {
            port_log("port> dvd: f%u %s +%u %u resident\n", d.frame, port_dvd_entry_path(d.entry),
                     d.offset, d.len);
        } else {
            port_log("port> dvd: f%u %s +%u %u %.1f ms disk%s\n", d.frame,
                     port_dvd_entry_path(d.entry), d.offset, d.len, (double)d.ms,
                     d.pre ? " (prefetched)" : "");
        }
    }
}

static int find_entry(const char* path) {
    int i, n = port_dvd_entry_count();
    for (i = 0; i < n; i++) {
        if (strcasecmp(port_dvd_entry_path(i), path) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- reading a file whole, off the game thread's FILE* ------------------- */

static int open_source(void) {
    const char* img = port_dvd_image_path();
    if (img) {
        fd = open(img, O_RDONLY);
        return fd >= 0;
    }
    return 1; /* the tree backend opens per file */
}

/* read [off, off+len) of entry n into dst (or, dst NULL, into the scratch
 * buffer and drop it); the number of bytes read */
static u32 read_range(int n, u32 off, u32 len, u8* dst) {
    u32 got = 0;
    if (port_dvd_image_path()) {
        off_t base = (off_t)port_dvd_entry_offset(n) + off;
        if (fd < 0) {
            return 0;
        }
        while (got < len) {
            u32 want = len - got;
            u8* to = dst ? dst + got : scratch;
            ssize_t r;
            if (!dst && want > SCRATCH) {
                want = SCRATCH;
            }
            r = pread(fd, to, want, base + got);
            if (r <= 0) {
                break;
            }
            got += (u32)r;
        }
    } else {
        char full[1200];
        int f;
        snprintf(full, sizeof(full), "%s/%s", port_dvd_tree_root(), port_dvd_entry_path(n));
        f = open(full, O_RDONLY);
        if (f < 0) {
            return 0;
        }
        while (got < len) {
            u32 want = len - got;
            u8* to = dst ? dst + got : scratch;
            ssize_t r;
            if (!dst && want > SCRATCH) {
                want = SCRATCH;
            }
            r = pread(f, to, want, (off_t)off + got);
            if (r <= 0) {
                break;
            }
            got += (u32)r;
        }
        close(f);
    }
    return got;
}

/* a bundle (or any file by path): read and drop */
static unsigned long read_file_drop(const char* path) {
    int f = open(path, O_RDONLY);
    unsigned long got = 0;
    ssize_t r;
    if (f < 0) {
        return 0;
    }
    while ((r = read(f, scratch, SCRATCH)) > 0) {
        got += (unsigned long)r;
    }
    close(f);
    return got;
}

/* ---- the set's bookkeeping (under mu) ------------------------------------ */

static void evict_one(void) {
    /* the least recently used, unpinned before pinned */
    int i, best = -1, pass;
    for (pass = 0; pass < 2 && best < 0; pass++) {
        for (i = 0; i < nres; i++) {
            if (!res[i].buf || res[i].pinned != pass) {
                continue;
            }
            if (best < 0 || res[i].last_use < res[best].last_use) {
                best = i;
            }
        }
    }
    if (best < 0) {
        return;
    }
    if (res[best].locked) {
        munlock(res[best].buf, res[best].len);
        stat_locked_bytes -= res[best].len;
    }
    free(res[best].buf);
    res[best].buf = NULL;
    res[best].locked = 0;
    total_bytes -= res[best].len;
    stat_evicted++;
    stat_evicted_bytes += res[best].len;
}

/* take ownership of buf (len bytes, entry n); 1 if it went in */
static int insert(int n, u8* buf, u32 len) {
    if (res[n].buf) {
        free(buf); /* a race between the list and a prefetch: keep the first */
        return 1;
    }
    if ((unsigned long)len > budget_bytes) {
        free(buf);
        return 0;
    }
    while (total_bytes + len > budget_bytes) {
        unsigned long before = total_bytes;
        evict_one();
        if (total_bytes == before) {
            free(buf);
            return 0;
        }
    }
    res[n].buf = buf;
    res[n].len = len;
    res[n].last_use = ++use_clock;
    total_bytes += len;
    if (mlock_ok) {
        if (mlock(buf, len) == 0) {
            res[n].locked = 1;
            stat_locked_bytes += len;
        } else {
            mlock_ok = 0; /* say so once, at the report; plain memory from here */
        }
    }
    return 1;
}

/* ---- one job ------------------------------------------------------------- */

static void job_finish(const Job* j, unsigned long bytes, double dt, int kept) {
    pthread_mutex_lock(&mu);
    if (dt_ - dh < DMAX) {
        Done* d = &done[dt_ % DMAX];
        const char* name = j->entry >= 0 ? port_dvd_entry_path(j->entry) : strrchr(j->path, '/');
        snprintf(d->name, sizeof(d->name), "%s", name ? (j->entry >= 0 ? name : name + 1) : "?");
        d->why = j->why;
        d->bytes = bytes;
        d->ms = dt * 1000.0;
        d->kept = kept;
        d->frame = j->frame;
        dt_++;
    }
    if (j->entry >= 0) {
        res[j->entry].queued = 0;
        prefetched[j->entry] = 1;
    }
    stat_pre_files++;
    stat_pre_bytes += bytes;
    stat_pre_s += dt;
    if (dt > stat_pre_worst_s) {
        stat_pre_worst_s = dt;
    }
    pthread_mutex_unlock(&mu);
}

/* the whole of a job, on the thread */
static void run_job(const Job* j) {
    double t0 = port_now_seconds();
    unsigned long bytes = 0;
    int kept = 0;
    if (j->entry < 0) {
        bytes = read_file_drop(j->path);
    } else {
        u32 len = port_dvd_entry_length(j->entry);
        int already;
        pthread_mutex_lock(&mu);
        already = res[j->entry].buf != NULL;
        pthread_mutex_unlock(&mu);
        if (already) {
            stat_pre_skipped++;
            pthread_mutex_lock(&mu);
            res[j->entry].queued = 0;
            pthread_mutex_unlock(&mu);
            return;
        }
        if (j->keep && budget_bytes && (unsigned long)len <= budget_bytes) {
            u8* buf = (u8*)malloc(len ? len : 1);
            if (buf) {
                bytes = read_range(j->entry, 0, len, buf);
                if (bytes == len) {
                    pthread_mutex_lock(&mu);
                    kept = insert(j->entry, buf, len);
                    pthread_mutex_unlock(&mu);
                } else {
                    free(buf);
                }
            }
        } else {
            bytes = read_range(j->entry, 0, len, NULL);
        }
    }
    job_finish(j, bytes, port_now_seconds() - t0, kept);
}

static void* thread_main(void* arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        Job j;
        while (qh == qt && !quit) {
            pthread_cond_wait(&cv_job, &mu);
        }
        if (quit) {
            break;
        }
        j = q[qh % QMAX];
        qh++;
        pthread_mutex_unlock(&mu);
        run_job(&j);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* ---- the one-CPU twin: the same job, a slice a retrace ------------------- */

static struct {
    int active;
    Job j;
    u32 off, len;
    u8* buf;      /* when keeping */
    double t0;
    int bfd;      /* a bundle's descriptor */
    unsigned long got;
} sl;

static void slice_begin(const Job* j) {
    sl.active = 1;
    sl.j = *j;
    sl.off = 0;
    sl.got = 0;
    sl.buf = NULL;
    sl.bfd = -1;
    sl.t0 = port_now_seconds();
    if (j->entry >= 0) {
        sl.len = port_dvd_entry_length(j->entry);
        if (res[j->entry].buf) {
            sl.len = 0; /* already there */
        } else if (j->keep && budget_bytes && (unsigned long)sl.len <= budget_bytes) {
            sl.buf = (u8*)malloc(sl.len ? sl.len : 1);
        }
    } else {
        sl.bfd = open(j->path, O_RDONLY);
        sl.len = 0xffffffffu;
    }
}

static void slice_end(void) {
    int kept = 0;
    if (sl.j.entry >= 0 && sl.buf) {
        if (sl.got == sl.len) {
            kept = insert(sl.j.entry, sl.buf, sl.len);
        } else {
            free(sl.buf);
        }
    }
    if (sl.bfd >= 0) {
        close(sl.bfd);
    }
    pthread_mutex_unlock(&mu);
    job_finish(&sl.j, sl.got, port_now_seconds() - sl.t0, kept);
    pthread_mutex_lock(&mu);
    sl.active = 0;
}

/* under mu; returns having read at most one slice */
static void slice_step(void) {
    if (!sl.active) {
        if (qh == qt) {
            return;
        }
        slice_begin(&q[qh % QMAX]);
        qh++;
    }
    if (sl.j.entry >= 0) {
        u32 want = sl.len - sl.off;
        u32 got;
        if (want > SLICE) {
            want = SLICE;
        }
        if (want == 0) {
            slice_end();
            return;
        }
        got = read_range(sl.j.entry, sl.off, want, sl.buf ? sl.buf + sl.off : NULL);
        sl.off += got;
        sl.got += got;
        if (got < want || sl.off >= sl.len) {
            slice_end();
        }
    } else {
        ssize_t r = sl.bfd >= 0 ? read(sl.bfd, scratch, SLICE) : 0;
        if (r > 0) {
            sl.got += (unsigned long)r;
        } else {
            slice_end();
        }
    }
}

/* ---- asking for a file --------------------------------------------------- */

static void enqueue_entry(int n, const char* why, int keep) {
    Job* j;
    if (n < 0 || n >= nres) {
        return;
    }
    pthread_mutex_lock(&mu);
    if (res[n].buf || res[n].queued || qt - qh >= QMAX) {
        pthread_mutex_unlock(&mu);
        return;
    }
    j = &q[qt % QMAX];
    j->entry = n;
    j->path[0] = '\0';
    j->why = why;
    j->keep = keep;
    j->frame = gl13_frame_number();
    res[n].queued = 1;
    qt++;
    stat_pre_jobs++;
    pthread_cond_signal(&cv_job);
    pthread_mutex_unlock(&mu);
}

static void enqueue_path(const char* path, const char* why) {
    Job* j;
    if (!path) {
        return;
    }
    pthread_mutex_lock(&mu);
    if (qt - qh >= QMAX) {
        pthread_mutex_unlock(&mu);
        return;
    }
    j = &q[qt % QMAX];
    j->entry = -1;
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->why = why;
    j->keep = 0;
    j->frame = gl13_frame_number();
    qt++;
    stat_pre_jobs++;
    pthread_cond_signal(&cv_job);
    pthread_mutex_unlock(&mu);
}

static void enqueue_named(const char* path, const char* why) {
    enqueue_entry(find_entry(path), why, 1);
}

/* the module: its REL out of the image and its bundle from the app */
static void enqueue_module(int ovl, const char* why) {
    char rel[96];
    if (ovl < 0 || ovl >= OVL_COUNT) {
        return;
    }
    snprintf(rel, sizeof(rel), "dll/%s.rel", ovl_name[ovl]);
    enqueue_named(rel, why);
    enqueue_path(port_dll_bundle_path(rel), why);
}

static void enqueue_datadir(u32 datanum, const char* why) {
    int dir = (int)(datanum >> 16);
    if (dir >= 0 && dir < DATADIR_COUNT) {
        enqueue_named(datadir_path[dir], why);
    }
}

/* a dealt minigame: the module, its data, the instruction screen's */
static int last_mg = -1;
static void prefetch_minigame(int mg, const char* why) {
    if (mg < 0 || mg >= 64 || mgInfoTbl[mg].ovl == 0xFFFF || mg == last_mg || port_opt.noprefetch) {
        return;
    }
    last_mg = mg;
    port_log("port> loader: %s dealt mg %d (%s) at frame %u -- prefetching its module and data\n",
             why, mg + 0x191, ovl_name[mgInfoTbl[mg].ovl], gl13_frame_number());
    enqueue_module((int)mgInfoTbl[mg].ovl, why);
    enqueue_datadir(mgInfoTbl[mg].data_dir, why);
    enqueue_module(DLL_instdll, why);
    enqueue_named("data/inst.bin", why);
    enqueue_named("data/instpic.bin", why);
    enqueue_module(DLL_resultdll, why);
    enqueue_named("data/result.bin", why);
}

static int board_ovl(int board) {
    static const int tbl[9] = { DLL_w01dll, DLL_w02dll, DLL_w03dll, DLL_w04dll, DLL_w05dll,
                                DLL_w06dll, DLL_w10dll, DLL_w20dll, DLL_w21dll };
    return (board >= 0 && board < 9) ? tbl[board] : -1;
}

static const char* board_data(int board) {
    static const char* const tbl[9] = { "data/w01.bin", "data/w02.bin", "data/w03.bin",
                                        "data/w04.bin", "data/w05.bin", "data/w06.bin",
                                        "data/w10.bin", "data/w20.bin", "data/w21.bin" };
    return (board >= 0 && board < 9) ? tbl[board] : NULL;
}

/* the board the game will re-enter after the results */
static void prefetch_board(int board, const char* why) {
    if (port_opt.noprefetch) {
        return;
    }
    enqueue_module(board_ovl(board), why);
    enqueue_named(board_data(board), why);
    enqueue_named("data/board.bin", why);
    enqueue_named("data/bguest.bin", why);
}

/* the board set, at the mode select: what every board start reads */
static void prefetch_board_set(const char* why) {
    int b;
    if (port_opt.noprefetch) {
        return;
    }
    enqueue_named("data/board.bin", why);
    enqueue_named("data/bguest.bin", why);
    enqueue_named("data/mgconst.bin", why);
    enqueue_module(DLL_instdll, why);
    enqueue_named("data/inst.bin", why);
    enqueue_named("data/instpic.bin", why);
    enqueue_module(DLL_resultdll, why);
    enqueue_named("data/result.bin", why);
    for (b = 0; b < 6; b++) {
        enqueue_module(board_ovl(b), why);
        enqueue_named(board_data(b), why);
    }
}

/* the patch hook: mg_setup.c:1142, the roulette's stop.  Called on the
 * roulette process's own coroutine stack (a few KB), so it only notes the
 * index; the retrace service below does the work a frame later. */
static int hook_mg = -1;
void port_mg_dealt(int mg_index) { hook_mg = mg_index; }

/* ---- init / shutdown ----------------------------------------------------- */

int port_dvd_cache_budget_mb(void) { return port_opt.resident > 0 ? port_opt.resident : 0; }

void port_dvd_cache_init(void) {
    int i, listed = 0, fits = 0;
    unsigned long list_bytes = 0, fit_bytes = 0;
    nres = port_dvd_entry_count();
    res = (Res*)calloc((size_t)nres, sizeof(Res));
    prefetched = (unsigned char*)calloc((size_t)nres, 1);
    scratch = (u8*)malloc(SCRATCH);
    budget_mb_resolved = port_dvd_cache_budget_mb();
    budget_bytes = (unsigned long)budget_mb_resolved * 1024UL * 1024UL;
    if (!open_source()) {
        port_log("port> loader: cannot open %s a second time (%s); the prefetch and the resident "
                 "set are off\n", port_dvd_image_path(), strerror(errno));
        port_opt.noprefetch = 1;
        budget_bytes = 0;
    }
    if (port_threads_on()) {
        if (pthread_create(&thread, NULL, thread_main, NULL) == 0) {
            struct sched_param sp;
            int policy = SCHED_OTHER;
            thread_up = 1;
            memset(&sp, 0, sizeof(sp));
            /* below the decoder: a disk read is nothing to wait for */
            if (pthread_getschedparam(thread, &policy, &sp) == 0) {
                sp.sched_priority -= 8;
                pthread_setschedparam(thread, policy, &sp);
            }
        }
    }
    /* the list, in order, until the budget is spent -- pinned; a file the
     * budget cannot hold is not asked for */
    for (i = 0; resident_list[i]; i++) {
        int n = find_entry(resident_list[i]);
        u32 len;
        if (n < 0) {
            continue;
        }
        len = port_dvd_entry_length(n);
        listed++;
        list_bytes += len;
        if (budget_bytes && fit_bytes + len <= budget_bytes) {
            res[n].pinned = 1;
            fit_bytes += len;
            fits++;
            enqueue_entry(n, "list", 1);
        }
    }
    port_log("port> loader: prefetch %s (%s); resident set %d MB: %d of the list's %d files "
             "fit (%.1f of %.1f MB), filling %s\n",
             port_opt.noprefetch ? "OFF" : "on",
             port_opt.noprefetch ? "--noprefetch" : thread_up ? "a thread" : "inline slices at the retrace",
             budget_mb_resolved, fits, listed, fit_bytes / 1048576.0, list_bytes / 1048576.0,
             budget_bytes == 0 ? "nothing (off)" : thread_up ? "on the thread" : "in slices");
}

void port_dvd_cache_shutdown(void) {
    if (thread_up) {
        pthread_mutex_lock(&mu);
        quit = 1;
        pthread_cond_broadcast(&cv_job);
        pthread_mutex_unlock(&mu);
        pthread_join(thread, NULL);
        thread_up = 0;
    }
}

/* ---- the game thread's side ---------------------------------------------- */

/* a read answered from the set: 1, and the bytes are in dst */
int port_dvd_cache_serve(int entry, unsigned offset, void* dst, unsigned len) {
    Res* r;
    if (!res || entry < 0 || entry >= nres) {
        return 0;
    }
    r = &res[entry];
    if (!r->buf) {
        return 0;
    }
    pthread_mutex_lock(&mu);
    if (!r->buf || offset + len > r->len) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    memcpy(dst, r->buf + offset, len);
    r->last_use = ++use_clock;
    r->hits++;
    stat_served++;
    stat_served_bytes += len;
    pthread_mutex_unlock(&mu);
    dvdlog_note(entry, offset, len, -1.0f, 0);
    return 1;
}

/* a read the disk answered: the log line, the stats, and -- a listed file
 * read whole by the game before the fill reached it -- adopt the bytes
 * rather than read them a second time */
void port_dvd_cache_disk_read(int entry, unsigned offset, const void* data, unsigned got, double dt) {
    stat_disk++;
    stat_disk_bytes += got;
    stat_disk_s += dt;
    if (res && entry >= 0 && entry < nres && prefetched[entry]) {
        stat_disk_after_pre++;
        if (dt > 0.1) {
            stat_disk_after_pre_over100++;
        }
    }
    dvdlog_note(entry, offset, got, (float)(dt * 1000.0),
                res && entry >= 0 && entry < nres && prefetched[entry]);
    if (res && entry >= 0 && entry < nres && res[entry].pinned && !res[entry].buf && offset == 0 &&
        got == port_dvd_entry_length(entry) && budget_bytes) {
        u8* buf = (u8*)malloc(got ? got : 1);
        if (buf) {
            memcpy(buf, data, got);
            pthread_mutex_lock(&mu);
            if (insert(entry, buf, got)) {
                stat_adopted++;
                stat_adopted_bytes += got;
            }
            pthread_mutex_unlock(&mu);
        }
    }
}

/* the retrace: the watches, the slice, the finished jobs' lines */
void port_dvd_cache_service(void) {
    static int last_next_ovl = -12345;
    static int last_mg_next = -1;
    int mg, nxt;
    if (!res) {
        return;
    }
    dvdlog_drain();
    if (hook_mg >= 0) {
        int h = hook_mg;
        hook_mg = -1;
        prefetch_minigame(h, "roulette");
    }
    /* the other dealers write GWSystem.mg_next directly (bowser.c, battle.c,
     * fortune.c, the story boards); the roulette's own write of it comes
     * after the hook and is the same index, so it is a no-op here */
    mg = (int)GWSystem.mg_next;
    if (mg != last_mg_next) {
        int cur = (int)omcurovl;
        last_mg_next = mg;
        /* only inside a board (board/main.c:306 writes a 0 at every board
         * start, and 0 is m401's index; a real m401 comes through the hook) */
        if (mg > 0 && mg < 64 && mgInfoTbl[mg].ovl != 0xFFFF && cur >= DLL_w01dll &&
            cur <= DLL_w21dll) {
            prefetch_minigame(mg, "mg_next");
        }
    }
    nxt = (int)omnextovl;
    if (nxt != last_next_ovl) {
        last_next_ovl = nxt;
        if (nxt == DLL_resultdll) {
            prefetch_board((int)GWSystem.board, "results");
        } else if (nxt == DLL_modeseldll) {
            prefetch_board_set("modesel");
        } else if (nxt >= 0 && nxt < OVL_COUNT && omMgIndexGet((s16)nxt) >= 0) {
            /* the minigame itself is next: its dealer has been seen; the
             * results screen after it is what follows */
            last_mg = -1;
        }
    }
    pthread_mutex_lock(&mu);
    if (!thread_up && (qh != qt || sl.active)) {
        /* the one-CPU twin: a slice only inside the schedule's slack, so the
         * retrace it belongs to is not delayed (5 ms cold at 13 MB/s) */
        double slack = port_vi_slack_seconds();
        if (slack > 0.008) {
            double t0 = port_now_seconds();
            slice_step();
            stat_slices++;
            stat_slice_s += port_now_seconds() - t0;
        }
    }
    while (dh != dt_) {
        Done d = done[dh % DMAX];
        dh++;
        pthread_mutex_unlock(&mu);
        if (port_opt.dvdlog || d.ms > 100.0 || port_opt.verbose) {
            port_log("port> loader: %s %s %lu bytes in %.0f ms%s (asked at frame %u, done at %u)\n",
                     d.why, d.name, d.bytes, d.ms, d.kept ? ", resident" : "", d.frame,
                     gl13_frame_number());
        }
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
}

void port_dvd_cache_status(char* buf, size_t n) {
    if (!res || !budget_bytes) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, n, "  res %lu/%d MB", total_bytes / 1048576UL, budget_mb_resolved);
}

void port_dvd_cache_report(void) {
    int i, held = 0, pinned_held = 0;
    if (!res) {
        return;
    }
    for (i = 0; i < nres; i++) {
        if (res[i].buf) {
            held++;
            if (res[i].pinned) {
                pinned_held++;
            }
        }
    }
    port_log("\nport> loader (M36): prefetch %s, %lu jobs, %lu files read ahead (%lu skipped as "
             "resident), %.1f MB in %.0f ms off the game thread (worst %.0f ms); %lu inline slices "
             "on one CPU (%.0f ms)\n",
             port_opt.noprefetch ? "off" : thread_up || stat_pre_files ? "on" : "idle", stat_pre_jobs,
             stat_pre_files, stat_pre_skipped, stat_pre_bytes / 1048576.0, stat_pre_s * 1000.0,
             stat_pre_worst_s * 1000.0, stat_slices, stat_slice_s * 1000.0);
    dvdlog_drain();
    port_log("port> loader: disk reads after a prefetch of the same file: %lu, %lu of them over "
             "100 ms\n", stat_disk_after_pre, stat_disk_after_pre_over100);
    if (stat_dvdlog_lost) {
        port_log("port> loader: --dvdlog dropped %lu line(s) (more than %d reads between two "
                 "retraces)\n", stat_dvdlog_lost, DVDLOG_RING);
    }
    port_log("port> loader: resident set %d MB: %d files held (%lu MB, %d of them from the list), "
             "%lu reads (%.1f MB) served from memory, %lu (%.1f MB, %.0f ms) from the disk, %lu "
             "adopted from the game's own reads, %lu evicted (%.1f MB); mlock %s (%lu MB locked)\n",
             budget_mb_resolved, held, total_bytes / 1048576UL, pinned_held, stat_served,
             stat_served_bytes / 1048576.0, stat_disk, stat_disk_bytes / 1048576.0,
             stat_disk_s * 1000.0, stat_adopted, stat_evicted, stat_evicted_bytes / 1048576.0,
             budget_bytes == 0 ? "not tried" : mlock_ok ? "allowed" : "refused (plain memory)",
             stat_locked_bytes / 1048576UL);
    if (port_opt.dvdlog && budget_bytes) {
        for (i = 0; i < nres; i++) {
            if (res[i].hits || res[i].buf) {
                port_log("port> loader:   %-28s %8u B  %s%s  %lu hits\n", port_dvd_entry_path(i),
                         port_dvd_entry_length(i), res[i].buf ? "held" : "gone",
                         res[i].pinned ? " (list)" : "", res[i].hits);
            }
        }
    }
}
