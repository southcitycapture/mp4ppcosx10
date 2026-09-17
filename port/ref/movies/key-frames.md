# Key frames — the comparison points

Frame numbers are Dolphin frame-dump indices (`framedump_N.png`, N from 1 = the first
presented XFB frame). Regenerate any capture with `port/ref/tools/capture.sh`; the
committed 320×264 copies are in `../frames/`.

## A. Unattended boot and attract loop — `boot-NNNN.png`

No input at all. Captured with the pinned config and nothing else; 14,557 frames.
**Verified reproducible**: two independent runs produced byte-identical PNGs at frames
100 / 300 / 1000 / 2000 / 3000 / 4000.

| Frame | Moment |
| --- | --- |
| 1 | first presented frame (black) |
| 4 | fade up begins |
| ~8–58 | Nintendo logo |
| ~108–168 | Hudson logo |
| 179–186 | fade to black |
| **187** | **opening movie `opmov_a00.thp` starts** |
| 4381 | opening movie ends |
| 4382–4422 | white wipe |
| **4454** | **title screen up, "PRESS START"** |
| 6304 | title idle timeout (1,800-frame loop) expires |
| 6334–6812 | attract-repeat logos (180 frames each — frame-deterministic, unlike the cold-boot ones) |
| **6821** | opening movie, second pass — exactly 4,195 frames again |
| 11090 | title, second pass |
| — | cycle period **6,636 frames ≈ 110.6 s** |

Caveat: frames 1–186 are **not** a valid comparison baseline. The cold-boot logo hold is
a 3,000 ms wall-clock minimum that DVD loading eats into (`bootDll/main.c:170, 205`), so
its length depends on host load speed. The attract repeat at 6334+ is pure frame
counting and is the right thing to diff. See `../notes.md` §2.

## B. Menu walk — `menu-NNNN.png`

Driven by `menu-walk.txt` → Gecko codes (`Dolphin.Core.EnableCheats=True`), with a
memory card in slot A. Frame numbers are from that run and only reproduce with that
exact script, config, and a **freshly created** memory card.

| Frame | Moment |
| --- | --- |
| ~300 | title (logos and opening movie skipped by holding START) |
| ~380 | title accepted |
| ~450–520 | **SELECT A FILE** — three save slots |
| ~700 | file created, transition |
| ~875–1400 | new-file opening scene (letter, card fan, "Pick a card to get this party started!") |
| ~1750 | Party Mode stage, hosts assembled |
| ~1925 | **PARTY MODE** banner |
| ~2100 | "Would you like to hear the rules for the Board Map?" |
| ~2275 | "One human player and three computer players are joining this party." |
| ~2625 | **character select** — the 8-character grid |
| ~2800 | characters chosen; `1P` + three `COM` badges, each showing **EASY** |
| ~3500 | **board settings** — Teams / Turns 20 / Mini-Games ALL / Bonus ON / Handicap |

The board settings screen is where the walk stops: it is reached but not confirmed.
Blindly pulsing A leaves the cursor on the Mini-Games row, and pulsing START backs out
of the whole flow and eventually returns to the attract loop.

## C. Board and minigame — **captured, 2026-09-17 (M12b)**

*The section below is kept as written because its reasoning was right about the
cost. What it did not know is that Gecko codes can write any memory, not just the
pad, so the board does not have to be navigated at all: poke
`GWPlayerCfg[i].iscom = 1` (all four CPU) and `GWSystem.mg_next` (the minigame), and
a board deals the module you asked for, over and over, with the A metronome doing
everything else. See `m444-drop.txt`, `mg-entries.txt`, `mg-roulette.txt` and
`m453-heap.txt` in this directory, `mkgecko.py`'s `poke` directive, and PLAN.md
§27.*

*Reached and committed as `../frames/`: title, SELECT A FILE, character select
(four `COM` + `EASY`), board settings, board map, board turn 1, m405's first
playable frame, and the whole of m444's module including a ball drop. Read PLAN.md
§27.2 before adding to that set — the `framedump_N` index is not a frame clock in
this Dolphin build.*

### The original note (obsolete)

`first-minigame.txt` is a **skeleton, not a working sequence.** Getting from the board
settings screen into a board and then into a minigame needs cursor moves that were not
calibrated, and each attempt costs a full run from boot:

* ~12,000 frames from boot to a first minigame, at ~30 emulated frames per second of
  wall clock, is **~7 minutes per attempt**, and the input script cannot branch on what
  is on screen.
* Dolphin's `.dtm` playback — which would have let a sequence be recorded once by hand
  and replayed — does not work in this build (`../../docs/reference-dolphin.md` §5.1).

The way to finish it is to calibrate the remaining screens one at a time against the
board-settings frame, extending `menu-walk.txt` and re-running. `dstk:DOWN` / `dstk:UP`
move menu cursors (they write `_PadDStk` / `_PadDStkRep`, which is what the menus read —
not the D-pad bits, which `HuPadRead` masks out). Add a `mark` for each screen as it is
pinned down so this table can be filled in.
