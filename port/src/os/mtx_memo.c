/* M42 (PLAN.md 57.3): the engine's two rotation builders, memoised.
 *
 * `mtxRot(m, x, y, z)` and `mtxRotCat(m, x, y, z)` (src/game/hsfdraw.c) build
 * a rotation from three angles in degrees -- up to three MTXRotRad (a sin/cos
 * pair and twelve stores each) and two or three general concats -- for every
 * object of every model the engine walks: the draw walk's objMesh/objNull/
 * objRoot, Hu3DModelObjMtxGet's PGObjCalc (a whole-tree walk per call), the
 * hooks, and the modules through kerent.  They are pure functions: mtxRot's
 * matrix is a function of the three angles' bits, mtxRotCat's of the input
 * matrix's twelve floats and the three angles' bits (every call below them --
 * MTXRotRad through the sin/cos memo, MTXIdentity, the blocked C_MTXConcat --
 * computes the same bits from the same bits, and none has another effect).
 * Static scenery and a pose held still ask the same questions every frame,
 * and a model walked twice in a frame asks them twice.
 *
 * So a direct-mapped table keyed on those bits (the whole key compared, not
 * a hash of it: a hit returns the very bits the game's body computed on the
 * same inputs).  patches.txt renames the game's bodies mtxRot_game /
 * mtxRotCat_game; a miss calls them.  --nomtxmemo: every call to them. */
#include <string.h>

#include <dolphin/types.h>
#include <dolphin/mtx.h>
#include "port.h"

void mtxRot_game(Mtx mtx, float x, float y, float z);
void mtxRotCat_game(Mtx mtx, float x, float y, float z);

#define ROT_SLOTS 2048
#define CAT_SLOTS 2048

typedef struct {
    u32 k[3]; /* the angles' bits */
    u32 valid;
    f32 m[12];
} RotSlot;
typedef struct {
    u32 k[15]; /* the input matrix's twelve floats, then the angles */
    u32 valid;
    f32 m[12];
} CatSlot;

static RotSlot rot_tab[ROT_SLOTS];
static CatSlot cat_tab[CAT_SLOTS];
static unsigned long rot_hit, rot_miss, cat_hit, cat_miss;

static inline u32 mix(u32 h, u32 v) {
    h ^= v;
    h *= 0x9E3779B1u;
    return h ^ (h >> 15);
}

void mtxRot(Mtx mtx, float x, float y, float z) {
    u32 k[3], h;
    RotSlot* s;
    if (port_opt.nomtxmemo || (x == 0.0f && y == 0.0f && z == 0.0f)) {
        mtxRot_game(mtx, x, y, z); /* all zero: the identity, cheaper than a lookup */
        return;
    }
    memcpy(&k[0], &x, 4);
    memcpy(&k[1], &y, 4);
    memcpy(&k[2], &z, 4);
    h = mix(mix(mix(0x2545F491u, k[0]), k[1]), k[2]);
    s = &rot_tab[h & (ROT_SLOTS - 1)];
    if (s->valid && s->k[0] == k[0] && s->k[1] == k[1] && s->k[2] == k[2]) {
        memcpy(mtx, s->m, sizeof(s->m));
        rot_hit++;
        return;
    }
    mtxRot_game(mtx, x, y, z);
    s->k[0] = k[0];
    s->k[1] = k[1];
    s->k[2] = k[2];
    memcpy(s->m, mtx, sizeof(s->m));
    s->valid = 1;
    rot_miss++;
}

void mtxRotCat(Mtx mtx, float x, float y, float z) {
    u32 k[15], h;
    CatSlot* s;
    int i;
    if (port_opt.nomtxmemo || (x == 0.0f && y == 0.0f && z == 0.0f)) {
        mtxRotCat_game(mtx, x, y, z); /* all zero: the game's body does nothing */
        return;
    }
    memcpy(k, mtx, 48);
    memcpy(&k[12], &x, 4);
    memcpy(&k[13], &y, 4);
    memcpy(&k[14], &z, 4);
    h = 0x2545F491u;
    for (i = 0; i < 15; i++) {
        h = mix(h, k[i]);
    }
    s = &cat_tab[h & (CAT_SLOTS - 1)];
    if (s->valid && !memcmp(s->k, k, sizeof(k))) {
        memcpy(mtx, s->m, sizeof(s->m));
        cat_hit++;
        return;
    }
    mtxRotCat_game(mtx, x, y, z);
    memcpy(s->k, k, sizeof(k));
    memcpy(s->m, mtx, sizeof(s->m));
    s->valid = 1;
    cat_miss++;
}

void port_mtx_memo_report(void) {
    if (rot_hit + rot_miss + cat_hit + cat_miss) {
        port_log("port> rotation memo (M42): mtxRot %lu hits / %lu misses, mtxRotCat %lu hits / %lu misses%s\n",
                 rot_hit, rot_miss, cat_hit, cat_miss, port_opt.nomtxmemo ? " (--nomtxmemo)" : "");
    }
}
