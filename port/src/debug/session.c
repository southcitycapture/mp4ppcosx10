/* M51 (PLAN.md 66): session recordings -- `--sessionrec FILE` writes one,
 * `--replay FILE` plays one back.
 *
 * What a session is.  From the boot, with the deterministic clock (`--rtc`:
 * OSGetTime is a function of the retrace count, so both of the game's RNGs
 * seed the same way every time -- os_misc.c), everything the game can see
 * that does not come from the disc: the four controllers as PADRead hands
 * them over each retrace (after the overlay menu has taken what it reads,
 * after the harness's own thumb -- exactly the four PADStatus the game
 * clamps), the start state (the clock's origin, the card, the settings, the
 * harness's flags), and every write the self-play harness makes to the
 * game's work areas (`--com4`, `--minigame`, `--humans`: GWPlayerCfg,
 * GWPlayer, GWSystem, diffed around port_selfplay_tick).  The port runs one
 * retrace per game frame and nothing else it does reaches the game's state
 * (vi.c), so that is the whole input and a replay is the same game.
 *
 * Two more things are written for the other side of the rig -- Dolphin
 * (tools/rec2dtm.py), where the game runs on the console's timeline, not
 * the port's:
 *
 *   `b` the board RNG's seed as BoardRandInit reads it (patches.txt M51:
 *       the console reads its own clock there, which the port cannot know,
 *       so the replay hands Dolphin the port's seeds -- a Gecko C2 hook);
 *   `s` the game's signature at the end of each frame: GlobalCounter, the
 *       overlay, the engine RNG, rand8's LCG, the board RNG -- written when
 *       any of them moved.  The Dolphin replay aligns the two timelines on
 *       it (the console waits on the disc where the port does not), and a
 *       port replay checks itself against it frame by frame.
 *
 * The format is text ("MP4REC 1"), a header of `key value` lines and then
 * one event a line, retrace-numbered:
 *
 *   p R PORT ERR BTN SX SY CX CY TL TR   a controller changed (hex BTN; ERR -1 none)
 *   s R GC OVL FRAND RND8 BRAND          the signature changed (hex)
 *   w R REGION OFF HEX                   the harness wrote bytes at REGION+OFF
 *   b R SEED                             BoardRandInit's seed (hex)
 *   m R TEXT                             a note (minigame entry, results, ...)
 *   end R
 *
 * Costs: a compare of 5 words and 4 PADStatus a retrace, a line when
 * something moved -- nothing a frame counter can see (PLAN.md 66). */
#include "port.h"

#include "game/gamework_data.h"
#include "game/object.h"
#include "game/pad.h"
#include "game/board/main.h"

#include <dolphin/pad.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern u32 GlobalCounter;
extern s32 rnd_seed;
u32 port_frand_seed(void);
int port_machine_line(char* out, size_t n);

/* ---- the recorder -------------------------------------------------------- */
static FILE* rec;
static PADStatus rec_last[PAD_CHANMAX];
static int rec_have_last;
static u32 sig_last[5];
static int sig_have;
static u32 rec_frame;
static char rec_path[1024];

/* the harness's regions */
typedef struct {
    const char* name;
    void* p;
    unsigned n;
} Region;
static Region regions[3];
static unsigned char region_before[3][0x120];

static void regions_init(void) {
    if (regions[0].p) {
        return;
    }
    regions[0].name = "GWPlayerCfg";
    regions[0].p = GWPlayerCfg;
    regions[0].n = (unsigned)sizeof(GWPlayerCfg);
    regions[1].name = "GWPlayer";
    regions[1].p = GWPlayer;
    regions[1].n = (unsigned)sizeof(GWPlayer);
    regions[2].name = "GWSystem";
    regions[2].p = &GWSystem;
    regions[2].n = (unsigned)sizeof(GWSystem);
}

static void rec_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void rec_printf(const char* fmt, ...) {
    va_list ap;
    if (!rec) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(rec, fmt, ap);
    va_end(ap);
}

int port_session_recording(void) { return rec != NULL; }
const char* port_session_record_path(void) { return rec ? rec_path : NULL; }

/* ~/Documents/MarioParty4 Recordings/DATE-TIME[-TAG].rec (made if missing) */
const char* port_session_default_path(const char* tag) {
    static char p[1024];
    char dir[900];
    const char* home = getenv("HOME");
    time_t t = time(NULL);
    struct tm tm;
    char when[40];
    localtime_r(&t, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H.%M.%S", &tm);
    snprintf(dir, sizeof(dir), "%s/Documents", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/Documents/MarioParty4 Recordings", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(p, sizeof(p), "%s/%s%s%s.rec", dir, when, tag && *tag ? " " : "", tag ? tag : "");
    return p;
}

void port_session_close(void) {
    if (!rec) {
        return;
    }
    rec_printf("end %u\n", rec_frame);
    fclose(rec);
    rec = NULL;
    port_log("port> session (M51): recording closed at retrace %u: %s\n", rec_frame, rec_path);
}

/* the argv the run was started with, and what a replay needs of it */
static const char* const replay_keep1[] = {"--rtc", "--rtcoffset", "--seed", "--card", "--cast", "--minigame",
                                           "--turns", "--board", "--giveitem", "--goto", "--liteopts",
                                           "--dvdheap", "--humans", "--litefishk", "--litechar", "--water",
                                           "--blobr", "--bloba", NULL};
static const char* const replay_keep0[] = {"--deterministic", "--freshcard", "--com4", "--mghold", "--nomovies",
                                           "--movies", "--lite", "--nolite", "--liteauto", "--mgexit", "--soak",
                                           NULL};

static int in_list(const char* a, const char* const* l) {
    int i;
    for (i = 0; l[i]; i++) {
        if (!strcmp(a, l[i])) {
            return 1;
        }
    }
    return 0;
}

void port_session_open(int argc, char** argv) {
    char line[160];
    int i;
    time_t t = time(NULL);
    struct tm tm;
    char when[40];
    if (!port_opt.session_record || rec) {
        return;
    }
    snprintf(rec_path, sizeof(rec_path), "%s", port_opt.session_record);
    rec = fopen(rec_path, "w");
    if (!rec) {
        port_log("port> session (M51): cannot create %s (%s); not recording\n", rec_path, strerror(errno));
        return;
    }
    setvbuf(rec, NULL, _IOFBF, 1 << 16);
    localtime_r(&t, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    rec_printf("MP4REC 1\n");
    rec_printf("game Mario Party 4 PowerPC Edition %s (milestone %s)\n", PORT_VERSION_STRING, PORT_MILESTONE);
    rec_printf("date %s\n", when);
    port_machine_line(line, sizeof(line));
    rec_printf("machine %s\n", line);
    rec_printf("rtc %lld\n", (long long)port_opt.rtc);
    rec_printf("seed %lld\n", (long long)port_opt.seed);
    rec_printf("clockskew %lld\n", port_opt.clockskew);
    rec_printf("card %s\n", port_opt.freshcard ? "fresh" : port_opt.card ? port_opt.card : "player");
    rec_printf("args");
    for (i = 1; i < argc; i++) {
        rec_printf(" %s", argv[i]);
    }
    rec_printf("\n");
    /* the replay's arguments: the state-bearing flags, and the settings the
     * config resolved (a replay reads no config) */
    rec_printf("replay --rtc %lld --clockskew %lld", (long long)port_opt.rtc, port_opt.clockskew);
    if (port_opt.rtc_offset != 0.0) {
        rec_printf(" --rtcoffset %.6f", port_opt.rtc_offset);
    }
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--clockskew")) {
            i++;
            continue;
        }
        if (!strcmp(a, "--rtc") || !strcmp(a, "--rtcoffset") || !strcmp(a, "--lite") || !strcmp(a, "--nolite") ||
            !strcmp(a, "--liteauto") || !strcmp(a, "--liteopts") || !strcmp(a, "--movies") ||
            !strcmp(a, "--nomovies") || !strcmp(a, "--water")) {
            if (in_list(a, replay_keep1) && i + 1 < argc) {
                i++;
            }
            continue; /* written below from the resolved settings */
        }
        if (in_list(a, replay_keep1) && i + 1 < argc) {
            rec_printf(" %s %s", a, argv[i + 1]);
            i++;
        } else if (in_list(a, replay_keep0)) {
            rec_printf(" %s", a);
        }
    }
    if (!port_opt.freshcard && !port_opt.card) {
        rec_printf(" --card \"%s/memcard-slot-a.raw\"", port_app_support_dir());
    }
    rec_printf(" %s", port_opt.nomovies ? "--nomovies" : "--movies");
    rec_printf(" %s", port_opt.lite == 0 ? "--nolite" : port_opt.lite == 1 ? "--lite" : "--liteauto");
    if (port_opt.liteopts && *port_opt.liteopts) {
        rec_printf(" --liteopts %s", port_opt.liteopts);
    }
    rec_printf(" --water %s", port_opt.water == 0 ? "off" : port_opt.water == 1 ? "cheap"
                              : port_opt.water == 2 ? "full" : "auto");
    rec_printf("\n");
    rec_printf("start\n");
    fflush(rec);
    port_log("port> session (M51): recording to %s (the clock's origin %lld, the card %s)\n", rec_path,
             (long long)port_opt.rtc, port_opt.freshcard ? "fresh" : "the player's");
    atexit(port_session_close);
}

void port_session_note(const char* fmt, ...) {
    va_list ap;
    char buf[256];
    if (!rec) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rec_printf("m %u %s\n", rec_frame, buf);
}

/* ---- the replayer ---------------------------------------------------------- */
typedef struct {
    u32 r;
    int port;
    PADStatus st;
} PadEv;
typedef struct {
    u32 r;
    u32 v[5];
} SigEv;
static PadEv* rp_pad;
static int rp_npad, rp_ipad;
static SigEv* rp_sig;
static int rp_nsig, rp_isig;
static u32 rp_end;
static PADStatus rp_cur[PAD_CHANMAX];
static int rp_loaded;
static u32 rp_mismatch_at, rp_mismatches, rp_checked;
static u32 rp_expect[5];
static int rp_have_expect;

static void replay_load(void) {
    FILE* f;
    char line[1024];
    int cap_p = 0, cap_s = 0, i;
    if (rp_loaded) {
        return;
    }
    rp_loaded = 1;
    for (i = 0; i < PAD_CHANMAX; i++) {
        rp_cur[i].err = PAD_ERR_NO_CONTROLLER;
    }
    f = fopen(port_opt.replay, "r");
    if (!f) {
        port_log("port> replay (M51): cannot open %s\n", port_opt.replay);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned r, a[10];
        int port, err;
        if (line[0] == 'p' && line[1] == ' ' &&
            sscanf(line + 2, "%u %d %d %x %u %u %u %u %u %u", &r, &port, &err, &a[0], &a[1], &a[2], &a[3], &a[4],
                   &a[5], &a[6]) == 10) {
            PadEv* e;
            if (rp_npad == cap_p) {
                cap_p = cap_p ? cap_p * 2 : 4096;
                rp_pad = realloc(rp_pad, sizeof(*rp_pad) * (size_t)cap_p);
            }
            e = &rp_pad[rp_npad++];
            memset(e, 0, sizeof(*e));
            e->r = r;
            e->port = port & 3;
            e->st.err = (s8)err;
            e->st.button = (u16)a[0];
            e->st.stickX = (s8)(int)a[1];
            e->st.stickY = (s8)(int)a[2];
            e->st.substickX = (s8)(int)a[3];
            e->st.substickY = (s8)(int)a[4];
            e->st.triggerL = (u8)a[5];
            e->st.triggerR = (u8)a[6];
        } else if (line[0] == 's' && line[1] == ' ' &&
                   sscanf(line + 2, "%u %x %x %x %x %x", &r, &a[0], &a[1], &a[2], &a[3], &a[4]) == 6) {
            SigEv* e;
            if (rp_nsig == cap_s) {
                cap_s = cap_s ? cap_s * 2 : 16384;
                rp_sig = realloc(rp_sig, sizeof(*rp_sig) * (size_t)cap_s);
            }
            e = &rp_sig[rp_nsig++];
            e->r = r;
            for (i = 0; i < 5; i++) {
                e->v[i] = a[i];
            }
        } else if (!strncmp(line, "end ", 4)) {
            rp_end = (u32)strtoul(line + 4, NULL, 10);
        }
    }
    fclose(f);
    port_log("port> replay (M51): %s: %d controller changes, %d signature lines, the end at retrace %u\n",
             port_opt.replay, rp_npad, rp_nsig, rp_end);
}

/* PADRead's four slots, last: record them, or replace them */
void port_session_pad(u32 frame, void* status4) {
    PADStatus* st = (PADStatus*)status4;
    int i;
    rec_frame = frame;
    if (port_opt.replay) {
        replay_load();
        while (rp_ipad < rp_npad && rp_pad[rp_ipad].r <= frame) {
            rp_cur[rp_pad[rp_ipad].port] = rp_pad[rp_ipad].st;
            rp_ipad++;
        }
        memcpy(st, rp_cur, sizeof(PADStatus) * PAD_CHANMAX);
    }
    if (!rec) {
        return;
    }
    for (i = 0; i < PAD_CHANMAX; i++) {
        const PADStatus* s = &st[i];
        const PADStatus* o = &rec_last[i];
        if (rec_have_last && s->err == o->err && s->button == o->button && s->stickX == o->stickX &&
            s->stickY == o->stickY && s->substickX == o->substickX && s->substickY == o->substickY &&
            s->triggerL == o->triggerL && s->triggerR == o->triggerR) {
            continue;
        }
        rec_printf("p %u %d %d %x %d %d %d %d %u %u\n", frame, i, (int)s->err, (unsigned)s->button, (int)s->stickX,
                   (int)s->stickY, (int)s->substickX, (int)s->substickY, (unsigned)s->triggerL,
                   (unsigned)s->triggerR);
    }
    memcpy(rec_last, st, sizeof(rec_last));
    rec_have_last = 1;
}

/* the harness's writes: the regions before and after port_selfplay_tick */
void port_session_harness_before(void) {
    int k;
    if (!rec) {
        return;
    }
    regions_init();
    for (k = 0; k < 3; k++) {
        memcpy(region_before[k], regions[k].p, regions[k].n);
    }
}

void port_session_harness_after(u32 frame) {
    int k;
    unsigned i;
    if (!rec) {
        return;
    }
    for (k = 0; k < 3; k++) {
        const unsigned char* now = (const unsigned char*)regions[k].p;
        for (i = 0; i < regions[k].n; i++) {
            unsigned j;
            if (now[i] == region_before[k][i]) {
                continue;
            }
            j = i;
            while (j < regions[k].n && (now[j] != region_before[k][j] || (j + 1 < regions[k].n &&
                                                                          now[j + 1] != region_before[k][j + 1]))) {
                j++;
            }
            rec_printf("w %u %s %x ", frame, regions[k].name, i);
            for (; i < j; i++) {
                rec_printf("%02x", now[i]);
            }
            rec_printf("\n");
        }
    }
}

/* patches.txt M51: BoardRandInit's seed */
void port_session_board_seed(u32* seed) {
    if (rec) {
        rec_printf("b %u %x\n", rec_frame, (unsigned)*seed);
    }
}

/* the end of a frame (VIWaitForRetrace, before the next retrace's PADRead
 * numbered `next`): the signature */
void port_session_frame_end(u32 next) {
    u32 v[5];
    int i;
    if (!rec && !port_opt.replay) {
        return;
    }
    v[0] = GlobalCounter;
    v[1] = (u32)omcurovl;
    v[2] = port_frand_seed();
    v[3] = (u32)rnd_seed;
    v[4] = boardRandSeed;
    if (rec) {
        if (!sig_have || v[1] != sig_last[1] || v[2] != sig_last[2] || v[3] != sig_last[3] || v[4] != sig_last[4] ||
            v[0] - next != sig_last[0]) {
            rec_printf("s %u %x %x %x %x %x\n", next, (unsigned)v[0], (unsigned)v[1], (unsigned)v[2],
                       (unsigned)v[3], (unsigned)v[4]);
            memcpy(sig_last, v, sizeof(v));
            sig_last[0] = v[0] - next;
            sig_have = 1;
        }
    }
    if (port_opt.replay && rp_loaded) {
        /* the recording's signature at `next`: the last line at or before it,
         * GlobalCounter advancing with the retraces from there */
        while (rp_isig < rp_nsig && rp_sig[rp_isig].r <= next) {
            memcpy(rp_expect, rp_sig[rp_isig].v, sizeof(rp_expect));
            rp_expect[0] -= rp_sig[rp_isig].r; /* as a delta */
            rp_have_expect = 1;
            rp_isig++;
        }
        if (rp_have_expect && (!rp_end || next <= rp_end)) {
            int same = v[0] - next == rp_expect[0];
            for (i = 1; i < 5 && same; i++) {
                same = v[i] == rp_expect[i];
            }
            rp_checked++;
            if (!same) {
                if (!rp_mismatches) {
                    rp_mismatch_at = next;
                    port_log("port> replay (M51): OUT OF STEP at retrace %u: gc %x ovl %x frand %08x rnd8 %08x "
                             "brand %08x, the recording has gc %x ovl %x frand %08x rnd8 %08x brand %08x\n",
                             next, (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)v[4],
                             (unsigned)(rp_expect[0] + next), (unsigned)rp_expect[1], (unsigned)rp_expect[2],
                             (unsigned)rp_expect[3], (unsigned)rp_expect[4]);
                }
                rp_mismatches++;
            }
        }
        if (rp_end && next == rp_end) {
            port_log("port> replay (M51): the recording's end, retrace %u: %u frames checked, %s\n", next,
                     rp_checked, rp_mismatches ? "OUT OF STEP" : "in step to the end");
            if (rp_mismatches) {
                port_log("port> replay (M51): %u frames out of step, the first at retrace %u\n", rp_mismatches,
                         rp_mismatch_at);
            }
            if (port_opt.replay_exit) {
                port_shutdown(rp_mismatches ? 3 : 0);
            }
        }
    }
}

/* main(): --replay FILE's own arguments (the header's `replay` line) go in
 * front of the command line's, so a flag on the command line still wins */
int port_session_replay_argv(int argc, char** argv, int* out_argc, char*** out_argv) {
    int i, n = 0, k;
    const char* path = NULL;
    FILE* f;
    char line[4096];
    char** nv;
    static char* toks[256];
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--replay")) {
            path = argv[i + 1];
        }
    }
    if (!path || !(f = fopen(path, "r"))) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "start", 5)) {
            break;
        }
        if (!strncmp(line, "replay ", 7)) {
            char* p = line + 7;
            while (*p && n < 250) {
                char* s;
                while (*p == ' ' || *p == '\n' || *p == '\r') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                if (*p == '"') {
                    s = ++p;
                    while (*p && *p != '"') {
                        p++;
                    }
                } else {
                    s = p;
                    while (*p && *p != ' ' && *p != '\n' && *p != '\r') {
                        p++;
                    }
                }
                if (*p) {
                    *p++ = 0;
                }
                toks[n++] = strdup(s);
            }
        }
    }
    fclose(f);
    nv = calloc((size_t)(argc + n + 2), sizeof(char*));
    k = 0;
    nv[k++] = argv[0];
    for (i = 0; i < n; i++) {
        nv[k++] = toks[i];
    }
    nv[k++] = (char*)"--noconfig";
    for (i = 1; i < argc; i++) {
        nv[k++] = argv[i];
    }
    nv[k] = NULL;
    *out_argc = k;
    *out_argv = nv;
    return 1;
}
