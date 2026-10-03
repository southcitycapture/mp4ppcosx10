#!/usr/bin/env python3
# M53 (PLAN.md 68): every FSLIDE member (decode types 4/5) of the disc, decoded the
# way HuDecodeFslide does -- which ones copy from before the output's start
# (the decoder does not check; HuDecodeSlide zero-fills).  None do.
import struct,sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m53_gcfs as gcfs
def scan_fslide(d, raw):
    # returns (min offset read relative to dst, count of before-start reads, first output pos)
    p=4  # slide header (size)
    out=0; flag=0; fl=0; minrel=0; nbefore=0; first=None
    size=raw
    while size>0:
        if fl==0:
            flag=struct.unpack('>I',d[p:p+4])[0]; p+=4; fl=32
        if flag>>31:
            p+=1; out+=1; size-=1
        else:
            dist=(d[p]<<8)|d[p+1]; p+=2
            ln=(dist>>12)&0xF; dist&=0xFFF
            if ln==0: ln=d[p]+18; p+=1
            else: ln+=2
            size-=ln
            for k in range(ln):
                rel=out-dist-1  # src[-1] relative to dst start
                if rel<0:
                    nbefore+=1; minrel=min(minrel,rel)
                    if first is None: first=out
                out+=1
        flag=(flag<<1)&0xFFFFFFFF; fl-=1
    return minrel,nbefore,first
names=[k for k in gcfs.ents if k.startswith('data/') and k.endswith('.bin')]
only=sys.argv[1:] 
tot=0
for nm in sorted(names):
    if only and nm not in only: continue
    f=gcfs.read(nm)
    n=struct.unpack('>I',f[:4])[0]
    offs=struct.unpack('>%dI'%n,f[4:4+4*n])
    for i,o in enumerate(offs):
        raw,typ=struct.unpack('>II',f[o:o+8])
        if typ in (4,5):
            try:
                mr,nb,first=scan_fslide(f[o+8:], raw)
            except Exception as e:
                print(nm,i,'ERR',e); continue
            if nb:
                tot+=1
                print(f'{nm} member {i} type {typ} raw {raw}: {nb} bytes read before dst, down to dst{mr}, first at output +{first}')
print('members with before-start reads:', tot)
