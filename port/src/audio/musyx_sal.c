/* The MusyX SAL: what the GameCube's AI, DSP and interrupt controller did.
 *
 * `extern/musyx` is AxioDL's MIT reimplementation of MusyX, and above the SAL
 * it is portable as it stands -- the sequencer, the voice and ADSR model, the
 * 3-D emitters, the streaming layer and the four aux effects all compile for
 * `MUSY_TARGET_PC` untouched (32 of the 34 runtime translation units build
 * clean; the two that do not are `hw_pc.c`, which this file replaces, and
 * `profile.c`, which is not in MusyX's own build list either).
 *
 * What is missing below the SAL is exactly thirteen symbols.  That is the
 * whole job:
 *
 *   salInitAi salStartAi salExitAi salAiGetDest      the audio interface
 *   salInitDsp salExitDsp salCtrlDsp                 the DSP
 *   hwInitIrq hwExitIrq hwEnableIrq hwDisableIrq     the interrupt controller
 *   hwIRQEnterCritical hwIRQLeaveCritical
 *
 * `hw_pc.c` in the tree is a skeleton of the first two groups with every real
 * call commented out and `salAiGetDest` returning NULL, so nothing has ever
 * driven it.  It is excluded from the build (port/Makefile) rather than
 * patched, because a SAL that has to be honest about a 4-buffer DMA ring, a
 * deterministic tick and a software mixer shares almost no code with it.
 *
 * ---- the shape of a frame -------------------------------------------------
 *
 * The console's numbers, all confirmed in the source rather than assumed:
 *
 *   DMA_BUFFER_LEN 0x280 bytes = 640 = 160 frames of interleaved stereo s16
 *   32000 Hz, so one AI buffer is 5 ms and the AI interrupt fires at 200 Hz
 *   synthInfo.numSamples = 0x20 = 32 samples: the *sub*-frame
 *   5 sub-frames x 32 = 160, which is why snd_handle_irq runs the sequencer
 *     five times per interrupt and why volume ramps divide by 160
 *   four buffers in the ring; the DSP fills the one two slots ahead of the
 *     one the AI is playing, so output latency is 2 x 5 = 10 ms
 *
 * `salCtrlDsp(dest)` is the seam.  On the console it built a command list and
 * kicked the DSP; here it calls the CPU mixer (musyx_mix.c) synchronously,
 * which lands the work in the same place in the frame the DSP's kick was --
 * before the five sequencer passes, operating on the state the *previous*
 * interrupt left.  Nothing about MusyX's ordering has to change.
 *
 * ---- the tick, and why it is not the audio clock --------------------------
 *
 * `salCallback` on the console was an interrupt from the AI's own crystal.
 * Here it is driven from the retrace gate (port/src/platform/vi.c) by an
 * integer accumulator: 32000/59.94 samples of credit per retrace, one 160-
 * sample frame spent per credit of 160.  No wall clock, no audio-device
 * clock, no floating point -- so the number of sequencer steps between two
 * video frames is a fixed function of the frame number, `--seed 12345` keeps
 * reproducing, and `--dumpframe` md5s do not move when the machine is busy.
 * (§15.9 item 5's warning is the other half of the same argument: the mixer
 * is on the game thread where --perf can see it, so it must also be cheap.)
 */
#include "port.h"

#include <dolphin/ai.h>

#include <stdlib.h>
#include <string.h>

#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/sal.h"
#include "musyx/synth.h"

#include "musyx_mix.h"

/* Declared rather than #included: <dolphin/vi.h> drags in the whole VI
 * surface and all this needs is the frame counter the gate keeps. */
u32 VIGetRetraceCount(void);

#define DMA_BUFFER_LEN 0x280 /* bytes: 160 stereo s16 frames, 5 ms at 32 kHz */
#define DMA_BUFFERS 4
#define MIX_FRQ 32000u
#define FRAME_SAMPLES 160

static void ai_dma_tick(void); /* defined with AIRegisterDMACallback below */

/* The retrace rate the port paces to (vi.c), as a rational so the tick can be
 * exact: 5994 / 100 Hz. */
#define RETRACE_NUM 5994
#define RETRACE_DEN 100

static u8* ai_buffers;   /* DMA_BUFFERS * DMA_BUFFER_LEN */
static u8 ai_index;
static SND_SOME_CALLBACK user_callback;
static int ai_started;
static int sal_up;

/* the deterministic tick: sample credit in 1/RETRACE_NUM units */
static unsigned long long tick_credit;

static long first_sound_frame;      /* retrace of the first non-silent sample */
static double first_sound_second;
static unsigned long stat_frames;   /* 160-sample frames mixed */
static unsigned long stat_retraces; /* retraces that carried audio */

int port_audio_enabled = 1; /* cleared by --noaudio */

/* ---- the interrupt controller -------------------------------------------- */
/* There is nothing to disable.  The mixer, the sequencer and every `sal*`
 * entry point run on the game thread; the only other thread that touches any
 * of this is SDL's audio callback, and it touches the output ring alone,
 * which has its own lock inside audio_out_sdl.c.  So MusyX's critical
 * sections are genuinely empty here rather than lazily empty -- and they are
 * entered several times per 5 ms, so a mutex would be a real cost for no
 * mutual exclusion. */

static u16 irq_level;

void hwInitIrq(void) { irq_level = 1; }
void hwExitIrq(void) {}
void hwEnableIrq(void) {
    if (irq_level) {
        irq_level--;
    }
}
void hwDisableIrq(void) { irq_level++; }
void hwIRQEnterCritical(void) {}
void hwIRQLeaveCritical(void) {}

/* ---- the audio interface -------------------------------------------------- */

/* The DMA buffers belong to the *port*, not to the game's call that first
 * asked for them.  Two reasons, both about snapshots (PLAN.md 24.2): a
 * restored process never runs the game's audio init, so `salAiGetDest` would
 * hand MusyX `NULL + index * 0x280` -- which is precisely the SIGBUS at
 * 0x780 that the first restore died of -- and the snapshot registry holds the
 * buffer's address, so it must not be freed and re-allocated underneath it. */
/* Plain malloc, not salMalloc: `salHooks.malloc` is the game's own allocator
 * and is NULL until MusyX has been initialised, which in a restored process
 * never happens (the game's init ran in the process that took the snapshot). */
static void ai_buffers_ensure(void) {
    if (!ai_buffers) {
        ai_buffers = (u8*)malloc(DMA_BUFFER_LEN * DMA_BUFFERS);
        if (ai_buffers) {
            memset(ai_buffers, 0, DMA_BUFFER_LEN * DMA_BUFFERS);
        }
    }
}

bool salInitAi(SND_SOME_CALLBACK callback, u32 flags, u32* outFreq) {
    (void)flags;
    ai_buffers_ensure();
    if (!ai_buffers) {
        return FALSE;
    }
    memset(ai_buffers, 0, DMA_BUFFER_LEN * DMA_BUFFERS);
    ai_index = 0;
    user_callback = callback;
    tick_credit = 0;
    /* The two numbers MusyX reads straight back out of this call. */
    synthInfo.numSamples = 0x20;
    *outFreq = MIX_FRQ;
    port_log("port> MusyX SAL: %u Hz, %d x %d-byte AI buffers (%d ms), "
             "%u-sample sub-frame\n",
             MIX_FRQ, DMA_BUFFERS, DMA_BUFFER_LEN,
             DMA_BUFFERS * FRAME_SAMPLES * 1000 / (int)MIX_FRQ, (unsigned)synthInfo.numSamples);
    return TRUE;
}

bool salStartAi(void) {
    ai_started = 1;
    sal_up = 1;
    /* --mute silences the *voices*, not the path: every voice is still
     * started, decoded, advanced and retired, and the frame still costs what
     * it costs.  That is the point -- it makes "is the audio path what broke
     * this" a one-flag question without changing the game's timing. */
    port_musyx_mix_mute = port_opt.mute;
    port_musyx_mix_init();
    port_log("port> MusyX SAL: mixing started\n");
    return TRUE;
}

bool salExitAi(void) {
    ai_started = 0;
    sal_up = 0;
    /* Kept, not freed: 2.5 KB whose address is in the snapshot registry. */
    memset(ai_buffers, 0, DMA_BUFFER_LEN * DMA_BUFFERS);
    return TRUE;
}

/* Which buffer the DSP fills: two slots ahead of the one being played, so the
 * ring always has a finished buffer between the writer and the reader. */
void* salAiGetDest(void) {
    if (!ai_buffers) {
        return NULL;
    }
    return ai_buffers + ((ai_index + 2) % DMA_BUFFERS) * DMA_BUFFER_LEN;
}

/* ---- the DSP -------------------------------------------------------------- */

bool salInitDsp(u32 flags) {
    (void)flags;
    return TRUE;
}

bool salExitDsp(void) { return TRUE; }

/* The seam.  On the console: build the command list, mail it to dspSlave.
 * Here: render the frame. */
void salCtrlDsp(s16* dest) {
    if (!dest) {
        return;
    }
    port_perf_audio_begin();
    ai_dma_tick();
    port_musyx_mix_frame(dest);
    port_perf_audio_end();
    stat_frames++;
    /* --audiolog: one line per second of mixed audio.  The peak and the ring
     * fill are the two numbers that separate the three ways this can be
     * wrong -- nothing is being mixed, something is being mixed but the ring
     * is starving, or both are fine and the fault is downstream. */
    if (port_opt.audiolog && (stat_frames % 200) == 0) {
        int i;
        unsigned peak = 0;
        for (i = 0; i < FRAME_SAMPLES * 2; i++) {
            unsigned a = (unsigned)(dest[i] < 0 ? -dest[i] : dest[i]);
            if (a > peak) {
                peak = a;
            }
        }
        port_log("audio> %6.2f s mixed at retrace %6lu: peak %5u, ring %5u/%u bytes\n",
                 stat_frames * (double)FRAME_SAMPLES / MIX_FRQ,
                 (unsigned long)VIGetRetraceCount(), peak, port_audio_out_queued(),
                 262144u);
    }
    /* "Does the title music start at the right frame" is the one question a
     * --wav capture cannot answer on its own, because the WAV has no frame
     * numbers in it.  So the first frame that carries any signal at all is
     * recorded here, against the retrace counter, and printed next to
     * Dolphin's own figure in the report. */
    if (!first_sound_frame) {
        int i;
        for (i = 0; i < FRAME_SAMPLES * 2; i++) {
            if (dest[i]) {
                first_sound_frame = (long)VIGetRetraceCount() + 1;
                first_sound_second = stat_frames * (double)FRAME_SAMPLES / MIX_FRQ;
                port_log("port> audio: first non-silent sample at retrace %ld "
                         "(%.2f s of mixed audio)\n",
                         first_sound_frame, first_sound_second);
                break;
            }
        }
    }
}

/* `salBuildCommandList` and `salStartDsp` live in hw_dspctrl.c and compile to
 * nothing on this target (their bodies are Dolphin-guarded).  Nothing calls
 * them now that salCtrlDsp does not. */

/* ---- the tick ------------------------------------------------------------- */

/* One simulated AI interrupt: retire the buffer the hardware would now have
 * finished playing, hand it to the output ring, and let MusyX fill the next
 * one but two. */
static void ai_interrupt(void) {
    ai_index = (u8)((ai_index + 1) % DMA_BUFFERS);
    port_audio_out_queue(ai_buffers + ai_index * DMA_BUFFER_LEN, DMA_BUFFER_LEN);
    if (user_callback) {
        user_callback(); /* snd_handle_irq: salCtrlDsp, aux, 5 x seq+synth */
    }
}

/* Called once per retrace from VIWaitForRetrace, before the game runs.
 *
 * Credit is accumulated in units of 1/RETRACE_NUM of a sample so the division
 * is exact and never drifts: each retrace is worth MIX_FRQ * RETRACE_DEN
 * units, and one 160-sample frame costs FRAME_SAMPLES * RETRACE_NUM.  At
 * 59.94 Hz that is 3.3367 frames a retrace, delivered as a fixed 3-3-4-3-3-4
 * pattern rather than a jittery one. */
void port_audio_tick(void) {
    const unsigned long long per_retrace = (unsigned long long)MIX_FRQ * RETRACE_DEN;
    const unsigned long long per_frame = (unsigned long long)FRAME_SAMPLES * RETRACE_NUM;
    int fired = 0;

    if (!sal_up || !ai_started || !port_audio_enabled) {
        return;
    }
    tick_credit += per_retrace;
    /* A guard, not a policy: if the game thread ever stalled long enough to
     * bank a second of audio the catch-up would be worse than the gap. */
    if (tick_credit > per_frame * 64) {
        tick_credit = per_frame * 64;
    }
    while (tick_credit >= per_frame) {
        tick_credit -= per_frame;
        ai_interrupt();
        fired++;
    }
    if (fired) {
        stat_retraces++;
    }
}

void port_audio_shutdown(void) {
    if (sal_up) {
        port_musyx_mix_shutdown();
        sal_up = 0;
    }
    port_audio_wav_finish();
    port_audio_out_shutdown();
}

void port_audio_report(void) {
    if (!stat_frames) {
        return;
    }
    port_log("\nport> MusyX: %lu frames of 160 samples mixed (%.2f s of audio) "
             "over %lu retraces\n",
             stat_frames, stat_frames * (double)FRAME_SAMPLES / MIX_FRQ, stat_retraces);
    if (first_sound_frame) {
        port_log("port> audio: first sound at retrace %ld; Dolphin's own DSP dump of "
                 "the same boot starts at 10.95 s = retrace ~656\n",
                 first_sound_frame);
    } else {
        port_log("port> audio: NOTHING was ever mixed above silence\n");
    }
    port_musyx_mix_report();
    port_musyx_aram_report();
    port_audio_out_report();
}

/* ---- AI, the SDK side ----------------------------------------------------- */
/* The game's own `HuAudStreamVolSet` / `HuAudStreamPauseOn` call these; they
 * are the *AI streaming* path (DVD audio played straight through the AI),
 * which Mario Party 4 does not use -- `HuAudStreamPlay` returns 0 without
 * doing anything.  They are defined here rather than left as generated stubs
 * so the stub report stops listing them and so a future THP soundtrack has an
 * obvious place to land. */

static u8 stream_vol_l, stream_vol_r;
static u32 stream_play;

/* ---- the AI DMA callback, and the three services that hang off it ----------
 *
 * This was a generated stub until M9b, and that is the m444 stall (PLAN.md
 * 22.4).  On the console `AIRegisterDMACallback` installs a function the AI
 * runs every time it finishes a DMA buffer -- 0x280 bytes, i.e. exactly one
 * 160-sample DSP frame -- and MusyX puts its own `salCallback` there.  The
 * *game* then chains itself in front of it (`msmSysInit` ->
 * `AIRegisterDMACallback(msmSysServer)`, src/msm/msmsys.c:887) and every
 * third callback runs the three periodic services:
 *
 *     msmMusPeriodicProc();      sequence fades, music state
 *     msmSePeriodicProc();       *frees finished sound-effect players*
 *     msmStreamPeriodicProc();   stream state and refill
 *
 * With the registration stubbed out, `msmSysServer` was installed into
 * nothing and none of the three ever ran.  The one that shows is the middle
 * one: a sound-effect player slot only returns to `status == 0` in
 * `msmSePeriodicProc` (src/msm/msmse.c:160-171), so after the first `se.sfx`
 * effects of the boot, every `msmSePlay` for the rest of the run returned
 * MSM_ERR_CHANLIMIT.  That is the 65,180 `SE Entry Error<... -110>` lines in
 * the soak log, the first of them at line 363, twelve hours before the stall
 * -- and it is why a screen that waits for a sound it started can wait for
 * ever.
 *
 * The cadence here is the console's: one call per DSP frame, from the same
 * place the frame is rendered and therefore on the game thread, which is what
 * the rest of this port does with MusyX for determinism.  It runs *before*
 * the mix because on the console the game's handler ran before the MusyX one
 * it chained to.  `--noaicb` puts the old behaviour back for an A/B. */
static AIDCallback ai_dma_cb;

/* Never NULL: the game calls whatever this returned as `sys.oldAIDCallback`
 * without checking, because on the console MusyX's own callback was always
 * already installed. */
static void ai_dma_none(void) {}

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    AIDCallback old = ai_dma_cb != NULL ? ai_dma_cb : ai_dma_none;
    ai_dma_cb = callback;
    port_log("port> AI: DMA callback %s (one call per %d-sample DSP frame)\n",
             callback != NULL ? "registered" : "cleared", FRAME_SAMPLES);
    return old;
}

static void ai_dma_tick(void) {
    if (ai_dma_cb != NULL && !port_opt.noaicb) {
        ai_dma_cb();
    }
}

void AIInit(u8* stack) { (void)stack; }
void AIInitDMA(u32 addr, u32 len) {
    (void)addr;
    (void)len;
}
void AIStartDMA(void) {}
void AIStopDMA(void) {}
u32 AIGetDMAStartAddr(void) { return 0; }
void AISetStreamVolLeft(u8 vol) { stream_vol_l = vol; }
void AISetStreamVolRight(u8 vol) { stream_vol_r = vol; }
u8 AIGetStreamVolLeft(void) { return stream_vol_l; }
u8 AIGetStreamVolRight(void) { return stream_vol_r; }
void AISetStreamPlayState(u32 state) { stream_play = state; }
u32 AIGetStreamPlayState(void) { return stream_play; }

/* ---- snapshots ------------------------------------------------------------
 * The mix is part of the determinism contract (PLAN.md §22.5): MusyX runs on
 * the game thread, the game reads voice, stream and channel state back, and
 * the DSP frame cadence is a function of the retrace count through
 * `tick_credit`.  So the SAL's cursor comes back with the snapshot.  MusyX's
 * own state is not here at all -- it is in the MusyX objects' globals and in
 * ARAM, both of which the snapshot carries wholesale.
 *
 * `ai_buffers` is host memory allocated at boot, so what travels is its
 * *contents*, restored into whatever address this process allocated. */
void musyx_sal_snap_register(void) {
    ai_buffers_ensure();
    port_snap_register("musyx.ai_index", &ai_index, sizeof(ai_index));
    port_snap_register("musyx.ai_started", &ai_started, sizeof(ai_started));
    port_snap_register("musyx.sal_up", &sal_up, sizeof(sal_up));
    port_snap_register("musyx.tick_credit", &tick_credit, sizeof(tick_credit));
    port_snap_register("musyx.user_callback", &user_callback, sizeof(user_callback));
    port_snap_register("musyx.ai_dma_cb", &ai_dma_cb, sizeof(ai_dma_cb));
    port_snap_register("musyx.irq_level", &irq_level, sizeof(irq_level));
    port_snap_register("musyx.stream_vol_l", &stream_vol_l, sizeof(stream_vol_l));
    port_snap_register("musyx.stream_vol_r", &stream_vol_r, sizeof(stream_vol_r));
    port_snap_register("musyx.stream_play", &stream_play, sizeof(stream_play));
    port_snap_register("musyx.stat_frames", &stat_frames, sizeof(stat_frames));
    port_snap_register("musyx.stat_retraces", &stat_retraces, sizeof(stat_retraces));
    if (ai_buffers) {
        port_snap_register("musyx.ai_buffers", ai_buffers,
                           (unsigned long)DMA_BUFFERS * DMA_BUFFER_LEN);
    }
}
