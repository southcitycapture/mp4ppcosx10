#!/usr/bin/env python3
"""Listen on Dolphin's MemoryWatcher socket and log m444dll's ball state per frame.

MemoryWatcher batches every location that CHANGED this frame into one datagram,
"<location>\n<value>\n" repeated, NUL-terminated.  GlobalCounter changes every
frame, so the frame number travels with the change and no clock is needed here.
NOTE: the value is hex WITH THOUSANDS SEPARATORS ("ff,fff,fff"), which is why
every value is stripped of commas before it is parsed.

The ball lives in m444dll's .bss, which is relocatable, so the location is a
MemoryWatcher POINTER CHAIN through the game's own module table:
    omDLLinfoTbl[i] (0x801901E0 + 4i) -> omDllData.bss (+8) -> lbl_1_bss_XXXX
and all 20 slots are watched because which one holds m444dll is not fixed.
"""
import socket, os, sys, time, struct, math
sock_path, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
try: os.unlink(sock_path)
except OSError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(sock_path); s.settimeout(2.0)
TBL = 0x801901E0
OFFS = ("1888", "188C", "1890", "1894", "1898", "189C")   # vel xyz, pos xyz
st = {}
f = open(out, "w", buffering=1)
f.write("frame,omcurovl,slot,velx,vely,velz,posx,posy,posz\n")
def f32(h):
    return struct.unpack(">f", struct.pack(">I", int(h, 16) & 0xFFFFFFFF))[0]
t0 = time.time(); n = 0
while time.time() - t0 < secs:
    try: d = s.recv(16384)
    except socket.timeout: continue
    n += 1
    p = d.rstrip(b"\x00").decode("ascii", "replace").split("\n")
    for i in range(0, len(p) - 1, 2):
        loc = p[i].strip()
        if loc: st[loc] = p[i+1].strip().replace(",", "")
    gc = st.get("801D3A54")
    if not gc: continue
    try: frame = int(gc, 16)
    except ValueError: continue
    ovl = st.get("801D3CE0", "?")
    for slot in range(20):
        base = TBL + 4*slot
        keys = [f"{base:08X} 8 {o}" for o in OFFS]
        vals = [st.get(k) for k in keys]
        if any(v is None for v in vals): continue
        try: v = [f32(x) for x in vals]
        except ValueError: continue
        if any(math.isnan(x) or math.isinf(x) for x in v): continue
        if not (-1000 < v[3] < 1000 and -1000 < v[5] < 1000): continue
        if all(x == 0.0 for x in v): continue
        f.write("%d,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n" % (frame, ovl, slot, *v))
print("datagrams", n, file=sys.stderr)
