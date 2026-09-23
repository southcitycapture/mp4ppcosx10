/* M36 (PLAN.md 51.3): the resident set's list, most-read first, as
 * port/tools/m36_dvdlog.py counted the game's reads per file over a
 * --dvdlog run of the deterministic 20-turn soak (dvdlog20-A.log).  dvd_cache.c
 * fills it in this order until the budget is spent; a file larger than
 * what is left is skipped.  Generated -- edit the run, not the list. */
"sound/mpgcsnd.msm", /* 277 reads, 35960 KB */
"sound/mpgcstr.pdt", /* 250 reads, 11109 KB */
"mess/board_e.dat", /* 59 reads, 142 KB */
"data/peachmdl1.bin", /* 57 reads, 481 KB */
"data/luigimdl1.bin", /* 57 reads, 354 KB */
"data/yoshimdl1.bin", /* 56 reads, 386 KB */
"data/mariomdl1.bin", /* 56 reads, 373 KB */
"data/instpic.bin", /* 34 reads, 2866 KB */
"data/bkujiya.bin", /* 23 reads, 743 KB */
"data/byokodori.bin", /* 21 reads, 697 KB */
"data/w01.bin", /* 20 reads, 1404 KB */
"data/bguest.bin", /* 20 reads, 662 KB */
"dll/w01Dll.rel", /* 20 reads, 124 KB */
"data/inst.bin", /* 17 reads, 1493 KB */
"data/result.bin", /* 17 reads, 924 KB */
"dll/resultDll.rel", /* 17 reads, 62 KB */
"dll/instDll.rel", /* 17 reads, 43 KB */
"data/m444.bin", /* 2 reads, 2483 KB */
"data/m438.bin", /* 2 reads, 765 KB */
"dll/m438Dll.rel", /* 2 reads, 111 KB */
"dll/m444dll.rel", /* 2 reads, 103 KB */
"data/bbattle.bin", /* 2 reads, 82 KB */
"data/ment.bin", /* 1 reads, 3914 KB */
"data/title.bin", /* 1 reads, 3645 KB */
"data/m430.bin", /* 1 reads, 2572 KB */
"data/m406.bin", /* 1 reads, 2169 KB */
"data/m429.bin", /* 1 reads, 2064 KB */
"data/m410.bin", /* 1 reads, 1729 KB */
"data/m421.bin", /* 1 reads, 1657 KB */
"data/board.bin", /* 1 reads, 1578 KB */
"data/m423.bin", /* 1 reads, 1403 KB */
"data/m424.bin", /* 1 reads, 1381 KB */
"data/yoshimot.bin", /* 1 reads, 1380 KB */
"data/luigimot.bin", /* 1 reads, 1305 KB */
"data/modesel.bin", /* 1 reads, 1272 KB */
"data/mariomot.bin", /* 1 reads, 1271 KB */
"data/m416.bin", /* 1 reads, 1270 KB */
"data/peachmot.bin", /* 1 reads, 1230 KB */
"data/m405.bin", /* 1 reads, 1226 KB */
"data/m428.bin", /* 1 reads, 957 KB */
"data/m422.bin", /* 1 reads, 938 KB */
"data/m420.bin", /* 1 reads, 865 KB */
"data/m412.bin", /* 1 reads, 836 KB */
"data/m431.bin", /* 1 reads, 827 KB */
"data/m407.bin", /* 1 reads, 666 KB */
"data/mariomdl0.bin", /* 1 reads, 514 KB */
"data/gamemes.bin", /* 1 reads, 486 KB */
"data/blast5.bin", /* 1 reads, 362 KB */
"dll/mentDll.rel", /* 1 reads, 218 KB */
"data/win.bin", /* 1 reads, 139 KB */
"dll/m406Dll.rel", /* 1 reads, 129 KB */
"dll/m428Dll.rel", /* 1 reads, 122 KB */
"dll/m430Dll.rel", /* 1 reads, 120 KB */
"dll/m423Dll.rel", /* 1 reads, 118 KB */
"data/mgconst.bin", /* 1 reads, 108 KB */
"dll/m429Dll.rel", /* 1 reads, 97 KB */
"dll/modeseldll.rel", /* 1 reads, 89 KB */
"dll/m424Dll.rel", /* 1 reads, 77 KB */
"dll/m422Dll.rel", /* 1 reads, 75 KB */
"dll/m405Dll.rel", /* 1 reads, 74 KB */
"dll/m412Dll.rel", /* 1 reads, 73 KB */
"dll/m421Dll.rel", /* 1 reads, 68 KB */
"dll/m410Dll.rel", /* 1 reads, 67 KB */
"dll/m431Dll.rel", /* 1 reads, 66 KB */
"dll/m416Dll.rel", /* 1 reads, 58 KB */
"dll/m420dll.rel", /* 1 reads, 56 KB */
"dll/m407dll.rel", /* 1 reads, 38 KB */
"dll/bootDll.rel", /* 1 reads, 30 KB */
"data/effect.bin", /* 1 reads, 10 KB */
/* M40 (PLAN.md 55), by hand after the generated list: the files behind the
 * M39 soak's three cold reads (PLAN.md 54.4) -- Bowser's space and the
 * board's ending -- which the 20-turn --dvdlog run that generated the list
 * above never met (no Bowser space in it; it ended at the board's end) */
"data/bkoopa.bin", /* Bowser's space: 1,697 ms cold at frame 65,362 */
"data/bkoopasuit.bin", /* Bowser's space, the suit */
"dll/mstory3Dll.rel", /* the board's ending: 1,083 ms cold */
"data/mstory3.bin", /* the board's ending: 1,059 ms cold, 3.5 MB */
