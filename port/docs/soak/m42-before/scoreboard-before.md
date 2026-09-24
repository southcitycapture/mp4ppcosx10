# M42 before: the three-run verdicts on 0.9.10

Bar: median presented fps >= 29.5 at >= 99% game speed. Logs: 36 (.).

**10 of 13 screens pass.**

Three-run mode (M42): 12 screens landed within 2 fps of the bar on their first run and were run three times; their **median fps is the median of the three runs' medians** (the runs column), their lines, p10 and costs pool the three runs. Every other screen is one run.

Per drawn frame (medians): the game thread's work and its decode share (gdec); the consumed frame's work; the render thread's replay (rt) and decode (dec); draw calls, vertices and GL calls (stream records) handed to GL; vc = vertices the static-geometry cache served.

| screen | lines | median fps | runs | p10 fps | speed | game work / gdec ms | consumed ms | rt ms | dec ms | draws | vertices | GL calls | vc | verdict |
|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| m463dll | 90 | 27.9 | 27.0 / 28.7 / 27.9 | 21.9 | 100.0% | 16.9 / 0.0 | 4.3 | 13.1 | 8.7 | 237 | 48722 | 2656 | 7% | short by 1.6 fps |
| m438dll | 90 | 28.5 | 27.0 / 28.7 / 28.5 | 22.0 | 100.0% | 22.7 / 0.0 | 6.0 | 19.0 | 7.3 | 556 | 48088 | 6577 | 2% | short by 1.0 fps |
| m423dll | 90 | 29.4 | 29.9 / 29.4 / 28.8 | 24.3 | 100.0% | 24.9 / 0.0 | 5.3 | 17.3 | 10.4 | 361 | 85519 | 3792 | 28% | short by 0.1 fps |
| m430dll | 90 | 29.6 | 29.9 / 29.5 / 29.6 | 23.6 | 100.0% | 24.1 / 0.0 | 6.5 | 16.6 | 7.2 | 529 | 50564 | 5304 | 1% | PASS |
| m412dll | 90 | 29.9 | 29.9 / 29.9 / 29.8 | 24.9 | 100.0% | 25.4 / 0.0 | 5.1 | 19.1 | 7.7 | 470 | 73170 | 3795 | 41% | PASS |
| m440dll | 90 | 29.9 | 29.9 / 29.9 / 29.9 | 24.9 | 100.0% | 26.1 / 0.0 | 3.9 | 18.2 | 5.7 | 490 | 67244 | 4954 | 52% | PASS |
| instdll | 60 | 30.0 | 1 run | 22.2 | 100.0% | 14.8 / 0.0 | 5.0 | 8.0 | 4.4 | 168 | 31693 | 2168 | 0% | PASS |
| w01dll | 153 | 30.0 | 30.0 / 30.0 / 30.0 | 24.1 | 100.0% | 20.3 / 0.0 | 5.9 | 14.4 | 5.4 | 307 | 38142 | 3692 | 1% | PASS |
| m406dll | 90 | 30.0 | 30.0 / 30.0 / 30.0 | 25.9 | 100.0% | 18.8 / 0.0 | 7.3 | 13.2 | 8.2 | 311 | 47823 | 3314 | 0% | PASS |
| m407dll | 90 | 30.0 | 30.0 / 30.0 / 29.9 | 26.8 | 100.0% | 21.6 / 0.0 | 4.7 | 16.7 | 4.1 | 401 | 71914 | 3455 | 55% | PASS |
| m415dll | 90 | 30.0 | 30.0 / 30.0 / 29.9 | 26.8 | 100.0% | 18.1 / 0.0 | 5.0 | 13.1 | 7.9 | 308 | 45442 | 3340 | 0% | PASS |
| m449dll | 90 | 30.0 | 29.9 / 30.0 / 30.0 | 27.0 | 100.0% | 19.6 / 0.0 | 3.9 | 17.6 | 6.2 | 336 | 74182 | 2817 | 54% | PASS |
| m429dll | 90 | 30.0 | 30.0 / 30.0 / 30.0 | 28.9 | 100.0% | 17.6 / 0.0 | 4.5 | 14.6 | 10.9 | 270 | 76663 | 3704 | 11% | PASS |
