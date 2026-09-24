/* M43 (PLAN.md 58.6): the vertex rewriters' join, made precise.
 *
 * ClusterProc and ShapeProc (the game's morph targets) rewrite a mesh's
 * position buffer in place inside the frame.  Since M29 the display-list
 * decode that reads those buffers may still be pending on the render thread,
 * so patches.txt made both call the port first, and the port waited for the
 * WHOLE decode stream to catch up (rt_decode_join): on m410 that was 11,420
 * waits and 1.3 s of the game thread's minigame, on m414 (ShapeProc) 1.3 s,
 * m450 1.2, m430 0.8 -- the game thread idle while the render thread decoded
 * lists that never read the buffers about to be written.
 *
 * Now the hooks name what they will write, walking the same structures the
 * game's own loops walk, to the byte: ClusterProc writes each target object's
 * `mesh.vertex->data` at the indices of the cluster's part list (and, with
 * cenvNum, entries 0..count-1); ShapeProc writes each shape object's buffer
 * at 0..count-1 of each shape.  The render thread's stream remembers, for
 * every array base a pending decode record reads, the last such record
 * (rt.c rt_vtx_note / rt_vtx_rewrite_range), and the hook waits for the last
 * record whose array starts inside a range about to be written -- the only
 * records that can read it: every indexed read of a record starts at its
 * array's base, and the buffers are the game's own allocations.  A record
 * the table had to forget is waited for, always.  `--oldvtxjoin` is M29's
 * wait for everything. */
#include <dolphin/types.h>
#include "port.h"

#include "game/hsfformat.h"
#include "game/hu3d.h"

void port_vtx_rewrite(const char* who);              /* rt.c: M29's join */
void rt_vtx_rewrite_range(const void* p, unsigned long n, const char* who);
void port_vtx_rewrite_done(void);                    /* rt.c: the vertex cache's epoch */

static unsigned long st_cluster_calls, st_shape_calls, st_ranges;

void port_vtx_rewrite_cluster(HU3DMODEL* model) {
    s32 i, j;
    st_cluster_calls++;
    if (port_opt.oldvtxjoin) {
        port_vtx_rewrite("ClusterProc");
        return;
    }
    for (i = 0; i < 4; i++) {
        s32 mot = model->motIdCluster[i];
        HSFDATA* mh;
        HSFCLUSTER* c;
        if (mot == -1) {
            continue;
        }
        mh = Hu3DMotion[mot].hsf;
        c = mh->cluster;
        for (j = 0; j < mh->clusterNum; j++, c++) {
            HSFOBJECT* o;
            u32 n, k;
            if (c->target == -1) {
                continue;
            }
            o = model->hsf->object + c->target;
            n = o->mesh.vertex->count; /* the cenvNum copy writes 0..count-1 */
            if (c->vertexNum != 0 && c->part) {
                /* SetClusterMain writes Vertextop[part->vertex[i]] */
                const u16* v = c->part->vertex;
                for (k = 0; k < c->part->num; k++) {
                    if ((u32)v[k] + 1 > n) {
                        n = (u32)v[k] + 1;
                    }
                }
            }
            st_ranges++;
            rt_vtx_rewrite_range(o->mesh.vertex->data, (unsigned long)n * sizeof(Vec), "ClusterProc range");
        }
    }
    port_vtx_rewrite_done();
}

void port_vtx_rewrite_shape(HSFDATA* hsf) {
    HSFOBJECT* o = hsf->object;
    s32 i, s;
    st_shape_calls++;
    if (port_opt.oldvtxjoin) {
        port_vtx_rewrite("ShapeProc");
        return;
    }
    for (i = 0; i < hsf->objectNum; i++, o++) {
        u32 n;
        if (o->type != 2 || o->mesh.shapeNum == 0) {
            continue;
        }
        n = o->mesh.vertex->count;
        for (s = 0; s < (s32)o->mesh.shapeNum; s++) {
            /* SetShapeMain writes Vertextop[0 .. shape->count-1] */
            if (o->mesh.shape[s] && (u32)o->mesh.shape[s]->count > n) {
                n = (u32)o->mesh.shape[s]->count;
            }
        }
        st_ranges++;
        rt_vtx_rewrite_range(o->mesh.vertex->data, (unsigned long)n * sizeof(Vec), "ShapeProc range");
    }
    port_vtx_rewrite_done();
}

void port_vtx_rewrite_report(void) {
    if (st_cluster_calls || st_shape_calls) {
        port_log("port> vertex rewriters (M43): %lu ClusterProc, %lu ShapeProc calls, %lu ranges "
                 "named; the joins are the render thread's report (%s)\n",
                 st_cluster_calls, st_shape_calls, st_ranges,
                 port_opt.oldvtxjoin ? "--oldvtxjoin: the whole stream each time" : "precise");
    }
}
