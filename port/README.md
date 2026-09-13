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

**Status: M1 done — it links and it talks.** Two binaries build from one
Makefile: `port/build-ppc.sh -j8` produces a `powerpc-apple-darwin8` executable
for the G4, and `make -C port TARGET=host -j8` produces an arm64 one for the
development Mac. Running the host build against your own disc image boots the
game's own `main()`, narrates `HuSysInit` through `omMasterInit` on stdout, and
reaches `objdll>Link DLL:dll/bootdll.rel` -- the point where it asks for the
first relocatable module, which is M2's job. The full narration is in
[`docs/m1-boot.log`](docs/m1-boot.log) and what M1 found is §9 of
[`docs/PLAN.md`](docs/PLAN.md); the design, the measured inventory, the
milestones and the risks are the rest of that file, with the numbers behind
them in [`docs/inventory.md`](docs/inventory.md).

```sh
port/build-ppc.sh -j8                       # the G4 binary
make -C port TARGET=host -j8                # the development binary
port/build-host/marioparty4 \
    --image "orig/GMPE01_01/Mario Party 4 (USA) (Rev 1).nkit.iso" \
    --watchdog 5 --log boot.log
```

`--image` takes the disc image directly (the FST is parsed at boot; an
NKit-trimmed ISO keeps every file at its original offset) or a directory
holding an extracted `files/` tree. Other flags: `--frames N`, `--watchdog SEC`,
`--turbo`, `--gxlog`, `--stub-trace`, `--deterministic`, `--verbose`.

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

**What it does there today:** the whole boot narration, in under a second, to
the same `objdll>Link DLL:dll/bootdll.rel` seam the host reaches — and with the
audio the host cannot do. On big-endian hardware `msmSysInit` succeeds where the
host fails, MusyX initialises, and the game's own read of the REL headers off
the disc is correct in every field. The captured run, the host/G4 diff and the
two port bugs the G4 found are in [`docs/g4-boot.log`](docs/g4-boot.log); §10 of
[`docs/PLAN.md`](docs/PLAN.md) is the log entry.

## Layout

| path | what |
|---|---|
| `docs/PLAN.md` | the plan and the engineering log |
| `docs/inventory.md` | generated: every SDK symbol the game calls, with counts |
| `docs/m1-boot.log` | the boot narration M1 reaches on the host, captured |
| `docs/g4-boot.log` | the same, on the real G4, with the host diff |
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
| `src/platform/` | `main`, the host loop, window and GL context, settings, argv |
| `src/os/` | OS shims, the `HUPROCESS` context switch, `PSMTX*` in C, cache no-ops |
| `src/gx/` | the GX state machine, vertex decode, texture decode, the GL 1.3 backend |
| `src/dvd/` | DVD over an extracted `files/` tree or a disc image |
| `src/card/` | CARD over host files |
| `src/pad/` | PAD over SDL2 and the IOKit Xbox One driver; `.dtm` and script playback |
| `src/audio/` | AI, the MusyX SAL replacement, the DSP command interpreter |
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
