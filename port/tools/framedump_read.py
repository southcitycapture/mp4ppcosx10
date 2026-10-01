#!/usr/bin/env python3
"""framedump_read.py -- the G4 video's frames (--framedump, src/debug/framedump.c)
as raw RGB for ffmpeg, one frame a retrace (M51, PLAN.md 66.5).

  framedump_read.py IN|- [--out RAW|-] [--from R] [--to R] [--index IDX.txt]
                       [--scale WxH]

IN is the MP4FD2 file, or '-' for a stream (`ssh g4 tail -c +1 -f FILE`): the
reader stops at the END! record.  The output has one 640x480 RGB frame per
retrace from --from (the first frame's, by default) to --to (the last's):
a retrace the game presented nothing on repeats the frame before it -- the
console's XFB shown again -- so the video runs at the game's 59.94 frames a
second and the --wavdump's audio, 32,000 samples a second from retrace 0,
lines up with it.  --index writes the retrace each output frame shows.
"""
import argparse, struct, sys, zlib


def frames(f):
    magic = f.read(7)
    if magic not in (b'MP4FD2\n', b'MP4FD1\n'):
        sys.exit(f'not a frame dump: {magic!r}')
    v2 = magic == b'MP4FD2\n'
    while True:
        tag = f.read(4)
        if len(tag) < 4 or tag == b'END!':
            return
        if v2:
            hdr = f.read(16)
            frame, retrace, w, h, n = struct.unpack('>IIHHI', hdr)
        else:
            hdr = f.read(12)
            frame, w, h, n = struct.unpack('>IHHI', hdr)
            retrace = frame
        data = b''
        while len(data) < n:
            chunk = f.read(n - len(data))
            if not chunk:
                return
            data += chunk
        yield frame, retrace, w, h, zlib.decompress(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp')
    ap.add_argument('--out', default='-')
    ap.add_argument('--from', dest='frm', type=int)
    ap.add_argument('--to', type=int)
    ap.add_argument('--index')
    a = ap.parse_args()
    f = sys.stdin.buffer if a.inp == '-' else open(a.inp, 'rb')
    out = sys.stdout.buffer if a.out == '-' else open(a.out, 'wb')
    idx = open(a.index, 'w') if a.index else None
    last = None
    cur_r = a.frm
    n = 0
    for frame, r, w, h, rgb in frames(f):
        if a.to is not None and r > a.to:
            break
        if cur_r is None:
            cur_r = r
        if r < cur_r:
            last = rgb
            continue
        while cur_r < r and last is not None:
            out.write(last)
            if idx:
                idx.write(f'{cur_r} {cur_r - 1}\n')
            cur_r += 1
            n += 1
        out.write(rgb)
        if idx:
            idx.write(f'{r} {r}\n')
        last = rgb
        cur_r = r + 1
        n += 1
    if a.to is not None and last is not None:
        while cur_r <= a.to:
            out.write(last)
            cur_r += 1
            n += 1
    out.flush()
    print(f'framedump_read: {n} frames out', file=sys.stderr)


if __name__ == '__main__':
    main()
