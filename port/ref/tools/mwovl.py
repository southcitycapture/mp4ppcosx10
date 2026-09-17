#!/usr/bin/env python3
"""Log every omcurovl change, with the GlobalCounter it happened at.

MemoryWatcher batches one datagram per frame containing every watched location
that changed, and GlobalCounter changes every frame, so the frame number
travels with the change and no clock is needed on this side.  This is the key
that lets a reference frame be named by the moment it was taken at rather than
by a PNG index -- the port's --ffto takes the same number.
"""
import socket, os, sys, time
sock_path, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
try: os.unlink(sock_path)
except OSError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(sock_path); s.settimeout(2.0)
f = open(out, "w", buffering=1); f.write("frame,omcurovl,omnextovl,omovlevtno\n")
KEYS = {"801D3A54": "gc", "801D3CE0": "cur", "801D3CE4": "nxt", "801D3CD4": "evt"}
st = {}; last = None; t0 = time.time()
while time.time() - t0 < secs:
    try: d = s.recv(8192)
    except socket.timeout: continue
    p = d.rstrip(b"\x00").decode("ascii", "replace").split("\n")
    for i in range(0, len(p) - 1, 2):
        k = KEYS.get(p[i].strip())
        if k: st[k] = p[i+1].strip().replace(",", "")
    if "gc" not in st: continue
    key = (st.get("cur"), st.get("nxt"), st.get("evt"))
    if key != last:
        last = key
        f.write("%d,%s,%s,%s\n" % (int(st["gc"], 16), key[0], key[1], key[2]))
