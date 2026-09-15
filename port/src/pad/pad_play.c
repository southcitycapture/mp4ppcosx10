/* Scripted controller input (`--play`) and recording (`--record`).
 *
 * There is no input-replay facility in the game itself (notes.md §6.3): the
 * clean seam is `PADRead`, which is called exactly once per retrace from the
 * VI post-retrace callback (notes.md §6.2), so a script that overwrites port
 * 1's *raw* PADStatus before PADClamp runs is indistinguishable, downstream,
 * from a real pad -- the game's own button-repeat and dstick-repeat counters
 * in src/game/pad.c run unchanged. Do not synthesise `BtnDown`/edges here.
 *
 * Format ("MP4PLAY"), one directive per line, '#' starts a comment. Frame
 * numbers are the port's own VI retrace count (VIGetRetraceCount()), which
 * tracks the game's own GlobalCounter frame-for-frame along the boot/menu
 * paths the reference rig's scripts are captured on:
 *
 *   at <frame> <frames> <tok>[,<tok>...]   hold for <frames> frames starting
 *                                          at frame <frame>; each <tok> is
 *                                          one of:
 *     A B X Y Z L R START                    a digital GC button
 *     dstk:<DIR>[:<DIR>]                     push the main stick toward DIR
 *                                             (UP/DOWN/LEFT/RIGHT, combine for
 *                                             a diagonal) -- resolved to a raw
 *                                             stick value, not a synthesised
 *                                             _PadDStk, so PadADConv derives
 *                                             the direction the same way it
 *                                             would from a real stick
 *     stick:<x>:<y>                          an explicit raw main-stick push,
 *                                             -100..100 each axis
 *   mark <frame> <label>                   no state change; port_log's the
 *                                          label when that frame is reached,
 *                                          for eyeballing a script against a
 *                                          run's own log
 *
 * `port/tools/gecko2play.py` converts a `port/ref/tools/mkgecko.py`-format
 * reference script (Dolphin Gecko codes patching the game's private globals
 * directly, address-based) into this format.
 *
 * `--record FILE` writes back in the same format: a run-length-encoded list
 * of `at` lines built from whatever port 1 actually reads each frame (after
 * the SDL/Xbox/keyboard mix, before PADClamp), so a captured session replays
 * with `--play` unchanged.
 */
#include "pad_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

struct pad_play_cmd {
    u32 first, last; /* inclusive frame range */
    u16 buttons;
    int has_stick;
    s8 x, y;
};

static struct pad_play_cmd* script;
static int script_len, script_cap;

struct pad_play_mark {
    u32 frame;
    char* label;
};
static struct pad_play_mark* marks;
static int marks_len, marks_cap;

static const struct { const char* name; u16 bit; } button_names[] = {
    { "A", PAD_BUTTON_A },     { "B", PAD_BUTTON_B },     { "X", PAD_BUTTON_X },
    { "Y", PAD_BUTTON_Y },     { "Z", PAD_TRIGGER_Z },    { "L", PAD_TRIGGER_L },
    { "R", PAD_TRIGGER_R },    { "START", PAD_BUTTON_START }, { NULL, 0 }
};

static int parse_button(const char* tok) {
    int i;
    for (i = 0; button_names[i].name != NULL; i++) {
        if (strcasecmp(tok, button_names[i].name) == 0) {
            return button_names[i].bit;
        }
    }
    return -1;
}

static void dstk_vector(const char* dirs, s8* x, s8* y) {
    /* dirs is e.g. "UP" or "UP:LEFT" (already split off the "dstk:" prefix) */
    char buf[32];
    char* tok;
    int up = 0, down = 0, left = 0, right = 0;
    long vx, vy;
    strncpy(buf, dirs, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = strtok(buf, ":,"); tok != NULL; tok = strtok(NULL, ":,")) {
        if (strcasecmp(tok, "UP") == 0) {
            up = 1;
        } else if (strcasecmp(tok, "DOWN") == 0) {
            down = 1;
        } else if (strcasecmp(tok, "LEFT") == 0) {
            left = 1;
        } else if (strcasecmp(tok, "RIGHT") == 0) {
            right = 1;
        } else {
            fprintf(stderr, "port: --play: unknown dstk direction '%s'\n", tok);
        }
    }
    vx = (long)right * 100 - (long)left * 100;
    vy = (long)up * 100 - (long)down * 100;
    if (vx != 0 && vy != 0) { /* diagonal: keep the pot inside its own range */
        vx = vx * 70 / 100;
        vy = vy * 70 / 100;
    }
    *x = (s8)vx;
    *y = (s8)vy;
}

static void append_cmd(struct pad_play_cmd c) {
    if (script_len == script_cap) {
        script_cap = script_cap ? script_cap * 2 : 64;
        script = realloc(script, sizeof(*script) * script_cap);
    }
    script[script_len++] = c;
}

static void append_mark(u32 frame, const char* label) {
    if (marks_len == marks_cap) {
        marks_cap = marks_cap ? marks_cap * 2 : 16;
        marks = realloc(marks, sizeof(*marks) * marks_cap);
    }
    marks[marks_len].frame = frame;
    marks[marks_len].label = strdup(label);
    marks_len++;
}

static void parse_at_tokens(char* toks, u32 first, u32 last) {
    char* save = NULL;
    char* tok;
    struct pad_play_cmd c;
    memset(&c, 0, sizeof(c));
    c.first = first;
    c.last = last;
    for (tok = strtok_r(toks, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        int bit;
        if (strncasecmp(tok, "dstk:", 5) == 0) {
            s8 x, y;
            dstk_vector(tok + 5, &x, &y);
            c.has_stick = 1;
            c.x = x;
            c.y = y;
        } else if (strncasecmp(tok, "stick:", 6) == 0) {
            int x = 0, y = 0;
            if (sscanf(tok + 6, "%d:%d", &x, &y) != 2) {
                fprintf(stderr, "port: --play: bad stick token '%s'\n", tok);
            }
            c.has_stick = 1;
            c.x = (s8)x;
            c.y = (s8)y;
        } else if ((bit = parse_button(tok)) >= 0) {
            c.buttons |= (u16)bit;
        } else if (strcmp(tok, "-") == 0) {
            /* explicit "nothing" placeholder, used by the recorder for idle runs */
        } else {
            fprintf(stderr, "port: --play: unknown token '%s'\n", tok);
        }
    }
    append_cmd(c);
}

/* A bare script name -- `--play board-start.play` -- is what every runbook and
 * every g4 invocation writes, and the working directory of a run started by the
 * console runner on the G4 is not the tree the scripts live in.  So a name with
 * no '/' in it is also looked for inside the bundle, next to the disc image:
 * make_bundle.sh ships port/ref/movies there as Contents/Resources/movies.
 * Without this the run does not fail -- it sits on the title screen for as long
 * as it is given, which cost one witness session an hour. */
static FILE* open_script(const char* path, char* used, size_t used_n) {
    FILE* f = fopen(path, "r");

    snprintf(used, used_n, "%s", path);
    if (f != NULL || strchr(path, '/') != NULL) {
        return f;
    }
#if defined(__APPLE__)
    {
        char exe[1024];
        uint32_t n = (uint32_t)sizeof(exe);
        if (_NSGetExecutablePath(exe, &n) == 0) {
            char* slash = strrchr(exe, '/'); /* strip the executable */
            if (slash != NULL) {
                *slash = '\0';
                slash = strrchr(exe, '/'); /* strip MacOS */
                if (slash != NULL) {
                    *slash = '\0';
                    snprintf(used, used_n, "%s/Resources/movies/%s", exe, path);
                    f = fopen(used, "r");
                    if (f == NULL) {
                        snprintf(used, used_n, "%s/Resources/%s", exe, path);
                        f = fopen(used, "r");
                    }
                }
            }
        }
    }
#endif
    if (f == NULL) {
        snprintf(used, used_n, "%s", path);
    }
    return f;
}

static void load_script(const char* path) {
    char used[1024];
    FILE* f = open_script(path, used, sizeof(used));
    char line[512];
    int lineno = 0;
    if (f == NULL) {
        port_log("port> pad: --play: cannot open %s\n", path);
        return;
    }
    if (strcmp(used, path) != 0) {
        port_log("port> pad: --play: %s -> %s\n", path, used);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char op[16], a1[16], a2[16], toks[480];
        char* hash;
        int n;
        lineno++;
        hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        n = sscanf(line, "%15s %15s %15s %479[^\n]", op, a1, a2, toks);
        if (n < 1 || op[0] == '\0') {
            continue;
        }
        if (strcasecmp(op, "at") == 0 && n == 4) {
            u32 first = (u32)strtoul(a1, NULL, 0);
            u32 count = (u32)strtoul(a2, NULL, 0);
            if (count == 0) {
                count = 1;
            }
            parse_at_tokens(toks, first, first + count - 1);
        } else if (strcasecmp(op, "mark") == 0 && n >= 3) {
            char label[480];
            label[0] = '\0';
            if (n == 4) {
                snprintf(label, sizeof(label), "%s %s", a2, toks);
            } else {
                snprintf(label, sizeof(label), "%s", a2);
            }
            append_mark((u32)strtoul(a1, NULL, 0), label);
        } else {
            fprintf(stderr, "port: --play: %s:%d: bad line: %s", path, lineno, line);
        }
    }
    fclose(f);
    port_log("port> pad: --play %s: %d directives, %d marks\n", path, script_len, marks_len);
}

/* ---- recording ------------------------------------------------------------ */

static FILE* rec;
static u32 rec_run_start;
static u16 rec_last_buttons;
static s8 rec_last_x, rec_last_y;
static int rec_have_run;

static void rec_write_run(u32 end_frame_exclusive) {
    if (!rec_have_run || end_frame_exclusive <= rec_run_start) {
        return;
    }
    fprintf(rec, "at %u %u ", rec_run_start, end_frame_exclusive - rec_run_start);
    if (rec_last_buttons == 0 && rec_last_x == 0 && rec_last_y == 0) {
        fprintf(rec, "-\n");
        return;
    }
    {
        int wrote = 0;
        int i;
        for (i = 0; button_names[i].name != NULL; i++) {
            if (rec_last_buttons & button_names[i].bit) {
                fprintf(rec, "%s%s", wrote ? "," : "", button_names[i].name);
                wrote = 1;
            }
        }
        if (rec_last_x != 0 || rec_last_y != 0) {
            fprintf(rec, "%sstick:%d:%d", wrote ? "," : "", rec_last_x, rec_last_y);
        }
        fprintf(rec, "\n");
    }
}

static void rec_start(const char* path) {
    rec = fopen(path, "w");
    if (rec == NULL) {
        port_log("port> pad: --record: cannot create %s\n", path);
        return;
    }
    fprintf(rec, "# Recorded by the Mario Party 4 port (port/src/pad/pad_play.c).\n");
    fprintf(rec, "# Frame numbers are VIGetRetraceCount(), port 1 raw state only.\n");
    port_log("port> pad: recording controller 1 to %s\n", path);
}

static void rec_step(u32 frame, const PortPadRaw* raw) {
    if (rec == NULL) {
        return;
    }
    if (!rec_have_run) {
        rec_have_run = 1;
        rec_run_start = frame;
        rec_last_buttons = raw->button;
        rec_last_x = raw->stickX;
        rec_last_y = raw->stickY;
        return;
    }
    if (raw->button != rec_last_buttons || raw->stickX != rec_last_x || raw->stickY != rec_last_y) {
        rec_write_run(frame);
        rec_run_start = frame;
        rec_last_buttons = raw->button;
        rec_last_x = raw->stickX;
        rec_last_y = raw->stickY;
    }
}

static void rec_finish(u32 last_frame_seen) {
    if (rec == NULL) {
        return;
    }
    rec_write_run(last_frame_seen + 1);
    fclose(rec);
    rec = NULL;
    port_log("port> pad: recording closed\n");
}

/* ---- public entry points --------------------------------------------------- */

static int have_script;
static u32 last_frame_seen;
static int have_last_frame;

void pad_play_init(const char* script_path, const char* record_path) {
    if (script_path != NULL) {
        load_script(script_path);
        have_script = 1;
    }
    if (record_path != NULL) {
        rec_start(record_path);
    }
}

void pad_play_shutdown(void) {
    if (have_last_frame) {
        rec_finish(last_frame_seen);
    }
    free(script);
    script = NULL;
    script_len = script_cap = 0;
    {
        int i;
        for (i = 0; i < marks_len; i++) {
            free(marks[i].label);
        }
    }
    free(marks);
    marks = NULL;
    marks_len = marks_cap = 0;
}

/* How many frames the script has actually driven a button on.  The soak's
 * title-screen guard asks this: "is anything pressing anything?" is a
 * different question from "was a script named", and the overnight run that
 * was lost to the attract loop failed the first one (PLAN.md 22.3). */
static u32 script_presses;

u32 pad_play_press_count(void) { return script_presses; }

void pad_play_step(u32 frame, PortPadRaw* raw) {
    last_frame_seen = frame;
    have_last_frame = 1;

    {
        int i;
        for (i = 0; i < marks_len; i++) {
            if (marks[i].frame == frame) {
                port_log("port> pad: --play mark %u: %s\n", frame, marks[i].label);
            }
        }
    }

    if (have_script) {
        int i;
        u16 buttons = 0;
        int has_stick = 0;
        s8 x = 0, y = 0;
        for (i = 0; i < script_len; i++) {
            struct pad_play_cmd* c = &script[i];
            if (frame < c->first || frame > c->last) {
                continue;
            }
            buttons |= c->buttons;
            if (c->has_stick) {
                has_stick = 1;
                x = c->x;
                y = c->y;
            }
        }
        raw->button = buttons;
        if (buttons != 0) {
            script_presses++;
        }
        if (has_stick) {
            raw->stickX = x;
            raw->stickY = y;
        } else {
            raw->stickX = raw->stickY = 0;
        }
        raw->substickX = raw->substickY = 0;
        raw->triggerL = raw->triggerR = 0;
    }

    rec_step(frame, raw);
}
