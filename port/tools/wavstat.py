#!/usr/bin/env python3
"""Describe a WAV in the terms an audio claim can actually be judged by.

Nobody can listen to the Power Mac G4 over SSH, so `--wav` is the only
evidence M6 has, and "it sounds right" is not a thing this repository can
assert.  What it can assert is: when the first sound arrives, how loud it is,
whether the level is plausible rather than clipped or nearly silent, whether
there is a discontinuity where a click would be, and where the energy sits in
the spectrum.  Those are the five things this prints.

    port/tools/wavstat.py FILE [FILE ...]          describe each
    port/tools/wavstat.py --compare A B            and line the two up

The intended B is Dolphin's own `Dump/Audio/*_dspdump.wav`, captured with
`DumpAudio = True` under `[DSP]` in the pinned user directory -- Dolphin's DSP
HLE writes the same 32 kHz stereo mix MusyX asks the hardware for, so it is a
like-for-like reference for the port's own mix rather than an approximation.
(Its companion `*_dtkdump.wav` is the AI streaming path, and for Mario Party 4
it is silent from end to end, which is itself a useful fact: the game puts
everything, music included, through MusyX.)

The spectrum is a plain Goertzel over a handful of octave-spaced bins rather
than an FFT, because the question is "is there broadband musical content here
or a single stuck tone" and that does not need resolution -- and because this
has to run with nothing but the standard library.
"""
import array
import math
import sys
import wave


def load(path):
    w = wave.open(path, "rb")
    ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if sw != 2:
        raise SystemExit("%s: only 16-bit WAVs, got %d-bit" % (path, sw * 8))
    a = array.array("h")
    a.frombytes(w.readframes(n))
    if sys.byteorder == "big":
        a.byteswap()  # WAV is little-endian; this script may run on the G4 too
    w.close()
    return ch, sr, n, a


def goertzel(samples, sr, freq):
    """Energy at one frequency, without an FFT."""
    n = len(samples)
    if n == 0:
        return 0.0
    k = 2.0 * math.cos(2.0 * math.pi * freq / sr)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - k * s1 * s2, 0.0)) / n


def describe(path, seconds_per_row=1.0, max_rows=40):
    ch, sr, n, a = load(path)
    dur = n / float(sr)
    print("%s" % path)
    print("  %d ch, %d Hz, %d frames = %.2f s" % (ch, sr, n, dur))

    left = a[0::ch]
    right = a[1::ch] if ch > 1 else left

    peak = max((abs(x) for x in left), default=0)
    peak_r = max((abs(x) for x in right), default=0)
    rms = math.sqrt(sum(float(x) * x for x in left) / len(left)) if left else 0.0
    print("  peak L %d  R %d  (of 32767, %.1f dBFS)  rms L %.0f"
          % (peak, peak_r, 20 * math.log10(peak / 32767.0) if peak else -999.0, rms))

    # First sound: the first frame whose |sample| clears a floor that a real
    # mix clears immediately and dither does not.
    first = None
    for i, x in enumerate(left):
        if abs(x) > 64:
            first = i
            break
    if first is None:
        print("  first sound: NONE -- the file is silent")
    else:
        print("  first sound: sample %d = %.3f s (retrace ~%d at 59.94 Hz)"
              % (first, first / float(sr), int(first / float(sr) * 59.94)))

    # Clicks: a click is a sample-to-sample step far larger than anything the
    # signal's own slope produces.  Report the worst step and how many exceed
    # half of full scale, which no musical waveform at this rate does.
    worst = 0
    big = 0
    prev = left[0] if left else 0
    for x in left:
        d = abs(x - prev)
        if d > worst:
            worst = d
        if d > 16384:
            big += 1
        prev = x
    print("  worst sample-to-sample step %d; %d step(s) over half full scale"
          % (worst, big))

    # Spectrum over the loudest second, so a long silent lead-in does not
    # flatten it.
    best_off, best_e = 0, -1.0
    for off in range(0, max(n - sr, 1), sr):
        seg = left[off:off + sr]
        e = sum(float(x) * x for x in seg)
        if e > best_e:
            best_e, best_off = e, off
    seg = left[best_off:best_off + sr]
    if seg:
        print("  spectrum of the loudest second (at %.1f s):" % (best_off / float(sr)))
        bins = [62, 125, 250, 500, 1000, 2000, 4000, 8000]
        mags = [goertzel(seg, sr, f) for f in bins]
        top = max(mags) or 1.0
        for f, m in zip(bins, mags):
            bar = "#" * int(40 * m / top)
            print("    %5d Hz %6.1f %s" % (f, m, bar))

    rows = min(int(dur / seconds_per_row), max_rows)
    if rows > 1:
        print("  per second (peakL peakR rmsL):")
        for s in range(rows):
            lo = int(s * seconds_per_row * sr)
            hi = int((s + 1) * seconds_per_row * sr)
            l, r = left[lo:hi], right[lo:hi]
            if not l:
                break
            pl = max(abs(x) for x in l)
            pr = max(abs(x) for x in r)
            rr = math.sqrt(sum(float(x) * x for x in l) / len(l))
            print("    %4d %6d %6d %6.0f" % (s, pl, pr, rr))
    print()
    return dict(sr=sr, dur=dur, peak=peak, rms=rms, first=first)


def main(argv):
    if len(argv) >= 4 and argv[1] == "--compare":
        a = describe(argv[2])
        b = describe(argv[3])
        print("---- comparison ----")
        print("  duration   %.2f s vs %.2f s" % (a["dur"], b["dur"]))
        print("  peak       %d vs %d  (%.2f x)"
              % (a["peak"], b["peak"], a["peak"] / float(b["peak"] or 1)))
        print("  rms        %.0f vs %.0f  (%.2f x)"
              % (a["rms"], b["rms"], a["rms"] / (b["rms"] or 1.0)))
        if a["first"] is not None and b["first"] is not None:
            print("  first snd  %.2f s vs %.2f s  (%+.2f s)"
                  % (a["first"] / float(a["sr"]), b["first"] / float(b["sr"]),
                     a["first"] / float(a["sr"]) - b["first"] / float(b["sr"])))
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    for p in argv[1:]:
        describe(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
