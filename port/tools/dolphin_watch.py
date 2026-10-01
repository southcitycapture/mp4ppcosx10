#!/usr/bin/env python3
"""dolphin_watch.py -- run the reference Dolphin (littlejelly's Flatpak) headless
on a fresh copy of port/ref/dolphin-user, optionally playing a .dtm and/or a
Gecko code set, and log every change of a list of RAM words (MemoryWatcher) with
the game's VCounter/GlobalCounter at the moment (M51, PLAN 66).

  dolphin_watch.py OUTDIR [--dtm F] [--gecko GMPE01.ini] [--watch ADDR[:name] ...]
                   [--until-gc N] [--until-vc N] [--timeout S] [--dump] [--rtc SECS]
                   [--audio WAV] [-C KEY=VAL ...]

Writes OUTDIR/watch.csv (wall,vc,gc,name,value per change), OUTDIR/dolphin.log,
OUTDIR/summary.json and, with --dump, OUTDIR/frames.avi (FFV1, one frame per
presented XFB) -- split it with ffmpeg.  One Dolphin at a time: waits out any
dolphin-emu it did not start; kills only its own.
"""
import argparse, glob, json, os, re, shutil, signal, socket, subprocess, sys, time

HOME = os.path.expanduser('~')
HERE = os.path.dirname(os.path.abspath(__file__))
SEED = os.path.join(HERE, '..', 'ref', 'dolphin-user')
ISO = os.environ.get('MP4_ISO', f'{HOME}/MarioParty4/mp4.nkit.iso')

NAMES = {
    '801D3A54': 'gc', '801D3A58': 'vc', '801D3CE0': 'ovl', '801D3D10': 'frand',
    '801D3F14': 'brand', '801D3A9C': 'padbtn01', '801D3AA0': 'padbtn23',
    '801D3A88': 'stkx', '801D3A84': 'stky', '801D342C': 'rnd8',
}

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--dtm')
ap.add_argument('--gecko')
ap.add_argument('--watch', action='append', default=[])
ap.add_argument('--until-gc', type=int)
ap.add_argument('--until-vc', type=int)
ap.add_argument('--timeout', type=float, default=600)
ap.add_argument('--dump', action='store_true')
ap.add_argument('--audio', help='dump the DSP audio to this WAV')
ap.add_argument('--rtc', type=int)
ap.add_argument('--card', help='a raw card image for slot A (else a fresh one)')
ap.add_argument('--user', default=f'{HOME}/mp4-m51-dolphin', help='the scratch user dir (short: sun_path)')
ap.add_argument('-C', dest='conf', action='append', default=[])
a = ap.parse_args()

os.makedirs(a.out, exist_ok=True)
U = a.user
if os.path.exists(U):
    shutil.rmtree(U)
shutil.copytree(SEED, U)
if a.rtc is not None:
    p = f'{U}/Config/Dolphin.ini'
    s = open(p).read()
    s = re.sub(r'CustomRTCValue = \d+', f'CustomRTCValue = {a.rtc}', s)
    open(p, 'w').write(s)
for d in ('GameSettings', 'MemoryWatcher', 'Dump/Frames', 'Dump/Audio', 'GC/USA/Card A'):
    os.makedirs(f'{U}/{d}', exist_ok=True)
if a.gecko:
    shutil.copy(a.gecko, f'{U}/GameSettings/GMPE01.ini')
if a.card:
    shutil.copy(a.card, f'{U}/GC/MemoryCardA.USA.raw')

watch = ['801D3A54', '801D3A58']
names = dict(NAMES)
for w in a.watch:
    addr, _, nm = w.partition(':')
    addr = addr.upper().replace('0X', '')
    if nm:
        names[addr] = nm
    if addr not in watch:
        watch.append(addr)
open(f'{U}/MemoryWatcher/Locations.txt', 'w').write(''.join(w + '\n' for w in watch))
sock_path = f'{U}/MemoryWatcher/MemoryWatcher'
try:
    os.unlink(sock_path)
except OSError:
    pass
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(sock_path)
sock.settimeout(2.0)


def dolphins():
    r = subprocess.run(['pgrep', '-x', 'dolphin-emu'], capture_output=True, text=True)
    return [int(x) for x in r.stdout.split()]


w = 0
while dolphins():
    if w == 0:
        print(f'waiting: a dolphin-emu I did not start is running ({dolphins()})', flush=True)
    time.sleep(10)
    w += 1
    if w > 360:
        sys.exit('gave up waiting for the foreign Dolphin')
before = set(dolphins())

env = dict(os.environ, DISPLAY=os.environ.get('DISPLAY', ':0'),
           XAUTHORITY=f'{HOME}/.Xauthority', LC_ALL='C.UTF-8')
cmd = ['flatpak', 'run', '--filesystem=home', 'org.DolphinEmu.dolphin-emu', '-u', U, '-b',
       '-e', ISO, '-v', 'OGL', '-C', 'Dolphin.Movie.DumpFrames=' + ('True' if a.dump else 'False'),
       '-C', 'GFX.Settings.FrameDumpsUseFFV1=True']
if a.gecko:
    cmd += ['-C', 'Dolphin.Core.EnableCheats=True']
if a.audio:
    cmd += ['-C', 'Dolphin.Movie.DumpAudio=True']
else:
    cmd += ['-C', 'Dolphin.DSP.Backend=No Audio Output']
for c in a.conf:
    cmd += ['-C', c]
if a.dtm:
    cmd += ['-m', os.path.abspath(a.dtm)]
dlog = open(f'{a.out}/dolphin.log', 'w')
t0 = time.time()
proc = subprocess.Popen(cmd, stdout=dlog, stderr=subprocess.STDOUT, env=env, stdin=subprocess.DEVNULL)

csv = open(f'{a.out}/watch.csv', 'w')
csv.write('wall,vc,gc,name,value\n')
st = {}
reason = None
last_prog = time.time()
last_vc = -1
while True:
    now = time.time()
    if now - t0 > a.timeout:
        reason = 'timeout'
        break
    try:
        d = sock.recv(65536)
    except socket.timeout:
        if now - last_prog > 60 and last_vc > 0:
            reason = f'stalled at vc {last_vc}'
            break
        if proc.poll() is not None and not (set(dolphins()) - before):
            reason = 'dolphin exited'
            break
        continue
    p = d.rstrip(b'\x00').decode('ascii', 'replace').split('\n')
    changed = []
    for i in range(0, len(p) - 1, 2):
        k = p[i].strip().upper()
        try:
            v = int(p[i + 1].strip().replace(',', ''), 16)
        except ValueError:
            continue
        st[k] = v
        changed.append(k)
    vc = st.get('801D3A58', 0)
    gc = st.get('801D3A54', 0)
    if vc != last_vc:
        last_vc = vc
        last_prog = now
    for k in changed:
        if k in ('801D3A58',):
            continue
        csv.write(f'{now - t0:.2f},{vc},{gc},{names.get(k, k)},{st[k]:08x}\n')
    if a.until_gc and gc >= a.until_gc:
        reason = f'gc {gc}'
        break
    if a.until_vc and vc >= a.until_vc:
        reason = f'vc {vc}'
        break
csv.close()

mine = set(dolphins()) - before
for pid in mine:
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        pass
for _ in range(15):
    if not (set(dolphins()) & mine):
        break
    time.sleep(1)
for pid in set(dolphins()) & mine:
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
try:
    proc.terminate()
    proc.wait(10)
except Exception:
    pass
time.sleep(2)
sock.close()
avis = sorted(glob.glob(f'{U}/Dump/Frames/*.avi'))
if avis:
    shutil.move(avis[0], f'{a.out}/frames.avi')
wavs = sorted(glob.glob(f'{U}/Dump/Audio/*.wav'))
if a.audio and wavs:
    shutil.move(max(wavs, key=os.path.getsize), a.audio)
summary = dict(reason=reason, vc=st.get('801D3A58'), gc=st.get('801D3A54'), wall=round(time.time() - t0, 1),
               avi=bool(avis), dtm=a.dtm, gecko=a.gecko)
json.dump(summary, open(f'{a.out}/summary.json', 'w'), indent=1)
print(json.dumps(summary))
