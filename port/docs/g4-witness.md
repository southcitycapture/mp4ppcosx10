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
tailscale status | grep littlejelly
```

**Expect:** a line with an IP and no `offline`. `littlejelly` is the ProxyJump
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
