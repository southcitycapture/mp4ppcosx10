# Eight loops the decompilation's own array lengths were deleting

*Draft of a note for the `mariopartyrd` decompilation. Nothing here has been
filed upstream — this is the working paper the decision would be made from.*

## What this is about

`-faggressive-loop-optimizations` is on by default at `-O2` in GCC. It reads an
out-of-bounds array access as proof that the iteration performing it never
happens, and then uses that to bound the loop — including, when the loop's own
exit test is redundant under that bound, by **deleting the exit test**.

Every step of that is correct C. The premise, in a decompilation, is a guess:
the array lengths in the reconstructed structs are inferred from the offsets of
the fields the decompiler could name, and a field that is really element *n* of
the array before it looks exactly like a new field. Where that guess is one or
two elements short and the code's own loop runs to the true length, GCC is
entitled to remove the only thing keeping a store inside memory.

That is not theoretical. It is the `m425dll` crash (PLAN.md §18.2): `s32
unk_3C[5]` against a `var_r29 < 6` loop cost `fn_1_E914` its bound and let it
write `-1`, `0` and `0.0f` every four bytes for 16 MB past the top of MEM1.
Metrowerks, which the tree matches against, does not do this. Modern GCC does.

The port keeps `-fno-aggressive-loop-optimizations` in `GAME_CFLAGS` as a
safety net and is not proposing that the decomp adopt it. The flag hides the
symptom; the eight cases below are the disease, and each of them is a wrong
declaration (or, once, a wrong loop) that is wrong whoever compiles it.

## How each case was established

`port/tools/ubaudit.sh` recompiles the port's source mirror with
`-Waggressive-loop-optimizations` and `-Warray-bounds`, which the build's `-w`
hides: 39 loop sites in 20 modules. `ubaudit.sh --triage` compiles each warning
file twice, with and without the optimisation, and reports every function whose
instruction count differs — 8 functions in 6 files, which is this list.
`ubaudit.sh --prove FILE` is the per-file form used below: warnings, and the
per-function counts under both settings.

Two equalities are asked for after a correction, and they say different things:

| equality | meaning |
|---|---|
| fixed-on == fixed-off | no undefined access is left for the optimisation to act on; the safety-net flag is now a no-op for this function |
| fixed-on == unfixed-off | the correction restored exactly the code the flag was restoring, and changed nothing else |

The second only holds where the patch is purely a declaration merge over
existing bytes. A patch that grows a stack array, or corrects a loop bound,
changes the program — it keeps the first equality and loses the second, and
that is the honest signal that it did.

All counts are PowerPC instructions from
`powerpc-apple-darwin8-gcc 14.2 -O2`, per function, `llvm-objdump -d`.
"before" is the state of the tree at M8; the `off` column of "before" is what
`-fno-aggressive-loop-optimizations` produced from the unfixed source.

## Summary

| # | file | function(s) | declared | actual | before (on→off) | after (on/off) | kind |
|---|---|---|---|---|---|---|---|
| 1 | `REL/m453Dll/score.c` | `fn_1_8F48`, `fn_1_940C`, `fn_1_9484` | `s16 unk_0C[4]` | `[6]` | 134→138, 26→30, 28→32 | 138/138, 30/30, 32/32 | declaration |
| 2 | `REL/m453Dll/score.c` | `fn_1_91D8` | loop `< 7` over 6 handles | `< 6` | 27→32 | 31/31 | loop bound (game's own) |
| 3 | `REL/m443Dll/main.c` | `fn_1_3370` | `s16 lbl_1_bss_10[1]` | `[2]` | 171→170 | 170/170 | declaration |
| 4 | `REL/m428Dll/map.c` | `fn_1_8F90` (site in `fn_1_5684`) | `s32 unk_0C[3]` | `[4]` | 115→112 | 115/112 † | declaration |
| 5 | `REL/m449Dll/main.c` | `fn_1_758` | `s32 unk_1C4[4]` | `[16]` | 3358→3363 | 3363/3363 | declaration |
| 6 | `REL/ztardll/main.c` | `fn_1_40E4` | `s16 sp14[4]` (stack) | `[6]` | 97→102 | 100/100 | declaration |
| 7 | `game/board/last5.c` | `ExecLast5` (inlines `InitLotteryTicket`) | loop `while(j>=0)` writing `character[-1]` | `while(j>0)` | 2554→2573 | 2553/2572 † | loop bound (game's own) |
| 8 | `REL/m425Dll/thwomp.c` | `fn_1_109EC` | — | — | 159→160 | 159/160 † | benign: no warning site |

† The file emits **no** `-Waggressive-loop-optimizations` or `-Warray-bounds`
warning after the correction, so there is no undefined access left to bound a
loop with; the residual on/off difference is the optimisation's ordinary effect
on loop analysis in code that is perfectly defined. `--triage`'s own caveat
covers this: a codegen difference is a reason to read a function, not a verdict
on it. Case 8 is the clearest example — `fn_1_109EC` differs by one
instruction in a file whose only real bug (`unk_3C[5]`) was corrected at M8 and
which has warned about nothing since.

**After all six corrections, the six files emit no loop-optimisation warning at
all**, and the four `-Warray-bounds` lines left in `m449Dll/main.c` are a
different pattern (`OMOBJ[0]` — a flexible one-element array the module indexes
to 1), not this one.

---

## 1–2. `src/REL/m453Dll/score.c` — `s16 unk_0C[4]` is `[6]`, and the destructor runs to 7

**Struct.**

```c
typedef struct M453ScoreUnkStruct {
    s32 unk_00;
    s32 unk_04;
    s16 unk_08;
    s16 unk_0A;
    s16 unk_0C[4];
    s16 unk_14;
    s16 unk_16;
    s32 unk_18;
    float unk_1C;
} M453ScoreUnkStruct; /* size = 0x20 */
```

**Evidence for `[6]`.** `0x0C + 4*2 == 0x14` and `0x0C + 5*2 == 0x16`, so
`unk_14` and `unk_16` *are* `unk_0C[4]` and `unk_0C[5]`. They are not
incidentally adjacent: `fn_1_8F48` fills them with `espEntry` handles in the
same constructor and in the same style as the first four (score.c:54–59), and
three separate loops then run `var_r30 < 6` over `unk_0C` — the constructor's
own final `espDispOff` sweep (score.c:71) and both display states
(score.c:138, 151). Six sprites: two score digits, two timer digits, and the
two labels beside them. The merge moves nothing — the array still begins at
0x0C, `unk_18` still begins at 0x18, and the struct is still 0x20.

**Evidence for the destructor's `< 6`.** `fn_1_91D8` runs `for (var_r31 = 0;
var_r31 < 7; var_r31++) espKill(var_r30->unk_0C[var_r31])`. Six handles are
created and seven are killed. The seventh reads the two bytes at 0x18 — the top
half of `s32 unk_18`, a flag `fn_1_9510` sets to 0 or 1, so on a big-endian
machine the half-word is 0, every time. `espKill(0)` is not a no-op:
`esprite.c:89` kills `esprite[0]`, which belongs to whoever created it, and
decrements that entry's animation use count. This one is the game's bug, not
the decompiler's, and it is the reason M8 left this case open — it is the one
correction here that changes what the program does.

**Patch** (`port/patches.txt`, exact-text against the decomp's file):

```
s16 unk_0C[4];\n    s16 unk_14;\n    s16 unk_16;\n   ->  s16 unk_0C[6];\n
var_r31->unk_14 = espEntry(0x530000, 2, 0); ... (the six uses, renamed to unk_0C[4] / unk_0C[5])
for (var_r31 = 0; var_r31 < 7; var_r31++) {  ->  for (var_r31 = 0; var_r31 < 6; var_r31++) {
```

**Proof.** Before: `fn_1_8F48` 134→138, `fn_1_940C` 26→30, `fn_1_9484` 28→32,
`fn_1_91D8` 27→32. After: 138/138, 30/30, 32/32 — each equal to the `off`
number, which is the m425 criterion. `fn_1_91D8` is 31/31: one instruction
below the `off` number because the loop really does run one time fewer now.
No warnings remain in the file.

**Untested.** Minigame m453 has not been played before or after.

## 3. `src/REL/m443Dll/main.c` — `lbl_1_bss_10[1]` is `[2]`

The decomp asks the question itself:

```c
s16 lbl_1_bss_10[1]; // why only 1 long?
```

The answer is at main.c:786, where a `var_r30 < 2` loop fills it with two
`espEntry` handles and gives each its own scale — one horizontal bar and one
vertical — and at main.c:920–927, which turns both on and both off by name.
A one-element bss array indexed to 1 is a write into whatever the linker put
next.

**Patch.** `s16 lbl_1_bss_10[1]; // why only 1 long?` → `s16 lbl_1_bss_10[2];`

**Proof.** `fn_1_3370` 171→170 before; 170/170 after, and all four of the
file's warnings (one `-Waggressive-loop-optimizations`, three `-Warray-bounds`)
are gone.

**Untested.** Minigame m443 has not been played.

## 4. `src/REL/m428Dll/map.c` — `s32 unk_0C[3]` is `[4]`, and `unk_18` is the fourth element

```c
typedef struct M433DllUnkStruct4 {
    Vec unk_00;
    s32 unk_0C[3];
    s8  unk_18[4];
    s8  unk_1C;
    ...
} M433DllUnkStruct4; /* size = 0x34 */
```

This is the per-face collision record the module builds from the stage's HSF
mesh. `unk_0C` holds the face's vertex indices and `unk_1C` holds how many
there are: map.c:472 sets it to 3 for a triangle and map.c:502 sets it to 4 for
a quad — and the quad case then writes `unk_0C[0..3]` (map.c:505, the warning
site). The fourth index lands at 0x18, where the decomp put `s8 unk_18[4]`, a
field **no line in the module names**. `0x0C + 4*4 == 0x1C`, so the array runs
exactly up to `unk_1C` and the struct is still 0x34.

**Patch.** `s32 unk_0C[3];\n    s8 unk_18[4];` → `s32 unk_0C[4];`

**Proof.** The warning at map.c:505 is gone. `fn_1_8F90` is 115/112 both before
and after — the patch does not change its code at all, because the bytes
written were always those bytes; what changes is that writing them is now
defined. See the † note above.

**Untested.** Minigame m428 has not been played.

## 5. `src/REL/m449Dll/main.c` — `s32 unk_1C4[4]` is `[16]`

```c
    s32  unk_1C4[4];
    char unk1D4[0x30];
    s32  unk_204;
```

`fn_1_758` runs `for (var_r21 = 0; var_r21 < 0x10; var_r21++)
var_r31->unk_1C4[var_r21]++` (main.c:1145) — sixteen counters bumped every
frame, and reset to 0 by the four-wide collision tests above it, which use them
as a per-pair debounce (`unk_1C4[...] > 0xF`). `0x1C4 + 16*4 == 0x204`, exactly
where `unk_204` begins, and `unk1D4` is the 0x30 bytes in between: nothing in
the module names it. The struct stays 0x218 and every field keeps its offset.

**Patch.** `s32 unk_1C4[4];\n    char unk1D4[0x30];` → `s32 unk_1C4[16];`

**Proof.** `fn_1_758` 3358→3363 before; 3363/3363 after — exactly the `off`
number, the m425 criterion, in a 3,363-instruction function. The
`-Waggressive-loop-optimizations` site is gone.

**Untested.** Minigame m449 has not been played.

## 6. `src/REL/ztardll/main.c` — a stack array, not a struct

The only case here the decompiler had no field layout to read a length off,
and it guessed low. `fn_1_40E4` picks the minigame list for the Bowser board's
last five turns:

```c
    s16 sp14[4];
    ...
    for (var_r31 = var_r30; var_r31 < 8; var_r31++) {
        if ((var_r31 != GWPlayerCfg[spC[0]].character) &&
            (var_r31 != GWPlayerCfg[spC[1]].character)) {
            sp14[var_r30++] = var_r31;
        }
    }
    for (var_r31 = 0; var_r31 < 0x1E; var_r31++) {
        var_r30 = frandmod(6);  var_r29 = frandmod(6);
        ... swap sp14[var_r30], sp14[var_r29] ...
    }
    for (var_r31 = 0; var_r31 < 6; var_r31++) {
        mgIndexList[var_r31] = sp14[var_r31];
    }
```

Eight characters minus the two the players hold is **six**; the shuffle draws
`frandmod(6)` for both indices; the copy reads `sp14[0..5]`. Six, in three
independent places. At `[4]` the fill runs two entries past the end and thirty
shuffle steps churn the neighbouring locals — which on this stack frame are
`spC[]`, the very array the exclusion test reads.

**Patch.** `s16 sp14[4];` → `s16 sp14[6];` (matched with the two declarations
below it, so the substitution is unique).

**Proof.** `fn_1_40E4` 97→102 before; 100/100 after. Not equal to the `off`
number, and it should not be: this patch is the one that grows a *stack* array,
so the frame layout changes with it. The equality that matters — the
optimisation no longer has an undefined access to work from — holds, and the
warning at main.c:906 is gone.

**Untested.** The Bowser board's last five turns have not been reached.

## 7. `src/game/board/last5.c` — a wrong loop in a hand-named file

The one case on this list that is not a declaration at all, which is why it is
argued rather than asserted.

```c
    j=3;
    while(j>=0) {
        s32 player_spr;
        j--;
        work->character[j] = GWPlayer[ticket_player & 0x3].character;
        player_spr = playerSprTbl[work->character[j]];
        member = j+1;
        ...
        ticket_player >>= 2;
    }
```

`j` takes 2, 1, 0 and then **-1**. `s8 character[3]` is right — three is the
number of numbers on a Last Five Turns lottery ticket, and
`UpdateLotteryTicketMatch(progress, ...)` is called from the draw loop
(last5.c:1086) with `ticket` counting 0, 1, 2 and indexes
`work->character[progress]`. So the fourth pass writes `character[-1]`, into
the `unk02`/`state` bytes ahead of it, and sets sprite group member 0 — which
is the ticket background, created twenty lines above with its own scale and
attributes. Three passes fill members 3, 2, 1; members 0, 4 and 5 are each set
exactly once elsewhere; the group is created with capacity 6. `while(j>0)` is
the loop that matches all of that.

GCC reads it the other way round: `character[-1]` is undefined, therefore the
fourth pass does not happen, therefore `j >= 0` is not the test that ends the
loop.

**Patch.** `while(j>=0) {` → `while(j>0) {`

**Proof.** `ExecLast5` (which inlines `InitLotteryTicket`) 2554→2573 before,
2553/2572 after, and the file's warning is gone — the residual difference is
in code with no undefined access left in it († above). This patch changes the
program: it drops a write that was really being made. It is here because the
write is to `character[-1]` and to a sprite slot that already has an owner, and
because no compiler flag can fix a loop that is genuinely one iteration too
long.

**Untested.** A board's last five turns, and its lottery, have not been played.

## 8. `src/REL/m425Dll/thwomp.c` — `fn_1_109EC`, on the list and not a bug

`--triage` listed `fn_1_109EC` at 159→160 alongside `fn_1_E914`, which was the
real crash. After M8's `unk_3C[6]` correction the file emits no warning of
either kind, and `fn_1_109EC` still differs by one instruction between the two
settings. That is the benign class the triage note warns about, and it is worth
keeping in the write-up as the calibration case: a one-instruction difference
in a file with nothing left to warn about is not evidence of anything.

---

## What is still owed

Every correction above is proved at the level of the generated instructions and
**none has been witnessed running**. Each lives in a minigame or a board
sequence that a single `--minigame` or board run would exercise, and
`port/docs/g4-witness.md` lists those runs in order. The two behaviour-changing
patches (§1–2's destructor and §7's loop) should be seen before-and-after
before any of this is offered upstream.
