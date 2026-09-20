/* M28 experiment (a): the material walk with nothing drawn (PLAN.md 43).
 *
 * On a consumed frame (frame mode, PLAN.md 32.1) the GX calls are dropped at
 * the port's door, but hsfdraw.c's `FaceDraw` still *decides* every one of
 * them: the blend mode, the channel colours, the Z/alpha/cull setup, sixteen
 * konst-alpha selects, the vertex descriptor, the texture loads (LoadTexture
 * initialises a GXTexObj and loads it; HuSprTexLoad the same for the
 * reflection, toon, shadow, projection and hilite maps), and then
 * `SetTevStageNoTex` / `SetTevStageTex` -- texgens, TEV stages, konst
 * colours, texture matrices with real matrix arithmetic behind them -- and
 * finally `GXCallDisplayList`.  §32.4 measured that at 449 samples of a
 * consumed board frame's 7,200 (FaceDraw inclusive).
 *
 * What of it does any game logic read back?  Everything FaceDraw writes was
 * read for this file (src/game/hsfdraw.c, src/game/hsfman.c lightSet,
 * src/game/hsfanim.c Hu3DAnimSet, src/game/sprput.c HuSprTexLoad) and
 * grepped for readers in src/game and src/REL:
 *
 *   - hsfdraw.c's own statics (materialBak, vtxModeBak, shadingBak, lightBit,
 *     BmpPtrBak[], texCol[], kColor/kColorIdx, TL32F, the *MapNo slots,
 *     reflectionMapNo...): every one is reset per model (Hu3DDraw), per
 *     object (ObjDraw / Hu3DDrawPost) or per material before it is read
 *     again, so nothing survives into the next drawn frame's walk.  Two of
 *     them, shadingBak and lightBit, gate a call with a side effect and are
 *     made extern by the patch so the gate is kept exactly (below).
 *   - `drawCnt`: FaceDraw's caller indexes DrawData[drawCnt - 1] to step
 *     through the faces, so the increment is kept.
 *   - `totalMatCnt` / `totalTexCnt` / `totalTexCacheCnt` / `totalPolyCnt`:
 *     copied once a frame into the *Cnted twins (hsfman.c:124) and printed
 *     by the debug overlay (objmain.c:492) and by nothing else.  totalMatCnt
 *     (per material change) and totalPolyCnt (in the caller) are kept; the
 *     two texture counters are the walk's own LoadTexture bookkeeping and
 *     are the one difference a snapshot shows (a debug-print value).
 *   - `lightSet` (hsfman.c) for a light of type 1 writes
 *     `light->pos = light->dir * -1e6` into Hu3DGlobalLight / Hu3DLocalLight,
 *     which fourteen REL modules read (mostly to set; m425's own light set
 *     reads .pos).  That write is idempotent in `dir`, but a consumed frame
 *     that skips it would leave `pos` stale after a `dir` change until the
 *     next drawn frame, so the call is kept, under the game's own gate
 *     (shading != shadingBak, per material, per object).
 *   - `Hu3DAnimSet` (hsfanim.c) writes the 2D texture animation's scale and
 *     trans into the attribute's HU3DATTRANIM, read by SetTevStageTex alone
 *     -- in the same FaceDraw, after a fresh call -- so a skip would be
 *     exact for the picture; it is kept anyway so a snapshot of the heap is
 *     identical too (it is a few float divides).
 *   - `constData->matrix`, the hook-model matrices, MTXBuf: written by
 *     objMesh, which is the *object* walk and is not touched (Hu3DModelObjMtxGet
 *     reads it, PLAN.md 33.3).
 *
 * So the patch (port/patches.txt, hsfdraw.c FaceDraw) asks
 * `port_consumed_frame()` right after the material is known, and on a
 * consumed frame does only: the material-change bookkeeping, this file's
 * `port_face_consumed()` (the two side effects above, under the game's own
 * conditions, in the game's order), `drawCnt++`, return.  On a drawn frame,
 * and under `--nomatwalk`, FaceDraw runs as written.
 */
#include "port.h"
#include "gx_internal.h"

#include "game/hu3d.h"

/* hsfdraw.c's statics, made extern by the patch: the gate of Hu3DLightSet */
extern s32 shadingBak;
extern s16 lightBit;

static unsigned long stat_faces, stat_materials, stat_lightsets, stat_animsets;

int port_consumed_frame(void) {
    if (port_opt.nomatwalk || !gl13_draw_off()) {
        return 0;
    }
    stat_faces++;
    return 1;
}

/* The side effects of FaceDraw's material setup, and nothing else.  Mirrors
 * hsfdraw.c FaceDraw -> (SetTevStageNoTex | the texture loop + SetTevStageTex)
 * for the two writes that reach game memory; every condition is the game's. */
void port_face_consumed(HU3DDRAWOBJ* drawObj, HSFMATERIAL* mat) {
    HSFOBJECT* object = drawObj->object;
    HU3DMODEL* model = drawObj->model;
    s32 shading;
    s16 matHiliteF = (mat->vtxMode == 2 || mat->vtxMode == 3);
    stat_materials++;
    if (mat->attrNum == 0) {
        /* SetTevStageNoTex: shading = matHiliteF ? 2 : vtxMode */
        shading = matHiliteF ? 2 : mat->vtxMode;
    } else {
        /* FaceDraw's texture loop: a 2D-animated attribute is re-sampled
         * through Hu3DAnimSet (which writes the anim's scale/trans) */
        s16 i;
        for (i = 0; i < mat->attrNum; i++) {
            HSFATTRIBUTE* attr = &object->mesh.attribute[mat->attr[i]];
            if (attr->animWorkP) {
                HU3DATTRANIM* dd = attr->animWorkP;
                HU3DTEXANIM* ta = &Hu3DTexAnimData[dd->animId];
                if ((dd->attr & HU3D_ATTRANIM_ATTR_ANIM2D) && !(ta->attr & HU3D_ANIM_ATTR_NOUSE)) {
                    Hu3DAnimSet(model, attr, i);
                    stat_animsets++;
                }
            }
        }
        /* SetTevStageTex: compares vtxMode itself */
        shading = mat->vtxMode;
    }
    if (shading != shadingBak) {
        shadingBak = shading;
        lightBit = Hu3DLightSet(model, &Hu3DCameraMtx, &Hu3DCameraMtxXPose,
                                matHiliteF ? mat->hiliteScale : 0.0f);
        stat_lightsets++;
    }
}

void port_matwalk_report(void) {
    if (stat_faces || port_opt.nomatwalk) {
        port_log("port> material walk on consumed frames: %s; %lu faces skipped, %lu material "
                 "changes (%lu Hu3DLightSet, %lu Hu3DAnimSet kept)\n",
                 port_opt.nomatwalk ? "the game's (--nomatwalk)" : "skipped", stat_faces,
                 stat_materials, stat_lightsets, stat_animsets);
    }
}
