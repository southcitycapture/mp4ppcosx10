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
shape for anything new. The coroutines are all parked in `HuPrcVSleep`, so
the interesting frame is the *caller*, at `*(*(jump.sp) + 8)`.

The loop: `port/build-ppc.sh -j8 && port/tools/g4_debug_sync.sh && g4 push-bin`.
A binary and its `.o` tree must come from the same build, or gdb reads the
wrong lines.

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
