# The witness list — what the next hardware session runs, in order

Everything in PLAN.md §18 and §18.10 is a compiler result, a disassembly or a
reading of the source. Two crashes are *believed* fixed and one measurement has
never been taken. This file is the shortest path from that to evidence: a list
of single commands, each with the thing it produces and where that thing lands,
in the order that makes each one worth running.

Nothing here is new work. Every flag already exists; the reason none of it has
run is that M8 and M8b had no route to the lab (§18.7).

Budget: about two hours, of which the soak is unattended.

---

## 0. Is there a lab at all

```sh
tailscale status | grep littlejelly && tailscale ping -c 1 littlejelly-macbookpro
```

**Expect:** a line with an IP and no `offline`, **and a `pong`**. The ping is
not belt-and-braces: on 2026-09-17 (M12) `tailscale status` listed
`littlejelly-macbookpro` as online all afternoon while every ssh to it timed
out during banner exchange, and the ping was the only thing that said why —
*peer's node key has expired*. An expired key needs the user to re-authenticate
that machine; nothing on this end fixes it, and until it is fixed there is no
route to the G4 from anywhere but the house. `littlejelly` is the ProxyJump
host; with it down there is no route to the G4 from anywhere but the house, and
`192.168.0.200` on the office LAN is somebody else's machine (§18.8). If this
line is missing, stop — every command below will either hang or, worse, reach a
stranger's `sshd`.

```sh
g4 power status && g4 use MarioParty4.app && g4 log 5
```

**Expect:** the G4 answers, `~/isle.app` points at MarioParty4.app, and the
last run's log tail prints. Away from home, `G4_HOST=g4-jump` first.

Then ship what this session built — the six corrected modules are **not** on
the G4 yet:

```sh
port/build-ppc.sh -j8 && port/tools/make_bundle.sh && port/tools/g4_install.sh
```

**Expect:** 99 REL bundles rebuilt and copied. `--image` is not needed; the
disc image is already in `~/MarioParty4/` (§README "Running on the G4").

---

## 0b. There is a debugger now *(2026-09-16)*

The G4 has gdb: Xcode 3.0's `DeveloperToolsCLI.pkg` (gdb-768, plus nm, otool,
atos, malloc_history) installed from the Leopard disc image on the Mac
(`os-images/leopard-10.5.4-install.toast`, `Optional Installs/Xcode Tools/Packages`).
`zach` is in group `procmod`, and `taskgated` runs `-p`, so gdb attaches
without sudo; `sudo gdb`, `sudo ~/bin/mp4peek` and `sudo date` are NOPASSWD.

What makes it useful:

* objects are built `-gdwarf-2 -gstrict-dwarf` (Makefile `DEBUG_G`) — gdb-768
  cannot read GCC 14's DWARF 5 (`Cannot handle DW_FORM_<unknown>`);
* the debug map in the binary names `/work/mp4/port/build-ppc-darwin/...`,
  and on the G4 `/work/mp4 -> ~/mp4-work`; `port/tools/g4_debug_sync.sh`
  ships the `.o` tree there after every build, mtimes preserved;
* `mp4bt [pid]` on the G4 prints every thread's backtrace with file:line and
  detaches; the game keeps running. For anything more, a command file:
  `gdb -batch -x cmds.gdb ~/isle.app/Contents/MacOS/isle PID` (gdb-768 has no
  `-ex`; give it the executable explicitly or it looks for a file named PID).

**How to not kill the process you are debugging** (learned 2026-09-16 at
the cost of the third-visit m444 repro, seven hours in): Apple gdb-768
answers `info symbol ADDR` and some `print`s on an unmapped address by
calling `objc_lookUpClass` *inside the game*; if that faults, the game has
faulted, and `-batch` detaches from a corpse. Every command file starts
with `set unwindonsignal on`, resolves addresses with `info line *ADDR`
(no inferior call), and range-checks any pointer read off a saved stack.
`port/tools/gdb/procs.gdb` is the safe walk of the coroutine list; copy its
shape for anything new.

**Two more rules, both learned on 2026-09-16 at the cost of a 25-minute
reproduction each** (PLAN.md §23.1):

* **Never pipe gdb's output.** `gdb -batch … | head` closes the pipe, gdb dies of
  `SIGPIPE` *while attached*, and the game dies with it (`EXITCODE=137`). Use
  `port/tools/gdb/mpgdb CMDFILE OUTFILE`, which redirects to a file; read the
  file afterwards.
* **Any error in a batch script aborts it before `detach`,** and the game dies
  again (`EXITCODE=132`). So a script must not touch anything it has not proved
  is there. The safe shape is two phases: `info sharedlibrary` on its own for a
  module's load address, then `x/` at addresses computed from `nm` — `x` on a
  mapped address cannot error, and a REL's file-local symbols need no lookup.
  `x/f` in gdb-768 is an **8-byte double**; floats want `x/3wf`.
  `port/tools/gdb/mot.gdb` is the safe `Hu3DData` motion walk. The coroutines are all parked in `HuPrcVSleep`, so
the interesting frame is the *caller*, at `*(*(jump.sp) + 8)`.

**`procs.gdb` is not safe either** (M11, 2026-09-17): a saved-stack pointer
that passes the range check can still be unmapped (guard pages, freed
stacks), and gdb-768 has no try/catch, so the "Cannot access memory" error
aborts the script before `detach` and the game dies (EXITCODE=132). Walk
the coroutine list with `sudo ~/bin/mp4peek PID procs <processtop>` instead
(task_for_pid reads cannot hurt the process); keep gdb for `mp4bt` and
breakpoints. Anything in a batch file that *can* error goes last, after a
`detach` that has already run.

**Watchers die with their run** (2026-09-18): a background watcher that greps
`~/isle-log.txt` outlives the run it was armed for, and `g4 run` rotates that
file, so a late watcher reports on someone else's run — and if it has a write
side effect (`cp`, `tail >`) it clobbers files (M13's overwrote `~/ab-old.log`).
Stop every watcher when its run ends; never give a watcher a side effect.
Also: `--stuckwatch` below ~200 s is a hazard now that the navigator's presses
land (M14): it mashes buttons on a healthy board and walks into the pause menu.

The loop: `port/build-ppc.sh -j8 && port/tools/g4_debug_sync.sh && g4 push-bin`.
A binary and its `.o` tree must come from the same build, or gdb reads the
wrong lines.

## 0c. Teleport to the bug *(M10, 2026-09-16)*

Two flags that turn "reproduce it" from an overnight job into a coffee break.
Both are witnessed against the §21.1 reference frames; PLAN.md §24 has the
numbers.

### Fast-forward: be at frame N without waiting for it

```sh
g4 run --ffto 7000 --dumpframe 7000 --shotdir ~/m10shots \
       --turbo --com4 --rtc dolphin --freshcard --play board-start-com4.play \
       --frames 9000
g4 ssh md5 '~/m10shots/frame-07000.ppm'
```

**Expect:** `port> ffto: reached frame 7000 in 41.4 s (6997 frames, 168.9 fps
...)` and `3488c83d092ed08a078e26ec7319d749`. The renderer is off until frame
N − 1, and the game cannot tell: GX is a write-only command stream here. The
mix keeps running, so the run is the *same run*, 6–20× faster.

`--nodraw` on its own is the same thing without an end — the right flag for a
soak that only has to reach a state, and the reason a 6,100-frame walk takes
51 s instead of 436. `--dumpframe` inside a `--nodraw` stretch refuses rather
than writing the stale framebuffer.

### Snapshots: start minutes before the crash, with gdb already attached

```sh
# leave a run soaking with a ring of three
g4 run --soak --com4 --rtc dolphin --freshcard --snap-every 5000 --snap-keep 3

# when it dies, the fault report lists what survived; take the newest
g4 run --restore ~/MarioParty4/snaps/f235000.snap --turbo --com4 \
       --rtc dolphin --play board-start-com4.play
# ...and on the G4, once it is past the restore line:
mp4bt $(pgrep -f MacOS/isle)
```

**Expect:** `port> --restore: ... frame 235000, N modules, 71 registry
entries, 0.35 s` then `port> --restore: resuming the game`, and the run
continues **byte-identically** — proved by restoring a frame-6000 snapshot and
dumping frame 7000 to §21.1's md5.

Rules of the road:

* **Same binary.** A snapshot carries the game's globals by address; the build
  id (the snapmap plus the executable's size and mtime) refuses anything else
  with a clear message. Do not rebuild between taking and restoring.
* **`isle.snapmap` must sit next to `isle`** in the bundle; `make_bundle.sh`
  puts it there. Without it snapshots are unavailable and the port says so.
* Every other flag on the restore line still applies, so a restore can be
  `--dumpframe`d, `--wav`ed, run under `--minigame`, or driven by a different
  `--play` script. `--freshcard` is pointless on a restore: the card comes back
  with the snapshot.
* 40.7 MB and ~3 s per snapshot, 0.35 s to restore, `~/MarioParty4/snaps` by
  default (`--snap-dir` elsewhere). `--snap-keep N` is a ring over the
  snapshots *this run* wrote, so a restore's source is never swept away.
* **When a restored run misbehaves**, take a snapshot at the same frame in both
  runs (`--snap-at F --snap-dir ~/snapA` / `~/snapB`) and byte-diff them on the
  G4:

  ```sh
  g4 ssh python '~/snapdiff.py ~/snapA/f006050.snap ~/snapB/f006050.snap'
  ```

  (`port/tools/snapdiff.py`, Python 2.5-safe on purpose.) `--snapdiff` in both
  runs is the cheaper first pass: a digest table every 50 frames, per 64 KB of
  MEM1, per global range, and per registry entry *by name*. The first line that
  differs names what was not carried — that is how all four of §24.4's bugs
  were found.

---

## 0d. Running the lab from littlejelly *(M16, 2026-09-18)*

M16 ran on littlejelly (the Linux lab host on the G4's wired LAN) rather than
on the Mac. What is different there:

* `~/bin` is not on a non-login shell's PATH: `export PATH=$HOME/bin:$PATH`
  before `g4 …`, or call `~/bin/g4`.
* The build loop is `port/build-ppc.sh -j4 && port/tools/g4_debug_sync.sh &&
  sh port/tools/make_bundle.sh && sh port/tools/g4_install.sh
  port/build-ppc-darwin/MarioParty4.app` — Docker is native, an incremental
  build is about a minute. The host build (`make -C port`) wants clang, which
  is not installed; do not use it.
* `g4_install.sh` streams the bundle with GNU tar, whose flags are not
  bsdtar's; fixed in M16 (the metadata flags only on Darwin).
* **littlejelly is a laptop with a lid.** On 2026-09-18 someone closed it at
  14:43 (deep suspend, 25 minutes) and the wired link (`tg3 enp1s0f0`) had
  already gone down at 14:32; every background task on the host froze, and
  the whole 192.168.0.0/24 — the G4, the MacBook bench — was unreachable for
  the rest of the afternoon. `ip link show enp1s0f0` (want `LOWER_UP`)
  before blaming the G4; the G4 keeps running whatever the runner was given.
  Keep long chains on the G4 side and read results afterwards, and do not
  arm anything on the host that must fire on time.
* git has no global identity there; the repo carries the fork's
  (`user.name zachxjack`) as a per-repo setting.

## 0e. A chain the host cannot interrupt, and the real-time witness *(M17, 2026-09-18)*

* **G4-side chains.** `~/MarioParty4-chain.app/Contents/MacOS/isle` on the G4
  is a *shell script*; `g4 use MarioParty4-chain.app; g4 run` makes the
  console runner execute it in the Aqua session, so a sequence of A/B walks
  (each `"$HOME/MarioParty4.app/Contents/MacOS/isle" $ARGS > ~/ab-m17/NAME.log`)
  runs to completion whatever happens to littlejelly's lid or wire. `g4
  stop` kills the *game* (`killall isle` matches the binary, not the `sh`),
  and the script then starts the next one — to stop the chain, `kill` the
  `sh` by pid first. A profile step is the game in the background, `sleep`,
  `sample isle 10 -file …`, `kill $pid`; the port answers SIGTERM with a
  soft reset and keeps running, so `kill -9` afterwards and `ps` as always.
  `g4 use MarioParty4.app` before the leave-behind soak.
* **Two bundles for a compiler A/B**: `port/build-ppc.sh -j4 TUNE=
  BUILD=build-ppc-notune` builds the old flags into a separate tree, and
  `sh port/tools/make_bundle.sh port/build-ppc-notune/marioparty4
  /tmp/MarioParty4-notune.app && sh port/tools/g4_install.sh
  /tmp/MarioParty4-notune.app` ships it next to the default one. Never copy
  a raw binary into a bundle: make_bundle rewrites the SDL install name
  (`dyld: Library not loaded: /work/panther-sdl2/...`, EXITCODE=133).
* **The real-time witness** is the §21.1 walk with `--realtime` instead of
  `--turbo` plus `--perfdump ~/rt.csv`; the `realtime:` line under each
  `--perfwin` window is the speed, and `port/docs/PLAN.md` §32.1 shows how
  to read the CSV (consumed and drawn frame costs, skips between draws).
  Frame numbers in `--dumpframe` are drawn-frame numbers either way, so the
  §31 md5s are the check. `--lockstep` is the old gate on the same binary.

## 0f. Three things M18 paid for *(2026-09-19)*

* **`--dumpframe` without `--shotdir` writes to the runner's cwd, which is
  `/`.** `ls /frame-0*.ppm` is where a run's frames went if the log says
  `wrote ./frame-00800.ppm`; a second run overwrites them. Always give an
  A/B arm its own `--shotdir ~/ab-mNN/<arm>`.
* **`g4 run` keeps one previous log (`~/isle-log.prev.txt`).** A soak's
  shutdown report — the texture cache line, the palette counts, the skin
  and mixer statistics — is gone after two more runs. `g4 ssh 'cat
  ~/isle-log.txt' | gzip > port/docs/soak/<name>.log.gz` *before* the next
  `g4 run`.
* **A `.wav` md5 is not an oracle across builds** (PLAN.md 33.4): two
  builds differing only in dead code produce different bytes from retrace
  1,580. Compare audio on one binary with a runtime lever, or per sample
  (`--mixcheck`). For game-state divergence between two arms use
  `--snap-at F` in both and `python ~/snapdiff.py A B --spans=60` on the G4
  — it names the global (it named `MTXBuf`).

## 0g. Four things M19 paid for *(2026-09-19)*

* **A snapshot is the binary's, full stop.** The build id hashes the
  executable's size and mtime, and MEM1 holds code addresses (process
  callbacks, coroutine LRs), so a snapshot cannot be restored into a
  rebuilt binary even when the snapmap is identical. `--restore-lax`
  exists for a re-link of the *same* source only. To reproduce a crash
  from an old build's ring, reproduce it on that build — or, as M19 did,
  re-take the ring on the new build with a lever that keeps the old
  behaviour (`--noskinlifetime`) and make it deterministic with `--ffto`
  to just before the fault frame.
* **Restores had no memory card until M19.** Every `--restore` since M10
  ran without a card (the image was allocated by the game's `CARDInit`,
  which a restore never runs), which only shows at a save — the results
  screen after a minigame stops on "No valid Memory Card is inserted"
  and the navigator cannot dismiss it. Fixed in `card_file.c`; a restored
  card's saves stay in memory (`CARD: the restored card has no file in
  this process`).
* **`--ffto N` was lockstep to the end** until M19 (the skip borrowed
  `--turbo` before frame mode read it). Teleported real-time measurements
  before commit `8cf90b8a` are lockstep numbers.
* **Check `g4_install.sh`'s output.** Piping it through `tail -0` hid a
  failed install for two A/B rounds; the traces that "did not change" were
  the old binary's. `md5` the installed `isle` against the local bundle
  before an A/B (`g4 ssh 'md5 ~/MarioParty4.app/Contents/MacOS/isle'`).
  Also: the G4's clock runs a few minutes behind littlejelly's; do not
  read file times across the two.

## 0h. Three things M20 paid for *(2026-09-19)*

* **A stall with the loop alive is a state question first.** `m406dll`
  sat for 80,000 frames at 100% with nothing wrong in any port counter;
  the answer was one `s32` in the module's `.data` (PLAN.md §35.1). Read
  the module before suspecting the port: `gdb` on the file-local symbols
  (`powerpc-apple-darwin8-nm build-ppc-darwin/rels/<mod>.bundle` — the
  bundles are prelinked at fixed addresses, no slide, so `x/` those
  addresses directly; the G4's own `nm` cannot read them). Read twice
  with the module's own frame counter alongside so a rate, not just a
  value, comes out.
* **A `--restore` reproducing the stall proves the state is *in* the
  snapshot.** MEM1 and every module's `__data/__bss/__common` travel with
  it; the port's own registries and caches do not. The addendum's
  "restore clears it, so it is port-side" inverted this — the restore
  had stalled too, and a gdb poke was what played it out. Check the log
  for the poke before drawing that conclusion.
* **The old binary's snapshot on a rebuilt tree:** `git stash` the fix,
  `build-ppc.sh` (12 s), `make_bundle.sh` to a second name, `stash pop`,
  rebuild — the same-source re-link is `--restore-lax`'s one sound case
  (`~/MarioParty4-m19src.app` on the G4, the runner slot flipped with
  `g4 use`, absolute paths for `--restore`: the runner's cwd is not `~`).
  And a minigame the soaks never dealt (`m415`, 0 of 44 dealt) is a
  minigame nothing has tested: `--minigame` it on purpose.

## 0i. Four things M21 paid for *(2026-09-19)*

* **`--ffto N --realtime` used to sleep the fast-forward's lead out.**
  The retrace schedule advanced 1/59.94 s per retrace while ffto ran at
  160+ fps, so real time began with a pause of (game time − ffto wall
  time): 154 s on the instruction screen for `--ffto 14400`, and the
  process sat at 3% CPU looking hung. Fixed (`port_vi_rebase_schedule`);
  any teleported real-time number before commit M21 that was read *soon*
  after the handover was read during that sleep.
* **A snapshot from an intermediate build is gone with the build.** M21
  took the results-screen and the paper rings on a build it then
  overwrote; both `--restore`s are refused now. Either keep the bundle
  under another name (`cp -R ~/MarioParty4.app ~/MarioParty4-mNN.app`
  *before* installing over it — M21 did this for the M20 build and not for
  its own) or record the 2-minute `--ffto` reproduction instead, which is
  what PLAN.md 36 does.
* **A `--minigame NAME` run without `--play` sits at the title.** The
  navigator only comes with `--soak`; a `--ffto` to a minigame frame needs
  `--play board-start-com4.play` on the line or the frame dumped is the
  title screen (M21 lost one round to it).
* **`g4 shot` has a 45-second window for a minigame at real time.** The
  status line's frame number tells you where the run is; Stamp Out! runs
  from 14,477 to 17,178 in a `--minigame m415 --turns 1` walk, i.e. from
  4:00 to 4:45 after boot. Poll every 20 s once `w01dll` shows, not every
  75.

## 0j. Four things M22 paid for *(2026-09-19)*

* **Do not start an A/B walk right after `g4_install.sh`.** The install
  rewrites the 20 MB bundle and the G4 spends the next half-minute on
  it; two walks started within a minute of one read 20.8 and 23.0 fps on
  the title window against 27–28.6 for every other run (PLAN.md 37.5).
  Ninety seconds, or a throwaway run, first.
* **The M21 "mergeable" count hashed an address.** `gx_bound_tex()`
  returns `&gx.bound[unit]` — the same pointer whatever is loaded — so a
  signature that mixes the *return value* never sees a texture change.
  Hash the object's contents (PLAN.md 37.2). The same trap waits in any
  future instrument that keys on a "bound" thing.
* **A white surface can be two draws.** The results portraits were the
  frame (a 216-vertex mask/reflection quadruple) *and* a 16-vertex face
  quad drawn after it with a different register shape; a per-unit debug
  cycle on the first draw changed nothing, and the GL trace (`--gltrace
  F`) showing the units programmed exactly as designed was what said
  "look at the next draw". `--drawlog` prints `creg`/`areg` and the
  konst colours now; `--dumptex` slot numbers are cache slots, not the
  drawlog's `gl N`.
* **A setter that flushes at once cannot see a transient.** hsfdraw.c's
  material setup writes states no primitive is drawn under and comes
  back; making setters compare-first peels one layer at a time
  (GXInitTexObj → GXSetTexCoordGen2 → GXLoadTexMtxImm → the konst
  selects). The lazy flush sees through it and is still not faster,
  because the batch count was never the cost (PLAN.md 37.2).

## 0k. Four things M23 paid for *(2026-09-19)*

* **The MacBook is the place to diagnose a picture.** Stamp Out!'s paper
  needed six reproductions with `--dumpcopy`/`--dumptex`/`--drawlog`/
  `--gltrace` before the cause was in hand (PLAN.md 38.2); at seven
  minutes each on `mbp` they cost the G4 nothing, and the G4 ran the
  soak meanwhile. The G4 does the witness only. What the MacBook cannot
  say: anything about time (its read-back cost 2 ms where the Radeon's
  was 120), so the real-time run from boot on the G4 is part of every
  witness, not a formality — the first paper fix passed the picture and
  cost two resyncs.
* **`glTexImage2D(…, NULL)` is not a clear.** The padding of a
  power-of-two texture holds whatever VRAM held, per card and per run;
  anything sampled through a projection reaches it. Size with defined
  texels.
* **Read the shutdown report's own numbers.** `634 misses, 527
  re-uploads` had been in every walk's report since M17 and said "every
  texture is decoded twice" to anyone who divided (PLAN.md 38.3).
* **`--dumpcopy` and friends write files on the game thread**; a run with
  an instrument that writes is not a speed measurement, and its stalls
  are the instrument's. The Dolphin capture on this host needs `-C
  Dolphin.Core.EnableCheats=True`, and `kill` `pgrep -x dolphin-emu`
  afterwards, every time.

## 0o. Five things M26 paid for *(2026-09-20)*

* **The MacBook refuses `open` for a bundle with `LSRequiresNativeExecution`.**
  M25's plist key makes LaunchServices answer "incorrect executable
  format" under Rosetta, before `--force` is ever read; delete the key
  from the *bench copy's* Info.plist and `lsregister -f` it. `open` also
  reports `-10810` for any run that exits non-zero within a second
  (`--machinecheck` = 2, a usage error = 1): the real output is in
  `/var/log/system.log` under the bundle id. And `--log` takes a FILE —
  `--log --viewtexgen` silently eats the next flag, which cost one A/B arm.
* **The MacBook's picture is not the G4's for cluster shapes.** Slime
  Time's blobs (m402, `ClusterProc` morphs) render as spikes on the Intel
  driver under Rosetta on every bundle back to M24b; the G4 draws them
  round. A MacBook A/B says whether *a change* moved a picture, never
  whether the picture is right.
* **The oracle rig can run the console out of `HEAP_DVD`.** Forcing
  `mg_next` from frame 9,000 leaves the board's own roulette choice
  preloaded and `instDll` then loads the forced game's directory on top;
  m402 panicked (`dvd.c:75`, the allocation error) on Dolphin where
  m405/m406/m408/m415/m416 had not. The port's harness moves the value
  before the preload (PLAN.md 28.4) and never sees it. A panic leaves
  Dolphin sitting on a magenta screen with the 2,100 s budget running:
  `ffmpeg -f x11grab` of `:0` is the 2-second check.
* **A gallery chain runs 3 minutes a game on the G4** (`--ffto 14000` at
  ~150 fps, then 2,600 lockstep frames at 12–25): 63 games in 3 h 10 min,
  and the same chain again on a second build for a byte-diff. Two runs of
  the whole set is a night; plan the second before starting the first.
* **`--mgdump` / `--mgend` make the entry frame the harness's problem.**
  Every game entered at 14,477 or 14,478 on the `board-start-com4.play`
  walk, but the levers cost nothing and the chain never had to know.
* **Kill by exact name.** A `kill` of every PID whose arguments matched
  "queue.sh" — aimed at the oracle's own `queue.sh` — also took
  littlejelly's `~/bin/agent-queue.sh`, the brief runner this very
  session is a child of; the session survived (re-parented to init) and
  a resumer was armed to finish the runner's bookkeeping and restart it.
  `pgrep -x`, or the full path, never a substring.

## 1. m425 to its result screen — the M8 crash, witnessed

§18.2 proved the `unk_3C[6]` correction at the level of the instructions. This
is the run that says the minigame plays.

```sh
g4 run --rtc dolphin --card ~/m425.raw --freshcard --com4 --minigame m425 \
       --turns 10 --status --stuckwatch 90 --play board-start.play --turbo \
       --log ~/m425.log
g4 shot mp4-minigame-m425
```

**Expect:** the module is entered, four CPU players play it, and the run
reaches the minigame's result screen with no `port: fault:` line and no 16 MB
scribble. The old failure was a SIGBUS with a fault address in the tens of
megabytes past MEM1; §18.1's guard pages mean any recurrence now names the
region and the end it ran off.

**Evidence lands:** `g4 pull ~/m425.log port/docs/m8b-m425.log`, and the shot
as `port/docs/screenshots/mp4-minigame-m425.png` — the file §18.2 says does not
exist. Both go in the commit that closes the "m425 now plays to its result
screen — **not tested**" row of §18.7.

---

## 2. A short board through the results — the sprite guard, witnessed

§18.4's `HuSprBegin` guard converts the end-of-game SIGSEGV at `0x88888888`
into a named report. Either outcome is a result; the report is the more useful
one.

```sh
g4 run --rtc dolphin --card ~/board.raw --freshcard --com4 --turns 3 \
       --status --stuckwatch 90 --play board-start.play --turbo \
       --ovllog --log ~/board.log
g4 shot mp4-board-results
```

**Expect:** three turns of Toad's Midway Madness with four CPU players, the
results sequence, and the return to the menu. Watch `~/board.log` for

```
port> HuSprBegin: group G slot S names sprite M, which is not a live sprite
```

If it fires, that is the diagnosis §18.9 item 2 asks for — note `G`, `S`, and
the overlay `--ovllog` names either side of it, and look at the `omOvlKill`
around `result_seq.c:602`. If it does not fire and the crash still happens, the
reading in §18.4 is wrong and `HuSprCall` should range-check `data` itself.

`--turns 3` rather than 10 on purpose: the crash is at the results transition,
not at turn ten, and three turns is about fifteen minutes with `--turbo`.

**Evidence lands:** `port/docs/m8b-board.log`,
`port/docs/screenshots/mp4-board-results.png`.

---

## 3. The resampler A/B — the one measurement M8 never took

```sh
G4=1 port/tools/audio_ab.sh --frames 40000
```

**Expect:** two runs, four CPU players, identical but for `--resample1`, and a
table of both `aud` lines and both `--clickstat` counts. Twenty minutes.

**Read it like this** (§18.5, and the host numbers in §18.11 which this is the
real version of):

- `aud` **mean** is the number the 1.5 ms budget is about; `worst` is the one
  stutter is about.
- `clickstat` is §16.7's discontinuity count and the only reason the 4-tap
  filter exists.
- If linear is cheaper *and* clicks no more, linear becomes the default and the
  4-tap becomes the flag. That decision is pre-agreed; do not re-argue it,
  record it.
- **The comparison is only an experiment if both runs reached the same place.**
  The script prints the last status line of each precisely so this can be
  checked; if the two differ in screen, turn or module, the numbers are
  worthless and the run should be repeated.

**Evidence lands:** `/tmp/mp4-audio-ab/{4tap,linear}.log` on the Mac — copy
both into `port/docs/m8b-audio-ab.log` with the table at the top.

---

## 4. The six corrected modules, played once each

§18.10's corrections are proved as instructions and have never run. Each lives
in a minigame that `--minigame` can park the roulette on, so each is one
command. These are cheap; run them after the two that matter.

```sh
for m in m453 m443 m428 m449; do
    g4 run --rtc dolphin --card ~/ub-$m.raw --freshcard --com4 --minigame $m \
           --turns 10 --status --stuckwatch 90 --play board-start.play \
           --turbo --log ~/ub-$m.log
    g4 shot mp4-minigame-$m
done
```

**Expect:** each plays to its result screen. Two of the six corrections change
behaviour and are the ones to actually look at:

- **m453** — the score readout's six sprites (two score digits, two timer
  digits, two labels) must all still appear, and the module must exit without
  taking somebody else's `esprite[0]` with it. The visible symptom of the old
  destructor would be a sprite disappearing from the *next* screen, not this
  one, so watch the shot after m453 as well as the one during it.
- **last5 / ztar** are not minigames: the lottery ticket loop needs a board's
  last five turns, which is `--turns 5` played to the end, and `ztardll` needs
  the Bowser board. Both are a board run, not a `--minigame` run, and both can
  ride along on the soak below rather than costing their own session.

**Evidence lands:** four logs and four shots under `port/docs/`; then the
per-case "Untested" lines in `port/docs/decomp-struct-notes.md` get replaced by
what was seen, which is the thing that makes that note fit to offer upstream.

---

## 5. The soak — left running

```sh
g4 run --rtc dolphin --card ~/soak.raw --freshcard --soak --turns 10 \
       --play board-start.play --turbo --stuckwatch 90 --log ~/soak.log
```

**Expect:** boot, a full ten-turn board, every minigame the roulette deals,
logged in and out, for as long as it is left. §18.6's watchdog now hashes the
game's own progress rather than the overlay, so a healthy board no longer
reports itself stuck ten times per run.

Leave it overnight. In the morning:

```sh
g4 log 200 ; g4 pull ~/soak.log port/docs/m8b-soak.log
```

**Read for:** any `port: fault:` line (now naming a region and, if a guard, the
mapping and the end), any `HuSprBegin` stale-sprite report, any `stuck` line
with the limit it applied, and the list of modules entered — the soak is also
the widest test the six corrections will get, because it deals them at random
alongside the other 93.

**Evidence lands:** `port/docs/m8b-soak.log`, which is the file §18.7 says does
not exist.

---

## After

Update §18.7's evidence table: every row that says **not tested** either gains
a log and a screenshot or gains a new bug. Then `port/docs/decomp-struct-notes.md`
is either ready to offer upstream or has a case to remove.

## 0l. Five things M24 paid for *(2026-09-19)*

* **A MusyX header's `#pragma pack(4)` never closes.** Any port file
  that includes `musyx/synth.h` (directly or through `dspvoice.h`)
  before `port.h` sees a packed `PortOptions`, and since M20 that meant
  `musyx_mix.c` read `depop` as `--stuckwatch` and `resample4` as
  `--soak` (PLAN.md 39.1). `port.h` first, always; and read the report's
  own option lines (`resampler …, depop …`) against the command line.
* **A Makefile `-D` rename needs the object rebuilt.** `hardware.o`'s
  rule did not list the Makefile, so a rename added to it changed
  nothing until `hardware.c` changed — M24's aux hook was never linked
  in through a whole A/B chain. `nm` the object for the renamed symbol
  after touching the rule; the rule lists the Makefile now.
* **`--wav` on the worker meets the disc image on the disk.** A queue
  step's `fwrite` at a scene load waited behind the DVD reads for up to
  428 ms and the game thread waited at the join with it (4.1 s over a
  walk); a walk with `--wav` is an identity witness, not a speed one.
  The same for `--mixtrace` (40 MB of text per 1,400 frames).
* **The M23 bundle does not know M24's flags.** A chain step that passes
  `--predecodelog` to `~/MarioParty4-m23.app` exits 1 in a second and the
  chain moves on; check every `EXIT=` line before reading a comparison.
* **A real-time divergence with identical lockstep traces is a
  timing-dependent input.** The first differing `--mixtrace` line under
  `--realtime` (`retB` at F3862, the aux-B return) named the effect
  state the game swaps at a scene change (PLAN.md 39.6); the `--nodraw
  --turbo` runs had been identical because the worker always finished
  before the game's frame reached the swap.

## 0m. Three things M24b paid for *(2026-09-20)*

* **A copy's rows are GL's, whatever its origin is.** `glCopyTexSubImage2D`
  puts the region's *bottom* row at `t = 0`; flipping the source
  rectangle's origin (`480 - (st + sh)`) does not flip the rows inside
  it. Every EFB copy sampled with the game's coordinates was upside down
  from M3 to M24 (PLAN.md 39b), and the consumers that showed it were
  the ones nobody watched: a 30-frame crossfade at the start of every
  board turn, and a minigame dealt three times in soaks with no picture
  taken. `--copylog` now names every copy on a walk; a `--dumpframe` of
  the frames after each one is the cheap check.
* **A soak's status line cannot see a picture fault.** Soak 13 read
  100% speed and 17–20 fps through the whole of m416 while the screen
  was a mirror. The realtime soaks are watched by numbers; a minigame
  the soaks keep dealing deserves one `g4 shot` at least once, and the
  console frames for it in `port/ref` before anyone reads the port's.
* **`--gltrace` with `--drawlog` faulted under Rosetta** (the M23 bundle
  on the MacBook, `signal 11 at 0x0` at draw 368 of the traced frame);
  the same instruments on the G4 did not. The trace before the fault
  was enough. Also: the Dolphin capture ran on 30 minutes past its 900 s
  again (§0k's `kill` note stands: `pkill -x dolphin-emu` by hand).

## 0n. Five things M25 paid for *(2026-09-20)*

* **The MacBook bench needs `--force` from M25 on.** The machine check
  refuses the PowerPC binary under Rosetta (`sysctl.proc_native` reads 0
  there; the key does not exist on a PowerPC kernel, which is taken as
  native), so every `open -a ~/MarioParty4.app --args …` and every ssh
  exec on `mbp` carries `--force` now — or `--machinecheck`, which exits
  before the refusal. CGL makes a context from a plain ssh exec on 10.6
  too, so `--machinecheck` works headless on both machines.
* **The config file is shared by every run on a machine.** `--fullscreen`
  is remembered in `~/Library/Application Support/MarioParty4/config`
  and the next run *without the flag* comes up fullscreen (that is the
  point; PLAN.md 40.6). A lab run that must not inherit anything passes
  `--noconfig`; a witness that set fullscreen ends with a `--windowed`
  run. The soak command does neither and is fine: nothing it does writes
  a key (the `machine` summary is written once per verdict).
* **The verdict is in the window title only until the first drawn frame,
  which is under a second from the window.** Photograph it with
  `--nodraw` (no frame is ever drawn, the title stays); a real run's
  title is plain `Mario Party 4` by the time `g4 shot` lands.
* **A CFUserNotification dialog on the G4 is dismissed with `osascript
  … set frontmost of process "UserNotificationCenter" to true … key code
  36`.** `click button "OK"` needs assistive access, which is off, and
  `g4 key` targets the `isle` process, which does not own the dialog.
  The Navigation Services chooser (the disc image dialog) did NOT answer
  Escape or ⌘. from System Events at all: kill the process
  (`kill -9`, then `mv ~/MarioParty4.hold ~/MarioParty4` if the disc
  folder was hidden to trigger it).
* **Escape ends a run with its reports.** `g4 key esc` → the port's soft
  reset → `port_shutdown` → every report line and `EXITCODE=0`, where
  `g4 stop` (`killall`) writes none. A soak read that way has the
  `worker mixer` line, the `split frames … position mismatches` line
  (M25's finding, PLAN.md 40.1) and the texture-cache totals. Check `ps`
  afterwards anyway.

## 0p. Five things M27 paid for *(2026-09-20)*

* **A GL context can live on a pthread under SDL 2.0.3 on 10.5 and 10.6.**
  The Cocoa backend's `SDLOpenGLContext` keeps an atomic dirty flag so
  that `SDL_GL_MakeCurrent` / `SDL_GL_SwapWindow` run the deferred
  `update` on the thread that uses the context while the main thread
  only schedules one; `SDL_GL_MakeCurrent(window, NULL)` on the main
  thread, `SDL_GL_MakeCurrent(window, ctx)` on the render thread, and the
  swap's `SDL_GL_GetCurrentWindow` check is per-thread TLS.  The main
  thread keeps `SDL_PollEvent` and the window title.  Witnessed on the
  Radeon 9000 (frame 800 `0b58c5ee` in `--renderthread 3`) and on the
  MacBook's Intel driver.
* **The ring's reuse has two fences, not one.**  The direct path finished
  the GPU fence before writing a chunk again; with a render thread the
  writer must also wait for the *issue* (the replay past the position the
  chunk was left at) — and the GPU fence still has to be *finished* before
  the CPU writes, which the replaying side can only report back (a per-
  chunk epoch the reader publishes when its test passes).  The first
  build waited for the issue alone and would have let the writer overwrite
  vertices the GPU was still DMAing; the fix is `rt_ring_enter` (PLAN.md
  42.1c).
* **A record-per-call instrument is not free on the replaying side.**
  `--gxsplit`'s class timing on the replay — two `mach_absolute_time`
  reads per record, ~2,800 records a frame — inflated the MacBook's inline
  walk from 355 to 447 s before it was moved behind `--rtsplit`.  Anything
  timed per record needs its own flag.
* **`--frames N` ends before frame N is presented.**  `--frames 7001` wrote
  frames 800 and 3000 and not 7000 (the present is at the top of the next
  retrace); the walk that wants 7000 is `--frames 9000`, as §31.2's was.
* **`pthread_cond_timedwait` wants the epoch clock.**  `port_now_seconds`
  starts at zero for the run (clock.c); a timespec built from it is 1970
  and the wait returns at once — a busy loop where a doze was meant.
  `gettimeofday` for the condvar, `port_now` for everything else.

## 0q. Four things M28 paid for *(2026-09-20)*

* **A `cp -R` of a bundle on the G4 during a real-time soak is a 3-second
  dip.** Keeping the M27 bundle under another name (`cp -R ~/MarioParty4.app
  ~/MarioParty4-m27.app`, 20 MB) while soak 18 ran read as three status lines
  at 18.7 / 12.2 / 9.5 fps and `speed 87%` at frames 139,200–139,320, then
  `118%` catching up — the same shape as §0j's install rule, from a copy
  instead of an install. The soak's read (PLAN.md 43.1) marks it; anything
  that touches the G4's disk waits for the soak to end.
* **The cross toolchain's `mtxtest` target links and then fails in
  `dsymutil`** ("unable to get target for ''"): the binary is complete
  (`build-ppc-darwin/mtx_test`, run it on the G4 as `~/mtx_test_m28`); only
  the dSYM step dies. Since M18 the test also needs `port_opt` and
  `port_log` defined — tests/mtx_test.c carries stubs now.
* **`--nodraw --ffto N` drew past N until M28.** fastfwd.c's end switched
  the renderer on whatever `--nodraw` said (`--ffto` sets `nodraw` itself),
  so the first "consumed-frame" profile of the day was a drawn one at 35
  fps — the status lines' fps after N is the tell (a consumed board frame
  runs at 160+). Fixed (`nodraw_user`); the M27 bundle still has it, which
  is why the chain's `PN` walks to the board under `--nodraw` instead of
  teleporting. And `sample isle 10` halves the fps for its ten seconds
  (160 → 85 on consumed frames, 36 → 18 drawn), which is how the sampled
  window is read off the status lines afterwards.
* **A snapshot's differing spans are named by the link map, not by
  `snapdiff.py`**: `port/build-ppc-darwin/marioparty4.map` (the address
  column) turns a `first at (addr 0x...)` into a symbol; hsfdraw.c's
  statics sit together in one `.bss` run, which is how "the walk's own
  residue" is told from a game global.

## 0r. Six things M29 paid for *(2026-09-20)*

* **The G4 lost power at 18:21 and came back with its clock at 1969.**
  No panic log, no plug configured on the hub, the RTC reset (a warm
  reboot or a panic keeps it; a power cut with the dead PRAM battery does
  not), the journals replayed: a power interruption mid-walk. The runner
  restarts with the auto-login, so `g4 run` works again after `sudo date
  $(date +%m%d%H%M%Y.%S)` from littlejelly — and *then* `periodic
  weekly` fires because the clock jumped, and its `locate` rebuild
  (`find -s /`, as `nobody`) takes half a core for a quarter of an hour.
  `ps auxww | sort -k3 -rn | head` before any speed run that follows a
  clock change; two real-time walks (`R2a`, `R2b`) read a frame low
  because of it.
* **`g4_debug_sync.sh` during a real-time run costs the next quarter
  hour.** It ships ~300 MB of `.o` files to `~/mp4-work`, and Spotlight
  indexes them for as long after; three walks (`R2d`–`R2f`) ran under
  that and one stalled 5 s on a disk-bound frame. The sync is part of
  the install now — do both, then wait for `mds`/`mdworker` to leave the
  top of `ps`, not just the 90 s of §0j. `~/mp4-work` and `~/m29` carry a
  `.metadata_never_index` marker from today on.
* **`cp -R` of a bundle breaks its snapshots.** The build id hashes the
  executable's size and *mtime* (§0g), and `cp -R` gives the copy a new
  one: `~/MarioParty4-m28.app` refused `m28-results-stall-f014150.snap`
  by a build id though the bytes are identical (`md5 04a6764f`).
  `cp -Rp` when keeping a bundle for its snapshots; `--restore FILE
  --restore-lax` is the sound answer for a byte-identical copy (the
  range table is still checked).
* **A chain's run names are per build, not per arm.** The M29 chain
  re-ran `T2` on the final build and overwrote the flag build's `T2.log`
  on the G4 before it was pulled; the flag build's stage-2 turbo numbers
  survive only in the console transcript (PLAN.md 44.4). Pull the logs
  *before* starting a chain that reuses a name, or name them by build.
* **A suffix letter in a run name is an arm.** `R2i` was meant as the
  ninth real-time run and `arm_flags` read the `i` as `--renderthread 1`
  (the inline twin). A useful accident — it is the single-core witness
  of the record — but read the `args=` column of `index.txt` before
  reading a number.
* **A clock that wraps the `fwrite` and not the `fopen` cannot see a
  truncate.** M23's `CARD: image flush took` timed the write and the
  close; the 1.7 s was in the `fopen(path, "wb")` before them (PLAN.md
  44.6). Time the whole operation, and print the split.

## 0s. Five things M30 paid for *(2026-09-20)*

* **The bench is where a picture cause gets read.** With the G4 soaking,
  forty-one two-minute `open -W` runs on the MacBook (`~/m30/run1*.sh`,
  one bundle per build) did every drawlog, copy dump, texture dump and
  A/B of the day; the G4's part was the two turbo walks and a 15-game
  gallery (50 minutes). A drawlog is the port's own account of what it
  submitted and is the same on both machines; `--cpuxf` prints the
  transformed vertex positions, which the GPU path cannot. The G4 judges
  the picture (§0o still holds: the blobs are spikes under Rosetta).
* **`--drawlog-at F` needs the run to present F.** `--ffto 14870 --frames
  14880` never presents 14877: the present count runs about six behind
  the retrace count after a `--ffto` (PLAN.md 41b.2), and `--frames`
  ends before its last frame is presented (§0p). `--ffto F-80 --frames
  F+25` is the shape that works.
* **`snaps/lib/m458-fault-f015200.snap` was not the M26 gallery bundle's**
  (build `abbd973e` against `d2767188`); `--restore-lax` took it and the
  restored run faulted at the §41 frame. And gdb must be attached in the
  *same* ssh command as the wait for `resuming the game` — the fault is
  six seconds after the restore, less than a second ssh round trip plus
  a sleep.
* **`--dumpframe` into a directory that does not exist writes nothing**
  (`--dumpframe: cannot write …` once per frame, the run otherwise
  fine); the gallery chain `mkdir -p`s, a hand-run must too. A run of
  three minutes lost.
* **`scp host:a host:b local/` hangs** (it tries a remote-to-remote
  copy); one remote path per scp.

## 0t. Five things M31 paid for *(2026-09-21)*

* **Read the stub report before reading the game.** Every run ends with
  `---- SDK surface hit at boot: N distinct stubs ----`; m417's said
  `C_QUATMultiply 2532` and the whole "logic divergence" was there. M30
  read the water shader, the play's end and m430's column for a day
  with the answer on the last screen of each log. `gen_stubs.py` makes
  a loud stub of anything the link cannot find, including a `static
  inline` the mirrored header does not declare (`GXUnknownu16`, typed
  `long f(void)`), and a stub returns without writing its outputs.
* **A bundle made on littlejelly carries `LSRequiresNativeExecution`**,
  which the Intel MacBook honours: `open` says "incorrect executable
  format" and nothing runs. M30's bench bundles had the key deleted by
  hand. `sed -i "" /LSRequiresNativeExecution/d Info.plist`, and
  LaunchServices caches the old plist by path — copy to a new name and
  `lsregister -f` it, or the deletion changes nothing.
* **An edit after the build is not on the G4.** The J fix (portMessTag)
  went in after the 06:22 build and before the 06:51 install with no
  build between; the gallery's m435 row came back without the name and
  the day lost a re-install and a re-run. `port/build-ppc.sh` is the
  first word of the install line, every time.
* **`GL_TEXTURE` is 0x1702.** Reading a `--gltrace` line
  `glTexEnvi pname 8580 v 1702` as "SOURCE0_RGB = PRIMARY_COLOR" cost
  twenty minutes; 0x8577 is PRIMARY_COLOR, 0x8578 PREVIOUS, 0x8576
  CONSTANT, 0x1702 TEXTURE.
* **The G4's disk reads at 13 MB/s** (`dd` of the disc image, page cache
  cold, `mds` idle): a shingled 2.5" FireCuda on the Quicksilver's ATA
  bus. A cold 1.3 MB module read is 100 ms, a rename over an existing
  file up to 1.8 s. Not a fault and not a setting; anything that must
  be quick stays in the page cache or on a thread.

## 0u. Six things M32 paid for *(2026-09-21)*

* **LaunchServices hands a double-clicked app `-psn_0_NNNNN`.** The
  first Finder launch of the bundle printed the usage and exited on it
  (`open` says `LSOpenURLsWithRole() failed with error -10810`, which is
  "the app exited at once", not a launch failure).  The parser skips
  `-psn_*` now; a runner launch never passes one, which is why thirty
  milestones never saw it.
* **The Navigation Services chooser answers a real click, not a
  keystroke, when its process is not foreground.** M25 could not drive it
  from System Events because a shell-launched process is not a foreground
  application until SDL makes it one, and the dialog runs before SDL.  The
  chooser now `TransformProcessType`s itself, and a keystroke reaches the
  *first* dialog; a second dialog after a CFUserNotification does not get
  key focus until something clicks it.  `~/click.py X Y` on the G4 posts a
  Quartz mouse click (Leopard's PyObjC, Python 2.5) and `~/key.py CODE
  [HOLD]` a held key -- an osascript `key code` is released within a
  millisecond and the game samples the keyboard once per retrace, so it
  never sees a `keystroke`; F5/F12 go through the event queue and do.
  `g4 key`/`g4 keys` are fine for the CFUserNotification dialogs
  (UserNotificationCenter, key code 36 on the frontmost process).
* **F12 is Dashboard on Leopard.** The screenshot key never reached the
  game, Dashboard came up, and SDL 2.0.3 minimised the fullscreen window
  on the focus loss with nothing to bring it back (the wrapper app had no
  Dock icon).  The port sets `SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS` to 0
  and takes F5 as well as F12; a fullscreen game that loses focus stays on
  screen.
* **A fresh user is `HOME`, not a new account.** `~/MarioParty4-fresh.app`
  is a wrapper whose executable is a shell script exporting
  `HOME=/Users/zach/mp4-fresh-home` and exec'ing the real
  `~/MarioParty4.app/Contents/MacOS/isle` with stdout to
  `~/mp4-fresh.out`; `open ~/MarioParty4-fresh.app` from ssh is a
  LaunchServices launch (`-psn`, foreground, the Dock).  The wrapper's plist
  needs its own `CFBundleIdentifier` and an `lsregister -f`
  (`/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister`
  on Leopard).  The player's own Application Support was never touched.
  A test home has no `Library` and no `Desktop`: the port creates every
  level now.
* **`screencapture` from ssh works on the MacBook (10.6)**, unlike the
  G4's Leopard, so the MacBook's fresh-user walk photographs itself.  Its
  `open` needs the bundle's `LSRequiresNativeExecution` removed and the
  binary `--force`d (the wrapper does both) -- and `open` on 10.6 also
  passes `-psn`.
* **`make_dmg.sh` builds on the G4 and fetches with `ssh cat`**: scp
  mangles a remote path with spaces (`Mario Party 4 PowerPC Edition
  0.9.dmg`), and `hdiutil` is a Mac tool; the staged folder goes over as
  a ustar stream, the image comes back through a pipe.

## 0v. Five things M33 paid for *(2026-09-21)*

* **`--forceobj` never matched a draw on its own.** The pointer the
  object names come from (`gx_last_posmtx_arg`) was captured under
  `--drawlog` only; every M31/M32 `--forceobj` run happened to carry
  `--drawlog`.  Three bench runs of a probe that printed nothing before
  the trace of names said "(null)" for every draw.  Captured under
  `--forceobj`/`--skipobj`/`--probeobj` now.  A diagnostic's first run
  should prove it fires (a count line) before its result is read.
* **A forced GL state has to go through the shadow.** `--forceobj` bit
  32 (the z write) set the shadow's `depth_mask` and then emitted
  `gx.z_update` — the probe's `DEPTH_WRITEMASK 0` line was the only thing
  that said so.  Emit the shadow's value, and read the probe's state
  block before trusting a picture.
* **A per-call trace with `dladdr` under `--ffto` is a 50 MB log and a
  ten-minute run.** Gate a trace on drawn frames (`!gl13_draw_off()`); the
  fast-forward runs the same GX calls with nothing drawn.
* **`--frames` after `--ffto` needs ten retraces before a frame is
  presented** (the present count runs behind, §0s): `--ffto 14500
  --frames 14504` drew nothing; `14508`/`14510` draws two to four frames.
* **`scp` to the MacBook takes half a minute a file** (its disk throttles:
  `ThrottleProcessIO`); a run's frames come back faster as one `tar`
  stream than as four `scp`s, and a bench script that copies inside the
  120 s tool timeout goes to the background.

## 0w. Six things M34 paid for *(2026-09-21)*

* **The probe's read-back needs the context: `--norenderthread`.** Under
  the render thread the game thread's `glGetIntegerv(GL_VIEWPORT)` is
  `0 0 0 0` and every draw is "nothing in the box" — a 10-minute G4 run
  that said nothing (`~/m34/S417p.log`, the first one).  The M33 comment
  ("the twins are joins, so it works under the render thread") was about
  the *drawlog*; the probe reads pixels.
* **A hook's draw has no object name.** `--skipobj` / `--probeobj NAME`
  cannot reach a `Hu3DHookFuncCreate` draw (m404's line, m417's water and
  particles); the lookup names it after whatever `DrawObjData` the stack
  matrix happens to fall near ("para-c" for the water).  `--skipverts N`
  and `--probeverts N` are the two knobs that were missing; the vertex
  count is the name.
* **The probe's frame numbers run about two behind `--ffto`.** `--ffto
  15674 --frames 15679` probed 15,672 alone and never wrote the 15,677
  dump; `--ffto 15676 --frames 15684` covered 15,674–15,677.  Widen the
  window by four on each side.
* **A "same play, other machine" difference is the GL driver until proven
  otherwise — and NaN is the first thing to look for.** The bench and the
  G4 ran the same binary and the same play; the only difference was the
  driver's answer to a NaN vertex (Intel drops it, the Radeon draws it at
  the screen centre).  Four arms (`nrt`, `noav`, `cpuxf`, `skipobj`) were
  spent on the threading and the transform before the probe's vertex
  dump (with the count raised past six) showed the NaN in the data.
* **The host's libm is not MSL.** `sqrtf(negative)` is the argument on
  the console and NaN on the port; the mirror's "replace MSL's libc
  headers with the host's" was right for 33 milestones and wrong for one
  line of m417.  When a game value goes NaN and the arithmetic has no
  division, look at the library functions between the C and the console.
* **`kerent.c` declares every libm name as `void f(void)`.** A header
  forced onto every game unit may not include `<math.h>`; a bare macro
  does the redirect and the host math.h's own prototype carries it.

## 0x. Six things M35 paid for *(2026-09-21)*

* **The lab can vanish under you: littlejelly's wired NIC dropped its
  link for forty minutes** (`tg3 … Link is down`, the PHY advertising
  10baseT alone), taking the G4 and the mbp with it, and there is no root
  on littlejelly to reset it.  `ip link show enp1s0f0` first when
  everything times out at once; the reading goes on from the code and
  the console frames, and the builds can be made blind
  (`port/build-ppc.sh` is local) and proved when the link returns.
* **`g4 stop` ends the runner's job, not a chain script's subshell.**
  `sh gallery_chain.sh` kept running through the bisect started after
  it; its `killall -9 isle` and mine killed each other's games (seven
  rows to rerun).  Kill the `sh` by pid (`ps aux | grep gallery_chain`),
  then the isle.
* **`port_fatal`'s dialog hangs an unattended G4 run.**  The m433 fault
  sat in its dialog for nine minutes inside the gallery chain.  Every
  chain that runs unattended watches its logs for `port: fatal` and
  kills the isle by pid (`m35_wave2.sh`'s `wrun`, `m35_final.sh`'s
  watcher).
* **The mbp needs the bundle's `LSRequiresNativeExecution` key gone**
  (the M32 packaging added it; Rosetta refuses the app with "incorrect
  executable format"), and LaunchServices caches the refusal by path:
  rename the bundle (`MarioParty4-m35b.app`) rather than edit it in
  place.
* **The mbp is not the G4 for the texture cache and for the six-unit
  chains.**  Its sampled hash caught m415's and m404's canvases by where
  its windows fell; m448's felt is green there and black on the Radeon
  with the same TEV; the cluster shapes spike (M26).  A mbp A/B says
  whether a lever moved a picture; the G4 says whether the picture is
  right.
* **The Dolphin dump index runs ~351 frames behind `gc`** (M26b's picks:
  `S_c = entry_gc − 351` for every game); a trimmer that keeps frames by
  `gc` keeps the wrong ones (four games re-captured).  Keep ±40 around
  `entry_gc − 351 + {64, 404, 1204, 2304}`.
