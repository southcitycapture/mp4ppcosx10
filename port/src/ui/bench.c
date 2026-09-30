/* M50 (PLAN.md 65): Benchmark Mode -- the game measures this Mac and picks its
 * settings.
 *
 * From the overlay menu (overlay.c) the game leaves through its own quit path
 * (the card flushed, port_shutdown) and starts itself again as the driver
 * `isle --benchmark`, which opens no window: it runs the fixed set of scenes
 * below one after another as child runs of the same program -- the
 * scoreboard's own teleports (tools/fps_board.sh: the --play walk, --ffto,
 * --minigame, lockstep-safe from the start of the game), at real time, each
 * in its own window with a one-line banner -- and reads each child's log the
 * way fps_board.sh does: the median of the presented frames a second over the
 * scene's `--status` lines at full game speed.  Lite off first; Lite on only
 * where the screen needs it.  Then it picks, in plain rules:
 *
 *   Butterfly Blitz (m441, the heaviest screen) holds 29.5 with Lite off and
 *     the board holds too  -> faster than the reference: Lite off;
 *   it holds with the reference's Lite set  -> the reference: Lite at auto
 *     (on the reference class) or the reference's set (anywhere else);
 *   neither  -> slower than the reference: Lite on, the reference's set and
 *     the user's extras (m401's fish and bubbles, the Bowser arena's pillars
 *     and fruit stands);
 *   Makin' Waves (m417) holds 29.5 with the water at the reference's level ->
 *     water auto (cheap on a machine below the reference), else off;
 *   the opening movie at full speed and 27+ frames a second -> movies on;
 *   the resident set: the machine check's rule by the RAM (machine.c).
 *
 * The choice goes into the config -- lite, liteopts, water, movies, resident:
 * the keys PowerPCube writes too -- and two texts: the result the overlay
 * shows when the game comes back (`--benchresult`), and a shareable report,
 * ~/Desktop/MarioParty4-benchmark-DATE.txt (the machine, the port's version,
 * each scene's frame rate, the settings; nothing personal).  The children
 * read neither the config nor the player's memory card (--noconfig,
 * --freshcard: the scratch card).
 *
 * Scripted runs never come here: nothing below runs without --benchmark,
 * and a child's only difference from a scoreboard run is its banner. */
#include "port.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

int port_machine_report(char* out, size_t n);
int port_machine_resident_rule(void);
int port_machine_class(void);
void port_request_reset(void);
unsigned gl13_frame_number(void);

#define BENCH_BAR 29.5  /* the scoreboard's bar */
#define BENCH_NEAR 29.0 /* one run's reading of "at the bar" (below) */

typedef struct {
    const char* key;    /* the scene's short name (logs) */
    const char* name;   /* in plain words */
    const char* screen; /* the status lines' screen, or NULL for any */
    int from;           /* the first frame counted: absolute, or after the
                         * minigame's entry when `mg` (its log line) */
    int mg;
    const char* args;   /* the teleport (the scoreboard's) */
} BenchScene;

/* The windows are the A/B's (tools/m44_ab.py): a minigame from its entry
 * +300 (after the load and the camera's sweep in), the board from 8,400
 * (after the fast-forward's hand-back and the turn's start); the lines
 * before them -- the load, --ffto's first real-time frames -- are not the
 * scene's speed (the first final build read m441 29.2 with them counted). */
#define WALK "--com4 --play board-start-com4.play --nomovies "
#define MG(n) WALK "--minigame m" #n " --turns 1 --ffto 14000 --frames 20000 --mgend 1300 "
static const BenchScene scenes[] = {
    {"movie", "The opening movie", NULL, 300, 0, "--frames 1500"},
    {"board", "Toad's Midway Madness (board)", "w01dll", 8400, 0, WALK "--board 1 --ffto 7808 --frames 9800"},
    {"m441off", "Butterfly Blitz, Lite off", "m441dll", 300, 1, MG(441) "--nolite"},
    {"m441lite", "Butterfly Blitz, Lite on", "m441dll", 300, 1, MG(441) "--lite --liteopts ref"},
    {"m417", "Makin' Waves (the water)", "m417dll", 300, 1, MG(417) "--water cheap"},
};
#define N_SCENES ((int)(sizeof(scenes) / sizeof(scenes[0])))
#define COMMON "--rtc dolphin --freshcard --noconfig --realtime --perf --status --ovllog --nomenu "

typedef struct {
    int ran, lines, exitcode;
    double median, p10, speed;
    double wall;
    int runs;           /* 1, or 3 under the three-run rule (below) */
    double each[3];     /* each run's median */
} BenchResult;

/* ---- the paths -------------------------------------------------------------- */
const char* port_bench_result_path(void) {
    static char p[1024];
    if (!p[0]) {
        snprintf(p, sizeof(p), "%s/benchmark-result.txt", port_app_support_dir());
    }
    return p;
}

static int self_path(char* out, size_t n) {
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)n;
    char tmp[2048];
    if (_NSGetExecutablePath(tmp, &sz) != 0) {
        return 0;
    }
    if (!realpath(tmp, out)) {
        snprintf(out, n, "%s", tmp);
    }
    return 1;
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) {
        return 0;
    }
    out[k] = 0;
    return 1;
#endif
}

/* Mac OS X 10.4/10.5 refuse execve() in a process with more than one thread
 * (ENOTSUP) -- the game has its render thread, its workers and the log
 * writer, the driver the log writer -- so a hand-over is a fork (whose child
 * has one thread) that execs, and the parent leaves. */
static int spawn(const char* exe, char* const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe, argv);
        _exit(127);
    }
    return pid > 0;
}

/* ---- leaving the game for the driver (the overlay's Start) ------------------ */
static int bench_pending;
static unsigned bench_asked_at;

static void bench_exec_driver(void) {
    char exe[2048];
    char* argv[4];
    int k = 0;
    if (!bench_pending || !self_path(exe, sizeof(exe))) {
        return;
    }
    bench_pending = 0;
    argv[k++] = exe;
    argv[k++] = (char*)"--benchmark";
    if (port_opt.force) {
        argv[k++] = (char*)"--force";
    }
    argv[k] = NULL;
    spawn(exe, argv); /* the game's own exit goes on */
}

int port_bench_start(void) {
    if (bench_pending) {
        return 1;
    }
    bench_pending = 1;
    bench_asked_at = gl13_frame_number();
    atexit(bench_exec_driver);
    port_log("port> benchmark (M50): the game closes through its quit path; Benchmark Mode starts\n");
    port_request_reset(); /* the fade, the card, port_shutdown -> exit -> the driver */
    return 1;
}

/* the quit path should take a few frames; ten seconds is the gl13 rule's */
void port_bench_tick(void) {
    if (bench_pending && gl13_frame_number() > bench_asked_at + 300) {
        port_log("port> benchmark (M50): the reset path did not finish; leaving directly\n");
        port_shutdown(0);
    }
}

/* ---- a child's banner ------------------------------------------------------- */
int port_bench_child_banner(char* out, size_t n) {
    const char* s = port_opt.benchchild;
    const char* c;
    if (!s) {
        return 0;
    }
    c = strchr(s, ':');
    snprintf(out, n, "Benchmark Mode - scene %.*s: %s - please wait", c ? (int)(c - s) : (int)strlen(s), s,
             c ? c + 1 : "");
    return 1;
}

/* ---- the driver --------------------------------------------------------------- */
static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec * 1e-6;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* fps_board.sh's stats: the screen's status lines at real time (speed 99..110%;
 * over 110 is a catch-up), their median and tenth percentile */
static void read_log(const char* path, const BenchScene* sc, BenchResult* r) {
    FILE* f = fopen(path, "r");
    char line[1024];
    static double v[4096], sp[4096];
    int n = 0;
    unsigned from = sc->mg ? 0xFFFFFFFFu : (unsigned)sc->from;
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f) && n < 4096) {
        unsigned fr, entry;
        char scr[64];
        const char *p, *q;
        double fps, speed;
        if (sc->mg && (p = strstr(line, "entered minigame ")) != NULL && (q = strstr(p, ") at frame ")) != NULL &&
            sscanf(q, ") at frame %u", &entry) == 1) {
            from = entry + (unsigned)sc->from;
            continue;
        }
        if (sscanf(line, "port> status f%u %63s", &fr, scr) != 2) {
            continue;
        }
        if ((sc->screen && strcmp(scr, sc->screen) != 0) || fr < from) {
            continue;
        }
        p = strstr(line, " speed ");
        q = strstr(line, " fps presented");
        if (!p || !q) {
            continue;
        }
        speed = atof(p + 7);
        while (q > line && q[-1] != ' ') {
            q--;
        }
        fps = atof(q);
        if (speed > 110.0) {
            continue;
        }
        sp[n] = speed;
        v[n++] = fps;
    }
    fclose(f);
    r->lines = n;
    if (n < 3) {
        return;
    }
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    qsort(sp, (size_t)n, sizeof(double), cmp_d);
    r->median = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    {
        double k = (n - 1) * 0.10;
        int lo = (int)k, hi = lo + 1 < n ? lo + 1 : n - 1;
        r->p10 = v[lo] + (v[hi] - v[lo]) * (k - lo);
    }
    r->speed = (n % 2) ? sp[n / 2] : (sp[n / 2 - 1] + sp[n / 2]) / 2.0;
}

static int bench_rep; /* the three-run rule's repeat, 0 1 2 (the log's name) */
static int run_scene(const char* exe, int k, int nsc, const BenchScene* sc, const char* dir, BenchResult* r) {
    char logp[1200], banner[160], args[1024];
    char* argv[64];
    int argc = 0, status = 0, ceiling;
    char* tok;
    pid_t pid;
    double t0;
    if (bench_rep) {
        snprintf(logp, sizeof(logp), "%s/bench-%d-%s-%d.log", dir, k + 1, sc->key, bench_rep + 1);
    } else {
        snprintf(logp, sizeof(logp), "%s/bench-%d-%s.log", dir, k + 1, sc->key);
    }
    snprintf(banner, sizeof(banner), "%d of %d:%s", k + 1, nsc, sc->name);
    snprintf(args, sizeof(args), COMMON "%s", sc->args);
    argv[argc++] = (char*)exe;
    for (tok = strtok(args, " "); tok && argc < 50; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }
    argv[argc++] = (char*)"--image";
    argv[argc++] = (char*)port_opt.image;
    argv[argc++] = (char*)"--log";
    argv[argc++] = logp;
    argv[argc++] = (char*)"--benchchild";
    argv[argc++] = banner;
    if (port_opt.force) {
        argv[argc++] = (char*)"--force";
    }
    argv[argc] = NULL;
    port_log("port> benchmark (M50): scene %d of %d, %s:", k + 1, nsc, sc->name);
    {
        int i;
        for (i = 1; i < argc; i++) {
            port_log(" %s", argv[i]);
        }
    }
    port_log("\n");
    unlink(logp);
    t0 = now_s();
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execv(exe, argv);
        _exit(127);
    }
    if (pid < 0) {
        port_log("port> benchmark (M50): fork failed (%s)\n", strerror(errno));
        return 0;
    }
    /* a G4 scene takes 40-110 s; Rosetta twice that: the ceiling is generous */
    ceiling = 480;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (w < 0 && errno != EINTR) {
            break;
        }
        if (now_s() - t0 > ceiling) {
            port_log("port> benchmark (M50): scene %d over its %d s ceiling -- stopped\n", k + 1, ceiling);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(250000);
    }
    r->ran = 1;
    r->wall = now_s() - t0;
    r->exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    read_log(logp, sc, r);
    port_log("port> benchmark (M50): scene %d: %d lines, median %.1f fps (p10 %.1f), speed %.0f%%, exit %d, "
             "%.0f s\n",
             k + 1, r->lines, r->median, r->p10, r->speed, r->exitcode, r->wall);
    return r->lines >= 3;
}

/* The scoreboard's three-run rule in miniature (tools/fps_board.sh FB_THREE):
 * a scene whose run lands at the edge -- 28.5 up to the bar -- runs twice
 * more and is judged by the median of the three runs' medians.  A scene
 * clearly over or under the bar runs once. */
static void run_scene_judged(const char* exe, int k, int nsc, const BenchScene* sc, const char* dir,
                             BenchResult* r) {
    BenchResult a[3];
    int i, n = 1;
    memset(a, 0, sizeof(a));
    run_scene(exe, k, nsc, sc, dir, &a[0]);
    if (a[0].lines >= 3 && a[0].median >= 28.5 && a[0].median < BENCH_BAR) {
        port_log("port> benchmark (M50): scene %d at the edge (%.1f): two more runs, the median of three\n",
                 k + 1, a[0].median);
        bench_rep = 1;
        run_scene(exe, k, nsc, sc, dir, &a[1]);
        bench_rep = 2;
        run_scene(exe, k, nsc, sc, dir, &a[2]);
        bench_rep = 0;
        n = 3;
    }
    *r = a[0];
    r->runs = n;
    for (i = 0; i < n; i++) {
        r->each[i] = a[i].median;
    }
    if (n == 3) {
        /* the run whose median is the middle one stands for the scene */
        int m = 0;
        for (i = 0; i < 3; i++) {
            int lo = 0, hi = 0, j;
            for (j = 0; j < 3; j++) {
                lo += a[j].median < a[i].median;
                hi += a[j].median > a[i].median;
            }
            if (lo <= 1 && hi <= 1) {
                m = i;
            }
        }
        *r = a[m];
        r->runs = 3;
        for (i = 0; i < 3; i++) {
            r->each[i] = a[i].median;
        }
        r->wall = a[0].wall + a[1].wall + a[2].wall;
    }
}

static const char* lite_names_ref =
    "Butterfly Blitz (no butterfly or net shadows and fewer fence flowers), and the lighter character models in Manta Rings, Fruits of Doom, Darts of Doom, "
    "Order Up, Reversal of Fortune and Panel Panic";
static const char* lite_names_extra =
    "plus fewer fish and no drifting bubbles in Manta Rings, and no pillar or fruit-stand shadows in "
    "the Bowser arena (Fruits of Doom, Darts of Doom)";

static void fmt_fps(char* out, size_t n, const BenchResult* r) {
    if (!r->ran) {
        snprintf(out, n, "not needed");
    } else if (r->lines < 3) {
        snprintf(out, n, "no measurement (the scene did not run: exit %d)", r->exitcode);
    } else if (r->runs == 3) {
        snprintf(out, n, "%.1f fps (three runs: %.1f / %.1f / %.1f), game speed %.0f%%", r->median, r->each[0],
                 r->each[1], r->each[2], r->speed);
    } else {
        snprintf(out, n, "%.1f fps (the slowest tenth %.1f), game speed %.0f%%", r->median, r->p10, r->speed);
    }
}

void port_bench_driver(void) {
    char exe[2048], dir[1100], date[32], stamp[64], repp[1400], mline[2048];
    BenchResult res[N_SCENES];
    int k, cls = port_machine_class();
    int fast, ref, slow, water_ok, movies_ok, resident;
    const char* verdict;
    const char *lite, *liteopts, *water;
    time_t t = time(NULL);
    struct tm tmv;
    FILE* f;
    double t0 = now_s();
    memset(res, 0, sizeof(res));
    localtime_r(&t, &tmv);
    strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tmv);
    if (!self_path(exe, sizeof(exe)) || !port_opt.image) {
        port_log("port> benchmark (M50): no program path or no disc image; nothing measured\n");
        port_log_sync();
        exit(1);
    }
    snprintf(dir, sizeof(dir), "%s/benchmark", port_app_support_dir());
    mkdir(dir, 0755);
    {
        /* the last run's scene logs go (a player's folder keeps one run) */
        int i, j;
        char p[1300];
        for (i = 0; i < N_SCENES; i++) {
            for (j = 1; j <= 3; j++) {
                if (j == 1) {
                    snprintf(p, sizeof(p), "%s/bench-%d-%s.log", dir, i + 1, scenes[i].key);
                } else {
                    snprintf(p, sizeof(p), "%s/bench-%d-%s-%d.log", dir, i + 1, scenes[i].key, j);
                }
                unlink(p);
            }
        }
    }
    port_log("port> benchmark (M50): Benchmark Mode, %d scenes, machine class %d, logs in %s\n", N_SCENES, cls,
             dir);
    {
        /* the lab's: MP4_BENCH_ONLY="1 5" runs only those scenes (the flow's
         * test); never set on a player's Mac */
        const char* only = getenv("MP4_BENCH_ONLY");
#define WANT(k) (!only || !*only || strchr(only, '1' + (k)))
        if (WANT(0)) run_scene(exe, 0, N_SCENES, &scenes[0], dir, &res[0]);
        if (WANT(1)) run_scene_judged(exe, 1, N_SCENES, &scenes[1], dir, &res[1]);
        if (WANT(2)) run_scene_judged(exe, 2, N_SCENES, &scenes[2], dir, &res[2]);
        fast = res[2].lines >= 3 && res[2].median >= BENCH_BAR && res[1].median >= BENCH_BAR;
        /* (BENCH_BAR for "faster": a wrong "faster" turns Lite off) */
        if (!fast && WANT(3)) {
            run_scene_judged(exe, 3, N_SCENES, &scenes[3], dir, &res[3]); /* Lite as needed */
        }
        if (WANT(4)) run_scene_judged(exe, 4, N_SCENES, &scenes[4], dir, &res[4]);
#undef WANT
    }
    /* "as fast as the reference" and the water keep BENCH_NEAR: the scoreboard
     * judges 29.5 by the median of three runs; one benchmark run of a screen
     * at the reference's edge spreads about half a frame either way (m441
     * with the reference's set: 29.2-30.0 in single runs), and a Mac truly
     * below the reference is frames under it, not tenths */
    ref = !fast && res[3].lines >= 3 && res[3].median >= BENCH_NEAR && res[1].median >= BENCH_NEAR;
    slow = !fast && !ref;
    water_ok = res[4].lines >= 3 && res[4].median >= BENCH_NEAR;
    movies_ok = res[0].lines >= 3 && res[0].median >= 27.0 && res[0].speed >= 98.0;
    resident = port_machine_resident_rule();
    if (fast) {
        verdict = "faster than the reference machine (a dual 1 GHz Power Mac G4 with a Radeon 9000)";
        lite = "off";
        liteopts = "";
    } else if (ref) {
        verdict = "as fast as the reference machine (a dual 1 GHz Power Mac G4 with a Radeon 9000)";
        lite = cls == 1 ? "auto" : "on";
        liteopts = cls == 1 ? "" : "ref";
    } else {
        verdict = "slower than the reference machine (a dual 1 GHz Power Mac G4 with a Radeon 9000)";
        lite = "on";
        liteopts = "ref,extras";
    }
    water = water_ok ? (cls >= 1 ? "auto" : "cheap") : "off";
    /* the config: the keys the game and PowerPCube read */
    port_config_set("lite", lite);
    port_config_set("liteopts", liteopts);
    port_config_set("water", water);
    port_config_set("movies", movies_ok ? "1" : "0");
    {
        char rs[16];
        snprintf(rs, sizeof(rs), "%d", resident);
        port_config_set("resident", rs);
    }
    {
        char b[96];
        snprintf(b, sizeof(b), "%s %s", stamp, fast ? "faster" : ref ? "reference" : "slower");
        port_config_set("benchmark", b);
    }
    port_config_set("benchoffer", "done");
    port_config_save();
    port_log("port> benchmark (M50): %s -> lite %s%s%s, water %s, movies %d, resident %d (config written)\n",
             fast ? "faster" : ref ? "the reference" : "slower", lite, *liteopts ? " liteopts " : "", liteopts,
             water, movies_ok, resident);

    /* the result, for the overlay: short lines; "+ " good, "! " a warning */
    f = fopen(port_bench_result_path(), "w");
    if (f) {
        fprintf(f, "%sThis Mac is %s\n", slow ? "! " : "+ ",
                fast ? "faster than the reference machine" : ref ? "as fast as the reference machine"
                                                               : "slower than the reference machine");
        fprintf(f, "%s(a dual 1 GHz Power Mac G4 with a Radeon 9000).\n", slow ? "! " : "+ ");
        fprintf(f, " \n");
        for (k = 0; k < N_SCENES; k++) {
            char a[32];
            if (!res[k].ran) {
                continue;
            }
            if (res[k].lines >= 3) {
                snprintf(a, sizeof(a), "%4.1f fps", res[k].median);
            } else {
                snprintf(a, sizeof(a), "no measurement");
            }
            fprintf(f, "  %-32s %s\n", scenes[k].name, a);
        }
        fprintf(f, " \n");
        fprintf(f, "Chosen and saved:\n");
        fprintf(f, "  Lite mode: %s\n", fast ? "off (every screen as on the console)"
                                     : ref ? "on for the heaviest screens (the reference set)"
                                           : "on, the reference set and the extras");
        fprintf(f, "  Water: %s - movies: %s - memory for loading: %d MB\n",
                !strcmp(water, "off") ? "flat" : !strcmp(water, "cheap") ? "ripples (cheap)" : "ripples (auto)",
                movies_ok ? "on" : "off", resident);
        fprintf(f, " \n");
        fprintf(f, "The report is on your Desktop:\n");
        fprintf(f, "  MarioParty4-benchmark-%s.txt\n", date);
        fclose(f);
    }

    /* the shareable report */
    {
        const char* home = getenv("HOME");
        char desk[1100];
        snprintf(desk, sizeof(desk), "%s/Desktop", home && *home ? home : ".");
        mkdir(desk, 0755);
        snprintf(repp, sizeof(repp), "%s/MarioParty4-benchmark-%s.txt", desk, date);
    }
    port_machine_report(mline, sizeof(mline));
    f = fopen(repp, "w");
    if (f) {
        char a[160];
        fprintf(f, "Mario Party 4 PowerPC Edition %s -- Benchmark Mode report\n", PORT_VERSION_STRING);
        fprintf(f, "Date:      %s (%.0f s)\n", stamp, now_s() - t0);
        fprintf(f, "Game:      %s (milestone %s)\n", PORT_VERSION_STRING, PORT_MILESTONE);
        fprintf(f, "%s", mline);
        fprintf(f, "\nScenes (the median of the frames presented each second at real time, from the\n"
                   "scene's start; 30 is the console's rate):\n");
        for (k = 0; k < N_SCENES; k++) {
            fmt_fps(a, sizeof(a), &res[k]);
            fprintf(f, "  %d. %-34s %s\n", k + 1, scenes[k].name, a);
        }
        fprintf(f, "\nVerdict:   %s.\n", verdict);
        fprintf(f, "\nSettings chosen (written to the game's config):\n");
        fprintf(f, "  lite     = %s%s%s\n", lite, *liteopts ? ", liteopts = " : "", liteopts);
        if (!fast) {
            /* wrapped at 76 columns, indented under the value */
            char w[1200];
            const char* p = w;
            snprintf(w, sizeof(w), "Lite changes only what is drawn on the heaviest screens: %s%s%s.", lite_names_ref,
                     slow ? ", " : "", slow ? lite_names_extra : "");
            while (*p) {
                size_t n = strlen(p), cut = n;
                if (n > 63) {
                    cut = 63;
                    while (cut > 10 && p[cut] != ' ') {
                        cut--;
                    }
                }
                fprintf(f, "             %.*s\n", (int)cut, p);
                p += cut;
                while (*p == ' ') {
                    p++;
                }
            }
        }
        fprintf(f, "  water    = %s\n", water);
        fprintf(f, "  movies   = %d\n", movies_ok);
        fprintf(f, "  resident = %d (MB of game files kept in memory)\n", resident);
        fprintf(f, "\nChange them any time: run Benchmark Mode again (F1 or M in the game), or edit\n"
                   "~/Library/Application Support/MarioParty4/config (the Read Me lists the keys).\n");
        fclose(f);
        port_log("port> benchmark (M50): the report: %s\n", repp);
    }

    /* back to the game, the result on screen */
    {
        char* argv[32];
        char** fargv = argv;
        int n = 0;
        port_log("port> benchmark (M50): done in %.0f s; the game starts again\n", now_s() - t0);
        argv[n++] = exe;
        argv[n++] = (char*)"--benchresult";
        argv[n++] = (char*)port_bench_result_path();
        if (port_opt.force) {
            argv[n++] = (char*)"--force";
        }
        {
            /* the lab's: flags for the game that comes back (a picture of the
             * result, a --frames to end it) -- never set on a player's Mac */
            const char* fin = getenv("MP4_BENCH_FINAL");
            static char buf[512];
            char* tok;
            if (fin && *fin) {
                snprintf(buf, sizeof(buf), "%s", fin);
                for (tok = strtok(buf, " "); tok && n < 30; tok = strtok(NULL, " ")) {
                    fargv[n++] = tok;
                }
            }
        }
        argv[n] = NULL;
        {
            int i;
            port_log("port> benchmark (M50): exec");
            for (i = 0; i < n; i++) {
                port_log(" %s", argv[i]);
            }
            port_log("\n");
        }
        if (!spawn(exe, argv)) {
            port_log("port> benchmark (M50): could not start the game again (%s); open it by hand\n",
                     strerror(errno));
        }
        port_log_sync();
        exit(0);
    }
}
