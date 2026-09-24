/* M42 (PLAN.md 57.3): Hu3DMotionExec, the same function with the game's
 * control flow, compiled as the port compiles its own code.
 *
 * The motion system runs on every frame, drawn or consumed, for every model
 * with a motion (a character is three calls: the motion, the overlay, the
 * shape), and M42's profile of m441's consumed frame puts it at 13%:
 * `Hu3DMotionExec` 5.7% self (the per-object reset loop and the track
 * switch, as M28 found), `GetObjTRXPtr` 2.6% (a nine-way switch called once
 * a track), `__memcpy` 1.1% (GCC calls memcpy for every 36-byte
 * `curr = base`), `GetCurve` 4.1%.
 *
 * This body is src/game/hsfmotion.c's, statement for statement: every
 * GetCurve / SetObj*Motion / GetCluster*Curve call is made under the same
 * condition, in the same order, on the same arguments (GetCurve has side
 * effects -- a Bezier track's `start`, the bitmap pointer -- so the calls
 * themselves must match, not only the values), and every store is the
 * same store.  What differs is how it is compiled: the counters are ints
 * (the game's s16 counters are sign-extended and compared every pass; the
 * counts are s16, so the same range), the struct copy is nine word moves
 * (a memcpy's bits, without the call), and GetObjTRXPtr's switch is inline.
 * patches.txt renames the game's body Hu3DMotionExec_game;
 * --nomotionexec calls it instead. */
#include <dolphin/types.h>
#include "port.h"

#include "game/ClusterExec.h"
#include "game/hu3d.h"

void Hu3DMotionExec_game(s16 arg0, s16 arg1, float arg2, s32 arg3);

static unsigned long mx_calls;

/* the game's `obj->mesh.curr = obj->mesh.base`: nine words, bit for bit */
static inline void copy_transform(HSFTRANSFORM* d, const HSFTRANSFORM* s) {
    u32* dw = (u32*)d;
    const u32* sw = (const u32*)s;
    dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2];
    dw[3] = sw[3]; dw[4] = sw[4]; dw[5] = sw[5];
    dw[6] = sw[6]; dw[7] = sw[7]; dw[8] = sw[8];
}

/* GetObjTRXPtr (hsfmotion.c), inline */
static inline float* trx_ptr(HSFOBJECT* arg0, u16 arg1) {
    HSFCONSTDATA* temp_r31 = arg0->constData;

    switch (arg1) {
    case 8:
        if (temp_r31 && (temp_r31->attr & 0x10)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.pos.x;
    case 9:
        if (temp_r31 && (temp_r31->attr & 0x20)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.pos.y;
    case 10:
        if (temp_r31 && (temp_r31->attr & 0x40)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.pos.z;
    case 28:
        if (temp_r31 && (temp_r31->attr & 0x80)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.rot.x;
    case 29:
        if (temp_r31 && (temp_r31->attr & 0x100)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.rot.y;
    case 30:
        if (temp_r31 && (temp_r31->attr & 0x200)) {
            return (float*)-1;
        }
        return &arg0->mesh.curr.rot.z;
    case 31:
        return &arg0->mesh.curr.scale.x;
    case 32:
        return &arg0->mesh.curr.scale.y;
    case 33:
        return &arg0->mesh.curr.scale.z;
    default:
        return (float*)-1;
    }
}

#ifdef PORT_PMC_WRAP
/* M43: the measurement build's --pmc region wraps this (src/debug/pmc_wrap.c) */
#define Hu3DMotionExec Hu3DMotionExec_port
#endif
void Hu3DMotionExec(s16 arg0, s16 arg1, float arg2, s32 arg3) {
    HU3DMOTION* sp18;
    HSFDATA* sp14;
    HSFTRACK* sp10;
    HSFCONSTDATA* temp_r28;
    HSFDATA* temp_r29;
    HSFMOTION* temp_r21;
    HSFOBJECT* temp_r31;
    HSFOBJECT* var_r19;
    HSFCLUSTER* var_r23;
    HSFTRACK* var_r30;
    HSFTRACK* temp_r25;
    HSFTRACK* temp_r22;
    HSFTRACK* var_r26;
    HU3DMODEL* temp_r27;
    s16 temp_r24;
    int var_r18, nobj;
    float* temp_r17;

    if (port_opt.nomotionexec) {
        Hu3DMotionExec_game(arg0, arg1, arg2, arg3);
        return;
    }
    mx_calls++;
    temp_r27 = &Hu3DData[arg0];
    sp18 = &Hu3DMotion[arg1];
    temp_r29 = temp_r27->hsf;
    sp14 = sp18->hsf;
    temp_r21 = sp14->motion;
    var_r30 = temp_r21->track;
    var_r19 = temp_r29->object;
    nobj = temp_r29->objectNum;
    if (arg3 == 0) {
        for (var_r18 = 0; var_r18 < nobj; var_r19++, var_r18++) {
            temp_r31 = var_r19;
            if (temp_r31->constData) {
                temp_r28 = temp_r31->constData;
                if (temp_r28->attr & 0x3F0) {
                    temp_r24 = temp_r28->attr;
                    if (!(temp_r24 & 0x10)) {
                        temp_r31->mesh.curr.pos.x = temp_r31->mesh.base.pos.x;
                    }
                    if (!(temp_r24 & 0x20)) {
                        temp_r31->mesh.curr.pos.y = temp_r31->mesh.base.pos.y;
                    }
                    if (!(temp_r24 & 0x40)) {
                        temp_r31->mesh.curr.pos.z = temp_r31->mesh.base.pos.z;
                    }
                    if (!(temp_r24 & 0x80)) {
                        temp_r31->mesh.curr.rot.x = temp_r31->mesh.base.rot.x;
                    }
                    if (!(temp_r24 & 0x100)) {
                        temp_r31->mesh.curr.rot.y = temp_r31->mesh.base.rot.y;
                    }
                    if (!(temp_r24 & 0x200)) {
                        temp_r31->mesh.curr.rot.z = temp_r31->mesh.base.rot.z;
                    }
                } else {
                    copy_transform(&temp_r31->mesh.curr, &temp_r31->mesh.base);
                }
            } else {
                copy_transform(&temp_r31->mesh.curr, &temp_r31->mesh.base);
            }
        }
    }
    sp10 = &var_r30[temp_r21->numTracks];
    for (; var_r30 < sp10; var_r30++) {
        switch (var_r30->type) {
        case 2:
            if (var_r30->target < temp_r29->objectNum && var_r30->target != -1) {
                temp_r31 = &temp_r29->object[var_r30->target];
                if (var_r30->channel == 0x28) {
                    temp_r31->mesh.mesh.baseMorph = GetCurve(var_r30, arg2);
                } else if (temp_r31->type == 7) {
                    if (temp_r27->attr & HU3D_ATTR_CAMERA_MOTON) {
                        SetObjCameraMotion(arg0, var_r30, GetCurve(var_r30, arg2));
                    }
                } else if (temp_r31->type == 8) {
                    SetObjLightMotion(arg0, var_r30, GetCurve(var_r30, arg2));
                } else if (var_r30->channel == 0x18) {
                    if (temp_r31->constData) {
                        temp_r28 = temp_r31->constData;
                        if (GetCurve(var_r30, arg2) == 1.0f) {
                            temp_r28->attr &= ~0x1000;
                        } else {
                            temp_r28->attr |= 0x1000;
                        }
                    }
                } else if (var_r30->channel == 0x1A) {
                    if (temp_r31->constData) {
                        temp_r28 = temp_r31->constData;
                        if (GetCurve(var_r30, arg2) == 1.0f) {
                            temp_r28->attr &= ~0x2000;
                        } else {
                            temp_r28->attr |= 0x2000;
                        }
                    }
                } else {
                    temp_r17 = trx_ptr(temp_r31, var_r30->channel);
                    if (temp_r17 != (float*)-1) {
                        *temp_r17 = GetCurve(var_r30, arg2);
                    }
                }
            }
            break;
        case 3:
            temp_r25 = var_r30;
            if (temp_r25->target < temp_r29->objectNum) {
                temp_r31 = &temp_r29->object[temp_r25->target];
                temp_r31->mesh.mesh.morphWeight[temp_r25->morphWeight] = GetCurve(temp_r25, arg2);
            }
            break;
        case 9:
            if (!(temp_r27->attr & HU3D_ATTR_CURVE_MOTOFF)) {
                if (var_r30->attrIdx < temp_r29->materialNum) {
                    SetObjMatMotion(arg0, var_r30, GetCurve(var_r30, arg2));
                }
            }
            break;
        case 5:
            if (!(temp_r27->attr & HU3D_ATTR_CURVE_MOTOFF)) {
                var_r23 = &temp_r29->cluster[var_r30->cluster];
                var_r23->index = GetClusterCurve(var_r30, arg2);
            }
            break;
        case 6:
            if (!(temp_r27->attr & HU3D_ATTR_CURVE_MOTOFF)) {
                temp_r22 = var_r30;
                var_r23 = &temp_r29->cluster[temp_r22->cluster];
                var_r23->weight[temp_r22->clusterWeight] = GetClusterWeightCurve(temp_r22, arg2);
            }
            break;
        case 10:
            var_r26 = var_r30;
            if (var_r26->cluster != -1 || !(temp_r27->attr & HU3D_ATTR_CURVE_MOTOFF)) {
                if (var_r26->attrIdx != -1 && var_r26->attrIdx < temp_r29->attributeNum) {
                    SetObjAttrMotion(arg0, var_r26, GetCurve(var_r26, arg2));
                }
            }
            break;
        }
    }
}

void port_motion_exec_report(void) {
    if (mx_calls) {
        port_log("port> motion exec (M42): %lu calls through the port's body%s\n", mx_calls,
                 port_opt.nomotionexec ? "" : "");
    } else if (port_opt.nomotionexec) {
        port_log("port> motion exec (M42): --nomotionexec, the game's body\n");
    }
}
