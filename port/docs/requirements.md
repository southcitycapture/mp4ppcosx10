# What the game needs

*(M25, 2026-09-20.  The plain-words version of the machine check in
`src/platform/machine.c`; the launcher and the README draw on this.  Run
`isle --machinecheck` on any Mac to get the verdict for that machine; the
tables below say what it will say and why.)*

## In one sentence

**Minimum:** a PowerPC G4 Mac running Mac OS X 10.4 or later, 256 MB of
memory, and a graphics card of the Radeon 9000 class or better (four or
more texture units with ATI's combiner extensions) — on which the game runs,
slower than a console on anything under 800 MHz.  **Recommended:** a dual
1 GHz G4 (or better) with a 64 MB Radeon 9000 or better and Mac OS X 10.5 —
the machine the port was built on, where it runs at console speed with
30 frames a second on the board (the two promises below: *full game
speed* is the minimum's, *30 fps everywhere* this machine's class's).

## The two promises *(M40, 2026-09-23)*

The machine check prints a `tier:` line under its verdict (`isle
--machinecheck`), one of these:

| tier | what it promises | the machines | the evidence |
|---|---|---|---|
| **full game speed** (the minimum) | the game runs at the console's speed -- the clock, the music, the CPU players -- on every screen; the picture rate varies by screen | any supported G4 at 800 MHz or more, one CPU or two, a Radeon 9000-class card | the soaks (PLAN.md 54.4: 99.9% over six and a half hours on the reference) |
| **30 fps everywhere** | every screen a player can reach holds a median of 29.5+ presented frames a second at full game speed -- boot, title, file and mode select, the character select, every board, every minigame, results, story, options, records, credits | **this machine's class and up: a dual 1 GHz G4 with a 64 MB Radeon 9000 or better** (the reference, PowerMac3,5). Faster machines only add headroom | `docs/fps-scoreboard.md`: the scoreboard, one real-time run per screen on the reference (`tools/fps_board.sh` + `tools/fps_board.py`, re-runnable) |

What other machines are expected to do -- **untested**, argued from the
reference's numbers, never run:

* **A single 1.25-1.42 GHz G4** (Mac mini G4, eMac 2005, iBook/PowerBook
  G4 1.33-1.67): **full game speed**; not 30 fps everywhere. On one CPU the
  render thread's replay and the vertex decode run on the game's own core
  (`--threads 0 --renderthread 1`, applied by the check), so a screen's
  drawn frame costs the sum of what the reference spreads over two cores:
  the reference's heaviest minigames spend 20-30 ms replaying and 10 ms
  decoding per drawn frame besides 15 ms of game, which a 1.42 GHz core
  (~1.4x the reference's per clock-for-clock work) brings to ~35-40 ms --
  20 fps there, 30 on the board and the menus. The check says "full game
  speed ... untested".
* **A G5** (any): expected to meet both promises -- a 1.6-2.7 GHz 970 does
  the game thread's work in well under half the reference's time, and the
  cards shipped with it (Radeon 9600/9800, GeForce FX 5200 and up) list
  every extension the port needs except where the table below says
  otherwise. A dual G5 is "30 fps everywhere, expected"; a single G5 is
  also expected to reach 30 in most scenes, but the check promises only
  full game speed on one CPU. Untested.
* **A dual 1.25-1.42 GHz G4** (MDD): the reference's promise with ~25-40%
  headroom. Untested.

The screens that miss the bar on the reference are listed, worst first,
with their wall and the next lever, in `docs/fps-scoreboard.md` and
PLAN.md 55; until the scoreboard shows every screen passing, the second
promise is the scoreboard's count, not a blanket claim.

## The verdicts

The check runs at boot, before the window opens, and prints one of three
words with a line per reason.  The window title carries the verdict until
the first frame is drawn.

| verdict | means | the port does |
|---|---|---|
| `ok` | every probe within the G4's envelope | runs |
| `degraded` | a fallback exists for what is missing; the reasons say which and what it costs | runs, applies the fallback's setting and prints it (`machine: applying --texbudget 24 (VRAM 32 MB)`) |
| `unsupported` | the game cannot be drawn correctly, or this is not a PowerPC Mac | refuses, with the reasons; `--force` runs it anyway |

## The machine

| | minimum (`degraded`) | full speed (`ok`) | why |
|---|---|---|---|
| CPU | any PowerPC G4 (7400 / 7450) or G3; **not Intel** — the PowerPC binary under Rosetta is refused | two G4 cores at 800 MHz or more | the board needs ~7 ms of game logic per 16.7 ms frame at 1 GHz; the mixer's second half runs on a second core (PLAN.md 39) |
| clock | anything; under 800 MHz the check says "will run below full speed" | 800 MHz+ | 1 GHz = 100% speed with headroom (PLAN.md 39.4) |
| memory | 256 MB | 512 MB+ | the process peaks at ~170 MB resident over a soak |
| OS | Mac OS X 10.4 (Tiger) | 10.5 (Leopard) | the binary is built for 10.4; 10.5.4 is the only OS it has run on natively |
| graphics | see below | a 64 MB Radeon 9000 | the texture cache's 40 MB budget was set on that card |
| disc | the player's own Mario Party 4 (USA, Rev 1) disc image | | no game data ships with the port |

## The graphics card

The renderer is fixed-function OpenGL 1.3 shaped around the Radeon 9000
(PLAN.md 3): every TEV stage is a texture unit, and the shapes the game
uses need these — **all required**:

| extension | used for |
|---|---|
| `GL_ARB_multitexture`, `GL_ARB_texture_env_combine` | one combiner unit per TEV stage |
| `GL_ARB_texture_env_crossbar` | a unit reading another unit's texture: the register rewrites (the board eyes and walkway, the results portraits) |
| `GL_ATI_texture_env_combine3` | `A*C + B` in one unit: the four-input stages and the textured highlight |
| `GL_EXT_secondary_color` | the specular channel folded in after the units (every shiny material) |
| **4 or more texture units** | the game's chains run to five stages; a card with fewer draws the picture wrong |

**Degraded without** (a fallback exists): `GL_ARB_vertex_program` (transform
and lighting move to the CPU: the pre-M11 speed), `GL_APPLE_vertex_array_range`
+ `GL_APPLE_fence` (vertex arrays copied per draw), `GL_EXT_multi_draw_arrays`,
`GL_EXT_blend_subtract`; **less than 64 MB of VRAM** (the texture cache's
budget is lowered to three quarters of the card: 32 MB → 24).

What that means by card, as far as the extension tables say (the port has
only ever run on the first line; everything else is argued from the
driver's lists and `--fake-machine`, and a real `--machinecheck` on the
machine is the answer):

| card | verdict | why |
|---|---|---|
| ATI Radeon 9000 / 9000 Pro, 64 MB (Power Mac G4 2002-03) | **`ok`** — witnessed | the reference card |
| ATI Radeon 9000 / 9200 / Mobility 9200, 32 MB (eMac 2004, iBook G4, PowerBook 12"/15" 2003) | `degraded` | the same core; VRAM → `--texbudget 24` |
| ATI Radeon 9600 / 9700 / 9800, Mobility 9600/9700 (PowerBook G4 2004+, Power Mac G4/G5) | expected `ok` | R300 lists every extension R200 does, with more units and VRAM — unverified |
| NVIDIA GeForce FX 5200 (iMac G4 20", PowerBook 12" 2004, Power Mac G5), GeForce 6600 | **unknown** | 4+ units and `ARB_texture_env_crossbar`; whether Apple's NVIDIA driver lists `GL_ATI_texture_env_combine3` decides it — run `--machinecheck` |
| NVIDIA GeForce4 MX / GeForce2 MX (iMac G4 2002-03, eMac 2002-03, PowerBook Titanium) | **`unsupported`** | two texture units, and the NVIDIA lists carry `NV_texture_env_combine4`, not `ATI_texture_env_combine3` |
| ATI Radeon 7500 / 7000 / Rage 128 (Power Mac G4 1999-2001, iBook G3/G4, PowerBook Titanium) | **`unsupported`** | three texture units or fewer |

## The settings the check applies

Printed on the log as `machine: applying ...` and skipped when the flag was
given on the command line:

| finding | setting |
|---|---|
| one CPU | `--threads 0` (the mixer stays on the game thread; ~1.2 ms a frame) |
| VRAM under 64 MB | `--texbudget N`, N = three quarters of the card rounded down to 4 MB |
| no `GL_ARB_vertex_program` | `--cpuxf` |

## What it refuses

`unsupported` refuses to start and says why (on the log, on stderr, and in
a dialog when launched from the Finder).  `--force` overrides it; nothing
promises the picture is right then.  The Rosetta case is deliberate: the
PowerPC binary does run on an Intel Mac under Rosetta (it is the project's
tools bench), at about half the G4's speed on the game's code and with
frames that differ from the Radeon's, and it is not a machine the port is
shipped for.
