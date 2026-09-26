/* Port replacement for <dolphin/os/OSFastCast.h>.
 *
 * The decomp's copy implements the Gekko paired-single fast casts with
 * Metrowerks `asm {}` blocks around `psq_l`/`psq_st`.  The 7450 has no paired
 * singles, so the port uses plain C conversions with the same semantics
 * (truncation toward zero, the GQR scale exponent is always 0 in this game)
 * and `OSInitFastCast` becomes a no-op.  Copied over the mirrored include tree
 * by port/Makefile; the decomp's own header is untouched.
 *
 * M45 (PLAN.md 60): **and the quantised store saturates.**  `psq_st` into a
 * u8/s8/u16/s16 clamps to the type's range before it truncates (Dolphin's
 * ScaleAndClamp: SaturatingCast); a C cast of an out-of-range float wraps (or
 * is undefined).  M1..M44 wrapped: the board's screen filter
 * (board/main.c UpdateFilter) fades its alpha out one step past zero before
 * it is killed, and the step's -12.8 stored as 243 -- a black screen for a
 * frame at the end of every board fade (the user's "black blip" at a Mega
 * Mushroom's use).  NaN stores 0.
 */
#ifndef _DOLPHIN_OSFASTCAST
#define _DOLPHIN_OSFASTCAST

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void OSInitFastCast(void) {}

static inline s16 __OSf32tos16(f32 inF) {
    return inF >= 32767.0f ? 32767 : inF <= -32768.0f ? -32768 : inF == inF ? (s16)inF : 0;
}
static inline u16 __OSf32tou16(f32 inF) {
    return inF >= 65535.0f ? 65535 : inF > 0.0f ? (u16)inF : 0;
}
static inline s8 __OSf32tos8(f32 inF) {
    return inF >= 127.0f ? 127 : inF <= -128.0f ? -128 : inF == inF ? (s8)inF : 0;
}
static inline u8 __OSf32tou8(f32 inF) { return inF >= 255.0f ? 255 : inF > 0.0f ? (u8)inF : 0; }

static inline void OSf32tos16(f32* f, s16* out) { *out = __OSf32tos16(*f); }
static inline void OSf32tou16(f32* f, u16* out) { *out = __OSf32tou16(*f); }
static inline void OSf32tos8(f32* f, s8* out) { *out = __OSf32tos8(*f); }
static inline void OSf32tou8(f32* f, u8* out) { *out = __OSf32tou8(*f); }

static inline f32 __OSs8tof32(const s8* arg) { return (f32)*arg; }
static inline f32 __OSs16tof32(const s16* arg) { return (f32)*arg; }
static inline f32 __OSu8tof32(const u8* arg) { return (f32)*arg; }
static inline f32 __OSu16tof32(const u16* arg) { return (f32)*arg; }

static inline void OSs8tof32(const s8* in, f32* out) { *out = (f32)*in; }
static inline void OSs16tof32(const s16* in, f32* out) { *out = (f32)*in; }
static inline void OSu8tof32(const u8* in, f32* out) { *out = (f32)*in; }
static inline void OSu16tof32(const u16* in, f32* out) { *out = (f32)*in; }

#ifdef __cplusplus
}
#endif

#endif
