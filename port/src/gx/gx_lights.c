/* M46 (PLAN.md 61): the Bowser arena's pillar lights at 30 presented frames.
 *
 * m435 Darts of Doom, m436 Fruits of Doom and m437 Balloon of Doom share an
 * arena whose two pillars (models "cyl3" and "cyl6") carry a translucent
 * glowing sphere with a star flare inside.  The pillars' motion (a 60-frame
 * loop) keys the sphere and the flare to a four-frame shimmer -- big,
 * medium, small, medium -- which on the console is a 60 Hz sparkle.  In
 * lockstep the port draws it frame for frame as the console does (the pairs
 * in PLAN.md 61 are identical).  At real time the port presents one frame in
 * two, and a four-frame cycle sampled every second frame is either medium,
 * medium, medium (one parity) or big, small, big, small (the other): a
 * steady ball that turns into a strobe -- with the flare's dark halo on
 * every big frame -- whenever the pacing slips a frame and the parity flips.
 * That is what the user saw ("flashing weird, it's not correct").
 *
 * The fix is the presentation's, not the game's: on a drawn frame whose
 * predecessor was consumed, each pillar object whose *world* transform
 * changed between the two frames is drawn at the midpoint of the two --
 * the moment in the middle of the 1/30 s the presented frame stands for.
 * The world transform is the loaded matrix with the camera taken off
 * (W = C^-1 M), so a camera move between the frames does not drag the
 * light off its pillar: M' = C_N ((W_N-1 + W_N) / 2).  Both parities then
 * present the same pair of pictures, (big+medium)/2 and (small+medium)/2,
 * a gentle 15 Hz pulse like the console's; an object whose transform did
 * not change keeps the game's own matrix, byte for byte.  Nothing changes
 * in lockstep, under --turbo or in a fast-forward (no frame is consumed
 * before a drawn one), so every md5 and picture check stays the game's.
 * `--nolights` is the old path.
 *
 * Only the three overlays, only the two models (named by their HSF's first
 * object), only on frames the frame mode decides: elsewhere this is one
 * branch in GXLoadPosMtxImm. */
#include "port.h"

#include "gx_internal.h"

#include "game/hu3d.h"
#include "game/hsfformat.h"
#include "game/object.h"

#include <dolphin/mtx.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

int gl13_draw_off(void);
unsigned gl13_frame_number(void);

#define LIGHTS_MAX 32 /* (model, object, occurrence) entries kept */

typedef struct {
    const void* model;  /* HU3DMODEL* */
    const void* object; /* HSFOBJECT* */
    int occ;            /* the n-th load of this object in its frame */
    unsigned frame;     /* the frame it was seen in */
    int consumed;       /* seen on a consumed frame */
    Mtx w;              /* its world transform then (C^-1 M) */
} LightEnt;

static LightEnt ents[LIGHTS_MAX];
static int n_ents;
static unsigned cur_frame = ~0u;
static int on;             /* the current overlay is one of the three */
static int last_ovl = -1;
static signed char pillar[HU3D_MODEL_MAX]; /* -1 unknown, 0 no, 1 a pillar */
static const void* pillar_hsf[HU3D_MODEL_MAX];
static Mtx nrm_next;       /* the normal matrix for the load that follows */
static int nrm_pending;
static unsigned long n_mid, n_same, n_jump;
static int told;

static int is_pillar(int idx, const HU3DMODEL* m) {
    const HSFDATA* h = m->hsf;
    if (pillar[idx] >= 0 && pillar_hsf[idx] == (const void*)h) {
        return pillar[idx];
    }
    pillar_hsf[idx] = h;
    pillar[idx] = 0;
    if (h && !(m->attr & HU3D_ATTR_HOOKFUNC) && h->object && h->objectNum > 0 && h->object[0].name &&
        (!strcmp(h->object[0].name, "cyl3") || !strcmp(h->object[0].name, "cyl6"))) {
        pillar[idx] = 1;
    }
    return pillar[idx];
}

static void overlay_check(void) {
    int o = (int)omcurovl;
    if (o == last_ovl) {
        return;
    }
    last_ovl = o;
    on = !port_opt.nolights && (o == DLL_m435dll || o == DLL_m436dll || o == DLL_m437dll);
    memset(pillar, -1, sizeof(pillar));
    n_ents = 0;
}

/* GXLoadPosMtxImm's hook: 1 and `out` filled when the matrix is replaced */
int gx_lights_posmtx(const void* mtx, u32 slot, f32 out[3][4]) {
    const HU3DDRAWOBJ* d;
    const char* base;
    unsigned fr;
    int idx, k, occ, drawn;
    LightEnt* e = NULL;
    LightEnt* prev = NULL;
    Mtx inv, w;
    nrm_pending = 0;
    overlay_check();
    if (!on || slot != 0 || !mtx || !port_framemode_active()) {
        return 0;
    }
    /* Hu3DDrawPost and ObjDraw load drawObj->matrix: the pointer is the
     * draw object (scenelog.c's port_drawobj_name does the same) */
    base = (const char*)mtx - offsetof(HU3DDRAWOBJ, matrix);
    if (((uintptr_t)base & 3u) || port_in_game_region(mtx)) {
        return 0;
    }
    d = (const HU3DDRAWOBJ*)base;
    if (d->model < &Hu3DData[0] || d->model >= &Hu3DData[HU3D_MODEL_MAX] || !d->object) {
        return 0;
    }
    idx = (int)(d->model - &Hu3DData[0]);
    if (!is_pillar(idx, d->model)) {
        return 0;
    }
    fr = gl13_frame_number() + 1; /* the frame being built */
    if (fr != cur_frame) {
        cur_frame = fr;
        /* this frame's occurrences start again; last frame's entries stay
         * readable (their frame number tells them apart) */
    }
    drawn = !gl13_draw_off();
    MTXInverse(Hu3DCameraMtx, inv);
    MTXConcat(inv, *(const Mtx*)mtx, w);
    /* this object's occurrence number in this frame, and its entry */
    occ = 0;
    for (k = 0; k < n_ents; k++) {
        if (ents[k].model == d->model && ents[k].object == d->object && ents[k].frame == fr) {
            occ++;
        }
    }
    for (k = 0; k < n_ents; k++) {
        if (ents[k].model == d->model && ents[k].object == d->object && ents[k].occ == occ) {
            if (ents[k].frame == fr - 1 && ents[k].consumed) {
                prev = &ents[k];
            }
            e = &ents[k];
            break;
        }
    }
    {
        int replaced = 0;
        if (drawn && prev) {
            f32 dmax = 0.0f, tmax = 0.0f;
            int r, c;
            for (r = 0; r < 3; r++) {
                for (c = 0; c < 3; c++) {
                    f32 v = fabsf(prev->w[r][c] - w[r][c]);
                    dmax = v > dmax ? v : dmax;
                }
                {
                    f32 v = fabsf(prev->w[r][3] - w[r][3]);
                    tmax = v > tmax ? v : tmax;
                }
            }
            if (dmax == 0.0f && tmax == 0.0f) {
                n_same++;
            } else if (tmax > 50.0f || dmax > 1.0f) {
                n_jump++; /* a move, not a shimmer: the game's own picture */
            } else {
                Mtx mid;
                for (r = 0; r < 3; r++) {
                    for (c = 0; c < 4; c++) {
                        mid[r][c] = 0.5f * (prev->w[r][c] + w[r][c]);
                    }
                }
                MTXConcat(Hu3DCameraMtx, mid, out);
                MTXInvXpose(out, nrm_next);
                nrm_pending = 1;
                replaced = 1;
                n_mid++;
                if (!told) {
                    told = 1;
                    port_log("port> lights: the pillar lights drawn between the game's two frames at "
                             "30 presented fps (model %d \"%s\"; --nolights the old path, PLAN.md 61)\n",
                             idx, d->model->hsf->object[0].name);
                }
            }
        }
        if (!e) {
            if (n_ents < LIGHTS_MAX) {
                e = &ents[n_ents++];
            } else {
                e = &ents[(fr + (unsigned)occ) % LIGHTS_MAX];
            }
            e->model = d->model;
            e->object = d->object;
            e->occ = occ;
        }
        e->frame = fr;
        e->consumed = !drawn;
        memcpy(e->w, w, sizeof(Mtx));
        return replaced;
    }
}

/* GXLoadNrmMtxImm's hook: the inverse transpose of the replaced matrix, for
 * the normal load the game makes right after the position load */
const void* gx_lights_nrmmtx(const void* mtx) {
    if (nrm_pending) {
        nrm_pending = 0;
        return nrm_next;
    }
    return mtx;
}

void gx_lights_report(void) {
    if (n_mid || n_jump) {
        port_log("port> lights: %lu pillar draws between two frames, %lu unchanged, %lu moves left alone\n",
                 n_mid, n_same, n_jump);
    }
}
