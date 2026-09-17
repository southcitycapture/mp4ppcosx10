# Two classes of decompilation bug that only a non-Metrowerks compiler sees

*Draft of a note for the `mariopartyrd` decompilation. Nothing here has been
filed upstream — this is the working paper the decision would be made from. No
issue and no pull request has been opened.*

Two independent parts, written at different times and both still drafts:

* **Part One — eight loops the decompilation's own array lengths were
  deleting.** Struct array lengths that are one or two elements short, against
  loops that run to the true length, and what GCC's
  `-faggressive-loop-optimizations` does with the difference. Written M8b
  (2026-09-14).
* **Part Two — forty-three functions that fall off the end.** Non-`void`
  functions with no `return` on some path out, which Metrowerks gives a value
  by accident and GCC does not. Written M12b (2026-09-17).

---

# Part One — eight loops the decompilation's own array lengths were deleting

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

**Played, 2026-09-14.** m428 was dealt by the roulette on turn 3 of the
three-turn witness board on the G4 and played to its result screen with four
CPU players, on a bundle built with this correction in place
(`docs/m8c-board.log`, `docs/screenshots/mp4-minigame-m428.png`). No fault, and
the module unloaded through the ordinary path. The screenshot catches the
module a frame or two after entry -- the distance HUD's sprite column is drawn
and the 3D scene behind it is still white -- which is worth another look but is
not this correction's business either way.

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

---
---

# Part Two — forty-three functions that fall off the end

*Added 2026-09-17 (M12b). Same status as Part One: a working paper, nothing
filed upstream, no issue and no PR. The user decides whether any of it is
offered.*

## What this is about

Metrowerks leaves the last value that happened to be in `r3` where it is. A
non-`void` function that reaches its closing brace without a `return` therefore
*has* a return value under MWCC — whatever the last call, or the last piece of
address arithmetic, left in the return register — and a decompilation that
matches MWCC's output byte for byte will faithfully reproduce functions with no
`return` whose callers still read a useful number.

GCC 14 does not do that. It returns whatever happens to be in `r3` at the
epilogue, which is sometimes the same value by luck (when the last statement is
a tail call whose result is what the caller wanted) and sometimes not (when the
last statement is a tail call to something `void`, or no call at all).

The decomp builds with `GAME_WARN := -w -Wno-return-mismatch …`, so this
diagnostic has never been seen in the project. Turned back on for one pass it
reports **43 non-`void` functions with at least one path out that has no
`return`**. The list, and the command that produced it, is
`port/docs/return-audit.txt`. One detail worth keeping: **`-fsyntax-only` does
not report them.** "control reaches end of non-void function" comes out of the
CFG pass, so the audit has to be a real compile; the first attempt came back
with zero hits and was wrong.

One of the 43 was a reproduced, eleven-hour hang: `mstory3Dll/result.c`'s
`fn_1_16924`, the board-result screen that no button could answer (PLAN.md
§26.1). It is patched in `port/patches.txt`. The table below is the other 42,
read one at a time against their callers.

## How to read the severity column

| label | meaning |
| --- | --- |
| `harmless` | every caller discards the value. Nothing observable depends on it under either compiler. |
| `wrong-on-GCC-only` | a caller reads the value, but the path with no `return` either cannot be reached, or ends in a tail call that leaves the wanted value in `r3` anyway. MWCC is right by construction; GCC is right by accident, and nothing in the C says so. |
| `likely wrong on console` | a caller reads the value in a way that steers control flow or stores a handle, and the value the *successful* path leaves is arbitrary under both compilers. These are the game's own bugs, not the decompiler's. |

"What GCC leaves in `r3`" below is read off the C, not off a disassembly,
except for `fn_1_16924`, which was read out of `otool -tV` (§26.1).

## The table

`file:line` is the **closing brace**, which is what `-Wreturn-type` points at,
matching `return-audit.txt`.

| file:line | function | ret | the path with no `return` | callers, and what they do with it | severity |
| --- | --- | --- | --- | --- | --- |
| `m404Dll/main.c:1433` | `fn_1_607C` | `u16` | the `default` of `switch (pixSize)`, which has no label | `main.c:605`, `:629`, `:634` all **test** it; `:1194` passes it as an argument | `wrong-on-GCC-only` — unreachable for pixSize ∈ {4,8,16}, which is every real bitmap |
| `m408Dll/stage.c:229` | `fn_1_CE68` | `s32` | `lbl_1_bss_140` outside {0,1,2} | `m408Dll/main.c:363`, `switch (fn_1_CE68())` | `wrong-on-GCC-only` — that bss is only ever assigned 0, 1 or 2 |
| `m411Dll/main.c:529` | `fn_1_1520` | `s32` | the only path | `main.c:719`, statement | `harmless` |
| `m411Dll/main.c:608` | `fn_1_1C4C` | `s32` | the only path | `main.c:722`, statement | `harmless` |
| `m411Dll/main.c:692` | `fn_1_20C8` | `s32` | the only path | `main.c:725`, statement | `harmless` |
| `m418Dll/main.c:682` | `fn_1_2178` | `s32` | the only path | `main.c:697`, statement | `harmless` |
| `m418Dll/main.c:2347` | `fn_1_88B4` | `s32` | the only path | sequence-table slot `lbl_1_data_444[0].unk4` (`:2607`) → called `:2625` → tested `:2817` | `wrong-on-GCC-only` — ends in a tail call to `fn_1_B034`, which returns the wanted 0/1 |
| `m423Dll/main.c:4654` | `fn_1_F574` | `s32` | the only path | `main.c:3521`, statement | `harmless` |
| `m424Dll/ball.c:629` | `fn_1_4698` | implicit `int` | the only path (empty body) | no caller found in repo | `harmless` — padding to make the REL match |
| `m430Dll/player.c:1762` | `fn_1_10F24` | `s32` | the slot-found path (`return -1` covers the other) | `player.c:968`, statement | `harmless` |
| `m430Dll/player.c:1930` | `fn_1_11648` | `s32` | the slot-found path | `player.c:980`, statement | `harmless` |
| `m430Dll/player.c:2157` | `fn_1_11F90` | `s32` | the slot-found path | `player.c:1954`, statement | `harmless` |
| `m435Dll/main.c:784` | `fn_1_28E8` | `s32` | the only path | `main.c:3610`, statement | `harmless` |
| `m435Dll/main.c:2477` | `fn_1_B1F4` | `s32` | the only path | stored as `objFunc` at `main.c:2522` | `harmless` — `OMOBJFUNC` is `void (*)(OMOBJ *)` |
| `m436Dll/main.c:1566` | `fn_1_62C4` | `s32` | the only path | stored as `objFunc` at `main.c:1574`, `:1604` | `harmless` — same, called as `void` |
| `m436Dll/main.c:2804` | `fn_1_E628` | `s32` | both arms | table slot `lbl_1_data_290[6].unk04` → `:3066` → tested `:3227` | `wrong-on-GCC-only` — both arms are tail calls returning the wanted value |
| `m437Dll/main.c:2933` | `fn_1_D930` | `s32` | the fall-through when the timer test fails | table slot `lbl_1_data_27C[0].unk0C` → `fn_1_F504` → tested `:3391` | `wrong-on-GCC-only` — reached only when `fn_1_11854` just returned 0, so `r3` is 0 |
| `m439Dll/main.c:541` | `fn_1_1128` | `s32` | the only path | `main.c:870`, statement | `harmless` |
| `m439Dll/main.c:596` | `fn_1_16B0` | `s32` | the only path | `main.c:871`, statement | `harmless` |
| `m440Dll/object.c:1077` | `fn_1_ED88` | `s32` | the only path | `object.c:577`, `:580`, `:584`, all statements | `harmless` |
| `m442Dll/main.c:1670` | `fn_1_59C0` | `s32` | the loop-exit (no match) path | no caller found in repo | `harmless` |
| `m442Dll/score.c:102` | `fn_1_9520` | `s32` | the only path | `score.c:61` returns it onward; `:80`, `:83`, `:92` statements. The propagating caller's own result is discarded | `harmless` |
| `m450Dll/main.c:6253` | `fn_1_1B4C8` | `s32` | the only path | `main.c:5760`, `:6755`, statements | `harmless` — no call in the body, so `r3` is still `arg0` |
| `m455Dll/main.c:740` | `fn_1_24F0` | `s32` | the only path | `main.c:554`, statement | `harmless` — last callee returns a float in `f1`; `r3` is scratch |
| `m459dll/main.c:1683` | `fn_1_5310` | `s32` | both | `main.c:1329`, statement | `harmless` |
| `m461Dll/main.c:2080` | `fn_1_ADDC` | `s32` | the only path | `main.c:2092`, statement | `harmless` |
| `mgmodedll/mgmode.c:524` | `fn_1_1B0C` | `s32` | both the `exit:` and the normal path | `mgmode.c:383`, statement | `harmless` — ends in `while (!Hu3DMotionEndCheck(id))`, so `r3` is that nonzero |
| `mgmodedll/mgmode.c:614` | `fn_1_21C4` | `s32` | same shape | `mgmode.c:387`, statement | `harmless` |
| `mgmodedll/mgmode.c:712` | `fn_1_2940` | `s32` | same shape | `mgmode.c:391`, statement | `harmless` |
| `mgmodedll/mgmode.c:803` | `fn_1_3150` | `s32` | same shape | `mgmode.c:395`, statement | `harmless` |
| **`mstory3Dll/result.c:375`** | **`fn_1_16924`** | `s32` | **all three exits** | **`result.c:752`, `if (fn_1_16924() != 0) break;`** | **FIXED in `port/patches.txt`** — see PLAN.md §26.1 |
| `w02Dll/main.c:210` | `fn_1_774` | `s32` | both | `landEventFunc` slot (`main.c:146`), invoked `src/game/board/space.c:605`, discarded | `harmless` |
| `w02Dll/main.c:464` | `fn_1_1128` | `s32` | all three | `walkMiniEventFunc` slot (`main.c:145`) → `space.c:87` → `BoardSpaceWalkMiniEventExec` → dropped at `board/main.c:1356` | `harmless` — but see the note below |
| `w02Dll/roulette.c:100` | `fn_1_BE74` | `s32` | the only path | `HuPrcDestructorSet2` slot (`roulette.c:826`) | `harmless` — the slot type is `void (*)(void)` |
| `w02Dll/shuffleboard.c:102` | `fn_1_94AC` | `s32` | the only path | `HuPrcDestructorSet2` slot (`shuffleboard.c:92`) | `harmless` — same |
| `w05Dll/main.c:415` | `fn_1_1114` | `s32` | all three | `walkMiniEventFunc` slot (`main.c:108`), same chain as `w02Dll/main.c:464` | `harmless` |
| `board/boo.c:306` | `BoardBooStealTypeSet` | `s32` | the steal-happened path (`return 0` covers "nobody stealable") | `boo_house.c:504`, `:578`, `item.c:1771`, `:1829`, all statements | `harmless` — the `return 0` is meaningful and nobody reads it |
| `board/bowser.c:130` | `BoardBowserExec` | `s32` | both branches | `board/main.c:591`, `board/space.c:567`, statements | `harmless` — and the header declares it `void` (see below) |
| `board/char_wheel.c:106` | `BoardCharWheelInit` | `s32` | both branches | `warp.c:72`, `item.c:766`, `:985`, statements | `harmless` |
| **`board/player.c:2952`** | **`MegaPlayerPassFunc`** | `s32` | **the successful mega-squish path** | **`player.c:934`, `if (MegaPlayerPassFunc(…) == 0)`** | **`likely wrong on console`** — the success path returns junk into a control-flow test |
| **`board/player.c:3063`** | **`MegaExecJump`** | `s32` | **the successful bowser-suit jump** | **only `MegaPlayerPassFunc:2861`, which tail-returns it into the same test** | **`likely wrong on console`** — needs its own fix, not folded into the one above |
| **`chrman.c:1714`** | **`CharNpcDustSet`** | `s32` | **both paths** | **six call sites store it; `m447dll/player.c:169`, `:172` pass it to `HuPrcKill`** | **`likely wrong on console`** — a stored handle that is not a handle |
| `hsfman.c:1398` | `Hu3DLightCreateV` | `inline s16` | the only path | `hsfman.c:1415`, `:1455`, both statements | `harmless` |

Counts: **35 harmless, 5 wrong-on-GCC-only, 3 likely wrong on console, 1 fixed.**

## The three that are real

### `MegaPlayerPassFunc` — `src/game/board/player.c:2842-2952` (static)

The contract is the inverse of what the name suggests. Zero means *"I did
nothing, walk the player to `sp8` yourself"*; non-zero means *"I have already
put the player there"*. The sole caller, inside the per-step board-walk loop:

```c
/* src/game/board/player.c:934 */
if (MegaPlayerPassFunc(arg0, sp8) == 0) {
    BoardPauseDisableSet(0);
    BoardPlayerMoveTo(arg0, sp8);
    BoardPauseDisableSet(1);
}
```

Both explicit `return 0`s (`:2864` not a mega player, `:2898` nobody on the
destination space) are the "do nothing" cases and are correct. The path with no
`return` is the one that actually ran the jump arc, squished the other players,
shook the camera and left the player standing on `sp8` at `:2942` — the one
case that *must* report non-zero. It ends in `HuPrcSleep(30)`, and `HuPrcSleep`
is `void` (`include/game/process.h:51`), so GCC emits a tail branch and `r3` is
whatever that leaves.

**When the leftover is zero, the caller walks the player onto the space a
second time**: a duplicate move animation, a second `BoardPauseDisableSet`
cycle, and the squished players' work objects still live through it. When it is
non-zero the frame is correct by luck. That is why this has never been filed as
a visible bug. The fix is a `return 1;` before `:2952`.

### `MegaExecJump` — `src/game/board/player.c:2954-3063` (static)

PLAN.md §26.2 folded this into the row above; that understates it. It is the
**bowser-suit** variant of the same jump (30 coins instead of 10,
`HuAudFXPlay(809)`/`(810)`, `bowserSuitMot[3]`), it has its own `return 0` at
`:3003` for "nobody on the space" and its own fall-off for the successful jump,
and it ends in the same `void` `HuPrcSleep(30)`. `MegaPlayerPassFunc:2861` does
`return MegaExecJump(player, space);`, so its junk lands unchanged in the same
`== 0` test. **Patching only `MegaPlayerPassFunc` leaves this one broken.**

### `CharNpcDustSet` — `src/game/chrman.c:1700-1714`

§26.2 said "what the handle is meant to *be* is not obvious from the body".
It is now: **the `HUPROCESS *` from `HuPrcChildCreate`.** Three independent
pieces of evidence.

1. The body produces exactly one candidate — `HUPROCESS *process =
   HuPrcChildCreate(UpdateNpcDust, 0x64, 0x2000, 0, parent);` at `chrman.c:1703`.
   `EffectInit()` at `:1712` is `static void` (`chrman.c:683`), and the
   `HuMemDirectMallocNum` result is already stored into `process->user_data`.
2. `src/REL/m447dll/player.c:149-150` casts the return **explicitly**:
   ```c
   temp_r3->unkB0 = (HUPROCESS *)CharNpcDustSet(temp_r3->unk68, temp_r3->unk6A[2], 1, 10);
   ```
3. `m447dll/player.c:168-172` is the matching teardown — `if (arg0->unkB0) {
   HuPrcKill(arg0->unkB0); }` — with both fields pre-set to `NULL` at `:138-139`
   and written only from `CharNpcDustSet`. A create/kill pair around a child
   process is unambiguous.

§26.2 also undercounted the storers. **Six** call sites store the value:
`m447dll/player.c:149`, `:150` (as `HUPROCESS *`, and read back by `HuPrcKill`);
`m459dll/main.c:635`, `:636`; `present/common.c:61`, `:62`;
`option/guide.c:72`, `:73`; `m448Dll/main.c:1759`, `:1760`. About eighteen
others discard it.

GCC emits `b EffectInit` for the tail call, so on the `process != NULL` path
`r3` is whatever `EffectInit` left — in practice non-`NULL` and not a process.
`m447dll` stores it, the `if (arg0->unkB0)` guard passes, and `HuPrcKill()` is
handed a bogus `HUPROCESS *` when the m447 player object is torn down. That is
a wild write into the process list, not a cosmetic miss. The `process == NULL`
path is accidentally *correct* — nothing clobbers `r3` after a NULL return — which
is why the guard has been doing its job for the other five storers.

The fix is `return (s32)process;` before `:1714`, which also makes the NULL
path explicit rather than accidental. The crash window is m447's object
teardown, so no title-screen or board soak will ever show it.

## Two structural notes

* **`BoardBowserExec` has a prototype/definition mismatch.**
  `include/game/board/bowser.h:6` declares `void BoardBowserExec(s32, s32);`
  while `src/game/board/bowser.c:107` defines it `s32`. Both callers compile
  against the `void` prototype, so the value is guaranteed-discarded. The audit
  entry can never become live without a header change — but the mismatch itself
  is worth fixing upstream on its own terms.
* **`walkMiniEventFunc` is harmless; `walkEventFunc` would not be.**
  `w02Dll/main.c:464` and `w05Dll/main.c:415` are `walkMiniEventFunc`
  implementations, whose value travels faithfully through
  `BoardSpaceWalkMiniEventExec` (`src/game/board/space.c:82-93`) and is then
  dropped by its single caller at `src/game/board/main.c:1356`. The sibling
  `BoardSpaceWalkEventExec` **is** tested — `src/game/board/player.c:945`,
  `if (BoardSpaceWalkEventExec() != 0) continue;`. None of the 43 is a
  `walkEventFunc`, but any future one would be live.

## What is still owed here

Nothing in Part Two has been witnessed on hardware; the lab was unreachable the
day it was written (PLAN.md §26.3). All three real bugs are game-behaviour
changes, so under the project's own rule they go through `port/patches.txt` and
get a G4 witness before they ship:

* `MegaPlayerPassFunc` / `MegaExecJump` — a board where a Mega player passes
  another player's space, once in the plain form and once in a Bowser suit.
* `CharNpcDustSet` — `--minigame m447`, played to its exit, which is where the
  bogus `HuPrcKill` would land.

Until then this is a reading, not a result.
