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

bool salInitAi(SND_SOME_CALLBACK callback, u32 flags, u32* outFreq) {
    (void)flags;
    ai_buffers = (u8*)salMalloc(DMA_BUFFER_LEN * DMA_BUFFERS);
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
    salFree(ai_buffers);
    ai_buffers = NULL;
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
                 65536u);
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
                 "the same boot starts at 10.4 s = retrace ~623\n",
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
