/* The skinning registry (gx_skin.c) as gx_draw.c and gx_vprog.c see it.
 * PLAN.md 33. */
#ifndef PORT_GX_SKIN_H
#define PORT_GX_SKIN_H

#include "port.h"
#include "dolphin/mtx.h"

struct HsfData_s;
struct HsfObject_s;
struct HsfCenvMulti_s;

/* program.env params per palette slot: three rows of the position matrix
 * (3x4) and three of the normal matrix (3x3) */
#define GX_PAL_STRIDE 6
#define GX_PAL_SLOTS_MAX 24

enum { SKIN_ENT_IDENTITY = 0, SKIN_ENT_SINGLE, SKIN_ENT_DUAL, SKIN_ENT_MULTI };

typedef struct SkinEnt {
    u8 kind;
    u32 t1, t2;                       /* bone (object) indices               */
    f32 w;                            /* dual: target1's weight              */
    const struct HsfCenvMulti_s* multi;
} SkinEnt;

typedef struct SkinMtx {
    Mtx m;
} SkinMtx;

typedef struct SkinMesh {
    struct SkinHsf* owner;
    struct HsfObject_s* obj;
    int objIdx, meshNo;
    const void* vtxenv;               /* mesh.vertex->data: the lookup key   */
    const void* normenv;
    int nvtx, nnrm;
    int nent;
    SkinEnt* ent;
    u16* pos_ent;                     /* position index -> entry             */
    u16* nrm_ent;                     /* normal index -> entry               */
    SkinMtx* P;                       /* per entry, the pose of pose_serial  */
    SkinMtx* N;
    unsigned pose_serial;
    int fallback;                     /* unused since the window (M18.1)     */
    /* the batch window (gx_draw.c pal_place): which entries hold a slot in
     * the pending batch, valid while map_batch/map_pose/map_posm match */
    f32* ent_slotf;                   /* per entry: GX_PAL_STRIDE*slot, or -1 */
    unsigned map_pose;
    f32 map_posm[12];
    f32 map_nrmm[9];
    struct SkinMesh* hnext;
    /* the shape, for --skinstats */
    int n_single, n_dual, n_dual_pairs, n_multi, n_bones, multi_max_w;
    unsigned long v_single, v_dual, v_multi, v_copy, v_unnamed;
    unsigned overlap;
} SkinMesh;

typedef struct SkinHsf {
    struct HsfData_s* hsf;
    u32 signature;
    unsigned serial;                  /* bumped by every EnvelopeProc        */
    int mtx_dirty;                    /* SetEnvelopMtx owed for `serial`     */
    int skin_dirty;                   /* SetEnvelopMain owed (deferred CPU)  */
    int cpu;                          /* this HSF skins on the CPU           */
    unsigned last_frame;
    int nmesh;
    SkinMesh* mesh;
} SkinHsf;

int gx_skin_on(void);                 /* the palette path is skinning         */
int gx_skin_mode(void);               /* 0 --cpuskin, 1 deferred CPU, 2 palette */
void gx_skin_array_bound(const void* pos_array); /* GXSetArray(GX_VA_POS)    */
int gx_skin_palette_slots(void);      /* gx_vprog.c: slots the layout holds  */
int gx_vprog_palette_available(void); /* gx_vprog.c: ARL loaded native      */
SkinMesh* gx_skin_lookup(const void* pos_array, unsigned frame);
const void* gx_skin_rest_pos(const SkinMesh* m);
const void* gx_skin_rest_nrm(const SkinMesh* m);
void gx_skin_pose(SkinMesh* m);
void gx_skin_count_vertex(const SkinMesh* m, unsigned pos_ix, unsigned nrm_ix, int have_nrm);
void gx_skin_frame_end(void);
void gx_skin_report(void);

/* the two hooks port/patches.txt plants in the game */
int port_envelope_proc(struct HsfData_s* hsf);
void port_envelope_sync(struct HsfData_s* hsf);

#endif
