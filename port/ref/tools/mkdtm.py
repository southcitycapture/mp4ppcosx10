#!/usr/bin/env python3
"""
mkdtm.py -- author a Dolphin .dtm input movie from a plain-text script.

The .dtm format is a 256-byte packed DTMHeader followed by one packed 8-byte
Movie::ControllerState record per *input poll* (not per video frame).
See port/docs/reference-dolphin.md for the field-by-field layout.

Script syntax (one directive per line, '#' starts a comment):

    frames <n>              # advance <n> polls with the current held state
    press  <btn>[,<btn>...] # hold for 1 poll, then release  (shorthand)
    tap    <btn> <n>        # hold <btn> for <n> polls, then release
    hold   <btn>[,<btn>...] # start holding
    release <btn>[,...]     # stop holding ('release all' clears everything)
    stick  <x> <y>          # main stick, 0..255 (128 = centre)
    cstick <x> <y>
    trigL  <v>              # 0..255
    trigR  <v>
    mark   <label>          # record "poll N = <label>" in the sidecar .marks

Buttons: A B X Y Z START L R UP DOWN LEFT RIGHT

Usage:  mkdtm.py script.txt out.dtm [--gameid GMPE01] [--rtc 1041472800]
"""
import argparse, struct, sys, os

BTN_BITS = {  # (byte index, bit index) inside the 8-byte ControllerState
    'START': (0, 0), 'A': (0, 1), 'B': (0, 2), 'X': (0, 3), 'Y': (0, 4), 'Z': (0, 5),
    'UP': (0, 6), 'DOWN': (0, 7),
    'LEFT': (1, 0), 'RIGHT': (1, 1), 'L': (1, 2), 'R': (1, 3),
    'DISC': (1, 4), 'RESET': (1, 5), 'CONNECTED': (1, 6), 'GET_ORIGIN': (1, 7),
}


class Pad:
    def __init__(self):
        self.held = set()
        self.sx = self.sy = self.cx = self.cy = 128
        self.tl = self.tr = 0

    def record(self):
        b0 = b1 = 0
        for name in self.held:
            i, bit = BTN_BITS[name]
            if i == 0:
                b0 |= 1 << bit
            else:
                b1 |= 1 << bit
        b1 |= 1 << BTN_BITS['CONNECTED'][1]  # is_connected always set
        return bytes((b0, b1, self.tl, self.tr, self.sx, self.sy, self.cx, self.cy))


def build_header(game_id, n_polls, n_frames, rtc, author=b'mp4-ref-rig',
                 video_backend=b'Vulkan', audio=b'HLE'):
    h = bytearray(256)
    h[0:4] = b'DTM\x1a'
    h[4:10] = game_id.encode().ljust(6, b'\0')[:6]
    h[10] = 0                       # bWii
    h[11] = 0x01                    # controllers: GC port 1 only
    h[12] = 0                       # bFromSaveState
    struct.pack_into('<Q', h, 13, n_frames)     # frameCount (video frames)
    struct.pack_into('<Q', h, 21, n_polls)      # inputCount
    struct.pack_into('<Q', h, 29, 0)            # lagCount
    struct.pack_into('<Q', h, 37, 0)            # uniqueID
    struct.pack_into('<I', h, 45, 0)            # numRerecords
    h[49:81] = author.ljust(32, b'\0')[:32]
    h[81:97] = video_backend.ljust(16, b'\0')[:16]
    h[97:113] = audio.ljust(16, b'\0')[:16]
    h[113:129] = b'\0' * 16                     # md5 of iso: 0 = "don't check"
    struct.pack_into('<Q', h, 129, rtc)         # recordingStartTime -> forced RTC
    h[137] = 1   # bSaveConfig: apply the flags below on playback
    h[138] = 0   # bSkipIdle
    h[139] = 0   # bDualCore  (single core = deterministic)
    h[140] = 0   # bProgressive
    h[141] = 1   # bDSPHLE
    h[142] = 0   # bFastDiscSpeed
    h[143] = 1   # CPUCore = PowerPC::CPUCore::JIT64/JITARM64
    h[144] = 1   # bEFBAccessEnable
    h[145] = 1   # bEFBCopyEnable
    h[146] = 1   # bSkipEFBCopyToRam (EFB->texture)
    h[147] = 0   # bEFBCopyCacheEnable
    h[148] = 0   # bEFBEmulateFormatChanges
    h[149] = 0   # bImmediateXFB
    h[150] = 1   # bSkipXFBCopyToRam
    h[151] = 0   # memcards: none inserted
    h[152] = 0   # bClearSave
    h[153] = 0   # bongos
    h[154] = 0   # bSyncGPU
    h[155] = 0   # bNetPlay
    h[156] = 0   # bPAL60
    h[157] = 0   # language = English
    h[159] = 0   # bFollowBranch
    h[160] = 1   # bUseFMA
    h[161] = 0   # GBAControllers
    h[162] = 0   # bWidescreen
    h[163] = 0   # countryCode
    # 169 discChange, 209 revision, 229/233 DSP hashes, 237 tickCount: left zero
    return bytes(h)


def compile_script(path):
    pad, out, marks, poll = Pad(), bytearray(), [], 0

    def emit(n):
        nonlocal poll
        rec = pad.record()
        out.extend(rec * n)
        poll += n

    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            op, args = parts[0].lower(), parts[1:]
            try:
                if op == 'frames':
                    emit(int(args[0]))
                elif op == 'mark':
                    marks.append((poll, ' '.join(args)))
                elif op in ('hold', 'release', 'press', 'tap'):
                    if op == 'release' and args[0].lower() == 'all':
                        pad.held.clear()
                        continue
                    names = [b.upper() for b in args[0].split(',')]
                    for b in names:
                        if b not in BTN_BITS:
                            raise ValueError(f'unknown button {b}')
                    if op == 'hold':
                        pad.held.update(names)
                    elif op == 'release':
                        pad.held.difference_update(names)
                    else:  # press / tap
                        n = int(args[1]) if op == 'tap' else 1
                        pad.held.update(names)
                        emit(n)
                        pad.held.difference_update(names)
                elif op == 'stick':
                    pad.sx, pad.sy = int(args[0]), int(args[1])
                elif op == 'cstick':
                    pad.cx, pad.cy = int(args[0]), int(args[1])
                elif op == 'trigl':
                    pad.tl = int(args[0])
                elif op == 'trigr':
                    pad.tr = int(args[0])
                else:
                    raise ValueError(f'unknown directive {op!r}')
            except Exception as e:
                sys.exit(f'{path}:{lineno}: {e}')
    return bytes(out), poll, marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('script')
    ap.add_argument('out')
    ap.add_argument('--gameid', default='GMPE01')
    ap.add_argument('--rtc', type=int, default=1041472800,
                    help='forced RTC, seconds since 1970 (default 2003-01-02 00:00:00 UTC)')
    a = ap.parse_args()

    body, polls, marks = compile_script(a.script)
    # MP4 polls the pad exactly once per VI frame, so frameCount == inputCount.
    hdr = build_header(a.gameid, polls, polls, a.rtc)
    with open(a.out, 'wb') as f:
        f.write(hdr)
        f.write(body)
    with open(a.out + '.marks', 'w') as f:
        for p, label in marks:
            f.write(f'{p}\t{label}\n')
    print(f'{a.out}: {polls} polls, {os.path.getsize(a.out)} bytes, {len(marks)} marks')


if __name__ == '__main__':
    main()
