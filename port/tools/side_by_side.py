#!/usr/bin/env python3
"""side_by_side.py -- the G4's frames and Dolphin's, in step, as one raw 1280x480
stream (M51, PLAN.md 66.5): the left half the G4 (the port's retrace r), the
right half Dolphin's frame for the console's field the session's map puts r
on (tools/dolphin_sync.py), its 640x528 picture's active rows (17..510)
scaled to 480 -- or with --frames, the pairs as PPMs for ppmdiff.

  side_by_side.py --g4 G4.mkv --g4-index IDX --map MAP.json --dolphin AVI
                  --dolphin-fields FIELDS [--out RAW|-] [--frames R,R,... --ppmdir DIR]

FIELDS: one line a Dolphin frame, the console field (VCounter) it shows
(tools/dolphin_fields.py).
"""
import argparse, json, os, subprocess, sys

W, H = 640, 480
FFMPEG = os.environ.get('FFMPEG', os.path.expanduser('~/bin/ffmpeg'))


def decoder(path, w, h, vf=None):
    cmd = [FFMPEG, '-v', 'error', '-i', path]
    if vf:
        cmd += ['-vf', vf]
    cmd += ['-vsync', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}', '-']
    return subprocess.Popen(cmd, stdout=subprocess.PIPE)


def read_frame(p, size):
    b = p.stdout.read(size)
    return b if len(b) == size else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--g4', required=True)
    ap.add_argument('--g4-index', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--dolphin', required=True)
    ap.add_argument('--dolphin-fields', required=True)
    ap.add_argument('--out', default='-')
    ap.add_argument('--frames', help='only these retraces, as PPM pairs')
    ap.add_argument('--ppmdir')
    a = ap.parse_args()
    vmap = {int(k): v for k, v in json.load(open(a.map))['vmap'].items()}
    g4r = [int(l.split()[0]) for l in open(a.g4_index)]
    dfields = [int(l.split()[0]) for l in open(a.dolphin_fields)]
    want = set(int(x) for x in a.frames.split(',')) if a.frames else None
    out = None if want else (sys.stdout.buffer if a.out == '-' else open(a.out, 'wb'))
    g4 = decoder(a.g4, W, H)
    # Dolphin's 640x528: the picture is rows 17..510 (port/docs/reference-dolphin.md 4), scaled to 480
    dol = decoder(a.dolphin, W, H, vf=f'crop=640:494:0:17,scale=640:480:flags=bilinear')
    dj, dframe = -1, None
    n = 0
    for r in g4r:
        gf = read_frame(g4, W * H * 3)
        if gf is None:
            break
        v = vmap.get(r)
        # Dolphin's newest frame at or before field v
        while dj + 1 < len(dfields) and v is not None and dfields[dj + 1] <= v:
            nf = read_frame(dol, W * H * 3)
            if nf is None:
                break
            dframe = nf
            dj += 1
        dpic = dframe if dframe else bytes(W * H * 3)
        if want is not None:
            if r in want:
                for tag, pic in (('g4', gf), ('dolphin', dpic)):
                    with open(os.path.join(a.ppmdir, f'{tag}-r{r}.ppm'), 'wb') as f:
                        f.write(f'P6\n{W} {H}\n255\n'.encode())
                        f.write(pic)
                n += 1
            continue
        row = W * 3
        line = bytearray()
        for y in range(H):
            line += gf[y * row:(y + 1) * row]
            line += dpic[y * row:(y + 1) * row]
        out.write(line)
        n += 1
    if out:
        out.flush()
    print(f'side_by_side: {n} frames', file=sys.stderr)


if __name__ == '__main__':
    main()
