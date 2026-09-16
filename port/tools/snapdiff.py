#!/usr/bin/env python
"""Byte-diff two snapshots (PLAN.md 24.2).

The question this answers is the only one that matters when a restored run
stops agreeing with a straight one: *which bytes* are different, and where do
they live.  Take a snapshot at the same frame in both runs and run this over
the pair; it reports the differing spans of MEM1, ARAM, the game's globals and
the port's registry, with MEM1/ARAM offsets turned back into the addresses the
boot log prints for the heaps.

Runs on the G4 as well as on the Mac: Python 2.5 compatible on purpose, so the
comparison can happen next to the 40 MB files instead of over ssh.

    python snapdiff.py A.snap B.snap [--bytes 64]
"""
import struct
import sys

# The snapshot is written by the PowerPC build, so every field is big-endian.
END = ">"
HDR_FIXED = 8 + 14 * 4
CTX = 256
MOD = 64 + 6 * 4
REG_NAME = 48


def read_header(f):
    f.seek(0)
    raw = f.read(HDR_FIXED)
    v = struct.unpack(END + "8s14I", raw)
    keys = ("magic version build_id frame mem1_addr mem1_size aram_addr "
            "aram_size stack_lo stack_size nranges ranges_bytes nmods nregs "
            "flags").split()
    h = {}
    for i, k in enumerate(keys):
        h[k] = v[i]
    f.read(CTX)
    return h


def sections(path):
    """Return (header, [(name, file_offset, length, base_addr_or_None)])."""
    f = open(path, "rb")
    h = read_header(f)
    off = HDR_FIXED + CTX
    out = []
    # the range table, then the ranges' bytes
    f.seek(off)
    table = []
    for _ in range(h["nranges"]):
        a, n = struct.unpack(END + "II", f.read(8))
        table.append((a, n))
    off += 8 * h["nranges"]
    for a, n in table:
        out.append(("gdata %08x" % a, off, n, a))
        off += n
    # the modules
    for _ in range(h["nmods"]):
        f.seek(off)
        raw = f.read(MOD)
        name = raw[:64].split("\0")[0]
        _handle, _image, data_lo, data_size, _open, _stuck = struct.unpack(
            END + "6I", raw[64:])
        off += MOD
        out.append(("rel %s" % name, off, data_size, data_lo))
        off += data_size
    # the registry
    for _ in range(h["nregs"]):
        f.seek(off)
        raw = f.read(REG_NAME + 4)
        name = raw[:REG_NAME].split("\0")[0]
        size = struct.unpack(END + "I", raw[REG_NAME:])[0]
        off += REG_NAME + 4
        out.append(("reg %s" % name, off, size, None))
        off += size
    out.append(("MEM1", off, h["mem1_size"], h["mem1_addr"]))
    off += h["mem1_size"]
    out.append(("ARAM", off, h["aram_size"], h["aram_addr"]))
    off += h["aram_size"]
    out.append(("stack", off, h["stack_size"], h["stack_lo"]))
    f.close()
    return h, out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nbytes = 32
    for a in sys.argv[1:]:
        if a.startswith("--bytes"):
            nbytes = int(a.split("=")[-1])
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    ha, sa = sections(args[0])
    hb, sb = sections(args[1])
    print("A frame %u build %08x   B frame %u build %08x"
          % (ha["frame"], ha["build_id"], hb["frame"], hb["build_id"]))
    if ha["build_id"] != hb["build_id"]:
        print("!! different builds; the comparison below is meaningless")
    fa, fb = open(args[0], "rb"), open(args[1], "rb")
    CHUNK = 1 << 16
    for (name, off, length, base), (nameb, offb, lengthb, _bb) in zip(sa, sb):
        if name != nameb or length != lengthb:
            print("%-28s LAYOUT DIFFERS (%s %d vs %s %d)"
                  % (name, name, length, nameb, lengthb))
            continue
        first = None
        ndiff = 0
        pos = 0
        while pos < length:
            n = min(CHUNK, length - pos)
            fa.seek(off + pos)
            fb.seek(offb + pos)
            da, db = fa.read(n), fb.read(n)
            if da != db:
                for i in range(n):
                    if da[i] != db[i]:
                        ndiff += 1
                        if first is None:
                            first = pos + i
            pos += n
        if ndiff:
            where = "" if base is None else " (addr %08x)" % (base + first)
            print("%-28s %8d bytes differ, first at +%08x%s"
                  % (name, ndiff, first, where))
            fa.seek(off + first)
            fb.seek(offb + first)
            ra, rb = fa.read(nbytes), fb.read(nbytes)
            print("    A: " + " ".join(["%02x" % ord(c) for c in ra]))
            print("    B: " + " ".join(["%02x" % ord(c) for c in rb]))
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
