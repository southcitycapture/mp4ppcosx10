#!/usr/bin/env python3
"""M46 (PLAN.md 61): the flash rule (rt.c halfwatch) and M45's, simulated over every
dumped frame of a stretch (python3 port/tools/m46_flash.py DIR_OF_frame-N.ppm): the
three rows the counter reads, each rule at every sampling step 1-9 and phase, and
at 2,000 irregular samplings like a soak's --halfwatch 3 at 30 presented fps."""
import sys, glob
from PIL import Image
def lum(path):
    im=Image.open(path).convert('RGB'); w,h=im.size; px=im.load(); s=0
    for gy in (120,240,360):
        y=h-1-gy
        for x in range(640):
            s+=max(px[x,y])
    return s//1920
def rule45(seq):  # seq: list of (frame,lum) checks
    n=0; lb=None; ind=False; df=0
    for f,l in seq:
        if lb is None: lb=l; continue
        if not ind:
            if lb>=40 and l*100<lb*35: ind=True; df=f
            else: lb=l
        elif l*100>=lb*70 or f-df>12:
            if l*100>=lb*70 and f-df<=12: n+=1
            ind=False; lb=l
    return n
def rule46(seq):
    n=0; lb=lb2=0; ind=False; df=dmin=0; pend=None
    for f,l in seq:
        if pend:
            if pend*85<=l*100<=pend*115: n+=1
            pend=None
        if not ind:
            if lb>=40 and l*100<=lb*20 and lb*100>=lb2*85: ind=True; df=f; dmin=l
            else: lb2=lb; lb=l
        elif l*100>=lb*70:
            if f-df<=12 and dmin*100<=l*20: pend=l
            ind=False; lb2=lb=l
        elif f-df>12: ind=False; lb2=lb=l
        else: dmin=min(dmin,l)
    return n
d=sys.argv[1]
fs=sorted(glob.glob(d+'/frame-*.ppm')); L=[(int(f[-9:-4]),lum(f)) for f in fs]
print(d, ' '.join(str(l) for _,l in L))
for step in range(1,10):
    r45=[rule45(L[p::step]) for p in range(step)]; r46=[rule46(L[p::step]) for p in range(step)]
    print(f'  every {step}: M45 rule fires at {sum(1 for x in r45 if x)}/{step} phases, M46 rule at {sum(1 for x in r46 if x)}/{step}')
import random
random.seed(46)
t45=t46=0; T=2000
for _ in range(T):
    seq=[]; i=random.randrange(3)
    while i<len(L):
        seq.append(L[i]); i+=random.choice([4,4,6,6,6,8,9,12])
    t45+=rule45(seq)>0; t46+=rule46(seq)>0
print(f"  irregular (every 4-12, like the soak presented checks): M45 rule {t45}/{T}, M46 rule {t46}/{T}")
