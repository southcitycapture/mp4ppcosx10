# Mario Party 4, PowerPC edition

A native port of Mario Party 4 to the Power Mac G4 (Mac OS X 10.4 and 10.5,
PowerPC), built on top of the [mariopartyrd/marioparty4](https://github.com/mariopartyrd/marioparty4)
decompilation. This fork is the port; the decompilation is theirs.

The game's own code is compiled as is. Not one line of the decomp's `src/`,
`include/` or `lib/` is edited here. Everything the game asked of the GameCube
is answered in [`port/`](port/): a host loop, the 99 relocatable modules as
native bundles, the disc as a file tree, the memory card as two files, the
controller through SDL2, MusyX audio mixed on the CPU, and the GameCube's GX
graphics pipeline translated into OpenGL 1.3 for the Radeon 9000, with the
transform and lighting on ARB vertex programs. Where the game's behaviour has
to change for the port, it is changed through a short list of exact-text
substitutions in [`port/patches.txt`](port/patches.txt) applied at build time.

**No game data is in this repository.** You need your own disc image.

## Where it stands

Played on a dual 1 GHz Quicksilver with a Radeon 9000:

* Full twenty-turn boards with computer players, chaining into the next game
  unattended overnight, at 100% of the console's game speed.
* The board presents about 18 frames per second at real speed, capped at 30.
  The renderer is the open work; the game logic runs at full speed.
* 45 of the 60 minigames have been dealt and played through in the soaks so
  far. Rendering is compared against Dolphin frame captures and re-based only
  with a written diff.
* Deterministic runs, scripted input, fast-forward to any frame, and byte-exact
  snapshots that restore in a quarter of a second, so any bug can be reproduced
  in minutes rather than hours of play.

The full engineering log, milestone by milestone with every measurement and
every wrong turn, is [`port/docs/PLAN.md`](port/docs/PLAN.md). Section 0 is the
working method. The findings about the decompilation itself that belong
upstream are collected in [`port/docs/decomp-struct-notes.md`](port/docs/decomp-struct-notes.md).

## Building

The G4 binary is cross-compiled from a Mac or Linux host with the Docker
toolchain the Snowboard Kids ports use (`powerpc-apple-darwin8`, GCC 14, the
MacOSX10.4u SDK, a Tiger-compatible SDL2):

```sh
port/build-ppc.sh -j8                          # the executable and 99 bundles
sh port/tools/make_bundle.sh                   # MarioParty4.app
```

A host build (`make -C port TARGET=host`) exists as a plumbing harness for
the development machine; it cannot draw the game, for reasons the port README
explains. See [`port/README.md`](port/README.md) for the flags and the layout.

## Branches

* `ppc-port` is the port. Everything lives under `port/`.
* `main` tracks the upstream decompilation.

## Credits

The decompilation is the work of the [mariopartyrd](https://github.com/mariopartyrd)
team. The port is built in agent sessions run by [southcitycapture](https://github.com/southcitycapture)
as part of a series of native ports for the Power Mac G4, alongside
Snowboard Kids 1 and 2 and LEGO Island.
