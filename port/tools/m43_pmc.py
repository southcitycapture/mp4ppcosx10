#!/usr/bin/env python3
"""M43 (PLAN.md 58.4): the performance-counter runs (tools/m43_chain.sh
K:GAME:SET), per screen: the game thread's user-mode cycles a drawn and a
consumed frame, the share stalled on L1 data misses (set 2's LMQ wait
cycles), the misses per frame at each level, the instructions (set 1) and
the IPC, and the regions' shares.

    m43_pmc.py DIR          (DIR holds K-*.log, plain or .gz)
"""
import sys, os, re, glob, gzip

SETS = {1: ['L1D load miss', 'L1D miss', 'instr', 'L2 data miss', 'L3 data miss'],
        2: ['L1D store miss', 'LMQ wait cyc', 'DTLB search cyc', 'L2 miss', 'L3 miss'],
        3: ['dcbt L1 miss', 'DTLB miss', 'instr', 'L3 miss', 'L2 miss']}

def opn(p):
    return gzip.open(p, 'rt', errors='replace') if p.endswith('.gz') else open(p, errors='replace')

def read(p):
    out = {}
    cur = None
    cols = None
    for line in opn(p):
        m = re.match(r'port> pmc: (drawn|consumed) frames (\d+), set (\d+)', line)
        if m:
            cur = {'frames': int(m.group(2)), 'set': int(m.group(3)), 'rows': {}}
            out[m.group(1)] = cur
            continue
        if cur is None:
            continue
        if line.startswith('port> pmc:   region'):
            cols = SETS[cur['set']]
            continue
        m = re.match(r'port> pmc:   (.+?)\s{2,}([\d.]+)\s+([\d.]+)\s+(.*)$', line)
        if m and cols:
            vals = [float(x) for x in m.group(4).split()]
            cur['rows'][m.group(1).strip()] = dict(calls=float(m.group(2)), mcyc=float(m.group(3)),
                                                   **dict(zip(cols, vals)))
    return out

def main():
    d = sys.argv[1]
    runs = {}
    for p in sorted(glob.glob(os.path.join(d, 'K-*.log*'))):
        m = re.match(r'K-(\w+)-(\d)-', os.path.basename(p))
        if not m:
            continue
        runs[(m.group(1), int(m.group(2)))] = read(p)
    games = sorted(set(g for g, s in runs))
    print('| screen | drawn Mcyc | consumed Mcyc | LMQ wait share drawn / consumed | L2 miss / L3 miss a drawn frame | DTLB search share | instr a drawn frame (M) | IPC drawn / consumed |')
    print('|---|---:|---:|---:|---:|---:|---:|---:|')
    for g in games:
        s2 = runs.get((g, 2), {})
        s1 = runs.get((g, 1), {})
        def tot(r, k, key):
            return r.get(k, {}).get('rows', {}).get('(total)', {}).get(key)
        dc, cc = tot(s2, 'drawn', 'mcyc'), tot(s2, 'consumed', 'mcyc')
        lw_d, lw_c = tot(s2, 'drawn', 'LMQ wait cyc'), tot(s2, 'consumed', 'LMQ wait cyc')
        l2, l3 = tot(s2, 'drawn', 'L2 miss'), tot(s2, 'drawn', 'L3 miss')
        tl = tot(s2, 'drawn', 'DTLB search cyc')
        ins_d, ins_c = tot(s1, 'drawn', 'instr'), tot(s1, 'consumed', 'instr')
        c1d, c1c = tot(s1, 'drawn', 'mcyc'), tot(s1, 'consumed', 'mcyc')
        f = lambda x, fmt='%.1f': '-' if x is None else fmt % x
        print('| %s | %s | %s | %s / %s | %s / %s | %s | %s | %s / %s |' % (
            g, f(dc), f(cc),
            f(100 * lw_d / (dc * 1e6), '%.0f%%') if lw_d and dc else '-',
            f(100 * lw_c / (cc * 1e6), '%.0f%%') if lw_c and cc else '-',
            f(l2, '%.0f'), f(l3, '%.0f'),
            f(100 * tl / (dc * 1e6), '%.1f%%') if tl and dc else '-',
            f(ins_d / 1e6, '%.1f') if ins_d else '-',
            f(ins_d / (c1d * 1e6), '%.2f') if ins_d and c1d else '-',
            f(ins_c / (c1c * 1e6), '%.2f') if ins_c and c1c else '-'))
    # regions, set 2, drawn + consumed together (a cycle)
    print()
    print('regions, a cycle (drawn + consumed), set 2: Mcycles (LMQ wait share)')
    regs = ['Hu3DDraw (object walk)', 'port GX', 'Hu3DExec (its own loops)', 'Hu3DMotionExec',
            'Hu3DDrawPost', 'Hu3DShadowExec', 'Hu3DModelObjMtxGet', 'HuPrcCall (processes)',
            'audio tick', 'present', 'rest']
    print('| screen | ' + ' | '.join(regs) + ' |')
    print('|---|' + '---:|' * len(regs))
    for g in games:
        s2 = runs.get((g, 2), {})
        cells = []
        for r in regs:
            mc = lw = 0.0
            for k in ('drawn', 'consumed'):
                row = s2.get(k, {}).get('rows', {}).get(r)
                if row:
                    mc += row['mcyc']
                    lw += row.get('LMQ wait cyc', 0.0)
            cells.append('%.1f (%.0f%%)' % (mc, 100 * lw / (mc * 1e6)) if mc > 0.05 else '-')
        print('| %s | %s |' % (g, ' | '.join(cells)))

if __name__ == '__main__':
    main()
