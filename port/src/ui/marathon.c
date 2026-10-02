/* M51 (PLAN.md 66.1): Developer Mode's minigame marathon -- the playtest rig.
 *
 * From the overlay menu (overlay.c: Developer Mode -> Minigame marathon) the
 * player picks the minigames (all, a range, a kind, or the list in the plan
 * file), how many people play (1-4, the rest COM) and who plays whom; the
 * game leaves through its quit path and starts itself again as the driver
 * `isle --marathon PLAN`, which opens no window: it plays the minigames one
 * after another as child runs of the same program, each the scoreboard's own
 * teleport (the --play walk with four COMs to the board, the roulette parked
 * on the minigame, --ffto to the instruction card) with the people at the
 * controllers from the instruction card on (--humans) and the child leaving
 * when the minigame's results are over (--mgexit).  Each child says how it
 * went in one log line ("marathon: result m441 coins +10 +0 +0 +0 frames N"),
 * which goes into the plan file at once -- so a marathon stopped half way
 * (the overlay's "Stop the marathon", or the window closed) resumes from the
 * next minigame.  At the end, or at a stop, the game comes back with the
 * summary (--marathonresult), which is also kept as a text file in
 * ~/Documents/MarioParty4 Recordings.
 *
 * The plan file (Application Support/MarioParty4/marathon.txt), `key = value`:
 *
 *   list   = 401,402,...       the minigames, in order (mgInfoTbl numbers)
 *   humans = 1..4              people at the controllers (pads 1..N)
 *   cast   = mario,luigi,...   who plays (P1..P4)
 *   record = 0|1               a session recording of each minigame
 *   next   = K                 the next to play (0-based)
 *   r401   = +10 +0 +0 +0 3123 the results so far
 *
 * The children read the player's config (Lite, the water, fullscreen, the
 * controls) but never the player's memory card (--freshcard: the scratch). */
#include "port.h"

#include "game/objsub.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

void port_request_reset(void);
unsigned gl13_frame_number(void);
const char* port_session_default_path(const char* tag);

/* ---- the names ------------------------------------------------------------- */
static const char* const MG_NAMES[63] = {
    "Manta Rings",          "Slime Time",          "Booksquirm",         "Trace Race",
    "Mario Medley",         "Avalanche!",          "Domination",         "Paratrooper Plunge",
    "Toad's Quick Draw",    "Three Throw",         "Photo Finish",       "Mr. Blizzard's Brigade",
    "Bob-omb Breakers",     "Long Claw of the Law", "Stamp Out!",        "Candlelight Flight",
    "Makin' Waves",         "Hide and Go BOOM!",   "Tree Stomp",         "Fish n' Drips",
    "Hop or Pop",           "Money Belts",         "GOOOOOOOAL!!",       "Blame it on the Crane",
    "The Great Deflate",    "Revers-a-Bomb",       "Right Oar Left?",    "Cliffhangers",
    "Team Treasure Trek",   "Pair-a-sailing",      "Order Up",           "Dungeon Duos",
    "Beach Volley Folly",   "Cheep Cheep Sweep",   "Darts of Doom",      "Fruits of Doom",
    "Balloon of Doom",      "Chain Chomp Fever",   "Paths of Peril",     "Bowser's Bigger Blast",
    "Butterfly Blitz",      "Barrel Baron",        "Mario Speedwagons",  "Reversal of Fortune",
    "Bowser Bop",           "Mystic Match 'Em",    "Archaeologuess",     "Goomba's Chip Flip",
    "Kareening Koopa",      "The Final Battle!",   "Jigsaw Jitters",     "The Final Battle! (2)",
    "Challenge Booksquirm", "The Final Battle! (3)", "Rumble Fishing",   "Take a Breather",
    "Bowser Wrestling",     "Panels of Doom",      "Mushroom Medic",     "Doors of Doom",
    "Bob-omb X-ing",        "Goomba Stomp",        "Panel Panic",
};

const char* port_mg_name(int num) {
    return num >= 401 && num <= 463 ? MG_NAMES[num - 401] : "?";
}

/* the minigame's kind, from mgInfoTbl (objsub.c) */
const char* port_mg_kind(int num) {
    static const char* const kinds[] = {"4-player", "1-vs-3", "2-vs-2", "Bowser", "Battle",
                                        "Item",     "Story",  "Extra",  "Story"};
    int idx = num - 401;
    if (idx < 0 || idx > 62 || mgInfoTbl[idx].ovl == 0xFFFF) {
        return "?";
    }
    return mgInfoTbl[idx].type < 9 ? kinds[mgInfoTbl[idx].type] : "?";
}

int port_mg_exists(int num) {
    int idx = num - 401;
    return idx >= 0 && idx <= 62 && mgInfoTbl[idx].ovl != 0xFFFF;
}

int port_mg_type(int num) {
    int idx = num - 401;
    return idx >= 0 && idx <= 62 ? mgInfoTbl[idx].type : -1;
}

/* ---- the plan --------------------------------------------------------------- */
const char* port_marathon_path(void) {
    static char p[1024];
    if (!p[0]) {
        snprintf(p, sizeof(p), "%s/marathon.txt", port_app_support_dir());
    }
    return p;
}

typedef struct {
    int list[64], n;
    int humans;
    char cast[96];
    int record;
    int next;
    char result[64][64]; /* "+10 +0 +0 +0 3123" by list position */
    char started[40];
    char childflags[256]; /* the lab's: more flags for every child (e.g. --keepplay --play SCRIPT) */
} Plan;

static void plan_clear(Plan* p) {
    memset(p, 0, sizeof(*p));
    p->humans = 1;
    snprintf(p->cast, sizeof(p->cast), "mario,luigi,peach,yoshi");
}

int port_marathon_load(const char* path, void* plan_out) {
    Plan* p = (Plan*)plan_out;
    FILE* f = fopen(path, "r");
    char line[512];
    plan_clear(p);
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char key[32], val[400];
        int i;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || sscanf(line, "%31s = %399[^\n]", key, val) != 2) {
            continue;
        }
        if (!strcmp(key, "list")) {
            char* s = strtok(val, ", ");
            p->n = 0;
            while (s && p->n < 64) {
                int num = atoi(s[0] == 'm' ? s + 1 : s);
                if (port_mg_exists(num)) {
                    p->list[p->n++] = num;
                }
                s = strtok(NULL, ", ");
            }
        } else if (!strcmp(key, "humans")) {
            p->humans = atoi(val);
        } else if (!strcmp(key, "cast")) {
            snprintf(p->cast, sizeof(p->cast), "%s", val);
        } else if (!strcmp(key, "record")) {
            p->record = atoi(val);
        } else if (!strcmp(key, "next")) {
            p->next = atoi(val);
        } else if (!strcmp(key, "childflags")) {
            snprintf(p->childflags, sizeof(p->childflags), "%s", val);
        } else if (!strcmp(key, "started")) {
            snprintf(p->started, sizeof(p->started), "%s", val);
        } else if (key[0] == 'r' && key[1] >= '0' && key[1] <= '9') {
            int num = atoi(key + 1);
            for (i = 0; i < p->n; i++) {
                if (p->list[i] == num) {
                    snprintf(p->result[i], sizeof(p->result[i]), "%s", val);
                }
            }
        }
    }
    fclose(f);
    if (p->humans < 1 || p->humans > 4) {
        p->humans = 1;
    }
    return p->n > 0;
}

int port_marathon_save(const char* path, const void* plan_in) {
    const Plan* p = (const Plan*)plan_in;
    char tmp[1100];
    FILE* f;
    int i;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        return 0;
    }
    fprintf(f, "# Mario Party 4 PowerPC Edition -- a minigame marathon (Developer Mode, M51)\n");
    fprintf(f, "# list: the minigames in order; next: the next to play; rNNN: the results so far\n");
    fprintf(f, "list = ");
    for (i = 0; i < p->n; i++) {
        fprintf(f, "%s%d", i ? "," : "", p->list[i]);
    }
    fprintf(f, "\nhumans = %d\ncast = %s\nrecord = %d\nnext = %d\n", p->humans, p->cast, p->record, p->next);
    if (p->started[0]) {
        fprintf(f, "started = %s\n", p->started);
    }
    if (p->childflags[0]) {
        fprintf(f, "childflags = %s\n", p->childflags);
    }
    for (i = 0; i < p->n; i++) {
        if (p->result[i][0]) {
            fprintf(f, "r%d = %s\n", p->list[i], p->result[i]);
        }
    }
    fclose(f);
    return rename(tmp, path) == 0;
}

size_t port_marathon_plan_size(void) { return sizeof(Plan); }

/* "12 of 60 done" for the menu, or 0 when there is nothing to resume */
int port_marathon_progress(int* done, int* total) {
    Plan p;
    if (!port_marathon_load(port_marathon_path(), &p) || p.next >= p.n) {
        return 0;
    }
    *done = p.next;
    *total = p.n;
    return 1;
}

/* ---- leaving the game for the driver ------------------------------------------ */
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

/* Leopard refuses execve with threads alive: fork, exec, the parent leaves */
static int spawn_v(const char* exe, char* const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe, argv);
        _exit(127);
    }
    return pid > 0;
}

static char handover_args[8][1100];
static int handover_n, handover_pending;
static unsigned handover_at;

static void handover_exec(void) {
    char exe[2048];
    char* argv[12];
    int i, k = 0;
    if (!handover_pending || !self_path(exe, sizeof(exe))) {
        return;
    }
    handover_pending = 0;
    argv[k++] = exe;
    for (i = 0; i < handover_n; i++) {
        argv[k++] = handover_args[i];
    }
    if (port_opt.image) {
        argv[k++] = (char*)"--image";
        argv[k++] = (char*)port_opt.image;
    }
    argv[k] = NULL;
    spawn_v(exe, argv);
}

/* the game closes through its own quit path and starts again with ARGS
 * (the marathon's driver, the soak, a recorded session) */
int port_handover(int nargs, const char* const* args) {
    int i;
    if (handover_pending) {
        return 1;
    }
    handover_n = nargs < 8 ? nargs : 8;
    for (i = 0; i < handover_n; i++) {
        snprintf(handover_args[i], sizeof(handover_args[i]), "%s", args[i]);
    }
    handover_pending = 1;
    handover_at = gl13_frame_number();
    atexit(handover_exec);
    port_log("port> developer mode (M51): the game closes and starts again with");
    for (i = 0; i < handover_n; i++) {
        port_log(" %s", handover_args[i]);
    }
    port_log("\n");
    port_request_reset();
    return 1;
}

void port_handover_tick(void) {
    if (handover_pending && gl13_frame_number() > handover_at + 300) {
        port_log("port> developer mode (M51): the reset path did not finish; leaving directly\n");
        port_shutdown(0);
    }
}

int port_marathon_start(const void* plan_in) {
    const char* args[2];
    if (!port_marathon_save(port_marathon_path(), plan_in)) {
        port_log("port> marathon (M51): cannot write %s\n", port_marathon_path());
        return 0;
    }
    args[0] = "--marathon";
    args[1] = port_marathon_path();
    return port_handover(2, args);
}

/* the overlay's Start: a new marathon */
int port_marathon_begin(const int* list, int n, int humans, const char* cast, int record) {
    Plan p;
    int i;
    plan_clear(&p);
    for (i = 0; i < n && i < 64; i++) {
        p.list[p.n++] = list[i];
    }
    p.humans = humans < 1 ? 1 : humans > 4 ? 4 : humans;
    snprintf(p.cast, sizeof(p.cast), "%s", cast);
    p.record = record;
    if (!p.n) {
        return 0;
    }
    return port_marathon_start(&p);
}

/* the overlay's Resume: the saved one, from its next minigame */
int port_marathon_resume(void) {
    Plan p;
    if (!port_marathon_load(port_marathon_path(), &p) || p.next >= p.n) {
        return 0;
    }
    return port_marathon_start(&p);
}

/* a child: "Stop the marathon" from its overlay -- the driver stops after it */
void port_marathon_stop_request(void) {
    char p[1100];
    FILE* f;
    snprintf(p, sizeof(p), "%s.stop", port_marathon_path());
    f = fopen(p, "w");
    if (f) {
        fputs("stop\n", f);
        fclose(f);
    }
    port_log("port> marathon (M51): stop asked; the game closes and the marathon stops here\n");
    port_request_reset();
}

int port_marathon_child_banner(char* out, size_t n) {
    if (!port_opt.marathonchild) {
        return 0;
    }
    snprintf(out, n, "Minigame marathon - %s", port_opt.marathonchild);
    return 1;
}

/* ---- the driver ------------------------------------------------------------------ */
static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec * 1e-6;
}

static int read_result(const char* logp, char* out, size_t n) {
    FILE* f = fopen(logp, "r");
    char line[512];
    int got = 0;
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        const char* p = strstr(line, "port> marathon: result ");
        if (p) {
            char mg[16];
            int c[4];
            unsigned fr;
            if (sscanf(p, "port> marathon: result %15s coins %d %d %d %d frames %u", mg, &c[0], &c[1], &c[2], &c[3],
                       &fr) == 6) {
                snprintf(out, n, "%+d %+d %+d %+d %u", c[0], c[1], c[2], c[3], fr);
                got = 1;
            }
        }
    }
    fclose(f);
    return got;
}

static void summary_write(const Plan* p, const char* path, int stopped) {
    FILE* f = fopen(path, "w");
    int i, done = 0, wins[4] = {0, 0, 0, 0};
    static const char* const who[8] = {"Mario", "Luigi", "Peach", "Yoshi", "Wario", "Donkey Kong", "Daisy",
                                       "Waluigi"};
    char cast[4][16];
    char tmp[96];
    char* s;
    int k = 0;
    if (!f) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s", p->cast);
    for (s = strtok(tmp, ","); s && k < 4; s = strtok(NULL, ","), k++) {
        int c;
        snprintf(cast[k], sizeof(cast[k]), "%s", s);
        for (c = 0; c < 8; c++) {
            if (!strncasecmp(who[c], s, 4)) {
                snprintf(cast[k], sizeof(cast[k]), "%s", who[c]);
            }
        }
    }
    for (; k < 4; k++) {
        snprintf(cast[k], sizeof(cast[k]), "P%d", k + 1);
    }
    for (i = 0; i < p->n; i++) {
        int c[4];
        if (p->result[i][0] && sscanf(p->result[i], "%d %d %d %d", &c[0], &c[1], &c[2], &c[3]) == 4) {
            int j, best = 0;
            done++;
            for (j = 0; j < 4; j++) {
                best = c[j] > best ? c[j] : best;
            }
            for (j = 0; j < 4 && best > 0; j++) {
                wins[j] += c[j] == best;
            }
        }
    }
    fprintf(f, "%s\n", stopped ? "! The marathon was stopped -- Developer Mode resumes it" : "+ The marathon is over");
    fprintf(f, "%d of %d minigames played, %d player%s at the controllers\n", done, p->n, p->humans,
            p->humans > 1 ? "s" : "");
    fprintf(f, "  Wins: %s %d, %s %d, %s %d, %s %d (most coins from the minigame)\n", cast[0], wins[0], cast[1],
            wins[1], cast[2], wins[2], cast[3], wins[3]);
    fprintf(f, " \n");
    for (i = 0; i < p->n; i++) {
        int c[4];
        unsigned fr = 0;
        if (!p->result[i][0]) {
            continue;
        }
        if (sscanf(p->result[i], "%d %d %d %d %u", &c[0], &c[1], &c[2], &c[3], &fr) >= 4) {
            fprintf(f, "  m%d %-22.22s %+3d %+3d %+3d %+3d  %u:%02u\n", p->list[i], port_mg_name(p->list[i]), c[0],
                    c[1], c[2], c[3], fr / 3600, (fr / 60) % 60);
        }
    }
    fclose(f);
}

const char* port_marathon_result_path(void) {
    static char p[1024];
    if (!p[0]) {
        snprintf(p, sizeof(p), "%s/marathon-result.txt", port_app_support_dir());
    }
    return p;
}

void port_marathon_driver(void) {
    Plan p;
    char exe[2048], dir[1024], stopf[1100];
    int k, stopped = 0;
    double t0 = now_s();
    if (!port_marathon_load(port_opt.marathon, &p)) {
        port_log("port> marathon (M51): no plan in %s\n", port_opt.marathon);
        port_shutdown(2);
    }
    if (!self_path(exe, sizeof(exe))) {
        port_shutdown(2);
    }
    if (!p.started[0]) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(p.started, sizeof(p.started), "%Y-%m-%d %H:%M", &tm);
    }
    snprintf(dir, sizeof(dir), "%s/marathon-logs", port_app_support_dir());
    mkdir(dir, 0755);
    snprintf(stopf, sizeof(stopf), "%s.stop", port_opt.marathon);
    unlink(stopf);
    port_log("port> marathon (M51): %d minigames, from %d; %d human%s; cast %s%s\n", p.n, p.next, p.humans,
             p.humans > 1 ? "s" : "", p.cast, p.record ? "; each recorded" : "");
    for (k = p.next; k < p.n; k++) {
        char logp[1200], banner[160], mgarg[16], hum[8], recp[1100];
        char* argv[48];
        int argc = 0, status = 0;
        pid_t pid;
        int num = p.list[k];
        snprintf(logp, sizeof(logp), "%s/m%d.log", dir, num);
        snprintf(banner, sizeof(banner), "%d of %d: %s", k + 1, p.n, port_mg_name(num));
        snprintf(mgarg, sizeof(mgarg), "%d", num);
        snprintf(hum, sizeof(hum), "%d", p.humans);
        argv[argc++] = exe;
        argv[argc++] = (char*)"--minigame";
        argv[argc++] = mgarg;
        argv[argc++] = (char*)"--turns";
        argv[argc++] = (char*)"1";
        argv[argc++] = (char*)"--com4";
        argv[argc++] = (char*)"--cast";
        argv[argc++] = p.cast;
        argv[argc++] = (char*)"--humans";
        argv[argc++] = hum;
        argv[argc++] = (char*)"--play";
        argv[argc++] = (char*)"board-start-com4.play";
        argv[argc++] = (char*)"--nomovies";
        argv[argc++] = (char*)"--rtc";
        argv[argc++] = (char*)"dolphin";
        argv[argc++] = (char*)"--freshcard";
        argv[argc++] = (char*)"--ffto";
        argv[argc++] = (char*)"14000";
        argv[argc++] = (char*)"--mgexit";
        argv[argc++] = (char*)"--marathonchild";
        argv[argc++] = banner;
        if (num == 453) {
            argv[argc++] = (char*)"--dvdheap";
            argv[argc++] = (char*)"5888";
        }
        if (p.record) {
            char tag[48];
            snprintf(tag, sizeof(tag), "marathon m%d", num);
            snprintf(recp, sizeof(recp), "%s", port_session_default_path(tag));
            argv[argc++] = (char*)"--sessionrec";
            argv[argc++] = recp;
        }
        if (port_opt.image) {
            argv[argc++] = (char*)"--image";
            argv[argc++] = (char*)port_opt.image;
        }
        if (p.childflags[0]) {
            static char cf[256];
            char* t;
            snprintf(cf, sizeof(cf), "%s", p.childflags);
            for (t = strtok(cf, " "); t && argc < 43; t = strtok(NULL, " ")) {
                argv[argc++] = t;
            }
        }
        if (port_opt.mute) {
            argv[argc++] = (char*)"--mute"; /* M52: a muted driver's children are muted too */
        }
        argv[argc++] = (char*)"--log";
        argv[argc++] = logp;
        argv[argc] = NULL;
        port_log("port> marathon (M51): %d of %d, m%d %s\n", k + 1, p.n, num, port_mg_name(num));
        unlink(logp);
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
            port_log("port> marathon (M51): fork failed (%s)\n", strerror(errno));
            stopped = 1;
            break;
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (!read_result(logp, p.result[k], sizeof(p.result[k]))) {
            port_log("port> marathon (M51): m%d ended without its results (exit %d) -- the marathon stops here\n",
                     num, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            stopped = 1;
            p.next = k;
            break;
        }
        p.next = k + 1;
        port_marathon_save(port_opt.marathon, &p);
        port_log("port> marathon (M51): m%d: %s\n", num, p.result[k]);
        if (access(stopf, F_OK) == 0) {
            unlink(stopf);
            stopped = 1;
            break;
        }
    }
    port_marathon_save(port_opt.marathon, &p);
    summary_write(&p, port_marathon_result_path(), stopped);
    {
        /* the summary also beside the recordings, for the playtest's notes */
        char copy[1100], line[256];
        FILE *in, *out;
        time_t t = time(NULL);
        struct tm tm;
        char when[40];
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H.%M", &tm);
        snprintf(copy, sizeof(copy), "%s", port_session_default_path("x"));
        {
            char* slash = strrchr(copy, '/');
            if (slash) {
                snprintf(slash + 1, sizeof(copy) - (size_t)(slash + 1 - copy), "Marathon %s.txt", when);
            }
        }
        in = fopen(port_marathon_result_path(), "r");
        out = fopen(copy, "w");
        if (in && out) {
            fprintf(out, "Mario Party 4 PowerPC Edition %s -- minigame marathon, started %s\n", PORT_VERSION_STRING,
                     p.started);
            while (fgets(line, sizeof(line), in)) {
                fputs(line[0] == '+' || line[0] == '!' ? line + 2 : line, out);
            }
        }
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
    }
    port_log("port> marathon (M51): %s after %.0f s; the game comes back with the summary\n",
             stopped ? "stopped" : "over", now_s() - t0);
    {
        char* argv[6];
        int argc = 0;
        argv[argc++] = exe;
        argv[argc++] = (char*)"--marathonresult";
        argv[argc++] = (char*)port_marathon_result_path();
        if (port_opt.image) {
            argv[argc++] = (char*)"--image";
            argv[argc++] = (char*)port_opt.image;
        }
        argv[argc] = NULL;
        spawn_v(exe, argv);
    }
    port_shutdown(0);
}
