# RC1: playing the whole minigame marathon on the G4

One page for the release-candidate playtest (M52, PLAN.md 67; M53, PLAN.md
68).  The build is **0.9.22** (`~/MarioParty4.app` on the G4, 0.9.21 kept as
`~/MarioParty4-0.9.21.app`; or the disk image
`MarioParty4-PowerPC-0.9.22.dmg`).  Nothing is named RC or 1.0 — that is
your call after this run.

## 1. Before you start

* The G4 as it is every day (Leopard, two CPUs, the Radeon 9000).  Close the
  leave-behind soak first if it is still running (the lab's notes give its
  pid; or just quit the game window).
* A pad on port 1 (the Xbox pad), or the keyboard (Z = A, X = B, Return =
  START; F5 = screenshot; F1 or M = the port's menu).
* About **2½–3 hours** for all 63 minigames (each one about two minutes on
  the G4: a minute of the game hurrying there by itself, then the minigame
  and its results -- the lab's dry run of ten took 22 minutes).  You can stop
  and resume at any point, so three sittings of ~21 minigames work just as
  well (From / To below).

## 2. Running it

1. Open the game, press **F1** (or **M**) → **Developer Mode** →
   **Minigame marathon**.
2. Set the rows (left/right on the stick, the D-pad or the arrow keys):
   * **Minigames**: *All* — or *From/To* for a sitting (e.g. m401 → m421,
     m422 → m442, m443 → m463);
   * **Players at the controllers**: 1 (the computer plays the other three);
   * **Player 1 … 4**: who plays whom (any; your pick is remembered);
   * **Record each minigame**: **On**.
3. **Start the marathon**.  The window closes and a new one opens for each
   minigame: a black screen with "Getting to the minigame — about 60 s",
   then the instruction card, which **waits for you: press START** (Z for
   the practice round).  Play.  After the results the next one loads.
4. To stop: **F1/M → Stop the marathon here** (the current minigame is not
   counted; it is first on resume).  To resume later: **Developer Mode →
   Resume the stopped marathon**.
5. At the end (or a stop) the game opens on the **summary**: every
   minigame, the coins each player won in it, the time, who won most.  It
   is also saved as `~/Documents/MarioParty4 Recordings/Marathon DATE.txt`.

## 3. What to note

For each minigame, only what is *off*: a wrong or missing picture (shadow,
water, colour, a model, black where there should be picture), the frame
rate visibly dropping or stuttering, the sound breaking up or out of step,
a control that does not answer, a hang (no progress for a minute), a crash
(the window vanishes; the marathon stops and says so).  Lite mode is on for
seven heavy minigames (LITE MODE in the Read Me says what it changes) —
that is expected, not a finding.

## 4. Reporting a finding

1. **Press F5 at the moment.**  The picture goes to the Desktop as
   `Mario Party 4 NNNNN (frame F).png` — **F is the game's frame**, the
   number the recording uses.  (The log of that minigame says the same:
   `screenshot (F5): … the game's frame F, recording …`, and the recording
   itself marks the moment.)
2. Note the minigame and the **recording's file name** — in
   `~/Documents/MarioParty4 Recordings/`, named
   `DATE TIME marathon mNNN.rec` (one per minigame, with its memory card
   beside it).
3. The report is three things: **the minigame, the screenshot (its frame F),
   the recording's name.**  With those the lab replays the exact moment on
   the G4, frame for frame (`--replay`), and can video it.  The minigame's
   own log is `~/Library/Application Support/MarioParty4/marathon-logs/mNNN.log`
   (attach it if anything crashed).
4. **If a minigame crashes or stops** (the marathon's summary says
   "stopped" or "crashed"), copy its log **before the next marathon or
   game** -- each run overwrites its own: `marathon-logs/mNNN.log` (outside
   the marathon, `MarioParty4.log` in the same folder).  0.9.22 writes what
   the lab needs into it (PLAN.md 68.3): lines starting `*** port: fault`
   (with every register and, at a model load, the loader's state), `port>
   DATA CHECK FAILED`, `port> RESIDENT COPY CHANGED` or `port> WRITE INTO A
   DATA DIRECTORY IMAGE`.  Any of those lines in any log is worth sending,
   crash or not -- the game carries on past the last three.

## 5. Making the videos

On littlejelly (the lab's tools; the G4 does the work, one job at a time —
do not run it while you are playing):

```sh
scp "g4:Documents/MarioParty4 Recordings/DATE TIME marathon mNNN.rec" ~/rc1/
port/tools/session_video.sh ~/rc1/"DATE TIME marathon mNNN.rec" \
    --from $((F - 600)) --to $((F + 600)) --nodolphin --name mNNN-finding
```

→ `~/mp4-videos/mNNN-finding-g4.mp4`: the G4 replaying those 20 seconds
(F ± 10 s) in lockstep, every frame, with the game's own sound.  For the
whole minigame use `--from 14100` and leave out `--to` (the recording
starts at the boot; before ~14,000 is the minute's hurry, which the replay
runs through undrawn).  The replay draws ~8 frames a second: the lab's dry
run made a 38-s minigame in 8 minutes and a 63-s one in 16
(`~/mp4-videos/m52-m412-g4.mp4`, `m52-m418-g4.mp4`).  `--nodolphin`: marathon recordings carry the lab's teleport
and do not turn into Dolphin movies yet (Read Me, WHAT IS NOT QUITE RIGHT
YET); a session recorded from the boot with **Record this session** does.

## 6. Known before you start

* Toad's Quick Draw (m409) and Beach Volley Folly (m433) sit on the 29.5
  line on the reference machine (29.2 / 29.3 on the last count; 29.5 / 29.9
  over five runs): a dip to 29 there is the machine's edge, not a finding.
* With a Finder window open on the network (SHARED in its sidebar), the
  G4 now and then spends a few seconds of a CPU browsing it; a minigame
  playing then can stutter for those seconds.  Close Finder windows before
  a sitting.
* A minigame that crashes stops the marathon there (the summary says so);
  Resume goes on from it.  The lab has seen one such fault (Dungeon Duos,
  at its first model load, once in 208 loads; none in M53's 125): if it
  happens, the minigame's log in `marathon-logs/` is the report (section 4).
  If its cause is the minigame's data in memory, 0.9.22 puts the disc's
  bytes back and the game goes on -- the log then has a `DATA CHECK FAILED`
  line, which is the report.
* The Dolphin replay of a recording is exact through the menus, the
  board's dialogs and people's turns; inside a long physics minigame the
  two part after tens of seconds (float rounding), and a computer player's
  board turn can start later on the console.
* The round shadows (`<game>.blob`) are **not** on on the G4 — Benchmark
  Mode turns them on only on a Mac slower than this one.
