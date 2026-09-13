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

**Status: M2a done — the 99 modules load, and GX draws.** Two binaries and two
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
`--turbo`, `--gxlog`, `--stub-trace`, `--deterministic`, `--verbose`,
`--reldir DIR`, `--reltest`, `--relzerobss`, `--noaudio`, `--headless`,
`--glcheck`, `--gxwarn`, `--gxdemo`, `--dumpframe N`, `--shotdir DIR`,
`--scale N`.

## Layout

| path | what |
|---|---|
| `docs/PLAN.md` | the plan and the engineering log |
| `docs/inventory.md` | generated: every SDK symbol the game calls, with counts |
| `docs/m1-boot.log` | the boot narration M1 reaches, captured |
| `docs/m2a-boot.log` | the boot narration M2a reaches, captured |
| `docs/m2a-reltest.log` | all 99 REL bundles loaded and unloaded twice |
| `docs/m2a-gxdemo.png` | the GX self-test frame, the graphics layer's reference |
| `Makefile` | the whole build, `TARGET=host` or `TARGET=ppc-darwin` |
| `build-ppc.sh` | the Docker wrapper around the PowerPC cross build |
| `patches.txt` | every change the port makes to game sources, as exact text |
| `include/override/` | SDK headers the port replaces wholesale |
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
