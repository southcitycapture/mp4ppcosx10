# M47 working notes (folded into PLAN 62)

## item 0
M46 leave-behind (0.9.15, isle 16a042ad, pid 70071): started ~13:10 G4 time, read+stopped 13:35 (SIGINT, EXITCODE=0).
80,100 retraces (status f79980), game 1335.30 s vs wall 1335.40 s = 100.0%; 0 `*** port` faults; skin 0 guard hits;
0 resyncs, worst 668.5 ms behind; board w01 turns 1-9; minigames 401 402 410 411 412 415 419 420 422.
No --halfwatch in the leave command (no blip/half counters). log m47-soak-m46-leave.log.gz

## item 1 skin at the decode (mp4-a fa1f5b59)
L:m436 lockstep: --skinverify 72,254,460 positions + 72,254,460 normals compared, 0 differ (0 unnamed);
frames 14444 8afb5912 / 15344 48a9addc in all three arms (verify, fused, --noskindecode) = M46's T:x436.
190 meshes fusable, 5 not (multi entries) -> their HSFs run the old body (1846 of 8132 bodies in lockstep).
A/B ab1 (3 runs, interleaved, @a vs @a --noskindecode):
 m436 27.9 / 27.9 ; m435 28.9 / 29.0 ; m444 29.7 / 29.2
 drawn frame game_ms 12.8 vs 15.4 (skin off the game thread), gx_ms 14.75 vs 12.2 (gdec 6.2 vs 4.4): level.
pmc m436 (pmc47): object walk 8.10 -> 6.01 M cyc; decode/job 2.08 -> 3.45; vcache keys 1.81 -> 2.02 (lists un-dyn'd: fixed in c).
Reason: the skin multiplies once per LIST vertex (~55k/frame on m436), the body once per ARRAY vertex.
## AltiVec (--skinvec, mp4-b/c)
L:m436 --skinvec --skinverify: 72,254,460 + 72,254,460 compared, 0 differ; VSCR NJ was SET (non-Java) at entry once/twice per run
(each thread) -> set to Java mode by the loop. pmc m436: decode/job 2.80 -> 3.77 M cyc (slower), total 23.27 -> 24.33.
## N walk on d: 0b58c5ee / 2b99c60a / 4a9a640c (refs)
