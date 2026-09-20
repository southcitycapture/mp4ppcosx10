/* A CPU mixer that stands in for the GameCube's dspSlave ucode.
 *
 * Background: on Dolphin, `salBuildCommandList()` (extern/musyx/src/musyx/
 * runtime/hw_dspctrl.c) walks every active studio and voice and marshals a
 * `_PB`-based command list for the DSP ucode to execute.  That entire
 * function -- studio zeroing, studio-input chaining, per-voice ADPCM/PCM
 * decode setup, volume-ramp setup, aux fold-back and the final L/R/S sum --
 * is wrapped in `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN` (hw_dspctrl.c:738-
 * 2009) and therefore compiles to *nothing* on MUSY_TARGET_PC.  The `_PB`
 * per-voice parameter blocks (`DSPvoice::pb`) are consequently never filled
 * in on this target -- `pb` itself is not even guaranteed to point anywhere
 * useful -- so this file must not read or write through `dsp_vptr->pb` at
 * all.  Instead it keeps its own shadow state per voice (`MixVoice`, below)
 * and drives everything else -- `DSPvoice`'s host-visible fields and the
 * `DSPstudioinfo` bus buffers -- exactly the way the Dolphin path would have
 * left them, because the sequencer, the streaming layer and the game itself
 * only ever look at those.
 *
 * This is therefore a fairly literal port of salBuildCommandList's opcode
 * order (zero buses -> studio inputs -> voices -> aux return -> output) onto
 * a plain sample-at-a-time CPU mixer, with the DSP's fixed-point `_PB`
 * representation replaced by ordinary 32-bit integer math.  Every deliberate
 * simplification against the original DSP behaviour is called out at the
 * point it is made; see also port_musyx_mix_report() and the port's PLAN.md
 * for the summary.
 *
 * Threading / performance: port_musyx_mix_frame() is called synchronously
 * from the game thread (musyx_sal.c's salCtrlDsp), roughly 3.3x per video
 * frame.  It must not block or allocate.  All per-voice scratch state is
 * preallocated in port_musyx_mix_init(); the per-sample inner loops are
 * plain scalar fixed-point integer code (no division, no floating point, no
 * function-pointer calls) so the cost stays predictable across ~64 voices.
 */
#include "port.h" /* before any MusyX header: see musyx_mix.h */
#include "musyx_mix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/memory.h"

static FILE* mixtrace_f; /* --mixtrace, opened by mixtrace_voice */

#include "musyx/adsr.h"
#include "musyx/dspvoice.h"
#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/sal.h"
#include "musyx/stream.h"
#include "musyx/synth.h"

#include "port.h"

int port_musyx_mix_mute;

/* ---- console geometry, confirmed in extern/musyx ------------------------- */
#define MIX_FRQ 32000u
#define FRAME_SAMPLES 160u    /* one salCtrlDsp call                        */
#define SUBFRAME_SAMPLES 32u  /* synthInfo.numSamples                       */
#define NUM_SUBFRAMES 5u
#define BUS_LEN 480u          /* 3 x 160: L[0..159] R[160..319] S[320..479] */
#define BUS_L_OFF 0u
#define BUS_R_OFF 160u
#define BUS_S_OFF 320u

#define VOL_UNITY 0x7fff /* 0x7FFF u16 == unity gain, per DSPvoice's *Vol fields */

/* salFrame / salAuxFrame are plain file-scope globals in hardware.c; nothing
 * in extern/musyx/include exports them, so we declare the same symbols here.
 * musyx_sal.c's comment (and hardware.c's snd_handle_irq) confirm they still
 * hold *this* frame's values while salCtrlDsp -- and therefore this file --
 * is running; they are advanced only after salCtrlDsp returns. */
extern u8 salFrame;
extern u8 salAuxFrame;

/* Which channel the console's SET_OPPOSITE_LR folded the surround channel
 * into at which sign.  Flip this if a real recording says we picked the
 * wrong side; nothing else needs to change. */
#define SURROUND_ADDS_TO_L 1
#define SURROUND_SUBTRACTS_FROM_R 1

/* ---- per-voice shadow state ----------------------------------------------
 *
 * This is the CPU-mixer equivalent of `_PB`: everything salBuildCommandList
 * would have derived from `smp_info`/`extraData` once at voice-start time and
 * then carried in the DSP's own registers frame to frame.  Indexed the same
 * way as the real `dspVoice` array (by voice number, 0..salNumVoices-1).
 */
typedef struct MixVoice {
    u8 live;      /* our own idea of "this slot has decode state" (0/1)     */
    u8 compType;  /* SAMPLE_INFO.compType at voice-start time               */
    u8 loopType;  /* 0: restore yn1/yn2/pred_scale; 1: restore pred_scale
                   * only, from dsp_vptr->streamLoopPS (streams/vsamples)   */
    u8 looping;

    u32 addrBase; /* smp_info.addr widened to u32, format-native units --
                   * used ONLY for the currentAddr write-back formulas
                   * (writeback_current_addr()), which must keep computing
                   * exactly what hwGetPos()'s inverse expects.  NOT used for
                   * actual memory access any more -- see readBase/readLen.  */
    const u8* readBase; /* resolved host pointer to sample byte 0 (see
                         * resolve_sample_ptr()); all decode reads index this
                         * with a RELATIVE offset, never addrBase.            */
    u32 readLen;        /* bytes valid from readBase; bound-checked on every
                         * read via voice_read_u8()/voice_read_s16be().      */
    u8 readKind;         /* SAMPLE_LOC_*, for aram_clamp_report() only        */
    u32 length;   /* smp_info.length: one-shot stops when curSample>=length */
    u32 loopStart;
    u32 loopEnd; /* inclusive: smp_info.loop + loopLength - 1               */

    /* ADPCM decode state (compType 0,1,4,5) */
    s16 coefTab[8][2];
    s32 yn1, yn2;
    u8 predScale;
    u32 frameOffset; /* 0..13: position within the current 14-sample block  */
    s16 loopY0, loopY1;
    u8 loopPS;

    u32 curSample; /* absolute sample index; matches what hwGetPos() would
                     * report for this voice (see hw_dspctrl.c:456-486)     */

    /* resampler: srcTypeSelect 2 has no lookahead; 0/1 keep the four-sample
     * window the console's own polyphase filter keeps -- `_PBSRC` is
     * `u16 last_samples[4]` (musyx/include/musyx/voice.h:98), zeroed at voice
     * start (hw_dspctrl.c:916-920), so this is the DSP's state and not an
     * invention of the port's.  hist[1] and hist[2] straddle the output
     * position; hist[0] is one behind and hist[3] one ahead.               */
    u16 srcType;
    u32 pitch; /* 16.16, 0x10000 == 1.0x; adopted from changed[]&8           */
    u32 phase; /* 16.16, in [0, 0x10000)                                    */
    s32 hist[4];

    u32 streamLoopCnt; /* stands in for the DSP's _PB.streamLoopCnt; nothing
                         * reads it yet -- see the report for what a host
                         * accessor would need.                             */
    u8 ended;      /* one-shot ran past its length                          */
    u8 postBreak;  /* hwBreak seen this voice, fading via a 10-unit release */
} MixVoice;

static MixVoice* voices;
typedef char mixvoice_fits_the_plan[sizeof(MixVoice) <= sizeof(((MixVoicePlan*)0)->mv) ? 1 : -1];
static u32 num_voices;
static int mixer_up;

/* ---- stats ---------------------------------------------------------------- */
static unsigned long stat_frames_mixed;
static unsigned long stat_voices_started;
static unsigned long stat_voices_ended;
static unsigned long stat_aram_clamped;
static unsigned long stat_peak_abs; /* largest |sample| ever written to dest */

/* Concurrency: how many voices this mixer actually rendered in one frame,
 * and the worst such count seen -- the number the 64-voice budget
 * extrapolation should really be checked against, once a real board run has
 * exercised it. */
static u32 stat_voices_active_this_frame;
static u32 stat_max_concurrent_voices;
static unsigned long stat_voices_no_extradata;

/* M7: the two audio repairs, counted rather than asserted.  `stat_depop_cuts`
 * is how many voices ended part-way through a frame -- the step the depop
 * ramp exists to unwind -- and `stat_clicks` is the same rule wavstat.py
 * applies to a --wav capture (a sample-to-sample step over half full scale),
 * evaluated in process so a soak reports it without a capture. */
static unsigned long stat_depop_cuts;
static unsigned long stat_clicks;
static long stat_worst_step;

static void add_dpop(s32* sum, s32 delta);
static void apply_depop(const MixFramePlan* fp);

/* --perf-gated per-call cost, in seconds; port_now_seconds() is the same
 * clock port_perf_* already uses elsewhere, so this composes with --perf
 * without adding its own overhead when the flag is off. */
static double stat_time_sum;
static double stat_time_worst;
static unsigned long stat_time_samples;

/* ---- resolving a sample address: ARAM offset, or a MEM1 host pointer -----
 *
 * A hardware run (15,400 frames on a real G4) turned up a real bug here,
 * diagnosed against `hwSaveSample` (extern/musyx/src/musyx/runtime/
 * hardware.c:559-576) -- worth recording in full because it is the single
 * most surprising thing in this milestone.
 *
 * On Dolphin, a freshly loaded sample lives in main memory, and
 * `hwSaveSample` is the call that moves it into ARAM and rewrites the sample
 * directory's address to match:
 *
 *     *((u32*)data) = (u32)aramStoreData((void*)*((u32*)data), len);
 *
 * -- i.e. "take the main-memory pointer this SDIR entry currently holds,
 * copy the bytes into ARAM, and replace the pointer with the ARAM address it
 * landed at." That line, like the rest of `hwSaveSample`'s body, is wrapped
 * in `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN` and therefore never runs on
 * this target. The consequence: `SDIR_DATA.addr` (and therefore
 * `SAMPLE_INFO.addr`, via `dataGetSample`, synthdata.c:625) is left holding
 * whatever it started as -- a genuine MEM1 pointer to the bank data the game
 * loaded off disc -- for every ordinary sequenced sample. This mixer used to
 * treat every `smp_info.addr` as an ARAM byte offset unconditionally, which
 * is only true for the two paths that go through *this port's own*
 * `aramStoreData`/`aramGetStreamBufferAddress` (musyx_aram.c): ADPCM stream
 * buffers (compType 4) and virtual-sample ring buffers (compType 5's
 * `vSampleInfo.loopBufferAddr`). Everything else -- every plain sequenced
 * note, i.e. most of the game's actual sound -- was being read as an offset
 * into the wrong 16 MB region, which is why the G4 run logged ~2000 refused
 * reads per 160-sample frame and 16,967/10,696 started/ended voices: reading
 * garbage past MEM1's own bounds routinely reads a sample "length" or "loop"
 * field's worth of noise into what should have been silence.
 *
 * The fix is not to copy sample data into ARAM the way the console had to --
 * a CPU mixer has no DSP-can-only-see-ARAM constraint, and 8 MB of copying
 * to satisfy an obsolete one would be pure waste. Instead, resolve the
 * address to a host pointer once, at voice start, by range:
 *
 *   - `[0, PORT_ARAM_SIZE)`: an ARAM byte offset (this port's own streaming
 *     paths, and, in principle, the console's habit of siting sample data
 *     below `HU_AMEM_BASE` -- see musyx_aram.c's banner). Resolves against
 *     `port_aram()`.
 *   - `[port_mem1_lo(), port_mem1_hi())`: a genuine MEM1 host pointer --
 *     the SDIR_DATA.addr case above. Resolves to itself.
 *   - Anything else is not a valid sample address at all and refuses the
 *     voice outright rather than guessing.
 *
 * These two windows are cleanly separable in practice: ARAM offsets used by
 * this port never exceed `PORT_ARAM_SIZE` (16 MB), while MEM1 sits at
 * `[0x2000000, 0x3800000)` on the G4 (confirmed in the same hardware log,
 * `OSInit: MEM1 24 MB [0x2000000, 0x3800000)`) and at whatever `mmap()` gave
 * `port_mem_init()` (port/src/os/os_arena.c) on the dev host -- neither of
 * which a small ARAM offset can collide with.
 *
 * Every decode read now goes through the RESOLVED per-voice pointer
 * (`MixVoice::readBase`/`readLen`), bound-checked against that buffer's own
 * length, not a blanket 16 MB. `MixVoice::addrBase` is kept *only* for the
 * `currentAddr` write-back math in writeback_current_addr(): hwGetPos()'s
 * inverse on the other end expects exactly the same units this mixer always
 * wrote there, so that formula is deliberately untouched -- only the actual
 * memory reads changed. */
typedef enum { SAMPLE_LOC_INVALID = 0, SAMPLE_LOC_ARAM, SAMPLE_LOC_MEM1 } SampleLoc;

static unsigned long stat_bad_sample_addr; /* addr in neither ARAM nor MEM1 */
static int mem1_dump_shown;

static int resolve_sample_ptr(void* addr, const u8** out_base, u32* out_len, SampleLoc* out_kind) {
    uintptr_t p = (uintptr_t)addr;
    uintptr_t mem1_lo = (uintptr_t)port_mem1_lo();
    uintptr_t mem1_hi = (uintptr_t)port_mem1_hi();

    if (p < PORT_ARAM_SIZE) {
        *out_base = (const u8*)port_aram() + p;
        *out_len = PORT_ARAM_SIZE - (u32)p;
        *out_kind = SAMPLE_LOC_ARAM;
        return 1;
    }
    if (p >= mem1_lo && p < mem1_hi) {
        *out_base = (const u8*)addr;
        *out_len = (u32)(mem1_hi - p);
        *out_kind = SAMPLE_LOC_MEM1;
        return 1;
    }
    *out_base = NULL;
    *out_len = 0;
    *out_kind = SAMPLE_LOC_INVALID;
    stat_bad_sample_addr++;
    return 0;
}

/* When a read is refused, the interesting thing is not the count -- it is
 * *which voice* asked and what its addressing looked like, because that is
 * the difference between "one bad sample in the bank" and "a whole format's
 * address arithmetic is in the wrong units". The first few are described in
 * full and the rest counted. */
static const MixVoice* aram_blame;
static const DSPvoice* aram_blame_dv;
static int aram_clamp_shown;

static void aram_clamp_report(u32 off);

static const char* sample_loc_name(SampleLoc k) {
    switch (k) {
    case SAMPLE_LOC_ARAM: return "ARAM";
    case SAMPLE_LOC_MEM1: return "MEM1";
    default: return "invalid";
    }
}

static u8 voice_read_u8(MixVoice* mv, u32 off) {
    if (off >= mv->readLen) {
        stat_aram_clamped++;
        aram_clamp_report(off);
        return 0;
    }
    return mv->readBase[off];
}

static s16 voice_read_s16be(MixVoice* mv, u32 off) {
    u8 hi, lo;
    if (off + 1 >= mv->readLen) {
        stat_aram_clamped++;
        aram_clamp_report(off);
        return 0;
    }
    /* PCM16 sample data is big-endian in memory on both the console and (per
     * the port's own convention, see port/src/gx/gx_draw.c's read_component)
     * the little-endian dev host; assemble the bytes explicitly so this is
     * correct either way rather than relying on host struct layout. */
    hi = mv->readBase[off];
    lo = mv->readBase[off + 1];
    return (s16)((hi << 8) | lo);
}

/* Name the first few refused reads in full.  `aram_blame` is set by
 * render_voice for the voice currently being decoded, so this can say which
 * format and which addressing produced the address, which is the whole
 * difference between one bad sample in the bank and a format whose address
 * arithmetic is in the wrong units. */
static void aram_clamp_report(u32 off) {
    const MixVoice* mv = aram_blame;
    if (aram_clamp_shown >= 8 || !mv) {
        return;
    }
    aram_clamp_shown++;
    port_log("port> musyx_mix: %s read refused at relative offset 0x%08x (buffer is %u "
             "bytes from its resolved base).  voice: compType %u, smp_info.addr %p, "
             "addrBase 0x%08x, curSample %u of length %u, loop %s [%u..%u], "
             "frameOffset %u, pitch 0x%05x, srcType %u\n",
             sample_loc_name((SampleLoc)mv->readKind), off, mv->readLen, mv->compType,
             aram_blame_dv ? aram_blame_dv->smp_info.addr : NULL, mv->addrBase, mv->curSample,
             mv->length, mv->looping ? "on" : "off", mv->loopStart, mv->loopEnd,
             mv->frameOffset, mv->pitch, mv->srcType);
    if (aram_clamp_shown == 8) {
        port_log("port> musyx_mix: (further refused reads are counted, not printed)\n");
    }
}

/* ---- small helpers --------------------------------------------------------- */

static s16 clamp_s16(s32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

static s32 clamp_accum(s64 v) {
    /* Studio bus accumulators are s32 with roughly 24-bit headroom on the
     * console (hw_dspctrl.c's host depop path saturates at +-0x7FFFFF); mix
     * math below stays well inside s32 range for any sane number of voices,
     * but clamp anyway since a runaway voice must not corrupt a shared bus. */
    if (v > 0x7fffff) return 0x7fffff;
    if (v < -0x7fffff) return -0x7fffff;
    return (s32)v;
}

/* gain is a u16 with 0x7FFF == unity (DSPvoice's volL/volR/... convention,
 * and the widened studio-input vol/volA/volB from salAddStudioInput). */
static s32 apply_gain(s32 sample, s32 gain) {
    return (s32)(((s64)sample * gain) >> 15);
}

/* The voice path's gain and accumulate, in 32 bits (M18, PLAN.md 33.4).
 *
 * A 64-bit multiply and a 64-bit compare are each half a dozen instructions
 * on a 32-bit PowerPC, and render_voice did five of the one and four of the
 * other per sample per voice; M17's profile put the mixer at 22% of a
 * consumed frame.  In the voice path every operand is bounded: the resampler
 * hands back an s16 (the linear blend cannot leave [hist1, hist2], the 4-tap
 * is clamped), the envelope is 0..0x8000, so |e| <= 32768; a bus volume is
 * `(s16)lastVol` plus at most 160 steps of a delta that was `(vol - last) /
 * 160`, so it stays within s16 -- |e * vol| <= 2^30 and the product fits an
 * s32, where the arithmetic right shift gives the very same bits the 64-bit
 * one did.  The bus holds at most +-0x7fffff and the addend at most 2^30, so
 * the sum fits an s32 too and the clamp is the same clamp.  Nothing in the
 * output can differ: port/tools/audio_ab.sh checks the .wav byte for byte.
 * The studio-input path keeps the 64-bit form -- its gain is a widened u16
 * and its sample a bus value. */
static unsigned long stat_mixcheck_samples, stat_mixcheck_bad;

/* bus + addend, clamped, in 32 bits; under --mixcheck compared with the
 * 64-bit sum and clamp */
static s32 mixcheck_acc(s32 bus, s32 add) {
    s32 r = bus + add;
    if (r > 0x7fffff) r = 0x7fffff;
    if (r < -0x7fffff) r = -0x7fffff;
    if (port_opt.mixcheck) {
        s64 v = (s64)bus + add;
        if (v > 0x7fffff) v = 0x7fffff;
        if (v < -0x7fffff) v = -0x7fffff;
        if ((s32)v != r) {
            stat_mixcheck_bad++;
        }
    }
    return r;
}

static s32 apply_gain32(s32 sample, s32 gain) {
    s32 r = (sample * gain) >> 15;
    if (port_opt.mixcheck) {
        /* --mixcheck: the 64-bit form alongside, every sample, and a count of
         * disagreements -- the proof of the bound above on the real data
         * rather than on paper */
        stat_mixcheck_samples++;
        if (r != (s32)(((s64)sample * gain) >> 15)) {
            stat_mixcheck_bad++;
        }
    }
    return r;
}

static s32 clamp_accum32(s32 v) {
    if (v > 0x7fffff) return 0x7fffff;
    if (v < -0x7fffff) return -0x7fffff;
    return v;
}

/* ---- ADSR-style linear volume ramp (sal_setup_dspvol, hw_dspctrl.c:597) ---
 *
 * The console ramps from lastVol* to vol* linearly over all 160 samples of
 * the frame: delta = (vol - lastVol) / 160 (16-bit signed divide, so it
 * truncates toward zero exactly like the original), then lastVol advances by
 * delta*160 -- which is *not* necessarily vol itself, matching hardware
 * (a residual is left for salCheckVolErrorAndResetDelta to chase, which this
 * mixer does not implement -- see the deferral note in the report).  This
 * returns the delta and leaves *last_vol at its new post-frame value; the
 * caller must remember the *pre*-update value as the ramp's starting point.
 */
static s16 setup_ramp(u16* last_vol, u16 vol) {
    s16 delta = (s16)(((s16)vol - (s16)*last_vol) / 160);
    *last_vol = (u16)(*last_vol + (s16)(delta * 160));
    return delta;
}

/* ---- ADPCM decode ----------------------------------------------------------
 *
 * Standard GameCube DSPADPCM: 8-byte frames, 14 samples each.  Byte 0 is the
 * header (predictor = ps>>4, scale = ps&0xF); bytes 1..7 hold the 14 signed
 * 4-bit nibbles, high nibble first.  There is no reference decoder anywhere
 * in extern/musyx (the DSP did this in hardware), so this is written from
 * the format's well-known definition plus the addressing arithmetic given in
 * hw_dspctrl.c:1030-1036.
 */
static s32 sign_extend4(u32 nibble) { return ((s32)(nibble << 28)) >> 28; }

/* ---- M24: the three instantiations of the frame (PLAN.md 39.2) ------------
 *
 * Every function of the render path below takes a `mode` that is a compile-
 * time constant at each call site: MIX_BOTH is the fused frame every run
 * before M24 ran (and what `--threads 0` runs); MIX_CTL is the control half
 * on the game thread -- the same per-sample loop with the value arithmetic
 * elided, so that the position of every voice at every sample is the fused
 * path's; MIX_VAL is the value half on the worker, on a copy of the
 * DSPvoice, with the two MusyX calls (the voice-done message, the
 * deactivation) and the heap walk compiled out.  GCC folds the constant
 * and each wrapper is the fused body with its other half dead. */
#define MIX_BOTH 0
#define MIX_CTL 1
#define MIX_VAL 2
#define MIX_INLINE __attribute__((always_inline)) static inline

/* Decode exactly the next ADPCM sample for `mv`, honouring loop wraparound,
 * and advance `mv->curSample`/`mv->frameOffset`.  `frame_byte` is the offset
 * of this 8-byte frame RELATIVE to `mv->readBase` (sample byte 0), which is
 * the same for every compType that reaches here -- 0/4/5 start at
 * frameNo==0, and compType 1's seek already lands `curSample` on a frame
 * boundary at voice-start (see start_voice()), so there is no separate base
 * to add here. Returns the decoded sample already normalised to the common
 * s16 scale (see the gain-scaling note in start_voice()).  In MIX_CTL the
 * value work is elided and only the position advances. */
MIX_INLINE s32 adpcm_decode_advance(int mode, DSPvoice* dv, MixVoice* mv) {
    s32 out = 0;

    if (mode != MIX_CTL) {
        u32 frame_no = mv->curSample / 14u;
        u32 frame_byte = frame_no * 8u;
        if (mv->frameOffset == 0) {
            u8 ps = voice_read_u8(mv, frame_byte);
            mv->predScale = ps;
        }
        {
            u8 predictor = (u8)(mv->predScale >> 4);
            u8 scale = (u8)(mv->predScale & 0xF);
            u32 data_byte_index = 1u + mv->frameOffset / 2u;
            u8 raw = voice_read_u8(mv, frame_byte + data_byte_index);
            u32 nibble = (mv->frameOffset & 1u) ? (raw & 0xF) : (raw >> 4);
            s32 s = sign_extend4(nibble);
            s32 c0 = mv->coefTab[predictor][0];
            s32 c1 = mv->coefTab[predictor][1];
            s64 acc = ((s64)s << scale << 11) + (s64)c0 * mv->yn1 + (s64)c1 * mv->yn2;
            out = clamp_s16((s32)((acc + 1024) >> 11));
            mv->yn2 = mv->yn1;
            mv->yn1 = out;
        }
    }

    mv->curSample++;
    mv->frameOffset++;
    if (mv->frameOffset == 14) {
        mv->frameOffset = 0;
    }

    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->frameOffset = (u32)(mv->loopStart % 14u);
        if (mode != MIX_CTL) {
            if (mv->loopType == 0) {
                mv->yn1 = mv->loopY1;
                mv->yn2 = mv->loopY0;
                mv->predScale = mv->loopPS;
            } else {
                mv->predScale = dv->streamLoopPS;
            }
        }
        mv->streamLoopCnt++;

        /* Virtual sample (compType 5) hand-off to the streaming ring buffer.
         *
         * hw_dspctrl.c:1190-1215 -- inside the same MUSY_TARGET_DOLPHIN-only
         * block as everything else in salBuildCommandList, so nobody does
         * this on the PC target unless this mixer does -- checks, once per
         * *frame*, whether a virtual-sample voice has ever wrapped
         * (`pb->streamLoopCnt != 0`) and has not yet switched
         * (`vSampleInfo.inLoopBuffer == 0`); if so it repoints the sample at
         * `vSampleInfo.loopBufferAddr` and recomputes the end address from
         * `loopBufferLength` alone (bn/bo computed from `loopBufferLength -
         * 1`, with no reference to the original `smp_info.loop`/`loopLength`
         * at all). `vsSampleStartNotify` (synth_vsamples.c) shows why: a
         * virtual sample is fed by a genuinely separate ring buffer that the
         * streaming layer DMAs fresh compressed data into ahead of the read
         * cursor (via hwGetPos -> our currentAddr write-back), allocated
         * independently of the voice's initial `smp_info.addr`/`loop`. The
         * initial `smp_info.loop`/`loopLength` region is a short seed loop
         * that plays while that ring buffer is primed; once it has been
         * played once in full (this wrap), the console commits to reading
         * the ring buffer from here on, whose own loop region is the *whole*
         * buffer (loop 0, length loopBufferLength) rather than the seed's.
         * We do the same, and reset curSample/frameOffset to 0 (the ring
         * buffer's own origin) rather than leaving them at the seed's
         * loopStart, since they no longer address the same memory. yn1/yn2
         * are left untouched (loopType 1's contract: only pred_scale is
         * restored on a wrap, from `streamLoopPS`, which the switch above
         * already did), so there is no click at the hand-off -- the decoder
         * simply keeps going, now reading a different but ADPCM-continuous
         * buffer. This only runs once per voice (`inLoopBuffer` latches).
         * (M24: a compType 5 voice keeps the tick on the fused path, so the
         * split halves never reach this arm; see port_musyx_mix_needs_inline.) */
        if (mv->compType == 5 && !dv->vSampleInfo.inLoopBuffer &&
            dv->vSampleInfo.loopBufferLength != 0) {
            SampleLoc kind;
            mv->addrBase = (u32)(uintptr_t)dv->vSampleInfo.loopBufferAddr;
            /* The ring buffer is a different address from the seed sample
             * (see the long comment above resolve_sample_ptr()), so the
             * resolved read pointer has to be redone here too -- not just
             * addrBase. It is allocated by this port's own
             * aramGetStreamBufferAddress (musyx_aram.c), which always hands
             * out an ARAM offset, so this should never fail in practice; if
             * it somehow does, refuse cleanly rather than read through a
             * stale (and by now wrong-length) pointer. */
            if (resolve_sample_ptr(dv->vSampleInfo.loopBufferAddr, &mv->readBase, &mv->readLen,
                                    &kind)) {
                mv->readKind = (u8)kind;
                mv->loopStart = 0;
                mv->loopEnd = dv->vSampleInfo.loopBufferLength - 1;
                mv->curSample = 0;
                mv->frameOffset = 0;
                dv->vSampleInfo.inLoopBuffer = 1;
            } else {
                mv->ended = 1;
            }
        }
    } else if (!mv->looping && mv->curSample >= mv->length) {
        mv->ended = 1;
    }

    return out;
}

/* PCM16 (big-endian in ARAM) and PCM8, normalised to the same s16 scale as
 * the ADPCM decoder above.
 *
 * Scale note (hw_dspctrl.c:984-1006): the real DSP keeps a wide internal
 * accumulator and gives each format its own `_PBADPCM.gain` shift to land in
 * it -- 0 for ADPCM (the decoder above already produces full-scale s16),
 * 0x800 for PCM16, 0x100 for PCM8.  That is an artifact of the *DSP's*
 * fixed-point headroom, not of the source material's loudness: a PCM16
 * sample is already s16 by definition, and an s8 PCM8 sample reaches the
 * same perceptual level once left-shifted by 8 (s16 = s8 * 256), which is
 * exactly the same relationship as "0x100 relative to 0x800" scaled down by
 * the common factor of 8.  This mixer targets a flat s16 scale for every
 * format instead of replicating the DSP's internal shift constants, since
 * the two are audibly equivalent and the flat scale is what every later
 * volume/envelope multiply below expects. */
MIX_INLINE s32 pcm16_decode_advance(int mode, MixVoice* mv) {
    s32 out = mode == MIX_CTL ? 0 : voice_read_s16be(mv, mv->curSample * 2u);
    mv->curSample++;
    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->streamLoopCnt++;
    } else if (!mv->looping && mv->curSample >= mv->length) {
        mv->ended = 1;
    }
    return out;
}

MIX_INLINE s32 pcm8_decode_advance(int mode, MixVoice* mv) {
    s32 out = 0;
    if (mode != MIX_CTL) {
        s8 raw = (s8)voice_read_u8(mv, mv->curSample);
        out = (s32)raw << 8;
    }
    mv->curSample++;
    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->streamLoopCnt++;
    } else if (!mv->looping && mv->curSample >= mv->length) {
        mv->ended = 1;
    }
    return out;
}

MIX_INLINE s32 voice_decode_advance(int mode, DSPvoice* dv, MixVoice* mv) {
    if (mv->ended) {
        return 0;
    }
    switch (mv->compType) {
    case 0:
    case 1:
    case 4:
    case 5:
        return adpcm_decode_advance(mode, dv, mv);
    case 2:
        return pcm16_decode_advance(mode, mv);
    case 3:
        return pcm8_decode_advance(mode, mv);
    default:
        mv->ended = 1;
        return 0;
    }
}

/* ---- the 4-tap polyphase resampler ----------------------------------------
 *
 * The console interpolated with a 4-tap polyphase filter, selected per voice
 * by `srcCoefSelect` (hwSetPolyPhaseFilter, hardware.c:301); `_PBSRC` keeps
 * `last_samples[4]` for exactly that.  **The coefficients themselves are not
 * recoverable from this tree**: `dsp_import.c` is the assembled `dspSlave[]`
 * ucode as hex and there is no symbolic table anywhere in `extern/musyx`.
 * They are not taken from an emulator either -- PLAN.md §5.1's rule is that
 * Dolphin is a reference runtime and never a source of code.
 *
 * So the port supplies a kernel of the same shape and the same cost: a
 * Catmull-Rom 4-point cubic, tabulated over 256 phases in Q14.  It is a
 * genuine 4-tap polyphase filter, it is continuous in its first derivative
 * across sample boundaries (which linear interpolation is not, and which is
 * the property that matters for the clicks), and it costs four 32x16
 * multiplies where the linear blend cost one 64-bit multiply and a shift.
 *
 * Q14 rather than Q15 for a bound rather than a hope: Catmull-Rom's
 * coefficients sum to 1 and their absolute values sum to at most 1.25 (at the
 * half phase, [-1,9,9,-1]/16), so the accumulator cannot exceed
 * 32768 * 1.25 * 16384 = 6.7e8 and stays inside s32 without a saturating add
 * in the inner loop.
 */
#define SRC_PHASES 256
#define SRC_Q 14
static s16 src_coef[SRC_PHASES][4];

static void src_table_init(void) {
    int p;
    for (p = 0; p < SRC_PHASES; p++) {
        double t = (double)p / (double)SRC_PHASES;
        double t2 = t * t, t3 = t2 * t;
        /* Catmull-Rom, in the y(-1), y(0), y(1), y(2) basis. */
        double c0 = -0.5 * t3 + t2 - 0.5 * t;
        double c1 = 1.5 * t3 - 2.5 * t2 + 1.0;
        double c2 = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
        double c3 = 0.5 * t3 - 0.5 * t2;
        src_coef[p][0] = (s16)(c0 * (1 << SRC_Q) + (c0 >= 0 ? 0.5 : -0.5));
        src_coef[p][1] = (s16)(c1 * (1 << SRC_Q) + (c1 >= 0 ? 0.5 : -0.5));
        src_coef[p][2] = (s16)(c2 * (1 << SRC_Q) + (c2 >= 0 ? 0.5 : -0.5));
        src_coef[p][3] = (s16)(c3 * (1 << SRC_Q) + (c3 >= 0 ? 0.5 : -0.5));
    }
}

/* One resampled output sample.
 *
 * srcTypeSelect == 2 ("no SRC") advances exactly one input sample per output
 * sample, per spec.  0 and 1 are the console's interpolating resamplers; the
 * 4-tap table above stands in for the DSP's polyphase filter, and
 * `--resample1` keeps the linear blend M6 shipped so the two can be measured
 * against each other on the same walk. */
/* `use4` is passed rather than read from `port_opt` here, and that is not
 * tidiness.  This function is inlined into `render_voice`'s per-sample loop,
 * the loop calls `voice_decode_advance`, and `port_opt` is a global struct --
 * so GCC has to assume the call might change it and reloads the flag, and
 * re-tests the branch, on every output sample of every voice.  Hoisting it to
 * a local in the caller is a load and a branch removed from the innermost loop
 * the port has, for no change in behaviour: it is a command-line flag and it
 * cannot change while a frame is being mixed. */
MIX_INLINE s32 voice_output_sample(int mode, DSPvoice* dv, MixVoice* mv, int use4) {
    s32 out = 0;

    if (mv->srcType == 2) {
        return voice_decode_advance(mode, dv, mv);
    }

    if (mode != MIX_CTL) {
        if (use4) {
            const s16* c = src_coef[(mv->phase & 0xFFFF) >> (16 - 8)];
            out = (mv->hist[0] * c[0] + mv->hist[1] * c[1] + mv->hist[2] * c[2] +
                   mv->hist[3] * c[3]) >> SRC_Q;
            if (out > 32767) out = 32767;
            else if (out < -32768) out = -32768;
        } else {
            out = mv->hist[1] + (s32)((((s64)(mv->hist[2] - mv->hist[1])) *
                                       (s64)(mv->phase & 0xFFFF)) >> 16);
        }
    }
    mv->phase += mv->pitch;
    while (mv->phase >= 0x10000u && !mv->ended) {
        mv->phase -= 0x10000u;
        if (mode != MIX_CTL) {
            mv->hist[0] = mv->hist[1];
            mv->hist[1] = mv->hist[2];
            mv->hist[2] = mv->hist[3];
            mv->hist[3] = voice_decode_advance(mode, dv, mv);
        } else {
            voice_decode_advance(mode, dv, mv);
        }
    }
    return out;
}

/* ---- voice start (state 1 -> 2) --------------------------------------------
 *
 * Mirrors the voice-start half of salBuildCommandList (hw_dspctrl.c:884-
 * 1149) -- the part of that function that decides how to *begin* reading a
 * sample -- but writes into `MixVoice` instead of `dsp_vptr->pb`, since `pb`
 * is not usable on this target (see the file header).  Returns 0 and leaves
 * the voice deactivated if it could not be started (mirrors every `continue`
 * in the original after a `salDeactivateVoice`).
 *
 * M24 splits it at the priming decodes: `start_voice_init` is the decision
 * and the MixVoice's setup (every MusyX call the start makes is here, so it
 * runs on the game thread only: MIX_BOTH and MIX_CTL), `start_voice_prime`
 * is the resampler window's three decodes and the dv writes that follow
 * (all three modes; in MIX_CTL the decodes advance the position only). */
/* M19 (PLAN.md 34.4): which HuMem block holds a MEM1 sample, and is it
 * allocated?  The header is src/game/memory.c's `struct memory_block`
 * (32 bytes: size, magic 0xa5 allocated / 0xcd free, flag, prev, next, num,
 * retaddr).  Read-only walk of the five heaps; NULL if no heap holds it. */
typedef struct PortMemBlock {
    s32 size;
    u8 magic;
    u8 flag;
    struct PortMemBlock* prev;
    struct PortMemBlock* next;
    u32 num;
    u32 retaddr;
} PortMemBlock;
void* HuMemHeapPtrGet(HeapID heap);
static const PortMemBlock* mem_block_of(const void* p, int* heap_out) {
    int h;
    for (h = 0; h < HEAP_MAX; h++) {
        const PortMemBlock* head = (const PortMemBlock*)HuMemHeapPtrGet((HeapID)h);
        const PortMemBlock* b = head;
        int guard = 0;
        if (!head) {
            continue;
        }
        do {
            if ((const u8*)p >= (const u8*)b && (const u8*)p < (const u8*)b + b->size) {
                *heap_out = h;
                return b;
            }
            b = b->next;
        } while (b && b != head && ++guard < 100000);
    }
    return NULL;
}
static unsigned long stat_voices_in_free_block;
static int ctl_poisoned; /* M24: a MEM1 voice started under the split halves */

static void mixtrace_start_note(unsigned vi, unsigned smp, const PortMemBlock* b, int heap) {
    if (mixtrace_f) {
        fprintf(mixtrace_f, "START voice %u sample %u in block %p size %d num %08x call %08x heap %d\n",
                vi, smp, (const void*)b, b->size, b->num, b->retaddr, heap);
    }
}

static int start_voice_init(DSPvoice* dv, MixVoice* mv, MixVoicePlan* plan) {
    SAMPLE_INFO* smp = &dv->smp_info;

    memset(mv, 0, sizeof(*mv));
    mv->compType = smp->compType;
    mv->length = smp->length;
    mv->looping = (smp->loopLength != 0);
    mv->loopStart = smp->loop;
    mv->loopEnd = smp->loop + smp->loopLength - 1u;
    mv->srcType = dv->srcTypeSelect;
    mv->pitch = dv->pitch[dv->singleOffset];
    if (mv->pitch == 0) {
        mv->pitch = 0x10000u; /* nothing has set pitch[] yet: assume unity */
    }

    if (smp->compType == 5) {
        dv->vSampleInfo.loopBufferLength = 0;
        dv->virtualSampleID = salSynthSendMessage(dv, 2);
        if (dv->vSampleInfo.loopBufferLength == 0) {
            salSynthSendMessage(dv, 1);
            salDeactivateVoice(dv);
            return 0;
        }
    }

    switch (smp->compType) {
    case 0:
    case 4:
    case 5: {
        SNDADPCMinfo* info = (SNDADPCMinfo*)smp->extraData;
        int i;
        /* `extraData` is the sample directory's per-sample ADPCM block, and
         * `dataGetSample` only fills it when the SDIR entry has one.  Every
         * ADPCM sample in this game's bank does, and none has ever been
         * missing on hardware -- but this runs on a machine that has to stay
         * up, and dereferencing it unchecked turns a malformed bank into a
         * segfault instead of a silent voice.  Refuse the voice instead. */
        if (!info) {
            stat_voices_no_extradata++;
            salDeactivateVoice(dv);
            return 0;
        }
        mv->addrBase = (u32)(uintptr_t)smp->addr;
        mv->yn1 = 0;
        mv->yn2 = 0;
        mv->predScale = info->initialPS;
        for (i = 0; i < 8; i++) {
            mv->coefTab[i][0] = info->coefTab[i][0];
            mv->coefTab[i][1] = info->coefTab[i][1];
        }
        mv->curSample = 0;
        mv->frameOffset = 0;
        if (smp->compType == 4 || smp->compType == 5) {
            mv->loopType = 1;
        } else {
            mv->loopType = 0;
            mv->loopY0 = info->loopY0;
            mv->loopY1 = info->loopY1;
            mv->loopPS = info->loopPS;
        }
        dv->playInfo.posHi = 0;
        break;
    }
    case 1: {
        DSPADPCMplusInfo* info = (DSPADPCMplusInfo*)smp->extraData;
        if (!info) { /* see the compType 0/4/5 arm above */
            stat_voices_no_extradata++;
            salDeactivateVoice(dv);
            return 0;
        }
        u32 start_frame = (smp->offset + 13u) / 14u;
        int i;
        mv->addrBase = (u32)(uintptr_t)smp->addr;
        mv->yn2 = info->blk[start_frame].Y0;
        mv->yn1 = info->blk[start_frame].Y1;
        mv->predScale = info->blk[start_frame].PS;
        mv->loopType = 0;
        mv->loopY0 = info->loopY0;
        mv->loopY1 = info->loopY1;
        mv->loopPS = info->loopPS;
        for (i = 0; i < 8; i++) {
            mv->coefTab[i][0] = info->coefTab[i][0];
            mv->coefTab[i][1] = info->coefTab[i][1];
        }
        mv->curSample = start_frame * 14u;
        mv->frameOffset = 0;
        dv->playInfo.posHi = mv->curSample;
        break;
    }
    case 2:
        mv->addrBase = (u32)(uintptr_t)smp->addr; /* byte address, 2 bytes/sample */
        mv->curSample = smp->offset;
        dv->playInfo.posHi = smp->offset;
        break;
    case 3:
        mv->addrBase = (u32)(uintptr_t)smp->addr; /* byte address, 1 byte/sample */
        mv->curSample = smp->offset;
        dv->playInfo.posHi = smp->offset;
        break;
    default:
        /* compType 6 (MUSY_VERSION >= 2.0.2 PCM16 virtual sample) is not
         * present in this tree's musyx/version.h gate; anything else is a
         * malformed bank. Deactivate rather than guess. */
        salSynthSendMessage(dv, 0);
        salDeactivateVoice(dv);
        return 0;
    }

    /* Resolve the sample address to a host pointer once, here -- see the
     * long comment above resolve_sample_ptr() for why this can no longer
     * assume every smp_info.addr is an ARAM offset. addrBase (set in every
     * arm above from the same smp->addr) stays as the currentAddr
     * write-back needs it; readBase/readLen are what actual reads use. */
    {
        SampleLoc kind;
        if (!resolve_sample_ptr(smp->addr, &mv->readBase, &mv->readLen, &kind)) {
            port_log("port> musyx_mix: sample address %p is in neither ARAM "
                     "[0, 0x%x) nor MEM1 [%p, %p) -- refusing voice (compType %u)\n",
                     smp->addr, PORT_ARAM_SIZE, port_mem1_lo(), port_mem1_hi(),
                     (unsigned)smp->compType);
            salSynthSendMessage(dv, 0);
            salDeactivateVoice(dv);
            return 0;
        }
        mv->readKind = (u8)kind;
        if (kind == SAMPLE_LOC_MEM1) {
            int heap = -1;
            const PortMemBlock* b = mem_block_of(mv->readBase, &heap);
            /* M24: a MEM1 sample can be freed or rewritten by the game
             * during its frame, which is when the value half would read
             * it.  The job so far is finished on the game thread at once
             * and the tick runs fused from here (port_musyx_mix_needs_inline
             * sees the live voice's readKind). */
            if (plan) {
                ctl_poisoned = 1;
            }
            if (!b || b->magic != 0xa5 || !b->flag) {
                stat_voices_in_free_block++;
                if (stat_voices_in_free_block <= 12) {
                    port_log("port> musyx_mix: SAMPLE IN FREED MEMORY: voice %u sample %u "
                             "(compType %u, %p, %u samples) starts in %s at DSP frame %lu%s\n",
                             (unsigned)(dv - dspVoice), (unsigned)dv->smp_id, mv->compType,
                             (const void*)mv->readBase, mv->length,
                             b ? "a FREE block" : "no heap block", stat_frames_mixed,
                             b ? "" : "");
                    if (b) {
                        port_log("port> musyx_mix:   block %p size %d magic %02x flag %u num %08x "
                                 "last call %08x, heap %d\n",
                                 (const void*)b, b->size, b->magic, b->flag, b->num, b->retaddr,
                                 heap);
                    }
                }
            } else if (plan) {
                /* the value half prints the note where the fused path does */
                plan->has_note = 1;
                plan->note_block = b;
                plan->note_size = b->size;
                plan->note_num = b->num;
                plan->note_call = b->retaddr;
                plan->note_heap = heap;
            } else {
                mixtrace_start_note((unsigned)(dv - dspVoice), (unsigned)dv->smp_id, b, heap);
            }
        }
        /* One-shot: what is actually AT the resolved address?  A valid
         * DSPADPCM frame's header byte has predictor 0..7 in its high nibble,
         * so a run of high nibbles above 7 says the pointer is wrong rather
         * than the gain.  Printed for the first few MEM1-resolved voices
         * only, which is the case under suspicion. */
        if (kind == SAMPLE_LOC_MEM1 && mem1_dump_shown < 4) {
            const u8* q = mv->readBase;
            int k;
            mem1_dump_shown++;
            port_log("port> musyx_mix: MEM1 sample id %u compType %u at %p len %u:",
                     (unsigned)dv->smp_id, (unsigned)mv->compType, (const void*)q,
                     mv->readLen);
            for (k = 0; k < 16; k++) {
                port_log(" %02x", q[k]);
            }
            port_log("\n");
        }
    }

    dv->playInfo.posLo = 0;
    dv->playInfo.pitch = mv->pitch;

    if (mv->looping) {
        /* one-shot end check is meaningless for a looping sample */
    } else if (mv->curSample >= mv->length) {
        salSynthSendMessage(dv, 0);
        salDeactivateVoice(dv);
        return 0;
    }

    if (adsrSetup(&dv->adsr) != 0) {
        /* VoiceDone immediately, e.g. a zero-length envelope */
        salSynthSendMessage(dv, 0);
        salDeactivateVoice(dv);
        return 0;
    }
    return 1;
}

/* The second half of the start: the resampler window and the state flip.
 * The value half runs this on its copy of the DSPvoice, taken at the
 * frame's entry -- before the init above set the envelope up and wrote the
 * play-info into the real one -- so it redoes those two writes on the copy
 * first: adsrSetup is a function of the ADSR struct alone (synth_adsr.c)
 * and returned 0 for a voice that got this far, and posHi is the start
 * sample in every format (0 from the top, the seek frame, the offset). */
MIX_INLINE void start_voice_prime(int mode, DSPvoice* dv, MixVoice* mv) {
    if (mode == MIX_VAL) {
        adsrSetup(&dv->adsr);
        dv->playInfo.posHi = mv->curSample;
        dv->playInfo.posLo = 0;
        dv->playInfo.pitch = mv->pitch;
    }
    /* Resampler window.  The DSP zeroes `last_samples[]` at voice start
     * (hw_dspctrl.c:916-920) rather than back-filling with the first sample,
     * so hist[0] stays 0 and the filter eases in from silence -- which is the
     * *start* of the same anti-step argument the depop path makes at the end.
     * hist[1] is the first decoded sample, and hist[2..3] the lookahead the
     * 4-tap kernel reads ahead of the output position. */
    mv->hist[0] = 0;
    if (mode != MIX_CTL) {
        mv->hist[1] = voice_decode_advance(mode, dv, mv);
        mv->hist[2] = mv->ended ? mv->hist[1] : voice_decode_advance(mode, dv, mv);
        mv->hist[3] = mv->ended ? mv->hist[2] : voice_decode_advance(mode, dv, mv);
    } else {
        voice_decode_advance(mode, dv, mv);
        if (!mv->ended) voice_decode_advance(mode, dv, mv);
        if (!mv->ended) voice_decode_advance(mode, dv, mv);
    }
    mv->phase = 0;

    mv->live = 1;
    dv->state = 2;
    if (mode != MIX_VAL) {
        stat_voices_started++;
    }
}

/* Finish a voice this frame: notify the sequencer and unlink it from its
 * studio's voice list (hw_dspctrl.c:1224/1600/1657 all do exactly this pair
 * when a one-shot runs out or adsrHandle reports VoiceDone). */
MIX_INLINE void finish_voice(int mode, DSPvoice* dv, MixVoice* mv) {
    if (mode != MIX_VAL) {
        salSynthSendMessage(dv, 0);
        salDeactivateVoice(dv);
        stat_voices_ended++;
    }
    mv->live = 0;
}

/* ---- per-sub-frame `changed[]` handling ------------------------------------
 *
 * hw_dspctrl.c tests these bits while building the command list; the ones
 * handled here are the ones the spec calls out as cheap and load-bearing.
 * Anything else (0x100 srcType, 0x1 volume-changed as a *gate* rather than
 * an unconditional recompute, filter/ITD bits, ...) is read every frame
 * regardless below rather than gated on `changed[]`, which is simpler and
 * strictly a superset of the work the console did -- see the report. */
static void apply_subframe_changes(DSPvoice* dv, MixVoice* mv, u32 s) {
    u32 chg = dv->changed[s];

    if (chg & 0x20) { /* hwBreak: fast fade so the cut does not click */
        adsrStartRelease(&dv->adsr, 10);
        dv->postBreak = 1;
        mv->postBreak = 1;
    }
    if (chg & 0x40) { /* hwKeyOff: normal release */
        adsrRelease(&dv->adsr);
    }
    if (chg & 0x08) { /* pitch changed at this sub-frame */
        mv->pitch = dv->pitch[s];
        dv->playInfo.pitch = mv->pitch;
    }
    if (chg & 0x10) { /* ADSR (re)assigned */
        adsrSetup(&dv->adsr);
    }
    if (chg & 0x100) { /* srcTypeSelect changed */
        mv->srcType = dv->srcTypeSelect;
    }
}

/* sal_update_hostplayinfo (hw_dspctrl.c:602-624), replicated for one-shots
 * only (loopLength == 0) since that guard is in the original too. Called
 * once per sub-frame, i.e. once per 32 samples, matching the cadence the
 * console ran it at (it is folded into the same per-sub-frame command-list
 * pass). `playInfo.pitch` is set from the live pitch value in
 * apply_subframe_changes()/start_voice(); the exact scale relationship
 * between it and the DSP's own `pb->src.ratioHi/Lo` was not recoverable from
 * this tree (the whole function is dead code on this target), so this uses
 * the pseudocode given in the M6 spec verbatim rather than guessing at a
 * different scale factor. It is host-side bookkeeping only -- nothing in
 * this mixer reads posHi/posLo back -- so an off-by-a-shift here would not
 * affect audio, only whatever the game itself does with the field. */
static void update_hostplayinfo(DSPvoice* dv) {
    u32 pitch;
    u32 old_lo;

    if (dv->smp_info.loopLength != 0) {
        return;
    }
    pitch = (dv->srcTypeSelect != 2) ? (dv->playInfo.pitch << 5) : 0x200000u;
    old_lo = dv->playInfo.posLo;
    dv->playInfo.posLo += pitch * 0x10000u;
    dv->playInfo.posHi += (pitch >> 16) + (old_lo > dv->playInfo.posLo ? 1u : 0u);
}

/* Write dv->currentAddr back in the exact units hwGetPos()'s inverse expects
 * (hw_dspctrl.c:456-486 / hardware.c's hwGetPos): a nibble offset for ADPCM,
 * a byte offset for PCM8, a 16-bit-sample offset for PCM16. This is the
 * single most important write-back -- stream.c's refill logic depends on
 * reading the right position back out of it. */
static void writeback_current_addr(DSPvoice* dv, const MixVoice* mv) {
    switch (mv->compType) {
    case 0:
    case 1:
    case 4:
    case 5: {
        u32 frame_no = mv->curSample / 14u;
        u32 frame_off = mv->curSample - frame_no * 14u;
        dv->currentAddr = mv->addrBase * 2u + frame_no * 16u + 2u + frame_off;
        break;
    }
    case 3:
        dv->currentAddr = mv->addrBase + mv->curSample;
        break;
    case 2:
        dv->currentAddr = mv->addrBase / 2u + mv->curSample;
        break;
    default:
        break;
    }
}

/* M25 (PLAN.md 40.1): what the control half last decided for each voice,
 * for the mismatch line -- the soak's 14 mismatches in 254,573 joins were
 * all "control all zeros, value live", and the line below did not say what
 * the control half had done with the slot in that tick. */
static unsigned long stat_voices_refused; /* M25: starts start_voice_init refused */
static u8 diag_last_action[64];
static u8 diag_last_state[64];
static u8 diag_last_refused[64];
static unsigned long diag_last_frame[64];
unsigned long port_audio_flushes(void);

/* ---- one voice, one 160-sample frame --------------------------------------
 *
 * Renders `dv` into its studio's main[]/auxA[]/auxB[] buses (already zeroed
 * by the caller for this frame) and updates every host-visible field this
 * mixer is responsible for. `next_out` receives dv->next captured *before*
 * any deactivation, since salDeactivateVoice() unlinks the voice from the
 * list the caller is walking.
 *
 * M24: `plan` is the voice's entry in the frame plan -- written in MIX_CTL
 * (the action and, for a start, the MixVoice as initialised), read in
 * MIX_VAL (where `dv` is the plan's copy), NULL in MIX_BOTH. */
MIX_INLINE void render_voice(int mode, DSPvoice* dv, MixVoice* mv, DSPstudioinfo* stp,
                             u8 sal_frame, u8 sal_aux_frame, MixVoicePlan* plan) {
    /* Read once per voice per frame, not once per sample: see
     * voice_output_sample. */
    const int use_src4 = port_opt.resample4;
    s32* main_buf = stp->main[sal_frame];
    s32* auxa_buf = stp->auxA[sal_aux_frame];
    s32* auxb_buf = stp->auxB[sal_aux_frame];

    s16 dL, dR, dS, dLa, dRa, dSa, dLb, dRb, dSb;
    s32 volL, volR, volS, volLa, volRa, volSa, volLb, volRb, volSb;
    u32 s, i;
    int done = 0;
    int main_live, auxa_live, auxb_live, surround_live;
    /* The nine per-bus values this voice last put into the mix.  If it stops
     * before the end of the frame, these are exactly the step it leaves
     * behind, and they are what the depop path has to unwind -- the console
     * calls them `_PB.dpop.a*` (musyx/include/musyx/voice.h:51) and folds
     * them into the studio's `hostDPopSum` in HandleDepopVoice
     * (hw_dspctrl.c:644). */
    s32 dpop_l = 0, dpop_r = 0, dpop_s = 0;
    s32 dpop_la = 0, dpop_ra = 0, dpop_sa = 0;
    s32 dpop_lb = 0, dpop_rb = 0, dpop_sb = 0;
    u32 last_idx = 0;

    if (mode == MIX_VAL) {
        if (plan->action == MIX_PLAN_SKIP) {
            return;
        }
        if (plan->action == MIX_PLAN_CLEAR) {
            /* M25: the control half refused this slot's start and zeroed its
             * entry; the same bytes here, so the join is exact */
            memset(mv, 0, sizeof(*mv));
            return;
        }
        if (plan->action == MIX_PLAN_START) {
            memcpy(mv, plan->mv, sizeof(*mv));
            if (plan->has_note) {
                mixtrace_start_note(plan->vi, (unsigned)dv->smp_id,
                                    (const PortMemBlock*)plan->note_block, plan->note_heap);
            }
            start_voice_prime(mode, dv, mv);
        }
    } else {
        if (dv->state == 1) {
            if (!start_voice_init(dv, mv, plan)) {
                /* M24: a refused start used to leave its half-initialised
                 * MixVoice behind (live 0; nothing reads it); cleared, so the
                 * fused table and the worker's are the same bytes and the
                 * traces compare */
                memset(mv, 0, sizeof(*mv));
                if (mode == MIX_CTL) {
                    plan->action = MIX_PLAN_CLEAR; /* M25: not SKIP -- see musyx_mix.h */
                    stat_voices_refused++;
                    diag_last_refused[plan->vi] = 1;
                    diag_last_action[plan->vi] = 3;
                    diag_last_frame[plan->vi] = stat_frames_mixed;
                }
                return;
            }
            if (mode == MIX_CTL) {
                plan->action = MIX_PLAN_START;
                memcpy(plan->mv, mv, sizeof(*mv));
            }
            start_voice_prime(mode, dv, mv);
        } else if (mode == MIX_CTL) {
            plan->action = MIX_PLAN_RUN;
        }
        if (dv->state != 2 || !mv->live) {
            if (mode == MIX_CTL) {
                plan->action = MIX_PLAN_SKIP;
                diag_last_action[plan->vi] = 4 + (plan->action == MIX_PLAN_START);
                diag_last_state[plan->vi] = (u8)dv->state;
                diag_last_frame[plan->vi] = stat_frames_mixed;
            }
            return;
        }
        if (mode == MIX_CTL) {
            diag_last_action[plan->vi] = (u8)plan->action;
            diag_last_state[plan->vi] = (u8)dv->state;
            diag_last_refused[plan->vi] = 0;
            diag_last_frame[plan->vi] = stat_frames_mixed;
        }
    }
    if (mode != MIX_CTL) {
        stat_voices_active_this_frame++;
        aram_blame = mv;
        aram_blame_dv = dv;
    }

    /* Cheap "is this bus worth touching" gate, reproducing the console's own
     * mixerCtrl policy (hw_dspctrl.c:1131 for L/R, 1157-1166/1537-1544 for
     * aux and surround: each is an OR of that bus's volumes -- if a bus's
     * target *and* its current ramp position are both silent, the ramp stays
     * at zero for the whole frame and every sample of that bus's gain+clamp
     * work is provably a no-op). Read before setup_ramp() below, which
     * mutates lastVol* in place. A voice whose main/auxA/auxB are *all* dead
     * this way has nothing left to do per sample but decode -- which still
     * has to happen for compType 4/5 (streams / virtual samples), since
     * hwGetPos()/currentAddr drive the ring-buffer refill regardless of
     * whether anything is audible right now. */
    main_live = dv->volL || dv->volR || dv->volS || dv->lastVolL || dv->lastVolR || dv->lastVolS;
    auxa_live = dv->volLa || dv->volRa || dv->volSa || dv->lastVolLa || dv->lastVolRa || dv->lastVolSa;
    auxb_live = dv->volLb || dv->volRb || dv->volSb || dv->lastVolLb || dv->lastVolRb || dv->lastVolSb;
    surround_live = dv->volS || dv->volSa || dv->volSb || dv->lastVolS || dv->lastVolSa || dv->lastVolSb;

    /* Volume ramps: capture the pre-update lastVol* as the ramp's starting
     * point (sal_setup_dspvol advances lastVol* to its post-frame value as a
     * side effect), then walk each bus from there over 160 samples. */
    volL = (s16)dv->lastVolL;   dL = setup_ramp(&dv->lastVolL, dv->volL);
    volR = (s16)dv->lastVolR;   dR = setup_ramp(&dv->lastVolR, dv->volR);
    volS = (s16)dv->lastVolS;   dS = setup_ramp(&dv->lastVolS, dv->volS);
    volLa = (s16)dv->lastVolLa; dLa = setup_ramp(&dv->lastVolLa, dv->volLa);
    volRa = (s16)dv->lastVolRa; dRa = setup_ramp(&dv->lastVolRa, dv->volRa);
    volSa = (s16)dv->lastVolSa; dSa = setup_ramp(&dv->lastVolSa, dv->volSa);
    volLb = (s16)dv->lastVolLb; dLb = setup_ramp(&dv->lastVolLb, dv->volLb);
    volRb = (s16)dv->lastVolRb; dRb = setup_ramp(&dv->lastVolRb, dv->volRb);
    volSb = (s16)dv->lastVolSb; dSb = setup_ramp(&dv->lastVolSb, dv->volSb);

    for (s = 0; s < NUM_SUBFRAMES && !done; s++) {
        u16 env_start = 0, env_delta = 0;
        s32 env, env_step;
        u32 voice_done;

        apply_subframe_changes(dv, mv, s);
        voice_done = adsrHandle(&dv->adsr, &env_start, &env_delta);
        update_hostplayinfo(dv);
        /* Both of these casts are load-bearing, and getting either wrong is
         * loud.  `adsrHandle` (synth_adsr.c:153) hands back a *volume* and a
         * *delta* through two `u16*`, and they are not the same kind of
         * number:
         *
         *   *adsr_start = old_volume >> 16;          -- 0..0x8000, unsigned.
         *                                               0x8000 is the value a
         *                                               voice starts at
         *                                               (hw_dspctrl.c:895), so
         *                                               reading it as s16
         *                                               turns unity gain into
         *                                               *minus* unity.
         *   *adsr_delta = -(-currentDelta >> 21);    -- SIGNED, stuffed into a
         *                                               u16.  Every release
         *                                               ramp is negative.
         *
         * Adding the delta unsigned is what pinned the whole mix at full
         * scale on the first board run: a release of -3 arrives as 65533, so
         * a voice fading out instead ramps its gain up by 65533 per sample
         * and clips everything else out of the mix with it. */
        env = (s32)(u16)env_start;
        env_step = (s32)(s16)env_delta;

        for (i = 0; i < SUBFRAME_SAMPLES; i++) {
            u32 idx = s * SUBFRAME_SAMPLES + i;
            s32 raw;

            if (mode == MIX_CTL) {
                /* the position only: the value half redoes this loop with
                 * the arithmetic, from the same state */
                if (!mv->ended) {
                    voice_output_sample(mode, dv, mv, use_src4);
                }
                if (mv->ended) {
                    done = 1;
                    break;
                }
                continue;
            }

            if (mv->ended || port_musyx_mix_mute) {
                raw = 0;
            } else {
                raw = voice_output_sample(mode, dv, mv, use_src4);
            }

            /* total gain per bus = envelope volume x bus volume, both
             * ramping; each is a 0x7FFF-unity u16-scale multiply. Only the
             * buses (main_live/auxa_live/auxb_live) and channel
             * (surround_live) that could possibly be non-zero this frame are
             * touched -- see the flags computed above render_voice's ramp
             * setup. */
            if (raw != 0 && (main_live || auxa_live || auxb_live)) {
                s32 e = apply_gain32(raw, env);
                last_idx = idx;
                dpop_l = dpop_r = dpop_s = 0;
                dpop_la = dpop_ra = dpop_sa = 0;
                dpop_lb = dpop_rb = dpop_sb = 0;
                if (main_live) {
                    dpop_l = apply_gain32(e, volL);
                    dpop_r = apply_gain32(e, volR);
                    main_buf[BUS_L_OFF + idx] = mixcheck_acc(main_buf[BUS_L_OFF + idx], dpop_l);
                    main_buf[BUS_R_OFF + idx] = mixcheck_acc(main_buf[BUS_R_OFF + idx], dpop_r);
                    if (surround_live) {
                        dpop_s = apply_gain32(e, volS);
                        main_buf[BUS_S_OFF + idx] = mixcheck_acc(main_buf[BUS_S_OFF + idx], dpop_s);
                    }
                }
                if (auxa_live) {
                    dpop_la = apply_gain32(e, volLa);
                    dpop_ra = apply_gain32(e, volRa);
                    auxa_buf[BUS_L_OFF + idx] = mixcheck_acc(auxa_buf[BUS_L_OFF + idx], dpop_la);
                    auxa_buf[BUS_R_OFF + idx] = mixcheck_acc(auxa_buf[BUS_R_OFF + idx], dpop_ra);
                    if (surround_live) {
                        dpop_sa = apply_gain32(e, volSa);
                        auxa_buf[BUS_S_OFF + idx] = mixcheck_acc(auxa_buf[BUS_S_OFF + idx], dpop_sa);
                    }
                }
                if (auxb_live) {
                    dpop_lb = apply_gain32(e, volLb);
                    dpop_rb = apply_gain32(e, volRb);
                    auxb_buf[BUS_L_OFF + idx] = mixcheck_acc(auxb_buf[BUS_L_OFF + idx], dpop_lb);
                    auxb_buf[BUS_R_OFF + idx] = mixcheck_acc(auxb_buf[BUS_R_OFF + idx], dpop_rb);
                    if (surround_live) {
                        dpop_sb = apply_gain32(e, volSb);
                        auxb_buf[BUS_S_OFF + idx] = mixcheck_acc(auxb_buf[BUS_S_OFF + idx], dpop_sb);
                    }
                }
            }

            env += env_step;
            /* A release can walk the envelope past zero inside a sub-frame;
             * the console's own multiplier saturates rather than wrapping
             * into a loud positive gain. */
            if (env < 0) {
                env = 0;
            } else if (env > 0x8000) {
                env = 0x8000;
            }
            volL += dL; volR += dR; volS += dS;
            volLa += dLa; volRa += dRa; volSa += dSa;
            volLb += dLb; volRb += dRb; volSb += dSb;

            if (mv->ended) {
                done = 1;
                break;
            }
        }

        if (voice_done) {
            done = 1;
            break;
        }
    }

    writeback_current_addr(dv, mv);

    /* Either the sample ran past its (non-looping) length, or adsrHandle
     * decided the envelope itself is over (a normal release or the fast
     * postBreak fade) -- both are the same lifecycle event from here on:
     * notify the sequencer and unlink the voice (hw_dspctrl.c:1224/1600/
     * 1657 all pair these two calls the same way). */
    if ((mv->ended || done) && mv->live) {
        /* The step, banked.  A voice that ran to the last sample of the frame
         * and *then* ended leaves nothing behind for this frame -- the next
         * frame simply has one fewer voice, and the difference between the
         * last sample of this frame and the first of the next is the step.
         * So the value is banked whatever `last_idx` was; what
         * `stat_depop_cuts` counts is the mid-frame case, which is the loud
         * one and the one PLAN.md §16.7 localised. */
        if (mode != MIX_CTL && port_opt.depop) {
            add_dpop(&stp->hostDPopSum.l, dpop_l);
            add_dpop(&stp->hostDPopSum.r, dpop_r);
            add_dpop(&stp->hostDPopSum.s, dpop_s);
            add_dpop(&stp->hostDPopSum.lA, dpop_la);
            add_dpop(&stp->hostDPopSum.rA, dpop_ra);
            add_dpop(&stp->hostDPopSum.sA, dpop_sa);
            add_dpop(&stp->hostDPopSum.lB, dpop_lb);
            add_dpop(&stp->hostDPopSum.rB, dpop_rb);
            add_dpop(&stp->hostDPopSum.sB, dpop_sb);
            if (last_idx + 1 < BUS_LEN / 3) {
                stat_depop_cuts++;
            }
        }
        finish_voice(mode, dv, mv);
    }
}


/* ---- the depop path (hw_dspctrl.c:631-705, 1880-1888) ----------------------
 *
 * A voice that stops does not stop at zero.  It stops at whatever its last
 * output sample times its bus gain happened to be, and every later sample of
 * that frame is missing that value -- a step, which is a click.  The DSP's
 * answer is not to fade the voice (there is no time: the decision is made
 * between frames) but to inject the step back into the bus as a DC offset and
 * then ramp *that* to zero, which spreads one discontinuity of arbitrary size
 * over 160 samples of at most 20 units each.
 *
 * `AddDpop` (hw_dspctrl.c:626) is the accumulator, `DoDepopFade`
 * (hw_dspctrl.c:631) the ramp, and `DSPstudioinfo::hostDPopSum` -- a field
 * this port already has, and never wrote until now -- is where the two meet.
 */
static void add_dpop(s32* sum, s32 delta) {
    s32 v = *sum + delta;
    if (v > 0x7fffff) v = 0x7fffff;
    if (v < -0x7fffff) v = -0x7fffff;
    *sum = v;
}

static void depop_bus(s32* bus, u32 off, s32* sum) {
    s32 start = *sum;
    s32 delta;
    u32 i;
    if (start == 0) {
        return;
    }
    if (start <= -160) {
        delta = (start <= -3200) ? 0x14 : (-start / 160);
    } else if (start >= 160) {
        delta = (start >= 3200) ? -0x14 : (-start / 160);
    } else {
        /* Below 160 the console's own arithmetic gives a delta of zero and
         * leaves the offset in place forever.  A permanent DC of under 160
         * units is inaudible but it is also pointless, and it would make two
         * runs of the same seed differ in their *accumulated* residue rather
         * than in anything audible, so the port retires it in one frame. */
        delta = 0;
    }
    for (i = 0; i < FRAME_SAMPLES; i++) {
        bus[off + i] = clamp_accum((s64)bus[off + i] + start + (s32)i * delta);
    }
    *sum = (delta == 0) ? 0 : start + delta * (s32)FRAME_SAMPLES;
}

static void apply_depop(const MixFramePlan* fp) {
    u8 st;
    if (!port_opt.depop) {
        return;
    }
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        s32* mb;
        s32* aa;
        s32* ab;
        if (fp->st[st].state != 1) {
            continue;
        }
        mb = stp->main[fp->salFrame];
        aa = stp->auxA[fp->salAuxFrame];
        ab = stp->auxB[fp->salAuxFrame];
        depop_bus(mb, BUS_L_OFF, &stp->hostDPopSum.l);
        depop_bus(mb, BUS_R_OFF, &stp->hostDPopSum.r);
        depop_bus(mb, BUS_S_OFF, &stp->hostDPopSum.s);
        depop_bus(aa, BUS_L_OFF, &stp->hostDPopSum.lA);
        depop_bus(aa, BUS_R_OFF, &stp->hostDPopSum.rA);
        depop_bus(aa, BUS_S_OFF, &stp->hostDPopSum.sA);
        depop_bus(ab, BUS_L_OFF, &stp->hostDPopSum.lB);
        depop_bus(ab, BUS_R_OFF, &stp->hostDPopSum.rB);
        depop_bus(ab, BUS_S_OFF, &stp->hostDPopSum.sB);
    }
}

/* ---- studio-level passes ---------------------------------------------------- */
/* M24: every pass reads the studios' state, type, master flag and input
 * list from the frame plan -- captured at the frame's entry on the game
 * thread (capture_studios), which in the fused frame is the same instant
 * and the same values as reading dspStudio[] directly, and on the worker
 * is the only way to read them at all.  The bus buffers and the depop sums
 * are dspStudio's own: nothing but the mixer touches them. */

static void capture_studios(MixFramePlan* fp) {
    u8 st;
    fp->salFrame = salFrame;
    fp->salAuxFrame = salAuxFrame;
    for (st = 0; st < salMaxStudioNum && st < MIX_MAX_STUDIOS; st++) {
        const DSPstudioinfo* stp = &dspStudio[st];
        MixStudioPlan* sp = &fp->st[st];
        sp->state = stp->state;
        sp->isMaster = stp->isMaster;
        sp->numInputs = stp->numInputs;
        sp->type = (u32)stp->type;
        memcpy(sp->in, stp->in, sizeof(sp->in));
    }
}

static void zero_buses(const MixFramePlan* fp) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        if (fp->st[st].state != 1) {
            continue;
        }
        memset(stp->main[fp->salFrame], 0, BUS_LEN * sizeof(s32));
        memset(stp->auxA[fp->salAuxFrame], 0, BUS_LEN * sizeof(s32));
        memset(stp->auxB[fp->salAuxFrame], 0, BUS_LEN * sizeof(s32));
    }
}

/* Studio input chaining: each studio's `in[]` reads *last* frame's main[] of
 * its source studio (main[salFrame ^ 1]) so that studio chains do not need a
 * particular activation order within a frame (hw_dspctrl.c:867-872). */
static void mix_studio_inputs(const MixFramePlan* fp) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        u8 in;
        if (fp->st[st].state != 1) {
            continue;
        }
        for (in = 0; in < fp->st[st].numInputs; in++) {
            const DSPinput* di = &fp->st[st].in[in];
            DSPstudioinfo* src = &dspStudio[di->studio];
            s32* src_main = src->main[fp->salFrame ^ 1];
            s32* dst_main = stp->main[fp->salFrame];
            s32* dst_auxa = stp->auxA[fp->salAuxFrame];
            s32* dst_auxb = stp->auxB[fp->salAuxFrame];
            u32 i;
            if (di->studio >= MIX_MAX_STUDIOS || fp->st[di->studio].state != 1) {
                continue;
            }
            for (i = 0; i < BUS_LEN; i++) {
                s32 sample = src_main[i];
                dst_main[i] = clamp_accum((s64)dst_main[i] + apply_gain(sample, di->vol));
                dst_auxa[i] = clamp_accum((s64)dst_auxa[i] + apply_gain(sample, di->volA));
                dst_auxb[i] = clamp_accum((s64)dst_auxb[i] + apply_gain(sample, di->volB));
            }
        }
    }
}

/* dspVoice points at the base of the console's voice array; used only to
 * turn a DSPvoice* back into an index into our own `voices[]` shadow array,
 * which salActivateVoice's linked list does not otherwise give us. */
/* --mixtrace: the mixer's inputs, voice by voice, before each voice is
 * rendered.  Two builds that mix differently (PLAN.md 33.4: the .wav is not
 * stable across builds) write two files whose first differing line names the
 * DSP frame, the voice and the field. */
static void mixtrace_voice(const DSPvoice* dv, u32 vi, u8 st) {
    if (!port_opt.mixtrace) {
        return;
    }
    if (!mixtrace_f) {
        mixtrace_f = fopen(port_opt.mixtrace, "w");
        if (!mixtrace_f) {
            port_log("port> musyx_mix: --mixtrace: cannot write %s\n", port_opt.mixtrace);
            port_opt.mixtrace = NULL;
            return;
        }
    }
    fprintf(mixtrace_f,
            "f%lu st%u v%u state %u addr %08x pitch %x %x %x %x %x chg %x %x %x %x %x "
            "vol %u %u %u a %u %u %u b %u %u %u last %u %u %u smp %u info %x addr %08x "
            "off %x len %x loop %x/%x ct %u adsr %u/%u/%x src %u/%u itd %u/%u "
            "so %u play %x/%x/%x lu %u/%u/%u/%u vs %x flags %x prio %u adsrx %x/%x/%x "
            "%08x/%08x/%08x/%08x\n",
            stat_frames_mixed, st, vi, dv->state, dv->currentAddr, dv->pitch[0], dv->pitch[1],
            dv->pitch[2], dv->pitch[3], dv->pitch[4], dv->changed[0], dv->changed[1],
            dv->changed[2], dv->changed[3], dv->changed[4], dv->volL, dv->volR, dv->volS,
            dv->volLa, dv->volRa, dv->volSa, dv->volLb, dv->volRb, dv->volSb, dv->lastVolL,
            dv->lastVolR, dv->lastVolS, dv->smp_id, dv->smp_info.info,
            (unsigned)(uintptr_t)dv->smp_info.addr, dv->smp_info.offset, dv->smp_info.length,
            dv->smp_info.loop, dv->smp_info.loopLength, dv->smp_info.compType, dv->adsr.mode,
            dv->adsr.state, dv->adsr.currentVolume, dv->srcTypeSelect, dv->srcCoefSelect,
            dv->itdShiftL, dv->itdShiftR, dv->singleOffset, dv->playInfo.posHi, dv->playInfo.posLo, dv->playInfo.pitch,
            dv->lastUpdate.pitch, dv->lastUpdate.vol, dv->lastUpdate.volA, dv->lastUpdate.volB,
            dv->virtualSampleID, dv->flags, dv->prio, dv->adsr.cnt, dv->adsr.currentIndex,
            dv->adsr.currentDelta, ((const u32*)&dv->adsr.data)[0], ((const u32*)&dv->adsr.data)[1],
            ((const u32*)&dv->adsr.data)[2], ((const u32*)&dv->adsr.data)[3]);
}

static u32 trace_fnv_s32(const s32* b, u32 n) {
    u32 h = 2166136261u;
    const u8* p = (const u8*)b;
    n *= sizeof(s32);
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}
/* --mixtrace: one line per DSP frame with a digest of studio 0's buses at
 * each stage, so a divergence names its stage: after the voices (main/auxA/
 * auxB of this frame), after the depop, the aux returns the effects wrote
 * (what fold_aux_return is about to add), and the folded main. */
static void mixtrace_stage(const MixFramePlan* fp, const char* tag) {
    DSPstudioinfo* stp = &dspStudio[0];
    if (!mixtrace_f || fp->st[0].state != 1) {
        return;
    }
    fprintf(mixtrace_f, "F%lu %s main %08x auxA %08x auxB %08x retA %08x retB %08x\n",
            stat_frames_mixed, tag, trace_fnv_s32(stp->main[fp->salFrame], BUS_LEN),
            trace_fnv_s32(stp->auxA[fp->salAuxFrame], BUS_LEN),
            trace_fnv_s32(stp->auxB[fp->salAuxFrame], BUS_LEN),
            trace_fnv_s32(stp->auxA[(fp->salAuxFrame + 1) % 3], BUS_LEN),
            trace_fnv_s32(stp->auxB[(fp->salAuxFrame + 1) % 3], BUS_LEN));
}

/* the `mv` half of the trace, from the voice table the rendering half
 * holds (the fused frame's `voices[]`, the worker's `wvoices[]`) */
static void mixtrace_mv(const MixVoice* mv, u32 vi, const DSPstudioinfo* stp,
                        const MixFramePlan* fp) {
    u32 sd = 0;
    (void)stp;
    (void)fp;
    if (mv->live && mv->readBase) {
        /* the sample bytes about to be read: ADPCM is 8 bytes per
         * 14 samples, PCM16 2 per sample, PCM8 1 */
        u32 off = mv->compType == 2 ? mv->curSample * 2
                  : mv->compType == 3 ? mv->curSample
                                      : (mv->curSample / 14u) * 8u;
        if (off + 256 <= mv->readLen) {
            sd = trace_fnv_s32((const s32*)(mv->readBase + off), 64);
        }
    }
    fprintf(mixtrace_f, "  smp256 %08x\n", sd);
    /* M24: an ARAM read base as its offset, so two processes' traces
     * compare (the arena's address is the mmap's, not the run's) */
    fprintf(mixtrace_f,
            "  mv live %u ct %u lt %u loop %u base %08x read %s%lx/%x/%u len %x "
            "ls %x le %x yn %d/%d ps %x fo %u cur %x src %u pitch %x phase %x "
            "hist %d/%d/%d/%d slc %x ended %u pb %u\n",
            mv->live, mv->compType, mv->loopType, mv->looping, mv->addrBase,
            mv->readKind == SAMPLE_LOC_ARAM ? "aram+" : "",
            mv->readKind == SAMPLE_LOC_ARAM
                ? (unsigned long)(mv->readBase - (const u8*)port_aram())
                : (unsigned long)(uintptr_t)mv->readBase,
            mv->readLen, mv->readKind, mv->length,
            mv->loopStart, mv->loopEnd, mv->yn1, mv->yn2, mv->predScale,
            mv->frameOffset, mv->curSample, mv->srcType, mv->pitch, mv->phase,
            mv->hist[0], mv->hist[1], mv->hist[2], mv->hist[3],
            mv->streamLoopCnt, mv->ended, mv->postBreak);
    (void)vi;
}

static void mixtrace_after(u32 vi, const DSPstudioinfo* stp, const MixFramePlan* fp) {
    fprintf(mixtrace_f, "  after v%u main %08x auxA %08x\n", vi,
            trace_fnv_s32(stp->main[fp->salFrame], BUS_LEN),
            trace_fnv_s32(stp->auxA[fp->salAuxFrame], BUS_LEN));
}

/* The voices, walked.  MIX_BOTH and MIX_CTL walk the studios' live lists as
 * the console's command-list builder did, capturing `next` before a voice
 * can deactivate itself; MIX_CTL also records every voice it visits (with
 * the DSPvoice as it stood at that moment -- after the voices before it in
 * the walk have run, which matters when a voice-done message deactivates a
 * later one) into the plan.  MIX_VAL walks the plan.  Returns the pool
 * entries used, or -1 when MIX_CTL ran out of pool. */
static int mix_studio_voices(int mode, MixFramePlan* fp, MixVoice* vtab, MixVoicePlan* pool,
                             unsigned pool_cap) {
    u8 st;
    unsigned used = 0;
    if (mode == MIX_VAL) {
        unsigned k;
        for (k = 0; k < fp->nvoices; k++) {
            MixVoicePlan* p = &fp->voices[k];
            DSPstudioinfo* stp = &dspStudio[p->studio];
            if (mixtrace_f || port_opt.mixtrace) {
                mixtrace_voice(&p->dv, p->vi, p->studio);
                if (mixtrace_f) {
                    mixtrace_mv(&vtab[p->vi], p->vi, stp, fp);
                }
            }
            render_voice(MIX_VAL, &p->dv, &vtab[p->vi], stp, fp->salFrame, fp->salAuxFrame, p);
            if (mixtrace_f) {
                mixtrace_after(p->vi, stp, fp);
            }
        }
        return (int)fp->nvoices;
    }
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        DSPvoice* dv;
        if (fp->st[st].state != 1) {
            continue;
        }
        dv = stp->voiceRoot;
        while (dv) {
            DSPvoice* next = dv->next; /* capture before a possible deactivate */
            u32 vi = (u32)(dv - dspVoice);
            if (vi < num_voices) {
                if (mode == MIX_CTL) {
                    MixVoicePlan* p;
                    if (used >= pool_cap) {
                        return -1;
                    }
                    p = &pool[used++];
                    p->vi = (u8)vi;
                    p->studio = st;
                    p->action = MIX_PLAN_SKIP;
                    p->has_note = 0;
                    memcpy(&p->dv, dv, sizeof(p->dv));
                    render_voice(MIX_CTL, dv, &vtab[vi], stp, fp->salFrame, fp->salAuxFrame, p);
                } else {
                    if (mixtrace_f || port_opt.mixtrace) {
                        mixtrace_voice(dv, vi, st);
                        if (mixtrace_f) {
                            mixtrace_mv(&vtab[vi], vi, stp, fp);
                        }
                    }
                    render_voice(MIX_BOTH, dv, &vtab[vi], stp, fp->salFrame, fp->salAuxFrame, NULL);
                    if (mixtrace_f) {
                        mixtrace_after(vi, stp, fp);
                    }
                }
            }
            dv = next;
        }
    }
    fp->nvoices = used;
    fp->voices = pool;
    return (int)used;
}

static void add_buf(s32* dst, const s32* src) {
    u32 i;
    for (i = 0; i < BUS_LEN; i++) {
        dst[i] = clamp_accum((s64)dst[i] + src[i]);
    }
}

/* Aux return: fold the slot the host's reverb/delay callback (run by
 * salHandleAuxProcessing, between salCtrlDsp calls) already processed back
 * into this frame's main[] bus. */
static void fold_aux_return(const MixFramePlan* fp) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        if (fp->st[st].state != 1) {
            continue;
        }
        add_buf(stp->main[fp->salFrame], stp->auxA[(fp->salAuxFrame + 1) % 3]);
        if (fp->st[st].type == SND_STUDIO_TYPE_STD) {
            add_buf(stp->main[fp->salFrame], stp->auxB[(fp->salAuxFrame + 1) % 3]);
        } else {
            /* SND_STUDIO_TYPE_DPL2: auxB carries the rear L/R of the Dolby
             * Pro Logic II matrix rather than an effects send. There is no
             * decoder here to place a true rear image; this folds the rear
             * pair straight into the front stereo image at half gain (a
             * "downmix", not a matrix decode) so a DPL2 studio is not
             * silent rather than because it is spec-accurate. auxB[salFrame]
             * is this frame's own rear write (voices render into it above);
             * auxB[salFrame ^ 1] would be last frame's, which is not used
             * here but is what a real decoder would also want for its own
             * all-pass/delay state. */
            s32* rear = stp->auxB[fp->salFrame];
            s32* main_buf = stp->main[fp->salFrame];
            u32 i;
            for (i = 0; i < BUS_LEN; i++) {
                main_buf[i] = clamp_accum((s64)main_buf[i] + (rear[i] >> 1));
            }
        }
    }
}

static void render_output(const MixFramePlan* fp, short* dest) {
    static s32 out_l[FRAME_SAMPLES], out_r[FRAME_SAMPLES], out_s[FRAME_SAMPLES];
    u8 st;
    u32 i;

    memset(out_l, 0, sizeof(out_l));
    memset(out_r, 0, sizeof(out_r));
    memset(out_s, 0, sizeof(out_s));

    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        s32* mb;
        if (fp->st[st].state != 1 || !fp->st[st].isMaster) {
            continue;
        }
        mb = stp->main[fp->salFrame];
        for (i = 0; i < FRAME_SAMPLES; i++) {
            out_l[i] = clamp_accum((s64)out_l[i] + mb[BUS_L_OFF + i]);
            out_r[i] = clamp_accum((s64)out_r[i] + mb[BUS_R_OFF + i]);
            out_s[i] = clamp_accum((s64)out_s[i] + mb[BUS_S_OFF + i]);
        }
    }

    for (i = 0; i < FRAME_SAMPLES; i++) {
        s32 l = out_l[i];
        s32 r = out_r[i];
        s32 s_ch = out_s[i];
#if SURROUND_ADDS_TO_L
        l += s_ch;
#else
        r += s_ch;
#endif
#if SURROUND_SUBTRACTS_FROM_R
        r -= s_ch;
#else
        l -= s_ch;
#endif
        {
            s16 ls = clamp_s16(l);
            s16 rs = clamp_s16(r);
            u32 abs_l = (u32)(ls < 0 ? -ls : ls);
            u32 abs_r = (u32)(rs < 0 ? -rs : rs);
            if (abs_l > stat_peak_abs) stat_peak_abs = abs_l;
            if (abs_r > stat_peak_abs) stat_peak_abs = abs_r;
            if (port_opt.clickstat) {
                /* wavstat.py's rule, in process and on the same channel: the
                 * left-channel sample-to-sample step, and how many exceed
                 * half full scale.  Same threshold, so the number a soak
                 * prints and the number a capture is measured at agree. */
                static int have_prev;
                static s32 prev_l;
                if (have_prev) {
                    long d = (long)ls - (long)prev_l;
                    if (d < 0) d = -d;
                    if (d > stat_worst_step) stat_worst_step = d;
                    if (d > 16384) stat_clicks++;
                }
                prev_l = ls;
                have_prev = 1;
            }
            dest[i * 2 + 0] = ls;
            dest[i * 2 + 1] = rs;
        }
    }
}


/* ---- public entry points ---------------------------------------------------- */

/* The voice array is allocated once, at a fixed size, and never handed back.
 *
 * It is the console's *DSP state*: every voice's sample cursor, ADPCM decode
 * history, resampler window and loop bookkeeping -- state that on the
 * GameCube lived in the DSP's own registers and that MusyX reads back through
 * `hwGetPos`/`hwIsActive` to decide when a sound has finished.  So a snapshot
 * has to carry it (PLAN.md 24.2), and a restored process has to have somewhere
 * to put it *before* the game's audio init would have created it -- which in a
 * restored process never runs.  Hence a fixed MIX_MAX_VOICES rather than
 * `salNumVoices`: the registry entry must be the same size in the run that
 * takes a snapshot and the run that restores it.
 *
 * The first restore without this mixed nothing at all: `voices` was NULL, the
 * mixer was down, and MusyX's own state then diverged within fifty frames
 * because no voice ever reported itself finished (PLAN.md 24.4). */
#define MIX_MAX_VOICES 64

void port_musyx_mix_voices_ensure(void) {
    /* Same reasoning as gx_draw_reset in port_gx_init: the resampler's
     * coefficient table is the process's, not the game's init's. */
    src_table_init();
    if (!voices) {
        voices = (MixVoice*)calloc(MIX_MAX_VOICES, sizeof(MixVoice));
    }
}

void port_musyx_mix_snap_register(void) {
    port_musyx_mix_voices_ensure();
    if (!voices) {
        return;
    }
    port_snap_register("musyx.mix_voices", voices,
                       (unsigned long)MIX_MAX_VOICES * sizeof(MixVoice));
    port_snap_register("musyx.mix_num_voices", &num_voices, sizeof(num_voices));
    port_snap_register("musyx.mixer_up", &mixer_up, sizeof(mixer_up));
}

void port_musyx_mix_init(void) {
    src_table_init();
    num_voices = salNumVoices;
    if (num_voices > MIX_MAX_VOICES) {
        port_log("port> musyx_mix: %u voices is more than the %d a snapshot "
                 "can carry; snapshots of this run will not restore\n",
                 num_voices, MIX_MAX_VOICES);
    }
    port_musyx_mix_voices_ensure();
    if (voices) {
        memset(voices, 0, (size_t)MIX_MAX_VOICES * sizeof(MixVoice));
    }
    mixer_up = (voices != NULL);
    stat_frames_mixed = 0;
    stat_voices_started = 0;
    stat_voices_ended = 0;
    stat_aram_clamped = 0;
    stat_peak_abs = 0;
    stat_max_concurrent_voices = 0;
    stat_time_sum = 0.0;
    stat_time_worst = 0.0;
    stat_time_samples = 0;
    port_log("port> musyx_mix: CPU mixer up, %u voices, %u studios, %u Hz%s%s\n",
             num_voices, (unsigned)salMaxStudioNum, MIX_FRQ,
             port_opt.mixtrace ? ", tracing to " : "", port_opt.mixtrace ? port_opt.mixtrace : "");
}

/* M19 (PLAN.md 34.4): HuMemMemoryFree, via gx_skin.c's port_mem_freed.  A
 * voice whose sample lives in MEM1 and is still being read when the game
 * frees the block will play whatever the heap puts there next -- including
 * the block headers' return addresses, which is what makes the .wav a
 * function of the binary's layout.  Counted, and the first few named. */
static unsigned long stat_voices_freed_under;
void port_musyx_mix_mem_freed(const void* data, unsigned long size) {
    const u8* lo = (const u8*)data;
    const u8* hi = lo + size;
    u32 vi;
    if (!mixer_up || !voices) {
        return;
    }
    for (vi = 0; vi < num_voices; vi++) {
        MixVoice* mv = &voices[vi];
        const u8* p;
        if (!mv->live || mv->readKind != SAMPLE_LOC_MEM1) {
            continue;
        }
        p = mv->readBase;
        if (p >= lo && p < hi) {
            DSPvoice* dv = &dspVoice[vi];
            stat_voices_freed_under++;
            if (stat_voices_freed_under <= 8) {
                port_log("port> musyx_mix: FREED UNDER A VOICE: voice %u (sample %u, compType %u, "
                         "MEM1 %p, at sample %u of %u, state %u, vol %u/%u env %x) -- the game "
                         "freed block %p+%lu at DSP frame %lu while the voice reads it%s\n",
                         vi, (unsigned)dv->smp_id, mv->compType, (const void*)p, mv->curSample,
                         mv->length, dv->state, dv->volL, dv->volR, (unsigned)dv->adsr.currentVolume,
                         data, size, stat_frames_mixed, mixtrace_f ? " (see the trace)" : "");
            }
            if (mixtrace_f) {
                fprintf(mixtrace_f, "FREED UNDER voice %u sample %u at frame %lu block %p+%lu\n",
                        vi, (unsigned)dv->smp_id, stat_frames_mixed, data, size);
            }
        }
    }
}

/* The fused frame: control and values in one pass on the game thread.
 * Every run before M24 ran this and `--threads 0` still does; with the
 * workers on it is also what a tick runs when a voice's sample cannot be
 * read behind the game's back (port_musyx_mix_needs_inline). */
void port_musyx_mix_frame(short* dest) {
    double t0 = 0.0;
    MixFramePlan fp;

    if (!dest) {
        return;
    }
    if (!mixer_up) {
        memset(dest, 0, FRAME_SAMPLES * 2 * sizeof(short));
        return;
    }

    if (port_opt.perf) {
        t0 = port_now_seconds();
    }

    capture_studios(&fp);
    stat_voices_active_this_frame = 0;
    zero_buses(&fp);
    mix_studio_inputs(&fp);
    mixtrace_stage(&fp, "in");
    mix_studio_voices(MIX_BOTH, &fp, voices, NULL, 0);
    mixtrace_stage(&fp, "voices");
    /* After the voices, before the aux return: the step a cut voice leaves is
     * in the dry bus, and the console injects the compensating offset into
     * the same bus in the same frame (hw_dspctrl.c:1880, right after
     * UPLOAD_LRS). */
    apply_depop(&fp);
    mixtrace_stage(&fp, "depop");
    fold_aux_return(&fp);
    mixtrace_stage(&fp, "folded");
    render_output(&fp, dest);

    if (stat_voices_active_this_frame > stat_max_concurrent_voices) {
        stat_max_concurrent_voices = stat_voices_active_this_frame;
    }

    if (port_opt.perf) {
        double dt = port_now_seconds() - t0;
        stat_time_sum += dt;
        stat_time_samples++;
        if (dt > stat_time_worst) {
            stat_time_worst = dt;
        }
    }

    stat_frames_mixed++;
}

/* ---- M24: the frame in two halves ------------------------------------------
 *
 * `wvoices` is the worker's voice table.  At the start of a tick it is a
 * copy of `voices` (the two are equal then: the last job was joined and
 * reconciled); the control halves of the tick's frames advance `voices`
 * (positions only), the value halves advance `wvoices` (positions and
 * values) from the same inputs; at the join the positions are compared and
 * `voices` takes `wvoices` whole, so the snapshot registry's `voices` is the
 * complete state at every retrace boundary and a restore seeds the worker
 * for free through the copy at the next tick. */
static MixVoice* wvoices;
static unsigned long stat_ctl_frames, stat_val_frames, stat_reconciles, stat_ctl_val_mismatch;
static double stat_val_time_sum;

/* A voice the value half must not read behind the game's back: a MEM1
 * sample (the game may free or rewrite the block during its frame; none
 * since M19 stored every sample in the port's heap) or a virtual sample
 * (compType 5: its start is a MusyX message the value half cannot send;
 * unused by this game).  A stream (compType 4) is fine: its ring buffer is
 * refilled through aramUploadData, which finishes the pending job first. */
static unsigned stat_inline_named;
int port_musyx_mix_needs_inline(void) {
    u32 vi;
    if (!mixer_up || !voices) {
        return 0;
    }
    for (vi = 0; vi < num_voices; vi++) {
        const MixVoice* mv = &voices[vi];
        const DSPvoice* dv = &dspVoice[vi];
        int why = 0;
        if (mv->live && (mv->readKind == SAMPLE_LOC_MEM1 || mv->compType == 5)) {
            why = 1;
        } else if (dv->state == 1 && dv->smp_info.compType == 5) {
            why = 2;
        }
        if (why) {
            if (stat_inline_named < 4) {
                stat_inline_named++;
                port_log("port> musyx_mix: tick fused: voice %u %s (compType %u, readKind %u, "
                         "state %u, sample %u) at DSP frame %lu\n",
                         vi, why == 1 ? "live" : "starting", why == 1 ? mv->compType : dv->smp_info.compType,
                         mv->readKind, dv->state, (unsigned)dv->smp_id, stat_frames_mixed);
            }
            return 1;
        }
    }
    return 0;
}

int port_musyx_mix_ctl_poisoned(void) {
    int p = ctl_poisoned;
    ctl_poisoned = 0;
    return p;
}

void port_musyx_mix_job_begin(void) {
    if (!wvoices) {
        wvoices = (MixVoice*)calloc(MIX_MAX_VOICES, sizeof(MixVoice));
    }
    if (wvoices && voices) {
        memcpy(wvoices, voices, (size_t)MIX_MAX_VOICES * sizeof(MixVoice));
    }
    ctl_poisoned = 0;
}

int port_musyx_mix_frame_ctl(MixFramePlan* fp, short* dest, unsigned long retrace,
                             MixVoicePlan* pool, unsigned pool_cap) {
    int n;
    if (!dest) {
        return 0;
    }
    fp->dest = dest;
    fp->retrace = retrace;
    fp->nvoices = 0;
    fp->voices = pool;
    if (!mixer_up || !wvoices) {
        /* nothing to control: the value half writes silence */
        capture_studios(fp);
        return 0;
    }
    capture_studios(fp);
    n = mix_studio_voices(MIX_CTL, fp, voices, pool, pool_cap);
    if (n >= 0) {
        stat_ctl_frames++;
    }
    return n;
}

void port_musyx_mix_frame_val(const MixFramePlan* fp) {
    double t0 = 0.0;
    MixFramePlan f = *fp; /* the walk fills nothing here, but the type is shared */
    short* dest = fp->dest;

    if (!mixer_up || !wvoices) {
        memset(dest, 0, FRAME_SAMPLES * 2 * sizeof(short));
        return;
    }
    if (port_opt.perf) {
        t0 = port_now_seconds();
    }
    stat_voices_active_this_frame = 0;
    zero_buses(&f);
    mix_studio_inputs(&f);
    mixtrace_stage(&f, "in");
    mix_studio_voices(MIX_VAL, &f, wvoices, NULL, 0);
    mixtrace_stage(&f, "voices");
    apply_depop(&f);
    mixtrace_stage(&f, "depop");
    fold_aux_return(&f);
    mixtrace_stage(&f, "folded");
    render_output(&f, dest);

    if (stat_voices_active_this_frame > stat_max_concurrent_voices) {
        stat_max_concurrent_voices = stat_voices_active_this_frame;
    }
    if (port_opt.perf) {
        double dt = port_now_seconds() - t0;
        stat_val_time_sum += dt;
        stat_time_sum += dt;
        stat_time_samples++;
        if (dt > stat_time_worst) {
            stat_time_worst = dt;
        }
    }
    stat_val_frames++;
    stat_frames_mixed++;
}

/* The join: the control half's positions against the value half's, then
 * `voices` takes the worker's table whole. */
void port_musyx_mix_job_reconcile(void) {
    u32 vi;
    if (!wvoices || !voices) {
        return;
    }
    for (vi = 0; vi < num_voices; vi++) {
        const MixVoice* a = &voices[vi];
        const MixVoice* b = &wvoices[vi];
        /* a voice the start refused leaves its half-initialised MixVoice
         * behind on the control side (live 0), as the fused path does; only
         * a live voice's position is a claim */
        if (!a->live && !b->live) {
            continue;
        }
        if (a->live != b->live || a->curSample != b->curSample ||
            a->frameOffset != b->frameOffset || a->phase != b->phase || a->ended != b->ended ||
            a->streamLoopCnt != b->streamLoopCnt || a->readBase != b->readBase ||
            a->pitch != b->pitch || a->srcType != b->srcType || a->loopEnd != b->loopEnd) {
            stat_ctl_val_mismatch++;
            if (stat_ctl_val_mismatch <= 24) {
                const DSPvoice* dv = &dspVoice[vi];
                port_log("port> musyx_mix: SPLIT MISMATCH voice %u: control live %u cur %x fo %u "
                         "phase %x ended %u slc %x pitch %x | value live %u cur %x fo %u phase %x "
                         "ended %u slc %x pitch %x | dsp state %u smp %u comp %u readKind %u/%u "
                         "| ctl last: action %u (0 skip 1 run 2 start 3 refused 4 skip-dead "
                         "5 skip-after-start) state %u refused %u at frame %lu; now frame %lu "
                         "joins %lu flushes %lu\n",
                         vi, a->live, a->curSample, a->frameOffset, a->phase, a->ended,
                         a->streamLoopCnt, a->pitch, b->live, b->curSample, b->frameOffset,
                         b->phase, b->ended, b->streamLoopCnt, b->pitch, (unsigned)dv->state,
                         (unsigned)dv->smp_id, (unsigned)dv->smp_info.compType,
                         (unsigned)a->readKind, (unsigned)b->readKind,
                         diag_last_action[vi], diag_last_state[vi], diag_last_refused[vi],
                         diag_last_frame[vi], stat_frames_mixed, stat_reconciles,
                         port_audio_flushes());
            }
        }
    }
    memcpy(voices, wvoices, (size_t)MIX_MAX_VOICES * sizeof(MixVoice));
    stat_reconciles++;
}

void port_musyx_mix_split_report(void) {
    if (!stat_ctl_frames && !stat_val_frames) {
        return;
    }
    port_log("port> musyx_mix: split frames: %lu control halves on the game thread, %lu value "
             "halves (%.0f ms), %lu joins, %lu position mismatches%s\n",
             stat_ctl_frames, stat_val_frames, stat_val_time_sum * 1000.0, stat_reconciles,
             stat_ctl_val_mismatch, stat_ctl_val_mismatch ? "  <-- NOT EXACT" : "");
}


void port_musyx_mix_shutdown(void) {
    if (mixtrace_f) {
        fclose(mixtrace_f);
        mixtrace_f = NULL;
    }
    /* The array is kept -- its address is in the snapshot registry -- and only
     * emptied.  It is 64 voices, not a leak worth the risk. */
    if (voices) {
        memset(voices, 0, (size_t)MIX_MAX_VOICES * sizeof(MixVoice));
    }
    if (wvoices) {
        memset(wvoices, 0, (size_t)MIX_MAX_VOICES * sizeof(MixVoice));
    }
    mixer_up = 0;
}

void port_musyx_mix_split_report(void);
void port_musyx_mix_report(void) {
    port_log("port> musyx_mix: %lu frames mixed, %lu voices started, "
             "%lu voices ended, peak |sample| %lu%s, %lu clamped ARAM reads, "
             "%u max concurrent voices (of %u allocated)\n",
             stat_frames_mixed, stat_voices_started, stat_voices_ended,
             (unsigned long)stat_peak_abs, stat_peak_abs > 32000 ? " (near full scale)" : "",
             stat_aram_clamped, stat_max_concurrent_voices, num_voices);
    if (port_opt.mixcheck) {
        port_log("port> musyx_mix: --mixcheck: %lu gains and their accumulates checked against "
                 "the 64-bit form, %lu disagreed\n",
                 stat_mixcheck_samples, stat_mixcheck_bad);
    }
    if (stat_voices_in_free_block) {
        port_log("port> musyx_mix: %lu voice(s) started on a MEM1 sample that no allocated "
                 "heap block holds (PLAN.md 34.4)\n",
                 stat_voices_in_free_block);
    }
    if (stat_voices_freed_under) {
        port_log("port> musyx_mix: %lu time(s) the game freed a MEM1 block a live voice was "
                 "still reading (PLAN.md 34.4)\n",
                 stat_voices_freed_under);
    }
    port_log("port> musyx_mix: resampler %s, depop %s; %lu voice(s) cut mid-frame\n",
             port_opt.resample4 ? "4-tap Catmull-Rom" : "linear",
             port_opt.depop ? "on" : "off", stat_depop_cuts);
    if (port_opt.clickstat) {
        port_log("port> musyx_mix: --clickstat: %lu step(s) over half full scale, "
                 "worst step %ld (wavstat.py's rule, on the mixed left channel)\n",
                 stat_clicks, stat_worst_step);
    }
    if (stat_voices_no_extradata) {
        port_log("port> musyx_mix: %lu ADPCM voice(s) refused for a missing extraData "
                 "block\n",
                 stat_voices_no_extradata);
    }
    if (stat_voices_refused) {
        port_log("port> musyx_mix: %lu start(s) refused by start_voice_init (a zero-length "
                 "one-shot, an envelope done at once, an unreadable sample); the slot's "
                 "entry cleared on both halves (M25)\n",
                 stat_voices_refused);
    }
    if (stat_bad_sample_addr) {
        port_log("port> musyx_mix: %lu sample address(es) resolved to neither ARAM nor "
                 "MEM1 -- voice(s) refused rather than read\n",
                 stat_bad_sample_addr);
    }
    if (port_opt.perf && stat_time_samples) {
        port_log("port> musyx_mix: --perf: mean %.1f us/frame, worst %.1f us/frame, "
                 "over %lu timed frames\n",
                 (stat_time_sum / stat_time_samples) * 1e6, stat_time_worst * 1e6,
                 stat_time_samples);
    }
    port_musyx_mix_split_report();
}
