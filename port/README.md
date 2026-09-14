# Mario Party 4 on the Power Mac G4

This directory is a native PowerPC port of Mario Party 4, built on top of the
`mariopartyrd/marioparty4` decompilation without editing a single line of it.
The decomp's own sources — the DOL's engine and board game, all 99 relocatable
modules, Hudson's `msm` sound manager, and the pure-C half of the Dolphin SDK's
matrix library — are mirrored into a build directory, patched with a short list
of exact-text substitutions, and cross-compiled for `powerpc-apple-darwin8` with
GCC 14.2 against the MacOSX10.4u SDK, using the same Docker toolchain and Tiger
SDL2 build as the two Snowboard Kids ports. Everything the game asks of the
GameCube is answered by `src/`: a host loop that delivers pad input, disc
completions, ARAM DMA and audio at the point the game calls `VIWaitForRetrace`;
`OSLink` re-expressed as `dlopen` over one native shared library per REL, which
matches the game's own loader contract exactly; DVD as an extracted `files/`
tree with a real FST; CARD as two host files; PAD as SDL2 plus the existing
IOKit Xbox One driver; ARAM as a 16 MB block indexed by offset; and — the large
one — GX translated into OpenGL 1.3 fixed function on the Radeon 9000, which
turns out to fit, because 93 of the game's 121 `GXSetNumTevStages` sites ask for
a single TEV stage and it never names a stage above `GX_TEVSTAGE4`. Two things
that cost the N64 ports dearly are simply absent here: the G4 is big-endian like
the GameCube, so nothing needs byteswapping, and the game addresses memory
through ordinary pointers and ARAM through plain offsets, so there is no
pinned-globals scheme.

**Status: M5 in part — it plays a board, and a minigame.** The port boots
through SELECT A FILE, the new-file scene, PARTY MODE and character select,
starts **Toad's Midway Madness** with 1P and three COM players on EASY, plays
the board intro and the turn-order roll, takes turns with dice, item spaces and
the Star, picks a 4-player minigame, shows its instruction screen, plays it and
pays out the results. The theatre-stage backdrop M4 could not draw is there,
and so is the board: both were the same kind of bug twice over — a `C_*` body
in the SDK's own matrix library that the GameCube never called, that the decomp
is right to have written the way it is, and that `-DMTX_USE_C` makes the one
that runs. `C_MTXIdentity` leaves the translation column unwritten, which
`hsfdraw.c:mtxRot` then accumulates into tens of thousands over a frame;
`C_VECScale` is `C_VECNormalize` under the wrong name, which is `1/sqrt(0)` on
a zero vector and put a NaN in the board camera on the eleventh frame of every
board. `port/tests/mtx_test.c` now holds both fixed. What is left of M5 is
turns two to ten: the *second* minigame dies with SIGBUS inside its own draw
hook. See §15 of [`docs/PLAN.md`](docs/PLAN.md). Two binaries and two
sets of modules build from one Makefile: `port/build-ppc.sh -j8` produces a
`powerpc-apple-darwin8` executable plus 99 Mach-O bundles for the G4, and
`make -C port TARGET=host -j8` produces an arm64 pair for the development Mac.
Every relocatable module is one `dlopen`'ed bundle, which maps exactly onto
`objdll.c`'s own five-point loader contract; `--reltest` loads and unloads all
99 twice and reports 198/198 clean, with `dlclose` genuinely unloading each
one. `bootDll` runs its `_prolog` and its `ObjectSetup`. The GX layer is real:
all 114 entry points, vertex assembly and decode, the ten texture formats,
display lists recorded in the console's own byte encoding, and the TEV chain
compiled into OpenGL 1.3 fixed function on an SDL2 window.

The development Mac cannot render the game's logos, and the reason is worth
knowing before reading further: the game's file formats embed 32-bit fields
its own headers call pointers, so on a 64-bit host the structs are the wrong
size before endianness is even considered. That is free on the G4, which is
32-bit and big-endian like the disc. The host build is therefore a plumbing
harness, and `--gxdemo` is how the graphics layer is verified on it — it
drives GX with data the port builds itself and writes the frame out. See
[`docs/m2a-gxdemo.png`](docs/m2a-gxdemo.png), the M2a log in §10 of
[`docs/PLAN.md`](docs/PLAN.md), and the host-vs-G4 table in §10.7.

```sh
port/build-ppc.sh -j8                       # the G4 binary and its 99 bundles
make -C port TARGET=host -j8                # the development pair
port/build-host/marioparty4 --reltest       # load and unload all 99, twice
port/build-host/marioparty4 --gxdemo --shotdir .      # the GX self-test frame
port/build-host/marioparty4 \
    --image "orig/GMPE01_01/Mario Party 4 (USA) (Rev 1).nkit.iso" \
    --noaudio --frames 40 --dumpframe 30
```

`--image` takes the disc image directly (the FST is parsed at boot; an
NKit-trimmed ISO keeps every file at its original offset) or a directory
holding an extracted `files/` tree. Other flags: `--frames N`, `--watchdog SEC`,
`--turbo`, `--gxlog`, `--stub-trace`, `--deterministic`, `--seed N`,
`--verbose`, `--reldir DIR`, `--reltest`, `--relzerobss`, `--reldlclose`,
`--noaudio`, `--headless`, `--glcheck`, `--glinfo`, `--gxwarn`, `--gxdemo`,
`--perf`, `--drawlog N`, `--drawlog-at F`, `--dumptex`, `--texhash-full`,
`--dumpframe SPEC`, `--shotdir DIR`, `--scale N`, `--nocard`, `--nopad`,
`--paddbg`, `--play SCRIPT`, `--record FILE`, `--scenelog F[,F...]`,
`--ovllog`, `--nanwatch`, `--wav FILE`, `--mute`, `--audiolog`.

`--dumpframe` takes a frame *set*, not a frame: `187`, `1,90,186`, or
`1-400/20`. Comparing against Dolphin needs a spread, because the two sides do
not agree on absolute frame numbers -- the port skips the console's DVD seek
and the opening movie -- so you shoot a spread on both and find the matching
pair once.

Four flags exist because "the draw is right and the screen is black" has too
many causes to reason about from the source, and each of them is one line of
output:

| flag | answers |
|---|---|
| `--glinfo` | what the driver actually is: strings, twelve limits, every extension |
| `--drawlog N` | what the first N draws submitted: geometry after the CPU transform, raster colour, the texture actually bound, projection, TEV inputs, alpha compare, blend, z, scissor, and any pending GL error |
| `--dumptex` | what a texture decoded to -- colour as a PPM and alpha as a PGM, because an alpha test judges the alpha |
| `--perf` | where the frame went: game, gx and present, with mean/median/p95/worst, and the game clock against the wall clock (an idle-gated retrace hides overruns) |
| `--scenelog F` | what the 3D scene believes about itself on frame F: every camera, every model's placement, every HSF object transform -- which is how "the modelview is 67,720 out" became "the camera and the models are both fine and a second pass over the same objects is not" |
| `--ovllog` | one line whenever the scene changes, which is the only way to know which screen a scripted A press landed on without shooting the frame |
| `--wav FILE` | the mix, as a 32 kHz stereo WAV. Nobody can listen to the G4 over SSH, so this is how an audio claim is checked at all: peak levels, spectrum, and the frame the first sound arrives on, against Dolphin's own `dspdump.wav` of the same boot |
| `--mute` | every voice started, decoded, advanced and retired exactly as usual, and silence emitted. The frame costs the same, so this separates "the audio path broke it" from "the audio broke it" without moving the game's timing |
| `--nanwatch` | the first frame each camera, each model and the board's own `boardCamera` turns NaN, with the board camera printed for that frame *and the one before*. A NaN in a camera is completely silent -- nothing crashes, every comparison against it is false, and the 3D layer simply stops drawing -- so without this it looks like "the board does not render" |

## Running on the G4

The G4 test bench is driven by the `g4` helper from `isle-ppc-tools`, which
several PowerPC projects share: `~/isle.app` on the G4 is a symlink and
`g4 use NAME.app` points it at one of them.

```sh
port/build-ppc.sh -j8               # the PowerPC binary
port/tools/make_bundle.sh           # -> build-ppc-darwin/MarioParty4.app
port/tools/g4_install.sh --image    # bundle + the 598 MB disc image, first time
port/tools/g4_install.sh            # bundle only, every time after
g4 use MarioParty4.app
g4 run --watchdog 8
g4 log 60
g4 use SnowboardKids2.app           # hand the bench back
```

The executable inside the bundle is named `isle`, not `marioparty4`, because
the G4's console runner hard-codes that name; `make_bundle.sh` explains why.
The disc image lives in `~/MarioParty4/` on the G4 and the port finds it there
by itself — no `--image` needed — or you can build a self-contained bundle with
`make_bundle.sh --with-image`. M1 has no window, so the binary also runs
straight over SSH:

```sh
ssh g4 './MarioParty4.app/Contents/MacOS/isle --watchdog 6'
```

**What it does there today (M5):** the whole logo sequence, the title screen
**with its 3D characters and present boxes**, and then -- with an Xbox One pad
plugged in, or from a script -- SELECT A FILE, the new-file scene, PARTY MODE,
character select with 1P and three COM players on EASY **on its theatre stage**,
the board-settings screen, and then **Toad's Midway Madness itself**: the board
intro, the turn-order roll, dice, movement, item spaces, Toad showing where the
first Star is, a 4-player minigame with its instruction screen, and the results
screen that pays it out. It creates a save file on a 512 KB memory card image
on the way through and writes the game back to it. It also walks into
**Mini-Game mode**, which loads and runs `mgmodedll`. The opening THP movie is
skipped deliberately and says so. The board runs at about 14.9 fps and the
character select at about 14.6; where that time goes is measured, not guessed,
and the answer is the vertex path -- two thirds of the frame on both screens
(§15.5). It does not survive a whole ten-turn game yet: the second minigame
crashes in its own draw hook. See [`docs/PLAN.md`](docs/PLAN.md) §15.

```sh
# the walk into the board and its first minigame, replayed on the G4
g4 run --turbo --seed 12345 --frames 15400 --play board-start.play \
       --ovllog --nanwatch --gxwarn
```

```sh
# the reference menu walk, replayed on the G4
g4 run --frames 5200 --noaudio --seed 12345 --play menu-walk-port.play \
       --dumpframe 900,1200,1500,2700,3000,5100 --shotdir ~/mp4out
```

**What M1 did there:** booted, loaded `bootDll`, and ran the boot
sequence at 56.7 fps — 900 retraces in 15.88 s of wall clock for 1.17 s of CPU,
because GX still draws nothing. `--reltest` loads and unloads all 99 REL
bundles twice with 0 left resident after `dlclose`, which closes the plan's
second-largest risk on the only machine that could answer it. On big-endian
hardware `msmSysInit` succeeds where the host fails and MusyX initialises, the
game's own read of the REL headers off the disc is correct in every field, and
ARAM carries real transfers. The captured runs are
[`docs/g4-boot.log`](docs/g4-boot.log) (M1, with the field-by-field host diff)
and [`docs/g4-m2a-boot.log`](docs/g4-m2a-boot.log) (M2a); §10 of
[`docs/PLAN.md`](docs/PLAN.md) is the log entry.

## Self-play, and reproducible runs

Two things make a run of this port an experiment rather than an anecdote.

**`--rtc SECS`** pins the console's real-time clock, which is the port's whole
RNG story: the game has exactly two clock-derived seed sites -- `frand.c:13`
by way of `init.c:77`, and `BoardRandInit` at `board/main.c:1432` -- and both
read `OSGetTime` and nothing else. `--rtc` is `--seed` written in the units
Dolphin is already configured in, so `--rtc dolphin` (1041472800, the pinned
`CustomRTCValue` in `ref/dolphin-user/Config/Dolphin.ini`) puts the two rigs on
the same clock.

**`--card FILE` / `--freshcard`** pin the other input. The minigame roulette
reads the save file's played set, and every run rewrites the save, so before
this the same seed was not the same experiment twice.

Together they give byte-identical runs *with audio on* -- proved by md5, not
asserted:

```sh
g4 run --rtc dolphin --card ~/scratch.raw --freshcard --play board-start.play \
       --turbo --frames 2000 --dumpframe 400,800,1200,1600 --shotdir ~/det1 \
       --wav ~/det1.wav
```

Then the harness, which plays the game itself:

| flag | what |
|---|---|
| `--com4` | all four players are CPU. The game supports this -- mentDll's own attract loop sets exactly these fields -- and it is what makes the minigame instruction screen dismiss itself, because instDll auto-starts after 60 frames when all four players are CPU |
| `--minigame NAME\|ID` | park the roulette on one minigame: `--minigame m425dll`, `--minigame 425` and `--minigame 24` are the same thing. Reproducing a crash in a named module stops being a twenty-minute dice roll |
| `--turns N` | the board's turn count |
| `--status` | one line a second: screen, board, turn, the module the roulette dealt, coins and stars per player, `aud` ms and fps |
| `--stuckwatch SEC` | the live screen has not changed in SEC seconds: name it |
| `--soak` | all of the above, from boot, logging every minigame module entered and left, for as long as you leave it |

```sh
# reproduce a crash in one named module
g4 run --rtc dolphin --card ~/scratch.raw --freshcard --com4 --minigame m425 \
       --turns 10 --status --stuckwatch 90 --play board-start.play --turbo

# leave it playing itself overnight
g4 run --rtc dolphin --card ~/soak.raw --soak --turns 10 \
       --play board-start.play --turbo --log ~/soak.log
```

The harness never presses a button it can avoid pressing. It writes the game's
own globals -- `GWPlayerCfg`, `GWPlayer`, `GWSystem` -- from the retrace gate,
which is the lesson the two Snowboard Kids ports' `menu_nav.c` taught: name the
live screen and park the state the game would have set. `--play` is still what
walks the menus, because a metronome of A presses is enough once no screen
needs a *specific* button, and `board-start.play` is that metronome.

**Save-file handling.** Slot A is
`~/Library/Application Support/MarioParty4/memcard-slot-a.raw` unless `--card`
names another file. A soak should always be given its own image and
`--freshcard`, both so the roulette starts from a known played set and so a
crashed run does not leave a half-written save behind for the next one.

## Layout

| path | what |
|---|---|
| `docs/PLAN.md` | the plan and the engineering log |
| `docs/inventory.md` | generated: every SDK symbol the game calls, with counts |
| `docs/m1-boot.log` | the boot narration M1 reaches on the host, captured |
| `docs/m2a-boot.log` | the boot narration M2a reaches on the host, captured |
| `docs/m2a-reltest.log` | all 99 REL bundles loaded and unloaded twice |
| `docs/m2a-gxdemo.png` | the GX self-test frame, the graphics layer's reference |
| `docs/g4-boot.log` | the M1 boot on the real G4, with the host diff |
| `docs/g4-m2a-boot.log` | the G4 again once RELs loaded: --reltest, ARAM, 900 frames |
| `docs/g4-glinfo.log` | the Radeon 9000's own GL strings, limits and 77 extensions |
| `docs/g4-gxdemo.png` | the GX self-test as the real card draws it |
| `docs/g4-audio-first-sound.log` | the audio path on the G4 with audio on: the M6 hand-off |
| `docs/screenshots/` | both logos, the title with its 3D layer, the five menu screens, and the board and its first minigame |
| `ref/movies/menu-walk-port.play` | the reference menu walk, rebased on the port's own clock |
| `ref/movies/minigame-select.play` | the same walk, but taking the Mini-Game row of the mode ring |
| `ref/movies/board-start.play` | past the board settings into Toad's Midway Madness and its first minigame |
| `ref/movies/board-start.txt` | the same walk on the Dolphin side, for `tools/mkgecko.py` |
| `tests/mtx_test.c` | the matrix library against itself: `make -C port TARGET=host mtxtest` |
| `Makefile` | the whole build, `TARGET=host` or `TARGET=ppc-darwin` |
| `build-ppc.sh` | the Docker wrapper around the PowerPC cross build |
| `patches.txt` | every change the port makes to game sources, as exact text |
| `include/override/` | SDK headers the port replaces wholesale |
| `tools/make_bundle.sh` | wraps the PowerPC binary into `MarioParty4.app` for the G4 |
| `tools/g4_install.sh` | ships that bundle, and the disc image, to the G4 |
| `tools/inventory.py` | generates `docs/inventory.md`; re-run after any upstream merge |
| `tools/mirror_src.py` | builds the source mirror: patches, Metrowerks asm, overrides |
| `tools/widen_ptr_casts.py` | widens the game's pointer-through-`u32` casts, compiler-driven |
| `tools/gen_stubs.py` | generates one loud stub per unimplemented SDK symbol |
| `tools/gen_rels.py` | works out which sources go in which of the 99 modules |
| `tools/gen_kerent.py` | regenerates `kerent.c`'s 1,011 export thunks as assembly |
| `src/platform/` | `main`, the host loop, window and GL context, settings, argv |
| `src/os/` | OS shims, the `HUPROCESS` context switch, `PSMTX*` in C, cache no-ops |
| `src/gx/` | the GX state machine, vertex decode, texture decode, the GL 1.3 backend |
| `src/dvd/` | DVD over an extracted `files/` tree or a disc image; `host_data.c` is the one place every host-only divergence goes through |
| `src/relmod/` | the other side of the fence: compiled into every REL bundle, never into the main binary |
| `src/card/` | CARD over one 512 KB memory-card image in the console's own format |
| `src/pad/` | PAD over the IOUSBLib Xbox One driver, SDL2 and the keyboard; `--play` / `--record` |
| `src/audio/` | ARAM, the MusyX SAL replacement (`musyx_sal.c`), the CPU mixer that stands in for the `dspSlave` ucode (`musyx_mix.c`), MusyX's own ARAM allocator ported off its stubbed PC arm (`musyx_aram.c`), and the SDL output ring and `--wav` capture (`audio_out_sdl.c`) |
| `extern/musyx` | *not* part of the port, but compiled into it: AxioDL's MIT MusyX reimplementation, built straight out of the decomp's checkout with `MUSY_TARGET_PC`, unmirrored and unpatched. The Makefile drops the five files that are skeletons or Dolphin-only on that target |
| `src/debug/` | self-play, tracing, `--peek`, `--dumpdl`, `--perf` |
| `src/ui/` | launcher and in-game overlay |
| `scripts/` | input scripts and goldens |
| `ref/` | emulator reference frames and `.dtm` recordings |
| `resources/` | icons and bundle resources |

## Prerequisites

The decomp must build first — the port reads its symbol map and its config:

```sh
python3 configure.py --version GMPE01_01 --wrapper $HOME/.local/bin/wine && ninja
```

## Licence note

The port layer in `src/` and `tools/` is ours. The decompiled game sources it
builds are governed by the upstream repository's terms and contain no game
assets; MusyX is MIT (AxioDL). No Dolphin emulator code is read or copied —
Dolphin is GPLv2+ and this is not. `mariopartyrd/partyboard` and
`doldecomp/dolsdk2001` were read for reference; neither carries a licence file,
so nothing is copied from either. You bring your own disc image.
