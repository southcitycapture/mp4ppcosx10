/* OSReport, OSPanic, and the loud-stub table.
 *
 * `OSReport` is called from 578 sites: the game narrates its own boot, its DLL
 * loads, its heap state and its DVD errors.  That narration is M1's success
 * criterion, so it goes to stdout and, with --log, to a file as well.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* logfile;

/* M40 (PLAN.md 55): the log's writes off the calling thread.  The M39 soak's
 * ten one-second pauses on a `--status` retrace (54.4) were the game thread's
 * own fflush of stdout, held by the file system while the drive answered
 * something else.  Now a line is formatted where it is logged (vsnprintf)
 * and appended to a ring; a writer thread does the fwrite/fflush.  The ring
 * blocks the logger only when it is full (a megabyte behind).  Ordering is
 * the ring's: every thread's lines in the order they took the lock.
 *
 * Synchronous again (the ring drained first, then written in place) for a
 * fault, a fatal error, an exit and --synclog: port_log_sync() -- the crash
 * handler's lines must reach the disk before _exit. */
#include <pthread.h>
#define LOG_RING (1u << 20)
static char* log_ring;
static unsigned log_head, log_tail; /* bytes written in / taken out, mod 2^32 */
static int log_async, log_quit, log_sync_now;
static pthread_t log_thread;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_cv = PTHREAD_COND_INITIALIZER;   /* data for the writer */
static pthread_cond_t log_room = PTHREAD_COND_INITIALIZER; /* room for a logger */

static void log_out(const char* p, size_t n) {
    fwrite(p, 1, n, stdout);
    if (logfile) {
        fwrite(p, 1, n, logfile);
    }
}
static void log_flush(void) {
    fflush(stdout);
    if (logfile) {
        fflush(logfile);
    }
}

/* the ring's contents out, in up to two pieces; the lock is held only to
 * read the positions */
static void log_drain_locked_take(unsigned* from, unsigned* to) {
    *from = log_tail;
    *to = log_head;
}
static void log_write_span(unsigned from, unsigned to) {
    while (from != to) {
        unsigned off = from & (LOG_RING - 1);
        unsigned n = to - from;
        if (n > LOG_RING - off) {
            n = LOG_RING - off;
        }
        log_out(log_ring + off, n);
        from += n;
    }
}

static void* log_main(void* arg) {
    (void)arg;
    pthread_mutex_lock(&log_mu);
    for (;;) {
        unsigned from, to;
        while (log_head == log_tail && !log_quit) {
            pthread_cond_wait(&log_cv, &log_mu);
        }
        if (log_head == log_tail && log_quit) {
            break;
        }
        log_drain_locked_take(&from, &to);
        pthread_mutex_unlock(&log_mu);
        log_write_span(from, to);
        log_flush();
        pthread_mutex_lock(&log_mu);
        log_tail = to;
        pthread_cond_broadcast(&log_room);
    }
    pthread_mutex_unlock(&log_mu);
    return NULL;
}

/* everything in the ring out now, on this thread (the writer stopped or
 * racing: a try-lock, so a fault inside port_logv cannot deadlock itself) */
static void log_drain_here(void) {
    if (!log_ring) {
        return;
    }
    if (pthread_mutex_trylock(&log_mu) == 0) {
        unsigned from = log_tail, to = log_head;
        log_write_span(from, to);
        log_tail = to;
        pthread_mutex_unlock(&log_mu);
    }
    log_flush();
}

void port_log_sync(void) {
    log_sync_now = 1;
    log_drain_here();
}

static void log_atexit(void) {
    if (log_async) {
        pthread_mutex_lock(&log_mu);
        log_quit = 1;
        pthread_cond_broadcast(&log_cv);
        pthread_mutex_unlock(&log_mu);
        pthread_join(log_thread, NULL);
        log_async = 0;
    }
    log_drain_here();
}

void port_log_async_start(void) {
    if (log_async || port_opt.synclog) {
        return;
    }
    log_ring = (char*)malloc(LOG_RING);
    if (!log_ring) {
        return;
    }
    if (pthread_create(&log_thread, NULL, log_main, NULL) != 0) {
        free(log_ring);
        log_ring = NULL;
        return;
    }
    log_async = 1;
    atexit(log_atexit);
}

void port_logv(const char* fmt, va_list ap) {
    char small[2048];
    char* line = small;
    int n;
    va_list copy;
    if (!log_async || log_sync_now) {
        va_copy(copy, ap);
        log_drain_here();
        vfprintf(stdout, fmt, ap);
        fflush(stdout);
        if (logfile) {
            vfprintf(logfile, fmt, copy);
            fflush(logfile);
        }
        va_end(copy);
        return;
    }
    va_copy(copy, ap);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n >= (int)sizeof(small)) {
        line = (char*)malloc((size_t)n + 1);
        if (line) {
            vsnprintf(line, (size_t)n + 1, fmt, copy);
        } else {
            line = small;
            n = (int)sizeof(small) - 1;
        }
    }
    va_end(copy);
    if (n > 0) {
        unsigned len = (unsigned)n, done = 0;
        pthread_mutex_lock(&log_mu);
        while (done < len) {
            unsigned room, k, off;
            while ((room = LOG_RING - (log_head - log_tail)) == 0) {
                pthread_cond_signal(&log_cv);
                pthread_cond_wait(&log_room, &log_mu);
            }
            k = len - done < room ? len - done : room;
            off = log_head & (LOG_RING - 1);
            if (k > LOG_RING - off) {
                k = LOG_RING - off;
            }
            memcpy(log_ring + off, line + done, k);
            log_head += k;
            done += k;
        }
        pthread_cond_signal(&log_cv);
        pthread_mutex_unlock(&log_mu);
    }
    if (line != small) {
        free(line);
    }
}

void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
}

void port_fatal(const char* fmt, ...) {
    va_list ap;
    char msg[900];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    port_log_sync();
    port_log("\n*** port: fatal: %s\n", msg);
    port_stub_report();
    /* M32: a player who double-clicked sees a silent exit otherwise; the same
     * text is on the log (the dialog needs a window server -- from an ssh
     * exec it fails harmlessly, config.c) */
    if (!port_opt.headless && !port_opt.machinecheck) {
        char text[1100];
        snprintf(text, sizeof(text),
                 "%s\n\nThe log has the details:\n%s", msg,
                 port_opt.log ? port_opt.log : "(the Terminal's output)");
        port_dialog_notice("Mario Party 4 could not start", text);
    }
    exit(1);
}

void port_log_open(const char* path) {
    if (path) {
        logfile = fopen(path, "w");
    }
}

/* ---- the SDK's own reporting -------------------------------------------- */

void OSReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
}

void OSVReport(const char* fmt, va_list ap) { port_logv(fmt, ap); }

void OSPanic(const char* file, int line, const char* fmt, ...) {
    va_list ap;
    port_log_sync();
    port_log("\n*** OSPanic in \"%s\" on line %d: ", file, line);
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
    port_log("\n");
    port_stub_report();
    exit(2);
}

/* `printf` reaches the game through the decomp's own stdio shim (only
 * process.c's "stack overlap error" uses it); the host libc supplies it. */

/* ---- the loud-stub table ------------------------------------------------- */

#define PORT_STUB_MAX 1024

static struct {
    const char* name;
    unsigned long calls;
} stubs[PORT_STUB_MAX];
static int stub_count;

static int is_gx(const char* name) { return name[0] == 'G' && name[1] == 'X'; }

void port_stub(const char* name) {
    int i;
    for (i = 0; i < stub_count; i++) {
        if (stubs[i].name == name || strcmp(stubs[i].name, name) == 0) {
            stubs[i].calls++;
            if (port_opt.stub_trace || (port_opt.gxlog && is_gx(name))) {
                port_log("stub> %s\n", name);
            }
            return;
        }
    }
    if (stub_count < PORT_STUB_MAX) {
        stubs[stub_count].name = name;
        stubs[stub_count].calls = 1;
        stub_count++;
    }
    if (port_opt.verbose || port_opt.stub_trace || (port_opt.gxlog && is_gx(name))) {
        port_log("stub> %s (first call)\n", name);
    }
}

void port_stub_report(void) {
    int i;
    unsigned long total = 0;
    port_log("\n---- SDK surface hit at boot: %d distinct stubs, in first-call order ----\n",
             stub_count);
    for (i = 0; i < stub_count; i++) {
        port_log("%4d  %-32s %8lu\n", i + 1, stubs[i].name, stubs[i].calls);
        total += stubs[i].calls;
    }
    port_log("---- %lu stub calls total ----\n", total);
}
