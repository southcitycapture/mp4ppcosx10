/* The envelope (skinning) path, intercepted at the port (M18, PLAN.md 33).
 *
 * The game skins every character mesh on the CPU every frame:
 * `EnvelopeProc` (src/game/EnvelopeExec.c) walks the bone hierarchy into
 * `hsf->matrix->data` (`SetEnvelopMtx`), then for each skinned mesh builds a
 * matrix per *envelope entry* -- a run of vertices bound to one bone
 * (`HSFCENVSINGLE`), to a weighted pair (`HSFCENVDUALWEIGHT`), or to a list
 * of bones (`HSFCENVMULTI`) -- and multiplies the rest-pose positions and
 * normals through it into the buffers the draw reads (`SetEnvelop`).  M17
 * measured that at 30% of a consumed board frame (§32.4): `SetEnvelopMtx`
 * 747 samples, `PSMTXROMultVecArray` 681, `Hu3DMtxScaleGet` 532.
 *
 * The fact that makes it deferrable: nothing but the draw reads what it
 * produces.  The skinned buffers (`mesh.vertex->data`, `mesh.normal->data`)
 * are read by `FaceDraw`'s `GXSetArray`, and the bone matrices by one line
 * of `objMesh` (hsfdraw.c:230, the mesh's own world matrix); the
 * `Hu3DModelObjMtxGet` family recomputes from the transforms (`PGObjCalc`)
 * and never touches `MtxTop`.  So on a *consumed* frame (frame mode, §32.1:
 * nothing drawn) every cycle of it is waste, and on a drawn frame it can run
 * as late as the first read.  That is the default now:
 *
 *  1. `port_envelope_proc(hsf)`, patched into the head of `EnvelopeProc`
 *     (port/patches.txt), marks the HSF dirty and returns 1 -- the game's
 *     body does not run.  `--cpuskin` returns 0 and the game skins as it
 *     always did (the A/B).
 *  2. `port_envelope_sync(hsf)`, patched into `objMesh` before its read of
 *     the bone matrices, and `gx_skin_array_bound(p)` from `GXSetArray`
 *     when a skinned mesh's position buffer is bound (hooked models are
 *     drawn inside their parent's walk, before their own EnvelopeProc of the
 *     frame): if the HSF is dirty and the frame is drawn, run the game's
 *     own body -- `SetEnvelopMtx` + `SetEnvelopMain`, made extern by the same
 *     patch, on the same inputs it would have had -- and mark it clean.  A
 *     body runs at most once per dirty mark, and every mark follows the
 *     game's own refresh of the buffers it reads (InitVtxParm/ClusterProc),
 *     which is what keeps the in-place cluster case exact.  On a consumed
 *     frame nothing runs: the value would feed a `GXLoadPosMtxImm` the frame
 *     mode drops.  Bit-identical to the game by construction; the §32 md5s
 *     say so (PLAN.md 33.3).
 *
 * The other half of this file is the GPU path M18 was briefed to build: the
 * per-entry matrices computed here with the very PSMTX calls `SetEnvelop`
 * uses, uploaded as a vertex-program palette indexed per vertex, the card
 * doing the one multiply (`--palette`, gx_draw.c pal_place, gx_vprog.c).
 * It works and it is exact to rounding -- and it is slower on this driver,
 * because the Radeon 9000's ARL relative addressing runs in software
 * (PLAN.md 33.2).  It stays, opt-in, as the measured answer to §32.6 item 2.
 *
 * `--skinstats` prints each registered mesh's shape (entries per path, distinct
 * bones, vertices) and the per-frame totals.
 *
 * M19 (PLAN.md 34): the registry knows when the game frees a model
 * (`port_mem_freed`, from HuMemMemoryFree) and checks every pointer it
 * follows at draw time -- see "lifetime" below.  Without that, a model
 * freed while its entry was dirty (its last EnvelopeProc on a consumed
 * frame) was read through at the next bind of a reused address: the M18
 * soak's fault at 0x8200ad0a.
 */
#include "gx_internal.h"
#include "gx_skin.h"

#include "game/hsfformat.h"
#include "game/hsfex.h"
#include "dolphin/mtx.h"

unsigned gl13_frame_number(void);

#include <stdlib.h>
#include <string.h>

static int fuse_mode(void);           /* M47 */
extern int gx_skin_fuse_live_flag;    /* M47 */

/* the game's, made visible by port/patches.txt */
void SetEnvelopMtx(HSFOBJECT* arg0, HSFOBJECT* arg1, Mtx arg2);
void SetEnvelopMain(HSFDATA* arg0);
extern Mtx* MtxTop;
extern u32 nObj;
extern u32 nMesh;

/* ---- the registry ---------------------------------------------------------- */

#define SKIN_HSF_MAX 48
#define SKIN_HASH 256

static SkinHsf hsfs[SKIN_HSF_MAX];
static int nhsfs;
static SkinMesh* mesh_hash[SKIN_HASH]; /* by vtxenv pointer, chained */
static SkinMesh* nrm_hash[SKIN_HASH];  /* M47: by normenv pointer, chained */

static unsigned stat_proc_calls, stat_proc_deferred, stat_proc_cpu, stat_sync_runs,
    stat_sync_skipped_consumed, stat_pose_builds, stat_registered, stat_rebuilt,
    stat_fallback_hsf, stat_body_runs, stat_body_at_bind;
/* M19: entries dropped because the game freed their model, and reads the
 * guards refused (a pointer outside MEM1, or an HSF that no longer describes
 * what was registered).  The second number must stay 0. */
static unsigned stat_freed_drops, stat_guard_hits;
/* --noskinlifetime: what the M18 registry would have done -- entries left
 * behind by a free (and how many were dirty, i.e. armed to read at the next
 * bind), and binds that matched such an entry and read through it */
static unsigned stat_lever_stale_left, stat_lever_stale_dirty, stat_lever_stale_reads;
/* per frame, from the decode: vertices decoded from rest arrays by path */
static unsigned long stat_verts_single, stat_verts_dual, stat_verts_multi,
    stat_verts_copy, stat_verts_nrm_mismatch;
static unsigned stat_frames_with_skin;
static unsigned long frame_skin_verts, worst_frame_skin_verts;

static unsigned hash_ptr(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return (unsigned)((v >> 4) ^ (v >> 12) ^ (v >> 20)) & (SKIN_HASH - 1);
}

static void mesh_unhash(SkinMesh* m) {
    SkinMesh** pp = &mesh_hash[hash_ptr(m->vtxenv)];
    while (*pp) {
        if (*pp == m) {
            *pp = m->hnext;
            break;
        }
        pp = &(*pp)->hnext;
    }
    if (m->normenv) {
        pp = &nrm_hash[hash_ptr(m->normenv)];
        while (*pp) {
            if (*pp == m) {
                *pp = m->nhnext;
                break;
            }
            pp = &(*pp)->nhnext;
        }
    }
}

static void mesh_free(SkinMesh* m) {
    free(m->ent);
    free(m->pos_ent);
    free(m->nrm_ent);
    free(m->ent_slotf);
    free(m->P);
    free(m->N);
    free(m->fpent);
    free(m->fnent);
    free(m->PVbuf);
    free(m->NVbuf);
    memset(m, 0, sizeof(*m));
}

static unsigned reg_serial = 1; /* M47: bumped when the meshes' table changes */
static void sr_hsf(SkinHsf* h, int protect); /* M48: --skinreadwatch */
static void fuse_materialize_free(SkinHsf* h); /* M48 */
static void hsf_free(SkinHsf* h) {
    sr_hsf(h, 0);
    int i;
    reg_serial++;
    if (h->dec_valid) {
        /* M47: a pending decode job may hold this HSF's meshes' tables */
        rt_decode_join_pos(h->dec_pos, "skin free");
        h->dec_valid = 0;
    }
    for (i = 0; i < h->nmesh; i++) {
        mesh_unhash(&h->mesh[i]);
        mesh_free(&h->mesh[i]);
    }
    free(h->mesh);
    memset(h, 0, sizeof(*h));
}

/* ---- lifetime (M19, PLAN.md 34) ------------------------------------------
 *
 * The registry holds raw pointers into the game's heap: the HSF, its object
 * array, its bone matrices, each skinned mesh's buffers.  The game frees a
 * model (Hu3DModelKill -> HuMemDirectFree of the one file image all of that
 * lives in) without telling anyone, and in frame mode a model can be freed
 * while its entry is still dirty -- EnvelopeProc ran on a consumed frame, no
 * drawn frame followed before the kill -- so the next model whose position
 * array lands on the same address matched the stale entry in
 * gx_skin_array_bound and the read of `m->obj->mesh.vertex` went through
 * freed memory (the M18 soak's fault at 0x8200ad0a, 15 minutes in).  Lockstep
 * never showed it: every frame is drawn there, so an entry is clean by the
 * time its model can die.
 *
 * Two answers, both cheap.  `port_mem_freed`, planted in HuMemMemoryFree by
 * port/patches.txt, drops every entry that references the block going back
 * to the heap -- the precise lifetime, from the game's own free.  And every
 * read the draw side makes through a registry pointer is guarded first:
 * inside MEM1, and the HSF still holding the object array, matrix table and
 * object count it was registered with.  A guard that fires drops the entry
 * and skips the body (the model draws unskinned for a frame) and is counted
 * in the report, where the count must be zero. */
static int in_mem1(const void* p, size_t n) {
    const u8* lo = (const u8*)port_mem1_lo();
    const u8* hi = (const u8*)port_mem1_hi();
    return p != NULL && (const u8*)p >= lo && (const u8*)p + n <= hi;
}

static void guard_hit(SkinHsf* h, const char* what) {
    stat_guard_hits++;
    if (stat_guard_hits <= 8) {
        port_log("port> skin: GUARD: %s (hsf %p, frame %u, registered at frame %u); "
                 "entry dropped, body skipped\n",
                 what, (void*)h->hsf, gl13_frame_number(), h->last_frame);
    }
    hsf_free(h);
}

/* The HSF still describes what was registered.  Reads only fields of the
 * HSFDATA itself, which is inside MEM1 by the first test. */
static int hsf_live(SkinHsf* h, const char* where) {
    HSFDATA* hsf = h->hsf;
    if (port_opt.noskinlifetime) {
        return 1; /* M18: trust the pointer */
    }
    if (!in_mem1(hsf, sizeof(*hsf))) {
        guard_hit(h, where);
        return 0;
    }
    if (hsf->object != h->object || hsf->matrix != h->matrix ||
        hsf->objectNum != h->objectNum || !in_mem1(hsf->matrix, sizeof(HSFMATRIX)) ||
        !in_mem1(hsf->matrix->data, sizeof(Mtx))) {
        guard_hit(h, where);
        return 0;
    }
    return 1;
}

static int range_has(const u8* lo, const u8* hi, const void* p) {
    return p != NULL && (const u8*)p >= lo && (const u8*)p < hi;
}

/* HuMemMemoryFree: [data, data+size) is going back to the heap (the block's
 * body; the game keeps the file image of a model in one block, hsfload.c). */
void port_musyx_mix_mem_freed(const void* data, unsigned long size);
unsigned port_frame_frees; /* M24: HuMemMemoryFree calls this frame (perf.c's stall line) */
void port_curve_memo_freed(const void* data, unsigned long size);
void port_wb_freed(const void* ptr, size_t n);
void port_mem_freed(const void* data, unsigned long size) {
    const u8* lo = (const u8*)data;
    const u8* hi = lo + size;
    int i, j;
    port_frame_frees++;
    port_musyx_mix_mem_freed(data, size); /* M19 item 2: a voice still reading it? */
    port_curve_memo_freed(data, size);    /* M28 (b): a memoised track in it? */
    port_wb_freed(data, size);            /* M42: the write barrier forgets the block */
    for (i = 0; i < nhsfs; i++) {
        SkinHsf* h = &hsfs[i];
        int hit;
        if (!h->hsf) {
            continue;
        }
        hit = range_has(lo, hi, h->hsf) || range_has(lo, hi, h->object) ||
              range_has(lo, hi, h->matrix);
        for (j = 0; !hit && j < h->nmesh; j++) {
            const SkinMesh* m = &h->mesh[j];
            hit = range_has(lo, hi, m->obj) || range_has(lo, hi, m->vtxenv) ||
                  range_has(lo, hi, m->normenv) || range_has(lo, hi, m->cenv);
        }
        if (hit && port_opt.noskinlifetime) {
            /* the M18 behaviour, counted: the entry stays, pointing at memory
             * the game is about to reuse */
            stat_lever_stale_left++;
            if (h->mtx_dirty || h->skin_dirty) {
                stat_lever_stale_dirty++;
                if (stat_lever_stale_dirty <= 8) {
                    port_log("port> skin: --noskinlifetime: hsf %p freed at frame %u while DIRTY "
                             "(last EnvelopeProc frame %u); the entry stays\n",
                             (void*)h->hsf, gl13_frame_number(), h->last_frame);
                }
            }
            continue;
        }
        if (hit) {
            if (port_opt.skinstats) {
                port_log("port> skin: hsf %p freed by the game at frame %u (block %p+%lu)%s\n",
                         (void*)h->hsf, gl13_frame_number(), data, size,
                         (h->mtx_dirty || h->skin_dirty) ? " while dirty" : "");
            }
            if (h->owed == 1 && !port_opt.nofreemat) {
                /* M48 (PLAN.md 63.7): the arrays the skin decode owed are
                 * written before the game gets the memory back, from the last
                 * pose built -- what the game's body left in them.  The game
                 * reads freed memory as garbage in places (m427's river hook
                 * never sets its ripple phase, unk_24, and takes whatever the
                 * model heap held), and the garbage must be the old path's.
                 * The first free touching the entry drops it, so the rest of
                 * the model's memory is still the model's here. */
                fuse_materialize_free(h);
            }
            hsf_free(h);
            stat_freed_drops++;
        }
    }
}

static u32 fnv(const void* p, size_t n, u32 h) {
    const u8* b = (const u8*)p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

/* Everything the tables are a function of.  A model unloaded and another
 * loaded at the same address has to differ in one of these to be told apart;
 * the cenv tables themselves are hashed, so two different files at one base
 * with the same counts still differ. */
static u32 hsf_signature(const HSFDATA* hsf) {
    u32 h = 2166136261u;
    int i;
    h = fnv(&hsf->objectNum, sizeof(hsf->objectNum), h);
    h = fnv(&hsf->cenvNum, sizeof(hsf->cenvNum), h);
    h = fnv(&hsf->object, sizeof(hsf->object), h);
    h = fnv(&hsf->matrix, sizeof(hsf->matrix), h);
    for (i = 0; i < hsf->objectNum; i++) {
        const HSFOBJECT* o = &hsf->object[i];
        if (o->type != 2 || !o->mesh.cenvNum) {
            continue;
        }
        h = fnv(&o->mesh.vertex, sizeof(o->mesh.vertex), h);
        h = fnv(&o->mesh.normal, sizeof(o->mesh.normal), h);
        h = fnv(&o->mesh.cenv, sizeof(o->mesh.cenv), h);
        h = fnv(&o->mesh.cenvNum, sizeof(o->mesh.cenvNum), h);
        if (o->mesh.vertex) {
            h = fnv(&o->mesh.vertex->data, sizeof(void*), h);
            h = fnv(&o->mesh.vertex->count, sizeof(u32), h);
        }
        {
            int j;
            for (j = 0; j < (int)o->mesh.cenvNum; j++) {
                const HSFCENV* c = &o->mesh.cenv[j];
                h = fnv(&c->singleCount, 5 * sizeof(u32), h);
                h = fnv(c->singleData, c->singleCount * sizeof(HSFCENVSINGLE), h);
                if (c->dualCount) {
                    u32 k;
                    for (k = 0; k < c->dualCount; k++) {
                        h = fnv(&c->dualData[k].target1, 3 * sizeof(u32), h);
                        h = fnv(c->dualData[k].weight,
                                c->dualData[k].weightNum * sizeof(HSFCENVDUALWEIGHT), h);
                    }
                }
                if (c->multiCount) {
                    u32 k;
                    for (k = 0; k < c->multiCount; k++) {
                        h = fnv(&c->multiData[k].weightNum, 3 * sizeof(u32), h);
                        h = fnv(c->multiData[k].weight,
                                c->multiData[k].weightNum * sizeof(HSFCENVMULTIWEIGHT), h);
                    }
                }
            }
        }
    }
    return h;
}

/* ---- the tables: SetEnvelop's structure, read once ------------------------- */

static int ent_add(SkinMesh* m, int* cap, u8 kind, u32 t1, u32 t2, f32 w,
                   const HSFCENVMULTI* multi) {
    if (m->nent == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        SkinEnt* ne = (SkinEnt*)realloc(m->ent, (size_t)nc * sizeof(SkinEnt));
        if (!ne) {
            return -1;
        }
        m->ent = ne;
        *cap = nc;
    }
    m->ent[m->nent].kind = kind;
    m->ent[m->nent].t1 = t1;
    m->ent[m->nent].t2 = t2;
    m->ent[m->nent].w = w;
    m->ent[m->nent].multi = multi;
    return m->nent++;
}

static void range_set(u16* tab, int n, int from, int count, int e, unsigned* overlap) {
    int i;
    for (i = 0; i < count; i++) {
        int k = from + i;
        if (k < 0 || k >= n) {
            continue;
        }
        if (tab[k] != 0xFFFF) {
            (*overlap)++;
        }
        tab[k] = (u16)e;
    }
}

static void fuse_build(SkinMesh* m, HSFOBJECT* o);

/* Build one mesh's tables.  The order is SetEnvelop's -- single, dual, multi,
 * copy -- so a vertex two entries both name ends up with the one that wrote
 * it last on the CPU. */
static int mesh_build(SkinHsf* h, SkinMesh* m, HSFOBJECT* o, int objIdx, int meshNo) {
    int cap = 0;
    int j, i;
    unsigned k;
    m->owner = h;
    m->obj = o;
    m->objIdx = objIdx;
    m->meshNo = meshNo;
    m->vtxenv = o->mesh.vertex->data;
    m->normenv = o->mesh.normal ? o->mesh.normal->data : NULL;
    m->cenv = o->mesh.cenv;
    m->nvtx = (int)o->mesh.vertex->count;
    m->nnrm = o->mesh.normal ? (int)o->mesh.normal->count : 0;
    m->pos_ent = (u16*)malloc((size_t)(m->nvtx > 0 ? m->nvtx : 1) * sizeof(u16));
    m->nrm_ent = (u16*)malloc((size_t)(m->nnrm > 0 ? m->nnrm : 1) * sizeof(u16));
    if (!m->pos_ent || !m->nrm_ent) {
        return -1;
    }
    memset(m->pos_ent, 0xFF, (size_t)(m->nvtx > 0 ? m->nvtx : 1) * sizeof(u16));
    memset(m->nrm_ent, 0xFF, (size_t)(m->nnrm > 0 ? m->nnrm : 1) * sizeof(u16));
    /* entry 0 is the identity: the copy vertices, and any index no entry
     * names (the CPU left those buffers as loaded) */
    if (ent_add(m, &cap, SKIN_ENT_IDENTITY, 0, 0, 0.0f, NULL) < 0) {
        return -1;
    }
    for (j = 0; j < (int)o->mesh.cenvNum; j++) {
        const HSFCENV* c = &o->mesh.cenv[j];
        const HSFCENVSINGLE* s = c->singleData;
        const HSFCENVDUAL* d = c->dualData;
        const HSFCENVMULTI* mu = c->multiData;
        for (k = 0; k < c->singleCount; k++, s++) {
            int e = ent_add(m, &cap, SKIN_ENT_SINGLE, s->target, 0, 1.0f, NULL);
            if (e < 0) {
                return -1;
            }
            range_set(m->pos_ent, m->nvtx, s->pos, s->posNum, e, &m->overlap);
            /* M47: SetEnvelop multiplies ONE normal for a single of one
             * position (PSMTXMultVec), whatever its normalNum */
            range_set(m->nrm_ent, m->nnrm, s->normal, s->posNum == 1 ? 1 : s->normalNum, e,
                      &m->overlap);
            m->n_single++;
            m->v_single += s->posNum;
        }
        for (k = 0; k < c->dualCount; k++, d++) {
            const HSFCENVDUALWEIGHT* w = d->weight;
            u32 q;
            m->n_dual_pairs++;
            for (q = 0; q < d->weightNum; q++, w++) {
                int e = ent_add(m, &cap, SKIN_ENT_DUAL, d->target1, d->target2, w->weight,
                                NULL);
                if (e < 0) {
                    return -1;
                }
                range_set(m->pos_ent, m->nvtx, w->pos, w->posNum, e, &m->overlap);
                range_set(m->nrm_ent, m->nnrm, w->normal, w->normalNum, e, &m->overlap);
                m->n_dual++;
                m->v_dual += w->posNum;
            }
        }
        for (k = 0; k < c->multiCount; k++, mu++) {
            int e = ent_add(m, &cap, SKIN_ENT_MULTI, 0, 0, 0.0f, mu);
            if (e < 0) {
                return -1;
            }
            /* M47: a multi entry writes one vertex and one normal (SetEnvelop's
             * multi loop), whatever its posNum/normalNum */
            range_set(m->pos_ent, m->nvtx, mu->pos, 1, e, &m->overlap);
            range_set(m->nrm_ent, m->nnrm, mu->normal, 1, e, &m->overlap);
            m->n_multi++;
            m->v_multi += mu->posNum;
            if ((int)mu->weightNum > m->multi_max_w) {
                m->multi_max_w = (int)mu->weightNum;
            }
        }
        range_set(m->pos_ent, m->nvtx, (int)c->vtxCount, (int)c->copyCount, 0, &m->overlap);
        m->v_copy += c->copyCount;
    }
    /* indices no entry named: identity, and counted */
    for (i = 0; i < m->nvtx; i++) {
        if (m->pos_ent[i] == 0xFFFF) {
            m->pos_ent[i] = 0;
            m->v_unnamed++;
        }
    }
    for (i = 0; i < m->nnrm; i++) {
        if (m->nrm_ent[i] == 0xFFFF) {
            m->nrm_ent[i] = 0;
        }
    }
    /* distinct bones */
    {
        u8 seen[256];
        int e;
        memset(seen, 0, sizeof(seen));
        for (e = 1; e < m->nent; e++) {
            const SkinEnt* en = &m->ent[e];
            if (en->kind == SKIN_ENT_MULTI) {
                const HSFCENVMULTIWEIGHT* w = en->multi->weight;
                for (k = 0; k < en->multi->weightNum; k++, w++) {
                    if (w->target < 256 && !seen[w->target]) {
                        seen[w->target] = 1;
                        m->n_bones++;
                    }
                }
            } else {
                if (en->t1 < 256 && !seen[en->t1]) {
                    seen[en->t1] = 1;
                    m->n_bones++;
                }
                if (en->kind == SKIN_ENT_DUAL && en->t2 < 256 && !seen[en->t2]) {
                    seen[en->t2] = 1;
                    m->n_bones++;
                }
            }
        }
    }
    /* the batch window's entry -> slot map (gx_draw.c pal_place) */
    m->ent_slotf = (f32*)malloc((size_t)m->nent * sizeof(f32));
    m->P = (SkinMtx*)calloc((size_t)m->nent, sizeof(SkinMtx));
    m->N = (SkinMtx*)calloc((size_t)m->nent, sizeof(SkinMtx));
    if (!m->ent_slotf || !m->P || !m->N) {
        return -1;
    }
    for (i = 0; i < m->nent; i++) {
        m->ent_slotf[i] = -1.0f;
    }
    m->pose_serial = 0;
    m->fallback = 0;
    fuse_build(m, o);
    return 0;
}


/* ---- M47 (PLAN.md 62): the skin at the decode -------------------------------
 *
 * The deferred body (M18) runs SetEnvelopMain at the draw: every skinned
 * vertex read from the rest pose, multiplied, written into the mesh's arrays
 * -- and then the decode gathers the arrays it just wrote, index by index,
 * into the ring.  On m436 that is 8.1 M cycles of the drawn frame's object
 * walk, most of it the stores' and the gathers' cache misses (PLAN.md 61.9).
 *
 * Here the body builds only what SetEnvelop computes once per entry -- each
 * entry's position and normal matrix, with the game's own PSMTX calls in its
 * order (ent_pose, verbatim) -- and the decode, per list vertex, reads the
 * rest pose at the vertex's index and does the multiply SetEnvelop would
 * have done for that index: m[0]*x + m[1]*y + m[2]*z + m[3], which is the
 * expression of all three of SetEnvelop's multipliers (PSMTXMultVec,
 * PSMTXMultVecArray, PSMTXROMultVecArray on the reordered matrix) and
 * compiles to the same fmuls/fmadds/fmadds/fadds in each.  The arrays are not
 * written; nothing but the draw reads them (PLAN.md 33.3).  Which entry
 * wrote an index last is SetEnvelop's order, table by table below, with its
 * two quirks: a single entry of one position multiplies ONE normal whatever
 * its normalNum (PSMTXMultVec), and the copy writes positions only.
 *
 * A mesh the decode cannot do this for -- a multi entry (none on the walk,
 * PLAN.md 33.2), a range past its arrays, arrays that are the file's own, a
 * cluster or shape that wrote it this frame (writeNum: the in-place case) --
 * makes its HSF run the game's body as before.  And any reader that is not
 * the skin decode (a plan it cannot carry, the immediate mode, a rewriter
 * about to write the array) gets the arrays written first, from the same
 * matrices (fuse_materialize).  `--noskindecode` is the M46 path;
 * `--skinverify` runs both and compares every fused vertex, bit for bit. */
static unsigned long stat_fuse_poses, stat_fuse_old_bodies, stat_fuse_materialized,
    stat_fuse_mat_rewrite, stat_fuse_prims, stat_fuse_verts, stat_fuse_nrm_prims;
static unsigned long stat_verify_pos, stat_verify_nrm, stat_verify_pos_bad,
    stat_verify_nrm_bad, stat_verify_unnamed;
static unsigned stat_fuse_meshes_ok, stat_fuse_meshes_no[4];

static int fuse_range(u16* tab, int n, int from, int count, int e) {
    int i;
    if (count < 0 || from < 0 || from + count > n) {
        return -1; /* SetEnvelop would write past the array: not ours to model */
    }
    for (i = 0; i < count; i++) {
        tab[from + i] = (u16)e;
    }
    return 0;
}

/* SetEnvelop's writes, in its order: per cenv, the singles, the duals' weights,
 * (the multis), the copy.  The entries are numbered as mesh_build numbered
 * them (the same walk), so e indexes m->P / m->N. */
static void fuse_build(SkinMesh* m, HSFOBJECT* o) {
    int j, e = 1, why = 0;
    unsigned k;
    m->fuse_ok = 0;
    m->fpent = (u16*)malloc((size_t)(m->nvtx > 0 ? m->nvtx : 1) * sizeof(u16));
    m->fnent = (u16*)malloc((size_t)(m->nnrm > 0 ? m->nnrm : 1) * sizeof(u16));
    if (!m->fpent || !m->fnent) {
        return;
    }
    memset(m->fpent, 0xFF, (size_t)(m->nvtx > 0 ? m->nvtx : 1) * sizeof(u16));
    memset(m->fnent, 0xFF, (size_t)(m->nnrm > 0 ? m->nnrm : 1) * sizeof(u16));
    if (!o->mesh.normal || !m->normenv || m->vtxenv == o->mesh.file[0] ||
        m->normenv == o->mesh.file[1] || !o->mesh.file[0] || !o->mesh.file[1]) {
        why = 1;
    }
    for (j = 0; !why && j < (int)o->mesh.cenvNum; j++) {
        const HSFCENV* c = &o->mesh.cenv[j];
        const HSFCENVSINGLE* sg = c->singleData;
        const HSFCENVDUAL* d = c->dualData;
        for (k = 0; !why && k < c->singleCount; k++, sg++, e++) {
            if (m->ent[e].kind != SKIN_ENT_SINGLE ||
                fuse_range(m->fpent, m->nvtx, (int)sg->pos, (int)sg->posNum, e) < 0 ||
                fuse_range(m->fnent, m->nnrm, (int)sg->normal,
                           sg->posNum == 1 ? 1 : (int)sg->normalNum, e) < 0) {
                why = 2;
            }
        }
        for (k = 0; !why && k < c->dualCount; k++, d++) {
            const HSFCENVDUALWEIGHT* w = d->weight;
            u32 q;
            for (q = 0; !why && q < d->weightNum; q++, w++, e++) {
                if (m->ent[e].kind != SKIN_ENT_DUAL ||
                    fuse_range(m->fpent, m->nvtx, (int)w->pos, (int)w->posNum, e) < 0 ||
                    fuse_range(m->fnent, m->nnrm, (int)w->normal, (int)w->normalNum, e) < 0) {
                    why = 2;
                }
            }
        }
        if (!why && c->multiCount) {
            why = 3;
        }
        if (!why && fuse_range(m->fpent, m->nvtx, (int)c->vtxCount, (int)c->copyCount, 0) < 0) {
            why = 2;
        }
    }
    if (!why && e != m->nent) {
        why = 2;
    }
    if (why) {
        stat_fuse_meshes_no[why]++;
        return;
    }
    m->fuse_ok = 1;
    stat_fuse_meshes_ok++;
    m->dec.live_pos = (const u8*)m->vtxenv;
    m->dec.live_nrm = (const u8*)m->normenv;
    m->dec.pent = m->fpent;
    m->dec.nent = m->fnent;
    m->dec.P = &m->P[0].m[0][0];
    m->dec.N = &m->N[0].m[0][0];
    m->dec.npos = (u32)m->nvtx;
    m->dec.nnrm = (u32)m->nnrm;
    m->dec.rest_pos = (const f32*)o->mesh.file[0];
    m->dec.rest_nrm = (const f32*)o->mesh.file[1];
    if (port_opt.skinvec) {
        /* malloc is 16-byte aligned on Mac OS X (gx_skinvec.c's lvx) */
        m->PVbuf = (f32*)calloc((size_t)m->nent * 16, sizeof(f32));
        m->NVbuf = (f32*)calloc((size_t)m->nent * 16, sizeof(f32));
        if (m->PVbuf && m->NVbuf && !((uintptr_t)m->PVbuf & 15) && !((uintptr_t)m->NVbuf & 15)) {
            m->dec.PV = m->PVbuf;
            m->dec.NV = m->NVbuf;
        }
    }
}

static unsigned long stat_fuse_shared;
static void fuse_materialize(SkinHsf* h);
static void fuse_share_off(SkinHsf* h, SkinHsf* other) {
    if (other->owed == 1) {
        fuse_materialize(other);
    }
    if (h->fuse_ok || other->fuse_ok) {
        stat_fuse_shared++;
        if (port_opt.skinstats) {
            port_log("port> skin: hsf %p and hsf %p share a buffer: both run the game's body\n",
                     (void*)h->hsf, (void*)other->hsf);
        }
    }
    h->fuse_ok = 0;
    other->fuse_ok = 0;
}

static SkinHsf* hsf_find(const HSFDATA* hsf) {
    int i;
    for (i = 0; i < nhsfs; i++) {
        if (hsfs[i].hsf == hsf) {
            return &hsfs[i];
        }
    }
    return NULL;
}

/* M47 (PLAN.md 62): the pointers and counts hsf_signature starts from, and
 * each skinned object's buffers -- everything but the envelope tables'
 * contents, which are the model file's and change only if the model is
 * freed and another loaded (port_mem_freed drops the entry then, M19). */
static u32 hsf_cheap_signature(const HSFDATA* hsf) {
    u32 h = 2166136261u;
    int i;
    h = fnv(&hsf->objectNum, sizeof(hsf->objectNum), h);
    h = fnv(&hsf->cenvNum, sizeof(hsf->cenvNum), h);
    h = fnv(&hsf->object, sizeof(hsf->object), h);
    h = fnv(&hsf->matrix, sizeof(hsf->matrix), h);
    for (i = 0; i < hsf->objectNum; i++) {
        const HSFOBJECT* o = &hsf->object[i];
        if (o->type != 2 || !o->mesh.cenvNum) {
            continue;
        }
        h = fnv(&o->mesh.vertex, sizeof(o->mesh.vertex), h);
        h = fnv(&o->mesh.normal, sizeof(o->mesh.normal), h);
        h = fnv(&o->mesh.cenv, sizeof(o->mesh.cenv), h);
        h = fnv(&o->mesh.cenvNum, sizeof(o->mesh.cenvNum), h);
        if (o->mesh.vertex) {
            h = fnv(&o->mesh.vertex->data, sizeof(void*), h);
            h = fnv(&o->mesh.vertex->count, sizeof(u32), h);
        }
    }
    return h;
}
static unsigned long stat_sig_full, stat_sig_cheap;

static SkinHsf* hsf_register(HSFDATA* hsf, unsigned frame) {
    SkinHsf* h = NULL;
    int i, n = 0, meshNo = 0;
    u32 sig, csig;
    h = hsf_find(hsf);
    if (h && !port_opt.fullskinsig && !port_opt.noskinlifetime) {
        /* M47: EnvelopeProc runs every frame for every skinned model, and
         * the full signature hashed its envelope tables each time (1% of
         * m436's game thread).  The pointers every call; the tables at the
         * first sight, when a pointer moved, and every 256th call */
        csig = hsf_cheap_signature(hsf);
        if (csig == h->cheap_sig && ++h->sig_calls < 256) {
            stat_sig_cheap++;
            return h;
        }
    }
    csig = hsf_cheap_signature(hsf);
    sig = hsf_signature(hsf);
    stat_sig_full++;
    if (h) {
        if (h->signature == sig) {
            h->cheap_sig = csig;
            h->sig_calls = 0;
            return h;
        }
        hsf_free(h);
        stat_rebuilt++;
    } else {
        /* a slot emptied by a free (M19) before a new one */
        for (i = 0; i < nhsfs; i++) {
            if (!hsfs[i].hsf) {
                h = &hsfs[i];
                break;
            }
        }
    }
    if (h) {
        /* an emptied slot, taken */
    } else if (nhsfs < SKIN_HSF_MAX) {
        h = &hsfs[nhsfs++];
    } else {
        /* the least recently seen slot: a model that has not skinned in a
         * while is a model that was unloaded */
        int oldest = 0;
        for (i = 1; i < nhsfs; i++) {
            if (hsfs[i].last_frame < hsfs[oldest].last_frame) {
                oldest = i;
            }
        }
        h = &hsfs[oldest];
        hsf_free(h);
        stat_rebuilt++;
    }
    memset(h, 0, sizeof(*h));
    reg_serial++;
    h->hsf = hsf;
    h->signature = sig;
    h->cheap_sig = csig;
    h->object = hsf->object;
    h->matrix = hsf->matrix;
    h->objectNum = hsf->objectNum;
    h->serial = 1;
    h->mtx_dirty = 1;
    for (i = 0; i < hsf->objectNum; i++) {
        if (hsf->object[i].type == 2 && hsf->object[i].mesh.cenvNum &&
            hsf->object[i].mesh.vertex) {
            n++;
        }
    }
    h->mesh = (SkinMesh*)calloc((size_t)(n > 0 ? n : 1), sizeof(SkinMesh));
    if (!h->mesh) {
        h->cpu = 1;
        return h;
    }
    /* SetEnvelopMain numbers the meshes by walking every type-2 object, with
     * or without a cenv -- `Meshno` indexes the bind-matrix table */
    for (i = 0; i < hsf->objectNum; i++) {
        HSFOBJECT* o = &hsf->object[i];
        if (o->type != 2) {
            continue;
        }
        if (o->mesh.cenvNum && o->mesh.vertex) {
            SkinMesh* m = &h->mesh[h->nmesh];
            if (mesh_build(h, m, o, i, meshNo) < 0) {
                mesh_free(m);
                h->cpu = 1;
                break;
            }
            h->nmesh++;
            m->hnext = mesh_hash[hash_ptr(m->vtxenv)];
            mesh_hash[hash_ptr(m->vtxenv)] = m;
            if (m->normenv) {
                m->nhnext = nrm_hash[hash_ptr(m->normenv)];
                nrm_hash[hash_ptr(m->normenv)] = m;
            }
        }
        meshNo++;
    }
    h->last_frame = frame;
    stat_registered++;
    gx_skin_fuse_live_flag = fuse_mode();
    h->fuse_ok = !h->cpu && h->nmesh > 0;
    for (i = 0; i < h->nmesh; i++) {
        h->fuse_ok &= h->mesh[i].fuse_ok;
    }
    /* M47 (PLAN.md 62.10): two registered HSFs whose meshes share a buffer
     * (models built from one file: each body rewrites the same arrays in
     * turn, the draw reading whichever pose went last) are not skinned at the
     * decode -- a buffer names one mesh's matrices, not two.  Both run the
     * game's body from here on, the other's owed arrays written first. */
    for (i = 0; i < h->nmesh; i++) {
        SkinMesh* m = &h->mesh[i];
        SkinMesh* o;
        for (o = mesh_hash[hash_ptr(m->vtxenv)]; o; o = o->hnext) {
            if (o->owner != h && o->vtxenv == m->vtxenv) {
                fuse_share_off(h, o->owner);
            }
        }
        if (m->normenv) {
            for (o = nrm_hash[hash_ptr(m->normenv)]; o; o = o->nhnext) {
                if (o->owner != h && o->normenv == m->normenv) {
                    fuse_share_off(h, o->owner);
                }
            }
        }
    }
    if (h->cpu) {
        stat_fallback_hsf++;
    }
    if (port_opt.skinstats) {
        port_log("port> skin: hsf %p registered: %d objects, %d skinned meshes%s\n",
                 (void*)hsf, (int)hsf->objectNum, h->nmesh,
                 h->cpu ? " -- CPU (a mesh exceeds the palette)" : "");
        for (i = 0; i < h->nmesh; i++) {
            const SkinMesh* m = &h->mesh[i];
            port_log("port> skin:   mesh \"%s\" obj %d meshNo %d: %d vtx %d nrm, entries %d "
                     "(single %d [%lu vtx], dual %d in %d pairs [%lu vtx], multi %d "
                     "[%lu vtx, max %d bones], copy %lu, unnamed %lu), %d bones, "
                     "overlaps %u%s\n",
                     m->obj->name ? m->obj->name : "?", m->objIdx, m->meshNo, m->nvtx,
                     m->nnrm, m->nent, m->n_single, m->v_single, m->n_dual,
                     m->n_dual_pairs, m->v_dual, m->n_multi, m->v_multi, m->multi_max_w,
                     m->v_copy, m->v_unnamed, m->n_bones, m->overlap,
                     m->fallback ? " FALLBACK" : "");
        }
    }
    return h;
}

/* ---- the hooks ------------------------------------------------------------- */

/* 0: the game skins at EnvelopeProc (--cpuskin); 1: deferred CPU skinning
 * (the default); 2: the vertex-program palette (--palette, and the palette
 * must actually be there -- a deferred EnvelopeProc with no palette to skin
 * at the draw would draw the rest pose). */
int gx_skin_mode(void) {
    if (port_opt.cpuskin) {
        return 0;
    }
    if (port_opt.palette && gx_palette_on()) {
        return 2;
    }
    return 1;
}
int gx_skin_on(void) { return gx_skin_mode() == 2; }

static void hsf_mtx_sync(SkinHsf* h);

/* EnvelopeProc's head.  1 = handled here (the game's body must not run). */
int port_envelope_proc(HSFDATA* hsf) {
    SkinHsf* h;
    unsigned frame = gl13_frame_number();
    stat_proc_calls++;
    if (gx_skin_mode() == 0 || !hsf || !hsf->matrix) {
        stat_proc_cpu++;
        /* M29 (PLAN.md 44.1 rule 3): the game's own body rewrites the
         * buffers now */
        rt_decode_join("cpuskin EnvelopeProc");
        gx_vc_epoch++; /* M40: the game's body rewrites the arrays after this */
        return 0;
    }
    h = hsf_register(hsf, frame);
    if (!h || h->cpu) {
        stat_proc_cpu++;
        rt_decode_join("cpuskin EnvelopeProc");
        gx_vc_epoch++; /* M40: the game's body rewrites the arrays after this */
        return 0;
    }
    h->serial++;
    h->mtx_dirty = 1;
    h->last_frame = frame;
    stat_proc_deferred++;
    if (gx_skin_mode() == 1 && !port_opt.skindeferall) {
        /* The bone walk runs now, on every frame, as the game does it: the
         * draw walk's matrix stack (hsfdraw.c MTXBuf) is built from these
         * matrices, and the game's own Hu3DModelObjMtxGet family walks the
         * same stack, so a stale bone matrix on a consumed frame reaches game
         * logic through positions (found by the .wav of the walk: a 3D sound
         * placed a rounding step away at retrace 8,108).  Only the vertex
         * skinning -- read by nothing but GXSetArray -- is deferred. */
        hsf_mtx_sync(h);
    }
    return 1;
}

/* The bone matrices, exactly as EnvelopeProc would have left them, if the
 * pose moved since they were last built.  `MtxTop`/`nObj`/`nMesh` are the
 * game's own statics for SetEnvelopMtx, set the way EnvelopeProc sets them. */
static void hsf_mtx_sync(SkinHsf* h) {
    Mtx id;
    HSFMATRIX* mx;
    if (!h->mtx_dirty) {
        return;
    }
    mx = h->hsf->matrix;
    MtxTop = mx->data;
    nObj = mx->count;
    nMesh = mx->base_idx;
    PSMTXIdentity(id);
    SetEnvelopMtx(h->hsf->object, h->hsf->root, id);
    h->mtx_dirty = 0;
    h->skin_dirty = 1;
    stat_sync_runs++;
}

/* SetEnvelopMain writes each skinned mesh's position and normal buffers:
 * the vertex cache is told those, by their extent (M47) */
static void hsf_arrays_written(SkinHsf* h) {
    int k;
    for (k = 0; k < h->nmesh; k++) {
        SkinMesh* m = &h->mesh[k];
        gx_vc_arrays_written(m->vtxenv, (size_t)m->nvtx * 12, &m->vmark_pos);
        if (m->normenv) {
            gx_vc_arrays_written(m->normenv, (size_t)m->nnrm * 12, &m->vmark_nrm);
        }
    }
}

/* The game's own EnvelopeProc body, on the inputs it has now: what the
 * deferred call would have computed, computed at the first read instead. */
static void hsf_run_body(SkinHsf* h) {
    HSFMATRIX* mx = h->hsf->matrix;
    Mtx id;
    /* M29 (PLAN.md 44.1 rule 2): SetEnvelopMain rewrites this HSF's vertex
     * and normal buffers in place; a decode record that reads them may still
     * be pending on the render thread (a hooked child drawn in the shadow
     * pass and again after its own mark) */
    if (h->dec_valid) {
        rt_decode_join_pos(h->dec_pos, "skin body");
        h->dec_valid = 0;
    }
    MtxTop = mx->data;
    nObj = mx->count;
    nMesh = mx->base_idx;
    if (h->mtx_dirty) {
        PSMTXIdentity(id);
        SetEnvelopMtx(h->hsf->object, h->hsf->root, id);
        h->mtx_dirty = 0;
    }
    SetEnvelopMain(h->hsf);
    /* M40: the arrays were rewritten; the vertex cache re-hashes them -- M47:
     * those two arrays of each mesh, not every array (gx_vc_arrays_written) */
    hsf_arrays_written(h);
    h->skin_dirty = 0;
    stat_body_runs++;
}


/* ---- M47: the body, the pose, the arrays on demand (see fuse_build) ------- */
static int fuse_mode(void) {
    return !port_opt.noskindecode && !port_opt.dlcache && gx_skin_mode() == 1;
}

static void ent_pose(const SkinMesh* m, const SkinEnt* e, Mtx* top, u32 nobj, u32 nmesh,
                     const Mtx inv, Mtx P, Mtx N);

/* the arrays, written from the matrices of the pose built (what SetEnvelop
 * would have written at that pose: the same multiply, the same entry per
 * index); the unnamed indices are left alone as SetEnvelop leaves them */

/* ---- M48 (PLAN.md 63.7): --skinreadwatch, m427's reader hunted ------------
 *
 * Under the skin decode a skinned mesh's arrays are owed -- not written --
 * from the pose build to their materialization.  Nothing but the decode
 * should read them then (M47's premise, which m427 breaks).  With this
 * diagnostic the pages lying wholly inside each owed array are made
 * unreadable at the pose build and readable again at the materialization
 * (or the game's body, or a free); the first access to one faults, and the
 * handler (crash.c, before the write barrier's) records the program
 * counter, the caller, whether it was a store and the frame, and opens the
 * page.  The write barrier forgets the pages first (a diagnostic run is not
 * timed, and the vertex cache re-hashes what it cannot trust).  The report
 * lists the readers by place, symbolized at exit. */
#include <signal.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <sys/ucontext.h>
#endif
#define SR_PAGE 4096u
#define SR_MAX 256
static struct { unsigned long pc, lr; const void* addr; unsigned frame; int store; unsigned n; } sr_hits[SR_MAX];
static volatile int sr_nhits;
static volatile unsigned long sr_faults, sr_protects, sr_opens;
static volatile u8* sr_page; /* per MEM1 page: 1 = protected by the watch */
static volatile u8* sr_ever; /* per MEM1 page: the watch has protected it (a late fault retries) */
static uintptr_t sr_lo, sr_hi;
void port_wb_disarm(const void* ptr, size_t n);
int port_wb_fault(const void* addr);
static int sr_init(void) {
    if (sr_page) {
        return 1;
    }
    sr_lo = ((uintptr_t)port_mem1_lo() + SR_PAGE - 1) & ~(uintptr_t)(SR_PAGE - 1);
    sr_hi = (uintptr_t)port_mem1_hi() & ~(uintptr_t)(SR_PAGE - 1);
    if (sr_hi <= sr_lo) {
        return 0;
    }
    sr_ever = (volatile u8*)calloc((sr_hi - sr_lo) / SR_PAGE, 1);
    sr_page = sr_ever ? (volatile u8*)calloc((sr_hi - sr_lo) / SR_PAGE, 1) : NULL;
    return sr_page != NULL;
}
/* the owed arrays' byte ranges: a fault on a page they share with other
 * data counts only when it lands inside one of them (the others open the
 * page and are counted as neighbours) */
#define SR_RANGES 512
static struct { uintptr_t lo, hi; } sr_rng[SR_RANGES];
static volatile unsigned long sr_neighbours;
static void sr_note(const void* p, size_t n, int on) {
    int i, fr = -1;
    for (i = 0; i < SR_RANGES; i++) {
        if (sr_rng[i].lo == (uintptr_t)p && sr_rng[i].hi) {
            if (!on) {
                sr_rng[i].hi = 0;
            }
            return;
        }
        if (!sr_rng[i].hi && fr < 0) {
            fr = i;
        }
    }
    if (on && fr >= 0) {
        sr_rng[fr].lo = (uintptr_t)p;
        sr_rng[fr].hi = (uintptr_t)p + n;
    }
}
static int sr_inside(uintptr_t a) {
    int i;
    for (i = 0; i < SR_RANGES; i++) {
        if (sr_rng[i].hi && a >= sr_rng[i].lo && a < sr_rng[i].hi) {
            return 1;
        }
    }
    return 0;
}
static void sr_range(const void* p, size_t n, int protect) {
    /* every page the array touches, its partial end pages too */
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(SR_PAGE - 1);
    uintptr_t b = ((uintptr_t)p + n + SR_PAGE - 1) & ~(uintptr_t)(SR_PAGE - 1);
    if (p && n) {
        sr_note(p, n, protect);
    }
    if (!p || !sr_init() || a < sr_lo || b > sr_hi || b <= a) {
        return;
    }
    for (; a < b; a += SR_PAGE) {
        unsigned long pg = (a - sr_lo) / SR_PAGE;
        if (protect && !sr_page[pg]) {
            port_wb_disarm((const void*)a, SR_PAGE);
            sr_ever[pg] = 1;
            if (mprotect((void*)a, SR_PAGE, PROT_NONE) == 0) {
                sr_page[pg] = 1;
                sr_protects++;
            }
        } else if (!protect && sr_page[pg]) {
            mprotect((void*)a, SR_PAGE, PROT_READ | PROT_WRITE);
            sr_page[pg] = 0;
            sr_opens++;
        }
    }
}
/* a kernel writer's destination (gx_wb.c port_wb_disarm): every watched
 * page in it opened first (a read(2) into a PROT_NONE page fails) */
void port_skinwatch_open(const void* ptr, size_t n) {
    uintptr_t a, b;
    if (!sr_page || !ptr || !n) {
        return;
    }
    a = (uintptr_t)ptr & ~(uintptr_t)(SR_PAGE - 1);
    b = ((uintptr_t)ptr + n + SR_PAGE - 1) & ~(uintptr_t)(SR_PAGE - 1);
    if (a < sr_lo) {
        a = sr_lo;
    }
    if (b > sr_hi) {
        b = sr_hi;
    }
    for (; a < b; a += SR_PAGE) {
        unsigned long pg = (a - sr_lo) / SR_PAGE;
        if (sr_page[pg]) {
            mprotect((void*)a, SR_PAGE, PROT_READ | PROT_WRITE);
            sr_page[pg] = 0;
            sr_opens++;
        }
    }
}
static void sr_hsf(SkinHsf* h, int protect) {
    int k;
    if (!port_opt.skinreadwatch) {
        return;
    }
    for (k = 0; k < h->nmesh; k++) {
        const SkinMesh* m = &h->mesh[k];
        sr_range(m->vtxenv, (size_t)m->nvtx * 12u, protect);
        sr_range(m->normenv, (size_t)m->nnrm * 12u, protect);
    }
}
int port_skinwatch_fault(const void* addr, void* uap) {
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(SR_PAGE - 1);
    unsigned long pg, pc = 0, lr = 0;
    int store = 0, i, n;
    if (!sr_page || a < sr_lo || a >= sr_hi) {
        return 0;
    }
    pg = (a - sr_lo) / SR_PAGE;
    if (!sr_page[pg]) {
        /* another thread opened it between this access and this handler
         * (the access retries), or the write barrier armed it since (its
         * handler's); a page the watch never touched is not ours */
        if (!sr_ever[pg]) {
            return 0;
        }
        if (port_wb_fault(addr)) {
            return 1;
        }
        mprotect((void*)a, SR_PAGE, PROT_READ | PROT_WRITE);
        return 1;
    }
    mprotect((void*)a, SR_PAGE, PROT_READ | PROT_WRITE);
    sr_page[pg] = 0;
    if (!sr_inside((uintptr_t)addr)) {
        sr_neighbours++;
        return 1; /* a neighbour of an owed array on a shared page */
    }
    sr_faults++;
#if defined(__APPLE__) && defined(__ppc__)
    {
        ucontext_t* uc = (ucontext_t*)uap;
        if (uc && uc->uc_mcontext) {
            pc = uc->uc_mcontext->ss.srr0;
            lr = uc->uc_mcontext->ss.lr;
            store = (uc->uc_mcontext->es.dsisr & 0x02000000u) != 0;
        }
    }
#else
    (void)uap;
#endif
    n = sr_nhits;
    for (i = 0; i < n; i++) {
        if (sr_hits[i].pc == pc && sr_hits[i].lr == lr) {
            sr_hits[i].n++;
            return 1;
        }
    }
    if (n < SR_MAX) {
        sr_hits[n].pc = pc;
        sr_hits[n].lr = lr;
        sr_hits[n].addr = addr;
        sr_hits[n].frame = gl13_frame_number();
        sr_hits[n].store = store;
        sr_hits[n].n = 1;
        sr_nhits = n + 1;
    }
    return 1;
}
static void sr_report(void) {
    int i;
    if (!port_opt.skinreadwatch) {
        return;
    }
    port_log("port> skinreadwatch (M48): %lu pages protected, %lu opened, %lu faults inside the "
             "owed arrays at %d places, %lu faults of their neighbours on shared pages\n",
             sr_protects, sr_opens, sr_faults, sr_nhits, sr_neighbours);
    for (i = 0; i < sr_nhits; i++) {
        const char *s1 = "?", *s2 = "?";
        unsigned long o1 = 0, o2 = 0;
#ifdef __APPLE__
        Dl_info di;
        if (dladdr((void*)sr_hits[i].pc, &di) && di.dli_sname) {
            s1 = di.dli_sname;
            o1 = sr_hits[i].pc - (unsigned long)di.dli_saddr;
        }
        if (dladdr((void*)sr_hits[i].lr, &di) && di.dli_sname) {
            s2 = di.dli_sname;
            o2 = sr_hits[i].lr - (unsigned long)di.dli_saddr;
        }
#endif
        port_log("port> skinreadwatch:   %s at %s+0x%lx (pc %#lx), called from %s+0x%lx: %u times, "
                 "first at frame %u, address %p\n",
                 sr_hits[i].store ? "store" : "load ", s1, o1, sr_hits[i].pc, s2, o2, sr_hits[i].n,
                 sr_hits[i].frame, sr_hits[i].addr);
    }
}

static void fuse_materialize(SkinHsf* h) {
    int k;
    if (h->owed != 1) {
        return;
    }
    sr_hsf(h, 0); /* M48: --skinreadwatch */
    if (h->dec_valid) {
        rt_decode_join_pos(h->dec_pos, "skin arrays");
        h->dec_valid = 0;
    }
    for (k = 0; k < h->nmesh; k++) {
        const SkinMesh* m = &h->mesh[k];
        const GxSkinDec* d = &m->dec;
        f32* vp = (f32*)m->vtxenv;
        f32* np = (f32*)m->normenv;
        u32 ix;
        for (ix = 0; ix < d->npos; ix++) {
            u32 e = d->pent[ix];
            if (e == GX_SKIN_UNNAMED) {
                continue;
            }
            if (e == 0) {
                vp[3 * ix + 0] = d->rest_pos[3 * ix + 0];
                vp[3 * ix + 1] = d->rest_pos[3 * ix + 1];
                vp[3 * ix + 2] = d->rest_pos[3 * ix + 2];
            } else {
                gx_skin_mul(d->P + 12 * e, d->rest_pos + 3 * ix, vp + 3 * ix);
            }
        }
        for (ix = 0; ix < d->nnrm; ix++) {
            u32 e = d->nent[ix];
            if (e != GX_SKIN_UNNAMED) {
                gx_skin_mul(d->N + 12 * e, d->rest_nrm + 3 * ix, np + 3 * ix);
            }
        }
    }
    hsf_arrays_written(h); /* the arrays changed: the vertex cache re-checks them */
    h->owed = 0;
    stat_fuse_materialized++;
}
static unsigned long stat_fuse_mat_freed;
static void fuse_materialize_free(SkinHsf* h) {
    stat_fuse_mat_freed++;
    fuse_materialize(h);
}

/* The deferred body (hsf_run_body's place in the two hooks).  Under the skin
 * decode: SetEnvelopMain's per-mesh work up to the vertices -- the inverse it
 * stores in MtxTop[Meshno] for every mesh object, in its order -- and each
 * skinned mesh's entry matrices; the arrays are owed.  --skinverify: the
 * game's body as well (the arrays the truth the decode is checked against). */
static void hsf_body(SkinHsf* h) {
    HSFDATA* hsf = h->hsf;
    HSFMATRIX* mx = hsf->matrix;
    int i, k, meshNo, inplace = 0;
    sr_hsf(h, 0); /* M48: --skinreadwatch (the body writes the arrays) */
    if (!fuse_mode() || !h->fuse_ok) {
        hsf_run_body(h);
        h->owed = 0;
        stat_fuse_old_bodies += fuse_mode();
        return;
    }
    for (k = 0; k < h->nmesh; k++) {
        const HSFOBJECT* o = h->mesh[k].obj;
        if (o->mesh.writeNum != 0 || o->mesh.file[0] != (void*)h->mesh[k].dec.rest_pos ||
            o->mesh.file[1] != (void*)h->mesh[k].dec.rest_nrm) {
            inplace = 1; /* a cluster or shape wrote it this frame: SetEnvelop
                          * reads its own array (or the file moved) */
        }
    }
    if (inplace) {
        hsf_run_body(h);
        h->owed = 0;
        stat_fuse_old_bodies++;
        return;
    }
    if (port_opt.skinverify) {
        hsf_run_body(h);
    } else {
        if (h->dec_valid) {
            rt_decode_join_pos(h->dec_pos, "skin pose");
            h->dec_valid = 0;
        }
        MtxTop = mx->data;
        nObj = mx->count;
        nMesh = mx->base_idx;
        if (h->mtx_dirty) {
            Mtx id;
            PSMTXIdentity(id);
            SetEnvelopMtx(hsf->object, hsf->root, id);
            h->mtx_dirty = 0;
        }
    }
    for (i = 0, k = 0, meshNo = 0; i < (int)hsf->objectNum; i++) {
        if (hsf->object[i].type != 2) {
            continue;
        }
        if (!port_opt.skinverify) {
            /* SetEnvelopMain: PSMTXInverse(MtxTop[&obj[nMesh] - object], MtxTop[Meshno]) */
            PSMTXInverse(mx->data[mx->base_idx + i], mx->data[meshNo]);
        }
        if (k < h->nmesh && h->mesh[k].objIdx == i) {
            SkinMesh* m = &h->mesh[k];
            int e;
            for (e = 1; e < m->nent; e++) {
                ent_pose(m, &m->ent[e], mx->data, mx->count, mx->base_idx, mx->data[meshNo],
                         m->P[e].m, m->N[e].m);
                if (m->dec.PV) {
                    /* --skinvec: the columns, lane r = row r (plain copies) */
                    f32* pv = m->PVbuf + 16 * e;
                    f32* nv = m->NVbuf + 16 * e;
                    int r, c;
                    for (c = 0; c < 4; c++) {
                        for (r = 0; r < 3; r++) {
                            pv[4 * c + r] = m->P[e].m[r][c];
                            nv[4 * c + r] = m->N[e].m[r][c];
                        }
                    }
                }
            }
            m->pose_serial = 0; /* the palette's pose cache (--palette) is not this */
            k++;
        }
        meshNo++;
    }
    h->owed = port_opt.skinverify ? 2 : 1;
    h->skin_dirty = 0;
    stat_fuse_poses++;
    if (h->owed == 1) {
        sr_hsf(h, 1); /* M48: --skinreadwatch */
    }
}

static SkinMesh* mesh_by_pos(const void* p) {
    SkinMesh* m;
    for (m = mesh_hash[hash_ptr(p)]; m; m = m->hnext) {
        if (m->vtxenv == p) {
            return m;
        }
    }
    return NULL;
}
static SkinMesh* mesh_by_nrm(const void* p) {
    SkinMesh* m;
    for (m = nrm_hash[hash_ptr(p)]; m; m = m->nhnext) {
        if (m->normenv == p) {
            return m;
        }
    }
    return NULL;
}

static SkinHsf* fuse_last;
/* M47: every registered mesh's two buffers in one small open-addressed
 * table (8 KB, in the L1 once warm): the per-primitive question "is this
 * array a skinned mesh's?" is a probe or two, not a walk through the
 * meshes' structures.  Rebuilt when the registry changes (reg_serial). */
#define FTAB 1024
static const void* ftab_key[FTAB];
static SkinMesh* ftab_val[FTAB]; /* the key's mesh */
static u8 ftab_kind[FTAB];       /* bit 0: a normal buffer; bit 1: shared by two */
static unsigned ftab_serial;
static int ftab_ok;
static unsigned ftab_hash(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return (unsigned)((v >> 4) ^ (v >> 14)) & (FTAB - 1);
}
static void ftab_put(const void* key, SkinMesh* m, u8 kind) {
    unsigned h = ftab_hash(key), n;
    for (n = 0; n < FTAB; n++, h = (h + 1) & (FTAB - 1)) {
        if (!ftab_key[h]) {
            ftab_key[h] = key;
            ftab_val[h] = m;
            ftab_kind[h] = kind;
            return;
        }
        if (ftab_key[h] == key && (ftab_kind[h] & 1) == kind) {
            ftab_kind[h] |= 2; /* two meshes, one buffer: the chains decide */
            return;
        }
    }
    ftab_ok = 0;
}
static void ftab_build(void) {
    int i, k, n = 0;
    memset(ftab_key, 0, sizeof(ftab_key));
    ftab_ok = 1;
    for (i = 0; i < nhsfs; i++) {
        for (k = 0; hsfs[i].hsf && k < hsfs[i].nmesh; k++) {
            n += 2;
        }
    }
    if (n > FTAB / 2) {
        ftab_ok = 0; /* too full to probe fast: the chains */
    } else {
        for (i = 0; i < nhsfs; i++) {
            for (k = 0; hsfs[i].hsf && k < hsfs[i].nmesh; k++) {
                SkinMesh* m = &hsfs[i].mesh[k];
                ftab_put(m->vtxenv, m, 0);
                if (m->normenv) {
                    ftab_put(m->normenv, m, 1);
                }
            }
        }
    }
    ftab_serial = reg_serial;
}
static SkinMesh* ftab_find(const void* p, u8 kind) {
    unsigned h, n;
    if (!ftab_ok) {
        return kind ? mesh_by_nrm(p) : mesh_by_pos(p);
    }
    for (h = ftab_hash(p), n = 0; n < FTAB; n++, h = (h + 1) & (FTAB - 1)) {
        if (!ftab_key[h]) {
            return NULL;
        }
        if (ftab_key[h] == p && (ftab_kind[h] & 1) == kind) {
            if (ftab_kind[h] & 2) {
                /* two meshes sharing a buffer: the chains decide, as before */
                return kind ? mesh_by_nrm(p) : mesh_by_pos(p);
            }
            return ftab_val[h];
        }
    }
    return NULL;
}

/* M47: the lookup's one-entry memo, valid while no mesh was registered or
 * dropped (reg_serial) */
static const void* memo_pb;
static const void* memo_nb;
static SkinMesh* memo_m;
static SkinMesh* memo_mn;
static unsigned memo_serial;
int gx_skin_fuse_live(void) { return nhsfs && fuse_mode(); }
/* the same, as a flag the per-primitive test reads without a call: set at
 * each registration (the options do not change in a run) */
int gx_skin_fuse_live_flag;

const GxSkinDec* gx_skin_fuse_for(const void* pos_base, const void* nrm_base, int can_fuse,
                                  int nrm_ok, int* nrm_skinned) {
    SkinMesh* m = NULL;
    SkinMesh* mn = NULL;
    *nrm_skinned = 0;
    if (!nhsfs || !fuse_mode()) {
        return NULL;
    }
    if (pos_base == memo_pb && nrm_base == memo_nb && memo_serial == reg_serial) {
        /* the last primitive's arrays (a model's faces come in runs) */
        m = memo_m;
        mn = memo_mn;
    } else {
        if (ftab_serial != reg_serial) {
            ftab_build();
        }
        m = pos_base ? ftab_find(pos_base, 0) : NULL;
        mn = nrm_base ? ftab_find(nrm_base, 1) : NULL;
        memo_pb = pos_base;
        memo_nb = nrm_base;
        memo_m = m;
        memo_mn = mn;
        memo_serial = reg_serial;
    }
    if (!m && !mn) {
        return NULL;
    }
    if (m && m->owner->owed && can_fuse && m->fuse_ok && (!mn || (mn == m && nrm_ok))) {
        *nrm_skinned = mn == m;
        fuse_last = m->owner;
        stat_fuse_prims++;
        stat_fuse_nrm_prims += mn == m;
        return &m->dec;
    }
    if (m && m->owner->owed == 1) {
        fuse_materialize(m->owner);
    }
    if (mn && mn->owner->owed == 1) {
        fuse_materialize(mn->owner);
    }
    return NULL;
}

void gx_skin_fuse_stamp(unsigned pos) {
    if (fuse_last) {
        fuse_last->dec_pos = pos;
        fuse_last->dec_valid = 1;
    }
}

void gx_skin_rewrite_notify(const void* p) {
    SkinMesh* m;
    if (!nhsfs || !p) {
        return;
    }
    m = mesh_by_pos(p);
    if (m && m->owner->owed == 1) {
        stat_fuse_mat_rewrite++;
        fuse_materialize(m->owner);
    }
}

void gx_skin_rewrite_notify_all(void) {
    int i;
    for (i = 0; i < nhsfs; i++) {
        if (hsfs[i].hsf && hsfs[i].owed == 1) {
            stat_fuse_mat_rewrite++;
            fuse_materialize(&hsfs[i]);
        }
    }
}

/* --skinverify: the run the decode just wrote (n vertices of `stride` at
 * `v`), against the arrays the game's body wrote, every fused vertex */
void gx_skin_fuse_verify(const GxSkinDec* d, const u8* list, u32 n, u32 vbytes, int pos_off,
                         int nrm_off, const u8* v, u32 stride, int off_nrm, int nrm_skinned) {
    u32 i;
    for (i = 0; i < n; i++, list += vbytes, v += stride) {
        u32 ix = ((u32)list[pos_off] << 8) | list[pos_off + 1];
        if (ix < d->npos && d->pent[ix] == GX_SKIN_UNNAMED) {
            stat_verify_unnamed++;
        }
        stat_verify_pos++;
        if (memcmp(v, d->live_pos + (size_t)ix * 12, 12) != 0) {
            stat_verify_pos_bad++;
            if (stat_verify_pos_bad <= 8) {
                port_log("port> skinverify: position %u (entry %u) differs at frame %u\n", ix,
                         ix < d->npos ? d->pent[ix] : 0xFFFFu, gl13_frame_number());
            }
        }
        if (nrm_skinned && off_nrm >= 0) {
            u32 nx = ((u32)list[nrm_off] << 8) | list[nrm_off + 1];
            stat_verify_nrm++;
            if (memcmp(v + off_nrm, d->live_nrm + (size_t)nx * 12, 12) != 0) {
                stat_verify_nrm_bad++;
                if (stat_verify_nrm_bad <= 8) {
                    port_log("port> skinverify: normal %u (entry %u) differs at frame %u\n", nx,
                             nx < d->nnrm ? d->nent[nx] : 0xFFFFu, gl13_frame_number());
                }
            }
        }
    }
}

void gx_skin_fuse_count(unsigned long verts) { stat_fuse_verts += verts; }

/* objMesh, before its read of hsf->matrix->data[mesh]. */
void port_envelope_sync(HSFDATA* hsf) {
    SkinHsf* h;
    if (gx_skin_mode() == 0) {
        return;
    }
    h = hsf_find(hsf);
    if (!h) {
        /* a restored process (PLAN.md 24): the registry is not in the
         * snapshot, and a model whose motion has stopped will not call
         * EnvelopeProc again, but it is about to be drawn */
        if (!hsf->matrix || !hsf->cenvNum) {
            return;
        }
        h = hsf_register(hsf, gl13_frame_number());
        if (!h) {
            return;
        }
        h->serial++;
        h->mtx_dirty = 1;
        h->skin_dirty = 1;
    }
    h->last_frame = gl13_frame_number();
    if (h->cpu || !(h->mtx_dirty || h->skin_dirty)) {
        return;
    }
    if (!hsf_live(h, "objMesh sync: the HSF no longer describes the entry")) {
        return;
    }
    if (gl13_draw_off()) {
        /* a consumed frame: what this feeds goes into GX calls the frame
         * mode drops, and the next drawn frame syncs again */
        stat_sync_skipped_consumed++;
        return;
    }
    if (gx_skin_mode() == 1) {
        hsf_body(h);
    } else {
        hsf_mtx_sync(h);
    }
}

/* GXSetArray(GX_VA_POS, p): a skinned mesh about to be drawn whose HSF is
 * still dirty -- a hooked model, drawn inside its parent's object walk before
 * its own EnvelopeProc of the frame ran -- gets the body now.  Deferred-CPU
 * mode only: the palette path skins from the rest pose at the decode. */
static SkinHsf* bound_hsf; /* M29: the skinned HSF whose position array is bound */
void gx_skin_stamp_decode(unsigned pos) {
    if (bound_hsf) {
        bound_hsf->dec_pos = pos;
        bound_hsf->dec_valid = 1;
    }
}

void gx_skin_array_bound(const void* p) {
    SkinMesh* m;
    bound_hsf = NULL;
    if (gx_skin_mode() != 1 || !p || gl13_draw_off()) {
        return;
    }
    for (m = mesh_hash[hash_ptr(p)]; m; m = m->hnext) {
        if (m->vtxenv == p) {
            SkinHsf* h = m->owner;
            bound_hsf = h;
            const HSFBUFFER* v;
            if (!(h->mtx_dirty || h->skin_dirty) || h->cpu) {
                return;
            }
            /* every pointer on the way is the game's; none is followed
             * before it is known to be inside MEM1 and still what was
             * registered (the M18 fault read m->obj of a freed model here) */
            if (port_opt.noskinlifetime) {
                /* the M18 read, kept for the reproduction: m->obj may be freed.
                 * Report-only checks first, so the mechanism is on record even
                 * when the garbage read happens not to fault. */
                HSFDATA* hsf = h->hsf;
                int stale = !in_mem1(hsf, sizeof(*hsf)) || hsf->object != h->object ||
                            hsf->matrix != h->matrix || hsf->objectNum != h->objectNum ||
                            !in_mem1(m->obj, sizeof(HSFOBJECT)) ||
                            !in_mem1(m->obj->mesh.vertex, sizeof(HSFBUFFER));
                if (stale) {
                    stat_lever_stale_reads++;
                    if (stat_lever_stale_reads <= 8) {
                        port_log("port> skin: --noskinlifetime: STALE READ at frame %u: array %p "
                                 "matched entry of hsf %p (registered/last frame %u), whose "
                                 "object %p is now %p, matrix %p now %p; obj %p mesh.vertex "
                                 "reads %p -- the M18 read follows\n",
                                 gl13_frame_number(), p, (void*)hsf, h->last_frame,
                                 (void*)h->object,
                                 in_mem1(hsf, sizeof(*hsf)) ? (void*)hsf->object : NULL,
                                 (void*)h->matrix,
                                 in_mem1(hsf, sizeof(*hsf)) ? (void*)hsf->matrix : NULL,
                                 (void*)m->obj,
                                 in_mem1(m->obj, sizeof(HSFOBJECT)) ? (void*)m->obj->mesh.vertex
                                                                    : NULL);
                    }
                }
                if (m->obj->mesh.vertex && m->obj->mesh.vertex->data == p) {
                    hsf_body(h);
                    stat_body_at_bind++;
                }
                return;
            }
            if (!hsf_live(h, "array bind: the HSF no longer describes the entry")) {
                return;
            }
            if (m->obj != &h->object[m->objIdx] || !in_mem1(m->obj, sizeof(HSFOBJECT))) {
                guard_hit(h, "array bind: the mesh's object moved");
                return;
            }
            v = m->obj->mesh.vertex;
            if (!in_mem1(v, sizeof(*v))) {
                guard_hit(h, "array bind: the mesh's vertex table is outside MEM1");
                return;
            }
            if (v->data == p) {
                hsf_body(h);
                stat_body_at_bind++;
            }
            return;
        }
    }
}

/* ---- the draw side ----------------------------------------------------------- */

SkinMesh* gx_skin_lookup(const void* pos_array, unsigned frame) {
    SkinMesh* m;
    if (!pos_array || gx_skin_mode() != 2) {
        return NULL;
    }
    for (m = mesh_hash[hash_ptr(pos_array)]; m; m = m->hnext) {
        if (m->vtxenv == pos_array) {
            SkinHsf* h = m->owner;
            /* a model that was unloaded cannot have called EnvelopeProc:
             * an entry nothing has touched for a few frames is stale, and so
             * is one whose HSF no longer points at this buffer */
            if (h->cpu || frame > h->last_frame + 4 ||
                m->obj->mesh.vertex == NULL || m->obj->mesh.vertex->data != pos_array) {
                return NULL;
            }
            return m;
        }
    }
    return NULL;
}

const void* gx_skin_rest_pos(const SkinMesh* m) {
    /* SetEnvelopMain: a mesh a cluster or shape wrote this frame (writeNum)
     * is skinned from its own buffer, otherwise from the file's rest pose */
    return m->obj->mesh.writeNum != 0 ? m->obj->mesh.vertex->data : m->obj->mesh.file[0];
}

const void* gx_skin_rest_nrm(const SkinMesh* m) { return m->obj->mesh.file[1]; }

/* SetEnvelop's per-entry matrix, verbatim: the same PSMTX calls in the same
 * order on the same operands, so the bits match the CPU path's.  `inv` is what
 * SetEnvelopMain put in MtxTop[Meshno]; `top`/`nObj`/`nMesh` index the bone and
 * bind tables the way the game's statics do. */
static void ent_pose(const SkinMesh* m, const SkinEnt* e, Mtx* top, u32 nobj, u32 nmesh,
                     const Mtx inv, Mtx P, Mtx N) {
    Vec sc;
    Mtx sp140, sp1A0, spE0, sp170;
    int meshNo = m->meshNo;
    switch (e->kind) {
        case SKIN_ENT_SINGLE:
            PSMTXConcat(top[nmesh + e->t1], top[nmesh + nobj + nobj * meshNo + e->t1], sp140);
            PSMTXConcat(inv, sp140, sp1A0);
            Hu3DMtxScaleGet(sp1A0, &sc);
            if (sc.x != 1.0f || sc.y != 1.0f || sc.z != 1.0f) {
                PSMTXScale(spE0, 1.0 / sc.x, 1.0 / sc.y, 1.0 / sc.z);
                PSMTXConcat(spE0, sp1A0, sp170);
                PSMTXInvXpose(sp170, sp170);
            } else {
                PSMTXInvXpose(sp1A0, sp170);
            }
            PSMTXCopy(sp1A0, P);
            PSMTXCopy(sp170, N);
            break;
        case SKIN_ENT_DUAL: {
            Mtx spB0, sp110, sp80;
            f32 w;
            int r, c;
            PSMTXConcat(top[nmesh + e->t1], top[nmesh + nobj + nobj * meshNo + e->t1], sp140);
            PSMTXConcat(inv, sp140, sp1A0);
            PSMTXConcat(top[nmesh + e->t2], top[nmesh + nobj + nobj * meshNo + e->t2], sp140);
            PSMTXConcat(inv, sp140, spB0);
            w = e->w;
            for (r = 0; r < 3; r++) {
                for (c = 0; c < 4; c++) {
                    sp140[r][c] = sp1A0[r][c] * w;
                }
            }
            w = 1.0f - e->w;
            for (r = 0; r < 3; r++) {
                for (c = 0; c < 4; c++) {
                    sp110[r][c] = spB0[r][c] * w;
                }
            }
            for (r = 0; r < 3; r++) {
                for (c = 0; c < 4; c++) {
                    sp80[r][c] = sp110[r][c] + sp140[r][c];
                }
            }
            Hu3DMtxScaleGet(sp80, &sc);
            if (sc.x != 1.0f || sc.y != 1.0f || sc.z != 1.0f) {
                PSMTXScale(spE0, 1.0 / sc.x, 1.0 / sc.y, 1.0 / sc.z);
                PSMTXConcat(spE0, sp80, sp110);
                PSMTXInvXpose(sp110, sp110);
            } else {
                PSMTXInvXpose(sp80, sp110);
            }
            PSMTXCopy(sp80, P);
            PSMTXCopy(sp110, N);
            break;
        }
        case SKIN_ENT_MULTI: {
            /* v' = v + sum w_i (M_i v - v)  ==  [I + sum w_i (M_i - I)] v, and
             * the same for the normal through the inverse transposes */
            const HSFCENVMULTIWEIGHT* w = e->multi->weight;
            u32 k;
            int r, c;
            PSMTXIdentity(P);
            PSMTXIdentity(N);
            for (k = 0; k < e->multi->weightNum; k++, w++) {
                PSMTXConcat(top[nmesh + w->target], top[nmesh + nobj + nobj * meshNo + w->target],
                            sp1A0);
                PSMTXConcat(inv, sp1A0, sp1A0);
                PSMTXInvXpose(sp1A0, sp170);
                for (r = 0; r < 3; r++) {
                    for (c = 0; c < 4; c++) {
                        f32 i = (r == c) ? 1.0f : 0.0f;
                        P[r][c] += w->value * (sp1A0[r][c] - i);
                        N[r][c] += w->value * (sp170[r][c] - i);
                    }
                }
            }
            break;
        }
        default:
            PSMTXIdentity(P);
            PSMTXIdentity(N);
            break;
    }
}

/* Bring the mesh's per-entry matrices up to the HSF's current pose. */
void gx_skin_pose(SkinMesh* m) {
    SkinHsf* h = m->owner;
    HSFMATRIX* mx;
    Mtx inv;
    int e;
    if (m->pose_serial == h->serial) {
        return;
    }
    hsf_mtx_sync(h);
    mx = h->hsf->matrix;
    /* SetEnvelopMain: PSMTXInverse(MtxTop[&obj[nMesh] - object], MtxTop[Meshno]) */
    PSMTXInverse(mx->data[mx->base_idx + m->objIdx], inv);
    for (e = 0; e < m->nent; e++) {
        ent_pose(m, &m->ent[e], mx->data, mx->count, mx->base_idx, inv, m->P[e].m, m->N[e].m);
    }
    m->pose_serial = h->serial;
    stat_pose_builds++;
}

/* --skinstats accounting from the decode, per vertex */
void gx_skin_count_vertex(const SkinMesh* m, unsigned pos_ix, unsigned nrm_ix, int have_nrm) {
    int e = pos_ix < (unsigned)m->nvtx ? m->pos_ent[pos_ix] : 0;
    switch (m->ent[e].kind) {
        case SKIN_ENT_SINGLE: stat_verts_single++; break;
        case SKIN_ENT_DUAL: stat_verts_dual++; break;
        case SKIN_ENT_MULTI: stat_verts_multi++; break;
        default: stat_verts_copy++; break;
    }
    if (have_nrm && nrm_ix < (unsigned)m->nnrm && m->nrm_ent[nrm_ix] != e) {
        stat_verts_nrm_mismatch++;
    }
    frame_skin_verts++;
}

void gx_skin_frame_end(void) {
    bound_hsf = NULL; /* M29: a bind is per frame; never stamp across one */
    if (frame_skin_verts) {
        stat_frames_with_skin++;
        if (frame_skin_verts > worst_frame_skin_verts) {
            worst_frame_skin_verts = frame_skin_verts;
        }
    }
    frame_skin_verts = 0;
}

void gx_skin_report(void) {
    sr_report();
    unsigned long tot;
    if (!stat_proc_calls) {
        return;
    }
    port_log("port> skin: %s; EnvelopeProc %u calls: %u deferred, %u run by the game; "
             "%u HSFs registered (%u rebuilt); %u deferred bodies run at the draw (%u of "
             "them at the array bind), %u syncs skipped on consumed frames; palette: "
             "%u bone syncs, %u pose builds\n",
             gx_skin_mode() == 0 ? "--cpuskin" : gx_skin_mode() == 1 ? "deferred CPU skinning"
                                                                     : "--palette",
             stat_proc_calls, stat_proc_deferred, stat_proc_cpu, stat_registered,
             stat_rebuilt, stat_body_runs, stat_body_at_bind, stat_sync_skipped_consumed,
             stat_sync_runs, stat_pose_builds);
    port_log("port> skin: lifetime: %u entries dropped by the game's frees, %u guard hits "
             "(must be 0); signatures: %lu full, %lu by the pointers alone (M47%s)\n",
             stat_freed_drops, stat_guard_hits, stat_sig_full, stat_sig_cheap,
             port_opt.fullskinsig ? ", --fullskinsig" : "");
    if (port_opt.noskinlifetime) {
        port_log("port> skin: --noskinlifetime: %u entries left behind by frees (%u of them "
                 "dirty), %u binds read through a stale entry\n",
                 stat_lever_stale_left, stat_lever_stale_dirty, stat_lever_stale_reads);
    }
    if (fuse_mode() || port_opt.skinverify) {
        port_log("port> skin: M47 skin at the decode%s: %u meshes can (%u cannot: %u arrays, %u "
                 "ranges, %u multi); %lu poses built, %lu old bodies (in place or not fusable), "
                 "%lu arrays written on demand (%lu for a rewriter); %lu primitives skinned at "
                 "the decode (%lu with their normals), %lu vertices; %lu HSF pairs sharing a "
                 "buffer (the game's body); %lu HSFs' arrays written as the game freed them "
                 "(M48)\n",
                 port_opt.skinverify ? " (--skinverify)" : "", stat_fuse_meshes_ok,
                 stat_fuse_meshes_no[1] + stat_fuse_meshes_no[2] + stat_fuse_meshes_no[3],
                 stat_fuse_meshes_no[1], stat_fuse_meshes_no[2], stat_fuse_meshes_no[3],
                 stat_fuse_poses, stat_fuse_old_bodies, stat_fuse_materialized,
                 stat_fuse_mat_rewrite, stat_fuse_prims, stat_fuse_nrm_prims, stat_fuse_verts,
                 stat_fuse_shared, stat_fuse_mat_freed);
    }
    if (port_opt.skinvec) {
        extern unsigned long gx_skinvec_calls, gx_skinvec_nj_set;
        port_log("port> skin: --skinvec: %lu AltiVec runs, %lu of them found the vector unit "
                 "in non-Java mode and set Java mode\n",
                 gx_skinvec_calls, gx_skinvec_nj_set);
    }
    if (port_opt.skinverify) {
        port_log("port> skinverify: %lu positions compared, %lu differ (%lu of them unnamed "
                 "indices); %lu normals compared, %lu differ\n",
                 stat_verify_pos, stat_verify_pos_bad, stat_verify_unnamed, stat_verify_nrm,
                 stat_verify_nrm_bad);
    }
    tot = stat_verts_single + stat_verts_dual + stat_verts_multi + stat_verts_copy;
    if (tot) {
        port_log("port> skin: %lu vertices skinned on the GPU over %u drawn frames "
                 "(%.0f per such frame, worst %lu): single %lu (%.1f%%), dual %lu (%.1f%%), "
                 "multi %lu (%.1f%%), copy %lu (%.1f%%); %lu with a normal bound to a "
                 "different entry than the position\n",
                 tot, stat_frames_with_skin,
                 stat_frames_with_skin ? (double)tot / stat_frames_with_skin : 0.0,
                 worst_frame_skin_verts, stat_verts_single, 100.0 * stat_verts_single / tot,
                 stat_verts_dual, 100.0 * stat_verts_dual / tot, stat_verts_multi,
                 100.0 * stat_verts_multi / tot, stat_verts_copy,
                 100.0 * stat_verts_copy / tot, stat_verts_nrm_mismatch);
    }
}
