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
 *       table's `ref` set, below the reference that set and the `extra`
 *       options (M50: the user's picks; M49b gave class 0 the ref set), faster
 *       machines and machines not judged none;
 *     on: the reference's set on any machine (or --liteopts's list);
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
#include "game/chrman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int port_cur_mg_number(void);
int port_machine_class(void);

typedef struct {
    int id;             /* mg * 100 + n: the id the patched game code asks for */
    const char* name;   /* for --liteopts and the log */
    int ref;            /* on at `auto` on the reference class */
    const char* what;   /* what changes on screen */
    int extra;          /* M50: on at `auto` below the reference class too (the user's extras) */
} LiteOpt;

/* `ref`: the set that takes each screen over 29.5 on the reference, measured
 * option by option (PLAN.md 64.4).  M50 (PLAN.md 65, the user's picks of
 * 2026-09-30): m441's nets without their projected shadow and every other
 * fence flower join the butterflies' shadows (the round blob under each net,
 * m441.blob, is an option by name); the `extra` options are on at auto only below the
 * reference class, where the reference's set is not enough. */
static const LiteOpt lite_opts[] = {
    {44101, "m441.bshadow", 1, "Butterfly Blitz: the butterflies cast no shadow", 0},
    {44102, "m441.nshadow", 1, "Butterfly Blitz: the nets and baskets cast no shadow", 0},
    {44103, "m441.rings", 1, "Butterfly Blitz: every other flower around the field hidden", 0},
    /* M49b (PLAN.md 64b.1): not in the reference's set -- the lighter file's
     * net hook joint is not the m1 file's (--jointaudit) and m441 computes its
     * catches from it (main.c:1016) */
    {44104, "m441.char", 0, "Butterfly Blitz: the lighter character models", 0},
    /* M50: the user's "very basic circular shadow... like it's from the N64":
     * a flat dark disc on the floor under each net, drawn by the port after
     * the floor's layer (port_lite_layer_end); the net casts no projected
     * shadow while it is on (as m441.nshadow).  Not in `ref`: the user's rule
     * was "in the default set if it still holds 30.0 three runs" and it read
     * 30.0 / 29.9 / 29.9 (six runs: three 30.0, three 29.9; the drawn frame
     * 24.1 ms against 24.2 without it -- no dearer, but not 30.0 three times;
     * PLAN.md 65.2) */
    {44105, "m441.blob", 0, "Butterfly Blitz: a round N64-style shadow under each net instead of its projected one", 0},
    /* M51 (PLAN.md 66.6): the user's follow-up -- every shadow of the game a
     * round blob (the characters, the nets, the baskets, the butterflies);
     * the projected shadow pass has no caster left */
    {44106, "m441.bloball", 0, "Butterfly Blitz: every shadow a round N64-style blob (characters, nets, baskets, butterflies)", 0},
    {40101, "m401.fish", 0, "Manta Rings: each fish school drawn to its first 10 fish", 1},
    {40102, "m401.bubbles", 0, "Manta Rings: the ambient bubbles not drawn", 1},
    {40103, "m401.char", 1, "Manta Rings: the lighter character models", 0},
    {43601, "m436.plates", 0, "Fruits of Doom: the fruit stands cast no shadow", 1},
    {43602, "m436.pillars", 0, "Fruits of Doom: the two side pillars cast no shadow", 1},
    {43603, "m436.char", 1, "Fruits of Doom: the lighter character models", 0},
    {43501, "m435.pillars", 0, "Darts of Doom: the two side pillars cast no shadow", 1},
    {43502, "m435.char", 1, "Darts of Doom: the lighter character models", 0},
    {43101, "m431.char", 1, "Order Up: the lighter character models", 0},
    {44401, "m444.char", 1, "Reversal of Fortune: the lighter character models", 0},
    {46301, "m463.char", 1, "Panel Panic: the lighter character models", 0},
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
        /* the reference's set, on every machine Lite is on for.  M49b (PLAN.md
         * 64b.3): below the reference too -- M49 gave class 0 every option,
         * proved to the minigame's end only for the reference's set; the
         * others stay the user's choice (--liteopts) */
        /* M50 (PLAN.md 65): below the reference (class 0) the user's extras
         * too, each proved to its minigame's end on both casts */
        for (i = 0; i < N_LITE; i++) {
            lite_on[i] = (unsigned char)(lite_opts[i].ref || (cls == 0 && lite_opts[i].extra));
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
        if (!strcmp(name, "extras")) { /* M50: the below-the-reference extras */
            for (i = 0; i < N_LITE; i++) {
                lite_on[i] |= (unsigned char)lite_opts[i].extra;
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
    static int refused;
    if (!port_lite_opt(id)) {
        return model;
    }
    /* M49b (PLAN.md 64b.3): never two steps -- --litechar 8 on a game whose
     * own file is m1 (2) would load m3, whose joints were never proved; it
     * gets the one step (m2) */
    if (port_opt.litechar == 8 && model == 2) {
        if (!refused) {
            refused = 1;
            port_log("port> lite: --litechar 8 refused on this game (its own file is m1; m3 not proved): m2\n");
        }
        return 4;
    }
    if (port_opt.litechar == 4 || port_opt.litechar == 8) {
        return port_opt.litechar;
    }
    return model == 2 ? 4 : model == 4 ? 8 : model;
}

/* M49b (PLAN.md 64b.2): the eye textures of the file Lite loaded, for m436's
 * and m435's darkening pass (the loser charred, all but the eyes).  The game
 * asks CharModelEyeBmpGet(char, 2) -- m1's names; the m2 files' names, read
 * from each file (--jointaudit's material list), where the game's own table
 * (chrman.c charEyeBmpNameTbl) names textures the m2 file does not have
 * (Peach, Wario, Daisy, Waluigi).  Yoshi's pass goes by material index (not
 * here); Donkey Kong's m2 file has no eye texture of its own (the game's
 * table kept, matching nothing) and his eyes stay lit through the pass
 * anyway (docs/screenshots/m49b-eyes-m435-dk.jpg). */
static char* lite_eye_m2[8][2] = {
    {"s3c000m2_eyes", "s3c000m2_eyes"}, {"c001m3_eye", "c001m3_eye"},       {"c002m2_r_eye", "c002m2_l_eye"},
    {NULL, NULL},                       {"s3c004m3_eye", "s3c004m3_eye"},   {NULL, NULL},
    {"s3c006m2_eye", "s3c006m2_eye_R"}, {"s3c007_m2_eye", "s3c008_m2_eye"},
};

char** port_lite_eyebmp(s16 charNo, s16 model) {
    if (model == 4 && charNo >= 0 && charNo < 8 && lite_eye_m2[charNo][0]) {
        return lite_eye_m2[charNo];
    }
    return CharModelEyeBmpGet(charNo, model);
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

/* ---- m441.blob: the round shadow under each net (M50, PLAN.md 65) ----------
 * The user's request: instead of no shadow under Butterfly Blitz's nets, "a
 * very basic circular shadow... kinda looking like it's from the N64".  The
 * patched m441 player update hands over, each frame, the net point the game
 * has just computed (main.c:1016, work->unk28 -- read, never written) and the
 * net model's id (its DISPOFF bit says whether the net is out); after the
 * floor's layer of camera 0 Hu3DExec calls port_lite_layer_end, which draws
 * one flat dark disc per visible net on the floor (y = 0) under the point:
 * GX direct vertices, colour only (no texture, no light), blended, depth
 * tested against the floor and never written, so the players and the nets
 * drawn after it cover it as the console's shadow would be covered.  A disc
 * is a 16-triangle fan at full darkness and a 32-triangle rim fading to
 * nothing (the soft edge of the N64's blob texture).  Nothing the game reads
 * changes: no model, no motion, no RNG, no Hu3DData slot (--gamehash). */
#define BLOB_SEG 16
static struct {
    int mdl;
    float x, y, z;
} blob_net[4];
static int blob_armed, blob_mg;

void port_lite_blob_net(int player, const void* p, int netMdl) {
    const float* v = (const float*)p;
    if (player < 0 || player > 3 || !(port_lite_opt(44105) || port_lite_opt(44106))) {
        return;
    }
    if (blob_mg != port_cur_mg_number()) {
        memset(blob_net, 0, sizeof(blob_net));
        blob_mg = port_cur_mg_number();
    }
    blob_net[player].mdl = netMdl + 1; /* 0 = not registered */
    blob_net[player].x = v[0];
    blob_net[player].y = v[1];
    blob_net[player].z = v[2];
    blob_armed = 1;
}

/* ---- m441.bloball (M51, PLAN.md 66.6): every caster a blob ---------------- */
#define N_CASTER 64
static struct {
    int mdl;
    void* hsf;
    int kind; /* 0 character, 1 net (drawn from the net point), 2 basket, 3 butterfly, 4 the small butterfly */
} caster[N_CASTER];
static int ncaster, caster_mg;
static struct {
    int mdl;
    float x, y, z;
} hookw[4];

static void caster_reset_if_new_screen(void) {
    if (caster_mg != port_cur_mg_number()) {
        ncaster = 0;
        memset(hookw, 0, sizeof(hookw));
        caster_mg = port_cur_mg_number();
    }
}

void port_lite_blob_caster(int mdl, int kind) {
    int i;
    if (mdl < 0 || mdl >= HU3D_MODEL_MAX) {
        return;
    }
    caster_reset_if_new_screen();
    for (i = 0; i < ncaster; i++) {
        if (caster[i].mdl == mdl) {
            break;
        }
    }
    if (i == ncaster) {
        if (ncaster >= N_CASTER) {
            return;
        }
        ncaster++;
    }
    caster[i].mdl = mdl;
    caster[i].hsf = Hu3DData[mdl].hsf;
    caster[i].kind = kind;
    blob_armed = 1;
    blob_mg = port_cur_mg_number();
    if (kind == 2) {
        int k;
        for (k = 0; k < 4; k++) {
            if (!hookw[k].mdl || hookw[k].mdl == mdl + 1) {
                hookw[k].mdl = mdl + 1;
                break;
            }
        }
    }
}

/* hsfdraw.c (patched): a hooked model's matrix as the draw built it (camera
 * space); the baskets' is kept, in world space, for the next frame's blobs */
void port_lite_hook_mtx(int hookMdl, Mtx m) {
    int k;
    if (!ncaster || caster_mg != 441 || port_cur_mg_number() != 441) {
        return;
    }
    for (k = 0; k < 4; k++) {
        if (hookw[k].mdl == hookMdl + 1) {
            Mtx inv;
            Vec c, w;
            c.x = m[0][3];
            c.y = m[1][3];
            c.z = m[2][3];
            if (!MTXInverse(Hu3DCameraMtx, inv)) {
                return;
            }
            MTXMultVec(inv, &c, &w);
            hookw[k].x = w.x;
            hookw[k].y = w.y;
            hookw[k].z = w.z;
            return;
        }
    }
}

void port_vc_foreign(int on);

static void blob_vtx(float x, float z, u8 a) {
    GXPosition3f32(x, 1.0f, z);
    GXColor4u8(0, 0, 0, a);
}

typedef struct {
    float x, z, r, ri;
    u8 a;
} BlobDisc;

static int blob_disc(BlobDisc* d, float x, float y, float z, float r0, int a0) {
    /* higher = a little smaller and fainter, as the N64's blobs did */
    float h = y < 0.0f ? 0.0f : y > 400.0f ? 400.0f : y;
    d->x = x;
    d->z = z;
    d->r = r0 * (1.0f - h * 0.0006f);
    d->ri = d->r * 0.78f;
    d->a = (u8)((float)a0 * (1.0f - h * 0.0008f));
    return 1;
}

void port_lite_layer_end(int cam, int layer) {
    static float cs[BLOB_SEG + 1], sn[BLOB_SEG + 1];
    static BlobDisc disc[4 + N_CASTER];
    int i, k, n = 0, all;
    if (!blob_armed || cam != 0 || layer != 0) {
        return;
    }
    all = port_lite_opt(44106);
    if (blob_mg != port_cur_mg_number() || !(port_lite_opt(44105) || all)) {
        blob_armed = 0;
        return;
    }
    {
        float nr = port_opt.blobr > 0 ? (float)port_opt.blobr : 80.0f;
        int na = port_opt.bloba > 0 ? port_opt.bloba : 150;
        for (i = 0; i < 4; i++) {
            int m = blob_net[i].mdl - 1;
            if (m >= 0 && m < HU3D_MODEL_MAX && Hu3DData[m].hsf && !(Hu3DData[m].attr & HU3D_ATTR_DISPOFF)) {
                n += blob_disc(&disc[n], blob_net[i].x, blob_net[i].y, blob_net[i].z, all ? nr * 0.8f : nr, na);
            }
        }
    }
    if (all && caster_mg == port_cur_mg_number()) {
        for (i = 0; i < ncaster; i++) {
            int m = caster[i].mdl;
            HU3DMODEL* d = &Hu3DData[m];
            if (!d->hsf || d->hsf != caster[i].hsf || (d->attr & HU3D_ATTR_DISPOFF)) {
                continue;
            }
            switch (caster[i].kind) {
                case 0:
                    n += blob_disc(&disc[n], d->pos.x, d->pos.y, d->pos.z, 70.0f, 160);
                    break;
                case 2:
                    for (k = 0; k < 4; k++) {
                        if (hookw[k].mdl == m + 1 && (hookw[k].x != 0.0f || hookw[k].z != 0.0f)) {
                            n += blob_disc(&disc[n], hookw[k].x, hookw[k].y, hookw[k].z, 36.0f, 130);
                        }
                    }
                    break;
                case 3:
                    n += blob_disc(&disc[n], d->pos.x, d->pos.y, d->pos.z, 34.0f, 130);
                    break;
                case 4:
                    n += blob_disc(&disc[n], d->pos.x, d->pos.y, d->pos.z, 24.0f, 120);
                    break;
                default:
                    break;
            }
        }
    }
    if (!n) {
        return;
    }
    if (cs[0] == 0.0f) {
        for (k = 0; k <= BLOB_SEG; k++) {
            float t = 6.2831853f * (float)(k % BLOB_SEG) / (float)BLOB_SEG;
            cs[k] = cosf(t);
            sn[k] = sinf(t);
        }
    }
    port_vc_foreign(1);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZCompLoc(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXLoadPosMtxImm(Hu3DCameraMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT7, (u16)(n * BLOB_SEG * 9));
    for (i = 0; i < n; i++) {
        float x = disc[i].x, z = disc[i].z, r = disc[i].r, ri = disc[i].ri;
        u8 a = disc[i].a;
        for (k = 0; k < BLOB_SEG; k++) {
            /* the core */
            blob_vtx(x, z, a);
            blob_vtx(x + ri * cs[k], z + ri * sn[k], a);
            blob_vtx(x + ri * cs[k + 1], z + ri * sn[k + 1], a);
            /* the rim: a quad as two triangles, darkness to nothing */
            blob_vtx(x + ri * cs[k], z + ri * sn[k], a);
            blob_vtx(x + r * cs[k], z + r * sn[k], 0);
            blob_vtx(x + r * cs[k + 1], z + r * sn[k + 1], 0);
            blob_vtx(x + ri * cs[k], z + ri * sn[k], a);
            blob_vtx(x + r * cs[k + 1], z + r * sn[k + 1], 0);
            blob_vtx(x + ri * cs[k + 1], z + ri * sn[k + 1], a);
        }
    }
    GXEnd();
    /* the engine's usual state back (each material sets its own anyway) */
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetCullMode(GX_CULL_BACK);
    port_vc_foreign(0);
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

/* M49b: a value game logic computes outside Hu3DData / GWPlayer (m441's net
 * position, main.c:1016), mixed into this frame's hash by the patched game
 * line; a no-op without --gamehash */
static unsigned gh_extra = 2166136261u;
void port_gamehash_mix(const void* p, unsigned n) {
    if (port_opt.gamehash) {
        gh_extra = gh_mix(gh_extra, p, n);
    }
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
    h = gh_mix(h, &gh_extra, sizeof(gh_extra));
    gh_extra = 2166136261u;
    chain = gh_mix(chain, &h, sizeof(h));
    if (frame % (unsigned)port_opt.gamehash == 0u) {
        port_log("port> gamehash f%u mg %d frand %08x rand8 %08x frame %08x chain %08x\n", frame,
                 port_cur_mg_number(), fs, (unsigned)rs, h, chain);
    }
}

/* ---- --jointaudit: the character files' hook joints (M49b, PLAN.md 64b.1) ----
 * Called by the patched m441 player setup (the first player) with m441's own
 * motion table; with --jointaudit set, for each of the eight characters and
 * each file (m1, m2, m3: CharModelCreate 2, 4, 8) every hook joint the Lite
 * games (and chrman's effects) read is printed -- Hu3DModelObjMtxGet, the
 * twelve floats' bits -- at five times of each of the character's m441
 * motions (the idle, the net swings, the catches); then the run ends.
 * tools/m49b_joints.py compares the files.  Also each file's eye materials
 * (m436/m435's darkening pass keeps them lit). */
static const char* ja_hooks[] = {"a-itemhook-r", "a-itemhook-l", "a-itemhook-fr", "a-itemhook-fl", "a-itemhook-body",
                                 "test11_tex_we-itemhook-r", "test11_tex_we-ske_R_shoe1"};
#define N_JA_HOOKS ((int)(sizeof(ja_hooks) / sizeof(ja_hooks[0])))

static void ja_mtx(int c, int file, int m, int k, float t, int mdl) {
    int h, j, r;
    for (h = 0; h < N_JA_HOOKS; h++) {
        Mtx mtx;
        unsigned b[12];
        if (!Hu3DModelObjPtrGet(mdl, (char*)ja_hooks[h])) {
            continue;
        }
        Hu3DModelObjMtxGet(mdl, (char*)ja_hooks[h], mtx);
        for (r = 0; r < 3; r++) {
            for (j = 0; j < 4; j++) {
                memcpy(&b[r * 4 + j], &mtx[r][j], 4);
            }
        }
        port_log("joint c%d m%d mot%02d k%d t%.2f %s %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x"
                 " pos %.3f %.3f %.3f\n",
                 c, file, m, k, t, ja_hooks[h], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
                 b[11], mtx[0][3], mtx[1][3], mtx[2][3]);
    }
}

void port_joint_audit(const s32 (*mot)[16]) {
    static const s16 files[3] = {2, 4, 8};
    int c, f, m, k, h, i, j;
    if (!port_opt.jointaudit) {
        return;
    }
    port_log("port> jointaudit (M49b): 8 characters x m1/m2/m3, m441's motions\n");
    for (c = 0; c < 8; c++) {
        for (f = 0; f < 3; f++) {
            int mdl = CharModelCreate(c, files[f]);
            HU3DMODEL* mp = &Hu3DData[mdl];
            HSFDATA* hsf = mp->hsf;
            char** eye = CharModelEyeBmpGet(c, files[f]);
            char line[256];
            size_t n = 0;
            line[0] = 0;
            for (h = 0; h < N_JA_HOOKS; h++) {
                n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%d", ja_hooks[h],
                                      Hu3DModelObjPtrGet(mdl, (char*)ja_hooks[h]) != NULL);
            }
            port_log("joint c%d m%d file objects %d materials %d hooks%s\n", c, f + 1, (int)hsf->objectNum,
                     (int)hsf->materialNum, line);
            /* the materials carrying the file's eye textures, and 1/2 (Yoshi's branch) */
            for (i = 0; i < (int)hsf->materialNum; i++) {
                HSFMATERIAL* mat = &hsf->material[i];
                int eyes = 0;
                for (j = 0; j < (int)mat->attrNum; j++) {
                    HSFATTRIBUTE* a = &hsf->attribute[mat->attr[j]];
                    if (a->bitmap && a->bitmap->name
                        && (!strcmp(a->bitmap->name, eye[0]) || !strcmp(a->bitmap->name, eye[1]))) {
                        eyes = 1;
                    }
                }
                {
                    /* every texture of every material (M49b: the m2/m3 files' eye names) */
                    char nb[400];
                    size_t q = 0;
                    nb[0] = 0;
                    for (j = 0; j < (int)mat->attrNum && q < sizeof(nb) - 40; j++) {
                        HSFATTRIBUTE* a = &hsf->attribute[mat->attr[j]];
                        q += (size_t)snprintf(nb + q, sizeof(nb) - q, " %s",
                                              a->bitmap && a->bitmap->name ? a->bitmap->name : "-");
                    }
                    port_log("joint c%d m%d matnames %d:%s\n", c, f + 1, i, nb);
                }
                if (eyes || i == 1 || i == 2) {
                    const char* bn = "-";
                    if (mat->attrNum && hsf->attribute[mat->attr[0]].bitmap && hsf->attribute[mat->attr[0]].bitmap->name) {
                        bn = hsf->attribute[mat->attr[0]].bitmap->name;
                    }
                    port_log("joint c%d m%d material %d eye %d (m1 names %s / %s) first bitmap %s\n", c, f + 1, i, eyes,
                             CharModelEyeBmpGet(c, 2)[0], CharModelEyeBmpGet(c, 2)[1], bn);
                }
            }
            for (m = 0; m < 16; m++) {
                int motId;
                float max;
                if (!mot[c][m]) {
                    continue;
                }
                motId = CharMotionCreate(c, mot[c][m]);
                Hu3DMotionSet(mdl, motId);
                max = Hu3DMotionMaxTimeGet(mdl);
                for (k = 0; k <= 5; k++) {
                    float t = max * (float)k / 5.0f;
                    Hu3DMotionExec(mdl, motId, t, 0);
                    ja_mtx(c, f + 1, m, k, t, mdl);
                }
            }
            CharModelKill(c);
            CharModelDataClose(c);
        }
    }
    port_log("port> jointaudit done\n");
    port_shutdown(0);
}
