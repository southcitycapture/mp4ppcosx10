# Mario Party 4, PowerPC Edition

A native port of **Mario Party 4** to the **Power Mac G4**, running on
Mac OS X 10.4 and 10.5 for PowerPC. It is built on the
[mariopartyrd/marioparty4](https://github.com/mariopartyrd/marioparty4)
decompilation: this fork is the port, and the decompilation is theirs.

It is not an emulator. The game's own C code, recovered by the decompilation
project, is compiled natively for the G4's PowerPC processor. Not one line of
the decompilation's `src/`, `include/` or `lib/` is edited. Everything the game
asked of the GameCube is answered by the port in [`port/`](https://github.com/southcitycapture/mp4ppcosx10/tree/ppc-port/port):

* the GameCube's GX graphics pipeline, translated into OpenGL 1.3 for the
  Radeon 9000, with transform and lighting on vertex programs and a render
  thread on the second processor;
* the 99 relocatable game modules as native bundles;
* the disc as a file tree, with a loader that keeps the files a game re-reads
  in memory;
* the memory card as a file;
* MusyX sound mixed on the CPU, and the game's THP movies decoded by a small
  JPEG decoder of the port's own;
* controllers 1 to 4 through SDL2 and an Xbox One driver, with the keyboard as
  a fallback.

Where the game's behaviour has to change for the port, it changes through a
short list of exact-text substitutions in
[`port/patches.txt`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/patches.txt),
applied at build time.

**No game data is in this repository.** You need your own copy of the game.

## Where it stands

Release candidate **0.9.8**, measured on a dual 1 GHz Power Mac G4
(Quicksilver) with a Radeon 9000 under Mac OS X 10.5:

| | |
|---|---|
| Game speed | 100% of the console everywhere |
| Board | 29.8 frames per second, at the 30 cap |
| Minigames | 26 to 30 fps typical; the heaviest about 20 |
| Character select, title | about 23 and 25 fps |
| Picture | 59 of 63 minigames match the console frame for frame; all six boards play |
| Stability | a 6.5-hour unattended soak, five full boards and 131 minigames, no faults |
| Movies | all twelve play with sound |

The target for 1.0 is **30 fps on every screen** on that machine, which then
becomes the recommended system for 30 fps. The minimum to play at full game
speed is a G4 with Mac OS X 10.4 or later, 256 MB of memory and a Radeon
9000-class card. The app checks the machine at launch and says which it is.

Known and shipping as-is: the water ripple in three minigames, which Leopard's
Radeon driver can't draw; brief pauses on slow hard drives.

## Documentation

* [`port/docs/PLAN.md`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/docs/PLAN.md):
  the full engineering log, milestone by milestone, with every measurement and
  every wrong turn. Section 0 is the working method.
* [`port/docs/release-checklist.md`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/docs/release-checklist.md):
  what 1.0 needs and the evidence for each line.
* [`port/docs/requirements.md`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/docs/requirements.md):
  the machines it runs on.
* [`port/docs/decomp-struct-notes.md`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/docs/decomp-struct-notes.md):
  findings about the decompilation itself that belong upstream.

## Building

The G4 binary is cross-compiled from a Mac or Linux host with a Docker
toolchain (`powerpc-apple-darwin8`, GCC 14, the MacOSX10.4u SDK, a
Tiger-compatible SDL2), on the `ppc-port` branch:

```sh
git checkout ppc-port
port/build-ppc.sh -j8            # the executable and 99 bundles
sh port/tools/make_bundle.sh     # MarioParty4.app
sh port/tools/make_dmg.sh        # the disk image, with no game data in it
```

See [`port/README.md`](https://github.com/southcitycapture/mp4ppcosx10/blob/ppc-port/port/README.md)
for the flags and the layout.

## Branches

* **`ppc-port`** is the port. Everything the port adds lives under `port/`.
* **`main`** tracks the upstream decompilation, plus this README. The
  decompilation's own README, with its build instructions, is
  [`DECOMP.md`](DECOMP.md) on `main`.

## Credits

The decompilation is the work of the
[mariopartyrd](https://github.com/mariopartyrd) team. The port is built by
[southcitycapture](https://github.com/southcitycapture) as part of a series of
native ports for the Power Mac G4, alongside Snowboard Kids 1 and 2 and LEGO
Island.
