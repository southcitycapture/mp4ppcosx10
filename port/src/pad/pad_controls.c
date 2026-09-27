/* M46 (PLAN.md 61): PowerPCube's controls file.
 *
 *   ~/Library/Application Support/PowerPCube/controls/mp4.txt   key = value
 *   ~/Library/Application Support/PowerPCube/gamecontrollerdb.txt   SDL mappings
 *
 *   player1..player4 = auto | keyboard | pad:N | none
 *       auto: today's layout (the Xbox One pads, then SDL's, then the keyboard
 *       on the first free port or beside controller 1); keyboard: that port is
 *       the keyboard (what --kbport did); pad:N: the Nth pad in that same
 *       Xbox-first order (1 = the first); none: unplugged, a CPU player.
 *   pad.<id> = SDL game controller names, comma-separated: a b x y back guide
 *       start leftstick rightstick leftshoulder rightshoulder dpup dpdown
 *       dpleft dpright, the axes leftx lefty rightx righty lefttrigger
 *       righttrigger (an axis with + or - is one direction: righty- = the
 *       right stick up); for stick / cstick: left, right or none (a whole
 *       stick)
 *   key.<id> = SDL key names, comma-separated (SDL_GetScancodeFromName)
 *
 * ids: a b x y z l r start dup ddown dleft dright stick cstick, and for keys
 * stick_up/_down/_left/_right and cstick_up/_down/_left/_right.  The table's
 * defaults are the mapping pad_sdl.c and pad_xone.c had before this file
 * (the Read Me's), so a missing file, or a missing line, is today's mapping
 * exactly -- and with no file the old code paths run, untouched.  L and R
 * stay analog: the trigger's axis when one is mapped (255 for a button or a
 * key), the click bit past 200 as before.
 *
 * A scripted run -- --play, --record, --soak, --noconfig, --com4, --nopad --
 * reads neither file, so every golden and md5 walk stays what it was.  The
 * resolved table is logged at the start of an interactive run. */
#include "pad_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#endif

enum { ID_A, ID_B, ID_X, ID_Y, ID_Z, ID_L, ID_R, ID_START, ID_DUP, ID_DDOWN, ID_DLEFT, ID_DRIGHT,
       ID_STICK, ID_CSTICK, ID_SU, ID_SD, ID_SL, ID_SR, ID_CU, ID_CD, ID_CL, ID_CR, ID_N };

static const char* const id_name[ID_N] = {
    "a", "b", "x", "y", "z", "l", "r", "start", "dup", "ddown", "dleft", "dright",
    "stick", "cstick", "stick_up", "stick_down", "stick_left", "stick_right",
    "cstick_up", "cstick_down", "cstick_left", "cstick_right"};

/* today's mapping (pad_sdl.c, pad_xone.c, the Read Me) */
static const char* const def_pad[ID_N] = {
    "a", "b", "x", "y", "leftshoulder", "lefttrigger", "righttrigger", "start",
    "dpup", "dpdown", "dpleft", "dpright", "left", "right", "", "", "", "", "", "", "", ""};
static const char* const def_key[ID_N] = {
    "Z", "X", "C", "V", "Left Shift,Right Shift,Tab", "Q", "E", "Return,Keypad Enter",
    "T", "G", "F", "H", "", "", "Up,W", "Down,S", "Left,A", "Right,D", "I", "K", "J", "L"};

#define MAXIN 6
typedef struct {
    signed char kind[MAXIN]; /* 1 button, 2 axis (dir in sign), 0 end */
    signed char code[MAXIN]; /* PadIn button / axis */
    signed char dir[MAXIN];  /* axis: +1 / -1 / 0 (a trigger, whole) */
    int n;
} PadBind;

static const char* const btn_name[PADIN_NBUTTONS] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
static const char* const axis_name[PADIN_NAXES] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};

static int active;          /* a file was read: the table decides */
static int scripted;
static char pad_txt[ID_N][96], key_txt[ID_N][96];
static PadBind pbind[ID_N];
static int kcode[ID_N][MAXIN], kn[ID_N];
static int stick_src = 0, cstick_src = 1; /* 0 left, 1 right, -1 none */
static int player[PAD_CHANMAX];            /* PADCTL_* */

static void trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') {
        memmove(s, s + 1, strlen(s));
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
}

static int find_id(const char* s) {
    int i;
    for (i = 0; i < ID_N; i++) {
        if (!strcmp(s, id_name[i])) {
            return i;
        }
    }
    return -1;
}

static void parse_pad(int id, const char* txt) {
    char buf[96], *tok, *save = NULL;
    PadBind* b = &pbind[id];
    memset(b, 0, sizeof(*b));
    if (id == ID_STICK || id == ID_CSTICK) {
        int* s = id == ID_STICK ? &stick_src : &cstick_src;
        *s = !strcmp(txt, "left") ? 0 : !strcmp(txt, "right") ? 1 : -1;
        return;
    }
    snprintf(buf, sizeof(buf), "%s", txt);
    for (tok = strtok_r(buf, ",", &save); tok && b->n < MAXIN; tok = strtok_r(NULL, ",", &save)) {
        int k, dir = 0;
        size_t L;
        trim(tok);
        L = strlen(tok);
        if (L > 1 && (tok[L - 1] == '+' || tok[L - 1] == '-')) {
            dir = tok[L - 1] == '+' ? 1 : -1;
            tok[L - 1] = 0;
        }
        for (k = 0; k < PADIN_NBUTTONS; k++) {
            if (!strcmp(tok, btn_name[k])) {
                b->kind[b->n] = 1;
                b->code[b->n] = (signed char)k;
                b->dir[b->n++] = 0;
                break;
            }
        }
        if (k < PADIN_NBUTTONS) {
            continue;
        }
        for (k = 0; k < PADIN_NAXES; k++) {
            if (!strcmp(tok, axis_name[k])) {
                /* a stick axis without a sign is its + direction */
                if (!dir && k < PADIN_AX_LT) {
                    dir = 1;
                }
                b->kind[b->n] = 2;
                b->code[b->n] = (signed char)k;
                b->dir[b->n++] = (signed char)dir;
                break;
            }
        }
        if (k == PADIN_NAXES) {
            port_log("port> controls: pad.%s: \"%s\" is not an SDL controller name; ignored\n",
                     id_name[id], tok);
        }
    }
}

static void parse_keys(int id, const char* txt) {
    char buf[96], *tok, *save = NULL;
    kn[id] = 0;
#ifndef PORT_NO_SDL
    snprintf(buf, sizeof(buf), "%s", txt);
    for (tok = strtok_r(buf, ",", &save); tok && kn[id] < MAXIN; tok = strtok_r(NULL, ",", &save)) {
        SDL_Scancode sc;
        trim(tok);
        if (!*tok) {
            continue;
        }
        sc = SDL_GetScancodeFromName(tok);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            port_log("port> controls: key.%s: \"%s\" is not an SDL key name; ignored\n", id_name[id], tok);
            continue;
        }
        kcode[id][kn[id]++] = (int)sc;
    }
#else
    (void)buf; (void)tok; (void)save; (void)txt;
#endif
}

static const char* ppc_dir(char* out, size_t n, const char* leaf) {
    const char* home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PowerPCube/%s", home ? home : ".", leaf);
    return out;
}

/* PADInit, before any pad is opened */
void pad_controls_init(void) {
    char path[1024], line[512];
    FILE* f;
    int i, nlines = 0;
    for (i = 0; i < PAD_CHANMAX; i++) {
        player[i] = PADCTL_AUTO;
    }
    scripted = port_opt.pad_play || port_opt.pad_record || port_opt.soak || port_opt.noconfig ||
               port_opt.com4 || port_opt.nopad;
    if (scripted) {
        port_log("port> controls: a scripted run (--play/--record/--soak/--noconfig/--com4/--nopad): "
                 "PowerPCube's controls file and gamecontrollerdb.txt are not read\n");
        return;
    }
    f = fopen(ppc_dir(path, sizeof(path), "controls/mp4.txt"), "r");
    if (!f) {
        port_log("port> controls: no %s; the built-in mapping\n", path);
        return;
    }
    for (i = 0; i < ID_N; i++) {
        snprintf(pad_txt[i], sizeof(pad_txt[i]), "%s", def_pad[i]);
        snprintf(key_txt[i], sizeof(key_txt[i]), "%s", def_key[i]);
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq, *k, *v;
        int id;
        trim(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        k = line;
        v = eq + 1;
        trim(k);
        trim(v);
        nlines++;
        if (!strncmp(k, "player", 6) && k[6] >= '1' && k[6] <= '4' && !k[7]) {
            int p = k[6] - '1';
            player[p] = !strcmp(v, "keyboard") ? PADCTL_KEYBOARD : !strcmp(v, "none") ? PADCTL_NONE
                      : !strncmp(v, "pad:", 4) && atoi(v + 4) >= 1 ? PADCTL_PAD0 + atoi(v + 4) - 1
                      : PADCTL_AUTO;
        } else if (!strncmp(k, "pad.", 4) && (id = find_id(k + 4)) >= 0) {
            snprintf(pad_txt[id], sizeof(pad_txt[id]), "%s", v);
        } else if (!strncmp(k, "key.", 4) && (id = find_id(k + 4)) >= 0) {
            snprintf(key_txt[id], sizeof(key_txt[id]), "%s", v);
        } else {
            port_log("port> controls: \"%s\" is not a key this game reads; ignored\n", k);
        }
    }
    fclose(f);
    for (i = 0; i < ID_N; i++) {
        parse_pad(i, pad_txt[i]);
        parse_keys(i, key_txt[i]);
    }
    active = 1;
    port_log("port> controls: %s (%d lines)\n", path, nlines);
    port_log("port> controls: players %s\n", "");
    for (i = 0; i < PAD_CHANMAX; i++) {
        char pn[16];
        int p = player[i];
        if (p >= PADCTL_PAD0) {
            snprintf(pn, sizeof(pn), "pad:%d", p - PADCTL_PAD0 + 1);
        }
        port_log("port> controls:   player%d = %s\n", i + 1,
                 p == PADCTL_AUTO ? "auto" : p == PADCTL_NONE ? "none" : p == PADCTL_KEYBOARD ? "keyboard" : pn);
    }
    for (i = 0; i < ID_N; i++) {
        port_log("port> controls:   %-12s pad %-24s keys %s\n", id_name[i],
                 pad_txt[i][0] ? pad_txt[i] : "-", key_txt[i][0] ? key_txt[i] : "-");
    }
}

/* after SDL's controller subsystem is up, before the pads are opened */
void pad_controls_sdl_db(void) {
#ifndef PORT_NO_SDL
    char path[1024];
    FILE* f;
    if (scripted) {
        return;
    }
    f = fopen(ppc_dir(path, sizeof(path), "gamecontrollerdb.txt"), "r");
    if (!f) {
        return;
    }
    fclose(f);
    {
        int n = SDL_GameControllerAddMappingsFromRW(SDL_RWFromFile(path, "rb"), 1);
        port_log("port> controls: %s: %d SDL mapping(s) added%s%s\n", path, n < 0 ? 0 : n,
                 n < 0 ? " -- " : "", n < 0 ? SDL_GetError() : "");
    }
#endif
}

int pad_controls_active(void) { return active; }
int pad_controls_player(int port) { return active && port >= 0 && port < PAD_CHANMAX ? player[port] : PADCTL_AUTO; }

static const u16 id_bit[ID_N] = {
    PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R,
    PAD_BUTTON_START, PAD_BUTTON_UP, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT};

/* a pad through the table: `st` is its buttons and axes in SDL's terms */
void pad_controls_eval(const PadInState* st, PortPadRaw* out) {
    int id, k;
    u16 b = 0;
    memset(out, 0, sizeof(*out));
    for (id = ID_A; id <= ID_DRIGHT; id++) {
        const PadBind* pb = &pbind[id];
        int pressed = 0, analog = 0;
        for (k = 0; k < pb->n; k++) {
            if (pb->kind[k] == 1) {
                if (st->button[(int)pb->code[k]]) {
                    pressed = 1;
                    analog = 255;
                }
            } else {
                int v = st->axis[(int)pb->code[k]];
                if (pb->dir[k] == 0) { /* a trigger, whole */
                    int a = v > 0 ? v >> 7 : 0;
                    analog = a > analog ? a : analog;
                    pressed |= a >= 200;
                } else if (v * pb->dir[k] > 16384) {
                    pressed = 1;
                    analog = 255;
                }
            }
        }
        if (id == ID_L || id == ID_R) {
            if (id == ID_L) {
                out->triggerL = (u8)analog;
            } else {
                out->triggerR = (u8)analog;
            }
            if (analog >= 200) {
                b |= id_bit[id];
            }
        } else if (pressed) {
            b |= id_bit[id];
        }
    }
    if (stick_src >= 0) {
        pad_stick_map(st->axis[stick_src * 2], st->axis[stick_src * 2 + 1], &out->stickX, &out->stickY);
    }
    if (cstick_src >= 0) {
        pad_stick_map(st->axis[cstick_src * 2], st->axis[cstick_src * 2 + 1], &out->substickX, &out->substickY);
    }
    out->button = b;
}

/* the keyboard through the table (`k` is SDL_GetKeyboardState's array) */
static int key_down(const unsigned char* kb, int id) {
    int i;
    for (i = 0; i < kn[id]; i++) {
        if (kb[kcode[id][i]]) {
            return 1;
        }
    }
    return 0;
}
void pad_controls_keys(const unsigned char* kb, PortPadRaw* out) {
    int id;
    u16 b = 0;
    memset(out, 0, sizeof(*out));
    for (id = ID_A; id <= ID_DRIGHT; id++) {
        if (key_down(kb, id)) {
            b |= id_bit[id];
            if (id == ID_L) {
                out->triggerL = 255;
            } else if (id == ID_R) {
                out->triggerR = 255;
            }
        }
    }
    out->stickX = (s8)((key_down(kb, ID_SR) ? 100 : 0) - (key_down(kb, ID_SL) ? 100 : 0));
    out->stickY = (s8)((key_down(kb, ID_SU) ? 100 : 0) - (key_down(kb, ID_SD) ? 100 : 0));
    out->substickX = (s8)((key_down(kb, ID_CR) ? 100 : 0) - (key_down(kb, ID_CL) ? 100 : 0));
    out->substickY = (s8)((key_down(kb, ID_CU) ? 100 : 0) - (key_down(kb, ID_CD) ? 100 : 0));
    out->button = b;
}
