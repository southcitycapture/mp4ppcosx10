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
