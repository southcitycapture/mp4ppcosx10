#!/usr/bin/env python3
"""M39c (PLAN.md 54b.5): the console's board frames.  One Dolphin run a board
(the Flatpak on littlejelly, OpenGL, no audio, FFV1 AVI), from M26b's
capture.py: a fresh user dir seeded from port/ref/dolphin-user, the schedule
port/ref/movies/board-com4-oracle.txt with GWSystem.board poked to N-1 while
omcurovl is mentdll (mkgecko.py's `pokeif`), stopped by the game's own state
(MemoryWatcher on GlobalCounter / omcurovl / GWSystem's turn word): 900 GC
after turn 2 begins on the board, or the board's entry + 24,000.

The AVI is split twice: every frame from 300 before the board's entry at
160x120 (small/, the matcher's haystack), and later -- `--pick` -- the
chosen dump indices at full size.  The AVI is deleted after the picks (the
67 GB disk lesson); only the picked PNGs are kept.

  m39c_oracle.py capture N            # board N (1-6)
  m39c_oracle.py pick N PORTDIR F1,F2,F3,F4 [--win W]
        # match the port's frames PORTDIR/frame-0F.ppm against small/ and
        # extract the best console frame for each at full size
"""
import os, sys, re, socket, subprocess, time, json, signal, shutil, glob, argparse

HOME = os.path.expanduser('~')
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = f'{HOME}/mp4-board-oracle'
U = f'{HOME}/mp4-bo-dolphin'              # short: sun_path is 104 bytes
ISO = f'{HOME}/MarioParty4/mp4.nkit.iso'
FFMPEG = f'{HOME}/bin/ffmpeg'
SEED = f'{REPO}/port/ref/dolphin-user'
MKGECKO = f'{REPO}/port/ref/tools/mkgecko.py'
SCHED = f'{REPO}/port/ref/movies/board-com4-oracle.txt'

ovl = []; skip = False
for line in open(f'{REPO}/include/ovl_table.h'):   # the USA table
    if line.startswith('#if VERSION_JP'): skip = True; continue
    if line.startswith('#else') or line.startswith('#endif'): skip = False; continue
    if skip: continue
    m = re.match(r'\s*DLL\((\w+)\)', line)
    if m: ovl.append(m.group(1))
ovl_id = {v: i for i, v in enumerate(ovl)}
def ovl_name(i): return ovl[i] if i is not None and 0 <= i < len(ovl) else str(i)

def dolphins():
    r = subprocess.run(['pgrep', '-x', 'dolphin-emu'], capture_output=True, text=True)
    return [int(x) for x in r.stdout.split()]

def capture(n, timeout):
    name = f'w0{n}'
    os.makedirs(f'{WORK}/logs', exist_ok=True)
    log = open(f'{WORK}/logs/{name}.capture.log', 'a')
    def say(s):
        line = f'{time.strftime("%H:%M:%S")} {name}: {s}'
        print(line, flush=True); log.write(line + '\n'); log.flush()
    target = ovl_id[f'w0{n}dll']
    if os.path.exists(U): shutil.rmtree(U)
    shutil.copytree(SEED, U)
    for d in ('GameSettings', 'MemoryWatcher', 'Dump/Frames'):
        os.makedirs(f'{U}/{d}', exist_ok=True)
    sched = open(SCHED).read().replace('@BOARD', str(n - 1))
    sp = f'{WORK}/logs/{name}-sched.txt'; open(sp, 'w').write(sched)
    subprocess.run(['python3', MKGECKO, sp, f'{U}/GameSettings/GMPE01.ini', '--name', f'M39c{name}'], check=True)
    # GlobalCounter, omcurovl, GWSystem+4 (turn, max_turn, star_flag, star_total)
    open(f'{U}/MemoryWatcher/Locations.txt', 'w').write('801D3A54\n801D3CE0\n8018FCFC\n')
    sock_path = f'{U}/MemoryWatcher/MemoryWatcher'
    try: os.unlink(sock_path)
    except OSError: pass
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); sock.bind(sock_path); sock.settimeout(2.0)
    w = 0
    while dolphins():
        if w == 0: say(f'waiting: a dolphin-emu I did not start is running ({dolphins()})')
        time.sleep(10); w += 1
        if w > 360: say('gave up waiting'); sys.exit(2)
    before = set(dolphins())
    env = dict(os.environ, DISPLAY=':0', XAUTHORITY=f'{HOME}/.Xauthority', LC_ALL='C.UTF-8')
    cmd = ['flatpak', 'run', '--filesystem=home', 'org.DolphinEmu.dolphin-emu', '-u', U, '-b', '-e', ISO, '-v', 'OGL',
           '-C', 'GFX.Settings.FrameDumpsUseFFV1=True', '-C', 'Dolphin.Core.EnableCheats=True',
           '-C', 'Dolphin.Movie.DumpFrames=True', '-C', 'Dolphin.DSP.Backend=No Audio Output']
    dlog = open(f'{WORK}/logs/{name}.dolphin.log', 'w')
    t0 = time.time()
    proc = subprocess.Popen(cmd, stdout=dlog, stderr=subprocess.STDOUT, env=env, stdin=subprocess.DEVNULL)
    say(f'launched: board {n} (w0{n}dll, overlay {target})')
    ev = open(f'{WORK}/logs/{name}.events.csv', 'w'); ev.write('wall,gc,cur,name,turn\n')
    st = {}; cur_last = None; turn_last = None; gc = 0
    entry_gc = turn2_gc = None; reason = None; last_prog = time.time(); last_gc = -1
    KEYS = {'801D3A54': 'gc', '801D3CE0': 'cur', '8018FCFC': 'sys'}
    while True:
        now = time.time()
        if now - t0 > timeout: reason = 'timeout'; break
        try:
            d = sock.recv(8192)
        except socket.timeout:
            if now - last_prog > 60 and last_gc > 0: reason = f'stalled at gc {last_gc}'; break
            if proc.poll() is not None and not (set(dolphins()) - before): reason = 'dolphin exited'; break
            continue
        p = d.rstrip(b'\x00').decode('ascii', 'replace').split('\n')
        for i in range(0, len(p) - 1, 2):
            k = KEYS.get(p[i].strip())
            if k:
                try: st[k] = int(p[i + 1].strip().replace(',', ''), 16)
                except ValueError: pass
        gc = st.get('gc', gc); cur = st.get('cur'); turn = (st.get('sys', 0) >> 24) & 0xFF
        if gc != last_gc: last_gc = gc; last_prog = now
        if cur != cur_last or turn != turn_last:
            cur_last = cur; turn_last = turn
            ev.write(f'{now - t0:.1f},{gc},{cur},{ovl_name(cur)},{turn}\n'); ev.flush()
            say(f'gc {gc}: overlay {cur} {ovl_name(cur)} turn {turn}')
            if cur is not None and ovl_id['w01dll'] <= cur <= ovl_id['w21dll'] and entry_gc is None:
                entry_gc = gc
                if cur != target: say(f'WRONG BOARD: {ovl_name(cur)}')
            if entry_gc is not None and turn == 2 and turn2_gc is None and cur == target:
                turn2_gc = gc
        if turn2_gc is not None and gc >= turn2_gc + 900: reason = 'turn 2 +900'; break
        if entry_gc is not None and gc >= entry_gc + 24000: reason = 'entry +24000'; break
        if entry_gc is None and gc >= 20000: reason = 'gc 20000 without a board'; break
    say(f'stop: {reason}; gc {gc}; entry {entry_gc} turn2 {turn2_gc}')
    mine = set(dolphins()) - before
    for pid in mine:
        try: os.kill(pid, signal.SIGTERM)
        except OSError: pass
    for _ in range(10):
        if not (set(dolphins()) & mine): break
        time.sleep(1)
    for pid in set(dolphins()) & mine:
        try: os.kill(pid, signal.SIGKILL)
        except OSError: pass
    try: proc.terminate(); proc.wait(10)
    except Exception: pass
    time.sleep(2); sock.close()
    avis = sorted(glob.glob(f'{U}/Dump/Frames/*.avi'))
    out = f'{WORK}/{name}'
    if os.path.exists(out): shutil.rmtree(out)
    os.makedirs(f'{out}/small')
    ws = max(1, (entry_gc or 7000) - 351 - 300)
    if avis:
        keep = f'{WORK}/{name}.avi'
        shutil.move(avis[0], keep)
        for av in avis[1:]: os.unlink(av)
        t1 = time.time()
        r = subprocess.run([FFMPEG, '-v', 'error', '-i', keep, '-vf', f'select=gte(n\\,{ws}),scale=160:120',
                            '-fps_mode', 'passthrough', '-start_number', str(ws), f'{out}/small/s%06d.png'],
                           capture_output=True, text=True)
        files = sorted(glob.glob(f'{out}/small/s*.png'))
        say(f'split: {len(files)} small frames from dump index {ws} in {time.time() - t1:.0f}s '
            f'({os.path.getsize(keep) >> 20} MB avi) {r.stderr.strip()[:200]}')
    else:
        say('NO AVI')
    shutil.rmtree(f'{U}/Dump/Frames', ignore_errors=True)
    json.dump(dict(board=n, reason=reason, entry_gc=entry_gc, turn2_gc=turn2_gc, last_gc=gc, ws=ws,
                   wall=round(time.time() - t0)), open(f'{WORK}/logs/{name}.json', 'w'), indent=1)

def pick(n, portdir, frames, win):
    from PIL import Image, ImageChops, ImageStat
    name = f'w0{n}'; out = f'{WORK}/{name}'
    info = json.load(open(f'{WORK}/logs/{name}.json'))
    small = sorted(glob.glob(f'{out}/small/s*.png'))
    idx = [int(os.path.basename(f)[1:7]) for f in small]
    cache = {}
    def load(i):
        if i not in cache: cache[i] = Image.open(f'{out}/small/s{i:06d}.png').convert('L')
        return cache[i]
    res = []
    prev = None
    for k, f in enumerate(frames):
        pth = f'{portdir}/frame-{f:06d}.ppm' if os.path.exists(f'{portdir}/frame-{f:06d}.ppm') else f'{portdir}/frame-{f:05d}.ppm'
        P = Image.open(pth).convert('L').resize((160, 120))
        # the port's board entry is the frame --boarddump 0 dumped; the console's is entry_gc - 351
        lo = (prev + 1) if prev else idx[0]
        best = None
        for i in idx:
            if i < lo: continue
            if win and prev and i > prev + win: break
            d = ImageStat.Stat(ImageChops.difference(P, load(i))).mean[0]
            if best is None or d < best[0]: best = (d, i)
        res.append((f, best[1], round(best[0], 2)))
        prev = best[1]
        print(f'{name}: port {f} -> console dump {best[1]} (mean diff {best[0]:.2f})', flush=True)
    keep = f'{WORK}/{name}.avi'
    for f, i, d in res:
        subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', keep, '-vf', f'select=eq(n\\,{i})', '-fps_mode', 'passthrough',
                        '-frames:v', '1', f'{out}/c{i:06d}.png'], check=True)
    json.dump(res, open(f'{out}/picks.json', 'w'))

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd'); ap.add_argument('board', type=int)
    ap.add_argument('portdir', nargs='?'); ap.add_argument('frames', nargs='?')
    ap.add_argument('--timeout', type=float, default=2400)
    ap.add_argument('--win', type=int, default=0)
    a = ap.parse_args()
    if a.cmd == 'capture': capture(a.board, a.timeout)
    elif a.cmd == 'pick': pick(a.board, a.portdir, [int(x) for x in a.frames.split(',')], a.win)
    elif a.cmd == 'clean':
        os.unlink(f'{WORK}/w0{a.board}.avi'); shutil.rmtree(f'{WORK}/w0{a.board}/small')
