/* M43 (PLAN.md 58.2): the render thread's instance of the translation.
 *
 * gl13_state.c, gx_tev.c, gx_tfs.c and gx_vprog.c are compiled twice (port/
 * Makefile, RTI_OBJS).  This header is -include'd into the second compile:
 * every global those four files define is renamed rti_* (the generated
 * gx_rti_names.h), so the second instance has its own GL state shadow, its
 * own TEV / fragment-shader / vertex-program caches and its own statistics,
 * and it reads the render thread's replica of GXState instead of the game
 * thread's.  What the game thread writes and the translation reads outside
 * GXState is redirected to the copies gx_rtgx.c sets from each record.
 * Nothing here is used by the first (the game thread's) instance. */
#ifndef PORT_GX_RTI_H
#define PORT_GX_RTI_H
#define GX_RTI 1
#include "gx_rti_names.h"
/* the replica (gx_rtgx.c) */
#define gx gx_rti
/* the game thread's per-draw globals, as the record carried them */
#define gx_hilite_stage rti_gx_hilite_stage
#define gx_hilite_mode rti_gx_hilite_mode
#define gx_force_flags rti_gx_force_flags
#define gx_unit_alpha_min rti_gx_unit_alpha_min
/* the bound-texture question, of the replica */
#define gx_bound_tex rti_gx_bound_tex
/* the draw-off switch, as it was when the batch was recorded */
#define gl13_live rti_gl13_live
/* gx_warn's once-per-message table is the game thread's */
#define gx_warn rti_gx_warn
/* the texture binds are the game thread's, always (gx_rtgx.c says why) */
#define gx_tex_bind_swapped rti_tex_bind_refused
#define gx_tex_bind_tiled rti_tex_bind_tiled_refused
#endif
