# M48 working notes (PLAN.md 63)

## item 0: the M47 leave-behind soak
0.9.16 d7da16eb pid 75831, started ~11:00 G4, stopped by SIGINT 11:26:44 (EXITCODE=0).
89,896 retraces, game 1499.77 s vs wall 1499.87 s = 100.0%; 0 faults, 0 skin guard hits, 0 resyncs,
worst 658.8 ms behind (a load), 43 stall lines, 74 underruns 1106 ms; board to turn 10;
minigames 401 402 406 410 411 412 415 419 420 422. docs/soak/m48/m48-soak-m47-leave.log.gz

## item 1: --repeatstat (mp4-rs cf69288e), window entry+300..+1500 (cs 3000..4800), realtime
see rs/*.txt. The measurement's own cost 13-15 ms a drawn frame (not in the columns): the
absolute ms are inflated, the shares are the reading.

## glists (render-thread display lists for region draws)
RS-m441 --vcache on: strict repeats' rt 9.53 -> 11.69 ms with --glists; total rt 24.25 -> 26.85.
L-m441 frames identical (d44a6ac2/5e319d4d); NA walk refs with --glists.
compile ~0.6 ms each (17057 in the walk: 18.9 s).

## compiled draws cd1 (41e8fe9a): exact (L-m441, NA walk) but pmc: replay 4.36 M cycles +17k L3
misses/drawn frame vs 7.4 M old translation; net -0.36 M. A: 25.9 vs 26.9 (1 run each).
