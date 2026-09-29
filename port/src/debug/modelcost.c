/* M49 (PLAN.md 64): --modelcost A,B -- what each model costs the game thread.
 *
 * Built only in the measurement tree (make M49X=1: patches-m49x.txt renames
 * Hu3DDraw / Hu3DDrawPost *_game, motion_exec.c's Hu3DMotionExec becomes
 * Hu3DMotionExec_m49).  Between frames A and B each call is timed with
 * mach_absolute_time and charged to its model: the draw -- the object walk,
 * the skin (port_envelope_sync runs inside it), the materials, the GX front
 * end -- split by pass (the main camera's, the shadow map's) and by frame
 * (drawn or consumed); the motion evaluations; Hu3DDrawPost (the sorted
 * translucent objects) as one line.  At exit: the models by cost, each with
 * its first object's name, its object count and its attributes -- which
 * model a Lite option would touch, and what it could buy on this thread. */
#include "port.h"

#if PORT_M49X
#include "game/hu3d.h"

#include <mach/mach_time.h>
#include <string.h>

void Hu3DDraw_game(HU3DMODEL* modelP, Mtx mtx, HuVecF* scale);
void Hu3DDrawPost_game(void);
void Hu3DMotionExec_m49(s16 arg0, s16 arg1, float arg2, s32 arg3);
int gl13_draw_off(void);
unsigned long port_mg_entry(void);


typedef struct {
    unsigned long long draw[2][2]; /* [consumed][shadow] ticks */
    unsigned long ndraw[2][2];
    unsigned long long motion;
    unsigned long nmotion;
    const void* hsf; /* named by --skinstats' "hsf 0x... registered" line */
    int objs;
    unsigned attr;
} MC;
static MC mc[HU3D_MODEL_MAX];
static unsigned long long mc_post[2];
static unsigned mc_frames[2];
static unsigned mc_last_frame;
/* frames A..B from the minigame's entry (a --minigame run), else retraces A..B */
static int mc_on_frame(void) {
    unsigned long base = 0, f;
    if (!port_opt.modelcost_to) {
        return 0;
    }
    if (port_opt.minigame) {
        base = port_mg_entry();
        if (!base) {
            return 0;
        }
    }
    f = VIGetRetraceCount() - base;
    if ((int)f < port_opt.modelcost_from || (int)f >= port_opt.modelcost_to) {
        return 0;
    }
    if (f != mc_last_frame) {
        mc_last_frame = f;
        mc_frames[gl13_draw_off() ? 1 : 0]++;
    }
    return 1;
}

static void mc_note(MC* m, HU3DMODEL* p) {
    /* not the objects' names: a lighter character file's root has none */
    if (p->hsf && !(p->attr & HU3D_ATTR_HOOK)) { /* a hook model's hsf is its function */
        m->hsf = p->hsf;
        m->objs = p->hsf->objectNum;
    }
    m->attr = p->attr;
}

void Hu3DDraw(HU3DMODEL* modelP, Mtx mtx, HuVecF* scale) {
    unsigned long long t0;
    int i, c, s;
    if (!mc_on_frame()) {
        Hu3DDraw_game(modelP, mtx, scale);
        return;
    }
    t0 = mach_absolute_time();
    Hu3DDraw_game(modelP, mtx, scale);
    i = (int)(modelP - Hu3DData);
    if (i < 0 || i >= HU3D_MODEL_MAX) {
        return;
    }
    c = gl13_draw_off() ? 1 : 0;
    s = shadowModelDrawF ? 1 : 0;
    mc[i].draw[c][s] += mach_absolute_time() - t0;
    mc[i].ndraw[c][s]++;
    mc_note(&mc[i], modelP);
}

void Hu3DDrawPost(void) {
    unsigned long long t0;
    if (!mc_on_frame()) {
        Hu3DDrawPost_game();
        return;
    }
    t0 = mach_absolute_time();
    Hu3DDrawPost_game();
    mc_post[gl13_draw_off() ? 1 : 0] += mach_absolute_time() - t0;
}

void Hu3DMotionExec(s16 arg0, s16 arg1, float arg2, s32 arg3) {
    unsigned long long t0;
    if (!mc_on_frame() || arg0 < 0 || arg0 >= HU3D_MODEL_MAX) {
        Hu3DMotionExec_m49(arg0, arg1, arg2, arg3);
        return;
    }
    t0 = mach_absolute_time();
    Hu3DMotionExec_m49(arg0, arg1, arg2, arg3);
    mc[arg0].motion += mach_absolute_time() - t0;
    mc[arg0].nmotion++;
    mc_note(&mc[arg0], &Hu3DData[arg0]);
}

static int mc_cmp_idx[HU3D_MODEL_MAX];
static double mc_total(int i) {
    return (double)(mc[i].draw[0][0] + mc[i].draw[0][1] + mc[i].draw[1][0] + mc[i].draw[1][1] +
                    mc[i].motion);
}

void port_modelcost_report(void) {
    mach_timebase_info_data_t tb;
    double ms, fd, fc;
    int i, j, n = 0;
    if (!port_opt.modelcost_to) {
        return;
    }
    mach_timebase_info(&tb);
    ms = (double)tb.numer / tb.denom / 1e6;
    fd = mc_frames[0] ? mc_frames[0] : 1;
    fc = mc_frames[1] ? mc_frames[1] : 1;
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        if (mc_total(i) > 0) {
            mc_cmp_idx[n++] = i;
        }
    }
    for (i = 1; i < n; i++) { /* by total cost, descending */
        int k = mc_cmp_idx[i];
        for (j = i; j > 0 && mc_total(mc_cmp_idx[j - 1]) < mc_total(k); j--) {
            mc_cmp_idx[j] = mc_cmp_idx[j - 1];
        }
        mc_cmp_idx[j] = k;
    }
    port_log("port> modelcost (M49): frames %d..%d: %u drawn, %u consumed; ms a drawn frame "
             "(main / shadow draw), ms a consumed frame (main / shadow walk), motion ms a frame\n",
             port_opt.modelcost_from, port_opt.modelcost_to, mc_frames[0], mc_frames[1]);
    port_log("port> modelcost:   Hu3DDrawPost: drawn %.3f, consumed %.3f\n", mc_post[0] * ms / fd,
             mc_post[1] * ms / fc);
    for (j = 0; j < n && j < 80; j++) {
        MC* m = &mc[mc_cmp_idx[j]];
        port_log("port> modelcost: %3d hsf %p objs %3d attr %08x  drawn %.3f / %.3f (%lu/%lu)  "
                 "consumed %.3f / %.3f  motion %.3f (%lu)\n",
                 mc_cmp_idx[j], m->hsf, m->objs, m->attr, m->draw[0][0] * ms / fd,
                 m->draw[0][1] * ms / fd, m->ndraw[0][0], m->ndraw[0][1], m->draw[1][0] * ms / fc,
                 m->draw[1][1] * ms / fc, m->motion * ms / (fd + fc), m->nmotion);
    }
}
#else
void port_modelcost_report(void) {
}
#endif
