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

**Where M41 left it** (0.9.10, `docs/fps-scoreboard.md`; M40's kept as
`docs/fps-scoreboard-m40-after.md`; PLAN.md 56): **59 of 82 screens pass.**
Crossed the bar: m447 Archaeologuess 23.9 → 30.0 (its six-lamp rooms lit on
the card), m412 28.9 → 29.9, m423 29.1 → 29.9, m430 29.0 → 29.9. The two
odd walls are gone as walls: m404 Trace Race 16.2 → 27.8 and m415 Stamp Out!
16.7 → 29.4 on the chain (27.9 and 29.9 in the three-run A/B; their painted
canvases are re-decoded only where they changed). Two screens M40 passed are
under the bar on this chain's single runs -- m429 27.1 and m407 29.3 -- and
are not claimed; a three-run A/B has both at the bar on 0.9.10 and on 0.9.9
alike (m429 30.0 / 30.0, m407 29.9 / 29.7: run-to-run variance, PLAN.md
56.6). **The line is not met.** The 23 still short, in three classes (PLAN.md
56.7): twelve carry more work on the two cores together than a 30 fps cycle
allows (m431 23.5, m441 23.2, m401, m436, m414, m409, m435, w04, the
character select 26.8, m433, m444, m418) -- the total must fall; five are
game-bound with room on the render thread (m432, m424, w05, m410, m404);
six are at the bar on their tails (m429, m463, m407, m415, w01, m438). All
82 at 100% game speed.

**Where M42 left it** (0.9.11, `docs/fps-scoreboard-m42-after.md`; M41's kept as
`docs/fps-scoreboard-m41-after.md`; PLAN.md 57): **64 of 82 screens pass**,
none of M41's passes lost. The scoreboard now runs a screen that lands
within 2 fps of the bar three times and judges it by the median of the
three runs' medians (17 screens this time). Crossed the bar: m432 Dungeon
Duos 25.7 → 29.9 (the vertex cache's write barrier: the maze's unchanging
arrays are no longer re-hashed every frame), m424 29.1 → 29.9, and m429,
m407 and m415 by three runs each (30.0 / 30.0 / 29.9). **The line is not
met.** The 18 still short (PLAN.md 57.10): eleven carry more work on the
two cores together than a 30 fps cycle allows (m441 24.1, m431 24.9, m436,
m401, m414, m435, the character select 26.7, m409, m433, m444, m418); four
are game-bound with 8-11 ms idle on the render thread (m404 28.0, m410
28.5, w05 29.2, w04 28.0) -- the next lever is the translation on the
render thread (PLAN.md 57.7), sized and not built; m463 and m438 are short
on a heavy phase; w01 is 29.2 by the pooled rule (its own runs are 30.0;
the ~110 board frames inside the minigame teleports' hand-overs pull it
under -- whether they count is the user's call). All 82 at 100% game
speed.

**Where M43 left it** (0.9.12, `docs/fps-scoreboard.md`; M42's kept as
`docs/fps-scoreboard-m42-after.md`; PLAN.md 58): **68 of 82 screens pass**,
none of M42's passes lost. Crossed the bar: m404 Trace Race 28.0 → 30.0,
m410 28.5 → 30.0, w05 Koopa's Seaside Soirée 29.2 → 30.0, m418 28.9 →
30.0 (the morph rewriters' precise join, the unread-TEX0 decode shapes,
the texture stripes' buffer pool, the write barrier on arrays' partial
pages -- all exact, both walks byte for byte). The translation on the
render thread (`--rtgx`) is built, exact and measured, and ships off: the
render thread pays more for it than the game thread saves. The compiler
(PGO, `-O3` on the hot files, LTO) measured level and ships as it was.
**The line is not met.** The 14 still short (PLAN.md 58.12): m441 24.0 and
m431 26.1 over both cores' budget; m401 26.2, m436 27.1, m433 27.6, m435
28.1, m409 28.2, m414 28.9, the character select 28.9, w04 29.0 and m444
29.1 game-bound (the engine's own walks: 12-14 M instructions a drawn
frame at under half an instruction a cycle, a fifth of the cycles waiting
on memory -- PLAN.md 58.4); m463 28.3 and m438 28.9 short on a phase of
vertex-animated geometry; w01 29.1 by the pooled rule (30.0 on its own
board frames). All 82 at 100% game speed.

**Where M44 left it** (0.9.13, `docs/fps-scoreboard.md`; M43's kept as
`docs/fps-scoreboard-m43-after.md`; PLAN.md 59): **73 of 82 screens pass**,
none of M43's passes lost. Crossed the bar: m433 Beach Volley Folly 27.6 →
29.8, m414 28.9 → 29.9, m438 28.9 → 29.9, the character select 28.9 →
30.0, w04 Boo's Haunted Bash 29.0 → 29.9 -- the GX front end rewritten for
the G4 (the TEV apply skipped when nothing it reads moved, the layout kept
by value, the vertex program's key and light rows kept by their bytes, the
render stream's records fewer and a cache line apart with `dcbz` ahead of
the writer, the word hashes, the transform and raster state skipped; all
exact, both walks byte for byte). The water ripples again (`--water
off|cheap|full|auto`, the warp at the vertices from the game's own bump
map, cheap by default on the reference; PLAN.md 59.7). The G4 locked up
once during the milestone's runs; `--fixbase` was measured off, cost four
passes and was put back after two hours of that exact workload, the whole
scoreboard and a two-hour soak ran clean on the same code (PLAN.md 59.9).
**The line is not met.** The 9 still short (PLAN.md 59.11): m441 26.3,
m431 27.9, m409 29.4 and m401 28.1 on the game thread's drawn frame; m436
27.8, m435 28.8 and m444 29.2 with 4.2-4.7 ms of decode on the game
thread; m463 28.9 on a phase of vertex-animated geometry; w01 29.4 by the
pooled rule (30.0 on its own board frames). All 82 at 100% game speed.

**Where M45 left it** (0.9.14, `docs/fps-scoreboard.md`; M44's kept as
`docs/fps-scoreboard-m44-after.md`; PLAN.md 60): **73 of 82 screens pass**,
none of M44's passes lost, none crossed; the nine short within their runs'
spread of M44's numbers (m441 26.9, m401 27.8, m436 27.9, m431 27.9, m435
28.8, m463 29.1, m444 29.1, w01 29.2 pooled / 30.0 board-only, m409 29.4).
The milestone was the user's findings on 0.9.13: **m427's headlamp pool
restored** (M44's water had put the river on the CPU path, whose depth
disagreed with the beams' -- the water's positions now go through a vertex
program), **the board's black blip fixed** (the port's `OSf32tou8` wrapped
where the Gekko's quantised store saturates: five black frames at the end of
every board dimming since 0.9.1), **m427's black left half** hunted to the
photograph's moment in lockstep and six times at real time from a snapshot,
not reproduced (PLAN.md 60.2 has the teleport); **picture checks** in the
chain (`PC:`, `tools/m45_piccheck.py`, `--halfwatch`, `--giveitem`: 584
frames and ~45,700 counted presented frames, plus the scoreboard's 178 frame
pairs, against the previous release); the water tuned to the user's notes
(Makin' Waves' waves x2.5 and no sky, Cheep Cheep Sweep's pond in two looks
for the user to pick, Mario Medley's colour found: a shadow map's background).

**Where M46 left it** (0.9.15, `docs/fps-scoreboard.md`; M45's kept as
`docs/fps-scoreboard-m45-after.md`; PLAN.md 61): **75 of 82 screens pass**,
none of M45's passes lost; m409 (29.8) and w01 (29.6 pooled, 30.0 board-only)
crossed.  Seven short: m441 25.9, m436 27.4, m401 27.7, m431 27.9, m463 28.8,
m435 28.8, m444 29.2 (the drawn frames' work as M45's; the walls in PLAN.md
61.9).  **The Bowser room's pillar lights** (the user's finding on 0.9.14):
drawn as the console draws them in lockstep, frame for frame (54 pairs); the
flashing was the console's 60 Hz shimmer shown at 30 fps and is now drawn
between the two game frames each picture stands for.  **The flash rule** of
the picture checks no longer counts the game's fades and still catches the
Mega blip.  **PowerPCube's hooks**: the config's movies / prefetch /
resident, `PowerPCube.plist` in the bundle, the controls file and
gamecontrollerdb.txt (interactive runs only).  **m427's black left half**
(M45's open line, the user's photograph of 0.9.13) found in the candidate's
soak and fixed: a copy read-back left the scissor test off in a window.

**Where M47 left it** (0.9.16, `docs/fps-scoreboard.md`; M46's kept as
`docs/fps-scoreboard-m46-after.md`; PLAN.md 62): **75 of 82 screens pass**,
none of M46's passes lost, none gained.  The seven short screens moved:
m441 25.9 -> 26.8, m436 27.4 -> 28.0, m431 27.9 -> 28.3, m463 28.8 -> 29.4,
m435 28.8 -> 29.0, m444 29.2 -> 29.0, m401 27.7 -> 27.2 (one run each for
m441 and m401).  **Shipped, exact, each with its old path, each through the
picture checks (638 of 638 frames identical to 0.9.15's set; the scoreboard's
174 pairs 157 identical, 17 differing by a hair, 0 FAIL) and both md5 walks**:
the skin registry's cheap signature, `dcbz` of the SDK skin loop's
destination lines, the render thread and the workers woken after the
unlock.  **Built and not shipped**: the skin at the decode (exact on m436,
0 of 72 M vertices differ; not on m427, where a reader other than the draw
sees the unwritten arrays -- `--skindecode`), its AltiVec loop (bit for bit,
slower), the vertex cache's precise skin notice (cost m433 its pass --
`--vcskinnotice`).  **Item 4, measured for the user** (PLAN.md 62.8): a
card-side skin buys at most ~1 fps on m435 and nothing that reaches the bar
on m436, m431 or m444; it would move a few edge pixels a frame.

**Where M48 left it** (no release: 0.9.16 stays; `docs/fps-scoreboard.md`, the final M48
build's; M47's kept as `docs/fps-scoreboard-m47-after.md`; PLAN.md 63): **74 of 82 screens
pass** -- m409 at 29.45 on the count (29.1 / 29.45 / 29.6; the same drawing path read 29.9
against 0.9.16's 29.9 in an interleaved A/B), nothing else crossed either way (m441 26.5,
m401 27.6, m436 28.0, m431 28.4, m444 29.0, m435 29.0, m463 29.2).  **Built, exact, measured
and not shipped**: the compiled draw (the translation's records replayed when the key and the
GL shadow match: half the translation's cycles gone, no screen faster, m409/m433/m444 0.7-0.8
slower built alone -- `make M48C=1`, `--compiled auto|on`); GL display lists for the static
geometry (slower on the ATI driver -- `--glists`); the skin decode, now exact (m427's
"reader" was the game reading an uninitialized ripple phase from freed memory: the owed arrays
are written as the game frees them), trading +0.4 m463 / +0.6 m441 against -0.7 m431 / -0.5
m444 (`--skindecode`).  **Found on the way**: M48's own hooks cost m409 and m433 their passes
with every lever off (0.3-0.6 ms of the game thread) -- compiled out of the player's build
(`PORT_M48X`, `PORT_M48C`).  The final build: both md5 walks the references, the picture checks
636 of 638 frames identical to 0.9.16's set (the 2 m427's known second river state), 0 blips, the scoreboard's frames 170 of 182 identical to M47's (the 12 the Bowser pillars'
phase and the screen-copy games, 0 FAIL).

**Where M49 left it** (**0.9.17**, `docs/fps-scoreboard.md`; M48's kept as
`docs/fps-scoreboard-m48-after.md`; PLAN.md 64): **81 of 82 screens pass** with **Lite mode**
at its auto default -- on the reference machine the seven short minigames draw the game's own
lighter character file (and Butterfly Blitz's butterflies cast no shadow): m441 26.5 -> 30.0,
m401 27.6 -> 29.9, m436 28.0 -> 30.0, m435 29.0 -> 30.0, m431 28.4 -> 30.0, m444 29.0 -> 29.8,
m463 29.2 -> 30.0 (and through each whole minigame, 29.9-30.0).  Lite changes what is drawn and
nothing the game computes: the determinism hash (both RNG seeds, every model's position, the
players) is identical with Lite on and off on every frame to each minigame's end, the same exit
frame, coins and results screen.  Every other screen is console-exact; faster Macs get no Lite;
`lite = off` (`--nolite`) is the console's drawing everywhere (the seven then 26.9-29.7).  The
one short: **m433 Beach Volley Folly 29.4** (not a Lite screen; 30.0 vs 29.9 against 0.9.16 in
an interleaved A/B -- its spread); a lighter file for it was refused (it moves a joint the game
reads).  Exact and for everyone: m431's sparkle list trimmed to its live quads (28.1 -> 29.1).
The final build: both md5 walks the references, the picture checks 638 of 638 identical to
0.9.16's with Lite off and with Lite on, 0 blips; fifteen before/after pictures of the options
(`docs/screenshots/m49-lite-*.jpg`) for the user's approval.

**Where M49b left it** (**0.9.18**, PLAN.md 64b -- a safety follow-up from an independent review):
**`m441.char` is out of the auto set.**  `--jointaudit` (the eight characters' hook joints in
the m1/m2/m3 files over m441's motions) found the net hook m441 computes its catches from
(`a-itemhook-r` x (0,0,170), main.c:1016) differs between m1 and m2 for seven of the eight
characters (Donkey Kong by up to 25 units, Yoshi 0.22, the rest in the last float bit; Peach
alone identical), and the determinism hash with the second cast (Wario / DK / Daisy / Waluigi)
split on m441 with the lighter file (the RNG moved from entry +563, the minigame ended 102
frames later).  The other six Lite games, second cast, Lite auto against off: identical to each
minigame's end; m441 with its new auto set (no butterfly shadows) identical on both casts.
**m441 with Lite at auto now reads 29.1** (29.1 / 29.0 / 29.2) -- short; with the nets' shadows
and every other fence flower as well 30.0 / 30.0 / 30.0 (proved to the end on both casts after
the soak; not shipped -- the user's call).  The final build: both md5 walks the references, the
picture checks 636 of 638 identical to 0.9.16's with Lite off (the 2 m427's known second river
state), 0 blips; a 1 h soak with Lite at auto, 0 faults, no lock-up.
Class 0 (below the reference) now gets the reference's set, not every option; `--litechar 8`
is refused on the m1 games (m3 never proved).  m436/m435's charring pass keeps the eyes of the
file Lite loaded (`docs/screenshots/m49b-eyes-m436.jpg`).

**Where M50 left it** (**0.9.19**, PLAN.md 65 -- the user's Lite picks and Benchmark Mode):
**Lite's default set** on the reference = M49b's + `m441.nshadow` + `m441.rings` (the nets'
shadows and every other fence flower; Butterfly Blitz 30.0); **the extras** (`m401.fish`,
`m401.bubbles`, `m436.plates`, `m436.pillars`, `m435.pillars`) auto-on only below the reference
class; `m441.char` out.  Every change proved with `--gamehash` to the minigame's end on both
casts.  **The round shadow** (`m441.blob`, the user's N64-style disc under each net) is built
and pictured (`docs/screenshots/m50-blob-m441.jpg`); it costs nothing measurable (drawn frame
24.1 against 24.2 ms) but read 30.0 / 29.9 / 29.9 -- not 30.0 three times, so by the user's
rule it is an option (`liteopts = ref,m441.blob`), not in the default set.  **m433** is not
short: 29.9 in five runs on 0.9.18 and on 0.9.16.  **Benchmark Mode** (F1 or M in the game;
offered once on the first launch): five scenes as child runs (the movie, a board, Butterfly
Blitz Lite off and on, Makin' Waves), about 6 minutes on the G4, the settings written to the
config (lite, liteopts, water, movies, resident) and a report on the Desktop; on the G4 it picks
today's defaults, on the MacBook under Rosetta "slower" (Lite + extras, water off).  The MacBook
could not play a board or a minigame since M42 (Rosetta's faults carry no address: `--nowb`
there now).  **The scoreboard: 81 of 82** (m441 30.0, m433 29.8; the one short m401 29.1 on the
count -- 30.0 against 0.9.18's 29.9 in the five-run A/B right after, the same work: the edge).
The final build: both md5 walks the references, the picture checks 636 of 638 identical to
0.9.16's with Lite off (the 2 m427's known second river state), 0 blips; a 2 h 05 min soak,
0 faults, 0 resyncs, no lock-up; `littlejelly:~/MarioParty4-PowerPC-0.9.19.dmg` (md5 ef548665).


**Where M51 left it** (**0.9.20**, PLAN.md 66 -- Developer Mode and the playtest rig):
**Developer Mode** in the overlay menu (F1 or M, under Benchmark Mode): the **minigame
marathon** (all 63, a range, a kind or your own list; 1-4 people, the rest COM, each player's
character; each minigame a child run that hurries to the instruction card and waits for START;
results saved as they come, stop/resume, a summary page and text file -- tested end to end on
the G4 with a scripted stand-in), **Record this session** (every launch recorded from the boot
to `Documents/MarioParty4 Recordings`: the four controllers each frame, the start state, a copy
of the card; no measurable cost: m441 and a board 30.0 x3 with and without), the **soak
planner** (`soak-plan.txt`, `--soakplan`).  A recording replays on the G4 in step to the end
(lockstep, drawn or not: real-time play changes nothing the game sees).  **The G4 video**:
`tools/session_video.sh` -- the replay in lockstep with every frame dumped and the mixer's sound,
MP4 and the side-by-side with Dolphin (`~/mp4-videos/t4-g4.mp4`, `t4-side.mp4`).  **The Dolphin
replay** (`rec2dtm.py` + `dolphin_sync.py`): exact from the boot through the human's first turn
and the first COM's (10,302 frames); a COM's turn then starts 49 frames later on the console
(its own waits) and the boards part; re-joined at the instruction card by a one-frame Gecko
hand-over, Tree Stomp plays with every random draw in step for 34 s while the characters'
positions drift (float rounding: the port's GCC and sin/cos against the console's MWCC/MSL) and
the results differ.  `m441.bloball` (every Butterfly Blitz shadow a round blob): identical game
on both casts, 30.0 x3, the drawn frame 20.2 against 24.1 ms -- off until the user approves the
picture (`docs/screenshots/m51-blob-all-m441.jpg`).  **The scoreboard: 80 of 82** (m401 30.0;
m409 and m433 29.1 at the edge (the A/Bs: m433 29.9 on 0.9.19 and 0.9.20 alike; m409 28.9 on 0.9.19, 29.4 on 0.9.20 -- the screen's edge)).  Both md5 walks the references; the picture checks
636 of 638 identical to 0.9.16's with Lite off (m427's known river state), 0 blips;
a 2 h 05 min soak on the shipped build, 0 faults, 0 resyncs, no lock-up; `littlejelly:~/MarioParty4-PowerPC-0.9.20.dmg` (md5 e048f6b6).

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
| the pools' ripple (m405 Mario Medley, m417 Makin' Waves, m434 Cheep Cheep Sweep) | the ripple drawn at the water's vertices (`--water`, PLAN.md 59.7); since 0.9.14 the port's own look (PLAN.md 60.6): Makin' Waves' waves x2.5 and no sky; Cheep Cheep Sweep's pond deep water with the sky on it ("sky") or without ("tint"), its reflection unrippled; Mario Medley's colour the console's, its second ripple fainter | GX's indirect warp per pixel is beyond the Radeon 9000's fixed pipeline (§50.14, §52.8); the looks are the user's notes from watching m417 on the G4; `--waterlook console` is the console's amplitude and sky; **the pond's two looks are the user's to pick** (`docs/screenshots/m45-water-m434-looks.jpg`) |
| controllers 3 and 4 | untested | wired the same way as 2 (§49.8: every Xbox One pad claimed, every SDL joystick opened, ports in order); there is one pad in the house |
| one memory card | slot A only; slot B is always empty | the game needs one card; a second would be a second image file and nobody has asked |
| one-CPU Macs and the movies | on one processor, Stamp Out! (m415) falls to about 13 frames a second and 94% of the console's speed, with one-second catch-ups and the sound breaking up (285 underruns in its minigame) | Stamp Out! reads its own picture back to paint with (§20's canvas copy reads), and on one CPU the game thread also replays the render thread's work. The same minigame on two CPUs: 20 fps, 100.6%, 0 underruns. It is **not** the movies: §54.2's A/B found 0 underruns in every movie on one CPU; the extra underruns M38 blamed on them were this minigame, which the movies' schedule deals where `--nomovies` deals m412. The reference machine has two CPUs; the Read Me says what one gets |
| one-to-two-second pauses, ~2.6 an hour | the game stops and catches up, the music with it | §54.4's seventeen in 6 h 30 min: 3 cold reads of files outside the resident set (Bowser's space, the board's ending), 2 card flushes waiting for the previous one, 10 on the lab's own `--status` log write, 2 unattributed; under all of them the G4's drive answering some requests in 1–2 s (26 card renames at 1.68–1.81 s); replayed from snapshots at the same frames, the Bowser read took 4 ms and the status-line frames 0 resyncs (§54.9). Two port-side fixes are small (the card writer coalescing, the log written on a thread) and one is a list edit (the resident set learning those files); each wants a soak of its own, so none went into the build under test |
| the picture about a level bright | not visible side by side | GX truncates at each TEV stage, GL's fixed function rounds (§53.8, §53.11); measured on the movie and m432's walls |
| m403's lamp, m432's walls, m450's +400 | small differences read and not fixed | §50.8's rows: the lamp's halo duller; the walls a level brighter (no channel bug, §53.11); m450 is the play |

## Open lines for the decision

Each line is something the evidence does not settle, for the user to weigh;
none is a known fault.

1. **The pauses -- taken into 0.9.9 (M40)**: the three fixes below are in,
   and a two-hour soak on 0.9.9 had **0 resyncs** against 0.9.8's 2.6 an
   hour (PLAN.md 55.7; the drive still takes ~1.7 s on card renames, now
   behind the game). Six hours, as the line below asked, is what the
   leave-behind soak will have when it is read. The original line: ship 0.9.8 with them, or take the three small
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
8. **Cheep Cheep Sweep's pond (M45)**: two looks built, "sky" (the default:
   deep water with the sky and the trees on it, the console's reading) and
   "tint" (the same water, no sky, as Makin' Waves); the user's pick
   (`docs/screenshots/m45-water-m434-looks.jpg`; `--pondlook tint` or
   `pondlook = tint` in the config switches it; the default is one line in
   `gx_water.c`).
9. **m427's black left half (M45) -- found and fixed in M46** (PLAN.md
   61.10): a copy read-back (Stamp Out!'s) left the scissor test off for
   the rest of a windowed run, and the right view's copy-and-clear wiped
   the left view in every Right Oar Left? after it.  Fullscreen play turned
   the test back on every frame and never showed it.  `--oldscissor
   --readbacktest` reproduces the old picture on demand.
10. **The pillar lights at 30 fps (M46)**: the console's four-frame shimmer
   cannot be shown at thirty frames a second; 0.9.15 draws each presented
   frame's lights between its two game frames (a gentle pulse, the same on
   every stretch), where 0.9.14 showed a steady ball or a strobe by chance
   (`docs/screenshots/m46-bowser-lights-realtime.jpg`: 0.9.14, 0.9.15, the
   console).  Whether it reads right on the G4 is the user's eye;
   `--nolights` is the old picture.
11. **PowerPCube's controls (M46)**: tested on the G4 through `--keytest`
   (the keyboard's own poll fed a held key: Space -> A with the file, Z -> A
   without) and the logged table; no real key press reached the game from
   the lab (System Events' taps are shorter than a frame), no pad remap was
   run with a pad, and PowerPCube itself has not launched this build.
12. **w01's rule**: 29.6 pooled / 30.0 board-only this time (M45: 29.2 /
   30.0) -- the pooled median lies where the board's lines and the teleports'
   hand-over lines meet, so it moves between runs; the rule is the user's.
13. **A card-side skin (M47, item 4)**: measured, not built -- at most a
   frame a second on m435 (`--skinfree`, the multiplies gone), nothing that
   reaches the bar on m436/m431/m444; its rounding moves 1-3 edge pixels a
   frame on m436 and none of the three md5 frames (`--skinround`,
   `docs/screenshots/m47-gpuskin-rounding.jpg`).  The tree's M18 palette
   draws skinned characters broken today
   (`docs/screenshots/m47-gpuskin-palette-broken.jpg`).  The user's call
   whether an inexact option is worth building at all.
14. **The skin at the decode (M47)** is opt-in (`--skindecode`): exact on
   m436 and m414 (after the shared-buffer rule), but on m427 something other
   than the draw reads the arrays it leaves unwritten (the left view's river
   changes, worst 32 levels); `L:m427:@BUNDLE,--skindecode` reproduces it.
   It would take m444 over the bar (29.8 on `c1`) once m427's reader is found.
15. **m427's river at real time has two states** in 0.9.15 and 0.9.16 alike
   (`R:m427:old`: `aa00e2b6` or `dd789764` at +300, by the run's course
   through the entry fade; the left view's river, worst 5 levels).  Not a
   regression; recorded so the picture checks' pairing reads it right.
16. **The G4 restarted once on its own** (2026-09-27 23:51, M47): no panic,
   the upstairs switch dropped littlejelly's link at the same second; with
   `autorestart` 0 it came back within a minute -- a brown-out reads best.
   Worth a look at the upstairs power if it happens again.
17. **Lite mode's options (M49) -- the user's approval -- taken in M50** (the user kept every
   option: the default set, the extras below the reference, `m441.char` out; PLAN.md 65.2).  Fifteen pictures,
   `docs/screenshots/m49-lite-*.jpg` (console-exact / Lite, the same lockstep frame, the most
   changed patch enlarged).  On at auto on the reference: the lighter character file in the
   seven (m1 -> m2; m2 -> m3 in m401 and m463: at the size played, a little blockier in the
   hands and hair) and no butterfly shadows in m441.  Off at auto, on below the reference or
   by `liteopts`: m441's net/basket shadows and every other big flower, m401's fish (10 a
   school) and bubbles, m436's plate shadows (visible) and pillar shadows, m435's pillar
   shadows.  A gentler set: the character files alone (m441 then 29.9).  PLAN.md 64.5 has each
   option's cost.  **M49b: `m441.char` left the auto set** (it moves the net's joint the game
   reads: PLAN.md 64b.1); m441 is at 29.1 with the butterfly shadows alone.  The user's call:
   also `m441.nshadow` + `m441.rings` (30.0 in the A/B; proved to the minigame's end on both
   casts, PLAN.md 64b.4; the pictures are among the fifteen), or leave m441 short.
18. **m433 Beach Volley Folly (M49)**: 29.4 on the count, 29.9-30.0 in A/Bs -- the one screen
   short; no Lite option is allowed there (the game reads the hand joint a lighter file moves).
   **M50: not short** -- five runs each, 0.9.18 29.9 x5, 0.9.16 29.9 (PLAN.md 65.3); 29.8 on
   0.9.19's count.  **m401 Manta Rings** took its place on the count: 29.1 (29.1 / 28.9 / 29.9),
   30.0 in the A/B right after (0.9.18 29.9) -- the same edge.
19. **The round shadow (M50)**: `m441.blob`, the user's request, built and proved; 30.0 / 29.9 /
   29.9 against the rule's 30.0 three times -- an option, not the default.  The user's call to
   put it in `ref` anyway (it is no dearer to draw: PLAN.md 65.2).
20. **Benchmark Mode (M50)**: tested on the G4 (the reference's defaults, twice) and the MacBook
   under Rosetta (slower); never on a real Mac faster or slower than the reference, on Tiger, or
   on one CPU.  The menu keys F1 and M could collide with a PowerPCube controls file that maps
   M or F1 to a button.
21. **m441.bloball (M51)** -- the user's picture to approve: every Butterfly Blitz shadow a round
   blob, 30.0 x3, 3.9 ms cheaper than today's set; `liteopts = ref,m441.bloball` now, into the
   default set if the user says so (`docs/screenshots/m51-blob-all-m441.jpg`).
22. **The Dolphin replay is exact only where people are the clock (M51, PLAN.md 66.3).**  Exact
   over a whole session would need the port to (a) compute floats as the console does (MWCC's
   fused multiply-adds and MSL's sin/cos/atan2, to the last bit -- the positions in a physics
   minigame drift after ~15 s and a collision falls the other way after ~34 s) and (b) wait on
   its disc and sound as the console does (a COM's turn starts 49 frames later on the console).
   Both are projects; until then the Gecko hand-over at each instruction card makes each
   minigame start in step.  Marathon recordings (the teleport's parks) do not convert yet.
23. **The marathon with real people**: tested on the G4 with a scripted player 1; never with the
   user's own pads in a long run (a child per minigame: the window closes and opens between
   minigames, about a minute of fast-forward each on the G4 (57 s measured)).
