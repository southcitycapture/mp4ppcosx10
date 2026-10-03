# M53 (PLAN.md 68): read a file out of the GameCube disc image by its FST path
#   m53_gcfs.py data/m432.bin OUT     (MP4_ISO, default ~/MarioParty4/mp4.nkit.iso)
import struct,sys
import os
ISO=os.environ.get('MP4_ISO', os.path.expanduser('~/MarioParty4/mp4.nkit.iso'))
f=open(ISO,'rb')
f.seek(0x424); fo,fs=struct.unpack('>II',f.read(8))
f.seek(fo); fst=f.read(fs)
n=struct.unpack('>I',fst[8:12])[0]; names=12*n
def name(o):
    e=fst.index(b'\0',names+o); return fst[names+o:e].decode()
ents={}; stack=[(n,'')]
for i in range(1,n):
    w,a,b=struct.unpack('>III',fst[12*i:12*i+12])
    while stack and i>=stack[-1][0]: stack.pop()
    pre=stack[-1][1] if stack else ''
    nm=name(w&0xffffff)
    if w>>24: stack.append((b,pre+nm+'/'))
    else: ents[(pre+nm).lower()]=(a,b)
def read(path):
    a,b=ents[path.lower()]; f.seek(a); return f.read(b)
if __name__=='__main__':
    open(sys.argv[2],'wb').write(read(sys.argv[1]))
