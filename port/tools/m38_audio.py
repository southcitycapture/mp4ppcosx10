#!/usr/bin/env python3
"""M38 (PLAN.md 53.5): is the movie's sound in the port's output, sample for
sample, at the level THPSimple mixes it?

  m38_audio.py PORT.wav MOVIE.raw

PORT.wav is the port's --wav (32 kHz s16 stereo, what the AI played);
MOVIE.raw is the THP's own audio track decoded by something else (ffmpeg's
adpcm_thp: `ffmpeg -i X.thp -vn -f s16le -ac 2 -ar 32000 MOVIE.raw`).  The
movie is found in the port's output by cross-correlating 1/32-decimated
envelopes, the offset refined to the sample, and then over the whole movie:
the least-squares gain (THPSimple's VolumeTable at the movie's volume), the
residual's level against the signal's (what is not the movie: the game's
own sound, rounding), and the same with the two channels swapped (which
says the channel order is right, or that it is not).
"""
import math
import struct
import sys


def read_wav(path):
    d = open(path, 'rb').read()
    i = 12
    fmt = None
    while i < len(d):
        cid, n = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[i + 8:i + 24])
        if cid == b'data':
            return fmt, d[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    raise SystemExit('no data chunk')


def main():
    fmt, pd = read_wav(sys.argv[1])
    md = open(sys.argv[2], 'rb').read()
    assert fmt[1] == 2 and fmt[2] == 32000, fmt
    p = struct.unpack('<%dh' % (len(pd) // 2), pd)
    m = struct.unpack('<%dh' % (len(md) // 2), md)
    pl, pr = p[0::2], p[1::2]
    ml, mr = m[0::2], m[1::2]
    D = 64

    def env(x):
        return [sum(abs(v) for v in x[k:k + D]) for k in range(0, len(x) - D, D)]
    pe = env(pl)
    me = env(ml)[:20 * 32000 // D]  # the movie's first 20 s
    mm = sum(me) / len(me)
    mc = [v - mm for v in me]
    mn = math.sqrt(sum(v * v for v in mc)) or 1
    # Pearson correlation of the envelopes, over the port's first 40 s: the
    # loudest stretch is not the movie, the best-shaped one is
    best, bo = -2.0, 0
    for o in range(0, min(len(pe) - len(me), 40 * 32000 // D)):
        seg = pe[o:o + len(me)]
        sm = sum(seg) / len(seg)
        num = sum((a - sm) * b for a, b in zip(seg, mc))
        den = math.sqrt(sum((a - sm) ** 2 for a in seg)) * mn or 1
        r = num / den
        if r > best:
            best, bo = r, o
    best_r = best
    coarse = bo * D
    # refine to the sample on 20,000 samples a second in
    W0, WN = 32000, 20000
    best, off = -1e30, coarse
    for o in range(coarse - 2 * D, coarse + 2 * D):
        if o + W0 + WN > len(pl) or o < 0:
            continue
        s = 0
        for k in range(W0, W0 + WN):
            s += pl[o + k] * ml[k]
        if s > best:
            best, off = s, o
    n = min(len(ml), len(pl) - off)

    def fit(a, b):  # port a, movie b: gain, residual/signal in dB
        sab = sum(a[off + k] * b[k] for k in range(n))
        sbb = sum(b[k] * b[k] for k in range(n)) or 1
        g = sab / sbb
        res = sum((a[off + k] - g * b[k]) ** 2 for k in range(n))
        sig = sum((g * b[k]) ** 2 for k in range(n)) or 1
        return g, 10 * math.log10(res / sig) if res > 0 else -999.0

    gl, rl = fit(pl, ml)
    gr, rr = fit(pr, mr)
    gx, rx = fit(pl, mr)
    print('movie found at sample %d of the port\'s output (%.3f s; envelope correlation %.3f), '
          '%d samples (%.2f s) compared' % (off, off / 32000.0, best_r, n, n / 32000.0))
    print('left:  gain %.4f, residual %.1f dB below the movie' % (gl, -rl))
    print('right: gain %.4f, residual %.1f dB below the movie' % (gr, -rr))
    print('swapped (port left vs movie right): gain %.4f, residual %.1f dB' % (gx, -rx))


if __name__ == '__main__':
    main()
