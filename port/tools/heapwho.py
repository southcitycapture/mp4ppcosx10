#!/usr/bin/env python3
"""Name the heap block behind a differing address (M28, PLAN.md 43.11).

    python3 port/tools/heapwho.py A.snap B.snap [--map=marioparty4.map] [--heaptbl=0x...] [--spans=N]

Diffs the two snapshots' MEM1 (as snapdiff.py does), then walks every HuMem
heap in A -- the blocks are a doubly linked chain from each heap's first
header (memory.c: size, magic 205/165, flag, prev, next, num, retaddr) --
and for every differing span prints the block that holds it: its allocation
site (`retaddr`, turned into a symbol through the link map), its size and the
span's offset inside it.  A span in a HuPrc coroutine's stack shows as a
block allocated by HuPrcCreate/HuPrcChildCreate; one in a model as a block
of Hu3DModelCreate; one in nothing as a heap header or free space.
"""
import bisect
import os
import struct
import sys

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
    h = dict(zip(keys, v))
    f.read(CTX)
    return h


def sections(path):
    """snapdiff.py's section table, in Python 3 (bytes)"""
    f = open(path, "rb")
    h = read_header(f)
    off = HDR_FIXED + CTX
    out = []
    f.seek(off)
    table = []
    for _ in range(h["nranges"]):
        a, n = struct.unpack(END + "II", f.read(8))
        table.append((a, n))
    off += 8 * h["nranges"]
    for a, n in table:
        out.append(("gdata %08x" % a, off, n, a))
        off += n
    for _ in range(h["nmods"]):
        f.seek(off)
        raw = f.read(MOD)
        name = raw[:64].split(b"\0")[0].decode("ascii", "replace")
        _handle, _image, data_lo, data_size, _open, _stuck = struct.unpack(END + "6I", raw[64:])
        off += MOD
        out.append(("rel %s" % name, off, data_size, data_lo))
        off += data_size
    for _ in range(h["nregs"]):
        f.seek(off)
        raw = f.read(REG_NAME + 4)
        name = raw[:REG_NAME].split(b"\0")[0].decode("ascii", "replace")
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


def read_section(path, name):
    h, secs = sections(path)
    for n, off, length, base in secs:
        if n == name:
            with open(path, "rb") as f:
                f.seek(off)
                return f.read(length), base
    raise KeyError(name)


def load_map(path):
    syms = []
    if not path or not os.path.exists(path):
        return syms
    for line in open(path, errors="replace"):
        if not line.startswith("0x"):
            continue
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        name = parts[3].strip()
        syms.append((addr, size, name))
    syms.sort()
    return syms


def sym_of(syms, addr):
    if not syms:
        return "?"
    keys = [s[0] for s in syms]
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return "?"
    a, n, name = syms[i]
    if addr < a + max(n, 1):
        return "%s+0x%x" % (name, addr - a)
    return "%s+0x%x?" % (name, addr - a)


def walk_heaps(mem, base, heap_addrs):
    """Every block of every heap: (start, end, flag, num, retaddr)."""
    blocks = []
    for hp in heap_addrs:
        if hp == 0:
            continue
        seen = set()
        b = hp
        while b and b not in seen:
            seen.add(b)
            off = b - base
            if off < 0 or off + 24 > len(mem):
                break
            size, magic, flag, _pad, prev, nxt, num, ret = struct.unpack(END + "iBBHIIII", mem[off:off + 24])
            if magic not in (205, 165):
                break
            blocks.append((b, b + 32 + (size if size > 0 else 0), flag, num, ret, hp))
            if nxt == hp:
                break
            b = nxt
    blocks.sort()
    return blocks


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mappath = None
    max_spans = 400
    for a in sys.argv[1:]:
        if a.startswith("--map="):
            mappath = a.split("=", 1)[1]
        if a.startswith("--spans="):
            max_spans = int(a.split("=")[1])
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    syms = load_map(mappath)
    ma, base = read_section(args[0], "MEM1")
    mb, _ = read_section(args[1], "MEM1")
    # the heap table: HeapTbl[5] in gdata
    heap_addrs = []
    heaptbl = None
    for a in sys.argv[1:]:
        if a.startswith("--heaptbl="):
            heaptbl = int(a.split("=")[1], 16)
    # HeapTbl is static (malloc.c): its address comes from
    # `powerpc-apple-darwin8-nm marioparty4 | grep _HeapTbl` (--heaptbl=0x...)
    for a, n, name in ([(heaptbl, 20, "_HeapTbl")] if heaptbl else []) + syms:
        if name == "_HeapTbl":
            h, secs = sections(args[0])
            for sn, off, length, sbase in secs:
                if sbase is not None and sn.startswith("gdata") and sbase <= a < sbase + length:
                    with open(args[0], "rb") as f:
                        f.seek(off + (a - sbase))
                        heap_addrs = list(struct.unpack(END + "5I", f.read(20)))
    if not heap_addrs:
        # fall back: scan for heap headers (magic 205, prev == next == self)
        for off in range(0, len(ma) - 24, 32):
            size, magic, flag, _p, prev, nxt, num, ret = struct.unpack(END + "iBBHIIII", ma[off:off + 24])
            if magic == 205 and ret == 0xCDCDCDCD and num == 0xFFFFFF00:
                heap_addrs.append(base + off)
    print("heaps: %s" % " ".join("%08x" % h for h in heap_addrs))
    blocks = walk_heaps(ma, base, heap_addrs)
    print("%d blocks" % len(blocks))
    starts = [b[0] for b in blocks]
    # spans
    spans = []
    i = 0
    n = len(ma)
    while i < n:
        if ma[i] != mb[i]:
            j = i
            while j + 1 < n and (ma[j + 1] != mb[j + 1] or (j + 1 - i) < 16 and any(ma[k] != mb[k] for k in range(j + 1, min(n, j + 17)))):
                j += 1
            spans.append((i, j))
            i = j + 1
        else:
            i += 1
    print("%d differing spans, %d bytes" % (len(spans), sum(j - i + 1 for i, j in spans)))
    byblock = {}
    for i, j in spans[:max_spans]:
        addr = base + i
        k = bisect.bisect_right(starts, addr) - 1
        if k >= 0 and blocks[k][0] <= addr < blocks[k][1]:
            b = blocks[k]
            key = (b[0], b[4])
            byblock.setdefault(key, []).append((addr, j - i + 1, addr - b[0] - 32, b))
        else:
            byblock.setdefault((None, None), []).append((addr, j - i + 1, 0, None))
    for key, lst in sorted(byblock.items(), key=lambda kv: (kv[0][0] or 0)):
        if key[0] is None:
            print("outside any block: %s" % " ".join("%08x(%d)" % (a, l) for a, l, _, _ in lst))
            continue
        b = lst[0][3]
        print("block %08x..%08x %s num %u %s: %d span(s), %d bytes -- %s" % (
            b[0], b[1], "used" if b[2] else "FREE", b[3], sym_of(syms, b[4]),
            len(lst), sum(l for _, l, _, _ in lst),
            " ".join("+%x(%d)" % (o, l) for _, l, o, _ in lst[:12]) + (" ..." if len(lst) > 12 else "")))


if __name__ == "__main__":
    sys.exit(main())
