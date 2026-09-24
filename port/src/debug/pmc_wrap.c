/* M43 (PLAN.md 58.4): the --pmc measurement tree's wrappers.  Built only
 * with PMC_WRAP=1 (port/Makefile), where patches-pmc.txt has renamed each
 * of these game bodies *_game; each wrapper reads the counters in and out
 * and calls the body with its arguments, nothing else. */
#ifdef PORT_PMC_WRAP
#include "port.h"
#include <dolphin/types.h>
#include "game/hu3d.h"
#include "game/process.h"

void Hu3DExec_game(void);
void Hu3DShadowExec_game(void);
void Hu3DDraw_game(HU3DMODEL* modelP, Mtx mtx, HuVecF* scale);
void Hu3DDrawPost_game(void);
void Hu3DModelObjMtxGet_game(HU3DMODELID modelId, char* objName, Mtx mtx);
void HuPrcCall_game(s32 tick);
void Hu3DMotionExec_port(s16 arg0, s16 arg1, float arg2, s32 arg3);

void Hu3DExec(void) {
    port_pmc_enter(PMC_R_3DEXEC);
    Hu3DExec_game();
    port_pmc_leave();
}
void Hu3DShadowExec(void) {
    port_pmc_enter(PMC_R_SHADOW);
    Hu3DShadowExec_game();
    port_pmc_leave();
}
void Hu3DDraw(HU3DMODEL* modelP, Mtx mtx, HuVecF* scale) {
    port_pmc_enter(PMC_R_DRAW);
    Hu3DDraw_game(modelP, mtx, scale);
    port_pmc_leave();
}
void Hu3DDrawPost(void) {
    port_pmc_enter(PMC_R_DRAWPOST);
    Hu3DDrawPost_game();
    port_pmc_leave();
}
void Hu3DModelObjMtxGet(HU3DMODELID modelId, char* objName, Mtx mtx) {
    port_pmc_enter(PMC_R_OBJMTX);
    Hu3DModelObjMtxGet_game(modelId, objName, mtx);
    port_pmc_leave();
}
void HuPrcCall(s32 tick) {
    port_pmc_enter(PMC_R_PRC);
    HuPrcCall_game(tick);
    port_pmc_leave();
}
void Hu3DMotionExec(s16 arg0, s16 arg1, float arg2, s32 arg3) {
    port_pmc_enter(PMC_R_MOTION);
    Hu3DMotionExec_port(arg0, arg1, arg2, arg3);
    port_pmc_leave();
}
#endif
