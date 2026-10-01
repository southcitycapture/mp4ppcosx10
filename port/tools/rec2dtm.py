#!/usr/bin/env python3
"""rec2dtm.py -- a session recording (MP4REC, src/debug/session.c) as a Dolphin
input movie (.dtm) plus the Gecko codes the console needs, so that Dolphin,
started with the same settings, plays the session (M51, PLAN.md 66).

  rec2dtm.py REC OUT.dtm [--gecko OUT.ini] [--map MAP.json] [--until R]

What goes in:

 * the .dtm: the 256-byte header (the recording's RTC as recordingStartTime,
   the controllers the session had, a fresh memory card, single core, DSP HLE,
   JIT, the tick count far past the end -- Dolphin ends a movie the moment the
   emulated ticks pass the header's tickCount, which is why every .dtm this
   project wrote before M51 "did nothing"), then one 8-byte ControllerState
   per controller per SI poll: two polls a video field, and the game's PADRead
   at VCounter v reads poll 2v+53 (measured, PLAN.md 66.2).
 * the timeline: the port runs one retrace per game frame and loads off a
   disc image at once; the console waits on its disc (the main loop stalls,
   VCounter runs on) and the game's own async waits last longer.  So the port's
   retrace r goes to the console's field v(r) -- the map, found by
   tools/dolphin_sync.py by running Dolphin and aligning its trace of the game
   (GlobalCounter, the overlay, the three RNG states) on the recording's `s`
   lines.  A field no retrace maps to holds the last input (a stall: PADRead
   runs, the game does not).
 * the Gecko codes (GMPE01.ini): the board RNG's seeds -- BoardRandInit reads
   the console's clock, which the port cannot know, so a C2 hook at its store
   (0x8005FB08, `stw r4, boardRandSeed`) hands it the recording's seeds in
   order.  A recording with the self-play harness's writes in it (`w` lines:
   a marathon minigame, reached through the port's teleport) is not
   converted: the console would need those parks too (as Gecko writes held
   over their GlobalCounter spans, the M26b rig's way) -- rec2dtm says so and
   stops.  Sessions played from the boot carry none.
"""
import argparse, json, struct, sys

PAD_LEFT, PAD_RIGHT, PAD_DOWN, PAD_UP = 0x1, 0x2, 0x4, 0x8
PAD_Z, PAD_R, PAD_L = 0x10, 0x20, 0x40
PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START = 0x100, 0x200, 0x400, 0x800, 0x1000

import os
FAST_DISC = os.environ.get('MP4_DTM_FASTDISC', '0') == '1'
POLL_OF_V = 9 if FAST_DISC else 53  # PADRead at VCounter v reads poll 2v + 53 (2v + 9 with fast disc; PLAN.md 66.2)

REGION_ADDR = {'GWPlayerCfg': 0x8018FC10, 'GWPlayer': 0x8018FC38, 'GWSystem': 0x8018FCF8,
               'GWGameStat': 0x8018FDD8}
FRAND_SEED, RND_SEED, BOARD_SEED = 0x801D3D10, 0x801D342C, 0x801D3F14
GLOBALCOUNTER = 0x801D3A54
BOARDRANDINIT_STORE = 0x8005FB08
BOARDRANDINIT_STORE_INSN = 0x908D8AF4  # stw r4, boardRandSeed@sda21(r13)
MGNEXT = 0x801D4208  # mg_setup.c's static mgNext (s16): the board preloads from it (PLAN.md 41b.1)
GWSYSTEM_MG_NEXT_OFF = 0x34


def parse_rec(path):
    hdr, pads, sigs, boards, pokes, notes, areas = {}, [], [], [], [], [], []
    end = None
    body = False
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if not body:
            if line == 'start':
                body = True
                continue
            k, _, v = line.partition(' ')
            hdr[k] = v
            continue
        t = line.split(' ')
        if t[0] == 'p' and len(t) == 11:
            pads.append((int(t[1]), int(t[2]), int(t[3]), int(t[4], 16), int(t[5]), int(t[6]), int(t[7]),
                         int(t[8]), int(t[9]), int(t[10])))
        elif t[0] == 's' and len(t) in (7, 13):
            v = tuple(int(x, 16) for x in t[2:])
            sigs.append((int(t[1]),) + v + (0,) * (11 - len(v)))
        elif t[0] == 'b' and len(t) == 3:
            boards.append((int(t[1]), int(t[2], 16)))
        elif t[0] == 'w' and len(t) == 5:
            pokes.append((int(t[1]), t[2], int(t[3], 16), bytes.fromhex(t[4])))
        elif t[0] == 'x' and len(t) == 4:
            areas.append((int(t[1]), t[2], bytes.fromhex(t[3])))
        elif t[0] == 'm':
            notes.append((int(t[1]), ' '.join(t[2:])))
        elif t[0] == 'end':
            end = int(t[1])
    if end is None:
        end = max([p[0] for p in pads] + [s[0] for s in sigs] + [0])
    return dict(hdr=hdr, pads=pads, sigs=sigs, boards=boards, pokes=pokes, notes=notes, areas=areas, end=end)


def pad_timeline(rec):
    """state[r] = tuple of 4 (err, btn, sx, sy, cx, cy, tl, tr), r = 0..end"""
    end = rec['end']
    cur = [(-1, 0, 0, 0, 0, 0, 0, 0)] * 4
    out = [None] * (end + 1)
    ev = sorted(rec['pads'])
    i = 0
    for r in range(end + 1):
        while i < len(ev) and ev[i][0] <= r:
            e = ev[i]
            cur = list(cur)
            cur[e[1]] = e[2:]
            cur = tuple(cur)
            i += 1
        out[r] = tuple(cur)
    return out


def sig_timeline(rec):
    """sig[r] = (gc, ovl, frand, rnd8, brand) at the end of the frame before PADRead r"""
    end = rec['end']
    out = [None] * (end + 2)
    ev = rec['sigs']
    i = 0
    cur = None
    delta = 0
    for r in range(end + 2):
        while i < len(ev) and ev[i][0] <= r:
            cur = ev[i][1:]
            delta = ev[i][1] - ev[i][0]
            i += 1
        if cur is not None:
            out[r] = ((r + delta) & 0xFFFFFFFF,) + tuple(cur[1:])
    return out


def ctl_state(st):
    """one controller's port state -> the 8 DTM bytes"""
    err, btn, sx, sy, cx, cy, tl, tr = st
    if err != 0:
        return bytes((0, 0, 0, 0, 128, 128, 128, 128))  # is_connected clear
    b0 = ((btn & PAD_START) and 1) | ((btn & PAD_A) and 2) | ((btn & PAD_B) and 4) | ((btn & PAD_X) and 8) | \
         ((btn & PAD_Y) and 16) | ((btn & PAD_Z) and 32) | ((btn & PAD_UP) and 64) | ((btn & PAD_DOWN) and 128)
    b1 = ((btn & PAD_LEFT) and 1) | ((btn & PAD_RIGHT) and 2) | ((btn & PAD_L) and 4) | ((btn & PAD_R) and 8) | 0x40
    c = lambda v: max(0, min(255, v + 128))
    return bytes((b0, b1, tl & 0xFF, tr & 0xFF, c(sx), c(sy), c(cx), c(cy)))


def header(rec, n_polls, n_fields, controllers, backend='OGL'):
    h = bytearray(256)
    h[0:4] = b'DTM\x1a'
    h[4:10] = b'GMPE01'
    h[11] = controllers
    struct.pack_into('<Q', h, 13, n_fields)
    struct.pack_into('<Q', h, 21, n_polls)
    h[49:81] = b'Mario Party 4 PowerPC (M51)'.ljust(32, b'\0')
    h[81:97] = backend.encode().ljust(16, b'\0')[:16]  # the movie layer sets Dolphin's backend to it
    h[97:113] = b'HLE'.ljust(16, b'\0')
    struct.pack_into('<Q', h, 129, int(rec['hdr'].get('rtc', '1041472800')))
    h[137] = 1   # bSaveConfig: the settings below
    h[139] = 0   # bDualCore off: deterministic
    h[141] = 1   # bDSPHLE
    h[142] = 1 if FAST_DISC else 0  # bFastDiscSpeed (PLAN.md 66.3: the port reads its disc at once)
    h[143] = 1   # CPUCore: the JIT
    h[144] = 1   # EFB access
    h[145] = 1   # EFB copy
    h[146] = 1   # skip EFB copy to RAM
    h[150] = 1   # skip XFB copy to RAM
    fresh = rec['hdr'].get('card', 'fresh') in ('fresh', 'scratch')
    h[151] = 1   # a card in slot A
    h[152] = 1 if fresh else 0  # bClearSave: a new card, as the port's --freshcard
    h[160] = 1   # bUseFMA
    # tickCount: past the end (486 MHz x the fields / 59.94, and a margin)
    struct.pack_into('<Q', h, 237, int((n_fields + 600) / 59.94 * 486e6 * 1.5))
    return bytes(h)


def build_dtm(rec, vmap, out_path, until=None, prefix=None, prefix_fields=0, backend='OGL'):
    """vmap[r] = the console's VCounter for the port's PADRead r (None: not placed);
    `prefix` (an earlier movie's body) is kept as it is for the fields before
    `prefix_fields` -- dolphin_sync.py's frozen, verified start"""
    end = rec['end'] if until is None else min(until, rec['end'])
    st = pad_timeline(rec)
    controllers = 0
    for r in range(end + 1):
        for i in range(4):
            if st[r][i][0] == 0:
                controllers |= 1 << i
    if not controllers:
        controllers = 1
    ports = [i for i in range(4) if controllers & (1 << i)]
    vend = max(v for v in vmap[:end + 1] if v is not None) + 1
    # the input read at each field v: the last port retrace placed at or before v
    field = [None] * (vend + 1)
    for r in range(end + 1):
        v = vmap[r]
        if v is not None and 0 <= v <= vend:
            field[v] = r
    last = 0
    per_v = []
    for v in range(vend + 1):
        if field[v] is not None:
            last = field[v]
        per_v.append(b''.join(ctl_state(st[last][i]) for i in ports))
    npolls = 2 * vend + POLL_OF_V + 1
    neutral = b''.join(ctl_state((0, 0, 0, 0, 0, 0, 0, 0)) for _ in ports)
    body = bytearray()
    for q in range(npolls):
        v = (q - (POLL_OF_V - 1)) // 2
        body += per_v[v] if 0 <= v <= vend else neutral
    if prefix is not None and prefix_fields > 0:
        k = min(len(prefix), (2 * prefix_fields + POLL_OF_V - 1) * len(neutral), len(body))
        body[:k] = prefix[:k]
    with open(out_path, 'wb') as f:
        f.write(header(rec, npolls, npolls // 2, controllers, backend))
        f.write(body)
    return dict(fields=vend, polls=npolls, controllers=controllers, ports=ports, body=bytes(body))


def c2_board_seeds(seeds):
    """the C2 hook at BoardRandInit's store: the recording's seeds in order,
    then the console's own clock again once they run out"""
    n = len(seeds)
    w = [0] * (2 + n)
    w[0] = 0x48000001 | ((2 + n) * 4)          # bl over the data (LR = the counter)
    w[1] = 0                                   # the counter
    for i, s in enumerate(seeds):
        w[2 + i] = s & 0xFFFFFFFF
    code = [0x7D8802A6,                        # mflr r12
            0x816C0000,                        # lwz r11, 0(r12)
            0x280B0000 | n,                    # cmplwi r11, n
            0x40800000 | (6 * 4),              # bge +6 (to the store)
            0x556A103A,                        # slwi r10, r11, 2
            0x394A0004,                        # addi r10, r10, 4
            0x7C8C502E,                        # lwzx r4, r12, r10
            0x396B0001,                        # addi r11, r11, 1
            0x916C0000,                        # stw r11, 0(r12)
            BOARDRANDINIT_STORE_INSN]          # stw r4, boardRandSeed (the original)
    w += code
    if len(w) % 2 == 1:
        w.append(0x00000000)                   # the codehandler's branch back
    else:
        w += [0x60000000, 0x00000000]
    lines = [f'C2{BOARDRANDINIT_STORE & 0x1FFFFFF:06X} {len(w) // 2:08X}']
    for i in range(0, len(w), 2):
        lines.append(f'{w[i]:08X} {w[i + 1]:08X}')
    return lines


def gecko_resync(gc, frand, rnd8, brand, areas=(), mgnext=None):
    """one frame's hand-over: while GlobalCounter == gc (one frame of the
    instruction card, where nothing random moves), the console's RNG states
    and the game's work areas become the port's (PLAN.md 66.3)"""
    out = [f'20{GLOBALCOUNTER & 0x1FFFFFF:06X} {gc & 0xFFFFFFFF:08X}']
    for addr, val in ((FRAND_SEED, frand), (RND_SEED, rnd8), (BOARD_SEED, brand)):
        out.append(f'04{addr & 0x1FFFFFF:06X} {val & 0xFFFFFFFF:08X}')
    if mgnext is not None:
        out.append(f'02{MGNEXT & 0x1FFFFFF:06X} 0000{mgnext & 0xFFFF:04X}')
    for name, data in areas:
        addr = REGION_ADDR[name]
        out.append(f'06{addr & 0x1FFFFFF:06X} {len(data):08X}')
        pad = data + b'\0' * ((8 - len(data) % 8) % 8)
        for i in range(0, len(pad), 8):
            out.append(f'{pad[i:i + 4].hex().upper()} {pad[i + 4:i + 8].hex().upper()}')
    out.append('E0000000 80008000')
    return out


def write_gecko(rec, path, extra=()):
    seeds = [s for _, s in rec['boards']]
    out = ['[Gecko]']
    if seeds:
        out.append('$M51 board seeds (the recording\'s, in order)')
        out += c2_board_seeds(seeds)
    for name, lines in extra:
        out.append('$' + name)
        out += lines
    out.append('[Gecko_Enabled]')
    if seeds:
        out.append('$M51 board seeds (the recording\'s, in order)')
    for name, _ in extra:
        out.append('$' + name)
    open(path, 'w').write('\n'.join(out) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rec')
    ap.add_argument('out')
    ap.add_argument('--gecko')
    ap.add_argument('--map', help='the timeline map (tools/dolphin_sync.py); default: v = r + the boot offset')
    ap.add_argument('--until', type=int)
    ap.add_argument('--backend', default='OGL', help="the video backend the movie asks for ('Vulkan' on a Mac)")
    a = ap.parse_args()
    rec = parse_rec(a.rec)
    if rec['pokes']:
        sys.exit(f'rec2dtm: {a.rec} has {len(rec["pokes"])} writes of the self-play harness (a marathon or a '
                 f'scripted teleport): the console cannot be given those by a movie; only sessions played from '
                 f'the boot convert (PLAN.md 66.3)')
    if a.map:
        m = json.load(open(a.map))
        vmap = [None] * (rec['end'] + 1)
        for k, v in m['vmap'].items():
            if int(k) <= rec['end']:
                vmap[int(k)] = v
    else:
        vmap = [r - 4 for r in range(rec['end'] + 1)]
    info = build_dtm(rec, vmap, a.out, a.until, backend=a.backend)
    info.pop('body', None)
    if a.gecko:
        write_gecko(rec, a.gecko)
    print(json.dumps(info))


if __name__ == '__main__':
    main()
