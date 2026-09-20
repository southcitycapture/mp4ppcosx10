/* The CPU mixer that stands in for the GameCube's dspSlave ucode.
 * Implemented in musyx_mix.c, called from musyx_sal.c's salCtrlDsp.
 */
#ifndef PORT_MUSYX_MIX_H
#define PORT_MUSYX_MIX_H

/* port.h FIRST (M24, PLAN.md 39.1): musyx/synth.h opens `#pragma pack(4)` and
 * never closes it (its `#pragma push` is Metrowerks', which GCC ignores), so
 * every struct declared after a MusyX header in a translation unit is packed
 * -- including PortOptions, whose `long long seed` is 8-aligned everywhere
 * else.  musyx_mix.c had read port_opt through that packed layout since M20
 * added a field before `seed`: depop was stuckwatch, resample4 was soak,
 * mixcheck was skindeferall, mixtrace was restore_lax. */
#include "port.h"

#include "musyx/dspvoice.h"
#include "musyx/musyx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, from salStartAi, after salInitDspCtrl has allocated the
 * dspVoice array and the studio accumulation buffers. */
void port_musyx_mix_init(void);

/* Render exactly one DSP frame: 160 frames of interleaved stereo signed
 * 16-bit at 32 kHz (640 bytes) into `dest`, and update the host-visible voice
 * state (currentAddr above all) that MusyX reads back.  This is the fused
 * path: control and values in one pass, on the game thread -- the code
 * every run before M24 ran, and what `--threads 0` runs. */
void port_musyx_mix_frame(short* dest);

/* ---- M24: the frame in two halves (PLAN.md 39.2) ---------------------------
 *
 * The same frame split along the one seam it has: everything MusyX can see
 * (voice starts and ends, the ADSR, the volume ramps' bookkeeping, the
 * position write-backs) is the CONTROL half and runs on the game thread at
 * the retrace, exactly where the fused frame ran; the sample VALUES (the
 * ADPCM decode, the resampler, the gains, the buses, the depop, the aux
 * return, the output) are the VALUE half and run on the worker from a copy
 * of every input the control half read.  The control half drives the same
 * per-sample loop with the value arithmetic elided, so the two halves agree
 * on where every voice is at every sample -- the value half checks this at
 * the join.  One body, three instantiations (BOTH / CTL / VAL). */

/* One voice of one frame, as the value half needs it: the DSPvoice as the
 * mixer read it at entry (the value half works on this copy and never on
 * the real one), and for a voice starting this frame its MixVoice as the
 * control half initialised it, before the priming decodes. */
#define MIX_PLAN_SKIP 0  /* the value half has nothing to do for this voice */
#define MIX_PLAN_RUN 1   /* a voice already playing */
#define MIX_PLAN_START 2 /* started this frame: prime from `mv`, then run */
#define MIX_PLAN_CLEAR 3 /* M25: a start the control half REFUSED (a zero-length
                          * one-shot, an envelope that is done at once, an
                          * unreadable sample): its MixVoice is zeroed on both
                          * sides.  Before M25 the plan said SKIP and the value
                          * half kept the slot's previous voice -- live, if
                          * MusyX had stopped it and reused the slot -- which the
                          * join then took back: the soak's 14 mismatches in
                          * 254,573 joins (PLAN.md 40.1) */
typedef struct MixVoicePlan {
    u8 vi, studio, action, has_note;
    DSPvoice dv;
    /* MixVoice is private to musyx_mix.c; the plan carries it as bytes. */
    unsigned char mv[176];
    /* the START trace note (a MEM1 sample's heap block), printed by the
     * value half in the place the fused path prints it */
    const void* note_block;
    int note_size, note_heap;
    u32 note_num, note_call;
} MixVoicePlan;

typedef struct MixStudioPlan {
    u8 state, isMaster, numInputs;
    u32 type;
    DSPinput in[7];
} MixStudioPlan;

#define MIX_MAX_STUDIOS 8
typedef struct MixFramePlan {
    u8 salFrame, salAuxFrame;
    short* dest;
    unsigned long retrace;   /* VIGetRetraceCount() at the tick, for the logs */
    unsigned nvoices;
    MixVoicePlan* voices;    /* nvoices entries out of the job's pool */
    MixStudioPlan st[MIX_MAX_STUDIOS];
} MixFramePlan;

/* the control half, game thread: fills `fp` (studios, and one MixVoicePlan
 * per voice walked, taken from `pool`, at most `pool_cap`) and advances the
 * game-visible state.  Returns the number of pool entries used, or -1 if
 * the pool ran out (the caller then runs the frame fused instead: nothing
 * has been touched). */
int port_musyx_mix_frame_ctl(MixFramePlan* fp, short* dest, unsigned long retrace,
                             MixVoicePlan* pool, unsigned pool_cap);
/* the value half: the worker, or the game thread at a late join */
void port_musyx_mix_frame_val(const MixFramePlan* fp);
/* the tick's bracket: before the first control half of a tick the worker's
 * voice table is seeded from the game's; at the join the game's is taken
 * back from the worker's, after the position check */
void port_musyx_mix_job_begin(void);
void port_musyx_mix_job_reconcile(void);
/* a live voice the value half must not read behind the game's back: a MEM1
 * sample (the game may free or rewrite it mid-frame) or a stream / virtual
 * sample (compType 4/5, refilled in place by the streaming layer).  The
 * tick then runs fused, as before M24. */
int port_musyx_mix_needs_inline(void);
/* set by the control half when a voice it started this frame is one of the
 * above: the job so far is finished on the game thread at once */
int port_musyx_mix_ctl_poisoned(void);

void port_musyx_mix_shutdown(void);
void port_musyx_mix_report(void);

/* Set by --mute: keep every bit of state and timing, emit silence. */
extern int port_musyx_mix_mute;

#ifdef __cplusplus
}
#endif

#endif
