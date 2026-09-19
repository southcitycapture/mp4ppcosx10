# Mario Party 4 reference notes (USA Rev 1, `GMPE01`)

Observations from the Dolphin reference rig plus the corresponding code in the decomp.
Everything here is about *what the port has to reproduce*, not about how to build it.

Frame numbers are **Dolphin frame-dump indices** (`framedump_N.png`, N starting at 1 =
the first presented XFB frame after boot). See `port/docs/reference-dolphin.md` §6 for
how those relate to the game's own `GlobalCounter` / `VCounter`.

---

## 1. What the game does unattended — yes, there is an attract loop

There is **no recorded-input demo**. What MP4 has is a title-screen idle timeout that
falls back to the logo + opening-movie sequence and then returns to the title, forever.

`src/REL/bootDll/main.c`:

```c
650      for (i = scale_time = 0; i < 1800; i++) {          // BootTitleExec, NTSC
             if (HuPadBtnDown[0] & PAD_BUTTON_START) { ... return 1; }
         }
754      return 0;                                          // timed out
...
277      if (!BootTitleExec()) {
278          HuPrcSleep(60);
279          goto repeat;                                    // back to line 151
280      }
```

So the unattended cycle is: **Nintendo logo → Hudson logo → `movie/opmov_a00.thp` →
title → 1800 idle frames → repeat**. Nothing else happens; there is no demo minigame
and no board demo. Grepping `src/` and `include/` for `attract`, `autodemo`, `demoplay`
finds nothing; `demoWinId` / `UpdateDemoMess` / `demoMessTimeTbl` in `bootDll/main.c`
are the opening movie's *subtitle* window, and `DemoFrameBuffer1/2` / `DemoStatEnable`
in `src/game/init.c` are Nintendo SDK `DEMO*` library names.

## 2. Measured boot and attract timing

From a 14,557-frame unattended capture (`port/ref/frames/boot-*.png`, full-size set in
the scratchpad). Brightness-threshold boundaries, so ±1 frame.

| Frames | Duration | What |
| --- | --- | --- |
| 1–3 | 3 | black |
| 4–30 | 27 | fade up from black into the Nintendo logo |
| 31–178 | 148 | **white**: Nintendo logo (visible ≈ 8–58), gap, Hudson logo (visible ≈ 108–168) |
| 179–186 | 8 | fade to black |
| **187–4381** | **4195** (70.0 s) | opening movie `opmov_a00.thp` |
| 4382–4422 | 41 | white wipe |
| 4423–4453 | 31 | wipe down into the title |
| **4454–6304** | **1851** (30.9 s) | **title screen, "PRESS START"** — stable, mean luma 135 |
| 6305–6333 | 29 | fade to white |
| 6334–6812 | 479 | **white**: attract-repeat logos (Nintendo then Hudson) |
| 6813–6820 | 8 | fade to black |
| 6821–11015 | **4195** | opening movie, second pass — bit-identical length |
| 11016–11056 | 41 | white wipe |
| 11090–12935 | 1846 | title, second pass |
| 12936–12966 | 31 | fade to white |
| 12967– | | third pass |

**Attract cycle period: 6,636 frames ≈ 110.6 s.** Title-to-title: 4454 → 11090.

### The cold-boot logos are shorter than the attract-repeat logos, and that is a hazard

Cold boot spends **186 frames** on both logos; each attract repeat spends **487**. That
is not a bug in the capture, it is the code:

* Cold boot (`bootDll/main.c:170, 205`) holds each logo
  `while (OSTicksToMilliseconds(OSGetTick() - tick_prev) < 3000)` — a **wall-clock**
  minimum that exists only to hide `CharInit`/`MGSeqInit` and the sound-group DVD load
  behind the logo. Whatever the load already consumed comes off the wait.
* The repeat path (`:175, :210`) uses `for (i = 0; i < 180; i++)` — a **frame** count,
  skippable with START/A. 180 + 180 + wipes ≈ 487. ✔

**Consequence for the port:** the cold-boot logo duration is a function of how fast the
host loads from disc and is *not* a frame-exact reference. Do not diff frames 1–186
against the port and expect them to line up. The first genuinely comparable frame is
the start of the opening movie, and the first genuinely comparable *sequence* is an
attract repeat (frames 6334 onward), which is pure frame counting.

## 3. Menu structure

Scenes are REL overlays dispatched by `omMasterInit` / `omWatchOverlayProc`
(`src/game/objmain.c`); the id enum `OMOVL` is generated from
`include/ovl_table.h`, and the current id lives in `omcurovl` (see §7).

```
DLL_bootdll        src/REL/bootDll/          logos, opening THP, title, attract loop
  └─ START ──────► DLL_modeseldll  src/REL/modeseldll/
                     main.c        mode select   (Party / Story / Minigame / Extra / Present / Option)
                     filesel.c     file select   (memory card slots)
       ├─ Party ──► DLL_mentdll    src/REL/mentDll/     party + story menus, board launch
       │              └─ board ──► DLL_w01dll … w06dll, w10/w20/w21dll
       │                             └─ minigame ─► DLL_instdll  (rules screen)
       │                                            DLL_m401dll … m463dll  (the minigame)
       │                                            DLL_resultdll
       ├─ Minigame ► DLL_mgmodedll  src/REL/mgmodedll/  minigame mode: select, play, records
       ├─ Extra ───► DLL_mpexdll,  DLL_ztardll
       ├─ Present ─► DLL_present
       └─ Option ──► DLL_option
```

Observed on the rig (see `movies/key-frames.md` §B), holding START from boot skips both
logos and the 70 s opening movie and lands on the title in ~300 frames. Accepting the
title gives, in order: **SELECT A FILE** (three save slots) → new-file opening scene
(letter, card fan, "Pick a card to get this party started!") → **PARTY MODE** stage →
"Would you like to hear the rules for the Board Map?" → "One human player and three
computer players are joining this party." → **character select** → **board settings**
(Teams / Turns 20 / Mini-Games ALL / Bonus ON / Handicap).

Character/handicap selection is `src/REL/selmenuDll/`. Minigame metadata (overlay id,
type, record index, name, data dir, instruction pictures) is the table `mgInfoTbl[]` at
`src/game/objsub.c:11`, address `0x80131350`.

## 4. CPU difficulty

Two independent settings.

**Per-player CPU level** — `PlayerConfig.diff` (`s16`, offset 0x04 of the 0x0A-byte
struct) in `GWPlayerCfg[4]`, copied into the runtime `PlayerState.diff : 2` bitfield in
`GWPlayer[4]` at `src/game/board/player.c:331`. Values 0–3, labelled in
`src/REL/selmenuDll/main.c:750`:

```c
char *diffStr[] = { "EASY", "NORMAL", "HARD", "VERYHARD" };
```

So difficulty is chosen **per CPU player on the character-select screen**, not once for
the match — confirmed on the rig: at `menu-2800.png` the three CPU slots each carry a
`COM` badge above an `EASY` badge, and player 1 carries `1P`. `EASY` is the default. It can also be changed mid-board from the pause menu
(`src/game/board/pause.c:948`, which writes both `GWPlayer[i].diff` and
`GWPlayerCfg[i].diff`). **Very Hard is locked** behind `GWGameStat.veryHardUnlock`,
which is set only by finishing Story mode (`src/REL/mstory2Dll/ending.c:106`) — so on a
fresh save the reference rig can only select Easy / Normal / Hard.

Human-vs-CPU is derived from *pad presence*: `PlayerConfig.iscom` is set from which
controller ports report a pad in `src/REL/modeseldll/main.c:145-152`. This is why the
pinned config sets `SIDevice1..3 = 0` — with only port 1 populated, players 2–4 are
automatically CPU.

**Story-mode difficulty** is separate: `SystemState.diff_story` (`u8` at offset 0x01 of
`GWSystem`), set by `BoardStoryConfigSet()` (`src/game/board/main.c:351`) from the story
menu (`src/REL/mentDll/main.c:466`).

The AI consumes `GWPlayer[p].diff` in `src/game/board/com.c` (lines 386, 415, 446, 498,
541, 616, 703) and in `boo.c`, `boo_house.c`, `shop.c`, `char_wheel.c`.

## 5. Memory card behaviour

* `HuCardInit()` runs unconditionally at the end of `HuSysInit` (`src/game/init.c:82`)
  and succeeds with or without a card. **Boot is never blocked by a missing card** — you
  reach the title and mode select normally.
* The prompt lives on the **file-select screen**, entered from mode select
  (`src/REL/modeseldll/filesel.c`). `fn_1_562C()` (line 580) probes both slots via
  `HuCardSlotCheck(i)`: `-2` (no card) → message `MAKE_MESSID(16, 0x37)`, wrong sector
  size → `0x39`, `-128` → `0x53`. It returns 0 and the screen carries on in a degraded
  state; `SLSaveFlagSet(0)` (`filesel.c:239, 266, 525`) turns saving off and the session
  runs entirely from the in-RAM `GWGameStat` defaults set in `GWInit`
  (`src/game/gamework.c:119`), including `veryHardUnlock = 0`.
* Error recovery for a card that *is* present is `SLCardMount()` (`src/game/saveload.c:693`):
  `WRONGDEVICE` → msg 7, `FATAL_ERROR` → msg 1, `NOCARD` → msg 0, `BROKEN` → offer to
  format (msg 5).

**A card is mandatory to get past the title.** With both slots empty the rig reaches
SELECT A FILE, prints "No valid Memory Card is inserted.", and stops there — pressing A
for thousands of frames does nothing. So the "degraded state" the code describes is
degraded enough to be a dead end in practice: the pinned config uses `SlotA = 1`
(`EXIDeviceType::MemoryCard`) with Dolphin's auto-created `.raw`.

The card is therefore part of the reproducibility surface. A capture that must be
repeatable has to **delete the card before each run** so `GWGameStat` comes back at its
`GWInit` defaults (`veryHardUnlock = 0`, no records, no unlocks) and the menus take the
same branches. The card image is not committed.

## 6. Determinism — what the port must get right

### 6.1 Both RNGs are seeded from the real-time clock

This is the single biggest threat to frame-exact comparison.

**Engine RNG** — `src/game/frand.c`, a Lehmer/MINSTD generator (m = 2³¹−1, a = 16807,
q = 127773, r = 2836) implemented with Schrage's method:

```c
 7  static inline u32 frandom(u32 param) {
11      if (param == 0) {
12          param = rand8();
13          param = param ^ (s64)OSGetTime();     // <-- clock
14          param ^= 0xD826BC89;
15      }
17      rand2 = param / (u32)0x1F31D;
18      rand3 = param - (rand2 * 0x1F31D);
19      param = rand2 * 0xB14;
20      param =  param - rand3 * 0x41A7;
21      return param;
22  }
```

`frand_seed` is in `.bss` and therefore 0 at boot, so the *first* call re-seeds. That
call is `rnd_temp = frand();` at `src/game/init.c:77`, inside `HuSysInit`. Note the
re-seed branch fires **any time the state reaches 0**, not only at startup.

`rand8()` (`src/game/main.c:136`) is itself a fixed-seed LCG
(`rnd_seed = rnd_seed * 0x41C64E6D + 0x3039`, initial `0x0000D9ED`) — deterministic on
its own. It is only the `^ OSGetTime()` that breaks things.

**Board RNG** — `src/game/board/main.c`:

```c
1430  void BoardRandInit(void) { boardRandSeed = OSGetTime(); }     // <-- clock
1436  u32  BoardRand(void)     { boardRandSeed = boardRandSeed*0x19660D + 0x3C6EF35F;
1437                             return boardRandSeed; }
```

Called once from board setup (`board/main.c:660`).

**There are exactly two clock-derived seeding sites**: `frand.c:13` (reached from
`init.c:77`) and `board/main.c:1432`. Everything else that touches the clock is file
timestamps in `saveload.c` and timeouts in `audio.c`. The port needs a build-time or
replay-time override at those two sites; otherwise no two runs of the port agree with
each other, let alone with the reference.

On the Dolphin side the equivalent knob is `Main.Core.CustomRTCEnable` /
`CustomRTCValue`, and a `.dtm`'s `recordingStartTime` field. With the RTC pinned, the
rig is reproducible: **two independent captures of the first 4,000 frames produced
byte-identical PNGs** (verified by md5 at frames 100/300/1000/2000/3000/4000).

### 6.2 Frame pacing

The game is a strict 60 Hz single-field loop and has no frame-skip path:

* `minimumVcount` (`0x801D3B04`) is set to 1 in `src/game/init.c:81` and is only ever
  re-set to 1 elsewhere (`m402Dll`, `m429Dll`, `m441Dll`, `m450Dll`). **No caller ever
  passes 2.** There is no 30 Hz mode.
* The throttle is `HuSysDoneRender()` (`src/game/init.c:197`); with `minimumVcount == 1`
  its `while (VIGetRetraceCount() - retrace_count < minimumVcount - 1)` never runs and
  the single wait happens in `SwapBuffers()` → `VIWaitForRetrace()`.
* `PADRead` is called exactly once per retrace, from the VI post-retrace callback
  `PadReadVSync` (`src/game/pad.c:152`, installed at `pad.c:60`). So **one poll per
  frame**, and a `.dtm` for this game has `inputCount == frameCount` and `lagCount == 0`.

Two ways frames get lost, both of which the port must reproduce if frame numbering is
to stay aligned:

1. `src/game/main.c:87` — `if (HuSoftResetButtonCheck() != 0 || HuDvdErrWait != 0) continue;`
   skips the loop body **without** incrementing `GlobalCounter`, while `VCounter` keeps
   ticking.
2. Any overrun past one retrace. `worstVcount` (`0x801D3AFC`) records the worst span
   seen. On hardware this is a dropped frame; the XFB is re-presented, so Dolphin's PNG
   counter still advances.

### 6.3 Other things that must match

* **DVD timing** leaks into visible behaviour through the cold-boot logo waits (§2) and
  through `HuDvdErrWait`. A port with instant asset loads will show shorter logos.
* **No input-replay facility exists in the game.** There is no ring buffer, no recorded
  input array, nothing to reuse. The clean insertion point for a port-side replay layer
  is `HuPadRead()` (`src/game/pad.c:130`) — everything downstream reads only the
  `HuPadBtn` / `HuPadBtnDown` / `HuPadBtnRep` / `HuPadStkX` … globals, so a shim there
  covers the whole game. The reference rig injects at exactly the mirror-image point:
  it writes the *private* `_PadBtn` / `_PadBtnDown` / `_PadDStk` arrays from a Dolphin
  Gecko code at the VI hook, which `HuPadRead` then latches on the next frame. Writing
  the public `HuPad*` globals does **not** work — `HuPadRead` runs after the hook and
  overwrites them. Build the port's replay layer to feed `_Pad*`, not `HuPad*`, and the
  same input scripts drive both.
* Button auto-repeat is 20 frames of delay then every frame (`pad.c:187`); stick repeat
  is 20 then every 2. A replay layer must feed `HuPadRead` raw state and let the repeat
  logic run, not synthesise `BtnDown` itself.

## 7. Useful addresses (Rev 1 `main.dol`)

From `config/GMPE01_01/symbols.txt` (7,721 entries). RELs are relocatable and have no
fixed addresses here — only `m444dll`, `mstoryDll`, `mstory2Dll`, `mstory3Dll` have
per-REL symbol files.

| Symbol | Address | Meaning |
| --- | --- | --- |
| `GlobalCounter` | `0x801D3A54` | main-loop frame counter (`main.c:119`) |
| `VCounter` | `0x801D3A58` | VI retrace counter (`pad.c:222`) — diff vs above = dropped frames |
| `minimumVcount` / `worstVcount` | `0x801D3B04` / `0x801D3AFC` | pacing (always 1) / worst retrace span |
| `frand_seed` | `0x801D3D10` | engine RNG state |
| `boardRandSeed` | `0x801D3F14` | board RNG state |
| `rnd_seed` | `0x801D342C` | `rand8()` LCG state (`.sdata`, init `0x0000D9ED`) |
| `omcurovl` | `0x801D3CE0` | **current scene / overlay id** — the single best "where am I" watch |
| `omnextovl` / `omprevovl` | `0x801D3CE4` / `0x801D349C` | pending / previous overlay |
| `omovlevtno` | `0x801D3CD4` | current overlay's event number |
| `SystemInitF` | `0x801D3A00` | 0 = cold-boot logo path, 1 = attract-repeat path |
| `GWSystem` | `0x8018FCF8` | `SystemState` (0xDC): `+0x01` diff_story, `+0x04` turn, `+0x05` max_turn, `+0x08` board, `+0x0A` player_curr, `+0x34` mg_next |
| `GWPlayer` | `0x8018FC38` | `PlayerState[4]` (4 × 0x30): `+0x00` bits `diff:2, com:1, character:4`, `+0x0C` space, `+0x1C` coins, `+0x2A` stars |
| `GWPlayerCfg` | `0x8018FC10` | `PlayerConfig[4]` (0x28): character / pad_idx / diff / group / iscom |
| `GWGameStat` | `0x8018FDD8` | save-file stats (0x118), incl. `veryHardUnlock` |
| `HuPadBtn` / `HuPadBtnDown` | `0x801D3AD8` / `0x801D3AD0` | latched pad state, 4 × u16 |
| `HuPadStkX` / `HuPadErr` | `0x801D3AC4` / `0x801D3AA4` | 4 × s8 |
| `curSlotNo` | `0x801D3EA8` | active memory-card slot |
| `mgInfoTbl` | `0x80131350` | minigame metadata table (0xE00) |

Breakpoint targets: `main` `0x800057C0`, `rand8` `0x80005A30`, `frand` `0x800325F4`,
`frandmod` `0x80032778`, `BoardRandInit` `0x8005FAF8`, `BoardRand` `0x8005FB1C`,
`HuPadRead` `0x80005B4C`, `HuSysDoneRender` `0x8000A0DC`, `omMain` `0x8002FB40`.

A minimal `MemoryWatcher/Locations.txt` for a capture would be:

```
801D3A54
801D3A58
801D3CE0
801D3D10
801D3F14
```

(see `port/docs/reference-dolphin.md` §7 — MemoryWatcher is not confirmed enabled in
this Dolphin build).

## 8. Decomp status, for context

Both USA revisions are 100 % matching (`GMPE01_00`, `GMPE01_01`): 10,985 / 10,985
functions, 616 / 616 complete units, 5,938,052 bytes of code. PAL and JP are not.
Matching is not the same as documented — the README notes that most non-engine code is
undocumented, and the RELs are still full of `fn_1_*` / `lbl_1_bss_*` names (all of
`modeseldll/filesel.c`, for instance). There is no PC port yet.

## m406 on the console (2026-09-19, m406-end.txt schedule, capture on the Mac)

m406 is the downhill ski race. Frames `m406-console-*.png`: 12500 the race
(four skiers on the slope), 13800-14300 the ending: the winner (Peach) stands
centre celebrating and the THREE LOSERS ARE BURIED IN SNOW MOUNDS WITH ONLY
THEIR HEADS SHOWING. So a head on a snow mound at the end of m406 is BY
DESIGN, not a skinning fault. The race itself is ~1,500 frames on the
console (12,500 -> 14,000); the port's stalled visit sat in the module for
57,000+ frames. Not captured: a DRAW ending (nobody wins), which is what the
port's restored run produced; the striped garbage polygon the user
photographed on the G4 in that DRAW frame has no console counterpart here
and is still unexplained.

## m415 on the console (2026-09-19, capture on littlejelly, PLAN.md §35.3)

m415 is **Stamp Out!** (its instruction card, `m415-console-9600.png`), not
Trace Race. Schedule: `m406-end.txt` with the `mg_next` poke changed to 14
(index = minigame − 401); Dolphin Flatpak on littlejelly at ~9 fps with
`DumpFramesAsImages = True` under `[Settings]` of GFX.ini (an AVI otherwise;
no ffmpeg on that host), ~25 min to frame 12,700. Frames: 10300 the paper
before the start, 10973 mid-game, 12550 "MARIO WON!", 12700 the results.
What they settle for the port: (1) the paper is WHITE with faint blue line
art — the port's read-back canvas (`port_gx_copy_read`) matches, the
zero canvas (lavender) was wrong; (2) the toys around the paper (star
balls, the blue house, the red mushroom stamp, the yellow star, the
pencil) are textured and coloured on the console and plain white on the
port — an open m415 fault, snapshot `snaps/lib/m415-white-toys-f016000.snap`
on the G4; (3) the minigame results screen has the four character
portraits in its boxes; the port's boxes are blank (pre-existing since M8,
`screenshots/mp4-minigame-result.png`).
Note for `capture-linux.sh`: killing the Flatpak wrapper PID does not
reach `dolphin-emu` inside the sandbox — two captures on 2026-09-19 ran on
as orphans (one for 37 min, 8.9 GB of AVI); kill `pgrep -x dolphin-emu`.
