# Mario Party 4 PowerPC Edition — the v1.0 checklist

Written at M39 (2026-09-22) on the release candidate, **0.9.8**. This is
the evidence for a decision, not the decision: nothing is named 1.0. Each
line says what was measured, where, and on which build; PLAN.md's section
numbers are the long form. The machine is the reference G4 throughout — a
dual 1 GHz Power Mac G4 (PowerMac3,5), 1.5 GB, 64 MB Radeon 9000, Mac OS X
10.5 — unless a line says otherwise.

## The v1.0 line, amended at M40 (2026-09-23): "30 fps overall"

The user's one requirement for 1.0, in their words: **"30fps everything"** --
every screen a player can reach, menus included: boot and title, file
select, mode select, character select, every board, every minigame (party,
story, Bowser, extra room, battle), results and ceremonies, story mode, the
options and records screens, the credits. **Measured as a median of at least
29.5 presented frames a second at 100% game speed** on the reference (the
dual 1 GHz G4, Radeon 9000), screen by screen, by the scoreboard:
`tools/fps_board.sh` (one real-time `--perf` run per screen on the G4) and
`tools/fps_board.py` (the table), re-runnable by any milestone. The
reference is the machine class the second promise is made for
(`requirements.md`, the two promises: *full game speed* is the minimum's,
*30 fps everywhere* the reference's and up).

**Where 0.9.8/M39c stood when the line moved** -- the scoreboard's first full
run, `docs/fps-scoreboard-m40-before.md` (79 runs, 82 screens, the M39c build
`df75d3a6`): **55 of 82 screens pass.** Every one of them runs at 100% game
speed; the 27 that miss, miss on the picture rate alone. The gap per screen
(median fps; the gap to 29.5):

| screen | fps | gap | | screen | fps | gap |
|---|---:|---:|---|---|---:|---:|
| m404 Trace Race | 15.9 | 13.6 | | m444 | 24.9 | 4.6 |
| m415 Stamp Out! | 16.1 | 13.4 | | w05 Koopa's Seaside Soirée | 25.0 | 4.5 |
| m441 | 21.0 | 8.5 | | w04 Boo's Haunted Bash | 25.0 | 4.5 |
| m431 | 21.1 | 8.4 | | m418 | 26.0 | 3.5 |
| m432 Dungeon Duos | 22.0 | 7.5 | | m410 | 26.7 | 2.8 |
| m414 | 23.2 | 6.3 | | m412 | 26.8 | 2.7 |
| mentdll, the character select | 23.3 | 6.2 | | m423 | 26.8 | 2.7 |
| m433 | 23.8 | 5.7 | | m407 | 27.0 | 2.5 |
| m436 | 24.0 | 5.5 | | m424 | 27.3 | 2.2 |
| m447 | 24.0 | 5.5 | | m438 | 27.9 | 1.6 |
| m401 | 24.1 | 5.4 | | m463 | 28.0 | 1.5 |
| m409 | 24.4 | 5.1 | | m440 | 28.8 | 0.7 |
| m435 | 24.9 | 4.6 | | w01 Toad's Midway Madness | 29.1 | 0.4 |
| | | | | m430 | 29.2 | 0.2 |

Passing at the line: the boot and title (30.0), the mode select and the
file select inside it, the instruction cards, the results, the board's
ending (mstory3), the story ending (mstory2), the options, the Present Room,
the Extra Room, the Minigame Mode menu, the credits, four of the six boards
(w02, w03, w06; w01 at 29.1 on the teleported run, 30.0 over the long
soaks), and 36 of the 63 minigames. Not reached by the harness: story mode's
own board setup (`--goto mstorydll` panics in `dvd.c` without the file
select's loads -- a teleport's limit, the story's boards are the party
boards).

**Where M40 left it** (0.9.9, `docs/fps-scoreboard.md`, the same chain on
the final build): **57 of 82 screens pass** -- m407 and m440 crossed the
bar; the character select 23.3 → 25.9, m431 21.1 → 23.9, m432 22.0 → 24.6,
m444 24.9 → 27.8 (the static-geometry cache, PLAN.md 55.3-55.5); nothing
fell below it. **The line is not met.** The 25 screens still short, and the
wall of each (PLAN.md 55.9): **23 are the game thread's drawn frame** (the
engine's draw preparation, ~11 ms, and the port's GX front end, ~12 ms,
per drawn frame -- M41's lever); m404 Trace Race is its render thread's
replay alone (37 ms); m415 Stamp Out! its canvas's copy reads. A 1.0 under
this line waits for them, or for the user to move the line.

## Done

| area | the claim | the evidence |
|---|---|---|
| speed | the game runs at the console's speed, the picture at up to 30 frames a second | §54.4: the 6 h 30 min soak on 0.9.8, five full boards, **99.9%** of the console's speed (game 23,376 s against wall 23,404 s); the board at **28.2–28.9** frames a second a turn, 41 minigame modules at 19.6–29.9 (22 of them at 27+); §53.6 the movies |
| picture | every minigame compared with the console (Dolphin) at seven positions | §54.5: the whole gallery on 0.9.8 — **63 of 63 run with no fault; 59 match / 2 minor / 2 oracle-failed / 0 port-faulted** (the minors are the ripple, below; the oracle-failed are Dolphin hanging); 432 of 441 positions byte-identical to M35's final gallery, the rest m448's felt fixed and two sparkles; `docs/gallery/compare.html` |
| determinism | the three reference frames are byte-identical run to run, on and off the movies | §54.5: on the release candidate `fcf94d24…`, **800 `d2d40344` / 3000 `59008ce4` / 7000 `3f98f882`** with the movies and **`0b58c5ee` / `2b99c60a` / `4a9a640c`** with `--nomovies` — the refs since §53.9, identical in turbo, at real time and on one CPU (§54.2, eight walks) |
| movies | all twelve THP files play with their sound, the console's picture to a level | §53: opening 29.4 fps presented on two CPUs, the track sample-exact against an independent decode (52 dB residual), 0 decode errors, the console match +0.4/+0.8/+0.6 levels; on one CPU (`--threads 0`) **0 underruns during every movie** in eight runs, both arms of the M39 A/B, and the same pictures to the byte; the opening 28.7 fps, the mode select 23–26 (§54.2) |
| stability | long unattended runs with no fault | §54.4: **6 h 30 min, 1.4 M retraces, five 20-turn boards, 131 minigame plays of 41 modules: 0 faults, 0 guard hits, 0 mix mismatches**, rss − the resident set flat at 131–145 MB; §46 (7 h, M31), §53.1 and §54.1 before it |
| loading | the disc image's reads come from memory on a 1.5 GB machine | §51: a cold six-minigame walk, slow reads 30.7 → 3.3, resyncs 4.3 → 1.0 (3 × 3 runs); the M39 soak's `DVD:` line — `8691 reads, 2.22 GB, 4 over 100 ms`, 7,136 reads (2.0 GB) from memory, 0 disk reads after a prefetch of the same file |
| controllers | an Xbox One pad over USB (rumble too) and the keyboard beside it; two players witnessed | §47.2 step 5, §49.8 (pad as player 1, the keyboard as player 2, `--kbport 2`) |
| the machine check | refuses what cannot draw the game, warns once below the reference, applies settings by RAM and VRAM | §40 (the G4 `ok`, the MacBook under Rosetta `unsupported`, three `--fake-machine` profiles); §47.2b the two dialogs on a Finder launch |
| packaging | a dmg with the app, the Read Me and the licences, no game data; a first run from an empty home works | §47.2 (M32), §54.3 — M39 again on 0.9.8 from an empty home: the chooser, the opening with its sound (28.98 fps, 0 underruns), the title, a new file, the mode select's movies, a party board, two minigames, the results, Cmd-Q with the card written; twelve screenshots `docs/screenshots/m39-first-run-*` |
| the save | a 59-block card image in Dolphin's layout, flushed on a thread, written on quit | §47.2 step 7; the soak's `CARD:` lines — 110 writes, 222 flushes, all written, 5.6 s on the game thread over 6.5 h; §54.3 the first run's card created, written and flushed on Cmd-Q |

## Known, and shipping as it is

| item | what a player sees | why it ships |
|---|---|---|
| the pools' ripple (m405 Mario Medley, m417 Makin' Waves, m434 Cheep Cheep Sweep) | a flat tint where the console ripples the water; m405's water grey-green | the ripple is GX's indirect warp, which the Radeon 9000's fixed-function pipeline cannot do. A fragment-shader path (`--tfs`, `GL_ATI_text_fragment_shader`) is built and off: on the Leopard ATI driver its `SampleMap` returns a constant on one-texture draws and white on m434's copies while the coordinates and colours reach the program right (§50.14, §52.8 — the driver finding). Nothing on the port's side is left to try without a different driver |
| the character select at ~26 fps (0.9.9; ~23 in 0.9.8) | one of 25 screens below the 30 cap (the v1.0 line above) | M40 (PLAN.md 55.9): its game thread's cycle is 34.8 ms (a drawn frame 31.1 + a consumed 3.7) against 33.3, the render thread's 31.2; the eight breathing characters are two thirds of its 75,600 vertices a frame and cannot be cached (skinned every frame). M33's split, M37's batch enders and M40's cache took it 20 → 23 → 26. The game's speed there is the console's; only the picture rate is lower |
| controllers 3 and 4 | untested | wired the same way as 2 (§49.8: every Xbox One pad claimed, every SDL joystick opened, ports in order); there is one pad in the house |
| one memory card | slot A only; slot B is always empty | the game needs one card; a second would be a second image file and nobody has asked |
| one-CPU Macs and the movies | on one processor, Stamp Out! (m415) falls to about 13 frames a second and 94% of the console's speed, with one-second catch-ups and the sound breaking up (285 underruns in its minigame) | Stamp Out! reads its own picture back to paint with (§20's canvas copy reads), and on one CPU the game thread also replays the render thread's work. The same minigame on two CPUs: 20 fps, 100.6%, 0 underruns. It is **not** the movies: §54.2's A/B found 0 underruns in every movie on one CPU; the extra underruns M38 blamed on them were this minigame, which the movies' schedule deals where `--nomovies` deals m412. The reference machine has two CPUs; the Read Me says what one gets |
| one-to-two-second pauses, ~2.6 an hour | the game stops and catches up, the music with it | §54.4's seventeen in 6 h 30 min: 3 cold reads of files outside the resident set (Bowser's space, the board's ending), 2 card flushes waiting for the previous one, 10 on the lab's own `--status` log write, 2 unattributed; under all of them the G4's drive answering some requests in 1–2 s (26 card renames at 1.68–1.81 s); replayed from snapshots at the same frames, the Bowser read took 4 ms and the status-line frames 0 resyncs (§54.9). Two port-side fixes are small (the card writer coalescing, the log written on a thread) and one is a list edit (the resident set learning those files); each wants a soak of its own, so none went into the build under test |
| the picture about a level bright | not visible side by side | GX truncates at each TEV stage, GL's fixed function rounds (§53.8, §53.11); measured on the movie and m432's walls |
| m403's lamp, m432's walls, m450's +400 | small differences read and not fixed | §50.8's rows: the lamp's halo duller; the walls a level brighter (no channel bug, §53.11); m450 is the play |

## Open lines for the decision

Each line is something the evidence does not settle, for the user to weigh;
none is a known fault.

1. **The pauses (above).** Ship 0.9.8 with them, or take the three small
   fixes (card writer coalescing, the log on a thread, Bowser's space and
   the board's ending in the resident list) into a 0.9.9 and soak it again
   (six hours; the snapshots `snap-m39-bkoopa` and `snap-m39-statusline`,
   §54.9, are where to start).
2. **A real single-processor Mac** has never run the game. Every one-CPU
   number is the dual G4 under `--threads 0 --renderthread 1`, where the
   OS, the window server and the GL driver still have the second
   processor. The Read Me promises a G4 from 800 MHz.
3. **Mac OS X 10.4 (Tiger)** has never run the game. The binary is built
   for 10.4 (the 10.4u SDK, `LSMinimumSystemVersion 10.4.0`) and the G4 has
   a Tiger partition, but every run of every milestone was Leopard 10.5.4;
   running it would mean booting the G4 into Tiger, which the lab's rules
   leave to the user.
4. **Controllers 3 and 4** untested (one pad in the house); **one card
   slot** by design.
5. **Other cards** than the Radeon 9000 64 MB: the Read Me's table is the
   drivers' extension lists, not runs (§40); the 32 MB profiles only through
   `--fake-machine`.
6. **The Terminal lines in the Read Me** (`…/Contents/MacOS/isle --windowed`,
   `--kbport 2`, `--defaults`): the same launch the lab's runner makes in
   the console session on every run, but nobody has typed one into
   Terminal.app on the G4.
7. **The name.** Everything says 0.9.8; a 1.0 is a rename of the version
   string (`PORT_VERSION_STRING` in `include/port.h`, which the bundle's
   plist and the dmg's name are read from) and the Read Me's first lines,
   and a rebuild — nothing else.

