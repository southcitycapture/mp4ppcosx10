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

/* M47: the vertex cache's arrays overlapping one skinned buffer, kept while
 * the cache's array table has not changed (gx_draw.c gx_vc_arrays_written) */
#define GX_VC_MARK_MAX 8
typedef struct GxVcMark {
    unsigned gen;                     /* 0: scan */
    int n;
    void* a[GX_VC_MARK_MAX];
} GxVcMark;
void gx_vc_arrays_written(const void* p, size_t n, GxVcMark* c);

/* M47 (PLAN.md 62): the skin at the decode -- what a decode job needs to
 * produce, per list vertex, exactly the value SetEnvelop would have left in
 * the mesh's arrays: the rest pose, which entry wrote each index last, and
 * that entry's matrix.  Per mesh; the job holds a pointer (gx_draw.c). */
#define GX_SKIN_UNNAMED 0xFFFF        /* no entry writes it: the live array's */
typedef struct GxSkinDec {
    const f32* rest_pos;              /* Vertextop: mesh.file[0]             */
    const f32* rest_nrm;              /* normtop: mesh.file[1]               */
    const u8* live_pos;               /* mesh.vertex->data (vtxenv)          */
    const u8* live_nrm;               /* mesh.normal->data (normenv)         */
    const u16* pent;                  /* per position: 0 the copy (rest), e >= 1
                                       * entry e's matrix, GX_SKIN_UNNAMED    */
    const u16* nent;                  /* per normal: e >= 1, GX_SKIN_UNNAMED  */
    const f32* P;                     /* entry e's position matrix at P + 12e */
    const f32* N;                     /* ... and normal matrix at N + 12e     */
    u32 npos, nnrm;
    const f32* PV;                    /* --skinvec: entry e's columns C0..C3 of
                                       * P at PV + 16e (16-byte aligned)      */
    const f32* NV;
} GxSkinDec;

typedef struct SkinMesh {
    struct SkinHsf* owner;
    struct HsfObject_s* obj;
    int objIdx, meshNo;
    const void* vtxenv;               /* mesh.vertex->data: the lookup key   */
    const void* normenv;
    const void* cenv;                 /* mesh.cenv, for the lifetime check   */
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
    struct SkinMesh* nhnext;          /* M47: the normals' chain */
    /* the shape, for --skinstats */
    int n_single, n_dual, n_dual_pairs, n_multi, n_bones, multi_max_w;
    unsigned long v_single, v_dual, v_multi, v_copy, v_unnamed;
    unsigned overlap;
    /* M47: SetEnvelop's writes, index by index (the copy and the posNum == 1
     * single's lone normal included), and whether the mesh can be skinned
     * at the decode at all (no multi entry, no range past the arrays, the
     * arrays not the file's own) */
    u16* fpent;
    u16* fnent;
    f32* PVbuf;                       /* --skinvec: the columns (GxSkinDec)  */
    GxVcMark vmark_pos, vmark_nrm;    /* M47: the cache's arrays over them   */
    f32* NVbuf;
    int fuse_ok;
    GxSkinDec dec;
} SkinMesh;

typedef struct SkinHsf {
    struct HsfData_s* hsf;
    u32 signature;
    u32 cheap_sig;                    /* M47: the pointers and counts alone  */
    unsigned sig_calls;               /* M47: calls since the full signature */
    /* what the HSF pointed at when it was registered (M19): the lifetime
     * hook matches frees against these, and a draw-time read re-checks them
     * against the HSF before following anything */
    struct HsfObject_s* object;
    struct HsfMatrix_s* matrix;
    u32 objectNum;
    unsigned serial;                  /* bumped by every EnvelopeProc        */
    int mtx_dirty;                    /* SetEnvelopMtx owed for `serial`     */
    int skin_dirty;                   /* SetEnvelopMain owed (deferred CPU)  */
    int cpu;                          /* this HSF skins on the CPU           */
    unsigned last_frame;
    int nmesh;
    SkinMesh* mesh;
    /* M29 (PLAN.md 44.1 rule 2): the stream position after the last decode
     * record that read one of this HSF's buffers; the body joins on it */
    unsigned dec_pos;
    int dec_valid;
    /* M47: every mesh can be skinned at the decode; and `owed`: the pose is
     * built into the meshes' matrices but the arrays were not written */
    int fuse_ok;
    int owed;
} SkinHsf;

int gx_skin_on(void);                 /* the palette path is skinning         */
int gx_skin_mode(void);               /* 0 --cpuskin, 1 deferred CPU, 2 palette */
void gx_skin_array_bound(const void* pos_array); /* GXSetArray(GX_VA_POS)    */
void gx_skin_stamp_decode(unsigned pos);  /* M29: a decode record was emitted
                                            * under the bound position array  */
int gx_skin_palette_slots(void);      /* gx_vprog.c: slots the layout holds  */
int gx_vprog_palette_available(void); /* gx_vprog.c: ARL loaded native      */
SkinMesh* gx_skin_lookup(const void* pos_array, unsigned frame);
const void* gx_skin_rest_pos(const SkinMesh* m);
const void* gx_skin_rest_nrm(const SkinMesh* m);
void gx_skin_pose(SkinMesh* m);
void gx_skin_count_vertex(const SkinMesh* m, unsigned pos_ix, unsigned nrm_ix, int have_nrm);
void gx_skin_frame_end(void);
/* M47: the primitive in hand reads these arrays (NULL: not indexed); returns
 * the mesh's skin for the decode when `can_fuse` and the arrays are owed,
 * else writes any owed arrays first (the game's body) and returns NULL */
const GxSkinDec* gx_skin_fuse_for(const void* pos_base, const void* nrm_base, int can_fuse,
                                  int nrm_ok, int* nrm_skinned);
int gx_skin_fuse_live(void);          /* any HSF registered, the skin decode on */
void gx_skin_fuse_stamp(unsigned pos);    /* a job of the last mesh fused was recorded */
void gx_skin_fuse_verify(const GxSkinDec* d, const u8* list, u32 n, u32 vbytes, int pos_off,
                         int nrm_off, const u8* v, u32 stride, int off_nrm, int nrm_skinned);
void gx_skin_report(void);

/* M47: SetEnvelop's multiply, the expression of all three of its PSMTX
 * multipliers (psmtx_c.c: fmuls m[1]*y, fmadds m[0]*x, fmadds m[2]*z,
 * fadds m[3]).  `m` is a row-major 3x4 matrix.  Shared by the decode's loops
 * and the materializer so the two cannot drift. */
static inline void gx_skin_mul(const f32* m, const f32* r, f32* o) {
    f32 x = r[0], y = r[1], z = r[2];
    f32 ox = m[0] * x + m[1] * y + m[2] * z + m[3];
    f32 oy = m[4] * x + m[5] * y + m[6] * z + m[7];
    f32 oz = m[8] * x + m[9] * y + m[10] * z + m[11];
    o[0] = ox;
    o[1] = oy;
    o[2] = oz;
}
/* --skinround (M47 item 4's measurement): each component as one correctly
 * rounded dot product (in double, rounded once) -- a result a card's DP4
 * could give; not the game's bits */
static inline void gx_skin_mul_round(const f32* m, const f32* r, f32* o) {
    double x = r[0], y = r[1], z = r[2];
    f32 ox = (f32)((double)m[0] * x + (double)m[1] * y + (double)m[2] * z + (double)m[3]);
    f32 oy = (f32)((double)m[4] * x + (double)m[5] * y + (double)m[6] * z + (double)m[7]);
    f32 oz = (f32)((double)m[8] * x + (double)m[9] * y + (double)m[10] * z + (double)m[11]);
    o[0] = ox;
    o[1] = oy;
    o[2] = oz;
}
void gx_skin_rewrite_notify(const void* pos_array); /* a rewriter is about to write it */
void gx_skin_rewrite_notify_all(void);
void gx_skin_fuse_count(unsigned long verts);

/* the two hooks port/patches.txt plants in the game */
int port_envelope_proc(struct HsfData_s* hsf);
void port_envelope_sync(struct HsfData_s* hsf);

#endif
