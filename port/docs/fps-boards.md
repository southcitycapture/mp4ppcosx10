# The boards' frame rate (M39c, M40's input)

Every board module the game has, measured on the dual 1 GHz G4 (Leopard,
Radeon 9000) with the 0.9.8 code plus M39c's harness levers (the game's path
is unchanged: the md5 walks are identical, PLAN.md §54b.1). The bar is
M40's: median presented fps >= 29.5 at >= 99% game speed
(`tools/fps_board.py`, `docs/fps-scoreboard-m39-soak.md`).

**Presented fps**: `isle --soak --com4 --rtc dolphin --freshcard --status --perf
--board N --turns 5 --ovllog --dvdlog` at real time, movies on, over the
board module's own status lines (every turn pooled; a minigame's lines count
for the minigame, not the board). Board 1's row is M39's 6 h 30 soak (five
20-turn boards). The extras were reached with `--goto w20dll|w21dll:0:0
--turns 10`. Logs: `docs/soak/m39c-R*.log.gz`, `m39c-X-w*.log.gz`.

**Per drawn frame** (`tools/m39c_perf.sh`, `tools/m39c_fps.py`): each board's
survey walk (`--board N --nomovies --play board-start-com4.play`, entry at
frame 5108), fast-forwarded to entry +2700, then at real time to +3900 (the
host's first Star and turn 1) with `--perfdump`: medians over the drawn
frames of +3000…+3900, the texture cache warm. *game* = the game thread's ms,
*decode* = the render thread's vertex decode + the game thread's share,
*replay* = the render thread replaying the frame into GL. GL calls and
vertices are from `--gltrace`'s forty presented frames up to +3800. Logs:
`docs/soak/m39c-P*-perf.log.gz`.

| board | turns | presented fps median / p10 | speed | verdict | drawn frame: game / decode (render + game) / replay ms | GL calls a drawn frame (draw calls) | vertices a drawn frame |
|---|---:|---:|---:|---|---:|---:|---:|
| w01 Toad's Midway Madness | 5 × 20 | 29.9 / 25.4 | 100.1% | PASS | 12.3 / 5.8 + 0.0 / 13.5 | 344 (93) | 12,736 |
| w02 Goomba's Greedy Gala | 5 | 30.0 / 28.8 | 100.1% | PASS | 12.4 / 9.0 + 0.0 / 16.2 | 369 (89) | 12,890 |
| w03 Shy Guy's Jungle Jam | 5 | 29.9 / 23.9 | 100.2% | PASS | 14.0 / 6.7 + 0.0 / 16.7 | 300 (80) | 12,747 |
| **w04 Boo's Haunted Bash** | 5 | **24.8 / 21.1** | 100.1% | short by 4.7 | 15.2 / 8.6 + 3.5 / **23.8** | 290 (80) | 11,623 |
| **w05 Koopa's Seaside Soirée** | 5 | **26.0 / 22.1** | 100.1% | short by 3.5 | 15.9 / 8.7 + 2.0 / **23.4** | 304 (91) | 13,504 |
| w06 Bowser's Gnarly Party | 5 | 30.0 / 26.6 | 100.1% | PASS | 14.3 / 7.4 + 0.0 / 16.5 | 231 (54) | 10,984 |
| w20 Mega Board Mayhem (Extra Room) | 10 | 29.9 / 25.0 | 100.0% | PASS | — | — | — |
| w21 Mini-Board Mad Dash (Extra Room) | 10 | 30.0 / 26.9 | 100.0% | PASS | — | — | — |
| w10 the tutorial | — | not measured: a script read by a human (PLAN.md §54b.3) | | | | | |

**What M40 starts from.** Two boards miss the bar, Boo's and Koopa's, and
both for the same reason: the render thread's replay (23.8 and 23.4 ms a
drawn frame against Toad's 13.5), while their GL call counts and vertex
counts are no higher than the passing boards'. The game thread is not the
limit (15–16 ms), and the replay's time per call is what grows: both boards
draw large translucent/fogged surfaces (Boo's mansion interior, Koopa's
sea), so the first suspects are fill rate and blended overdraw on the
Radeon, then the auto plan pushing decode onto the game thread (the only
two boards where the game thread decodes at all: 3.5 and 2.0 ms). The four
passing boards' p10s (23.9–28.8) are the turns' camera sweeps and the
host's dialogs.
