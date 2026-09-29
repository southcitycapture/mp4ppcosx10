/* M49 (PLAN.md 64): Lite mode -- simplifications for the screens a machine
 * cannot hold at 30 fps console-exact.
 *
 * The water's levels (gx_water.c, M44) are the model: a per-screen,
 * per-machine setting, remembered in the config.  Lite is a list of
 * *options*, each belonging to one minigame, each a change to what is drawn
 * and never to what the game computes:
 *
 *   - a caster's Hu3DModelShadowSet not made (its shadow not drawn);
 *   - a decorative model left HU3D_ATTR_DISPOFF (not drawn, not animated);
 *   - a particle model's quads not built nor drawn (its hook -- the
 *     simulation and its RNG draws -- still runs: port_lite_nodraw);
 *   - the character file one step lighter (CharModelCreate 2 -> 4, 4 -> 8:
 *     the same skeleton and hook names, fewer vertices).
 *
 * THE RULES (the user's, M49): never change how many times frand / frandu8
 * / rand8 / rand16 are called, never change a position, timer, collision,
 * score or attribute that game logic reads.  `--gamehash N` is the proof: a
 * chained hash of both RNG seeds, every live model's position, rotation and
 * scale and the players' records, logged every N frames -- a lockstep run
 * with Lite on and off must print the same lines.
 *
 *   lite = auto|on|off   (the config key; --lite, --nolite, --liteauto)
 *     auto (the default): on only for the screens this machine class needs
 *       it on -- the reference class (a dual 1 GHz G4 + Radeon 9000) the
 *       table's `ref` set, below the reference every option, faster
 *       machines and machines not judged none;
 *     on: every option of every Lite screen (or --liteopts's list);
 *     off: console-exact everywhere.
 *   liteopts = LIST      (--liteopts): the options Lite turns on, by name
 *     (m441.bshadow,m401.fish,...), `ref` the reference's set, `all`.
 *
 * The exact trims (m431's live sparkles, m444's paused table) are not Lite:
 * they draw the same pixels and are on for everyone (`--notrim` the old
 * path). */
#include "port.h"

#include "game/hu3d.h"
#include "game/gamework_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int port_cur_mg_number(void);
int port_machine_class(void);

typedef struct {
    int id;             /* mg * 100 + n: the id the patched game code asks for */
    const char* name;   /* for --liteopts and the log */
    int ref;            /* on at `auto` on the reference class */
    const char* what;   /* what changes on screen */
} LiteOpt;

/* `ref`: the set that takes each screen over 29.5 on the reference, measured
 * option by option (PLAN.md 64.4) */
static const LiteOpt lite_opts[] = {
    {44101, "m441.bshadow", 1, "Butterfly Blitz: the butterflies cast no shadow"},
    {44102, "m441.nshadow", 1, "Butterfly Blitz: the nets and baskets cast no shadow"},
    {44103, "m441.rings", 1, "Butterfly Blitz: every other flower around the field hidden"},
    {44104, "m441.char", 0, "Butterfly Blitz: the lighter character models"},
    {40101, "m401.fish", 1, "Manta Rings: each fish school drawn to its first 10 fish"},
    {40102, "m401.bubbles", 1, "Manta Rings: the ambient bubbles not drawn"},
    {40103, "m401.char", 0, "Manta Rings: the lighter character models"},
    {43601, "m436.plates", 1, "Fruits of Doom: the plates cast no shadow (hidden under them)"},
    {43602, "m436.pillars", 0, "Fruits of Doom: the two side pillars cast no shadow"},
    {43603, "m436.char", 0, "Fruits of Doom: the lighter character models"},
    {43501, "m435.pillars", 0, "Darts of Doom: the two side pillars cast no shadow"},
    {43502, "m435.char", 0, "Darts of Doom: the lighter character models"},
    {43101, "m431.char", 0, "Order Up: the lighter character models"},
    {44401, "m444.char", 0, "Reversal of Fortune: the lighter character models"},
    {46301, "m463.char", 0, "Panel Panic: the lighter character models"},
};
#define N_LITE ((int)(sizeof(lite_opts) / sizeof(lite_opts[0])))

static unsigned char lite_on[N_LITE]; /* the options in force, when the screen's Lite is on */
static int lite_ready;

static int lite_index(int id) {
    int i;
    for (i = 0; i < N_LITE; i++) {
        if (lite_opts[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* the option set from --liteopts (or the reference's) */
static void lite_fill(void) {
    int i;
    const char* s = port_opt.liteopts;
    int cls = port_machine_class();
    lite_ready = 1;
    if (!s || !*s || !strcmp(s, "ref") || !strcmp(s, "default")) {
        /* auto below the reference: every option; otherwise the reference's */
        int all = port_opt.lite < 0 && cls == 0;
        for (i = 0; i < N_LITE; i++) {
            lite_on[i] = all ? 1 : (unsigned char)lite_opts[i].ref;
        }
        return;
    }
    memset(lite_on, 0, sizeof(lite_on));
    while (*s) {
        char name[48];
        size_t n = strcspn(s, ",");
        if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
        }
        memcpy(name, s, n);
        name[n] = 0;
        s += n;
        if (*s == ',') {
            s++;
        }
        if (!strcmp(name, "all")) {
            memset(lite_on, 1, sizeof(lite_on));
            continue;
        }
        if (!strcmp(name, "ref")) {
            for (i = 0; i < N_LITE; i++) {
                lite_on[i] |= (unsigned char)lite_opts[i].ref;
            }
            continue;
        }
        for (i = 0; i < N_LITE; i++) {
            if (!strcmp(name, lite_opts[i].name)) {
                lite_on[i] = 1;
                break;
            }
        }
        if (i == N_LITE && name[0]) {
            port_log("port> lite: unknown option \"%s\" in --liteopts (ignored)\n", name);
        }
    }
}

void port_lite_report(void);
static void lite_init(void) {
    lite_ready = 1;
    lite_fill();
    port_lite_report();
}

/* Lite in force for this machine at all (the screen's options decide the rest) */
static int lite_machine(void) {
    int cls;
    if (port_opt.lite >= 0) {
        return port_opt.lite;
    }
    cls = port_machine_class();
    return cls == 0 || cls == 1; /* below or at the reference; never faster, never unjudged */
}

/* The patched game code's question: is option ID in force now?  Only inside
 * its own minigame, so a table entry can never leak into another screen. */
int port_lite_opt(int id) {
    int i;
    if (!lite_ready) {
        lite_init();
    }
    if (port_cur_mg_number() != id / 100 || !lite_machine()) {
        return 0;
    }
    i = lite_index(id);
    return i >= 0 && lite_on[i];
}

/* m401's fish: the schools' models past the first K stay DISPOFF (their
 * simulation is the game's and runs for every fish) */
int port_lite_fishk(void) {
    return port_lite_opt(40101) ? (port_opt.litefishk > 0 ? port_opt.litefishk : 10) : 0x7FFF;
}

/* the character file for CharModelCreate's `model` argument: one step
 * lighter when the screen's char option is on (2 -> 4, 4 -> 8; --litechar N
 * forces the file) */
int port_lite_charmodel(int id, int model) {
    if (!port_lite_opt(id)) {
        return model;
    }
    if (port_opt.litechar == 4 || port_opt.litechar == 8) {
        return port_opt.litechar;
    }
    return model == 2 ? 4 : model == 4 ? 8 : model;
}

/* ---- particle models whose quads are not drawn ------------------------------
 * particleFunc (hsfanim.c, patched) asks after running the model's hook: the
 * hook -- the simulation, its frand calls -- runs as always, the vertex
 * build and the draw do not.  Registered by the patched game code; a slot is
 * forgotten when the screen changes or the model is not the one registered. */
#define N_NODRAW 8
static HU3DMODEL* nodraw_mdl[N_NODRAW];
static void* nodraw_hook[N_NODRAW];
static int nodraw_mg;

void port_lite_nodraw_set(int mdlId) {
    int i;
    HU3DMODEL* m;
    if (mdlId < 0 || mdlId >= HU3D_MODEL_MAX) {
        return;
    }
    m = &Hu3DData[mdlId];
    if (nodraw_mg != port_cur_mg_number()) {
        memset(nodraw_mdl, 0, sizeof(nodraw_mdl));
        nodraw_mg = port_cur_mg_number();
    }
    for (i = 0; i < N_NODRAW; i++) {
        if (!nodraw_mdl[i] || nodraw_mdl[i] == m) {
            nodraw_mdl[i] = m;
            nodraw_hook[i] = m->hookData;
            return;
        }
    }
}

int port_lite_nodraw(void* model) {
    int i;
    if (!nodraw_mdl[0]) {
        return 0;
    }
    if (nodraw_mg != port_cur_mg_number()) {
        memset(nodraw_mdl, 0, sizeof(nodraw_mdl));
        return 0;
    }
    for (i = 0; i < N_NODRAW && nodraw_mdl[i]; i++) {
        if (nodraw_mdl[i] == model) {
            return nodraw_hook[i] == ((HU3DMODEL*)model)->hookData;
        }
    }
    return 0;
}

/* the exact trims' old path */
int port_trim_off(void) {
    return port_opt.notrim;
}

/* ---- the report -------------------------------------------------------------- */
void port_lite_report(void) {
    int i, cls = port_machine_class();
    char buf[512];
    size_t n = 0;
    if (!lite_ready) {
        lite_init();
    }
    buf[0] = 0;
    for (i = 0; i < N_LITE && n < sizeof(buf) - 40; i++) {
        if (lite_on[i]) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? "," : "", lite_opts[i].name);
        }
    }
    port_log("port> lite (M49): %s -> %s on this machine (class %d); options when on: %s%s\n",
             port_opt.lite < 0 ? "auto" : port_opt.lite ? "on" : "off",
             lite_machine() ? "on for its screens" : "off (console-exact everywhere)", cls,
             buf[0] ? buf : "(none)", port_opt.notrim ? "; --notrim: the exact trims' old path" : "");
}

/* ---- --gamehash N: the determinism proof ------------------------------------ */
extern s32 rnd_seed;
unsigned port_frand_seed(void);

static unsigned gh_mix(unsigned h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}

void port_lite_tick(unsigned frame) {
    static unsigned chain = 2166136261u;
    unsigned h = 2166136261u, fs, i;
    s32 rs;
    if (!port_opt.gamehash) {
        return;
    }
    fs = port_frand_seed();
    rs = rnd_seed;
    h = gh_mix(h, &fs, sizeof(fs));
    h = gh_mix(h, &rs, sizeof(rs));
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        const HU3DMODEL* m = &Hu3DData[i];
        if (m->hsf) {
            h = gh_mix(h, &i, sizeof(i));
            h = gh_mix(h, &m->pos, sizeof(m->pos));
            h = gh_mix(h, &m->rot, sizeof(m->rot));
            h = gh_mix(h, &m->scale, sizeof(m->scale));
        }
    }
    h = gh_mix(h, GWPlayer, sizeof(GWPlayer));
    chain = gh_mix(chain, &h, sizeof(h));
    if (frame % (unsigned)port_opt.gamehash == 0u) {
        port_log("port> gamehash f%u mg %d frand %08x rand8 %08x frame %08x chain %08x\n", frame,
                 port_cur_mg_number(), fs, (unsigned)rs, h, chain);
    }
}
