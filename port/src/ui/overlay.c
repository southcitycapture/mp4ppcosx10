/* M50 (PLAN.md 65): the port's overlay menu -- Benchmark Mode's door.
 *
 * A panel drawn by the port over the game's picture, in the manner of the
 * Snowboard Kids ports' in-game launcher: GL 1.3 fixed function, one 8x16
 * bitmap font (Terminus Bold, ui_font16.h) in one 128x128 texture, quads in
 * a 640x480 orthographic frame drawn into the EFB just before the present --
 * so it is scaled with the picture in fullscreen, and on no frame the game
 * or --dumpframe reads.
 *
 *   F1 or M       opens and closes it (Esc closes it while it is open);
 *   the pad       up/down (the stick or the D-pad) chooses, A or Start
 *                 selects, B goes back; the keyboard through its pad mapping
 *                 (the arrows, Return, Z = A, X = B).
 *
 * While it is open the game keeps running and its controllers read nothing
 * (PADRead's four slots are held at rest after the menu has read them).  On
 * the first launch -- a config without `benchoffer` -- it opens once by
 * itself on the title to offer Benchmark Mode; the answer is remembered.
 * A scripted run (--play, --record, --soak, --noconfig, --com4, --nopad) and
 * --nomenu never see it (the md5 walks, the picture checks, the soaks);
 * --menu N[:PAGE] opens it at presented frame N for the lab's pictures.
 *
 * The drawing is one rt_call (gx_rt.h) a presented frame carrying the
 * frame's rectangles and strings: it runs on the render thread (or inline
 * without one), saves every GL state it touches (glPushAttrib and the three
 * matrix stacks) and puts it back, so the state shadows stay true. */
#include "port.h"

#include <dolphin/types.h>
#include <dolphin/pad.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#endif

#include "ui_font16.h"

void rt_call(void (*fn)(void*), const void* args, size_t n, int sync); /* gx_rt.h */
unsigned gl13_frame_number(void);
int port_machine_line(char* out, size_t n);
int port_machine_class(void);
int port_bench_start(void);            /* bench.c: leave the game for Benchmark Mode */
const char* port_bench_result_path(void);
int port_bench_child_banner(char* out, size_t n);
void port_request_reset(void);

/* ---- the frame's drawing: rectangles and strings ------------------------- */
#define UI_MAX_RECT 14
#define UI_MAX_TEXT 48
#define UI_COLS 76
typedef struct {
    short x0, y0, x1, y1;
    unsigned char c[4];
} UiRect;
typedef struct {
    short x, y;
    unsigned char scale;
    unsigned char c[4];
    char s[UI_COLS + 1];
} UiText;
typedef struct {
    int nrect, ntext;
    int clear; /* the whole EFB black first (Benchmark Mode's fast-forward) */
    UiRect rect[UI_MAX_RECT];
    UiText text[UI_MAX_TEXT];
} UiDraw;

static UiDraw ui_d;

static void d_rect(int x0, int y0, int x1, int y1, unsigned r, unsigned g, unsigned b, unsigned a) {
    UiRect* q;
    if (ui_d.nrect >= UI_MAX_RECT) {
        return;
    }
    q = &ui_d.rect[ui_d.nrect++];
    q->x0 = (short)x0;
    q->y0 = (short)y0;
    q->x1 = (short)x1;
    q->y1 = (short)y1;
    q->c[0] = (unsigned char)r;
    q->c[1] = (unsigned char)g;
    q->c[2] = (unsigned char)b;
    q->c[3] = (unsigned char)a;
}

static void d_text(int x, int y, int scale, unsigned rgb, const char* s) {
    UiText* t;
    if (ui_d.ntext >= UI_MAX_TEXT) {
        return;
    }
    t = &ui_d.text[ui_d.ntext++];
    t->x = (short)x;
    t->y = (short)y;
    t->scale = (unsigned char)scale;
    t->c[0] = (unsigned char)(rgb >> 16);
    t->c[1] = (unsigned char)(rgb >> 8);
    t->c[2] = (unsigned char)rgb;
    t->c[3] = 255;
    snprintf(t->s, sizeof(t->s), "%s", s);
}

#ifndef PORT_NO_SDL
/* the render thread's side: raw GL (this file does not include gx_rt.h) */
#define UI_TEX_NAME 0x7FFF0051u
#define UI_TEX_W 128
#define UI_TEX_H 128
static unsigned char ui_tex_px[UI_TEX_W * UI_TEX_H * 4];

static void ui_tex_build(void) {
    int i, x, y;
    memset(ui_tex_px, 0, sizeof(ui_tex_px));
    for (i = 0; i < 96; i++) {
        int cx = (i % 16) * 8, cy = (i / 16) * 16;
        for (y = 0; y < 16; y++) {
            unsigned bits = i < 95 ? ui_font16[i][y] : 0xFFu; /* cell 95: solid, for the panels */
            for (x = 0; x < 8; x++) {
                unsigned char* p = &ui_tex_px[((cy + y) * UI_TEX_W + cx + x) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = (bits & (0x80u >> x)) ? 255 : 0;
            }
        }
    }
}

static void ui_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    glTexCoord2f(u0, v0);
    glVertex2f(x0, y0);
    glTexCoord2f(u1, v0);
    glVertex2f(x1, y0);
    glTexCoord2f(u1, v1);
    glVertex2f(x1, y1);
    glTexCoord2f(u0, v1);
    glVertex2f(x0, y1);
}

static void ui_draw_gl(void* arg) {
    const UiDraw* d = (const UiDraw*)arg;
    int i, u;
    const float su = 8.0f / UI_TEX_W, sv = 16.0f / UI_TEX_H;
    /* the solid cell's middle, so filtering never reaches a glyph */
    const float wu = (15 * 8 + 4) / (float)UI_TEX_W, wv = (5 * 16 + 8) / (float)UI_TEX_H;
    static GLint nu;
    if (!nu) {
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &nu);
        nu = nu < 1 ? 1 : nu > 8 ? 8 : nu;
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    for (u = nu - 1; u >= 0; u--) {
        glActiveTexture(GL_TEXTURE0 + u);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_TEXTURE_GEN_R);
        glDisable(GL_TEXTURE_GEN_Q);
        glMatrixMode(GL_TEXTURE);
        glPushMatrix();
        glLoadIdentity();
    }
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, 640.0, 480.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, 640, 480);
    glDisable(0x8620); /* GL_VERTEX_PROGRAM_ARB */
    glDisable(0x8200); /* GL_TEXT_FRAGMENT_SHADER_ATI */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(0x8458); /* GL_COLOR_SUM */
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_FLAT);
    if (d->clear) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glEnable(GL_TEXTURE_2D);
    {
        /* the name is the port's own, far above both texture name ranges
         * (rt.c counts from 0x100000); a context made again (the fullscreen
         * switch) has lost it, and glIsTexture says so before the bind */
        static int built;
        int have = built && glIsTexture(UI_TEX_NAME);
        glBindTexture(GL_TEXTURE_2D, UI_TEX_NAME);
        if (!have) {
            if (!built) {
                ui_tex_build();
                built = 1;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, UI_TEX_W, UI_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui_tex_px);
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    for (i = 0; i < d->nrect; i++) {
        const UiRect* q = &d->rect[i];
        glColor4ub(q->c[0], q->c[1], q->c[2], q->c[3]);
        ui_quad(q->x0, q->y0, q->x1, q->y1, wu, wv, wu, wv);
    }
    for (i = 0; i < d->ntext; i++) {
        const UiText* t = &d->text[i];
        const char* s;
        float x = t->x, w = 8.0f * t->scale, h = 16.0f * t->scale;
        /* a one-pixel shadow first, then the letters */
        int pass;
        for (pass = 0; pass < 2; pass++) {
            x = t->x;
            if (pass == 0) {
                glColor4ub(0, 0, 0, 200);
            } else {
                glColor4ub(t->c[0], t->c[1], t->c[2], t->c[3]);
            }
            for (s = t->s; *s; s++, x += w) {
                int c = (unsigned char)*s;
                int k;
                float u0, v0, o = pass == 0 ? (float)t->scale : 0.0f;
                if (c < 33 || c > 126) {
                    continue;
                }
                k = c - 32;
                u0 = (float)(k % 16) * su;
                v0 = (float)(k / 16) * sv;
                ui_quad(x + o, t->y + o, x + w + o, t->y + h + o, u0, v0, u0 + su, v0 + sv);
            }
        }
    }
    glEnd();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    for (u = nu - 1; u >= 0; u--) {
        glActiveTexture(GL_TEXTURE0 + u);
        glMatrixMode(GL_TEXTURE);
        glPopMatrix();
    }
    glMatrixMode(GL_MODELVIEW);
    glPopClientAttrib();
    glPopAttrib();
}
#endif

static void ui_submit(void) {
#ifndef PORT_NO_SDL
    rt_call(ui_draw_gl, &ui_d, sizeof(ui_d), 0);
#endif
}

/* ---- the pages ------------------------------------------------------------ */
enum { PG_NONE, PG_MAIN, PG_OFFER, PG_CONFIRM, PG_RESULT,
       /* M51 (PLAN.md 66): Developer Mode */
       PG_DEV, PG_MARATHON, PG_MRESULT, PG_RECORD, PG_SOAK, PG_SOAKSAVED, PG_MCHILD };
enum { ACT_CLOSE = 1, ACT_BENCH_ASK, ACT_BENCH_GO, ACT_RESULT, ACT_BACK, ACT_OFFER_NO,
       ACT_DEV, ACT_MARATHON, ACT_MSTART, ACT_MRESUME, ACT_RECORD, ACT_REC_RESTART, ACT_REC_LATER, ACT_SOAK,
       ACT_SOAK_GO, ACT_SOAK_SAVE, ACT_OPT, ACT_MSTOP, ACT_DEVBACK };

#define UI_MAX_LINES 18
#define UI_MAX_ITEMS 13
static struct {
    int page;
    int cursor;
    int nlines, nitems;
    char title[48];
    char line[UI_MAX_LINES][UI_COLS + 1];
    unsigned color[UI_MAX_LINES];
    char item[UI_MAX_ITEMS][UI_COLS + 1];
    int act[UI_MAX_ITEMS];
    int* opt[UI_MAX_ITEMS];          /* M51: an option row's value (left/right), or NULL */
    int optn[UI_MAX_ITEMS];
    const char* (*optname[UI_MAX_ITEMS])(int);
    char foot[UI_COLS + 1];
} ui;
static int ui_allowed = -1;   /* the menu at all in this run */
static int ui_offer_due;      /* the first-launch offer still to show */
static u16 pad_prev;
static int pad_hold, pad_rep;
static unsigned ui_opened_at;
static int menu_done;         /* --menu has opened its page */

#define C_TEXT 0xE8E8E8u
#define C_DIM 0xA8B0C0u
#define C_TITLE 0xFFD84Au
#define C_GOOD 0x8CE88Cu
#define C_WARN 0xFFB060u

static void add_line(unsigned color, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_line(unsigned color, const char* fmt, ...) {
    va_list ap;
    char buf[512];
    const char* p;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* wrapped at the panel's width, on spaces */
    p = buf;
    do {
        size_t n = strlen(p), cut = n;
        if (ui.nlines >= UI_MAX_LINES) {
            return;
        }
        if (n > 68) {
            cut = 68;
            while (cut > 20 && p[cut] != ' ') {
                cut--;
            }
        }
        memcpy(ui.line[ui.nlines], p, cut);
        ui.line[ui.nlines][cut] = 0;
        ui.color[ui.nlines++] = color;
        p += cut;
        while (*p == ' ') {
            p++;
        }
    } while (*p);
}

static void add_item(int act, const char* s) {
    if (ui.nitems >= UI_MAX_ITEMS) {
        return;
    }
    snprintf(ui.item[ui.nitems], sizeof(ui.item[0]), "%s", s);
    ui.opt[ui.nitems] = NULL;
    ui.act[ui.nitems++] = act;
}

/* M51: an option row -- "Label:  < value >", left/right change it */
static void add_opt(const char* label, int* val, int n, const char* (*name)(int)) {
    if (ui.nitems >= UI_MAX_ITEMS) {
        return;
    }
    snprintf(ui.item[ui.nitems], sizeof(ui.item[0]), "%s", label);
    ui.opt[ui.nitems] = val;
    ui.optn[ui.nitems] = n;
    ui.optname[ui.nitems] = name;
    ui.act[ui.nitems++] = ACT_OPT;
}

static const char* cfg_or(const char* k, const char* dflt) {
    const char* v = port_opt.noconfig ? NULL : port_config_get(k);
    return v && *v ? v : dflt;
}

static void settings_lines(void) {
    char m[160];
    const char* lite = cfg_or("lite", "auto");
    const char* opts = cfg_or("liteopts", "");
    port_machine_line(m, sizeof(m));
    add_line(C_DIM, "This Mac: %s", m);
    add_line(C_DIM, "Now: Lite %s%s%s%s, water %s, movies %s, memory for loading %s%s", lite, *opts ? " (" : "",
             *opts ? opts : "", *opts ? ")" : "", cfg_or("water", "auto"), atoi(cfg_or("movies", "1")) ? "on" : "off",
             cfg_or("resident", "auto"), strcmp(cfg_or("resident", "auto"), "auto") ? " MB" : "");
}

/* ---- M51 (PLAN.md 66): Developer Mode's choices ---------------------------- */
const char* port_mg_name(int num);
const char* port_mg_kind(int num);
int port_mg_exists(int num);
int port_mg_type(int num);
int port_marathon_begin(const int* list, int n, int humans, const char* cast, int record);
int port_marathon_resume(void);
int port_marathon_progress(int* done, int* total);
void port_marathon_stop_request(void);
const char* port_marathon_result_path(void);
int port_marathon_child_banner(char* out, size_t n);
int port_handover(int nargs, const char* const* args);
void port_handover_tick(void);
int port_session_recording(void);
const char* port_session_record_path(void);
void port_session_close(void);
const char* port_soakplan_path(void);

static int mg_all[64], mg_nall;
static int dm_which, dm_from, dm_to = -1, dm_humans, dm_char[4] = {0, 1, 2, 3}, dm_record;
static int sk_board = 6, sk_turns = 1, sk_mg, sk_lite, sk_water, sk_snap, sk_len = 2;
static char dm_note[UI_COLS + 1];

static void mg_all_fill(void) {
    int n;
    if (mg_nall) {
        return;
    }
    for (n = 401; n <= 463; n++) {
        if (port_mg_exists(n)) {
            mg_all[mg_nall++] = n;
        }
    }
    if (dm_to < 0) {
        dm_to = mg_nall - 1;
    }
}

static const char* CHAR_NAMES[8] = {"Mario", "Luigi", "Peach", "Yoshi", "Wario", "Donkey Kong", "Daisy", "Waluigi"};
static const char* CHAR_ARGS[8] = {"mario", "luigi", "peach", "yoshi", "wario", "donkey", "daisy", "waluigi"};
static const char* nm_which(int v) {
    static const char* const w[] = {"all of them", "a range (below)", "4-player", "1-vs-3", "2-vs-2",
                                    "Battle",      "Bowser, item, story and extra", "my list (marathon-list.txt)"};
    return v >= 0 && v < 8 ? w[v] : "?";
}
static const char* nm_mg(int i) {
    static char b[2][64];
    static int k;
    k ^= 1;
    if (i < 0 || i >= mg_nall) {
        return "?";
    }
    snprintf(b[k], sizeof(b[k]), "m%d %s", mg_all[i], port_mg_name(mg_all[i]));
    return b[k];
}
static const char* nm_humans(int v) {
    static const char* const h[] = {"1 (the rest COM)", "2 (the rest COM)", "3 (the rest COM)", "4"};
    return v >= 0 && v < 4 ? h[v] : "?";
}
static const char* nm_char(int v) { return v >= 0 && v < 8 ? CHAR_NAMES[v] : "?"; }
static const char* nm_onoff(int v) { return v ? "on" : "off"; }
static const char* nm_board(int v) {
    static const char* const b[] = {"Toad's Midway Madness", "Goomba's Greedy Gala", "Shy Guy's Jungle Jam",
                                    "Boo's Haunted Bash",    "Koopa's Seaside Soiree", "Bowser's Gnarly Party",
                                    "every board in turn"};
    return v >= 0 && v < 7 ? b[v] : "?";
}
static const int SK_TURNS[] = {10, 20, 30, 50};
static const char* nm_turns(int v) {
    static char b[16];
    snprintf(b, sizeof(b), "%d", SK_TURNS[v & 3]);
    return b;
}
static const char* nm_skmg(int v) { return v ? "the marathon's selection (in turn)" : "the roulette's"; }
static const char* nm_lite(int v) { return v == 1 ? "on" : v == 2 ? "off" : "auto"; }
static const char* nm_water(int v) {
    static const char* const w[] = {"auto", "off", "cheap", "full"};
    return w[v & 3];
}
static const int SK_SNAP[] = {0, 5000, 20000};
static const char* nm_snap(int v) { return v == 1 ? "every 5,000 frames" : v == 2 ? "every 20,000 frames" : "off"; }
static const int SK_LEN[] = {30, 60, 120, 240, 480, 0};
static const char* nm_len(int v) {
    static const char* const l[] = {"30 minutes", "1 hour", "2 hours", "4 hours", "8 hours", "until stopped"};
    return v >= 0 && v < 6 ? l[v] : "?";
}

/* the marathon's minigames from the choices */
static int dm_list(int* out) {
    int i, n = 0;
    mg_all_fill();
    if (dm_which == 7) {
        char path[1100], buf[2048];
        FILE* f;
        snprintf(path, sizeof(path), "%s/marathon-list.txt", port_app_support_dir());
        f = fopen(path, "r");
        if (f) {
            size_t k = fread(buf, 1, sizeof(buf) - 1, f);
            char* t;
            buf[k] = 0;
            fclose(f);
            for (t = strtok(buf, ", \n\r\tm"); t && n < 64; t = strtok(NULL, ", \n\r\tm")) {
                int num = atoi(t);
                if (port_mg_exists(num)) {
                    out[n++] = num;
                }
            }
        }
        return n;
    }
    for (i = 0; i < mg_nall; i++) {
        int t = port_mg_type(mg_all[i]);
        int take = dm_which == 0 || (dm_which == 1 && i >= dm_from && i <= dm_to) || (dm_which == 2 && t == 0) ||
                   (dm_which == 3 && t == 1) || (dm_which == 4 && t == 2) || (dm_which == 5 && t == 4) ||
                   (dm_which == 6 && (t == 3 || t >= 5));
        if (take) {
            out[n++] = mg_all[i];
        }
    }
    return n;
}

static void page_set(int pg);
static void dev_marathon_lines(void) {
    int list[64], n = dm_list(list), d, t;
    if (n) {
        add_line(C_TEXT, "%d minigame%s: m%d %s ... m%d %s", n, n > 1 ? "s" : "", list[0], port_mg_name(list[0]),
                 list[n - 1], port_mg_name(list[n - 1]));
    } else {
        add_line(C_WARN, "No minigames chosen%s", dm_which == 7 ? " (marathon-list.txt is missing or empty)" : "");
    }
    add_line(C_DIM, "Each one: the game runs to its instruction card by itself, you play, the next loads.");
    if (port_marathon_progress(&d, &t)) {
        add_line(C_GOOD, "A marathon was stopped after %d of %d -- Resume picks it up.", d, t);
    }
}

static void page_set(int pg) {
    memset(&ui, 0, sizeof(ui));
    ui.page = pg;
    ui_opened_at = gl13_frame_number();
    switch (pg) {
        case PG_MAIN:
            snprintf(ui.title, sizeof(ui.title), "Mario Party 4 - %s", PORT_VERSION_STRING);
            settings_lines();
            add_line(C_TEXT, " ");
            add_line(C_TEXT, "Benchmark Mode plays five short scenes by itself (about five minutes), "
                             "measures them and picks this Mac's settings: Lite mode, the water, "
                             "the movies and the memory it keeps.");
            add_item(ACT_BENCH_ASK, "Run Benchmark Mode");
            {
                FILE* f = fopen(port_bench_result_path(), "r");
                if (f) {
                    fclose(f);
                    add_item(ACT_RESULT, "Show the last result");
                }
            }
            add_item(ACT_DEV, "Developer Mode");
            add_item(ACT_CLOSE, "Back to the game");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select - B or Esc back");
            break;
        case PG_DEV:
            snprintf(ui.title, sizeof(ui.title), "Developer Mode");
            add_line(C_TEXT, "The playtest rig: play every minigame one after another, record what "
                             "you play, plan a soak.");
            add_line(port_session_recording() ? C_GOOD : C_DIM, "Recording: %s",
                     port_session_recording() ? "on -- this session is being recorded"
                     : (port_config_get("record") && atoi(port_config_get("record"))) ? "on from the next launch"
                                                                                     : "off");
            if (dm_note[0]) {
                add_line(C_GOOD, "%s", dm_note);
                dm_note[0] = 0;
            }
            add_item(ACT_MARATHON, "Minigame marathon");
            add_item(ACT_RECORD, (port_session_recording() || (port_config_get("record") &&
                                                               atoi(port_config_get("record"))))
                                     ? "Record this session: ON  (select to turn off)"
                                     : "Record this session: OFF (select to turn on)");
            add_item(ACT_SOAK, "Soak planner");
            add_item(ACT_BACK, "Back");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select - B back");
            break;
        case PG_MARATHON: {
            int d, t;
            mg_all_fill();
            snprintf(ui.title, sizeof(ui.title), "Minigame marathon");
            dev_marathon_lines();
            add_opt("Minigames", &dm_which, 8, nm_which);
            if (dm_which == 1) {
                add_opt("From", &dm_from, mg_nall, nm_mg);
                add_opt("To", &dm_to, mg_nall, nm_mg);
            }
            add_opt("Players at the controllers", &dm_humans, 4, nm_humans);
            add_opt("Player 1", &dm_char[0], 8, nm_char);
            add_opt("Player 2", &dm_char[1], 8, nm_char);
            add_opt("Player 3", &dm_char[2], 8, nm_char);
            add_opt("Player 4", &dm_char[3], 8, nm_char);
            add_opt("Record each minigame", &dm_record, 2, nm_onoff);
            add_item(ACT_MSTART, "Start the marathon");
            if (port_marathon_progress(&d, &t)) {
                add_item(ACT_MRESUME, "Resume the stopped marathon");
            }
            add_item(ACT_DEVBACK, "Back");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - Left/Right change - A select - B back");
            break;
        }
        case PG_RECORD:
            snprintf(ui.title, sizeof(ui.title), "Record this session");
            add_line(C_TEXT, "Recording is on. A recording starts at the boot -- the start state and "
                             "every controller's every frame -- so it can be played back exactly, "
                             "on this Mac or in Dolphin.");
            add_line(C_DIM, "Recordings go to Documents/MarioParty4 Recordings (a few MB an hour).");
            add_line(C_DIM, "Every launch records until you turn it off here.");
            add_item(ACT_REC_RESTART, "Restart now and record");
            add_item(ACT_REC_LATER, "Keep playing (record from the next launch)");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select");
            break;
        case PG_SOAK:
            snprintf(ui.title, sizeof(ui.title), "Soak planner");
            add_line(C_TEXT, "The game plays itself (four COMs, no controller needed) and logs every "
                             "fault, stall and frame rate. The plan is saved as soak-plan.txt, which "
                             "the lab's chain tools read too.");
            add_opt("Board", &sk_board, 7, nm_board);
            add_opt("Turns", &sk_turns, 4, nm_turns);
            add_opt("Minigames", &sk_mg, 2, nm_skmg);
            add_opt("Lite", &sk_lite, 3, nm_lite);
            add_opt("Water", &sk_water, 4, nm_water);
            add_opt("Snapshots", &sk_snap, 3, nm_snap);
            add_opt("Length", &sk_len, 6, nm_len);
            add_item(ACT_SOAK_GO, "Start the soak");
            add_item(ACT_SOAK_SAVE, "Save the plan only");
            add_item(ACT_DEVBACK, "Back");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - Left/Right change - A select - B back");
            break;
        case PG_SOAKSAVED:
            snprintf(ui.title, sizeof(ui.title), "Soak planner");
            add_line(C_GOOD, "The plan is saved:");
            add_line(C_DIM, "%s", port_soakplan_path());
            add_line(C_TEXT, "Start it later from here, or on the command line: isle --soakplan FILE");
            add_item(ACT_DEVBACK, "OK");
            break;
        case PG_MCHILD: {
            char b[160];
            snprintf(ui.title, sizeof(ui.title), "Minigame marathon");
            (void)b;
            if (port_opt.marathonchild) {
                add_line(C_TEXT, "Now playing %s", port_opt.marathonchild);
            }
            add_line(C_DIM, "Stopping keeps the results so far; Developer Mode resumes from the next one.");
            add_item(ACT_CLOSE, "Back to the minigame");
            add_item(ACT_MSTOP, "Stop the marathon here");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select - B back");
            break;
        }
        case PG_OFFER:
            snprintf(ui.title, sizeof(ui.title), "Welcome!");
            add_line(C_TEXT, "Mario Party 4 can measure this Mac and choose its settings for you.");
            add_line(C_TEXT, " ");
            add_line(C_TEXT, "Benchmark Mode plays five short scenes by itself for about five "
                             "minutes, then opens the game again with the settings it picked.");
            add_line(C_TEXT, " ");
            add_line(C_DIM, "You can run it any time later: press F1 or M.");
            add_item(ACT_BENCH_ASK, "Run Benchmark Mode now");
            add_item(ACT_OFFER_NO, "Not now");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select");
            break;
        case PG_CONFIRM:
            snprintf(ui.title, sizeof(ui.title), "Benchmark Mode");
            add_line(C_TEXT, "The game will close and five scenes will play by themselves, "
                             "each in its own window:");
            add_line(C_DIM, "  1. the opening movie   2. Toad's Midway Madness");
            add_line(C_DIM, "  3-4. Butterfly Blitz (Lite off, then on if needed)");
            add_line(C_DIM, "  5. Makin' Waves (the water)");
            add_line(C_TEXT, " ");
            add_line(C_TEXT, "Please don't touch the controls for about five minutes. "
                             "Mario Party 4 opens again with the result, and a report is "
                             "saved on your Desktop.");
            add_line(C_DIM, "Your memory card is not touched.");
            add_item(ACT_BENCH_GO, "Start");
            add_item(ACT_BACK, "Cancel");
            snprintf(ui.foot, sizeof(ui.foot), "Up/Down choose - A or Return select - B back");
            break;
        case PG_MRESULT:
        case PG_RESULT: {
            FILE* f = fopen(pg == PG_MRESULT ? (port_opt.marathonresult ? port_opt.marathonresult
                                                                        : port_marathon_result_path())
                            : port_opt.benchresult ? port_opt.benchresult : port_bench_result_path(), "r");
            char buf[256];
            snprintf(ui.title, sizeof(ui.title), pg == PG_MRESULT ? "Minigame marathon: the summary"
                                                                 : "Benchmark Mode: the result");
            if (!f) {
                add_line(C_WARN, "No result found.");
            } else {
                while (fgets(buf, sizeof(buf), f)) {
                    unsigned c = C_TEXT;
                    buf[strcspn(buf, "\r\n")] = 0;
                    if (!strncmp(buf, "! ", 2)) {
                        c = C_WARN;
                        memmove(buf, buf + 2, strlen(buf + 2) + 1);
                    } else if (!strncmp(buf, "+ ", 2)) {
                        c = C_GOOD;
                        memmove(buf, buf + 2, strlen(buf + 2) + 1);
                    } else if (!strncmp(buf, "  ", 2)) {
                        c = C_DIM;
                    }
                    add_line(c, "%s", buf[0] ? buf : " ");
                }
                fclose(f);
            }
            add_item(ACT_CLOSE, "OK");
            snprintf(ui.foot, sizeof(ui.foot), "A or Return - F1 or M opens the menu again");
            break;
        }
        default:
            break;
    }
}

static void ui_close(void) {
    if (ui.page == PG_OFFER && !port_opt.noconfig) {
        port_config_set("benchoffer", "shown");
        port_config_save();
    }
    ui.page = PG_NONE;
    port_log("port> menu (M50): closed\n");
}

static void ui_act(int act) {
    port_log("port> menu (M50): page %d, item %d\n", ui.page, act);
    switch (act) {
        case ACT_CLOSE:
            ui_close();
            break;
        case ACT_OFFER_NO:
            ui_close();
            break;
        case ACT_BENCH_ASK:
            if (ui.page == PG_OFFER && !port_opt.noconfig) {
                port_config_set("benchoffer", "shown");
                port_config_save();
            }
            page_set(PG_CONFIRM);
            break;
        case ACT_BENCH_GO:
            if (port_bench_start()) {
                page_set(PG_NONE);
            }
            break;
        case ACT_RESULT:
            page_set(PG_RESULT);
            break;
        case ACT_BACK:
            page_set(PG_MAIN);
            break;
        /* ---- M51 (PLAN.md 66): Developer Mode ---- */
        case ACT_DEV:
        case ACT_DEVBACK:
            page_set(PG_DEV);
            break;
        case ACT_MARATHON:
            page_set(PG_MARATHON);
            break;
        case ACT_OPT:
            if (ui.opt[ui.cursor]) {
                int c = ui.cursor;
                *ui.opt[ui.cursor] = (*ui.opt[ui.cursor] + 1) % ui.optn[ui.cursor];
                page_set(ui.page);
                ui.cursor = c < ui.nitems ? c : ui.nitems - 1;
            }
            break;
        case ACT_MSTART: {
            int list[64], n = dm_list(list);
            char cast[96];
            snprintf(cast, sizeof(cast), "%s,%s,%s,%s", CHAR_ARGS[dm_char[0]], CHAR_ARGS[dm_char[1]],
                     CHAR_ARGS[dm_char[2]], CHAR_ARGS[dm_char[3]]);
            if (n && port_marathon_begin(list, n, dm_humans + 1, cast, dm_record)) {
                page_set(PG_NONE);
            }
            break;
        }
        case ACT_MRESUME:
            if (port_marathon_resume()) {
                page_set(PG_NONE);
            }
            break;
        case ACT_MSTOP:
            page_set(PG_NONE);
            port_marathon_stop_request();
            break;
        case ACT_RECORD:
            if (port_session_recording() || (port_config_get("record") && atoi(port_config_get("record")))) {
                const char* rp = port_session_record_path();
                const char* slash = rp ? strrchr(rp, '/') : NULL;
                if (!port_opt.noconfig) {
                    port_config_set("record", "0");
                    port_config_save();
                }
                if (port_session_recording()) {
                    snprintf(dm_note, sizeof(dm_note), "Recording off; kept %.50s", slash ? slash + 1 : "");
                    port_session_close();
                } else {
                    snprintf(dm_note, sizeof(dm_note), "Recording off.");
                }
                page_set(PG_DEV);
            } else {
                if (!port_opt.noconfig) {
                    port_config_set("record", "1");
                    port_config_save();
                }
                page_set(PG_RECORD);
            }
            break;
        case ACT_REC_RESTART: {
            const char* none[1] = {"--nomenu"};
            (void)none;
            page_set(PG_NONE);
            port_handover(0, none);
            break;
        }
        case ACT_REC_LATER:
            snprintf(dm_note, sizeof(dm_note), "Recording from the next launch.");
            page_set(PG_DEV);
            break;
        case ACT_SOAK:
            page_set(PG_SOAK);
            break;
        case ACT_SOAK_GO:
        case ACT_SOAK_SAVE: {
            FILE* f = fopen(port_soakplan_path(), "w");
            if (f) {
                int list[64], n = 0, i;
                fprintf(f, "# Mario Party 4 PowerPC Edition -- a soak plan (Developer Mode, M51; PLAN.md 66.1)\n");
                fprintf(f, "# isle --soakplan THIS_FILE plays it; tools/m51_soakplan.sh reads it too\n");
                if (sk_board == 6) {
                    fprintf(f, "boards = 1+\n");
                } else {
                    fprintf(f, "boards = %d\n", sk_board + 1);
                }
                fprintf(f, "turns = %d\n", SK_TURNS[sk_turns & 3]);
                if (sk_mg) {
                    n = dm_list(list);
                }
                fprintf(f, "minigames = ");
                if (!n) {
                    fprintf(f, "all");
                }
                for (i = 0; i < n && i < 16; i++) {
                    fprintf(f, "%s%d", i ? "," : "", list[i]);
                }
                fprintf(f, "\nlite = %s\nwater = %s\nsnapshots = %d\nminutes = %d\n", nm_lite(sk_lite),
                        nm_water(sk_water), SK_SNAP[sk_snap % 3], SK_LEN[sk_len % 6]);
                fclose(f);
            }
            if (act == ACT_SOAK_GO && f) {
                const char* args[2];
                args[0] = "--soakplan";
                args[1] = port_soakplan_path();
                page_set(PG_NONE);
                port_handover(2, args);
            } else {
                page_set(PG_SOAKSAVED);
            }
            break;
        }
        default:
            break;
    }
}

/* B / Esc: one page up */
static void ui_back(void) {
    switch (ui.page) {
        case PG_CONFIRM:
        case PG_RESULT:
        case PG_DEV:
            page_set(PG_MAIN);
            break;
        case PG_MARATHON:
        case PG_RECORD:
        case PG_SOAK:
        case PG_SOAKSAVED:
            page_set(PG_DEV);
            break;
        default:
            ui_close();
            break;
    }
}

/* ---- the switches ---------------------------------------------------------- */
static int ui_scripted(void) {
    return port_opt.pad_play || port_opt.pad_record || port_opt.soak || port_opt.noconfig || port_opt.com4 ||
           port_opt.nopad;
}

static int allowed(void) {
    if (ui_allowed < 0) {
        ui_allowed = !port_opt.nomenu && !port_opt.benchchild && !port_opt.headless && !port_opt.replay &&
                     (!ui_scripted() || port_opt.menu || port_opt.benchresult || port_opt.marathonchild ||
                      port_opt.marathonresult);
        ui_offer_due = ui_allowed && !ui_scripted() && !port_opt.benchresult && !port_opt.marathonresult &&
                       !port_opt.marathonchild && !port_opt.session_record && !port_config_get("benchoffer");
        if (port_opt.benchresult) {
            page_set(PG_RESULT);
        }
        if (port_opt.marathonresult) {
            page_set(PG_MRESULT); /* M51: the marathon's summary */
        }
    }
    return ui_allowed;
}

int port_ui_open(void) { return ui.page != PG_NONE; }

/* the SDL event loop's key (gl13.c): 1 = the menu took it */
int port_ui_key(int sym) {
#ifndef PORT_NO_SDL
    if (!allowed()) {
        return 0;
    }
    if (sym == SDLK_F1 || sym == SDLK_m) {
        if (ui.page == PG_NONE) {
            page_set(port_opt.marathonchild ? PG_MCHILD : PG_MAIN);
            port_log("port> menu (M50): opened (%s)\n", sym == SDLK_F1 ? "F1" : "M");
        } else {
            ui_close();
        }
        return 1;
    }
    if (ui.page != PG_NONE && sym == SDLK_ESCAPE) {
        ui_back();
        return 1;
    }
#else
    (void)sym;
#endif
    return 0;
}

/* PADRead's four slots after they are filled: the menu reads them, and while
 * it is open the game reads them at rest */
void port_ui_pad(void* status4) {
    PADStatus* st = (PADStatus*)status4;
    u16 b = 0, edge;
    int i, up = 0, down = 0;
    int lr_seen[PAD_CHANMAX] = {0, 0, 0, 0};
    if (ui.page == PG_NONE) {
        return;
    }
    for (i = 0; i < PAD_CHANMAX; i++) {
        if (st[i].err != PAD_ERR_NONE) {
            continue;
        }
        b |= st[i].button;
        lr_seen[i] = st[i].stickX;
        if (st[i].stickY > 48) {
            up = 1;
        } else if (st[i].stickY < -48) {
            down = 1;
        }
        st[i].button = 0;
        st[i].stickX = st[i].stickY = 0;
        st[i].substickX = st[i].substickY = 0;
        st[i].triggerL = st[i].triggerR = 0;
    }
    if (up) {
        b |= PAD_BUTTON_UP;
    }
    if (down) {
        b |= PAD_BUTTON_DOWN;
    }
    for (i = 0; i < PAD_CHANMAX; i++) {
        /* M51: left/right for the option rows (read before the slots were zeroed) */
        if (lr_seen[i] > 48) {
            b |= PAD_BUTTON_RIGHT;
        } else if (lr_seen[i] < -48) {
            b |= PAD_BUTTON_LEFT;
        }
    }
    edge = (u16)(b & ~pad_prev);
    pad_prev = b;
    /* a quarter of a second after opening, so the key that opened it does not select */
    if (gl13_frame_number() < ui_opened_at + 8) {
        return;
    }
    if (b & (PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
        /* held: repeat after 24 retraces, every 8 */
        if (edge & (PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
            pad_hold = 0;
            pad_rep = 1;
        } else if (++pad_hold > 24 && (pad_hold % 8) == 0) {
            pad_rep = 1;
        }
    }
    if (pad_rep && ui.nitems) {
        pad_rep = 0;
        if (b & PAD_BUTTON_UP) {
            ui.cursor = (ui.cursor + ui.nitems - 1) % ui.nitems;
        } else if (b & PAD_BUTTON_DOWN) {
            ui.cursor = (ui.cursor + 1) % ui.nitems;
        }
    }
    if ((edge & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) && ui.nitems && ui.opt[ui.cursor]) {
        /* M51: an option row */
        int c = ui.cursor, n = ui.optn[c];
        *ui.opt[c] = (*ui.opt[c] + ((edge & PAD_BUTTON_RIGHT) ? 1 : n - 1)) % n;
        page_set(ui.page);
        ui.cursor = c < ui.nitems ? c : ui.nitems - 1;
    } else if ((edge & (PAD_BUTTON_A | PAD_BUTTON_START)) && ui.nitems) {
        ui_act(ui.act[ui.cursor]);
    } else if (edge & PAD_BUTTON_B) {
        ui_back();
    }
}

/* ---- each presented frame (gl13_present, the game thread) ------------------ */
static void ui_layout(void) {
    int i, y, x0 = 36, x1 = 604, top, h;
    int ih = ui.nitems > 9 ? 21 : 24; /* M51: the Developer Mode pages' longer lists */
    h = 70 + ui.nlines * 17 + 14 + ui.nitems * ih + 34;
    top = (480 - h) / 2;
    if (top < 8) {
        top = 8;
    }
    d_rect(0, 0, 640, 480, 0, 0, 0, 90);
    d_rect(x0, top, x1, top + h, 16, 22, 46, 246);
    d_rect(x0, top, x1, top + 3, 255, 216, 74, 255);
    d_rect(x0, top + h - 3, x1, top + h, 255, 216, 74, 255);
    d_text(x0 + 16, top + 14, 2, C_TITLE, ui.title);
    y = top + 60;
    for (i = 0; i < ui.nlines; i++, y += 17) {
        d_text(x0 + 18, y, 1, ui.color[i], ui.line[i]);
    }
    y += 14;
    for (i = 0; i < ui.nitems; i++, y += ih) {
        char s[UI_COLS + 4];
        if (i == ui.cursor) {
            d_rect(x0 + 12, y - 3, x1 - 12, y + ih - 4, 60, 90, 170, 255);
        }
        if (ui.opt[i]) {
            snprintf(s, sizeof(s), "%s %-26s < %s >", i == ui.cursor ? ">" : " ", ui.item[i],
                     ui.optname[i](*ui.opt[i]));
        } else {
            snprintf(s, sizeof(s), "%s %s", i == ui.cursor ? ">" : " ", ui.item[i]);
        }
        d_text(x0 + 20, y, 1, i == ui.cursor ? 0xFFFFFFu : C_TEXT, s);
    }
    d_text(x0 + 18, top + h - 24, 1, C_DIM, ui.foot);
}

void port_bench_tick(void);
void port_ui_frame(void) {
    char banner[160];
    unsigned f = gl13_frame_number();
    port_bench_tick();
    port_handover_tick();
    memset(&ui_d, 0, offsetof(UiDraw, rect));
    if (port_opt.benchchild) {
        /* Benchmark Mode's child run: one strip at the top */
        if (port_bench_child_banner(banner, sizeof(banner))) {
            d_rect(0, 0, 640, 20, 0, 0, 0, 150);
            d_text(6, 2, 1, C_TITLE, banner);
            ui_submit();
        }
        return;
    }
    if (!allowed()) {
        return;
    }
    if (port_opt.menu && ui.page == PG_NONE && !menu_done) {
        unsigned at = (unsigned)atoi(port_opt.menu);
        const char* pg = strpbrk(port_opt.menu, ":/"); /* N:PAGE or N/PAGE (the chain splits on ':') */
        /* at the first presented frame from N (the count can step over N:
         * frames that were only consumed) */
        if (f >= (at ? at : 1)) {
            menu_done = 1;
            if (pg && !strcmp(pg + 1, "start")) {
                /* the lab's: Benchmark Mode's Start pressed at frame N (the
                 * whole hand-over, game -> driver -> game) */
                port_log("port> menu (M50): --menu %s: Start\n", port_opt.menu);
                page_set(PG_CONFIRM);
                ui_act(ACT_BENCH_GO);
                return;
            }
            page_set(!pg                         ? PG_MAIN
                     : !strcmp(pg + 1, "offer")    ? PG_OFFER
                     : !strcmp(pg + 1, "confirm")  ? PG_CONFIRM
                     : !strcmp(pg + 1, "result")   ? PG_RESULT
                     : !strcmp(pg + 1, "dev")      ? PG_DEV      /* M51 */
                     : !strcmp(pg + 1, "marathon") ? PG_MARATHON
                     : !strcmp(pg + 1, "record")   ? PG_RECORD
                     : !strcmp(pg + 1, "soak")     ? PG_SOAK
                     : !strcmp(pg + 1, "mresult")  ? PG_MRESULT
                     : !strcmp(pg + 1, "mchild")   ? PG_MCHILD
                                                   : PG_MAIN);
            if (ui.page == PG_MARATHON || ui.page == PG_SOAK) {
                ui.cursor = 1; /* a row with its value, for the picture */
            }
            port_log("port> menu (M50): --menu opened page %d at drawn frame %u\n", ui.page, f);
        }
    }
    if (ui_offer_due && ui.page == PG_NONE && f >= 420) {
        ui_offer_due = 0;
        page_set(PG_OFFER);
        port_log("port> menu (M50): the first-launch offer of Benchmark Mode\n");
    }
    if (ui.page == PG_NONE) {
        return;
    }
    ui_layout();
    ui_submit();
}

/* --ffto's frames are not drawn: Benchmark Mode's child shows its banner on a
 * black screen now and then while it fast-forwards (gl13.c) */
int port_ui_ffto_frame(unsigned frame) {
    char banner[160];
    if (port_opt.marathonchild && (frame % 60u) == 1u && port_marathon_child_banner(banner, sizeof(banner))) {
        /* M51: the marathon's child on its way to the minigame */
        unsigned left = frame < (unsigned)port_opt.ffto ? ((unsigned)port_opt.ffto - frame) / 150u : 0u;
        char eta[64];
        memset(&ui_d, 0, offsetof(UiDraw, rect));
        ui_d.clear = 1;
        d_text(40, 180, 2, C_TITLE, "Minigame marathon");
        d_text(40, 230, 1, C_TEXT, banner);
        snprintf(eta, sizeof(eta), "Getting to the minigame -- about %u s.", left);
        d_text(40, 256, 1, C_DIM, eta);
        d_text(40, 282, 1, C_DIM, "Get ready: press START on the instruction card. F1 or M: the menu.");
        ui_submit();
        return 1;
    }
    if (!port_opt.benchchild || (frame % 60u) != 1u || !port_bench_child_banner(banner, sizeof(banner))) {
        return 0;
    }
    memset(&ui_d, 0, offsetof(UiDraw, rect));
    ui_d.clear = 1;
    d_text(40, 200, 2, C_TITLE, "Benchmark Mode");
    d_text(40, 250, 1, C_TEXT, banner);
    d_text(40, 276, 1, C_DIM, "Getting to the scene (the game runs ahead without drawing)...");
    d_text(40, 302, 1, C_DIM, "Please don't touch the controls.");
    ui_submit();
    return 1;
}
