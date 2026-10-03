#!/usr/bin/env python3
# M53 (PLAN.md 68): decode each LZ member of a data directory and walk every HSF
# model the way LoadHSF's DispObject does -- each child index in the symbol
# table and the object table, no object reached twice.  m53_hsfscan.py data/m432.bin
import struct,sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import m53_gcfs as gcfs
def lz(src, raw):
    tb=bytearray(1024); pos=958; out=bytearray(); p=0; flag=0
    while len(out)<raw:
        flag>>=1
        if not flag&0x100:
            flag=src[p]|0xFF00; p+=1
        if flag&1:
            b=src[p]; p+=1; out.append(b); tb[pos]=b; pos=(pos+1)&0x3FF
        else:
            i=src[p]; c=src[p+1]; p+=2
            i|=((c&~0x3F)<<2); c=(c&0x3F)+3
            for j in range(c):
                b=tb[(i+j)&0x3FF]; out.append(b); tb[pos]=b; pos=(pos+1)&0x3FF
    return bytes(out), p
def check(name, d):
    if d[:3]!=b'HSF': return None
    secs=[struct.unpack('>Ii',d[8+8*k:16+8*k]) for k in range(21)]
    objofs,objn=secs[8]; symofs,symn=secs[19]; strofs=secs[20][0]
    issues=[]
    seen=set()
    def disp(k,depth):
        if k in seen: issues.append(f'object {k} visited twice (double conversion)'); return
        seen.add(k)
        o=objofs+k*0x144
        typ=struct.unpack('>I',d[o+4:o+8])[0]
        if typ>9: issues.append(f'obj {k} type {typ}'); return
        if typ in (0,1,2,3,4,5,6,9) and typ not in (7,8):
            cc,ch=struct.unpack('>II',d[o+20:o+28])
            for i in range(cc):
                a=symofs+4*(ch+i)
                if ch+i>=symn: issues.append(f'obj {k} child {i} symbol index {ch+i} >= {symn} (reads +{a} of {len(d)})')
                if a+4>len(d): issues.append(f'obj {k} child read past the data end'); continue
                idx=struct.unpack('>I',d[a:a+4])[0]
                if idx>=objn: issues.append(f'obj {k} child {i} -> {idx} >= {objn}'); continue
                disp(idx,depth+1)
    root=None
    for k in range(objn):
        if struct.unpack('>i',d[objofs+k*0x144+16:objofs+k*0x144+20])[0]==-1: root=k; break
    if objn: disp(root if root is not None else objn, 0)
    return objn, symn, len(seen), issues
if __name__ != "__main__":
    raise SystemExit
f=gcfs.read(sys.argv[1]); n=struct.unpack('>I',f[:4])[0]
offs=list(struct.unpack('>%dI'%n,f[4:4+4*n]))
for i,o in enumerate(offs):
    raw,typ=struct.unpack('>II',f[o:o+8])
    if typ!=1: continue
    d,used=lz(f[o+8:], raw)
    r=check(i,d)
    if r: print(i, 'raw',raw,'objects',r[0],'symbols',r[1],'reached',r[2], r[3][:5])
