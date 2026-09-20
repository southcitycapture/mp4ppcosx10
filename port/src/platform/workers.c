/* The second core (M24, PLAN.md 39).
 *
 * The G4 is a dual 1 GHz (PowerMac3,5, hw.ncpu 2 -- found in M17 and not
 * used since), and until M24 the port ran the game, the GX interpreter, the
 * mixer and the texture decode on one of them; the snapshot writer (§33.5)
 * was the only thread.  Most G4s are not dual -- every iMac G4, eMac and
 * PowerBook, most towers -- so the second core is an advantage taken where
 * it exists and never a requirement: with one CPU, or `--threads 0`, no
 * worker is started, no job is ever built, and every code path is the one
 * that ran before M24.  `--threads 1` forces the workers on anywhere, for
 * measurement (the MacBook bench under Rosetta, the host build).
 *
 * The rules every worker keeps:
 *
 *   - no GL call from a worker: one context, one thread (§32.3's --mpgl
 *     lesson: Apple's multithreaded engine was twice as slow);
 *   - a job reads nothing of the game's after the retrace it was published
 *     at: what it needs is copied into the job on the game thread;
 *   - the game thread never blocks mid-frame.  It publishes at the retrace
 *     boundary and checks completion at the next one; a job the worker has
 *     not picked up by then is run by the game thread itself (which is the
 *     single-core path, and counted as `late`), and a job the worker is in
 *     the middle of is waited for (counted, timed);
 *   - the results are byte-identical in both modes, by construction and by
 *     the --mixtrace / frame-md5 discipline (PLAN.md 39).
 *
 * Two workers: the mixer's (a 2-3 ms job every retrace, wants to run at
 * once) and the texture decode's (a job of tens of ms on a cold scene, may
 * wait).  Both are plain pthreads; the mixer's runs at a higher timeshare
 * precedence so that on a machine with exactly one spare core it is not
 * queued behind a decode.
 */
#include "port.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define QMAX 64

struct PortWorker {
    const char* name;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv_job;  /* a job was queued */
    pthread_cond_t cv_done; /* a job finished */
    int up, quit;
    PortJob* q[QMAX];
    unsigned qhead, qtail; /* monotonic; index at the use site */
    /* stats */
    unsigned long jobs;         /* submitted */
    unsigned long jobs_worker;  /* finished by the worker before the join */
    unsigned long late_inline;  /* not picked up by the join: the game thread ran them */
    unsigned long late_wait;    /* running at the join: the game thread waited */
    double wait_s;              /* time the game thread spent waiting at joins */
    double busy_s;              /* time the worker spent in jobs */
    double inline_s;            /* time the game thread spent in late jobs */
    double worst_wait_s;
};

static PortWorker mixer_w, decode_w;
static int threads_on;
static int ncpu_seen;

int port_ncpu(void) {
    int n = 1;
#ifdef __APPLE__
    size_t len = sizeof(n);
    if (sysctlbyname("hw.ncpu", &n, &len, NULL, 0) != 0 || n < 1) {
        n = 1;
    }
#else
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    if (v > 0) {
        n = (int)v;
    }
#endif
    return n;
}

int port_threads_on(void) { return threads_on; }

static void* worker_main(void* arg) {
    PortWorker* w = (PortWorker*)arg;
    pthread_mutex_lock(&w->mu);
    for (;;) {
        PortJob* j;
        while (w->qhead == w->qtail && !w->quit) {
            pthread_cond_wait(&w->cv_job, &w->mu);
        }
        if (w->quit && w->qhead == w->qtail) {
            break;
        }
        j = w->q[w->qhead % QMAX];
        w->qhead++;
        if (j->state != PORT_JOB_QUEUED) {
            continue; /* the game thread took it at a join */
        }
        j->state = PORT_JOB_RUNNING;
        j->t_start = port_now_seconds();
        pthread_mutex_unlock(&w->mu);
        j->run(j);
        pthread_mutex_lock(&w->mu);
        j->t_end = port_now_seconds();
        w->busy_s += j->t_end - j->t_start;
        w->jobs_worker++;
        j->state = PORT_JOB_DONE;
        pthread_cond_broadcast(&w->cv_done);
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

static void worker_start(PortWorker* w, const char* name, int prio) {
    memset(w, 0, sizeof(*w));
    w->name = name;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv_job, NULL);
    pthread_cond_init(&w->cv_done, NULL);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        port_log("port> workers: pthread_create(%s) failed; running inline\n", name);
        return;
    }
    w->up = 1;
    /* A timeshare precedence, not a real-time policy: the mixer's thread
     * above the default, the decoder's below it, so that with one spare core
     * a 2 ms mix is not queued behind a 100 ms decode.  Failure is not an
     * error -- it only changes who waits. */
    if (prio) {
        struct sched_param sp;
        int policy = SCHED_OTHER;
        memset(&sp, 0, sizeof(sp));
        if (pthread_getschedparam(w->thread, &policy, &sp) == 0) {
            sp.sched_priority += prio;
            if (pthread_setschedparam(w->thread, policy, &sp) != 0) {
                port_log("port> workers: %s: priority %+d refused\n", name, prio);
            }
        }
    }
}

void port_workers_init(void) {
    int n = port_ncpu();
    ncpu_seen = n;
    if (port_opt.threads == 0) {
        threads_on = 0;
    } else if (port_opt.threads > 0) {
        threads_on = 1;
    } else {
        threads_on = n > 1;
    }
    port_log("port> cpu: %d %s (hw.ncpu); workers %s%s\n", n, n == 1 ? "core" : "cores",
             threads_on ? "ON" : "OFF",
             port_opt.threads == 0 ? " (--threads 0)"
             : port_opt.threads > 0 ? " (--threads 1)"
                                    : "");
    if (!threads_on) {
        return;
    }
    worker_start(&mixer_w, "mixer", +8);
    worker_start(&decode_w, "decode", -8);
    if (!mixer_w.up && !decode_w.up) {
        threads_on = 0;
    }
}

PortWorker* port_worker_mixer(void) { return &mixer_w; }
PortWorker* port_worker_decode(void) { return &decode_w; }

int port_worker_submit(PortWorker* w, PortJob* j) {
    if (!threads_on || !w->up) {
        return 0;
    }
    pthread_mutex_lock(&w->mu);
    if (w->qtail - w->qhead >= QMAX) {
        pthread_mutex_unlock(&w->mu);
        return 0; /* the queue is full: the caller runs it */
    }
    j->state = PORT_JOB_QUEUED;
    j->ran_inline = 0;
    j->t_queued = port_now_seconds();
    j->t_start = j->t_end = 0.0;
    w->q[w->qtail % QMAX] = j;
    w->qtail++;
    w->jobs++;
    pthread_cond_signal(&w->cv_job);
    pthread_mutex_unlock(&w->mu);
    return 1;
}

int port_worker_done(const PortJob* j) { return j->state == PORT_JOB_DONE || j->state == PORT_JOB_IDLE; }

void port_worker_join(PortWorker* w, PortJob* j) {
    double t0;
    if (!w->up || j->state == PORT_JOB_IDLE || j->state == PORT_JOB_DONE) {
        return;
    }
    pthread_mutex_lock(&w->mu);
    if (j->state == PORT_JOB_QUEUED) {
        /* Never picked up: the game thread does it.  This is the single-core
         * path and it is the same function. */
        j->state = PORT_JOB_RUNNING;
        j->ran_inline = 1;
        w->late_inline++;
        pthread_mutex_unlock(&w->mu);
        t0 = port_now_seconds();
        j->run(j);
        w->inline_s += port_now_seconds() - t0;
        pthread_mutex_lock(&w->mu);
        j->state = PORT_JOB_DONE;
        pthread_mutex_unlock(&w->mu);
        return;
    }
    if (j->state == PORT_JOB_RUNNING) {
        double d;
        t0 = port_now_seconds();
        while (j->state == PORT_JOB_RUNNING) {
            pthread_cond_wait(&w->cv_done, &w->mu);
        }
        d = port_now_seconds() - t0;
        w->late_wait++;
        w->wait_s += d;
        if (d > w->worst_wait_s) {
            w->worst_wait_s = d;
        }
    }
    pthread_mutex_unlock(&w->mu);
}

void port_worker_stats(PortWorker* w, unsigned long* jobs, unsigned long* late_inline,
                       unsigned long* late_wait, double* wait_ms, double* busy_ms) {
    *jobs = w->jobs;
    *late_inline = w->late_inline;
    *late_wait = w->late_wait;
    *wait_ms = w->wait_s * 1000.0;
    *busy_ms = w->busy_s * 1000.0;
}

static void worker_stop(PortWorker* w) {
    if (!w->up) {
        return;
    }
    pthread_mutex_lock(&w->mu);
    w->quit = 1;
    pthread_cond_broadcast(&w->cv_job);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    w->up = 0;
}

void port_workers_shutdown(void) {
    worker_stop(&mixer_w);
    worker_stop(&decode_w);
}

void port_gx_predecode_join(void);

/* The top of VIWaitForRetrace: the one point in the frame where every job
 * published at the previous retrace is finished before anything reads what
 * it wrote -- the snapshot (which serialises the mixer's state), the
 * texture cache (which takes the staged decodes), the next tick. */
void port_workers_retrace_join(void) {
    if (!threads_on) {
        return;
    }
    port_audio_join();
    port_gx_predecode_join();
}

static void worker_report(PortWorker* w) {
    if (!w->up && !w->jobs) {
        return;
    }
    port_log("port> worker %-7s %lu jobs: %lu finished by the worker, %lu late (run inline "
             "by the game thread, %.0f ms), %lu waited for (%.0f ms, worst %.1f ms); "
             "%.0f ms of work on the second core\n",
             w->name, w->jobs, w->jobs_worker, w->late_inline, w->inline_s * 1000.0,
             w->late_wait, w->wait_s * 1000.0, w->worst_wait_s * 1000.0, w->busy_s * 1000.0);
}

void port_workers_report(void) {
    port_log("\nport> cpu %d: workers %s\n", ncpu_seen, threads_on ? "on" : "off");
    if (!threads_on) {
        return;
    }
    worker_report(&mixer_w);
    worker_report(&decode_w);
}
