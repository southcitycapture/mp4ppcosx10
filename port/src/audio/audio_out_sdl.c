/* The audio device: an SDL callback thread that only ever drains a ring.
 *
 * The split here is the one the Snowboard Kids ports settled on and the one
 * §15.9 item 5 asks for, but it is worth writing down *why* it is this way
 * round, because the obvious reading of "mix on the audio thread" is wrong for
 * this game.
 *
 * MusyX's per-frame work is not just mixing.  `snd_handle_irq` (hardware.c)
 * runs the *sequencer* five times per 5 ms frame -- and the sequencer fires
 * the game's own callbacks, allocates voices, and reads state the game thread
 * is writing.  Running that on SDL's callback thread would mean the number of
 * sequencer steps a frame depends on the host's audio clock, which is the
 * definition of non-determinism: `--seed 12345 --play board-start.play` would
 * stop reproducing, and `--dumpframe` md5s would move whenever the machine was
 * busy.  So the *cadence* is driven from the port's own 60 Hz gate
 * (port_audio_tick, musyx_sal.c) on the game thread, exactly 32000/59.94
 * samples' worth per retrace, and the SDL callback is left with the one job
 * that genuinely belongs to it: copying bytes out of a ring and padding with
 * silence when the game did not keep up.
 *
 * That puts the mix in the frame budget, where --perf can see it.  M6's
 * done-means allows 1.5 ms a frame and asks for the number, which is only a
 * question you can ask if the work is on the measured thread.
 *
 * The ring's read and write cursors are monotonically increasing `Uint32`
 * byte counts, wrapped only at the indexing site, so `write - read` is the
 * fill level even across the counter's own wrap.  Producer touches only
 * `write`, consumer only `read`.  SDL 2.0.3 (the Tiger backport this G4 runs)
 * has no SDL_QueueAudio, hence the hand-rolled ring.
 */
#include "port.h"

#include <stdio.h>
#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#endif

/* 32 kHz stereo s16 = 128000 B/s.  64 KB is 512 ms, which sounds enormous --
 * but the producer is the *video* frame, and a G4 running this game at 14.6
 * fps delivers audio in 68 ms bursts of 3-4 DSP frames.  A ring shorter than
 * a few of those bursts underruns on every slow frame.  The latency this
 * costs is bounded by how far ahead the game gets, not by the ring's size. */
#define RING_BYTES (256 * 1024)
/* M17: 256 KB = 2 s.  Under --realtime the gate catches up through a backlog
 * of up to a second (framemode.c) and the mix for that second arrives in a
 * burst; 64 KB (512 ms) dropped 414 ms of it to overrun on the first walk. */

static Uint8 ring[RING_BYTES];
static volatile Uint32 ring_read, ring_write;

#ifndef PORT_NO_SDL
static SDL_AudioDeviceID dev;
#endif
static int opened;

/* stats, printed by port_audio_report */
static unsigned long stat_queued_bytes;
static unsigned long stat_underrun_bytes;
static unsigned long stat_underruns;
static unsigned long stat_overrun_bytes;
static unsigned long stat_callbacks;
static unsigned peak_l, peak_r;

/* --wav */
static FILE* wav;
static unsigned long wav_bytes;

int port_audio_mute;

#ifndef PORT_NO_SDL
static void audio_cb(void* user, Uint8* stream, int len) {
    Uint32 avail = ring_write - ring_read;
    Uint32 n = avail < (Uint32)len ? avail : (Uint32)len;
    Uint32 head, first;
    (void)user;
    stat_callbacks++;
    head = ring_read % RING_BYTES;
    first = RING_BYTES - head;
    if (first > n) {
        first = n;
    }
    memcpy(stream, ring + head, first);
    if (n > first) {
        memcpy(stream + first, ring, n - first);
    }
    ring_read += n;
    if (n < (Uint32)len) {
        /* Underrun.  Silence is the only honest answer -- repeating the last
         * buffer would be a different kind of wrong and much harder to hear
         * as a fault.  Counted, because "no clicks" is a claim M6 has to be
         * able to prove rather than assert. */
        memset(stream + n, 0, (size_t)len - n);
        stat_underrun_bytes += (unsigned long)len - n;
        stat_underruns++;
    }
}
#endif

int port_audio_out_init(void) {
#ifndef PORT_NO_SDL
    SDL_AudioSpec want, have;
    if (opened) {
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        port_log("port> audio: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return 0;
    }
    memset(&want, 0, sizeof(want));
    want.freq = 32000; /* MusyX's own rate; salInitAi reports it to the game */
    /* The G4 is big endian and so is every sample the mixer produces, but the
     * device wants whatever the machine's native order is -- AUDIO_S16SYS is
     * that on both the G4 and the little-endian development host, and the
     * mixer writes host-order s16, so there is no swap anywhere. */
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    /* 512 frames = 16 ms, one video frame.  Smaller wakes the callback more
     * often for no benefit; larger makes the underrun granularity coarse. */
    want.samples = 512;
    want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        port_log("port> audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(dev, 0);
    opened = 1;
    port_log("port> audio: %d Hz, %d ch, %d-frame buffer, %d KB ring (%.0f ms)\n", have.freq,
             have.channels, have.samples, RING_BYTES / 1024,
             1000.0 * RING_BYTES / (32000.0 * 4.0));
    return 1;
#else
    return 0;
#endif
}

/* The producer.  `bytes` of host-order interleaved stereo s16. */
void port_audio_out_queue(const void* samples, unsigned bytes) {
    Uint32 space, head, first;
    const Uint8* src = (const Uint8*)samples;

    if (wav) {
        port_audio_wav_write(samples, bytes);
    }
    if (!opened) {
        return;
    }
    {
        /* peak meter, for the --wav-less "is anything coming out" question */
        const short* s = (const short*)samples;
        unsigned n = bytes / 2, i;
        for (i = 0; i + 1 < n; i += 2) {
            unsigned al = (unsigned)(s[i] < 0 ? -s[i] : s[i]);
            unsigned ar = (unsigned)(s[i + 1] < 0 ? -s[i + 1] : s[i + 1]);
            if (al > peak_l) {
                peak_l = al;
            }
            if (ar > peak_r) {
                peak_r = ar;
            }
        }
    }
#ifndef PORT_NO_SDL
    SDL_LockAudioDevice(dev);
    space = RING_BYTES - (ring_write - ring_read);
    if (bytes > space) {
        stat_overrun_bytes += bytes - space;
        bytes = space; /* overrun: drop the tail rather than lap the reader */
    }
    head = ring_write % RING_BYTES;
    first = RING_BYTES - head;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(ring + head, src, first);
    if (bytes > first) {
        memcpy(ring, src + first, bytes - first);
    }
    ring_write += bytes;
    stat_queued_bytes += bytes;
    SDL_UnlockAudioDevice(dev);
#else
    (void)space;
    (void)head;
    (void)first;
    (void)src;
    stat_queued_bytes += bytes;
#endif
}

unsigned port_audio_out_queued(void) { return (unsigned)(ring_write - ring_read); }
int port_audio_out_opened(void) { return opened; }
unsigned long port_audio_out_underruns(void) { return stat_underruns; }

/* --audiolead (M17): under --realtime the producer is paced to real time, so
 * the ring's fill level is only ever what the game got ahead by -- and a
 * drawn frame that overruns by 30 ms drains it.  Queue that much silence
 * once, before pacing starts; from then on the mix rides `ms` ahead of the
 * device.  It is latency, not a change to the mix: --wav is written before
 * the ring and does not see it. */
void port_audio_out_prime(unsigned ms) {
#ifndef PORT_NO_SDL
    static Uint8 zeros[4096];
    unsigned bytes = ms * 128; /* 32 kHz stereo s16 = 128 B/ms */
    if (!opened) {
        return;
    }
    if (bytes > RING_BYTES / 2) {
        bytes = RING_BYTES / 2;
    }
    while (bytes > 0) {
        unsigned n = bytes < sizeof(zeros) ? bytes : (unsigned)sizeof(zeros);
        Uint32 space;
        SDL_LockAudioDevice(dev);
        space = RING_BYTES - (ring_write - ring_read);
        if (n > space) {
            n = space;
        }
        if (n) {
            Uint32 head = ring_write % RING_BYTES;
            Uint32 first = RING_BYTES - head;
            if (first > n) {
                first = n;
            }
            memcpy(ring + head, zeros, first);
            if (n > first) {
                memcpy(ring, zeros, n - first);
            }
            ring_write += n;
        }
        SDL_UnlockAudioDevice(dev);
        if (!n) {
            break;
        }
        bytes -= n;
    }
    port_log("port> audio: %u ms of lead queued ahead of the mix (--audiolead)\n", ms);
#else
    (void)ms;
#endif
}

void port_audio_out_shutdown(void) {
#ifndef PORT_NO_SDL
    if (opened) {
        SDL_PauseAudioDevice(dev, 1);
        SDL_CloseAudioDevice(dev);
        opened = 0;
    }
#endif
}

/* ---- --wav ---------------------------------------------------------------- */
/* Nobody can listen to the G4 over SSH, so the only way to judge "does the
 * title music start at the right frame and does it sound like music" is to
 * capture it and look at it.  The header is written at close, over the 44
 * bytes reserved at open. */

static void put_le32(FILE* f, unsigned long v) {
    unsigned char b[4];
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    fwrite(b, 1, 4, f);
}

int port_audio_wav_start(const char* path) {
    wav = fopen(path, "wb");
    if (!wav) {
        port_log("port> --wav: cannot create %s\n", path);
        return 0;
    }
    fseek(wav, 44, SEEK_SET);
    wav_bytes = 0;
    port_log("port> --wav: capturing 32 kHz stereo to %s\n", path);
    return 1;
}

void port_audio_wav_write(const void* samples, unsigned bytes) {
    if (!wav) {
        return;
    }
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    {
        /* WAV is little endian and the G4 is not. */
        const unsigned char* s = (const unsigned char*)samples;
        unsigned char buf[1024];
        unsigned i, k = 0;
        for (i = 0; i + 1 < bytes; i += 2) {
            buf[k++] = s[i + 1];
            buf[k++] = s[i];
            if (k == sizeof(buf)) {
                fwrite(buf, 1, k, wav);
                k = 0;
            }
        }
        if (k) {
            fwrite(buf, 1, k, wav);
        }
    }
#else
    fwrite(samples, 1, bytes, wav);
#endif
    wav_bytes += bytes & ~1u;
}

void port_audio_wav_finish(void) {
    if (!wav) {
        return;
    }
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    put_le32(wav, 36 + wav_bytes);
    fwrite("WAVEfmt ", 1, 8, wav);
    put_le32(wav, 16);
    fwrite("\x01\x00\x02\x00", 1, 4, wav); /* PCM, 2 channels */
    put_le32(wav, 32000);
    put_le32(wav, 32000 * 4);
    fwrite("\x04\x00\x10\x00", 1, 4, wav); /* block align 4, 16 bits */
    fwrite("data", 1, 4, wav);
    put_le32(wav, wav_bytes);
    fclose(wav);
    port_log("port> --wav: %lu bytes = %.2f s at 32 kHz stereo\n", wav_bytes,
             wav_bytes / (32000.0 * 4.0));
    wav = NULL;
}

void port_audio_out_report(void) {
    if (!stat_queued_bytes && !wav_bytes) {
        return;
    }
    port_log("port> audio out: %.2f s queued, %lu callbacks, peak L %u R %u (of 32767)\n",
             stat_queued_bytes / (32000.0 * 4.0), stat_callbacks, peak_l, peak_r);
    port_log("port> audio out: %lu underrun(s) totalling %.1f ms, %.1f ms dropped to overrun\n",
             stat_underruns, stat_underrun_bytes / 128.0, stat_overrun_bytes / 128.0);
    /* Say what those two numbers mean, because on this machine they are a
     * frame-rate reading rather than an audio fault.
     *
     * The tick is deliberately tied to the retrace (musyx_sal.c), so audio
     * advances at exactly the rate the *game* advances -- which is what keeps
     * `--seed` reproducing and what the console did too, since the console ran
     * at 60 fps.  The port does not: PLAN.md §15.5 measures 14.6 fps on the
     * board.  So the producer delivers audio at about a quarter of real time
     * and the device starves; under `--turbo` the retrace has no pacing at all
     * and the producer overruns instead.  Neither is a click in the mix.
     *
     * The `--wav` capture is unaffected either way -- it is written on the
     * producer side, before the ring -- so it is the artefact to judge the
     * audio by, and it is correct in *game* time.  Live playback becomes
     * correct when the frame rate does. */
    if (stat_underruns || stat_overrun_bytes) {
        port_log("port> audio out: (the ring tracks the frame rate: audio is paced by the\n"
                 "port>             retrace, so under 60 fps it starves and under --turbo it\n"
                 "port>             overruns.  --wav is written before the ring and is whole.)\n");
    }
}
