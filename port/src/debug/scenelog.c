/* --scenelog F: what the 3D scene believes about itself on one presented frame.
 *
 * `--drawlog-at` says what a draw submitted; it cannot say *why* the position
 * matrix has the value it has, because by the time GX sees it the matrix is
 * already the product of the camera and the model.  This prints the two
 * factors separately -- every live `Hu3DCamera` slot and every live
 * `Hu3DData` model that carries a camera or is being drawn -- so "the camera
 * and the model disagree about the world offset" becomes a question with a
 * side.
 *
 * It reads the game's own globals directly.  `src/game/hsfman.c` is compiled
 * into this binary (it is DOL code, not a REL), so there is nothing to hook
 * and nothing to patch: the port simply looks. */
#include "port.h"

#include "game/hu3d.h"
#include "game/hsfformat.h"
#include "game/object.h"
#include "game/board/main.h"

/* w01dll, Toad's Midway Madness: the index of dll/w01dll.rel in include/ovl_table.h. */
#define PORT_BOARD_OVL_W01 89

#include <math.h>

#include <dolphin/mtx.h>

extern HU3DMODEL Hu3DData[0x200];
extern HU3DCAMERA Hu3DCamera[0x10];
extern Mtx Hu3DCameraMtx;
extern s16 Hu3DCameraNo;

unsigned gl13_frame_number(void);

/* objMesh's own concatenation, re-walked from outside the game.
 *
 * The drawn position matrix is `Hu3DCameraMtx * model placement * (the
 * product of every HSF object transform from the root down)`, and by frame
 * 5100 the first two are provably small while the third is in the tens of
 * thousands.  Walking the tree here, with the same T*R*S order
 * `src/game/hsfdraw.c:objMesh` uses, says which object in which model the
 * offset enters at -- which is the difference between "the port reads the HSF
 * wrong" and "the port's matrix arithmetic is wrong". */
static void walk_objects(int mdl, const HSFDATA* hsf, const HSFOBJECT* o, const Mtx par,
                         int depth, int use_curr, int* reported) {
    Mtx local;
    Mtx here;
    const HSFTRANSFORM* t = use_curr ? &o->mesh.curr : &o->mesh.base;
    int i;
    if (depth > 24 || *reported > 6) {
        return;
    }
    /* Only the transform-bearing object types share the HSFMESH layout; a
     * camera (7) or a light (8) puts something else in the union. */
    if (o->type == 7 || o->type == 8 || o->type > 9) {
        return;
    }
    C_MTXScale(local, t->scale.x, t->scale.y, t->scale.z);
    {
        Mtx r;
        C_MTXRotRad(r, 'x', t->rot.x * 0.017453292f);
        C_MTXConcat(r, local, local);
        C_MTXRotRad(r, 'y', t->rot.y * 0.017453292f);
        C_MTXConcat(r, local, local);
        C_MTXRotRad(r, 'z', t->rot.z * 0.017453292f);
        C_MTXConcat(r, local, local);
    }
    local[0][3] += t->pos.x;
    local[1][3] += t->pos.y;
    local[2][3] += t->pos.z;
    C_MTXConcat(par, local, here);
    if ((here[0][3] > 3000.0f || here[0][3] < -3000.0f) &&
        par[0][3] < 3000.0f && par[0][3] > -3000.0f) {
        (*reported)++;
        port_log("  hsf mdl%-3d depth %2d type %u \"%s\": local pos %10.2f %9.2f %9.2f"
                 "  scale %8.3f %8.3f %8.3f  rot %8.2f %8.2f %8.2f -> x %11.2f\n",
                 mdl, depth, (unsigned)o->type, o->name ? o->name : "?", t->pos.x,
                 t->pos.y, t->pos.z, t->scale.x, t->scale.y, t->scale.z, t->rot.x,
                 t->rot.y, t->rot.z, here[0][3]);
    }
    for (i = 0; i < (int)o->mesh.childrenCount && i < 512; i++) {
        if (o->mesh.children && o->mesh.children[i]) {
            walk_objects(mdl, hsf, o->mesh.children[i], here, depth + 1, use_curr,
                         reported);
        }
    }
}

/* --ovllog: the scene, named, every time it changes.
 *
 * `omcurovl` is `ref/notes.md` §7's "single best where-am-I watch", and a
 * scripted walk is otherwise blind: the only way to know which screen frame
 * 1,430's A press landed on was to shoot the frame and look at it.  This
 * turns that into one line per screen transition. */

void port_ovllog(void) {
    static int last_ovl = -12345;
    static int last_evt = -12345;
    if (!port_opt.ovllog) {
        return;
    }
    if (omcurovl == last_ovl && omovlevtno == last_evt) {
        return;
    }
    last_ovl = omcurovl;
    last_evt = omovlevtno;
    port_log("port> frame %u: overlay %d (next %d) event %d\n", gl13_frame_number(),
             (int)omcurovl, (int)omnextovl, (int)omovlevtno);
}

void port_scenelog(void) {
    int i;
    int shown;
    int live;
    unsigned f = gl13_frame_number();
    const char* p;
    int wanted = 0;
    if (!port_opt.scenelog) {
        return;
    }
    for (p = port_opt.scenelog; *p;) {
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p++ - '0');
        }
        if (v == f) {
            wanted = 1;
        }
        while (*p && (*p < '0' || *p > '9')) {
            p++;
        }
    }
    if (!wanted) {
        return;
    }
    port_log("---- scenelog: frame %u, overlay %d ----\n", f, (int)omcurovl);
    for (i = 0; i < HU3D_CAM_MAX; i++) {
        const HU3DCAMERA* c = &Hu3DCamera[i];
        if (c->fov == -1.0f) {
            continue;
        }
        port_log("  cam%-2d fov %7.2f near %9.2f far %10.2f  pos %11.2f %9.2f %9.2f\n",
                 i, c->fov, c->near, c->far, c->pos.x, c->pos.y, c->pos.z);
        port_log("        up %8.3f %7.3f %7.3f  target %11.2f %9.2f %9.2f\n", c->up.x,
                 c->up.y, c->up.z, c->target.x, c->target.y, c->target.z);
    }
    shown = 0;
    live = 0;
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        const HU3DMODEL* d = &Hu3DData[i];
        float ax = d->pos.x < 0 ? -d->pos.x : d->pos.x;
        float mx = d->mtx[0][3] < 0 ? -d->mtx[0][3] : d->mtx[0][3];
        if (d->hsf == 0) {
            continue;
        }
        live++;
        /* The confetti on these screens is fifty models of nothing; what a
         * stage offset looks like is a translation nothing else comes near. */
        (void)ax;
        (void)mx;
        if (shown >= 200) {
            continue;
        }
        shown++;
        port_log("  mdl%-3d attr %08x lay %d camBit %04x camInfo %u motId %4d  "
                 "pos %12.2f %9.2f %9.2f  scale %7.3f %7.3f %7.3f  "
                 "mtxT %12.2f %9.2f %9.2f\n",
                 i, (unsigned)d->attr, (int)d->layerNo, (unsigned)d->cameraBit,
                 (unsigned)d->camInfoBit, (int)d->motId, d->pos.x, d->pos.y, d->pos.z,
                 d->scale.x, d->scale.y, d->scale.z, d->mtx[0][3], d->mtx[1][3],
                 d->mtx[2][3]);
        if (d->mtx[0][0] != 1.0f || d->mtx[1][1] != 1.0f || d->mtx[2][2] != 1.0f ||
            d->mtx[0][1] != 0.0f || d->mtx[0][2] != 0.0f) {
            port_log("         mtx %8.3f %8.3f %8.3f %10.2f / %8.3f %8.3f %8.3f "
                     "%10.2f / %8.3f %8.3f %8.3f %10.2f  rot %8.2f %8.2f %8.2f\n",
                     d->mtx[0][0], d->mtx[0][1], d->mtx[0][2], d->mtx[0][3],
                     d->mtx[1][0], d->mtx[1][1], d->mtx[1][2], d->mtx[1][3],
                     d->mtx[2][0], d->mtx[2][1], d->mtx[2][2], d->mtx[2][3], d->rot.x,
                     d->rot.y, d->rot.z);
        }
    }
    port_log("  %d live models, %d shown\n", live, shown);
    /* Where a huge modelview translation can come from once the camera and
     * the model's own placement are ruled out: the HSF object hierarchy that
     * Hu3DDraw walks (src/game/hsfdraw.c objMesh), whose per-object
     * transforms are concatenated down the tree. */
    shown = 0;
    for (i = 0; i < HU3D_MODEL_MAX && shown < 20; i++) {
        const HU3DMODEL* d = &Hu3DData[i];
        const HSFDATA* hsf = d->hsf;
        float maxb = 0.0f, maxc = 0.0f;
        int j, cenv = 0, nobj;
        if (hsf == 0 || (d->attr & (HU3D_ATTR_HOOK | HU3D_ATTR_HOOKFUNC))) {
            continue;
        }
        nobj = hsf->objectNum;
        if (nobj <= 0 || nobj > 4096 || !hsf->object) {
            continue;
        }
        for (j = 0; j < nobj; j++) {
            const HSFOBJECT* o = &hsf->object[j];
            float b, c;
            if (o->type != 2 && o->type != 3 && o->type != 4 && o->type != 5) {
                continue;
            }
            b = (float)fabs(o->mesh.base.pos.x);
            c = (float)fabs(o->mesh.curr.pos.x);
            if (b > maxb) {
                maxb = b;
            }
            if (c > maxc) {
                maxc = c;
            }
            if (o->mesh.cenvNum) {
                cenv++;
            }
        }
        if (maxb < 2000.0f && maxc < 2000.0f) {
            continue;
        }
        shown++;
        port_log("  hsf mdl%-3d objs %4d cenvObjs %3d  max|base.pos.x| %10.2f  "
                 "max|curr.pos.x| %10.2f  matrixNum %d\n",
                 i, nobj, cenv, maxb, maxc, (int)hsf->matrixNum);
    }
    if (!shown) {
        port_log("  no model's HSF objects carry an x over 2000\n");
    }
    /* The envelope matrices.  `objMesh` does not use the object's own
     * transform for a skinned mesh at all -- it takes `hsf->matrix->data[i +
     * base_idx]`, which `EnvelopeExec.c` fills.  If those are wrong nothing in
     * the object tree shows it. */
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        const HU3DMODEL* d = &Hu3DData[i];
        const HSFDATA* hsf = d->hsf;
        float mx = 0.0f;
        int j, n;
        if (hsf == 0 || (d->attr & (HU3D_ATTR_HOOK | HU3D_ATTR_HOOKFUNC))) {
            continue;
        }
        if (!hsf->matrix || hsf->cenvNum == 0) {
            continue;
        }
        n = (int)hsf->matrix->count;
        if (n <= 0 || n > 4096 || !hsf->matrix->data) {
            port_log("  cenv mdl%-3d matrix header looks wrong: count %d data %p\n", i,
                     n, (void*)(hsf->matrix ? hsf->matrix->data : 0));
            continue;
        }
        for (j = 0; j < n; j++) {
            float v = (float)fabs(hsf->matrix->data[j][0][3]);
            if (v > mx) {
                mx = v;
            }
        }
        port_log("  cenv mdl%-3d cenvNum %d base_idx %u count %d  max|mtx[0][3]| %12.2f\n",
                 i, (int)hsf->cenvNum, (unsigned)hsf->matrix->base_idx, n, mx);
    }
    {
        int reported = 0;
        Mtx id;
        C_MTXIdentity(id);
        for (i = 0; i < HU3D_MODEL_MAX && reported <= 6; i++) {
            const HU3DMODEL* d = &Hu3DData[i];
            const HSFDATA* hsf = d->hsf;
            if (hsf == 0 || (d->attr & (HU3D_ATTR_HOOK | HU3D_ATTR_HOOKFUNC))) {
                continue;
            }
            if (!hsf->root || !hsf->object || hsf->objectNum <= 0 ||
                hsf->objectNum > 4096) {
                continue;
            }
            walk_objects(i, hsf, hsf->root, id, 0, d->motId != -1, &reported);
        }
        if (!reported) {
            port_log("  the HSF trees concatenate to nothing over 3000 in x\n");
        }
    }
    port_log("  cammtx %8.3f %8.3f %8.3f %12.2f\n", Hu3DCameraMtx[0][0],
             Hu3DCameraMtx[0][1], Hu3DCameraMtx[0][2], Hu3DCameraMtx[0][3]);
    port_log("         %8.3f %8.3f %8.3f %12.2f\n", Hu3DCameraMtx[1][0],
             Hu3DCameraMtx[1][1], Hu3DCameraMtx[1][2], Hu3DCameraMtx[1][3]);
    port_log("         %8.3f %8.3f %8.3f %12.2f\n", Hu3DCameraMtx[2][0],
             Hu3DCameraMtx[2][1], Hu3DCameraMtx[2][2], Hu3DCameraMtx[2][3]);
}

/* Which object a loaded position matrix belongs to.
 *
 * `Hu3DDrawPost` and `ObjDraw` both call `GXLoadPosMtxImm(drawObj->matrix,
 * GX_PNMTX0)`, and `drawObj->matrix` is a member of a `HU3DDRAWOBJ`, so the
 * pointer GX was handed *is* the draw object, minus the offset of the matrix
 * field.  The port can therefore name the model and the HSF object behind any
 * draw without the game telling it anything -- which turns "1,264 of 1,756
 * draws are off the side of the world" into a list of names.  Guarded on both
 * back-pointers landing inside `Hu3DData`, so a matrix loaded from anywhere
 * else simply reports nothing. */
const char* port_drawobj_name(const void* mtx, int* model_index) {
    const char* base = (const char*)mtx - offsetof(HU3DDRAWOBJ, matrix);
    const HU3DDRAWOBJ* d = (const HU3DDRAWOBJ*)base;
    long idx;
    *model_index = -1;
    if (!mtx || ((uintptr_t)base & 3u)) {
        return NULL;
    }
    if (d->model < &Hu3DData[0] || d->model >= &Hu3DData[HU3D_MODEL_MAX]) {
        return NULL;
    }
    idx = (long)(d->model - &Hu3DData[0]);
    *model_index = (int)idx;
    if (!d->object) {
        return NULL;
    }
    return d->object->name;
}

/* --nanwatch: the first frame each 3D slot turns NaN, and nothing after.
 *
 * A NaN in a camera position is silent: nothing crashes, nothing warns, every
 * comparison against it is false, and the whole 3D layer simply stops drawing
 * because every transformed vertex is NaN.  The board looked like "the map
 * does not render" for exactly that reason (PLAN.md 15.2).  Once a NaN is in
 * a persistent struct it stays, so the useful thing is the *first* frame -- one
 * line per slot, then silence -- which turns "somewhere in w01dll" into a
 * window of a few frames and the overlay that owned them.
 *
 * The board's own camera is behind `static BoardCameraData boardCamera` in
 * src/game/board/main.c, which the port cannot name; its getters are exported,
 * they only copy out of that struct, and they are the difference between "the
 * target model moved to NaN" and "the interpolator produced NaN". */

static int nan_f(float v) { return v != v; }

static void port_boardcam_dump(const BoardCameraData* c) {
    const BoardFocusData* k = &c->focus;
    port_log("        target %g %g %g   pos %g %g %g   offset %g %g %g\n", c->target.x,
             c->target.y, c->target.z, c->pos.x, c->pos.y, c->pos.z, c->offset.x,
             c->offset.y, c->offset.z);
    port_log("        rot %g %g %g  zoom %g fov %g near %g far %g  target_mdl %d "
             "target_space %d moving %u\n",
             c->rot.x, c->rot.y, c->rot.z, c->zoom, c->fov, c->near, c->far,
             (int)c->target_mdl, (int)c->target_space, (unsigned)c->moving);
    port_log("        focus type %d time %d/%d  target_start %g %g %g  target_end %g %g %g\n",
             (int)k->view_type, (int)k->time, (int)k->max_time, k->target_start.x,
             k->target_start.y, k->target_start.z, k->target_end.x, k->target_end.y,
             k->target_end.z);
    port_log("        focus zoom %g->%g fov %g->%g rot_start %g %g %g rot_end %g %g %g\n",
             k->zoom_start, k->zoom_end, k->fov_start, k->fov_end, k->rot_start.x,
             k->rot_start.y, k->rot_start.z, k->rot_end.x, k->rot_end.y, k->rot_end.z);
}

static int nan_vec(const HuVecF* v) { return nan_f(v->x) || nan_f(v->y) || nan_f(v->z); }

void port_nanwatch(void) {
    static u8 cam_said[HU3D_CAM_MAX];
    static u8 mdl_said[HU3D_MODEL_MAX];
    static u8 board_said;
    unsigned f;
    int i;
    if (!port_opt.nanwatch) {
        return;
    }
    f = gl13_frame_number();
    for (i = 0; i < HU3D_CAM_MAX; i++) {
        const HU3DCAMERA* c = &Hu3DCamera[i];
        if (c->fov == -1.0f || cam_said[i]) {
            continue;
        }
        if (nan_vec(&c->pos) || nan_vec(&c->target) || nan_vec(&c->up) ||
            nan_f(c->fov) || nan_f(c->near) || nan_f(c->far)) {
            cam_said[i] = 1;
            port_log("port> nanwatch frame %u overlay %d: cam%d pos %g %g %g "
                     "target %g %g %g up %g %g %g fov %g\n",
                     f, (int)omcurovl, i, c->pos.x, c->pos.y, c->pos.z, c->target.x,
                     c->target.y, c->target.z, c->up.x, c->up.y, c->up.z, c->fov);
        }
    }
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        const HU3DMODEL* d = &Hu3DData[i];
        if (d->hsf == 0 || mdl_said[i]) {
            continue;
        }
        if (nan_vec(&d->pos) || nan_vec(&d->rot) || nan_vec(&d->scale) ||
            nan_f(d->mtx[0][3]) || nan_f(d->mtx[1][3]) || nan_f(d->mtx[2][3])) {
            mdl_said[i] = 1;
            port_log("port> nanwatch frame %u overlay %d: mdl%d pos %g %g %g "
                     "rot %g %g %g scale %g %g %g mtxT %g %g %g\n",
                     f, (int)omcurovl, i, d->pos.x, d->pos.y, d->pos.z, d->rot.x,
                     d->rot.y, d->rot.z, d->scale.x, d->scale.y, d->scale.z,
                     d->mtx[0][3], d->mtx[1][3], d->mtx[2][3]);
        }
    }
    if (omcurovl == PORT_BOARD_OVL_W01) {
        /* `boardCamera` is extern, so the port can read the whole thing rather
         * than the four values the getters expose -- and the useful pair is
         * this frame's and the previous one's, because the interesting question
         * is which field went first. */
        static BoardCameraData prev;
        static int have_prev;
        const BoardCameraData* c = &boardCamera;
        int bad = nan_vec((const HuVecF*)&c->target) || nan_vec((const HuVecF*)&c->pos) ||
                  nan_vec((const HuVecF*)&c->offset) || nan_vec((const HuVecF*)&c->rot) ||
                  nan_f(c->zoom) || nan_f(c->fov);
        if (bad && !board_said) {
            board_said = 1;
            if (have_prev) {
                port_log("port> nanwatch frame %u: boardCamera the frame BEFORE:\n", f - 1);
                port_boardcam_dump(&prev);
            }
            port_log("port> nanwatch frame %u: boardCamera NOW:\n", f);
            port_boardcam_dump(c);
        }
        prev = *c;
        have_prev = 1;
    }
}
