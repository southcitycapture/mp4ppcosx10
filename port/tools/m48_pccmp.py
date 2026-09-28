#!/usr/bin/env python3
"""M48 (PLAN.md 63): the picture checks by the frames' md5s -- a PC: set of the
chain against another, from the two chains' index files (M47's rule: the
candidate's frames against the previous release's, every run exit 0, 0 faults).

    m48_pccmp.py REF_INDEX REF_ARM NEW_INDEX NEW_ARM [--from-ref LINE]

Runs pair by name with the arm stripped (L-m427-@c4 <-> L-m427-@cd5; the
real-time runs keep their repeat, R-m427-@c4-1 <-> R-m427-@cd5-1); the last
run of a name wins (a set re-run whole replaces the first).  --from-ref: only
the reference index's lines from LINE on (M47's c4 set starts at 291).
Prints each run's frames that differ, the totals, and the runs that failed."""
import re
import sys


def runs(path, arm, first=1):
    out = {}
    tag = '-@' + arm
    for n, line in enumerate(open(path, errors='replace'), 1):
        if n < first:
            continue
        m = re.match(r'^(\S+) EXIT=(\d+) wall=\S+ fault=(\d+) isle=\S+ md5(.*?) args=', line)
        if not m or tag not in m.group(1):
            continue
        name = m.group(1)
        i = name.index(tag)
        rest = name[i + len(tag):]
        if rest.startswith(','):
            continue  # an arm with flags: not the set
        key = name[:i] + rest
        frames = dict(re.findall(r'(\S+?):([0-9a-f]{8})', m.group(4)))
        out[key] = (int(m.group(2)), int(m.group(3)), frames)
    return out


def main():
    a = sys.argv[1:]
    first = 1
    if '--from-ref' in a:
        k = a.index('--from-ref')
        first = int(a[k + 1])
        del a[k:k + 2]
    ref = runs(a[0], a[1], first)
    new = runs(a[2], a[3])
    same = diff = missing = 0
    bad = []
    for name in sorted(new):
        e, f, fr = new[name]
        if e != 0 or f != 0:
            bad.append('%s EXIT=%d fault=%d' % (name, e, f))
        if name not in ref:
            print('%-24s no reference run' % name)
            continue
        rfr = ref[name][2]
        d = []
        for k in sorted(set(fr) | set(rfr), key=lambda s: (len(s), s)):
            if k not in fr or k not in rfr:
                missing += 1
                d.append('%s:%s/%s' % (k, rfr.get(k, '--------'), fr.get(k, '--------')))
            elif fr[k] == rfr[k]:
                same += 1
            else:
                diff += 1
                d.append('%s:%s/%s' % (k, rfr[k], fr[k]))
        if d:
            print('%-24s %s' % (name, ' '.join(d)))
    print('frames: %d identical, %d differ, %d in one set only; runs %d, failed %d' %
          (same, diff, missing, len(new), len(bad)))
    for b in bad:
        print('FAILED RUN', b)


main()
