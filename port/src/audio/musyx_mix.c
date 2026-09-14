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
#include "musyx_mix.h"

#include <stdlib.h>
#include <string.h>

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

    u32 addrBase; /* smp_info.addr widened to u32, format-native units      */
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

    /* resampler: srcTypeSelect 2 has no lookahead; 0/1 keep one sample of
     * history so a real 4-tap filter can replace the linear blend later
     * without changing anything else about this struct.                    */
    u16 srcType;
    u32 pitch; /* 16.16, 0x10000 == 1.0x; adopted from changed[]&8           */
    u32 phase; /* 16.16, in [0, 0x10000)                                    */
    s32 prevSample, curSampleValue;

    u32 streamLoopCnt; /* stands in for the DSP's _PB.streamLoopCnt; nothing
                         * reads it yet -- see the report for what a host
                         * accessor would need.                             */
    u8 ended;      /* one-shot ran past its length                          */
    u8 postBreak;  /* hwBreak seen this voice, fading via a 10-unit release */
} MixVoice;

static MixVoice* voices;
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

/* --perf-gated per-call cost, in seconds; port_now_seconds() is the same
 * clock port_perf_* already uses elsewhere, so this composes with --perf
 * without adding its own overhead when the flag is off. */
static double stat_time_sum;
static double stat_time_worst;
static unsigned long stat_time_samples;

/* ---- ARAM access, bounds-checked ------------------------------------------
 *
 * `smp_info.addr`/loop points ultimately come off disc data the port has not
 * independently validated, and this runs on real hardware -- a bad address
 * must degrade to silence, never to a wild read.  Every access funnels
 * through these three helpers so the clamp counter is exhaustive.
 */
static u8* aram_base_ptr;

static u8 aram_read_u8(u32 off) {
    if (off >= PORT_ARAM_SIZE) {
        stat_aram_clamped++;
        return 0;
    }
    return aram_base_ptr[off];
}

static s16 aram_read_s16be(u32 byte_off) {
    u8 hi, lo;
    if (byte_off + 1 >= PORT_ARAM_SIZE) {
        stat_aram_clamped++;
        return 0;
    }
    /* PCM16 sample data is big-endian in ARAM on both the console and (per
     * the port's own convention, see port/src/gx/gx_draw.c's read_component)
     * the little-endian dev host; assemble the bytes explicitly so this is
     * correct either way rather than relying on host struct layout. */
    hi = aram_base_ptr[byte_off];
    lo = aram_base_ptr[byte_off + 1];
    return (s16)((hi << 8) | lo);
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

/* Decode exactly the next ADPCM sample for `mv`, honouring loop wraparound,
 * and advance `mv->curSample`/`mv->frameOffset`.  `frame_byte_base` is the
 * ARAM byte address of sample 0 of frame 0 (== mv->addrBase for compType
 * 0/4/5; the seek-adjusted block start for compType 1). Returns the decoded
 * sample already normalised to the common s16 scale (see the gain-scaling
 * note in start_voice()). */
static s32 adpcm_decode_advance(DSPvoice* dv, MixVoice* mv) {
    u32 frame_no = mv->curSample / 14u;
    u32 frame_byte = mv->addrBase + frame_no * 8u;
    s32 out;

    if (mv->frameOffset == 0) {
        u8 ps = aram_read_u8(frame_byte);
        mv->predScale = ps;
    }

    {
        u8 predictor = (u8)(mv->predScale >> 4);
        u8 scale = (u8)(mv->predScale & 0xF);
        u32 data_byte_index = 1u + mv->frameOffset / 2u;
        u8 raw = aram_read_u8(frame_byte + data_byte_index);
        u32 nibble = (mv->frameOffset & 1u) ? (raw & 0xF) : (raw >> 4);
        s32 s = sign_extend4(nibble);
        s32 c0 = mv->coefTab[predictor][0];
        s32 c1 = mv->coefTab[predictor][1];
        s64 acc = ((s64)s << scale << 11) + (s64)c0 * mv->yn1 + (s64)c1 * mv->yn2;
        out = clamp_s16((s32)((acc + 1024) >> 11));
        mv->yn2 = mv->yn1;
        mv->yn1 = out;
    }

    mv->curSample++;
    mv->frameOffset++;
    if (mv->frameOffset == 14) {
        mv->frameOffset = 0;
    }

    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->frameOffset = (u32)(mv->loopStart % 14u);
        if (mv->loopType == 0) {
            mv->yn1 = mv->loopY1;
            mv->yn2 = mv->loopY0;
            mv->predScale = mv->loopPS;
        } else {
            mv->predScale = dv->streamLoopPS;
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
         * buffer. This only runs once per voice (`inLoopBuffer` latches). */
        if (mv->compType == 5 && !dv->vSampleInfo.inLoopBuffer &&
            dv->vSampleInfo.loopBufferLength != 0) {
            mv->addrBase = (u32)(uintptr_t)dv->vSampleInfo.loopBufferAddr;
            mv->loopStart = 0;
            mv->loopEnd = dv->vSampleInfo.loopBufferLength - 1;
            mv->curSample = 0;
            mv->frameOffset = 0;
            dv->vSampleInfo.inLoopBuffer = 1;
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
static s32 pcm16_decode_advance(MixVoice* mv) {
    s32 out = aram_read_s16be(mv->addrBase + mv->curSample * 2u);
    mv->curSample++;
    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->streamLoopCnt++;
    } else if (!mv->looping && mv->curSample >= mv->length) {
        mv->ended = 1;
    }
    return out;
}

static s32 pcm8_decode_advance(MixVoice* mv) {
    s8 raw = (s8)aram_read_u8(mv->addrBase + mv->curSample);
    s32 out = (s32)raw << 8;
    mv->curSample++;
    if (mv->looping && mv->curSample > mv->loopEnd) {
        mv->curSample = mv->loopStart;
        mv->streamLoopCnt++;
    } else if (!mv->looping && mv->curSample >= mv->length) {
        mv->ended = 1;
    }
    return out;
}

static s32 voice_decode_advance(DSPvoice* dv, MixVoice* mv) {
    if (mv->ended) {
        return 0;
    }
    switch (mv->compType) {
    case 0:
    case 1:
    case 4:
    case 5:
        return adpcm_decode_advance(dv, mv);
    case 2:
        return pcm16_decode_advance(mv);
    case 3:
        return pcm8_decode_advance(mv);
    default:
        mv->ended = 1;
        return 0;
    }
}

/* One resampled output sample.
 *
 * srcTypeSelect == 2 ("no SRC") advances exactly one input sample per output
 * sample, per spec.  0 and 1 are the console's interpolating resamplers (a
 * 4-tap polyphase filter selected by srcCoefSelect); this mixer does linear
 * interpolation between the two nearest source samples instead -- a
 * deliberate first-cut simplification -- but keeps a one-sample lookahead
 * (`prevSample`/`curSampleValue`) so dropping in the real 4-tap filter later
 * only touches this function. */
static s32 voice_output_sample(DSPvoice* dv, MixVoice* mv) {
    s32 out;

    if (mv->srcType == 2) {
        return voice_decode_advance(dv, mv);
    }

    out = mv->prevSample + (s32)((((s64)(mv->curSampleValue - mv->prevSample)) *
                                  (s64)(mv->phase & 0xFFFF)) >> 16);
    mv->phase += mv->pitch;
    while (mv->phase >= 0x10000u && !mv->ended) {
        mv->phase -= 0x10000u;
        mv->prevSample = mv->curSampleValue;
        mv->curSampleValue = voice_decode_advance(dv, mv);
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
 * in the original after a `salDeactivateVoice`). */
static int start_voice(DSPvoice* dv, MixVoice* mv) {
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

    /* Resampler lookahead: for srcType 0/1 we linearly blend between two
     * decoded samples, so prime both now.  srcType 2 does not need this,
     * but priming is harmless (voice_output_sample never reads it there). */
    mv->prevSample = voice_decode_advance(dv, mv);
    mv->curSampleValue = mv->ended ? mv->prevSample : voice_decode_advance(dv, mv);
    mv->phase = 0;

    mv->live = 1;
    dv->state = 2;
    stat_voices_started++;
    return 1;
}

/* Finish a voice this frame: notify the sequencer and unlink it from its
 * studio's voice list (hw_dspctrl.c:1224/1600/1657 all do exactly this pair
 * when a one-shot runs out or adsrHandle reports VoiceDone). */
static void finish_voice(DSPvoice* dv, MixVoice* mv) {
    salSynthSendMessage(dv, 0);
    salDeactivateVoice(dv);
    mv->live = 0;
    stat_voices_ended++;
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

/* ---- one voice, one 160-sample frame --------------------------------------
 *
 * Renders `dv` into its studio's main[]/auxA[]/auxB[] buses (already zeroed
 * by the caller for this frame) and updates every host-visible field this
 * mixer is responsible for. `next_out` receives dv->next captured *before*
 * any deactivation, since salDeactivateVoice() unlinks the voice from the
 * list the caller is walking. */
static void render_voice(DSPvoice* dv, MixVoice* mv, DSPstudioinfo* stp) {
    s32* main_buf = stp->main[salFrame];
    s32* auxa_buf = stp->auxA[salAuxFrame];
    s32* auxb_buf = stp->auxB[salAuxFrame];

    s16 dL, dR, dS, dLa, dRa, dSa, dLb, dRb, dSb;
    s32 volL, volR, volS, volLa, volRa, volSa, volLb, volRb, volSb;
    u32 s, i;
    int done = 0;
    int main_live, auxa_live, auxb_live, surround_live;

    if (dv->state == 1) {
        if (!start_voice(dv, mv)) {
            return;
        }
    }
    if (dv->state != 2 || !mv->live) {
        return;
    }
    stat_voices_active_this_frame++;

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
        s32 env;
        u32 voice_done;

        apply_subframe_changes(dv, mv, s);
        voice_done = adsrHandle(&dv->adsr, &env_start, &env_delta);
        update_hostplayinfo(dv);
        env = (s32)(s16)env_start;

        for (i = 0; i < SUBFRAME_SAMPLES; i++) {
            u32 idx = s * SUBFRAME_SAMPLES + i;
            s32 raw;

            if (mv->ended || port_musyx_mix_mute) {
                raw = 0;
            } else {
                raw = voice_output_sample(dv, mv);
            }

            /* total gain per bus = envelope volume x bus volume, both
             * ramping; each is a 0x7FFF-unity u16-scale multiply. Only the
             * buses (main_live/auxa_live/auxb_live) and channel
             * (surround_live) that could possibly be non-zero this frame are
             * touched -- see the flags computed above render_voice's ramp
             * setup. */
            if (raw != 0 && (main_live || auxa_live || auxb_live)) {
                s32 e = apply_gain(raw, env);
                if (main_live) {
                    main_buf[BUS_L_OFF + idx] = clamp_accum((s64)main_buf[BUS_L_OFF + idx] + apply_gain(e, volL));
                    main_buf[BUS_R_OFF + idx] = clamp_accum((s64)main_buf[BUS_R_OFF + idx] + apply_gain(e, volR));
                    if (surround_live) {
                        main_buf[BUS_S_OFF + idx] = clamp_accum((s64)main_buf[BUS_S_OFF + idx] + apply_gain(e, volS));
                    }
                }
                if (auxa_live) {
                    auxa_buf[BUS_L_OFF + idx] = clamp_accum((s64)auxa_buf[BUS_L_OFF + idx] + apply_gain(e, volLa));
                    auxa_buf[BUS_R_OFF + idx] = clamp_accum((s64)auxa_buf[BUS_R_OFF + idx] + apply_gain(e, volRa));
                    if (surround_live) {
                        auxa_buf[BUS_S_OFF + idx] = clamp_accum((s64)auxa_buf[BUS_S_OFF + idx] + apply_gain(e, volSa));
                    }
                }
                if (auxb_live) {
                    auxb_buf[BUS_L_OFF + idx] = clamp_accum((s64)auxb_buf[BUS_L_OFF + idx] + apply_gain(e, volLb));
                    auxb_buf[BUS_R_OFF + idx] = clamp_accum((s64)auxb_buf[BUS_R_OFF + idx] + apply_gain(e, volRb));
                    if (surround_live) {
                        auxb_buf[BUS_S_OFF + idx] = clamp_accum((s64)auxb_buf[BUS_S_OFF + idx] + apply_gain(e, volSb));
                    }
                }
            }

            env += env_delta;
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
        finish_voice(dv, mv);
    }
}

/* ---- studio-level passes ---------------------------------------------------- */

static void zero_buses(void) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        if (stp->state != 1) {
            continue;
        }
        memset(stp->main[salFrame], 0, BUS_LEN * sizeof(s32));
        memset(stp->auxA[salAuxFrame], 0, BUS_LEN * sizeof(s32));
        memset(stp->auxB[salAuxFrame], 0, BUS_LEN * sizeof(s32));
    }
}

/* Studio input chaining: each studio's `in[]` reads *last* frame's main[] of
 * its source studio (main[salFrame ^ 1]) so that studio chains do not need a
 * particular activation order within a frame (hw_dspctrl.c:867-872). */
static void mix_studio_inputs(void) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        u8 in;
        if (stp->state != 1) {
            continue;
        }
        for (in = 0; in < stp->numInputs; in++) {
            DSPinput* di = &stp->in[in];
            DSPstudioinfo* src = &dspStudio[di->studio];
            s32* src_main = src->main[salFrame ^ 1];
            s32* dst_main = stp->main[salFrame];
            s32* dst_auxa = stp->auxA[salAuxFrame];
            s32* dst_auxb = stp->auxB[salAuxFrame];
            u32 i;
            if (src->state != 1) {
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
static void mix_studio_voices(void) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        DSPvoice* dv;
        if (stp->state != 1) {
            continue;
        }
        dv = stp->voiceRoot;
        while (dv) {
            DSPvoice* next = dv->next; /* capture before a possible deactivate */
            u32 vi = (u32)(dv - dspVoice);
            if (vi < num_voices) {
                render_voice(dv, &voices[vi], stp);
            }
            dv = next;
        }
    }
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
static void fold_aux_return(void) {
    u8 st;
    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        if (stp->state != 1) {
            continue;
        }
        add_buf(stp->main[salFrame], stp->auxA[(salAuxFrame + 1) % 3]);
        if (stp->type == SND_STUDIO_TYPE_STD) {
            add_buf(stp->main[salFrame], stp->auxB[(salAuxFrame + 1) % 3]);
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
            s32* rear = stp->auxB[salFrame];
            s32* main_buf = stp->main[salFrame];
            u32 i;
            for (i = 0; i < BUS_LEN; i++) {
                main_buf[i] = clamp_accum((s64)main_buf[i] + (rear[i] >> 1));
            }
        }
    }
}

static void render_output(short* dest) {
    static s32 out_l[FRAME_SAMPLES], out_r[FRAME_SAMPLES], out_s[FRAME_SAMPLES];
    u8 st;
    u32 i;

    memset(out_l, 0, sizeof(out_l));
    memset(out_r, 0, sizeof(out_r));
    memset(out_s, 0, sizeof(out_s));

    for (st = 0; st < salMaxStudioNum; st++) {
        DSPstudioinfo* stp = &dspStudio[st];
        s32* mb;
        if (stp->state != 1 || !stp->isMaster) {
            continue;
        }
        mb = stp->main[salFrame];
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
            dest[i * 2 + 0] = ls;
            dest[i * 2 + 1] = rs;
        }
    }
}

/* ---- public entry points ---------------------------------------------------- */

void port_musyx_mix_init(void) {
    aram_base_ptr = (u8*)port_aram();
    num_voices = salNumVoices;
    voices = (MixVoice*)calloc(num_voices ? num_voices : 1, sizeof(MixVoice));
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
    port_log("port> musyx_mix: CPU mixer up, %u voices, %u studios, %u Hz\n",
             num_voices, (unsigned)salMaxStudioNum, MIX_FRQ);
}

void port_musyx_mix_frame(short* dest) {
    double t0 = 0.0;

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

    stat_voices_active_this_frame = 0;
    zero_buses();
    mix_studio_inputs();
    mix_studio_voices();
    fold_aux_return();
    render_output(dest);

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

void port_musyx_mix_shutdown(void) {
    if (voices) {
        free(voices);
        voices = NULL;
    }
    mixer_up = 0;
}

void port_musyx_mix_report(void) {
    port_log("port> musyx_mix: %lu frames mixed, %lu voices started, "
             "%lu voices ended, peak |sample| %lu%s, %lu clamped ARAM reads, "
             "%u max concurrent voices (of %u allocated)\n",
             stat_frames_mixed, stat_voices_started, stat_voices_ended,
             (unsigned long)stat_peak_abs, stat_peak_abs > 32000 ? " (near full scale)" : "",
             stat_aram_clamped, stat_max_concurrent_voices, num_voices);
    if (port_opt.perf && stat_time_samples) {
        port_log("port> musyx_mix: --perf: mean %.1f us/frame, worst %.1f us/frame, "
                 "over %lu timed frames\n",
                 (stat_time_sum / stat_time_samples) * 1e6, stat_time_worst * 1e6,
                 stat_time_samples);
    }
}
