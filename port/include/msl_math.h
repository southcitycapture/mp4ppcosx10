/* M34 (PLAN.md 49.7): the game's sqrtf, as the console computes it.
 *
 * The decomp's include/dolphin/math.h carries MSL's inline sqrtf for
 * Metrowerks: `if (x > 0.0f) { frsqrte + three Newton steps } return x;` --
 * for zero, a NEGATIVE number or NaN it returns the argument itself, not
 * NaN.  The mirror replaces MSL's math.h with the host's, so under GCC the
 * game linked libm's sqrtf, whose sqrtf(-0.5f) is NaN.  m417's water mesh
 * builds a rim weight as sqrtf((750 - r) * 0.01f) for r up to 850 (water.c
 * fn_1_3D58): on the console those weights are small negative numbers; on
 * the port they were NaN, and the first wave to reach such a vertex made it
 * NaN -- which the Radeon draws as a sliver to the screen centre (the
 * "streak fan" of M31's snapshot) and the Intel bench drops.
 *
 * This header is forced onto every game translation unit (-include, the
 * Makefile's GAME_CFLAGS) after the host's math.h; the port's own sources
 * do not take it.  Positive arguments are unchanged: port_msl_sqrtf calls
 * port_sqrtf, which is libm's correctly rounded value. */
#ifndef PORT_MSL_MATH_H
#define PORT_MSL_MATH_H
/* No <math.h> here: kerent.c (the SDK export table) declares every libm
 * name as `void f(void)` and includes no math.h.  The macro alone does it --
 * the host math.h's own `float sqrtf(float);` becomes port_msl_sqrtf's
 * prototype in the units that include it, which is every unit that calls
 * sqrtf (through ext_math.h). */
#define sqrtf(x) port_msl_sqrtf(x)
#endif
