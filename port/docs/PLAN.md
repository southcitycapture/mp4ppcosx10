# Mario Party 4: native port to the Power Mac G4

Third in the line after Snowboard Kids 1 and 2. Same shape as those two ports:
the decompiled game sources are never edited, a `port/` directory replaces the
console SDK, a host loop plays the part of the interrupt controller, and a
self-play harness proves the thing works by beating the game.

Everything below is measured from the decomp at `/Users/zachjack/Apps/marioparty4`
(mariopartyrd/marioparty4, `main` at `147b165a`, USA Rev 1 = `GMPE01_01`, 100%
matching). Numbers come from `port/tools/inventory.py`; its output is committed
as [`port/docs/inventory.md`](inventory.md) and can be regenerated at any time
with `python3 port/tools/inventory.py`.

Target: **Power Mac G4 Quicksilver 1 GHz, ATI Radeon 9000, Mac OS X 10.5.**
OpenGL 1.3, 6 texture units, `ARB_texture_env_combine` + `ATI_texture_env_combine3`
+ crossbar, `ARB_multisample`, `EXT_fog_coord`, `ATI_text_fragment_shader`.
No `ARB_fragment_program`, no FBO, no NPOT. 32-bit PowerPC 7450: AltiVec, and
crucially **no paired singles**. Built with `ghcr.io/variantxyz/gcc-powerpc-apple-darwin8`
(GCC 14.2 + MacOSX10.4u SDK) against a Tiger-targeted static SDL2.

---

## 0. The one-paragraph version

Mario Party 4 turns out to be a much friendlier port target than either
Snowboard Kids. It is **single-threaded** (one `OSCreateThread` in the entire
game, and it is the soft-reset watcher), it uses **zero paired-single
intrinsics in game code** (all 1,063 of them live in the SDK's MTX/THP/OS,
which we replace or switch to the pure-C path that is already in the tree), it
addresses main memory with **native pointers and ARAM with plain offsets** so
there is no fixed-address RDRAM to emulate and **no pinned-globals scheme** is
needed, and its GX usage is far simpler than the hardware allows: 93 of 121
`GXSetNumTevStages` sites ask for **one** stage, the highest TEV stage the game
ever names is `GX_TEVSTAGE4`, and it never uses per-vertex matrix indices. The
whole thing plausibly fits the Radeon 9000's six fixed-function texture units in
one pass for the overwhelming majority of draws. The hard parts are elsewhere:
**99 relocatable modules** with 899 colliding symbol names between them (solved
with one Mach-O bundle per REL behind `dlopen`, which maps 1:1 onto the game's
own `OSLink`/`OSUnlink` loader), **MusyX audio** whose mixing actually happens
on the DSP (so we need a DSP-command-list interpreter or a CPU synth), and
**THP movies**. Estimated ordering below.

---

## 1. Inventory of the game

### 1.1 Shape and size

| area | files (.c/.h) | lines |
|---|---:|---:|
| `src/game` (DOL, engine + system) | 50 | 35,165 |
| `src/game/board` (DOL, the board game) | 60 | 68,204 |
| `src/msm` (DOL, Hudson's sound manager over MusyX) | 6 | 3,201 |
| `src/libhu` | 2 | 14 |
| `src/REL/*` (96 module directories) | 256 | 288,048 |
| `src/dolphin` (the decompiled SDK, in-tree) | 88 | 30,796 |
| `extern/musyx` (AxioDL MusyX, MIT) | 35 | 21,015 |
| `src/{MSL_C,Runtime,TRK_MINNOW_DOLPHIN,OdemuExi2,amcstubs,odenotstub}` | 78 | 11,616 |
| **total** | **575** | **458,059** |

`build/GMPE01_01/main.dol` is 1,319,008 bytes. `orig/GMPE01_01/files/dll/`
holds **99 `.rel` files**, the smallest 572 bytes (`_minigameDll.rel`, a pure
stub) and `E3setupDLL.rel` 36,376 bytes.

**The SDK is in the repo.** `src/dolphin/` contains decompiled `os/`, `gx/`,
`dvd/`, `card/`, `pad/`, `si/`, `exi/`, `ai.c`, `dsp/`, `ar/`, `mtx/`, `vi.c`,
`thp/`, `demo/`, `db.c`, `PPCArch.c` — 88 files, 30,796 lines. `configure.py`
compiles these from source into the matching DOL rather than linking library
objects off the disc, which means **the port has the real source of every SDK
function it needs to replace**, and can compile the pure-software parts as-is.

### 1.2 What the DOL contains

`src/game/main.c`'s `main()` is the whole program:

```
HuSysInit(&GXNtsc480IntDf)   OSInit / DVDInit / VIInit / PADInit, render mode,
                             arena + heap, GXInit on a 1 MB FIFO, HuMemInitAll,
                             HuAudInit, HuARInit, OSInitFastCast, HuCardInit
HuPrcInit / HuPadInit / GWInit / pfInit / HuSprInit / Hu3DInit / HuDataInit
omMasterInit(0, _ovltbl, DLL_MAX, DLL_bootdll)     -> hand off to bootDll
while (1) { ... }                                  -> the frame loop
```

The DOL is the *engine*: the HSF model/animation system (`hsfdraw.c` 3,213
lines, `hsfanim.c`, `hsfman.c`, `hsfmotion.c`, `hsfload.c`, `hsfex.c`), sprites
(`sprman.c`, `sprput.c`, `esprite.c`), the process scheduler (`process.c`), the
object manager (`objmain.c`, `objsub.c`, `objsysobj.c`, `objdll.c`), memory
(`malloc.c`, `memory.c`, `armem.c`), data/DVD (`data.c`, `dvd.c`, `decode.c`),
audio glue (`audio.c` + `src/msm/*`), font/window/message
(`font.c`, `window.c`, `messdata.c`, `printfunc.c`), saves (`card.c`,
`saveload.c`), input (`pad.c`), movies (`thpmain.c`, `THPDraw.c`, `THPSimple.c`),
fault/reset (`fault.c`, `sreset.c`), and — unusually — **the entire board game**
(`src/game/board/`, 60 files, 68,204 lines: spaces, items, stars, Bowser, Boo,
the shop, the lottery, battle, Last 5 Turns, the CPU path-finder in
`com.c`/`com_path.c`). The board is *not* a REL.

### 1.3 What the RELs contain

99 modules on the disc; `include/ovl_table.h` lists 100 `DLL()` entries per
version (`DLL_MAX`), turned into a `FileListEntry _ovltbl[]` of
`{"dll/<name>.rel", 0}` by `src/game/ovllist.c`. Groups:

| group | modules | what |
|---|---|---|
| boot / setup | `bootDll`, `instDll`, `E3setupDLL`, `msetupDll`, `safDll` | logos, the instruction/attract loop, the E3 demo build's setup, the "save file" boot path |
| boards | `w01Dll` … `w06Dll` (Toad's Midway Madness → Bowser's Gnarly Party), `w10Dll`, `w20Dll`, `w21Dll` | one module per board plus three extras (`w10` is the Extra-room board, `w20`/`w21` the tutorial/practice boards) |
| minigames | `m300Dll`, `m302`, `m303`, `m330`, `m333`, `m401Dll` … `m463Dll` (with `_minigameDll` as the shared stub) | ~65 modules, one per minigame or minigame family |
| story | `mstoryDll`, `mstory2Dll`, `mstory3Dll`, `mstory4Dll`, `nisDll` | Story Mode's flow and cut-scenes |
| menus / front end | `mentDll`, `modeseldll`, `mgmodedll`, `selmenuDll`, `subchrselDll`, `option`, `present`, `messDll`, `mpexDll`, `resultDll`, `staffDll`, `modeltestDll`, `ztardll` | the entrance, mode select, the Minigame mode shell, character select, options, the Present/Extra room, results, credits, a model viewer, the Star Bank |

Four modules (`m302Dll`, `m303Dll`, `m330Dll`, `m333Dll`) share the same SHA-1
as each other in `config.yml` — they are identical placeholder RELs.

The largest are `mgmodedll` (8,107 lines), `m450Dll` (7,739), `mpexDll` (7,715),
`mentDll` (7,606), `m427Dll` (6,535).

**Only three modules link against another module**: three `mstory*` entries in
`config/GMPE01_01/config.yml` declare `links: [_minigameDll]`. Every other REL
imports only from the DOL. That is a nearly-flat dependency graph.

### 1.4 How RELs are loaded

`src/game/objdll.c`, 176 lines, is the entire loader:

```c
dll->module = HuDvdDataReadDirect(dllFile->name, HEAP_HEAP);   // read the .rel
dll->bss    = HuMemDirectMalloc(HEAP_HEAP, dll->module->bssSize);
if (OSLink(&dll->module->info, dll->bss) != TRUE) { ... }      // relocate
dll->ret = ((DLLProlog)dll->module->prolog)();                 // enter
```

and on the way out `((DLLEpilog)dll->module->epilog)()` then `OSUnlink()` then
three `HuMemDirectFree`s. `omDLLStart(overlay, flag)` with `flag == 0` will
*re-enter an already-loaded module*: it `memset`s the module's bss to zero,
`HuMemDCFlushAll()`s, and calls the prolog again. So the loader's contract is:

1. resolve a name to a relocatable image,
2. give it a fresh, zeroed bss,
3. call `_prolog`, later `_epilog`,
4. unload it and reclaim the memory,
5. and be able to re-enter a module that stayed resident, with bss re-zeroed.

There are at most `OM_DLL_MAX` modules live at once. This contract matters a
great deal in §2.4.

### 1.5 Memory map

- **MEM1**: 24 MB retail (`OSGetConsoleSimulatedMemSize()` = 0x01800000). The
  game asks for `OSGetPhysicalMemSize()` and `OSGetConsoleType()` and only takes
  the "more than 24 MB" development path (`LoadMemInfo`, which reads
  `/meminfo.bin` off the disc) when it is running on `OS_CONSOLE_DEVHW1`. On
  retail it just makes one heap from `OSInitAlloc(arena_lo, arena_hi, 1)` +
  `OSCreateHeap`.
- **Two external framebuffers** are carved off `arena_lo` first
  (`InitMem()` in `init.c`): `fbWidth` rounded to 16, times `xfbHeight`, times
  2 bytes — 640×480×2 = 614,400 bytes each for NTSC.
- **GX FIFO**: a single 1 MB buffer, `OSAlloc(0x100000)` then
  `GXInit(DefaultFifo, 0x100000)`.
- **Game heaps** sit on top, in `src/game/memory.c` / `malloc.c`: named ids
  (`HEAP_HEAP`, `HEAP_DVD`, `HEAP_SOUND`, `HEAP_MUSIC`, …), each a `HuMemHeap`
  with 32-byte-aligned allocations tagged with the caller's return address
  (`mflr` inline asm in `HuMemDirectMalloc`). The MusyX heap is a fixed
  `0x13FC00` = 1,309,696 bytes.
- **ARAM**: 16 MB. `src/game/armem.c` runs a 64-block first-fit allocator over
  ARAM *above* `HU_AMEM_BASE = 0x808000` (8,421,376), leaving everything below
  it to MusyX (`msmAram.aramEnd = HU_AMEM_BASE`, `skipARInit = TRUE`). So MusyX
  owns ~8.03 MB of sample data and the game gets ~8.39 MB for streamed model,
  texture and animation data, moved in and out with `ARQPostRequest` DMA.
  **ARAM is addressed by `AMEM_PTR`, a `u32` offset — never by pointer.** That
  is what makes ARAM trivial to host: one 16 MB `malloc`, index by offset,
  `memcpy` for DMA.

### 1.6 The frame loop

`src/game/main.c`:

```c
while (1) {
    retrace = VIGetRetraceCount();
    if (HuSoftResetButtonCheck() || HuDvdErrWait) continue;
    HuSysBeforeRender();            /* GXSetViewport(Jitter), GXInvalidateVtxCache,
                                       GXInvalidateTexAll */
    GXSetGPMetric(...); GXClearGPMetric(); ...  /* performance counters */
    Hu3DPreProc();
    HuPadRead();
    pfClsScr();
    HuPrcCall(1);                   /* run every HUPROCESS for one tick */
    MGSeqMain();
    Hu3DExec();                     /* the actual drawing */
    HuDvdErrorWatch();
    WipeExecAlways();
    pfDrawFonts();
    msmMusFdoutEnd();
    HuSysDoneRender(retrace);
    GXReadGPMetric(...); GXReadVCacheMetric(...); GXReadPixMetric(...); GXReadMemMetric(...);
    GlobalCounter++;
}
```

and `HuSysDoneRender` (in `init.c`) is where pacing lives:

```c
GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE); GXSetColorUpdate(GX_TRUE);
GXDrawDone();
GXCopyDisp(DemoCurrentBuffer, GX_TRUE);
if (minimumVcount) {
    retrace_dist = VIGetRetraceCount() - retrace_count;
    if (worstVcount < retrace_dist) worstVcount = retrace_dist;
    while (VIGetRetraceCount() - retrace_count < minimumVcount - 1) VIWaitForRetrace();
}
SwapBuffers();   /* VISetNextFrameBuffer, VIFlush, VIWaitForRetrace, flip */
```

`minimumVcount` defaults to 1 and is set by `HuSysVWaitSet()` — the game's own
frame-rate governor, i.e. some scenes deliberately run at 30 Hz. The loop
blocks in `VIWaitForRetrace()` exactly twice per frame (once in the pacing
loop when it is behind, once at the end of `SwapBuffers`). **That is our idle
gate**, and it is much cleaner than the N64 ports' heuristic: we do not have to
guess when the game has finished a frame, the game tells us by calling
`VIWaitForRetrace`.

### 1.7 Threads

**One.** `grep OSCreateThread src --include='*.c'` outside `src/dolphin` returns
exactly one hit:

```
src/game/sreset.c:182: OSCreateThread(&ToeThread, ToeThreadFunc, NULL,
                                      &ToeThreadStack[4096], 4096, 8, OS_THREAD_ATTR_DETACH);
```

the soft-reset ("power/reset button") watcher. Everything else in the game runs
on the main thread. `OSSleepThread`/`OSWakeupThread`/`OSResumeThread`/
`OSYieldThread` appear once each, all in `sreset.c`. The SDK internally creates
threads for DVD and audio callbacks; those go away with the SDK.

What the game *does* have instead is its own cooperative scheduler,
`src/game/process.c` + `src/game/jmp.c`: **`HUPROCESS`, stackful coroutines
built on hand-written PowerPC `gcsetjmp`/`gclongjmp`** that save/restore
`lr, cr, sp, r2, r13-r31, f14-f31, fpscr`, with `HuPrcCreate` faking a stack by
setting `jump.lr = func` and `jump.sp = base_sp`. `HuPrcCall(1)` from the frame
loop runs each process in priority order until it calls `HuPrcSleep`/
`HuPrcVSleep`, which `gclongjmp`s back. This is *the* game logic driver.

This is both the largest hazard and the largest gift of the port. See §2.2.

### 1.8 Audio

Two layers above the SDK:

- **`src/msm/`** (6 files, 3,201 lines, 100 distinct `msm*` entry points,
  436 call sites): Hudson's own sound manager — group/bank loading from
  `/sound/mpgcsnd.msm`, sequences, SFX, 3-D emitters, and streamed audio from
  `/sound/mpgcstr.pdt`. Pure C over MusyX; **portable as-is**.
- **`extern/musyx`** (AxioDL's MusyX reimplementation, **MIT licence**, 35 files,
  21,015 lines): `snd*` API (52 symbols, 105 call sites from `src/msm`),
  the sequencer (`seq.c` 1,384), the 3-D positional model (`snd3d.c` 1,567),
  the voice/ADSR model (`synth*.c` ≈ 5,000 lines), stream handling
  (`stream.c` 884), and the hardware layer.

**The DSP question.** MusyX on GameCube does *not* mix on the CPU. The CPU runs
the sequencer, the voice state machines and the parameter math; the actual
sample fetch, ADPCM decode, resample and mix happen on the **DSP**, driven by a
command list that `hw_dspctrl.c` (2,147 lines) builds each frame and hands to
`dspSlave` — the ucode blob embedded in `dsp_import.c` (1,885 lines of hex).
`hw_dolphin.c` (212 lines) is the Dolphin SAL: `AIInitDMA`, `DSPInit`,
`ARAM` DMA. There is a `hw_pc.c` (163 lines) PC SAL in the tree but it is a
skeleton — `salCallback` has its `AIInitDMA` commented out and there is no
software synth behind it.

So the port must supply one of:

1. **A CPU implementation of the `dspSlave` command set.** The command list is
   MusyX's own, fully visible in `hw_dspctrl.c` (studios, voices with
   ADPCM/PCM8/PCM16 sources, per-voice volume ramps into up to 8 studio
   buses, the AUX A/B send/return points that `snd_service.c` and the
   `StdReverb`/`CheapReverb`/`Chorus`/`Delay` directories already implement on
   the CPU). This is the direct analogue of the N64 ports' `audio_task.c`
   `aspMain` interpreter and is the recommended route: it reuses everything
   MusyX already does on the CPU, and only replaces the leaf mixer. Estimated
   1,000–1,500 lines.
2. **A DSP ucode interpreter.** Faithful but far more work, and it needs the
   GameCube DSP ISA. Rejected.
3. **Stub audio.** What the upstream `port` branch does today: `HuAudInit` has
   `msmSysInit` commented out and every `msm*`/`snd*` call short-circuits.
   Fine for M1–M5, not shippable.

Cost note: 1 GHz G4, 32 kHz stereo, MusyX's typical 64 voices — ADPCM decode
plus a 4-tap resample plus an 8-bus mix is roughly the same budget the SBK ports
paid for `aspMain` at 22 kHz, and they spent ~0.8 ms/frame. AltiVec is available
if it is tight. Not a blocker.

### 1.9 The file system

`DVDConvertPathToEntrynum` is called once per entry at boot by
`HuDataInit()` over `DataDirStat[]`, generated from
`include/datadir_table.h`: **138 `data/*.bin` files**, plus `dll/*.rel` (99)
and `sound/mpgcsnd.msm`, `sound/mpgcstr.pdt`, plus `sound/MPNGC02.son` /
`MPNGC16.son`. A missing file is an `OSPanic` at `data.c:65`, so the port must
present a complete FST.

Access is by entry number after boot: `HuDvdDataFastRead(entrynum)`,
`HuDvdDataFastReadAsync`, and `HuDvdDataReadDirect(path, heap)` for the RELs.
All reads are whole-file into a `HuMemDirectMalloc`'d, 32-byte-rounded buffer,
issued as `DVDReadAsync` and then spun on with `HuDvdErrorWatch()` until the
callback fires. `DVDGetDriveStatus()` drives the "no disc / cover open / wrong
disc" error screen.

`DVD` surface actually used: 12 symbols, 62 sites —
`DVDOpen`, `DVDClose`, `DVDFastOpen`, `DVDConvertPathToEntrynum`,
`DVDReadAsync`, `DVDRead`, `DVDGetDriveStatus`, `DVDInit`, plus the stream
(`DVDPrepareStreamAsync`, `DVDStopStreamAtEndAsync`, …) used by `msmstream.c`
for the ADPCM music stream.

### 1.10 Saves

`src/game/card.c` (186 lines) + `saveload.c`. `CARDInit`, a
`CARD_WORKAREA_SIZE` work buffer, `CARDProbeEx`/`CARDMount`/`CARDCheck` on both
slots, then `CARDOpen`/`CARDRead`/`CARDWrite`/`CARDCreate`/`CARDDelete`/
`CARDGetStatus`/`CARDSetStatus`/`CARDFormat`. 23 distinct `CARD*` symbols, 37
call sites — all in the DOL, none in any REL. A memory card file is a flat byte
array with a banner/icon in its status block; one host file per slot reproduces
it exactly.

### 1.11 Input

Tiny: **9 `PAD*` symbols, 18 call sites.** `PADInit`, `PADSetSpec`, `PADReset`,
`PADRecalibrate`, `PADRead`, `PADClamp`, `PADButtonDown`, `PADButtonUp`, and
`PADControlMotor` (7 sites — rumble). `src/game/pad.c` (313 lines) wraps
`PADRead` into the game's own `HuPadStatus`/`HuPadBtn*` arrays with repeat and
edge detection, and owns the rumble timers. One `SISetSamplingRate` call.

This is the smallest console interface in the whole port, and the SBK ports'
`input_sdl.c` + `input_xone.c` (the IOKit Xbox One driver) can be lifted with
only the button map changed.

### 1.12 Gekko paired singles

**Zero in game code.** `port/tools/inventory.py`'s hazard pass finds 1,063
paired-single intrinsic sites, distributed as:

| file | sites |
|---|---:|
| `src/dolphin/thp/THPDec.c` | 352 |
| `src/dolphin/mtx/mtx.c` | 253 |
| `src/dolphin/mtx/psmtx.c` | 127 |
| `src/dolphin/mtx/mtxvec.c` | 70 |
| `src/dolphin/os/OSContext.c` | 64 |
| `src/dolphin/mtx/vec.c` | 62 |
| `src/dolphin/mtx/quat.c` | 40 |
| `src/dolphin/gx/GXTransform.c` | 32 |
| `src/dolphin/os/OS.c` | 32 |
| `src/dolphin/mtx/mtx44.c` | 16 |
| `extern/musyx/.../StdReverb/reverb.c` | 8 |
| `extern/musyx/.../snd_service.c` | 6 |
| `src/dolphin/gx/GXLight.c` | 1 |

**`src/game` and `src/REL` contribute none.** Every one is inside a module we
either replace outright (OS, GX, THP) or can switch to a pure-C build (MTX).

And MTX already has the switch. `include/dolphin/mtx.h`:

```c
#define GEKKO
#ifndef GEKKO
#define MTX_USE_C
#undef MTX_USE_PS
#endif
#if (!defined(MTX_USE_PS) && !defined(MTX_USE_C))
#ifndef _DEBUG
#define MTX_USE_PS
#endif
#endif
```

so **`-DMTX_USE_C` on the command line is enough** — no header patch needed —
and every `MTXConcat`/`VECAdd`/… macro then resolves to the `C_MTX*`/`C_VEC*`
functions that are already in `src/dolphin/mtx/mtx.c`, `vec.c`, `quat.c`,
`mtx44.c`. The game side also calls `PSMTX*`/`PSVEC*` **directly** at 219 sites
(`PSMTXConcat` 67, `PSMTXRotRad` 40, `PSMTXMultVec` 21, `PSMTXReorder` 19,
`PSMTXIdentity` 17, `PSMTXInvXpose` 14, …), so the port also needs a
`port/src/os/psmtx_c.c`. Most of it is thin `PSMTX*` → `C_MTX*` forwarders, but
**two functions have no C equivalent anywhere in any SDK decompilation** and
must be written by hand: `PSMTXReorder` (19 sites) and `PSMTXROMultVecArray`
(18 sites), the reordered-matrix skinning fast paths from `psmtx.c` (§8.2).
That is about 60 lines of scalar C. 74 distinct MTX symbols are called from the
game side across **4,172 call sites**, easily the hottest SDK family after GX,
so this file is the first place to reach for AltiVec if `--perf` says so.

### 1.13 Other cross-build hazards

| hazard | game-side sites | where |
|---|---:|---|
| `asm { }` block | 5 | `src/game/jmp.c` (1 — `gcsetjmp`), `src/game/malloc.c` (4 — `mflr retaddr` for allocation tagging) |
| `asm <type> fn()` whole-function asm | 0 game-side | all in `src/dolphin`, `Runtime.PPCEABI.H`, `TRK_MINNOW_DOLPHIN` |
| `__declspec` | 2 | `src/game/hsfmotion.c` — `__declspec(weak)` on `GetObjTRXPtr` and `GetBezier` |
| `#pragma` | 6 | `#pragma dont_inline on/reset` in `m426Dll/main.c`, `m446Dll/card.c`, `m413Dll/main.c` |
| `register` storage class | ~90 game-side | harmless to GCC 14 in C, warns only |
| `OSf32tos8/16/32` fast-cast | 65 | 22 board/REL files; replace with plain casts (`OSInitFastCast` becomes a no-op) |
| `ATTRIBUTE_ALIGN` | 2 game-side | `src/game/window.c`; already a macro, redefine to `__attribute__((aligned(n)))` |
| `OSCachedToUncached` | 1 | `src/REL/m415Dll/main.c:429`; make it identity |
| absolute `0x8xxxxxxx` addresses | 2 real | `src/game/init.c:271-272`, inside the dev-hardware-only `LoadMemInfo` path. The other 30 hits are sign-bit masks. |
| `__attribute__` | 0 | — |
| GCC-style `asm volatile` | 0 | — |

That is a remarkably short list for 391,000 lines of game code. Compare the
Snowboard Kids ports, which needed a 156-line `patches.txt` and a 1,463-symbol
pinning scheme.

### 1.14 The GX call inventory

**128 distinct `GX*` symbols, 5,679 call sites from game code** (1,128 in
`src/game`, 236 in `src/game/board`, 4,315 across the RELs). The full
per-symbol table is in [`inventory.md` §3.1](inventory.md). Top of the list:

| symbol | sites | | symbol | sites |
|---|---:|---|---|---:|
| `GXSetVtxAttrFmt` | 260 | | `GXSetChanCtrl` | 140 |
| `GXSetVtxDesc` | 256 | | `GXColor1x16` | 135 |
| `GXSetTevColorIn` | 232 | | `GXSetBlendMode` | 120 |
| `GXSetTevOrder` | 212 | | `GXSetTexCoordGen2` | 118 |
| `GXPosition3f32` | 208 | | `GXSetNumTevStages` | 118 |
| `GXSetTevAlphaIn` | 201 | | `GXLoadPosMtxImm` | 117 |
| `GXPosition1x16` | 199 | | `GXClearVtxDesc` | 115 |
| `GXSetTevColorOp` | 191 | | `GXSetNumTexGens` | 114 |
| `GXSetTevAlphaOp` | 183 | | `GXTexCoord2f32` | 112 |
| `GXSetArray` | 159 | | `GXSetNumChans` | 105 |
| `GXTexCoord1x16` | 153 | | `GXLoadTexMtxImm` | 93 |
| `GXSetZMode` | 143 | | `GXSetTevColor` | 73 |
| `GXBegin` | 142 | | `GXInitTexObj` | 68 |

#### TEV complexity — the headline result

| measure | value |
|---|---|
| `GXSetNumTevStages` argument, 93 of 121 literal sites | **1** |
| … 14 sites | 2 |
| … 2 sites each | 3, 5 |
| … 8 sites | a variable (`tevStage`, `temp_r31`, a cast) — the HSF material path |
| highest `GX_TEVSTAGEn` ever named | **`GX_TEVSTAGE4`** (10 sites) — i.e. never more than 5 stages |
| `GX_TEVSTAGE0` / 1 / 2 / 3 / 4 sites | 612 / 114 / 24 / 15 / 10 |
| highest `GX_TEXMAPn` | `GX_TEXMAP3` (6 sites) — never more than 4 simultaneous textures |
| highest `GX_TEXCOORDn` | `GX_TEXCOORD4` (2 sites) |
| `GXSetNumTexGens` | 0 (35), 1 (53), 2 (12), 3 (5), 4 (2), 5 (1) |
| `GXSetNumChans` | 0 (9), 1 (93), 2 (4) |
| TEV konst select (`GXSetTevKColorSel`/`KAlphaSel`) | 17 |
| TEV swap (`GXSetTevSwapMode`/`Table`) | 28 |

**Indirect texturing is used, but narrowly.** `GXSetNumIndStages`: 0 at 15
sites, 1 at 11, 2 at 3, 3 at 1. `GXSetIndTexOrder` 22, `GXSetIndTexMtx` 21,
`GXSetIndTexCoordScale` 19, `GXSetTevIndWarp` 23, `GXSetTevInd(Tile|BumpST)` 2,
`GXSetTevDirect` 29 (turning it back off). Only `GXSetTevIndWarp` is used — the
simplest indirect mode, where an indirect texture's texel perturbs a regular
texcoord through a 2×3 matrix. There is no `GXSetTevIndirect` general form
anywhere. And `GX_VA_NBT` (binormal/tangent) appears at just 6 sites — the
bump-mapped surfaces are a handful of effects, not the art pipeline.

**Lighting**: `GXSetChanCtrl` 143, `GXInitLight*` 39 (`GXInitLightAttn` 13,
plus pos/dir/color/spot/distattn), `GXLoadLightObjImm` only 4. At most 2 colour
channels. This is per-vertex lighting with a small number of lights, which GL's
fixed-function `GL_LIGHTn` maps onto directly.

**Fog**: 9 sites total. **Z-textures**: 2. **Dither**: 0. **Dst-alpha**: 0.

**EFB copies**: `GXCopyTex` 28, `GXSetTexCopySrc`/`Dst` 66, `GXGetTexBufferSize`
22, plus `GXSetCopyClear` 13 and `GXCopyDisp` 2. Copy formats are only
`GX_CTF_R8` (6) and `GX_CTF_A8` (1), and depth as `GX_TF_Z24X8` (3) / `GX_TF_Z8`
(1) — so the EFB-copy uses are shadow/stencil-ish masks and a depth read, not
full-colour render-to-texture chains.

**Display lists**: `GXBeginDisplayList` 38, `GXEndDisplayList` 38,
`GXCallDisplayList` 42. Every one is **built at runtime by the game**
(`hsfdraw.c:2479` builds into `DLBufP` with a 0x20000 cap and stores
`DrawData[].dlOfs`/`dlSize`; `hsfanim.c:515` does the same for particles; nine
RELs build their own). **No display list is baked into disc data.** And what
goes *inside* one is only geometry: between `GXBeginDisplayList` and
`GXEndDisplayList` in `hsfdraw.c` the only GX calls are `GXResetWriteGatherPipe`
and 11 each of `GXPosition1x16`, `GXNormal1x16`, `GXColor1x16`, `GXTexCoord1x16`
— indexed 16-bit attributes — wrapped in `GXBegin`/`GXEnd`. All state is set
*outside* the list, immediately before `GXCallDisplayList`. That is a large
simplification; see §3.3.

**Vertex descriptors**: `GX_INDEX16` at 136 sites, `GX_DIRECT` at 110,
`GX_INDEX8` at 14. Attributes: `GX_VA_POS` 290, `GX_VA_CLR0` 163, `GX_VA_TEX0`
151, `GX_VA_NRM` 58, `GX_VA_TEX1` 12, `GX_VA_NBT` 6. Component formats:
`GX_F32` 145, `GX_RGBA8` 53, `GX_RGBA6` 32, `GX_U16` 9, `GX_S16` 6, `GX_U8` 6,
`GX_S8` 4. **`GX_VA_PNMTXIDX` and `GX_VA_TEXnMTXIDX`: zero sites.** The game
never uses per-vertex matrix indices — every draw uses the single loaded
position/normal matrix. That deletes an entire class of work.

**Primitives**: `GX_QUADS` 92 (!), `GX_TRIANGLESTRIP` 19, `GX_LINES` 13,
`GX_LINESTRIP` 13, `GX_TRIANGLEFAN` 5, `GX_TRIANGLES` 4. GL 1.3 has `GL_QUADS`
natively.

**Texture formats**: `GX_TF_RGB5A3` 36, `GX_TF_RGB565` 35, `GX_TF_RGBA8` 12,
`GX_TF_I8` 9, `GX_TF_C4` 8, `GX_TF_C8` 8, `GX_TF_CMPR` 6, `GX_TF_I4` 3,
`GX_TF_IA4` 2, `GX_TF_IA8` 2, plus `GX_TF_Z24X8` 3 / `GX_TF_Z8` 1 for the depth
copies. TLUTs: `GX_TL_RGB5A3` 8, `GX_TL_RGB565` 6, `GX_TL_IA8` 2. Ten formats
to decode; all of them are 4×4 (or 8×4/8×8) tiled and need a de-swizzle pass.

**Blend factors**: `GX_BL_SRCALPHA` 92, `GX_BL_INVSRCALPHA` 64, `GX_BL_ONE` 41,
`GX_BL_ZERO` 34, `GX_BL_INVDSTCLR` 18, `GX_BL_INVSRCCLR` 1. All are plain
`glBlendFunc` arguments.

### 1.15 The rest of the SDK surface

| family | distinct symbols | call sites | note |
|---|---:|---:|---|
| GX | 128 | 5,679 | §3 |
| MTX/VEC | 74 | 4,172 | pure C already in tree, `-DMTX_USE_C` |
| OS | 53 | 774 | 578 of them are `OSReport` |
| DC/IC cache | 5 | 284 | `DCFlushRange(NoSync)`, `DCStoreRange(NoSync)`, `DCInvalidateRange` — all become no-ops |
| MSM | 100 | 436 | game-side library, keep |
| AR/ARQ | 12 | 24 | (a further 112 "AR" hits are the `ARRAY_COUNT` macro) |
| DVD | 12 | 62 | |
| VI | 13 | 47 | |
| CARD | 23 | 37 | |
| THP | 27 | 61 | movies |
| PAD | 9 | 18 | |
| AI | 8 | 15 | |
| DEMO | 2 | 3 | `DEMOUpdateStats`/`DEMOPrintStats`, behind a disabled flag |
| SI | 1 | 1 | |
| EXI / DSP | 0 | 0 | game never touches them directly |

`OSReport` at 578 sites is the port's best friend: the game narrates its own
boot, its DLL loads, its heap state and its DVD errors. M1's success criterion
falls straight out of it.

---

## 2. Architecture for the port

### 2.1 The shape, mirrored from the Snowboard Kids ports

```
port/
  Makefile              the whole build
  build-ppc.sh          docker wrapper around it
  patches.txt           exact-text substitutions into mirrored sources
  README.md
  docs/PLAN.md          this file
  docs/inventory.md     generated
  src/
    platform/    main.c, the host loop, SDL window/GL context, settings, argv
    os/          OSThread/OSAlarm/OSInterrupt/OSAlloc/OSReport/OSLink shims,
                 the HUPROCESS context switch, psmtx_c.c, cache no-ops
    gx/          the GX state machine + the GL 1.3 backend + the DL recorder
    dvd/         DVD over an extracted files/ tree or the disc image
    card/        CARD over two host files
    pad/         PAD over SDL + the IOKit Xbox One driver
    audio/       AI + the MusyX DSP-command interpreter + SDL output
    debug/       self-play, tracing, --peek, --dumpdl, perf
    ui/          launcher and in-game overlay
  tools/         inventory.py, mirror_src.py, gen_bundles.py, make_bundle.sh, ...
  scripts/       input scripts and goldens
  ref/           emulator reference frames and .dtm movies
  resources/     icons, Info.plist bits
```

### 2.2 What we keep

**All of `src/game`, `src/game/board`, `src/msm`, `src/libhu` and all of
`src/REL` compile as-is**, modulo §1.13's short hazard list. In particular:

- **`src/game/jmp.c` ports nearly verbatim.** `gcsetjmp`/`gclongjmp` save
  `lr, cr, sp, r2, r13-r31, f14-f31, fpscr`. On the 32-bit Darwin PowerPC ABI
  r13–r31 are non-volatile exactly as in EABI, and r2 is volatile (so saving it
  is harmless). The only work is rewriting Metrowerks' `asm { }` with
  structure-member operands into a `.s` file (or GCC extended asm) — about 80
  lines, and the SBK ports already did the same job in
  `port/src/ultra/sbk_ctx_ppc_darwin.s`. **We do not need `libco`, `ucontext`
  or coroutines-in-C.** The game's own scheduler runs unmodified on the metal.
  One caution: the stacks are tiny (`HuPrcCreate` defaults to 2,048 bytes) and
  a Darwin/GCC-14 frame is fatter than a Metrowerks EABI one. The port should
  multiply `stack_size` (a one-line `patches.txt` hook, or a `-D`) and keep the
  game's own `stack overlap error` guard byte check in `HuPrcCall`.
- **`src/game/malloc.c`'s four `mflr` blocks** become
  `__builtin_return_address(0)`.
- **`src/dolphin/mtx/{mtx,vec,quat,mtx44,mtxvec}.c` compile with `-DMTX_USE_C`**,
  giving pure-C `C_MTX*`/`C_VEC*`. `psmtx.c` is dropped and replaced with
  forwarders.
- **`extern/musyx` above the SAL** compiles: the sequencer, synth, snd3d,
  streams, the reverb/chorus/delay DSP-side effects. Only the hardware layer is
  ours.

### 2.3 What we replace

| SDK module | verdict | what the port does |
|---|---|---|
| **OS: threads** | replace, trivially | one `OSThread` (the reset watcher). Implement `OSCreateThread`/`OSResumeThread`/`OSSleepThread`/`OSWakeupThread`/`OSYieldThread` over the host loop: run `ToeThreadFunc` as a callback once per frame. No scheduler needed. |
| **OS: interrupts** | no-op | `OSDisableInterrupts`/`OSRestoreInterrupts` (20 sites) return/accept a dummy cookie. We are single-threaded and cooperative. |
| **OS: cache** | no-op | `DCFlushRange`, `DCStoreRange`, `DCInvalidateRange`, `HuMemDCFlushAll` — 284 sites, all become empty. (They must *stay callable*, not be `#define`d away, because some are called through the SDK.) |
| **OS: time** | host | `OSGetTick`/`OSGetTime`/`OSTicksToMilliseconds` off `mach_absolute_time()`, scaled to the console's 40.5 MHz bus/4 tick so `OSCheckStopwatch` reads sensibly. |
| **OS: arena/heap** | host | `OSInitAlloc`/`OSCreateHeap`/`OSAlloc`/`OSAllocFixed`/`OSSetCurrentHeap` over one 24 MB `malloc`ed block, so the game's own heap accounting and its "Rest Memory" reports stay meaningful. `OSGetPhysicalMemSize`/`OSGetConsoleSimulatedMemSize` return 0x01800000, `OSGetConsoleType` returns retail — which keeps the game off the `LoadMemInfo` path entirely. |
| **OS: exceptions/reset** | stub | `OSPanic` prints and aborts (with a `--panic-abort` to drop into the debugger); `OSResetSystem`/`OSGetResetButtonState` map to a quit hotkey; `__OSFpscrEnv`, `OSSetErrorHandler`, TRK, `OdemuExi2` and the fault handler are dropped. |
| **OS: `OSInitFastCast` / `OSf32tos*`** | plain casts | 65 sites; a header in `port/include/` overrides them. |
| **OS: `OSLink`/`OSUnlink`** | replace | §2.4. |
| **DVD** | replace | ~250 lines over an extracted `files/` tree (`data/`, `dll/`, `sound/`). Build a path→entrynum table once at startup by walking the tree in FST order, so `DVDConvertPathToEntrynum`/`DVDFastOpen(entrynum)` work. `DVDReadAsync` reads synchronously and posts the callback at the *next* frame boundary, which preserves the game's `while (!CallBackStatus) HuDvdErrorWatch();` spin without deadlocking. `DVDGetDriveStatus` always returns ready. Also accept the raw ISO with an FST parser, as the launcher's bring-your-own-disc path. |
| **CARD** | replace | two host files under `~/Library/Application Support/MarioParty4/`, one per slot, holding the raw file bytes plus a small index; `CARDProbeEx` reports a 251-block card. ~350 lines. |
| **PAD** | replace | SDL2 game controller + the existing IOKit Xbox One driver from `snowboardkids2-decomp/port/src/platform/input_xone.c`. `PADControlMotor` drives SDL haptics / the Xbox driver's rumble. `PADRead` fills `PADStatus[4]` with the GameCube's own analog ranges and the game's own `PADClamp` does the rest. |
| **VI** | replace | `VIWaitForRetrace` **is the idle gate** (§2.5). `VIGetRetraceCount` counts host frames; `VISetNextFrameBuffer`/`VIFlush` mark a swap pending; `VIConfigure` records the render mode; `VISetPre/PostRetraceCallback` are called at the gate. ~200 lines. |
| **GX** | replace | §3. The big one. |
| **AI / DSP** | replace | `AIInit`/`AIRegisterDMACallback`/`AIInitDMA`/`AIStartDMA` become an SDL audio device that calls MusyX's DMA callback when it wants a buffer; `DSP*` is never called by game code and only by MusyX's `hw_dolphin.c`, which we replace with `port/src/audio/musyx_sal.c`. |
| **AR / ARQ** | replace | 16 MB `calloc`; `ARAlloc`/`ARFree` over the SDK's own allocator logic; `ARQPostRequest` becomes a `memcpy` plus a deferred callback at the next frame boundary (the game's `armem.c` counts outstanding requests in `arqCnt` and waits on it). ~200 lines. |
| **SI** | stub | one call. |
| **EXI** | drop | never called by game code. |
| **THP** | later | 27 symbols, 61 sites. `THPDec.c` is 352 paired-single sites of JPEG IDCT — do not port it. Decode THP with a small MJPEG decoder (or pre-transcode the movies during install) behind the same `THPSimple*` API. Until then, stub: `THPSimpleGetTotalFrame` returns 1 and `THPSimpleDrawCurrentFrame` draws black, so the flow proceeds. |
| **DEMO** | stub | 3 sites behind a disabled `DemoStatEnable`. |
| **TRK / OdemuExi2 / amcstubs / odenotstub / MSL / Runtime** | drop | debugger stubs and the Metrowerks C runtime; the host libc replaces them. |

### 2.4 REL loading: the decision

Four options.

**(a) Link everything statically, hand the loader a table of entry points.**
Cheapest at runtime, and the obvious first instinct. It fails on symbols: the
99 modules' `symbols.txt` files hold 26,735 names, of which **899 distinct
names are defined by more than one module, across 2,622 definitions** —
`_ctors` and `_dtors` in all 99, `_prolog` and `_epilog` in 92, `ObjectSetup` in
90, `BoardCreate`/`BoardDestroy` in 9, `__fakeHalf`/`__fakeThree` in 16, and
~880 more (many are `scope:local` and so already file-static, but the globals
alone are hundreds). Making this link means mechanically renaming every global
in every module — a `-D_prolog=_prolog_w01Dll` per translation unit, or an
`objcopy --redefine-syms` pass — and then teaching `omDLLLink` a generated
table. It also loses the loader's fifth contract point: re-entering a resident
module needs its bss re-zeroed, which with static linking means generating a
per-module bss extent table and hoping the linker keeps each module's data
contiguous (it will not, reliably — the SBK2 port's hardest bugs came from
exactly that assumption). **Rejected as the first step; kept as a fallback
single-binary build mode later, if bundle load time ever matters.**

**(b) One native shared library per REL, `dlopen`ed. — RECOMMENDED, and
independently proven: this is exactly what partyboard does.**

`/Users/zachjack/Apps/ref/partyboard/CMakeLists.txt:198-256` builds each of the
96 `src/REL/<mod>/` directories as a `SHARED` library exporting **exactly one
symbol, `ObjectSetup`** — enforced with `/EXPORT:ObjectSetup` on MSVC, a
`--version-script=rel.map` on ELF, and `-exported_symbols_list rel_symbols.txt`
(`_ObjectSetup`) on Mach-O. The main `dol` target is *itself* a shared library
exporting 1,106 symbols via a generated `dol.def`, so the REL libraries link
back against game code. And `omDLLLink` in
`/Users/zachjack/Apps/ref/partyboard/src/game/objdll.c:106-155` is rewritten
with a platform switch: `LoadLibrary` + `GetProcAddress` on Windows,
**`dlopen(name, RTLD_LAZY)` + `dlsym(handle, "ObjectSetup")` on
Linux/macOS/Android**, with the original `HuDvdDataReadDirect` + `OSLink` path
kept under `__MWERKS__`. `omDLLUnlink` calls `dlclose`.

That transfers to `powerpc-apple-darwin8` essentially verbatim: Mach-O
`-bundle -bundle_loader <exe>` (or a `.dylib` with `-exported_symbols_list`),
`@loader_path` rpath, `dlopen`/`dlsym`/`dlclose` — all present and working on
Mac OS X 10.4/10.5.

Why it is right independent of precedent: it maps **exactly** onto the five
contract points of §1.4. Each library is its own namespace, so the 899 colliding
names vanish untouched; `RTLD_LOCAL` keeps one module's `ObjectSetup` from being
seen by another; and `dlclose` of an `MH_BUNDLE` genuinely unloads it, so a
subsequent `dlopen` gives a fresh, zeroed bss for free — which is the re-entry
case (§1.4 point 5), handled without a bss extent table.

Two places where we should *not* follow partyboard:

- They export and call **`ObjectSetup`**, bypassing the module's real `_prolog`
  (and therefore its `_ctors`). For C modules that is harmless, but our loader
  should export and call `_prolog`/`_epilog` and keep `objdll.c`'s flow intact,
  so the game's own load/unload narration and its `dll->ret` stay meaningful.
- They `#else`-out the whole re-entry branch of `omDLLStart` (`objdll.c:44-56`),
  losing the "resident module, bss re-zeroed, prolog re-run" case. We should
  keep it, implemented as `dlclose` + `dlopen`.

The three `links: [_minigameDll]` modules are handled by making `_minigameDll`
a real dylib the three `mstory*` libraries link against, or (simpler, given it
is a 572-byte stub) by folding it into the main binary.

The costs are honest and small: 99 libraries of 30 KB–400 KB each inside the
`.app`; a `dlopen` at each scene transition, single-digit milliseconds on a G4
with a warm page cache against a real DVD seek of hundreds. Use `RTLD_NOW` so a
missing SDK stub is a loud failure at load rather than a crash later. Build
with `-fno-common`. And never call `dladdr` in a hot path — the SBK port
measured it at 27% of frame time.

The costs are honest and small: 99 bundles of 30 KB–400 KB each shipped inside
the `.app`; a `dlopen` at each scene transition, which on a G4 with a warm page
cache is single-digit milliseconds against a real DVD seek of hundreds; and
`RTLD_NOW` making every module's imports resolve up front, which is what we
want (a missing SDK stub becomes a loud failure at load, not a crash later).
`dlclose`'s reliability is the one thing to prove early — M1 should include a
"load and unload every one of the 99 bundles twice, in a loop" smoke test.

Two Darwin details to get right: build with `-fno-common` so tentative
definitions become real and land in the bundle; and never call `dladdr` in a
hot path (the SBK port measured `dladdr` per retrace at 27% of frame time).

**(c) Keep the `.rel` files as data and interpret them.** Rejected as the brief
says, and correctly: it is an emulator.

**(d) Keep the `.rel` files as data and *execute* them.** Worth naming because
This is the option that only a PowerPC target can even consider, so it deserves
a real answer rather than a dismissal. The G4 executes 750/Gekko integer and FP
code natively; the RELs contain no paired singles (§1.12); and `OSLink` is pure
relocation software — `doldecomp/dolsdk2001`'s `src/os/OSLink.c` is **378 lines
of plain C with no MMIO**, portable as-is. The apparent blocker, that a REL's
imports from module 0 are relocations against **absolute addresses in the
original DOL** (`0x800xxxxx`, kernel space on 32-bit Darwin), is not actually
fatal: `config/GMPE01_01/symbols.txt` gives every DOL symbol's original
address, so a loader could translate each module-0 relocation target from an
original address into a native function pointer through a generated table.

What kills it is the **ABI**. The REL code is Metrowerks EABI, which reserves
**r2 as the SDA2 (small read-only data) base and r13 as the SDA base**, set
once at startup and assumed constant thereafter. On 32-bit Darwin PowerPC r2 is
*volatile*, so any call from REL code into our natively compiled DOL would
return with r2 clobbered, and every subsequent `lwz rX, off(r2)` in that module
would read garbage. Fixing that means a save/restore thunk on every one of the
thousands of crossings in both directions, plus a per-module SDA allocation.
Since all 99 modules are 100%-matching *source* that we can simply compile,
paying that price would be perverse. **Rejected — but it is the reason option
(b) is a choice rather than a necessity, and worth remembering for a future
project whose overlays are not decompiled.**

**Recommendation: (b).** It is the least code, the least risk, the closest
match to what the game already believes is happening, and it is the option the
game's own upstream PC port already shipped.

### 2.5 The host loop and determinism

The game blocks in `VIWaitForRetrace()`. That is the whole design:

```c
/* port/src/platform/main.c */
mp4_host_init();          /* SDL window, GL 1.3 context, audio, pad, DVD, CARD */
mp4_gx_init();
game_main();              /* src/game/main.c's main(), renamed by -Dmain=mp4_game_main */
```

and `VIWaitForRetrace()`, in `port/src/os/vi.c`, is where the host runs:

```c
void VIWaitForRetrace(void) {
    if (vi_swap_pending) { gx_present(); vi_swap_pending = 0; }
    if (vi_pre_cb)  vi_pre_cb(vi_retrace_count);
    host_pump_events();          /* SDL events, quit, hotkeys */
    pad_update();                /* one sample per retrace */
    dvd_service();               /* fire completion callbacks queued this frame */
    arq_service();
    audio_service();
    reset_thread_tick();         /* the one OSThread */
    if (!turbo) sleep_until(next_retrace);
    next_retrace += 1.0/59.94;   /* resync if more than 250 ms behind */
    vi_retrace_count++;
    if (vi_post_cb) vi_post_cb(vi_retrace_count);
}
```

Everything the console delivered by interrupt — pad sampling, DVD completion,
ARAM DMA completion, audio DMA, the reset watcher — is delivered here, **once
per retrace, at a point the game chose**. Given the same input stream the game
sees the same events at the same points in its own logic, which is what makes
replay reproducible. `--turbo` and `--headless` drop the sleep.

The Snowboard Kids lesson applies: an idle-gated retrace **hides overruns** —
the game clock stays at 60 on paper while the wall clock falls behind. `--perf`
must always print both.

### 2.6 What we do *not* need, and why that matters

Two of the three hardest pieces of the N64 ports are simply absent here:

- **No pinned globals.** The N64 ports pinned 1,463 globals to their original
  RDRAM addresses because the game read across global boundaries and did
  physical-address arithmetic. Mario Party 4 addresses main memory through
  ordinary pointers into heaps it allocated itself, and ARAM through `u32`
  offsets. The only absolute addresses in game code are two lines in a
  dev-hardware-only path. So no `gen_pins.py`, no `pins.s`, no twin symbols,
  and none of the adjacency-pun bug class that cost the SBK2 port weeks.
  *(Caveat to verify at M2: the HSF file format and the `.bin` data files may
  contain internal offsets that the loader fixes up — `hsfload.c` — but those
  are offsets relative to the loaded buffer, not absolute addresses.)*
- **No byteswapping.** The G4 is big-endian, like the GameCube. Every `.bin`,
  `.hsf`, `.msm` and `.rel` on the disc can be used in place. The upstream
  `port` branch needs a 1,094-line `byteswap.cpp` for this; we need none of it.
  This is a real, quantified advantage of targeting PowerPC.

---

## 3. The GX plan for the Radeon 9000

### 3.1 The shape

Three layers, mirroring `gfx_pc.c` / `gfx_gl13.c` / `gfx_sdl_gl13.c`:

- **`port/src/gx/gx_state.c`** — the 128 `GX*` entry points. They do not draw;
  they write into one `GXState` struct (vertex descriptor and attribute
  formats, array bases and strides, the TEV stage array, texgens, channel
  controls and lights, matrices, texture objects and TLUTs, blend/z/alpha/fog,
  viewport/scissor/cull). This file is mostly mechanical and is where most of
  the 5,679 call sites land.
- **`port/src/gx/gx_draw.c`** — at each `GXBegin`…`GXEnd` (or
  `GXCallDisplayList`), resolve the state into a GL configuration, decode the
  vertices, and emit. Vertex decode is a per-draw CPU pass: read the VCD/VAT,
  and for each of the 92 `GX_QUADS`/19 strip/… primitives pull each attribute
  either inline (`GX_DIRECT`) or by index from the `GXSetArray` base
  (`GX_INDEX16` at 136 sites, `GX_INDEX8` at 14), converting `GX_S16`/`GX_U16`
  with the VAT's fractional shift, `GX_RGBA6`/`GX_RGB565` to RGBA8, into one
  interleaved float/ubyte vertex buffer, then `glDrawArrays`. `GL_QUADS` exists,
  so quads pass straight through.
- **`port/src/gx/gl13_backend.c`** — the fixed-function translation, §3.4.
- **`port/src/gx/gx_tex.c`** — texture decode and cache.

### 3.2 Matrices

`GXLoadPosMtxImm` (121 sites), `GXLoadNrmMtxImm` (27), `GXLoadTexMtxImm` (93),
`GXSetProjection` (44), `GXSetViewport` (37). **There are no per-vertex matrix
indices anywhere in the game** (§1.14), so there is exactly one active
position matrix, one normal matrix and up to eight texture matrices at any
draw. That means:

- position matrix → `glMatrixMode(GL_MODELVIEW); glLoadMatrixf(...)` with the
  GX 3×4 row-major matrix transposed into GL's 4×4 column-major;
- `GXSetProjection` → `GL_PROJECTION`, remembering that GX's projection maps z
  to [-1, 0] while GL maps to [-1, 1]: post-multiply by
  `diag(1,1,2,1) + translate(0,0,1)` (equivalently scale z by 2 and add w);
- texture matrices → `GL_TEXTURE` per unit;
- the normal matrix is GL's own inverse-transpose of the modelview, which GL
  computes; where the game loads a normal matrix that is *not* the inverse
  transpose of the position matrix (it does, for some effects), pre-transform
  normals on the CPU during vertex decode.

`GXSetViewport`'s z range `(0,1)` → `glDepthRange`. `GXSetViewportJitter` (used
in `HuSysBeforeRender` when `field_rendering` is on, which it is not for
`GXNtsc480IntDf`) can be a plain viewport.

### 3.3 Display lists

Because every display list is built by the game at runtime and contains only
`GXBegin`/vertex/`GXEnd` (§1.14), `GXBeginDisplayList(buf, size)` can simply
put `gx_state.c` into **record mode**: subsequent `GXPosition*`/`GXNormal*`/
`GXColor*`/`GXTexCoord*`/`GXBegin`/`GXEnd` append to `buf` in the **real GX
byte encoding** (1-byte `GXBegin` opcode with the vertex format in the low 3
bits, a big-endian `u16` vertex count, then packed attribute bytes in VCD
order), and `GXEndDisplayList()` returns the byte count.

Encoding it exactly as GX does, rather than inventing a record format, is worth
the small extra effort for one specific reason: the game slices the display-list
buffer by the `dlSize` we return (`DrawData[].dlOfs`, a 0x20000 cap in
`hsfdraw.c`, 0x10000 and 0x100 elsewhere). A fatter encoding risks overrunning
buffers the game sized for the console, in a way that would show up as
corruption a hundred frames later. Byte-exact sizes make that class of bug
impossible. `GXCallDisplayList(list, size)` then parses the stream against the
*current* VCD/VAT/array state — which is correct, because that is what the
hardware did, and the game always sets state immediately before the call.

`GXResetWriteGatherPipe` and `GXFlush`/`GXDrawDone`/`GXPixModeSync`/
`GXInvalidateVtxCache`/`GXInvalidateTexAll` are no-ops or cache flushes.

### 3.4 TEV → texture environment, in detail

The Radeon 9000 gives us six texture units, each running one
`ARB_texture_env_combine` stage with `ATI_texture_env_combine3`'s
`MODULATE_ADD_ATI` / `MODULATE_SIGNED_ADD_ATI` / `MODULATE_SUBTRACT_ATI`, plus
the crossbar so any stage can read any unit's texel. Sources per stage:
`GL_TEXTURE`, `GL_TEXTUREn` (crossbar), `GL_PREVIOUS`, `GL_PRIMARY_COLOR`,
`GL_CONSTANT` (that unit's `GL_TEXTURE_ENV_COLOR`), and each with
`GL_SRC_COLOR`/`ONE_MINUS_SRC_COLOR`/`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`.

A GX TEV stage computes, per channel,

```
out = (d + lerp(a, b, c)) * scale + bias      /* GX_TEV_ADD, clamped */
    = (d + a*(1-c) + b*c) * scale + bias
```

with `a,b,c,d` each chosen from ~16 sources (CPREV/APREV, C0-C2/A0-A2 konst
registers, TEXC/TEXA, RASC/RASA, ONE, HALF, KONST, ZERO). The mapping:

| GX pattern | how many sites | GL stages |
|---|---|---|
| `GXSetTevOp(GX_MODULATE)` / the equivalent explicit form: `out = TEXC*RASC` | the common case | **1** stage: `GL_MODULATE`, `GL_TEXTURE` × `GL_PRIMARY_COLOR` |
| `GX_REPLACE` (`out = TEXC`) | common | **1** stage: `GL_REPLACE` |
| `GX_PASSCLR` (`out = RASC`) | common | **1** stage: `GL_REPLACE` with `GL_PRIMARY_COLOR`, or texturing off |
| `out = TEXC*KONST` or `TEXC + KONST` | 73 `GXSetTevColor` sites | **1** stage, konst in `GL_TEXTURE_ENV_COLOR` |
| `lerp(a, b, TEXA)` — the standard blend of two colours by a texture's alpha | frequent in the sprite/UI path | **1** stage: `GL_INTERPOLATE` |
| `d + a*(1-c) + b*c` with `d ≠ 0` | the HSF material path's 2- and 3-stage forms | **2** stages via `MODULATE_ADD_ATI` |
| `scale = 2` or `4`, `bias = ±0.5` | occasional | `GL_RGB_SCALE` handles ×2/×4; bias by folding into a `SIGNED_ADD` or an extra stage |
| explicit 5-stage material | 2 sites | **5** stages, still inside 6 |

**The estimate.** Of 121 `GXSetNumTevStages` sites, 93 ask for one stage and 14
for two; the highest literal is 5 and the highest stage id ever named is
`GX_TEVSTAGE4`. Adding one GL stage for a fog blend and one for an alpha-test
helper in the worst case still lands inside six units. **The working assumption
is that essentially all of Mario Party 4 renders in one pass on this card**,
and that assumption is cheap to verify: the backend keeps a counter of
"combiner needed more than 6 units" and `--gxwarn` prints each distinct
offender with the source `shader_id`, exactly as `gfx_gl13.c` does.

Per-case fallbacks, in order of preference:

1. **More than 6 units needed** → split the stage chain into two passes:
   render stages 0..k with `GL_ONE, GL_ZERO` into the framebuffer, then stages
   k+1..n with additive or modulate blending. Costs a second transform of the
   same geometry; acceptable for the handful of cases expected.
2. **`GXSetTevSwapMode` (28 sites)** → swap tables reorder RGBA lanes before a
   stage reads them. The common swaps (RRRA / GGGA / BBBA, used to fan a
   channel out) are expressible as `GL_SRC_ALPHA`-style single-channel reads
   only for alpha; the general case needs a per-swap decoded copy of the
   texture in the cache (cheap: 28 sites, few distinct tables) or the escape
   hatch of §3.9.
3. **Indirect texturing (17 enabling sites, all `GXSetTevIndWarp`)** → not
   expressible in fixed function at all. Three responses, chosen per effect
   after looking at what each one is: (a) if the warp amplitude is small and
   the surface is a full-screen or large quad, tessellate the quad on the CPU
   and apply the warp per vertex — a heat-haze/water-ripple effect done this
   way is visually close and costs nothing on the GPU; (b) drop the indirect
   stage, keep the direct one, log once; (c) `ATI_text_fragment_shader` (§3.9).
   Since only 3 modules ever set more than one indirect stage, this is a small,
   bounded, *named* list of effects rather than an open-ended problem.
4. **Z textures (2 sites)** → drop, log.
5. **`GX_VA_NBT` bump mapping (6 sites)** → drop the perturbation, keep the
   base material.

### 3.5 Lighting

`GXSetNumChans` is 1 at 93 sites, 2 at 4, 0 at 9. `GXSetChanCtrl` (143 sites)
selects, per channel, whether the colour comes from the vertex, a register, or
lighting, and which of 8 lights are enabled with which diffuse/attenuation
functions. `GXInitLight*` at 39 sites, `GXLoadLightObjImm` at only 4.

Map to `GL_LIGHT0..7`: `GXInitLightPos` → `GL_POSITION` with w=1,
`GXInitLightDir` → w=0 (or `GL_SPOT_DIRECTION`), `GXInitLightColor` →
`GL_DIFFUSE`, `GXInitLightAttn`/`GXInitLightSpot`/`GXInitLightDistAttn` →
`GL_CONSTANT/LINEAR/QUADRATIC_ATTENUATION` and `GL_SPOT_CUTOFF`/`GL_SPOT_EXPONENT`.
`GXSetChanMatColor`/`GXSetChanAmbColor` → `GL_COLOR_MATERIAL` /
`glMaterialfv(GL_AMBIENT)`.

The approximations are known and acceptable: GX's attenuation is a
ratio-of-quadratics that GL's `1/(k0+k1 d+k2 d²)` cannot match exactly, and
GX's `GX_DF_CLAMP`/`GX_DF_SIGN` diffuse functions differ from GL's `max(0, N·L)`.
For a party game with a handful of lights per scene these show up as slightly
different falloff, not as wrong pictures. Where the game uses `GX_SRC_REG` with
lighting off — the overwhelmingly common case — GL lighting is simply disabled
and the colour comes from the vertex array.

### 3.6 Textures

Ten formats to decode (§1.14). All GX textures are stored in tiles: 4×4 for
16-bit formats, 8×4 for I4/C4, 8×8 for CMPR's 2×2 arrangement of DXT1 blocks.
`gx_tex.c`:

| GX format | GL |
|---|---|
| `GX_TF_RGB565` | `GL_RGB5`, `GL_UNSIGNED_SHORT_5_6_5` after de-tile |
| `GX_TF_RGB5A3` | expand to `GL_RGBA8` (the two sub-formats differ per texel) |
| `GX_TF_RGBA8` | de-tile the split AR/GB 4×4 blocks → `GL_RGBA8` |
| `GX_TF_I4` / `I8` | `GL_LUMINANCE8` — and, per the SBK lesson, an intensity texel is `(I,I,I,I)`, not `(I,I,I,1)`, so use `GL_LUMINANCE` only where alpha is unused and expand otherwise |
| `GX_TF_IA4` / `IA8` | `GL_LUMINANCE8_ALPHA8` |
| `GX_TF_C4` / `C8` | expand through the TLUT on the CPU to `GL_RGBA8`; the card has no paletted-texture path we should rely on |
| `GX_TF_CMPR` | `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` if `EXT_texture_compression_s3tc` is present (it is on the Radeon 9000) — but GX's CMPR differs from DXT1 in block ordering *and* in its 3-colour-mode interpolation; safest first cut is CPU decode to RGBA8, with S3TC as an optimisation once pixels match |

**No NPOT.** GX textures are always power-of-two, so this is mostly free —
but the SBK ports were bitten by a 3×1 texture becoming *incomplete* and
silently disabling texturing, so `gx_tex.c` must assert POT and log.

**Cache**: hash on `(data pointer, format, width, height, TLUT pointer, mip
count, content hash of the texel bytes and palette)`. The content hash is not
optional — Mario Party rewrites scratch textures and animated palettes in place
at the same address (`HuSprTexLoad` in the hilite path is visible doing exactly
this in `hsfdraw.c:716`), and without it the SBK ports got flickering text.

`GXInvalidateTexAll` is called once per frame from `HuSysBeforeRender`; it must
*not* flush our cache, only the (nonexistent) TMEM.

### 3.7 EFB copies, XFB, and scaling

There is no FBO on this card. `GXCopyTex` (28 sites) becomes
`glCopyTexSubImage2D` from the back buffer into a cached texture sized by
`GXSetTexCopySrc`/`Dst`, which is exactly what fixed-function-era engines did
and is well supported. The copy formats in use are `GX_CTF_R8` and `GX_CTF_A8`
plus `GX_TF_Z24X8`/`Z8`: single-channel masks and a depth read. Depth copies
need `GL_DEPTH_COMPONENT` via `glCopyTexImage2D` with
`ARB_depth_texture` — **check this at runtime**; if the Radeon 9000's GL 1.3
driver lacks it, the two `GX_TF_Z8`/three `GX_TF_Z24X8` sites get dropped with a
warning (they are almost certainly a shadow or a depth-of-field helper).

`GXSetCopyClear` (13 sites) → `glClearColor`/`glClearDepth`.
`GXCopyDisp` (2 sites, both in `init.c`/`HuSysDoneRender`) → mark a swap
pending; the actual `SDL_GL_SwapWindow` happens at the retrace gate (§2.5), so
the double-buffer discipline the game expects is preserved. `GXSetDispCopyYScale`
and the copy filter (`RenderMode->vfilter`, the deflicker filter) are ignored on
a progressive display, which is what we want.

`GXSetScissor` → `glScissor`; `GXSetCullMode` → `glCullFace`/`glFrontFace`
(remembering GX's front/back convention is the opposite of GL's default);
`GXSetZMode` → `glDepthFunc`/`glDepthMask`; `GXSetZCompLoc` (38 sites, early-Z)
is advisory and ignored; `GXSetAlphaCompare` (59) → `glAlphaFunc` for the
single-comparison cases and, for the two-comparison AND/OR forms, the
conservative single test plus a log; `GXSetBlendMode` (125) → `glBlendFunc`
with `GX_BM_SUBTRACT` needing `EXT_blend_subtract` (present); `GXSetFog` (9
sites) → `GL_FOG` with `EXT_fog_coord` for the per-vertex path.

### 3.8 How much fits in one pass

Summarising §3.4–§3.7 against the six units:

- **~90% of draws: 1 TEV stage → 1 GL unit.** Trivially one pass.
- **~10%: 2 stages → 2 units.** One pass.
- **A handful: 3 and 5 stages → 3 and 5 units.** One pass.
- **Indirect warp (17 sites, ≤3 ind stages) → not expressible.** Per-effect
  fallback; a named, bounded list.
- **Swap modes (28 sites) → mostly expressible, some need a decoded copy.**

So: one pass for effectively the whole game, with a short list of named effects
degraded. That is a far better position than the N64 ports started from, where
the RDP combiner routinely needed 4–6 units for ordinary geometry.

### 3.9 `GL_ATI_text_fragment_shader` as the escape hatch

The Radeon 9000 (R250) exposes `GL_ATI_text_fragment_shader`: a real,
if peculiar, fragment shader, specified as a text program compiled through
`glProgramStringARB`-like entry points into the R200's fragment pipeline. Its
budget is roughly the R200's: **2 arithmetic passes × 8 instructions, 6 texture
lookups, 6 constants, and — critically — dependent texture reads** (one level of
indirection between the two passes).

What that buys us, concretely:

- **Indirect texturing.** `GXSetTevIndWarp` is precisely "sample texture A,
  scale its texel through a 2×3 matrix, add to texcoord B, sample texture B" —
  a dependent read, which is exactly what the second arithmetic pass exists
  for. All 17 enabling sites use ≤3 indirect stages and the R200 pipeline
  supports the shape.
- **Arbitrary swizzles**, which fixed-function crossbar cannot express, are
  free in a text shader.
- **TEV chains deeper than 6 units** collapse into 8+8 instructions.
- **Alpha compare's two-comparison AND/OR forms** become a `KIL`-style discard.

What it does not buy: it is R200-only (so the backend needs both paths anyway,
and the fixed-function path stays the primary one for portability to other
old Macs), the extension's text syntax is idiosyncratic and thinly documented,
and 8 instructions per pass is not much once a TEV chain's lerps are expanded.

**Plan: build the fixed-function path first and completely (M2–M5). Add
`--gxshader` as an opt-in second backend at M8, targeted at exactly the effects
the `--gxwarn` counter names.** Do not make correctness depend on it. And note
for the record: no Dolphin code is to be read or copied for any of this —
Dolphin is GPLv2+ and this port is not. GX semantics come from the decomp's own
`src/dolphin/gx/` sources and from the publicly documented GX API in
`include/dolphin/gx*.h`, both already in this repository.

---

## 4. Milestones

Each has a **done means** that is checkable without argument.

### M0 — matching build *(done)*
`python3 configure.py --version GMPE01_01 --wrapper $HOME/.local/bin/wine && ninja`
produces `build/GMPE01_01/main.dol` matching `config/GMPE01_01/build.sha1`, plus
99 RELs.
**Done means:** it already does, on this Mac.

### M1 — it links and it talks *(done, §9)*
Cross-compile the DOL's game code (`src/game`, `src/game/board`, `src/msm`,
`src/libhu`) plus `src/dolphin/mtx` with `-DMTX_USE_C`, against a `port/` layer
in which every one of the ~280 non-GX SDK symbols and all 128 GX symbols exist —
most as loud stubs. Link one `powerpc-apple-darwin8` executable.
**Done means:** running it prints, in order, the `OSReport` narration of
`HuSysInit` through `omMasterInit` — specifically `objdll>Link DLL:dll/bootdll.rel`
— and then stops at the first unimplemented thing, naming it. No GL, no window,
no audio required.

Concretely, M1 needs (see §6):
1. the mirror + `patches.txt` machinery and the `Makefile`/`build-ppc.sh` pair;
2. `port/src/os/`: `OSReport` (578 sites), the arena/heap allocator, time,
   interrupts and cache as no-ops, `OSPanic`, the fast-cast overrides;
3. `port/src/os/jmp_ppc_darwin.s`: `gcsetjmp`/`gclongjmp`;
4. `port/src/os/psmtx_c.c`: the ~30 `PSMTX*`/`PSVEC*` forwarders;
5. `port/src/dvd/`: the extracted-tree FST and `DVDConvertPathToEntrynum` (the
   game `OSPanic`s at `data.c:65` on the first missing file, so this must be
   real, not a stub);
6. `port/src/gx/gx_stub.c`: all 128 GX entry points, empty, with a
   `--gxtrace` mode that prints each one the first time it is called;
7. a generated `port/gen/sdk_stubs.c` from `inventory.py`'s symbol table, so
   nothing is missed by hand.

### M2 — the first frame *(M2a done, §10)*
An SDL2 window with a **GL-1.3-restricted context** on the development Mac
(a debug layer that rejects any call outside the GL 1.3 + named-extension set,
so we cannot accidentally depend on something the Radeon 9000 lacks), the GX
state machine, vertex decode, the fixed-function backend, texture decode, and
REL loading via `dlopen`. `bootDll` runs.
**Done means:** the Hudson and Nintendo logos render, and a screenshot from the
Mac and one from the G4 are the same picture.

### M3 — menus
`mentDll`, `modeseldll`, `selmenuDll`, `option`, the sprite and font paths, PAD
input, CARD saves.
**Done means:** boot → title → mode select → character select → board select,
navigated with a real controller on the G4, with a save file created and
reloaded across a restart.

### M4 — one minigame
One `m4xxDll` playable end to end.
**Done means:** the minigame's instruction screen, the game, and the results
screen all run at 60 fps on the G4, and `--gxwarn` names every effect that was
degraded.

### M5 — a board with CPU players *(the first turn and the first minigame land, §15)*
A full board (`w01Dll` first — Toad's Midway Madness is the simplest) with four
players, three of them CPU, for a full 10-turn game including minigames, items,
the Star space and the Last 5 Turns event.
**Done means:** a complete game reaches the results screen without a crash, a
hang, or a wrong winner, three times running.
**Where it stands:** the board loads, plays its intro, rolls the turn order,
takes turns with items and spaces, picks a minigame, explains it, plays it and
pays it out (§15.4). It does not yet survive ten turns: the *second* minigame
(`m425dll`) dies with SIGBUS in its own draw hook on both builds (§15.6), and
that is what M5 has left.

### M6 — audio
The MusyX SAL replacement and the DSP-command interpreter; `src/msm` and
`extern/musyx` above the SAL compiled as-is; SDL audio out.
**Done means:** board music, minigame music, character voices and SFX all play,
the `--wav` capture of a fixed 30-second board segment matches a Dolphin
capture of the same segment to the ear, and the audio path costs less than
1.5 ms/frame in `--perf`.

### M7 — the self-play harness *(landed, §17; the m425 crash is reproduced and diagnosed, not fixed)*
The game's CPU players already exist: `GWPlayerCfg[i].diff` (an `s16` per
player) and `GWSystem.diff_story`. The harness sets those and hands player 1 to
the CPU, then drives menus with a task-name navigator (the equivalent of
`menu_nav.c`, keyed off the `HUPROCESS` list and the `omObjData` object table
rather than `dladdr`, to avoid the 27% cost).
**Done means:** `--autoplay` completes a full four-player board unattended and
prints one result line; `--nightmare` (a difficulty above the game's own
hardest, imposed on the CPU opponents through the same variables) is defined
and measurable; a `regress` mode replays a set of recorded input scripts and
gets identical results every time.

### M8 — enhancements and the launcher slot
Widescreen or higher internal resolution, the `--gxshader`
`ATI_text_fragment_shader` backend for the named degraded effects, THP movie
playback, the launcher UI and a slot alongside the two Snowboard Kids apps,
bring-your-own-disc extraction, `.app` bundling and a `.dmg`.
**Done means:** a package a person can double-click, point at their own disc
image, and play.

### Risks, ranked

1. **The GX layer is 5,679 call sites of surface, and TEV correctness is
   judged by eye.** Mitigation: the frame-comparison harness (§5) from day one,
   the `--gxwarn` counter, `--dumpdl`, and the SBK ports' hard-won debugging
   discipline (dump the combiner before suspecting the geometry).
2. **`dlclose` semantics.** If Mac OS X 10.5 declines to actually unload a
   bundle in some case, a re-entered module keeps its old bss and the game
   misbehaves subtly rather than crashing. Mitigation: the load/unload smoke
   test at M1, and a fallback of explicitly zeroing a bundle's `__bss`/`__common`
   sections via `getsectiondata` on re-entry.
3. **MusyX's DSP command set.** It is the largest single piece of new code
   after GX, it is judged by ear, and getting it wrong is subtle (voices that
   never key off, ADSR that ramps wrong). Mitigation: build it against
   `hw_dspctrl.c` as the specification, and A/B against Dolphin captures.
4. **Frame budget on a 1 GHz G4.** The GameCube's 485 MHz Gekko had a hardware
   T&L and rasterisation unit doing work we now do partly on the CPU (vertex
   decode, texture decode, TLUT expansion, CMPR decode) and partly on a much
   weaker GPU. `hsfdraw.c` draws through display lists of indexed vertices,
   which is the good case; the sprite path is immediate-mode `GX_QUADS` with
   `GXPosition3f32`, which is the bad case. Mitigation: `--perf` from M2,
   measure before optimising, and keep the vertex decoder's per-format inner
   loops AltiVec-able.
5. **The 2,048-byte `HUPROCESS` stacks.** A GCC-14 Darwin frame is fatter than
   a Metrowerks EABI one, and the failure mode is one process quietly
   scribbling on another's heap block. Mitigation: raise the default stack, keep
   the game's own guard-byte check, and add a `--stackcheck` that paints and
   watermarks every process stack.

---

## 5. Testing

### 5.1 Dolphin as the reference

Dolphin is the reference *runtime*, never a source of code (GPLv2+; we read
none of it).

**The rig for this already exists**, in `port/ref/`, built before this plan:

| path | what |
|---|---|
| `port/ref/tools/capture.sh` | headless Dolphin capture — `-b -e <iso> -v Vulkan` against a pinned, isolated user directory, one PNG per emulated frame into `Dump/Frames`, SIGTERM after N seconds. Takes extra Dolphin args, so `-m movie.dtm` replays a recording. ~0.4× real time on an M1 Max. |
| `port/ref/tools/mkdtm.py` | authors a Dolphin `.dtm` from a plain-text script (`frames`, `press`, `tap`, `hold`, `release`, `stick`, `cstick`, `trigL/R`, `mark`), writing the 256-byte header plus one packed 8-byte `ControllerState` **per input poll**, and a sidecar `.marks` file naming labelled polls. |
| `port/ref/tools/pick.sh` | downsamples selected frames to 320×264 (exactly half of Dolphin's 1× native 640×528), named by the **original emulated frame number** so the filename is the comparison key. |
| `port/ref/tools/contact.sh` | builds a labelled contact sheet so a long capture can be scanned by eye in one image. |
| `port/ref/Config/{Dolphin,GFX}.ini` | the pinned emulator configuration, so captures are reproducible. |
| `port/ref/frames/boot-*.png` | 78 already-captured boot frames, `boot-0001` through `boot-6880`. |

So the emulator half of §5 is done; what M2 adds is the port half — `--shotat`
and a differ — and `port/docs/reference-dolphin.md`, which `mkdtm.py`'s
docstring already cites and which should record the `.dtm` field layout and the
capture recipe.

Three of Dolphin's features do the work:

- **Frames at fixed moments.** Dolphin's frame dump and its screenshot hotkey
  give a 640×480 image at a known frame; the port's `--shotat R1,R2,...` writes
  the same at a known retrace. As in the SBK ports, the two sides will *not*
  agree on absolute frame count (we skip the console's DVD seek time), so shoot
  a spread on both, find the matching pair once, and reuse it — pairs are
  stable across rebuilds.
- **Savestates and the memory watch.** Dolphin's Cheat Manager / memory viewer
  reads MEM1 at `0x80000000`; the port's `--peek ADDR:LEN` reads the equivalent
  offset inside our 24 MB block, resolved through the DOL's map so that
  `--peek GWSystem` works by name. Comparing a game-state struct between the
  two at the same logical moment is the single most useful debugging tool this
  port will have, and it is nearly free.
- **`.dtm` input recordings.** Dolphin's movie format is a 256-byte header
  plus one 8-byte `ControllerState` per polled frame (buttons bitfield,
  L/R analog, stick X/Y, C-stick X/Y) — `port/ref/tools/mkdtm.py` already
  writes them. It is trivially parseable, and it is the same data `PADRead`
  returns. **Design the input path so a `.dtm` can
  drive it:** `port/src/pad/pad_source.h` defines one interface —
  `bool pad_next(int chan, PADStatus *out)` — with three implementations
  (`pad_sdl.c`, `pad_script.c` for the text scripts, `pad_dtm.c`), selected by
  `--play FILE`. That way one recording drives Dolphin *and* the port, and
  every frame comparison is apples to apples. The caveat is that `.dtm`
  supplies one sample per *polled* frame, so the port must poll exactly once
  per retrace — which it does (§2.5).

### 5.2 Determinism

Same discipline as the N64 ports, adapted:

- One pad sample per retrace, delivered at the retrace gate. Nothing else
  reads input.
- DVD and ARQ completions fire only at the retrace gate, never mid-frame.
- `frand`/`rand8` are the game's own LCGs seeded from `frand()` at
  `HuSysInit`; the port seeds deterministically under `--seed N` and prints the
  seed otherwise.
- **The gotchas the SBK ports paid for, transposed:** the memory card is the
  Controller Pak of this game — the game writes it during the menu walk, so a
  scripted run must use `--nocard` (CARD probes report no card) or a scratch
  card file deleted before each trial, or goldens go stale for reasons nothing
  in the script explains. Likewise `--nopad` so an attached controller cannot
  inject a frame. And settings must not be read in a scripted run.
- `--hashframe` prints a per-run fingerprint (a hash of the game-state block
  plus the retrace count) so a divergence is located to a frame rather than
  guessed at.

### 5.3 Goldens

`port/scripts/golden/*.txt` are input scripts; `port/scripts/golden/*.dtm` are
their Dolphin-recorded equivalents where one exists. A `regress` mode replays
each and compares the fingerprint and the result line. The pass condition is
that every golden reproduces exactly, and it is checked before any commit that
touches `src/gx` or `src/os`.

---

## 6. What M1 needs, concretely

A checklist, in dependency order:

1. `port/Makefile` + `port/build-ppc.sh` — the Docker invocation, the
   `-isysroot /usr/local/MacOSX10.4u.sdk -mmacosx-version-min=10.4
   -malign-natural -mone-byte-bool` flag set, and a **second, host-native
   target** (unlike the SBK ports, which had none — this port should be able to
   build and run on the development Mac from day one, because the frame
   comparison in §5 depends on it).
2. `port/tools/mirror_src.py` + `port/patches.txt` — the exact-text patch
   mechanism, initially carrying only: `src/game/malloc.c`'s four `mflr` blocks
   → `__builtin_return_address(0)`; `src/game/hsfmotion.c`'s two
   `__declspec(weak)` → nothing; the `HuPrcCreate` default stack size.
3. `port/include/port_override.h`, force-included into every game TU: the
   `OSf32tos*` overrides, `ATTRIBUTE_ALIGN`, `OSCachedToUncached` as identity,
   and `#pragma dont_inline` silenced.
4. `port/gen/sdk_stubs.c` — one loud stub per SDK symbol the game side calls,
   so the link cannot fail for a symbol we forgot. Drive the generator from
   `python3 port/tools/inventory.py --symbols`, which prints a TSV of
   `family, symbol, game_sites, rel_sites` (520 rows today), joined against the
   declarations in `include/dolphin/*.h` for the signatures.
5. `port/src/os/os_report.c`, `os_heap.c`, `os_time.c`, `os_misc.c`.
6. `port/src/os/jmp_ppc_darwin.s`.
7. `port/src/os/psmtx_c.c`.
8. `port/src/dvd/dvd_fs.c` + a `port/tools/extract_disc.py` that produces the
   `files/` tree (or reuse the decomp's existing extraction under
   `orig/GMPE01_01/`, which already has `files/dll/`).
9. `port/src/platform/main.c` — argv, then call the game's `main`.

Rough size: 2,500–3,500 lines of new code, most of it mechanical, plus the
generated stub file.

---

## 7. Licences, and what may be copied

Cloned read-only into `/Users/zachjack/Apps/ref/`.

| thing | licence | how we use it |
|---|---|---|
| `mariopartyrd/marioparty4` (this repo) | the repo's own terms; contains no game assets and no assembly | we compile it, unmodified |
| `extern/musyx` (AxioDL) | **MIT** — `extern/musyx/LICENSE`, © 2023 Axiomatic Data Laboratories | compiled above the SAL, unmodified |
| `mariopartyrd/partyboard` | **NO LICENSE FILE.** There is no `LICENSE`, `COPYING` or licence statement anywhere in the repository. It is therefore all-rights-reserved by default. | **Read for design; copy nothing.** Every idea taken from it in this plan is re-derived and re-implemented here. Where its own files credit third parties (`src/port/stubs.c` and `src/port/dvd.c` say "Credits: Super Monkey Ball"; `src/port/portmain.cpp` and `src/port/io.cpp` say "Credits: TwilitRealm"), that provenance is a further reason not to lift text. |
| `encounter/aurora` (partyboard's GX backend) | **MIT** — `extern/aurora/LICENSE`, © 2022 Luke Street | irrelevant to us in practice: it is WebGPU/WGSL with programmable vertex pulling and cannot run on a Radeon 9000 or build for `powerpc-apple-darwin8`. Useful only as a second opinion on GX *semantics*. |
| `doldecomp/dolsdk2001` | **NO LICENSE FILE**, no copyright statement in the README. Same posture as any decompilation. | reference; and see below for the handful of files worth compiling, which we would re-derive rather than vendor. |
| `higan-emu/libco` | ISC | not needed — see §2.2 |
| Dolphin emulator | **GPLv2+** | used as a *runtime* reference only. **No Dolphin source is read for implementation and none is copied.** GX semantics come from `src/dolphin/gx/` in this repository and from the SDK headers in `include/dolphin/`. |
| SDL2 | zlib | statically linked, Tiger build |
| the game's data | the user's own disc | never redistributed; bring-your-own-disc, as in the Snowboard Kids ports |

## 8. What the reference repositories actually told us

### 8.1 `mariopartyrd/partyboard`

A **hard fork of this decomp**, not a separate port repo: it keeps
`configure.py`, `config/`, `orig/` and the matching ninja build, and adds a
parallel CMake build alongside. HEAD at survey time was `9f60742`. One
pre-release, *Party Board Alpha 0.2.0*, 2026-05-08. A snapshot of an earlier
state of the same work also lives in this repository on the `origin/port`
branch, whose README now just says the port moved.

What it confirms, and what it warns about:

- **REL loading is solved the way §2.4 recommends.** 96 REL directories → 96
  native shared libraries, one exported symbol each, `dlopen`/`dlsym`, main
  binary as a shared library with a 1,106-symbol export list. This is the
  single most valuable thing the survey established: the approach is not
  speculative, it ships.
- **The graphics layer is unusable for us and there is no partial fallback.**
  Aurora is ~19,200 lines of GX-relevant code targeting **WebGPU via Dawn**,
  generating **WGSL shader strings at runtime** and fetching vertices by
  *programmable vertex pulling* out of storage buffers. Its "OpenGL backend" is
  Dawn's, which needs desktop **GL 4.4**. There is no fixed-function path
  anywhere in it. Our `port/src/gx/` is necessarily ours.
- **But one architectural idea in it is exactly right and worth re-deriving:**
  `GXBeginDisplayList` redirects FIFO writes into the caller's buffer and
  `GXCallDisplayList` writes the recorded bytes back into the FIFO, so the
  display list is replayed through the same command decoder as everything else
  (`extern/aurora/lib/dolphin/gx/GXDispList.cpp`, 88 lines). That is precisely
  the design §3.3 arrived at independently, and it is API-agnostic — it is
  worth doing even though we decode into GL rather than WebGPU.
- **Audio is not implemented there either.** MusyX is linked with
  `MUSY_TARGET_PC`, whose `hw_pc.c` opens with `// TODO: Finish implementation`
  and has its `AIInitDMA` commented out; `src/msm/*` is **not compiled at all**;
  `src/port/audio.c` has `msmSysInit` and 31 other `msm*` calls commented out;
  `src/port/stubs.c` no-ops the rest. Open issue #14 is "No Audio". So §1.8's
  DSP-command interpreter is genuinely new work with no upstream to lean on —
  and it is also the single biggest thing this port could contribute back.
- **The endianness dividend is measurable.** Their `src/port/byteswap.cpp` is
  1,087 lines defining shadow "32b" struct layouts (`AnimData32b`,
  `AnimBmpData32b`, `HsfCluster32b`, …) and swapping HSF and animation data in
  place, plus 119 `BYTESWAPPING` sites through the game sources. On a
  big-endian G4 all of that is deleted.
- **The pointer-width dividend is real too, in the other direction.** They had
  to change `u32` → `uintptr_t`/`size_t` across `src/game/memory.c`,
  `hsfdraw.c` and others for 64-bit hosts. We are 32-bit, so those changes are
  unnecessary — which matters, because it means our `patches.txt` stays short
  and upstream merges stay cheap.
- **A warning about method.** They edited the decomp's sources in place and
  reformatted them: the diff against this repo's `main` is 22,903 lines over
  `src/game` alone, most of it clang-format noise, plus 240 `#ifdef TARGET_PC`
  sites. That is exactly the trap the Snowboard Kids ports avoided with the
  mirror-plus-`patches.txt` design, and it is why §6.2 keeps that design here.
- **Their open issues are a preview of ours**: #16 THP movies do not play,
  #32 GCI saves do not work, #34/#31/#30/#29/#33 assorted per-minigame
  rendering bugs, #18/#7 PAL incomplete.
- One detail to steal outright in spirit: `src/port/dolassets.cpp` extracts
  data blobs the game expects to find inside the DOL (the `ANK8X8_4B` and
  `ASCII8X8_1BPP` fonts, `HILITEDATA`, `REFMAPDATA0-4`, `TOONMAPDATA`, the
  localised error screens) out of the user's own DOL by address, since the
  repository ships no assets. We need the same trick, and
  `config/GMPE01_01/symbols.txt` gives us the addresses.

### 8.2 `doldecomp/dolsdk2001`

A decompilation of the 22 May 2001 SDK, ~44,000 lines over 32 module
directories. This repository already carries its own SDK copy under
`src/dolphin/`, matched against *this* game, so `src/dolphin/` stays the primary
source and `dolsdk2001` is the cross-check. What the survey pinned down:

- **`src/os/OSLink.c` — 378 lines, pure C, no MMIO.** Real REL relocation is
  portable software. This is what makes option (d) in §2.4 worth reasoning
  about at all rather than dismissing.
- **MTX has a complete pure-C fallback, selected by a header switch.**
  `include/dolphin/mtx.h` maps the plain `MTX*`/`VEC*` names to `C_*` under
  `#ifdef DEBUG` and to `PS*` otherwise; both sets coexist under distinct
  symbol names. *(This repository's own `include/dolphin/mtx.h` uses the
  cleaner `MTX_USE_C` / `MTX_USE_PS` pair instead — §1.12 — so we get the same
  result from `-DMTX_USE_C` with no header patch.)* Per-file:
  `mtx44.c` and `mtxstack.c` are **zero** paired-single hits; `mtx.c`, `vec.c`
  and `mtxvec.c` are mixed files where the `C_*` bodies are plain C and the
  `PS*` bodies are Metrowerks `asm` blocks; and everything in `mtx.c` from
  `MTXRotRad` onward (`MTXRotTrig`, `MTXRotAxisRad`, `MTXTrans`, `MTXScale`,
  `MTXQuat`, `MTXReflect`, `MTXLookAt`, `MTXLightFrustum`,
  `MTXLightPerspective`, `MTXLightOrtho`) has **no PS variant at all** and
  ports free.
- **`src/mtx/psmtx.c` is 100% paired-single with no C equivalents**, five
  functions: `PSMTXReorder`, `PSMTXROMultVecArray`, `PSMTXROSkin2VecArray`,
  `PSMTXROMultS16VecArray`, `PSMTXMultS16VecArray` — the reordered-matrix
  skinning fast paths. **Mario Party 4 calls two of them**: `PSMTXReorder`
  (19 sites) and `PSMTXROMultVecArray` (18 sites). Those two must be
  hand-written in C for `port/src/os/psmtx_c.c`; the rest of the `PSMTX*`
  surface is a forwarder to an existing `C_MTX*`. That is a small, exactly
  known piece of work, and both are good later AltiVec candidates.
  Aurora, for reference, simply omits `psmtx.c` from its build entirely.
- **No AltiVec anywhere** in any of these SDK decompilations. Any AltiVec in
  this port is ours to write.
- **Other files that are pure software and could be compiled directly** if we
  ever want a second opinion on a format: `src/dvd/dvdfs.c` (645 lines, FST
  parsing, zero hardware references), 12 of the 17 `src/card` files (~1,690
  lines of block/directory/BAT/checksum logic — everything except
  `CARDBios.c`, `CARDMount.c`, `CARDCreate.c`, `CARDRename.c`, `CARDUnlock.c`,
  which touch EXI), `src/os/OSAlloc.c` (606 lines), `src/os/OSFont.c`,
  `src/axfx`, `src/syn`, `src/seq`, `src/mix`, `src/G2D`, `src/texPalette`,
  `src/dolformat`, `src/fileCache`. All of it is Metrowerks-syntax C, so the
  inline-asm files will not compile with GCC, but the listed ones have none.
- **`src/gx/` (9,327 lines) is hardware-only and not compilable** — 15 of its
  21 files write GX registers through the write-gather pipe. It is, however,
  the authoritative statement of *which BP/CP/XF register each GX call
  writes*, which is exactly the table a fixed-function backend has to invert.
  Read it alongside our own `src/dolphin/gx/`.


---

## 9. M1 log — it links and it talks *(done, 2026-09-13)*

Both done-means are met, and the boot goes further than the milestone asked:
past `objdll>Link DLL:dll/bootdll.rel` and on into the game's own frame loop,
which issues real GX draw calls before it runs out of things to do without a
REL.

### 9.1 The two binaries

| binary | how | size |
|---|---|---:|
| `port/build-ppc-darwin/marioparty4` | `port/build-ppc.sh -j8` -- GCC 14.2, `powerpc-apple-darwin8`, MacOSX10.4u SDK, `-malign-natural -mone-byte-bool` | 1,093,916 bytes |
| `port/build-host/marioparty4` | `make -C port TARGET=host -j8` -- the Mac's clang, arm64 | 1,079,192 bytes |

93 game translation units (50 `src/game`, 30 `src/game/board`, 6 `src/msm`,
2 `src/libhu`, 5 `src/dolphin/mtx`), 12 port sources, one assembly file, and
one generated stub pair. Both targets stub **exactly the same 188 symbols** —
the two `missing.txt` files are identical, which is the cheapest possible check
that the host build is not diverging from the real one.

The G4 was not available on the day, so the PowerPC binary is verified only to
compile and link clean; everything below was run on the host build.

### 9.2 The disc: the NKit-trimmed ISO is read in place

Checked before writing a line of the DVD layer, because the plan left it open.
`orig/GMPE01_01/Mario Party 4 (USA) (Rev 1).nkit.iso` is 0x23AA9800 bytes
against a 1.36 GB full disc, so NKit has removed the trailing junk -- but it has
**relocated nothing**. The FST is byte-identical to `dtk vfs cp sys/fst.bin` at
the raw offset the header names (0x160100), and seven files read straight out of
the image at their FST offsets -- the first, the last, the highest-offset one
(`sound/mpgcstr.pdt` at 0x22FD0080) and four at random -- are byte-identical to
`dtk vfs cp`. The highest byte any file reaches, 0x23AA95B8, is inside the
image. So `port/src/dvd/dvd_fs.c` parses the FST at boot and reads the image
directly: nothing to install, and the 357 files are exactly the ones the disc
has.

The extracted-`files/`-tree fallback works too and is chosen automatically when
`--image` names a directory. Pointing it at this repository's `orig/GMPE01_01`,
which holds only `files/dll/`, produces exactly the right failure --
`data.c: Data File Error(data/E3setup.bin)` and `OSPanic in "data.c" on line
65` -- which is the check that the FST is real rather than permissive.

### 9.2 The boot narration

Captured in full in [`m1-boot.log`](m1-boot.log) (202 lines, with `--verbose` so
each stub is marked where it is first reached). The shape of it:

```
port> OSInit: MEM1 24 MB, ARAM 16 MB, bus 162 MHz
port> DVDInit / VIInit / OSCreateHeap 0: ... (23375 KB) / VIConfigure: 640x480
port> OSCreateThread ... (the soft-reset watcher; not started)
HuMem> left memory space 2383KB(2441120)
port> ARInit: 16 MB of ARAM
DLL DBG OUT
objman>Call New Ovl 1(1)
++++++++++++++++++++ Start New OVL 1 (EVT:0 STAT:0x00000000) +++++++++++++++++
======== HuMem heap dump 059a0020 ========          <- three real heap dumps
objman>Used Memory Size:00012200
objman>Init esp
objman>Call objectsetup
DLLStart 1 0
Search:dll/bootdll.rel
objdll>Link DLL:dll/bootdll.rel                     <- M1's target line
Rest Memory 580000
port> OSLink(...): REL relocation is M2; returning FALSE
objdll>++++++++++++++++ DLL Link Failed
objman>ObjectSetup end
*** port: watchdog fired: ... omWatchOverlayProc
```

Two things worth reading twice. `HuMem> left memory space 2383KB(2441120)` and
`Rest Memory 580000` are the game's *own* accounting of the arena it was given,
so the 24 MB MEM1 and the OSAlloc heap underneath it are behaving. And
`bootDll.rel` is genuinely read off the disc image — 30.6 KB through the FST —
before `OSLink` declines it.

Where it stops is not a crash but a spin, and it is the right one:
`omWatchOverlayProc` reaches `HuPrcChildWatch()`, finds no child object because
`bootDll` never ran, and loops without yielding. That loop is exactly what M2
removes. `--watchdog N` exists for it: it prints the program counter to look up
with `atos` and the stub table, and exits.

### 9.3 The SDK surface the boot actually hit

56 distinct stubs, 111 calls, in first-call order -- 55 of them GX, which is the
best possible argument for the GX plan (§3) being the next milestone rather
than anything else:

```
   1  GXInit                                  1
   2  GXSetViewport                           6
   3  GXSetScissor                            4
   4  GXSetDispCopySrc                        1
   5  GXSetDispCopyDst                        1
   6  GXSetDispCopyYScale                     1
   7  GXSetCopyFilter                         1
   8  GXSetPixelFmt                           1
   9  GXCopyDisp                              2
  10  GXSetDispCopyGamma                      1
  11  sndIsInstalled                          1
  12  GXSetFog                                1
  13  GXSetDrawSyncCallback                   1
  14  GXInvalidateVtxCache                    2
  15  GXInvalidateTexAll                      4
  16  GXSetGPMetric                           2
  17  GXClearGPMetric                         2
  18  GXSetVCacheMetric                       2
  19  GXClearVCacheMetric                     2
  20  GXClearPixMetric                        2
  21  GXClearMemMetric                        2
  22  GXSetCopyClear                          2
  23  GXSetDrawSync                           2
  24  GXSetCurrentMtx                         3
  25  GXSetProjection                         3
  26  GXClearVtxDesc                          3
  27  GXSetVtxDesc                            6
  28  GXSetVtxAttrFmt                         6
  29  GXSetCullMode                           1
  30  GXSetZMode                              4
  31  GXLoadPosMtxImm                         2
  32  GXSetChanMatColor                       2
  33  GXSetNumChans                           2
  34  GXSetChanCtrl                           2
  35  GXSetTevOrder                           2
  36  GXSetTevOp                              2
  37  GXSetNumTexGens                         2
  38  GXSetNumTevStages                       2
  39  GXSetAlphaUpdate                        2
  40  GXSetColorUpdate                        2
  41  GXSetAlphaCompare                       2
  42  GXSetBlendMode                          2
  43  GXBegin                                 1
  44  GXPosition2u16                          4
  45  GXEnd                                   1
  46  GXSetArray                              1
  47  GXInitTexObj                            1
  48  GXInitTexObjLOD                         1
  49  GXLoadTexObj                            1
  50  GXSetTexCoordGen2                       1
  51  GXSetZCompLoc                           1
  52  GXDrawDone                              1
  53  GXReadGPMetric                          1
  54  GXReadVCacheMetric                      1
  55  GXReadPixMetric                         1
  56  GXReadMemMetric                         1
```

Everything not in that list is implemented for real: OSReport, the arena and
OSAlloc heaps, time and the stopwatch, interrupts and caches as no-ops, the one
OSThread, the DVD file system, ARAM and ARQ, VI, `gcsetjmp`/`gclongjmp`, and
the `PSMTX*`/`C_VEC*` surface.

Note that the HUPROCESS coroutines are proven working by this log, on arm64 as
well as in principle on PowerPC: `Start New OVL 1` is printed *after*
`HuPrcSleep(0)`, so a full `gclongjmp` out to the scheduler and back happened
before it.

### 9.4 patches.txt — 18 entries, four of them predicted

The plan predicted three (`malloc.c`'s `mflr`, `hsfmotion.c`'s `__declspec`, the
HUPROCESS stack size) and all three were needed. The rest divide cleanly:

| patch | why |
|---|---|
| `malloc.c` x4 `mflr retaddr` → `__builtin_return_address(0)` | predicted |
| `hsfmotion.c` x2 `__declspec(weak)` → `__attribute__((weak))` | predicted |
| `process.c` stack multiplier + a 64-byte guard on `base_sp` | predicted. `base_sp` left only 8 bytes above the allocation, which is enough for a Metrowerks EABI prologue and not for a Darwin one (PowerPC Darwin stores LR at `8(r1)` of the *caller's* frame), and arm64 additionally wants 16-byte alignment |
| `fault.c` `OSPanic` → `HuFaultPanicScreen` | new. fault.c defines `OSPanic` itself, draws it into the external framebuffer and `PPCHalt`s; the port wants it on stdout where M1 can read it |
| `objdll.c` failure path returns instead of falling through | new. The loader reports "DLL Link Failed" and then calls `module->prolog` anyway, at whatever un-relocated garbage it holds. This is also the exact seam M2 replaces with `dlopen` |
| `memory.c`, `init.c` x5, `dolphin/os.h` x3 | new. Pointer arithmetic done through `u32` — `OSRoundUp32B((u32)arena_lo)` and friends. Identical on the G4, a truncation on a 64-bit host |
| `include/dolphin/gx/GXGeometry.h` | new. The decomp's own `TARGET_PC` build of `GXSetArray` takes an extra `size` argument that the game's 159 call sites do not pass |
| `audio.c`, **host only** | new, and the interesting one — see §9.6 |

The mirror also does two things automatically, so they are not patches:

- **it drops every top-level definition containing Metrowerks assembly**, and
  says which: 29 in the MTX sources (`PSMTXConcat`, `PSVECNormalize`, …),
  `gcsetjmp`/`gclongjmp` in `jmp.c`, and `_kerent` in `kerent.c`. Dropping the
  *whole function* matters: stripping only the `asm { }` out of a
  `PSMTXIdentity` written as C-with-an-asm-body would leave behind an identity
  that identities nothing. If the port forgets to supply one, the link fails.
- **it removes `inline` from the 11 column-0 `inline` definitions** in
  `hsfman.c` and `hsfdraw.c` (`Hu3DLightCreateV`, `SetupGX`, …). Metrowerks
  emitted an out-of-line copy of those and other translation units call them;
  C99 `inline` emits nothing.

### 9.5 Darwin ABI issues actually met

- **`gcsetjmp` / `gclongjmp` port verbatim to PowerPC Darwin.** r13-r31 are
  non-volatile exactly as in EABI and r2 is volatile, so saving it is harmless.
  Both functions must stay leaf and frameless so the sp they save is the
  caller's — which is what `HuPrcCreate` assumes when it overwrites `lr` and
  `sp` by hand.
- **`__OSBusClock` / `__OSCoreClock` are tentative definitions** in every
  translation unit that includes `<dolphin/os.h>` (on the console they are
  fixed addresses in low memory). `-fcommon` on the game sources, filled in
  from `OSInit`.
- **`clock_gettime` is not in the 10.4 SDK** — it arrived in 10.12.
  `mach_absolute_time` is the same call on both targets;
  `port/src/platform/clock.c`.
- **`-DTARGET_PC` is required, not optional.** It is the decomp's own porting
  switch and it does three things the port needs: makes the fixed-width types
  actually fixed width (`u32` is `unsigned long` otherwise — 64 bits on the
  host), turns the `GXPosition*`/`GXColor*` vertex writers from write-gather
  pipe stores at `0xCC008000` into ordinary function calls, and widens a few
  parameters to `const void*`.
- **The decomp ships Metrowerks C-library headers** (`include/string.h` and
  friends, whose prototypes do not match a real libc — `strcat` takes three
  arguments there). The mirror leaves them out so the host's own are found.
- **`-std=gnu11`, not gnu99**: MusyX's `musyx.h` typedefs `bool` as
  `unsigned long` when `__STDC_VERSION__ <= 199901L`, which collides with
  `<stdbool.h>` that `dolphin/types.h` has already pulled in.

### 9.6 The one real host-only divergence: endianness

The GameCube, the G4 and the disc are all big endian, which is the port's
largest single structural dividend (§2.6, and partyboard's 1,087-line
`byteswap.cpp` that we do not need). The development host is little endian, so
**the game's own file parsers read byte-swapped values there**. It shows up at
the first parse: `msmSysInit` reads version `0x02000000` out of
`sound/mpgcsnd.msm`, returns `MSM_ERR_INVALIDFILE`, and `HuAudInit` spins in
`while (1)`. One `host:`-prefixed patch declines to hang there.

It is also visible, harmlessly and rather usefully, in the REL module dump in
the boot log: `version:0x4d7a0000` is `0x00007a4d` byte-swapped.

The consequence to plan around: **the host build is a development
convenience for the port layer, not a second reference implementation.**
Anything downstream of a disc-data parse — HSF models, the MSM sound bank, REL
relocation — is only correct on the PowerPC target. Two ways out, both M2 or
later: byteswap on read in the DVD layer for the host only (partyboard's
route), or accept the host build as a boot/plumbing harness and do all visual
comparison against the G4. The second is cheaper and matches how the two
Snowboard Kids ports were tested; decide at M2.

### 9.7 Two more things found, both deferred

- **`window.h`'s `MAKE_MESSID_PTR(ptr)` packs either a message id or a string
  pointer into a `u32`.** 28 call sites. On the G4 that is fine. On a 64-bit
  host it truncates, and unlike the other 234 pointer-through-`u32` casts (all
  widened automatically by `tools/widen_ptr_casts.py`, which asks the compiler
  which casts are pointer casts rather than guessing) this one cannot be fixed
  at the call site — the parameter type would have to change. Nothing in the M1
  boot reaches it. M3, with the window system.
- **`kerent.c`'s `_kerent`** is a 2,047-line Metrowerks `asm` trampoline table:
  `entry _kerjmp_X` / `b X`, the DOL's export thunks that RELs branch through.
  The mirror drops it and nothing in the DOL misses it. M2 needs it back, and
  it is about thirty lines of generator: each pair becomes
  `.globl __kerjmp_X ; __kerjmp_X: b _X`, which assembles identically on
  PowerPC and arm64.

### 9.8 Open for M2 *(all five answered in §10)*

1. **REL loading by `dlopen`** (§2.4b): one Mach-O bundle per REL, the
   `omDLLLink` patch seam is already cut, and the "load and unload all 99
   bundles twice" smoke test still has to be written.
2. **The first frame**: 55 GX entry points are already exercised before the
   port runs out of REL, and the list in §9.3 is the order to implement them in.
3. `kerent.c`'s trampolines, as above.
4. The soft-reset thread currently never runs. It blocks on `OSSleepThread`
   immediately, so this is behaviour-preserving until something posts to its
   queue; the host loop should poll it once per retrace.

---

## 10. M2a log — REL modules, the trampoline table, the reset poll, and the first slice of GX *(2026-09-13)*

M2's done-means is "the Hudson and Nintendo logos render, and a screenshot
from the Mac and one from the G4 are the same picture". This is the first
half of it, developed and tested on the Mac with the G4 unavailable. Four of
the five open items from §9.8 are closed; the fifth — the logos themselves —
turns out to be blocked on the host for a reason worth stating carefully, and
that is §10.6.

### 10.1 REL loading: all 99, twice, clean

Every one of the 99 relocatable modules is now a Mach-O bundle beside the
binary, built from the same mirror as the DOL:

| target | what | size |
|---|---|---:|
| `port/build-host/rels/*.dylib` | 99 `MH_BUNDLE`, arm64 | 9.0 MB |
| `port/build-ppc-darwin/rels/*.bundle` | 99 `MH_BUNDLE`, `powerpc` | 6.1 MB |

`port/tools/gen_rels.py` reads which translation units belong to which module
out of the decomp's own `config/GMPE01_00/rels/<mod>/splits.txt`, so the port
cannot drift from an upstream re-split. Three details the splits files do not
answer, answered by inspection:

- **18 modules do not list `REL/executor.c`.** Three of them (`mentDll`,
  `mstory4Dll`, `safDll`) wrote `_prolog`/`_epilog` out into their own source;
  the other 15 have the same 0xA0 bytes inside their own `.text` range where
  the split never separated it. The generator gives the 15 the shared
  `executor.c`, or `board_executor.c` when the module defines `BoardCreate`.
- **Six modules are 144-byte placeholder RELs** (`m300Dll`, `m302Dll`,
  `m303Dll`, `m330Dll`, `m333Dll`, `msetupDll`) with nothing but empty
  `.ctors`/`.dtors` — no code, no prolog. They get
  `port/src/relmod/rel_placeholder.c`, whose prolog returns 0, so all 99 load
  through one path.
- **`safDll` has no `ObjectSetup` anywhere.** A weak one in
  `rel_runtime.c` supplies the symbol and says so if it is ever entered.

Each bundle exports exactly three symbols — `_prolog`, `_epilog`,
`_unresolved` — through `resources/rel_exports.txt`, which makes everything
else `private_extern`. That is what dissolves the 899 names that collide
between modules (`ObjectSetup` in 90 of them, `_ctors`/`_dtors` in all 99)
without renaming a single one. The main binary is linked with
`-exported_symbols_list` over **3,080 symbols**, generated from the link's own
`nm` output rather than kept by hand as partyboard's 1,106-entry `dol.def` is;
each bundle is linked `-bundle -bundle_loader marioparty4`, so a module that
imports something the port has not implemented is a **build** error rather
than a crash.

**`dlclose` really unloads, on this host.** `--reltest` loads and unloads all
99 twice and asks dyld after every single unload whether the image is gone:

```
port> --reltest: 99 module bundles, two load/unload rounds each
port> --reltest: 198/198 load+unload cycles clean, 0 failed,
                 0 missing an entry point, 0 still resident after dlclose
```

That closes §4 risk 2 for the host and leaves it open only for Mac OS X 10.5,
where the same check runs on every unload at run time. The fallback is written
and testable now with `--relzerobss`: find the loaded image through the
module's own `_prolog` address (`dladdr` once per load, never in a hot path)
and zero its `__DATA,__bss` and `__DATA,__common` sections by hand, which is
exactly what `omDLLStart`'s re-entry branch used to do with
`memset(dll->bss, 0, module->bssSize)`.

`objdll.c` keeps its shape. Six exact-text patches replace `OSLink` with
`portDLLOpen`, `OSUnlink` with `portDLLClose`, the two prolog calls with
`portDLLProlog` and the re-entry `memset` with `portDLLReenter` — and the
module header is still read off the real disc, so `omDLLInfoDump` and
`omDLLHeaderDump` still narrate the true REL. The `dlopen` handle lives in
`omDllData.bss`, which held the module's bss block on the console and which
nothing outside the loader ever dereferences.

**`bootDll` runs.** `objdll> dll/bootdll.rel prolog start` /
`******* Boot ObjectSetup *********` / `prolog end` — the module's own code,
compiled from the decomp, executing inside the port.

### 10.2 The nine Metrowerks-isms in `src/REL`

Compiling 254 REL translation units with clang and GCC 14 found nine places
the Metrowerks compiler accepted and neither of ours will. None is a port
decision; each is rewritten to the meaning the compiled REL plainly has, as
exact text in `patches.txt` so an upstream fix breaks the build rather than
double-applying:

| what | where |
|---|---|
| chained lvalue casts, `var_r31 = (Vec*)*arg0 = malloc(...)` | `m438Dll/fire.c` ×3 |
| a five-way chained assignment through `(void*)` casts | `w06Dll/main.c` |
| a struct passed by value to `memset` (the decomp's own `NON_MATCHING` branch passes its address) | `m406Dll/map.c`, `m413Dll/main.c` |
| a call with too few arguments (the decomp comments one of them `// Bug:`) | `m404Dll/main.c`, `m447dll/main.c` |
| `static` definition of a function already declared `extern` in the same file | `w01Dll/main.c` |

Two mirror-level changes went with them: `src/REL/**` joined the mirror, and a
column-0 `inline` definition in a REL becomes a **weak** definition rather than
a plain external one, because the same helper is sometimes written out in two
translation units of the *same* module (`fabs2` in `m430Dll`'s `player.c` and
`water.c`).

One header patch was needed for the bundles rather than for the DOL:
`u32 __OSBusClock AT_ADDRESS(...)` in `<dolphin/os.h>` is a tentative
definition in every translation unit that includes it, and a bundle that
merged its own copy would read zero for `OS_BUS_CLOCK` — which five modules,
`bootDll` among them, use. The declarations become `extern` and the port owns
the one definition.

### 10.3 `kerent.c`'s trampoline table

`port/tools/gen_kerent.py` regenerates the 2,047-line Metrowerks `asm`
function as **1,011** entries of

```
	.globl __kerjmp_OSReport
__kerjmp_OSReport:
	b _OSReport
```

which assembles identically for arm64 and for 32-bit PowerPC. Six entries are
skipped: the `_savegpr_14/15/16` and `_restgpr_14/15/16` Metrowerks EABI
register-save helpers, which have no GCC/clang equivalent and which no REL
imports through the table. 35 of the 1,011 targets are not defined by the game
or the port; 29 of those are SDK symbols the stub generator picks up
automatically (the table's object is part of the undefined-symbol scan), and
the rest are libm.

The table is not *needed*: with one bundle per REL, dyld binds each module's
imports straight to the real symbols. It is regenerated because `_kerent` is
in `config.yml`'s `force_active` list and is therefore part of what the DOL
is, because it is the authoritative statement of which 1,011 symbols the DOL
exports to modules, and because the single-binary fallback of §2.4a would need
exactly this table.

### 10.4 The soft-reset watcher, polled

`sreset.c`'s `ToeThreadFunc` is `while (1) { OSSleepThread(...); <body> }`,
woken once per field by `HuDvdErrDispIntFunc`, which the game installs as VI's
pre-retrace callback. The obvious way to run that as a callback is a second
stack and a context switch. It does not need one: **the loop body carries no
state between iterations** — its only local is assigned before it is read — so
re-entering the function from the top once per retrace is indistinguishable
from letting it come round the loop. The port makes the *first*
`OSSleepThread` of each tick return normally, so the body runs, and the
*second* one (the loop coming back around) `longjmp` out. One `setjmp` per
retrace, no second stack, nothing that behaves differently on PowerPC.

Measured over a 40-frame boot: `soft-reset watcher: running, body polled 36
times` — one per retrace from the frame the game installs the callback.

The reset button is a **pulse**, not a level: `OSGetResetButtonState` returns
TRUE for three polls and then FALSE, because `ToeThreadFunc` sets
`H_ResetReady` on the press and only acts on the release. Ctrl-C, the window's
close button and Escape all request it; a second Ctrl-C leaves immediately.
The quit then goes out through the game's own `HuSoftResetPostProc` /
`HuRestartSystem` / `OSResetSystem`, and `port_shutdown` is the single exit
path so `--frames`, a reset and a normal return all report the same things in
the same order.

### 10.5 The GX slice

`port/src/gx/` is 5 files and about 2,300 lines, and it implements **all 114**
GX entry points the game's link needs — the boot's 55 (§9.3) and the 59 more
the RELs pull in. The stub table is down from 188 symbols to 95, and
**the host and PowerPC builds stub exactly the same 95**, which remains the
cheapest check that the two are not diverging.

| file | what |
|---|---|
| `gx_state.c` | the state entry points. Matrices (`GXSetProjection` keeps the six elements GX keeps, per the decomp's own `GXTransform.c`, not the whole 4×4), viewport and scissor with GX's top-down y flipped, cull with GX's inverted winding, z, blend, alpha compare, fog, TEV state, channels, lights, copies |
| `gx_draw.c` | vertex assembly in descriptor order; direct and indexed attributes decoded against the VAT's component count, type and fractional shift; **CPU transform and CPU per-vertex lighting**; texgen; display lists |
| `gx_tex.c` | all ten formats de-tiled and expanded to RGBA8, with a **content-keyed** cache |
| `gx_tev.c` | the TEV chain compiled into a GL 1.3 texture-environment chain |
| `gl13.c` | the only file that touches GL, plus the SDL2 window, `--glcheck` and the PPM writer |

Three decisions worth recording:

- **Lighting and the modelview are done on the CPU.** GL's modelview stays
  identity and `gx_draw.c` transforms positions by the loaded position matrix
  and normals by the loaded *normal* matrix. That is not laziness: the game
  loads normal matrices that are not the inverse transpose of the position
  matrix, and GL's fixed function has no way to say so. Lighting followed for
  the same reason — GX's `GX_DF_CLAMP`/`GX_DF_SIGN` diffuse functions and its
  ratio-of-quadratics attenuation are not GL's, and a party game with a
  handful of lights can afford the C.
- **Display lists are recorded in the real GX byte encoding** — one opcode
  byte with the vertex format in its low three bits, a big-endian `u16` count,
  packed attributes in descriptor order, padded to 32 bytes. The demo's
  eight-index strip records as 19 bytes padded to 32, and replays. Byte-exact
  sizes are what keep the game's own `dlSize` accounting (a 0x20000 cap in
  `hsfdraw.c`) from overrunning a buffer it sized for the console.
- **The TEV compiler recognises five shapes**, which cover the GX lerp
  `out = d + a*(1-c) + b*c` exactly: `REPLACE`, `MODULATE`, `ADD`,
  `INTERPOLATE`, and `MODULATE_ADD_ATI`. Anything outside them — a
  four-input stage with no combiner, a `±0.5` bias, `GX_CS_DIVIDE_2`, a
  non-identity swap table, an indirect stage — is counted and named once each
  by `--gxwarn` rather than drawn silently wrong.

`--glcheck` validates every GL entry point the backend calls against a written
list of GL 1.3 plus the four named extensions; the list is maintained by hand
because adding a GL call without adding it to the list is precisely the
mistake worth catching. The extension flags are answered from the **card's**
feature set (six texture units, not the host's eight) so the paths the G4 will
take are the paths exercised on the Mac.

**`--gxdemo` is how the slice is verified.** It drives the same public GX
entry points the game calls with data the port builds in memory and writes the
frame as a PPM: direct `GX_QUADS` with `GXPosition3f32`/`GXColor4u8`, alpha
blending, an indexed `GX_TRIANGLESTRIP` through `GXSetArray` +
`GXPosition1x16` with an S16 array and a fractional shift, recorded into a
display list and replayed, a two-stage konst-modulated chain, and one quad in
each of RGB565, RGB5A3, RGBA8, I8, I4, IA8, CMPR and C8-through-a-TLUT. The
result is [`m2a-gxdemo.png`](m2a-gxdemo.png), and it is correct: the tiling,
the fractional fixed point, the TLUT, the CMPR blocks, the blend and the
display-list round trip all come out right.

### 10.6 The host cannot render the logos, and the reason is not (only) endianness

§9.6 predicted that the little-endian host would stop at the first parse of
disc data and left the decision of what to do about it to M2. M2 measured it,
and the answer is larger than endianness.

The game's on-disc structures contain **32-bit fields that its own headers
declare as pointers**. `ANIMDATA`, the sprite bank, is the smallest example:

```c
typedef struct AnimData_s {
    s16 bankNum, patNum, bmpNum, useNum;
    ANIMBANK *bank;      /* a 4-byte file offset, relocated in place */
    ANIMPAT  *pat;
    ANIMBMP  *bmp;
} ANIMDATA;              /* sizeof 0x14 */
```

and `HuSprAnimRead` fixes them up with
`bank = (ANIMBANK *)((u32)anim->bank + (u32)data)`. On the GameCube and on the
G4 that struct is 0x14 bytes and the arithmetic is exact. On a 64-bit host
each pointer field is eight bytes, the struct is 0x20, and every field after
`bankNum` lands on the wrong bytes — **before endianness is even
considered**. HSF models, animation banks and the message data all have the
same shape. partyboard needs 1,087 lines of shadow "32b" struct definitions
for exactly this; that work buys this port nothing, because the G4 is 32-bit
*and* big-endian and every one of these parses is correct as written there.

So the decision §9.6 left open is taken, with evidence: **the host build is a
plumbing harness, not a second reference implementation.** All host-only
divergences go through `port/src/dvd/host_data.c` and the four `host:` patches
that call into it, so the list is one grep long.

Where a byteswap-on-read shim is cheap *and* unambiguously correct, it is
done. There is exactly one so far and it is worth having: `GetFileInfo` in
`data.c` reads three big-endian `u32` **scalars** — a file's offset inside its
`data/*.bin` archive, its raw length and its compression type — and every data
file in the game passes through it. Those are not pointers, so nothing is
structural, and swapping them opens the whole decode path (`HuDecodeSlide` and
friends already read their own headers a byte at a time and are endian-clean
as written). `portBE32` is the identity on the G4 and the compiler deletes it.

### 10.7 host vs G4

What the development Mac can and cannot do, so the frame-comparison work knows
where it stands. Everything in the right-hand column is correct on the G4 with
no port code at all: it is 32-bit and big-endian, like the disc.

| step | host (arm64, 64-bit, little-endian) | G4 (PowerPC, 32-bit, big-endian) |
|---|---|---|
| the DVD file system and the FST | works — the FST is parsed byte-wise | works |
| REL bundles: load, prolog, epilog, unload, re-entry | works, 198/198 | expected to work; `--reltest` is the check to run first |
| the archive directory (`GetFileInfo`) | works, via the `portBE32` shim | native |
| file decompression (`HuDecodeSlide`/`Lz`/`Fslide`) | works — byte-wise headers | native |
| sprite banks (`ANIMDATA`/`ANIMBANK`/`ANIMPAT`/`ANIMBMP`) | **skipped** — 0x14 vs 0x20 struct, embedded 32-bit offsets | native |
| the Nintendo/Hudson logos (`nintendoData`, `TITLE_HUDSON_ANM`) | **skipped**, same reason | expected to work |
| message data (`messdata.c`'s bank tables) | **fails** — big-endian `u32` offsets read natively; this is what ends the host boot at frame 41 | native |
| HSF models (`hsfload.c`) | not reached yet; same struct-layout problem is expected | native |
| the MSM sound bank (`msmSysInit`) | fails; `--noaudio` carries on | native, but audio itself is M6 |
| THP movies | not started | not started |
| GX state, vertex decode, texture decode, TEV, display lists | works, verified by `--gxdemo` | expected to work; `--gxdemo` is the check to run first |
| vertex arrays through `GXSetArray` | read **big-endian**, which is right for the disc and for the G4 and wrong for arrays the game builds itself on the host | native either way |
| the frame loop, the retrace gate, the reset watcher | works | expected to work |

### 10.8 Where the host boot ends, exactly

40 frames, then a fault. In detail: the whole of `HuSysInit`, `omMasterInit`,
`bootDll.rel` loaded as a bundle and its `_prolog` run, `BootExec` created as a
`HUPROCESS`, the wipe running, 33 primitives and 132 vertices decoded and
drawn through the real GX path, the soft-reset watcher polled 36 times, and
two data files read off the disc image. Then, on frame 41, a fault inside
`_platform_memmove` reached from the message-data path, which reads
big-endian `u32` bank offsets natively (§10.7). It is host-only; there is
nothing to fix for the G4.

The frame the host does present is black, and correctly so — the wipe draws a
full-screen quad over sprites that were skipped. It is not comparable against
`port/ref/frames/boot-0001.png` and later, which show the Nintendo logo; that
comparison is a G4 job.

### 10.9 What the G4 session should do first

In order, because each one gates the next:

1. `port/build-ppc.sh -j8`, then copy `port/build-ppc-darwin/marioparty4` and
   the whole `port/build-ppc-darwin/rels/` directory (99 bundles, 6.1 MB) next
   to each other on the G4 — the loader looks for `rels/` beside the
   executable, or wherever `--reldir` says.
2. `./marioparty4 --reltest`. If any module reports "still resident after
   dlclose", note which and run the game with `--relzerobss`; that is the
   §4 risk-2 fallback and it is already written.
3. `./marioparty4 --gxdemo --shotdir .` and compare `gxdemo.ppm` against
   [`m2a-gxdemo.png`](m2a-gxdemo.png) by eye. This is the one test that
   separates "the GX layer is wrong" from "the Radeon 9000 is different", and
   it needs no disc.
4. `./marioparty4 --image <disc> --frames 400 --noaudio --gxwarn --dumpframe N`.
   On the G4 the sprite banks parse, so this is where the Nintendo and Hudson
   logos should appear. Compare against `port/ref/frames/boot-*.png` — the
   logos are frames 1–186 there, but wall-clock timed, so shoot a spread and
   find the matching pair once.
5. Whatever `--gxwarn` names, in the order of how much of the screen it covers.


---

## 11. G4 first run — the M1 binary on real PowerPC hardware

2026-09-13. Everything in §9 was measured on the little-endian development
host. This section is the same binary's first outing on the machine it is for:
the Power Mac G4, Mac OS X 10.5.4 (9E25), Radeon 9000, big endian. The whole
narration is in [`docs/g4-boot.log`](g4-boot.log), with the host/G4 diff and
the field-by-field REL header table at the bottom of it; this is the summary.

### 11.1 The workflow

Two scripts, both new, both modelled on the Snowboard Kids ports:

- `port/tools/make_bundle.sh` wraps `build-ppc-darwin/marioparty4` into
  `MarioParty4.app`. The executable inside is named **`isle`**, because the
  isle-ppc-tools console runner on the G4 hard-codes
  `~/isle.app/Contents/MacOS/isle` and `~/isle.app` is a symlink that
  `g4 use NAME.app` flips between projects. `--with-image` puts the disc image
  in `Contents/Resources` for a self-contained bundle.
- `port/tools/g4_install.sh` ships the bundle to `~/MarioParty4.app` (its own
  name, *not* the shared `~/isle.app` slot, which `g4 push` would overwrite and
  break for every other project) and, with `--image`, the 598 MB NKit ISO to
  `~/MarioParty4/mp4.nkit.iso` once.

`main()` gained a disc search so a bundle the runner launches with a fixed
argument line can find its own image: `$MARIOPARTY4_IMAGE`, then the .app's
`Contents/Resources`, then `~/MarioParty4`, first `*.iso` or `files/` wins.
`--image` still overrides everything.

    port/build-ppc.sh -j8
    port/tools/make_bundle.sh
    port/tools/g4_install.sh          # add --image the first time
    g4 use MarioParty4.app
    g4 run --watchdog 8 ; g4 log 60

M1 has no window, so the binary also runs straight over SSH
(`ssh g4 './MarioParty4.app/Contents/MacOS/isle --watchdog 6'`), which is how
these numbers were taken; the console runner is only needed once there is
something to draw.

Note for anyone rebuilding in a git worktree: `extern/musyx` (a submodule) and
`build/GMPE01_01/include` (the decomp's generated headers) are not materialised
by `git worktree add`. Copy them in — 1.6 MB total — or the PPC build stops at
`musyx/musyx.h: No such file`.

### 11.2 What the G4 does that the host cannot

**`msmSysInit` passes.** This was the one prediction §9.6 made about hardware
and it came true exactly: no `Error Code -121`, no
`port> little-endian host: ...` line, no `host:`-prefixed patch in play. The
game's own MSM parser reads `sound/mpgcsnd.msm` in place, loads the base group,
and brings MusyX up — ten SDK entry points (`AIInit`, `sndInit`,
`sndStreamAllocEx`, the aux callbacks, `sndOutputMode`, `sndVolume`) that the
host build has never once reached. The audio surface can only be measured here.

**The REL headers are right.** The failure path's module dump is the game
reading `dll/bootdll.rel` through `-malign-natural` structs from a PowerPC
`FILE*`, and all sixteen fields of `OSModuleInfo` + `OSModuleHeader` match the
raw disc bytes (`id=1`, `numSections=14`, `nameSize=47`, `version=2`,
`relOffset=0x5c9d`, `impOffset=0x7a4d`, …). On the host all sixteen are
garbage. Alignment and endianness proved in one table.

**The coroutines work.** `++++ Start New OVL 1 ++++` is printed after
`HuPrcSleep` yields and the scheduler resumes, so `gcsetjmp`/`gclongjmp` in
`port/src/os/jmp_ppc_darwin.s` complete a round trip under the real Darwin PPC
ABI, and the three heap dumps and `objectsetup` that follow all run on the
fabricated HUPROCESS stack.

**The numbers get closer to the console.** `objman>Used Memory Size` is
`0x121E0` on the G4 against `0x12200` on the host — two live objects, sixteen
bytes smaller each, because the game's structures hold pointers and here they
are the width the GameCube's are. Heap sizes, `Rest Memory` and
`left memory space` are identical on both.

### 11.3 Two port bugs the host build could not see

The first G4 run stopped forty lines earlier than the host, at
`HuMem> Failed OSAlloc left space` and `MSM(Sound Manager) Error:Error Code -31`.
Both were one-liners, both are fixed, and neither is G4-specific in principle —
only in reachability.

1. **`OSCheckHeap` returned a number `OSAlloc` could not honour.**
   `HuMemInitAll` ends with `OSAlloc(OSCheckHeap(h))`. Requests round up to 32;
   the free-block header is 12 bytes on a 32-bit target, so a free total is
   generically 20 (mod 32) and the round-up overshoots. The 64-bit host's
   16-byte header left the total already aligned, by luck. `OSCheckHeap` now
   rounds its answer down to the allocation granularity
   (`port/src/os/os_arena.c`); host output is byte-identical, and the G4 now
   creates heap 4.
2. **A generated stub answered FALSE where MusyX answers TRUE.** Reachable only
   because (1) was fixed: `msmSysSetAuxParam` treats a FALSE from
   `sndAuxCallbackPrepareReverbHI` as failure, which becomes
   `MSM_ERR_INVALID_AUXPARAM` and a `while (1)` in `HuAudInit`. The four
   `sndAuxCallbackPrepare*` symbols are now in `gen_stubs.py`'s existing
   `RETURN_OVERRIDES` table, and the untyped generator honours that table too
   (it previously always emitted `return 0`). A placeholder answer, not an
   implementation.

With both in, the G4 narration reaches the same seam as the host —
`objdll>Link DLL:dll/bootdll.rel` → `OSLink … returning FALSE` →
`objman>ObjectSetup end` — and spins in `omWatchOverlayProc` as documented.

### 11.4 Timings and footprint

| | |
|---|---|
| boot, `main()` to `objman>ObjectSetup end` | under 1 s |
| process overhead outside the watchdog | ~10 ms (`--watchdog 2` exits at 2.010 s, three runs) |
| after the seam | 100% of one CPU, spinning in `omWatchOverlayProc` |
| resident / virtual | 1,104 KB / 126,176 KB (8 MB game stack + 24 MB MEM1 + 16 MB ARAM + libs) |
| PowerPC binary | 1,094,304 bytes |
| SDK surface at boot | 66 distinct stubs, 123 calls (host: 56 / 111) |

### 11.5 Still unproved on hardware

- **ARAM does no work at boot.** `ARInit` is the only ARAM line; nothing in M1
  issues an ARQ transfer, so the block is allocated and indexed but never
  touched. First real traffic is MusyX sample upload, at M3.
- **`dlclose` on 10.5 — PLAN risk #2 — is untested.** `--reltest` needs the REL
  loader, which was not yet on `fork/ppc-port` when this was captured. It is
  the first thing to run on the G4 once it lands; the workflow above is in
  place and takes about a minute end to end.
- **The crash handler** links and installs but nothing crashed, so its
  backtrace path is unexercised on this target.
- **Anything visual.** M1 has no window; the 66 GX entry points are counted,
  not drawn.

### 11.6 Same day, an hour later: M2a on the G4, and risk #2 closed

The REL loader landed on `ppc-port` while the M1 run above was being written
up, so this worktree was rebased onto it and the whole thing went round again.
The full narration is in [`docs/g4-m2a-boot.log`](g4-m2a-boot.log).

**`--reltest`: 198/198 clean, 0 still resident after `dlclose`, in 0.241 s.**
Ninety-nine PowerPC `MH_BUNDLE`s, opened and closed twice each, with
`dlopen(RTLD_NOLOAD)` asked after every single unload whether the image really
went away. It always had. Identical to the host result, and it is the answer
**§4 risk 2** has been waiting for since the plan was written: dyld on Mac OS X
10.5.4 genuinely unloads a bundle, so the game's re-entry contract — fresh,
zeroed bss on every re-load — holds without the by-hand fallback, which was
never reached.

One PowerPC-only build fix was needed first: `dll_load.c`'s 32-bit branch
called `getsegbynamefromheader()`, absent from the MacOSX10.4u SDK. It now
walks the load commands for `__TEXT` itself. Five lines; the 64-bit branch is
untouched.

**bootDll runs, and so does the game.** `objdll>LinkOK`, `Boot ObjectSetup`,
`InitObjMan`, prolog end — and then 900 frames of the real boot sequence.

- **ARAM is finally exercised on hardware.** §11.5 had to record that `ARInit`
  was the only ARAM line in the M1 boot. bootDll's asset load does four real
  transfers (`ARAM Trans 808000 / 80aaa0 / 82da60 / 8a7640`) against the game's
  own `Rest Memory` accounting, ending at `data num 74000b`. Written and read
  on the G4, no fault.
- **The GX surface triples and changes shape.** 83 distinct stubs against 66,
  and the counts stop being ones and twos: 97,944 `GXPosition1x16`, 97,944
  `GXNormal1x16`, 97,268 `GXTexCoord1x16` inside 1,320 display lists, 891 TLUT
  loads, 893 `GXDrawDone`, 398,743 calls in 900 frames. Indexed vertices,
  display lists and colour-index textures — that is what M3's GL 1.3 backend
  actually has to do, and §9.3's implementation order should be re-derived from
  these counts rather than from M1's.
- **Speed: 900 retraces in 15.88 s wall against a 15.02 s game clock (56.7 fps)
  for 1.17 s of user CPU — about 7% of one processor.** Meaningless as a
  finished-port number, because GX draws nothing; meaningful as a statement
  that the engine, the coroutines, the DVD reads and the ARAM traffic together
  leave essentially the whole frame budget on this machine to the GL backend.

Two tooling notes. M2a links SDL2 dynamically from the Docker mount path, which
does not exist on the G4, so the first push died in dyld; `make_bundle.sh` now
copies the dylib into `Contents/Frameworks` and rewrites the reference to
`@executable_path` — using the **cross** toolchain's `install_name_tool` inside
the build image, because the host's own refuses these binaries with "malformed
load command 0". And `--watchdog` is a plain `alarm(N)` whose message says "the
game is not making progress"; at M2a it says that after 541 healthy frames, so
`--frames` is the way to end a run now.

---

## 12. M2b log — the first real frames on the Radeon 9000 *(2026-09-13)*

M2's done-means was "the Hudson and Nintendo logos render, and a screenshot
from the Mac and one from the G4 are the same picture". **Both logos render on
the G4, and so does the title screen.** The Mac half of that sentence turned
out to be the wrong test and §12.7 says why.

This session merged the G4 branch into `ppc-port`, ran the module and GX
self-tests on hardware, and then spent almost all of its time on a single
class of bug: the port had copied GX's *shape* faithfully and its *timing*
carelessly, in five separate places.

### 12.1 The merge, and the two self-tests on hardware

`fork/ppc-port-g4` merged into `ppc-port` with three conflicts, all resolved
by keeping both sides (the log entry is in the commit). The M2a log stays §10;
the G4 first-run log became §11.

**`--reltest`: 198/198 on the real machine**, 0 failed, 0 missing an entry
point, **0 still resident after `dlclose`**. That is §4's risk 2 closed on the
only machine that could answer it, and `--relzerobss` — the fallback that
zeroes a bundle's `__bss`/`__common` by hand — has still never been needed.

**`--gxdemo` on the Radeon 9000 is the same picture as the host's**, which is
the result that mattered most and the one there was least reason to expect.
17,397 of 307,200 pixels differ (5.7%) and **the largest difference in any
channel is 6/255**; the deltas are ±1 on interpolated gradients and inside
filtered texels, i.e. the two rasterisers' interpolation rounding. Every quad,
every one of the ten texture formats, the TLUT, the CMPR blocks, the S16
fractional-fixed-point array, the alpha blend and the display-list round trip
land in the same place with the same colours. **No GL-1.3 or Radeon difference
needed fixing in `gl13.c`** — the emulated feature set the host had been
exercising was honest.

`--glcheck` passes on the real ATI driver: no GL call the backend makes falls
outside the written GL 1.3 + named-extension list.

### 12.2 The card, recorded rather than remembered

`--glinfo` is new — the equivalent of the N64 ports' `--glinfo` — and dumps the
driver's strings, twelve limits and every extension. The full output is
[`docs/g4-glinfo.log`](g4-glinfo.log). The card is exactly what §0 assumed:

```
GL_VENDOR    ATI Technologies Inc.
GL_RENDERER  ATI Radeon 9000 OpenGL Engine
GL_VERSION   1.3 ATI-1.5.28
GL_MAX_TEXTURE_UNITS 6      GL_MAX_TEXTURE_SIZE 2048    GL_MAX_LIGHTS 8
77 extensions
```

with `ATI_texture_env_combine3`, `ARB_texture_env_crossbar`,
`EXT_texture_compression_s3tc`, `EXT_blend_subtract`, `EXT_fog_coord`,
`ARB_multisample` and `ATI_text_fragment_shader` all present, and no
`ARB_fragment_program`, no FBO, no NPOT.

One capability the host had wrong: **the Radeon 9000 has no
`ARB_depth_texture`** (the host reported 1, the card reports 0). Nothing has
needed it yet, but the game does three `GX_TF_Z24X8` and one `GX_TF_Z8` depth
copy (§1.14), and those four sites now have no obvious GL 1.3 home. That is a
real item for whoever reaches them.

### 12.3 Five bugs between a correct draw and a black screen

The GX slice was right. The C8-through-a-TLUT decode of the 576×480 big-endian
Nintendo logo is pixel-perfect the first time it runs. The ortho projection,
the vertex assembly, the `MODULATE` TEV chain, the raster colour out of the
channel register: all correct. The screen was black anyway, five times over.

1. **`GXLoadTexObj` aliased the caller's `GXTexObj` instead of copying it.**
   The hardware loads the object into the texture registers and the caller's
   object is dead the instant it returns — and the game leans on exactly that:
   `HuSprTexLoad` (`src/game/sprput.c`) builds its `GXTexObj` as a **stack
   local**, loads it, and returns before a single vertex is emitted. The port
   was reading a dead stack frame at draw time; the magic word no longer
   matched and `gx_tex_bind` returned without binding anything. Textures are
   now held **by value** in `GXState` and `gx_bound_tex()` is the only way to
   ask what a unit holds.

2. **Non-power-of-two textures.** The Radeon has no NPOT support, and an NPOT
   `glTexImage2D` makes the texture *incomplete*, which silently disables
   texturing for that unit — no GL error, nothing in the log. The Nintendo logo
   is 576×480. Decoded images are padded up to the next power of two with the
   edge replicated, and the fraction holding real texels is folded into that
   unit's `GL_TEXTURE` matrix, which this backend was not otherwise using
   because texgen is done on the CPU. The vertex decoder still emits the game's
   own 0..1 texcoords and knows nothing about it.

3. **`GXCopyDisp` cleared the back buffer before the swap.** On the console
   `GXCopyDisp` copies the EFB to the XFB and only *then* clears the EFB for
   the next frame, so the clear never touches the image being shown. Here the
   back buffer *is* the image and the swap happens later, at the retrace gate —
   so the game's own `HuSysDoneRender` was throwing each frame away
   microseconds after drawing it. **Every frame rendered correctly and every
   frame was black.** The clear is now queued and run immediately after the
   swap. This also corrects §10.8, which blamed the host's black frame on the
   wipe drawing over skipped sprites: true about the sprites, but not why the
   frame was black. This was, on both targets.

4. **`__OSBusClock` was zero inside every REL, so `OSTicksToMilliseconds`
   divided by zero.** `<dolphin/os.h>` does not `extern` it — on the console it
   is a fixed address in low memory, so the header simply declares it, which
   off the console is a tentative definition in every translation unit that
   includes it. Built with `-fno-common` and an exported-symbols list, each
   bundle linked its own private zero copy (`nm` showed `s ___OSBusClock`).
   `OS_TIMER_CLOCK` is `OS_BUS_CLOCK / 4`, so inside all 99 modules every
   `OSTicks*` conversion was `x / 0`, which PowerPC does not trap. `bootDll`
   paces the Nintendo logo with
   `while (OSTicksToMilliseconds(OSGetTick() - t0) < 3000) HuPrcVSleep();`, so
   the boot sat on that logo for as long as you cared to watch, at a healthy
   60 fps, with nothing in the log. §10.2 describes this patch as already made;
   it was not in `patches.txt`, and that paragraph was ahead of the tree.

5. **The wall-clock tick rate was 40 kHz, not 40.5 MHz.**
   `us * (PORT_TIMER_CLOCK / 1000000) / 1000` truncates 40.5 to 40 and then
   divides by a thousand more than it should. It is now `ns * 81 / 2000`,
   exactly 40.5 MHz. `--deterministic` advances `PORT_TIMER_CLOCK/60` per
   retrace and was right all along, which is why nothing had caught it.

`port> OS clock: N ticks in W s = R MHz (console 40.500)` is now printed at
shutdown and says `*** WRONG` if it is not. A clock wrong by a factor is not a
small error in this game — a dozen places pace themselves off it — and it
presents as a hang that looks like a rendering bug.

### 12.4 What renders, and how it compares to Dolphin

| what | port | against Dolphin |
|---|---|---|
| the wipe-in over the Nintendo logo | correct, 30 frames | matches `boot-0011`..`0031` in content |
| **the Nintendo logo** | **correct** — [`screenshots/mp4-logo-nintendo.png`](screenshots/mp4-logo-nintendo.png) | same image as `boot-0091`..`0181` |
| the wipe-out, the 60-frame gap | correct | matches |
| **the Hudson logo** | **correct** — [`screenshots/mp4-logo-hudson.png`](screenshots/mp4-logo-hudson.png) | same image |
| the opening THP movie | **skipped**, cleanly and deliberately — §12.5 | Dolphin's frames 187–4381; the port takes zero frames |
| **the title screen, 2D layer** | **correct** — [`screenshots/mp4-title.png`](screenshots/mp4-title.png): the starburst background, the MARIO PARTY 4 logo, PRESS START, both copyright lines | pixel-for-pixel the same as `boot-5025`'s 2D content |
| the title screen, 3D layer | **missing**: Peach, Wario, Mario, Daisy, Goomba, Toad, Boo, Koopa, DK and the present boxes do not appear | `boot-5025` has all of them |

Frame numbers do not correspond between the two sides and were never going to:
the port skips the console's DVD seek and, now, seventy seconds of movie. The
comparison is by content, which is what `--dumpframe`'s new frame-*set*
argument (`850,900,950` or `1-400/20`) is for.

**The 3D layer is the M3 opener, and it is not silent.** 27,041 vertices per
frame are being assembled and submitted through 358 display lists and 360
`glDrawArrays` — the HSF models *are* being drawn, and they are invisible. So
this is not a missing code path; it is a state bug, and the two obvious
suspects are the depth configuration (the 2D layer runs with `GXSetZMode`
false and the models do not) and the `GXInitSpecularDir` / two-konst /
alpha-compare degradations §12.6 names. `--drawlog` was written for exactly
this and should be pointed at the first model draw.

### 12.5 The opening movie, skipped on purpose

THP decode is M8: `THPDec.c` is 352 paired-single sites of JPEG inverse DCT,
the densest concentration of Gekko-only code in the tree. What M2b found is
that the game cannot survive simply being told so. `THPTestProc` retries
`THPSimpleOpen` **forever**, and `HuTHPEndCheck` asks
`THPSimpleGetTotalFrame()`, whose stub returns 0 — which its own
`if (temp_r31 <= 0) return FALSE;` reads as "not finished". `BootExec` then
waits on `while (!HuTHPEndCheck())` and the boot stops at the movie, at 60 fps,
printing `THPSimpleOpen fail` a few thousand times a second: two processes each
waiting on the other's impossible condition.

Rather than fake a movie — a frame count the port would have to advance, a
decode buffer it would have to size, an audio track it would have to pretend to
mix — the port says plainly that it cannot play one.
[`port/src/dvd/thp_stub.c`](../src/dvd/thp_stub.c) holds the policy in one
function, `portTHPAvailable()`, and two exact-text patches ask it:
`HuTHPEndCheck` returns TRUE at once, and `THPTestProc` tears itself down
exactly as its own tail does (kill the sprite it was drawing into, clear
`THPProc` so a later `HuTHPSprCreateVol` still works, `HuPrcKill` itself). A
movie takes zero frames and leaves nothing behind. Every skip is named in the
log and counted at shutdown, so a missing cut-scene is never a silent
difference from the console. When the decoder lands, `portTHPAvailable()`
returns 1 and both patches fall through to the original code.

### 12.6 What `--gxwarn` names at the title screen

Four distinct degradations, in order of how much of the screen they cover:

| warning | count in 999 frames | what it means |
|---|---:|---|
| `GXSetAlphaCompare: two live OR comparisons, the first is used` | 79,488 | GL has one alpha test; GX has two combined by AND/OR. Both AND cases seen so far are the same comparison twice (`GEQUAL 1 AND GEQUAL 1`), so the first is exact. The OR cases are not, and 79,488 of them is every draw. |
| `GXInitSpecularDir: specular is approximated by the diffuse term` | 9,108 | the title models' specular highlights |
| `TEV: a stage needs two different constants; the first wins` | 8,280 | one `GL_TEXTURE_ENV_COLOR` per unit against GX's four konst registers |
| `indirect texturing … direct stage only` + `GXSetTevIndTile: dropped` | 48 each | `HuSprDisp`'s background-tiling path (`sprite->bg`), §3.4 case 3 |

The first three are all on the invisible 3D layer, which is suggestive.

### 12.7 Performance on the G4, and the honest ratio

`--perf` is new and reports both clocks, because §2.5's lesson is that an
idle-gated retrace **hides overruns**: the game clock stays at 60 on paper
while the wall clock falls behind.

**The logo sequence keeps up exactly.** 400 frames:

```
  game     mean   0.75  median   0.30  p95   0.53  worst 144.98 ms
  gx       mean   1.66  median   1.50  p95   1.82  worst  52.29 ms
  present  mean   4.07  median   0.26  p95  38.77  worst  43.03 ms
  clocks   game 6.66 s vs wall 6.66 s -- ratio 1.000  (keeping up)
```

**The title screen does not.** 1,000 frames, ending on the title:

```
  game     mean   1.29  median   0.23  p95   3.44  worst 144.95 ms
  gx       mean  83.00  median   0.15  p95 301.26  worst 636.85 ms
  present  mean   0.54  median   0.24  p95   0.70  worst  67.79 ms
  frame    mean  84.83  median   0.62  p95 305.75  worst 643.18 ms
  budget   16.68 ms/frame at 59.94 Hz; 282 of 999 frames over it (28.2%)
  clocks   game 16.67 s vs wall 95.95 s -- ratio 5.757  (WALL CLOCK IS BEHIND)
  fps      10.4 effective
```

**Everything is in `gx`, and it is not the geometry.** 27,041 vertices per
frame across 360 draws is 75 vertices a draw, and 83 ms / 360 draws is
**230 µs of fixed cost per draw**. A G4 transforms and lights 75 vertices in
single-digit microseconds. The cost is per-draw state: `gl13_apply_transform`,
`gl13_apply_raster_state` and `gx_tev_apply` re-emit the entire GL pipeline
configuration — six texture units of `glTexEnv*`, matrices, blend, alpha, z —
for every single `glDrawArrays`, and on this driver each of those provokes
validation. **The fix is state caching: track what GL already has and emit only
the difference.** That is a well-understood piece of work, it is the single
largest speed item in the project, and M3 should do it before anything else.

One cost was already found and removed. The texture cache is content-keyed on
purpose — the game reuses one buffer for different images and calls
`GXInvalidateTexAll` on every sprite pass, so there is no invalidation signal
worth honouring — but it was hashing every texture **in full on every bind**,
and at 427 binds a frame with a 270 KB logo that is megabytes of FNV per frame
on a 1 GHz machine. After a buffer's first sight the hash is now sampled:
header, tail and a bounded spread of interior points, 4 KB total, with the
first sight always hashed in full so a texture is never wrong when it appears.
`--texhash-full` restores the exhaustive hash, which is how to prove a
suspected staleness bug is or is not this. The run above reports 99 MB hashed
in full against 1.4 GB sampled.

### 12.8 Determinism, proved

`--seed N` is the deterministic clock started at a chosen reading, and it is
the port's whole RNG-seed story. The game has exactly two random sources and
**both seed from `OSGetTime` and from nothing else**:

- `src/game/frand.c:13` — `frandom(0)` is `rand8() ^ (s64)OSGetTime() ^ 0xD826BC89`, reached once from `init.c:77`;
- `src/game/board/main.c:1432` — `BoardRandInit()` sets `boardRandSeed = OSGetTime()`;

and `rand8`'s own `rnd_seed` is the literal `0x0000D9ED` in `main.c:134`. So
moving the clock's origin moves both generators together, through the game's
own seed sites — no patch to game source, no second seeding path to keep in
step, and the two RNGs keep the relationship to each other that they have on
the console. `--seed` implies `--deterministic`.

Two independent runs on the G4, `--frames 260 --noaudio --seed 12345
--dumpframe 120,200,250`:

```
run A  757e95efaf39876580f6852102be0072  frame-00120.ppm
       757e95efaf39876580f6852102be0072  frame-00200.ppm
       80a2f1dee0d65d8e175289184fe4b84f  frame-00250.ppm
run B  757e95efaf39876580f6852102be0072  frame-00120.ppm
       757e95efaf39876580f6852102be0072  frame-00200.ppm
       80a2f1dee0d65d8e175289184fe4b84f  frame-00250.ppm
```

Byte-identical. (The two 120/200 hashes matching each other is the logo
holding still, not a bug.)

### 12.9 Audio on the G4, and what M6 actually needs first

Run deliberately **without** `--noaudio`; the whole narration is
[`docs/g4-audio-first-sound.log`](g4-audio-first-sound.log). Three results.

**`msmSysInit` succeeds on the G4.** No failure, no hang in `HuAudInit`, and
`--noaudio` is not needed: the boot reaches the title screen with the sound
manager live. `src/msm` reads `/sound/mpgcsnd.msm` correctly because the file
is big-endian and so is the machine — the same story as the sprite banks.

**There is no `dspSlave` command batch to log yet, and the reason is
structural.** MusyX's DSP command list is built by `extern/musyx`'s
`hw_dspctrl.c` and handed over by `hw_dolphin.c`, and **`extern/musyx` is not in
the build**: all 52 `snd*` entry points are generated stubs, so nothing
downstream of `sndInit` exists to emit a batch. M6's first task is therefore not
"interpret the DSP command list" but "compile `extern/musyx` above the SAL and
write `port/src/audio/musyx_sal.c`"; the command list appears the moment that
happens, and `hw_dspctrl.c` is its own specification.

**The first sound is a stream, not a sequence.** The first sound the game asks
for is `MSM_SE_SEL_01` / `SE Num 0`, the title screen's selection effect, and
what it reaches is `sndStreamMixParameterEx`, `sndStreamFrq`,
`sndStreamADPCMParameter`, `sndStreamARAMUpdate`, `sndStreamActivate` — twice.
That is `msmstream.c`'s ADPCM streaming path into ARAM, not the sequencer. So
the first thing M6 has to make audible is the ADPCM stream; the sequencer,
voice and ADSR machinery can follow. The 18 stubs' first-call order in that log
is the implementation order.

### 12.10 Tooling added, and one build bug worth naming

| flag | what |
|---|---|
| `--glinfo` | the driver's strings, twelve limits and every extension, one per line |
| `--perf` | per-frame game/gx/present with mean, median, p95 and worst, plus both clocks and the ratio between them |
| `--drawlog N` | explains the first N draws in full: geometry after the CPU transform, raster colour, the texture actually bound, projection, TEV inputs, alpha compare, blend, z, scissor, and any pending GL error — and the first N display-list replays, opcode by opcode |
| `--dumptex` | every decoded texture as it was decoded: colour as PPM, alpha as PGM, because an alpha test judges the alpha |
| `--seed N` | §12.8 |
| `--texhash-full` | §12.7 |
| `--dumpframe SPEC` | now a frame *set*: `187`, `1,90,186`, or `1-400/20` |

`--watchdog` was a stopwatch pretending to be a watchdog: a plain `alarm(N)`
that fired after N seconds whether or not the game was healthy and then
reported "the game is not making progress". When the game is merely slower than
N seconds that is a lie, and it cost an hour here — it fired inside
`GXCallDisplayList` and sent this session hunting an infinite loop in a
display-list parser that turned out to be correct. It now re-arms every period
and reports only when the retrace count has not moved.

**The build had no header dependencies.** `$(BUILD)/port/%.o: %.c` and nothing
else, so editing `port/include/port.h` rebuilt *nothing*. Adding fields to
`PortOptions` therefore left every object but `main.c` on the old struct
layout, and `--gxwarn` set whatever field used to live at that offset — which
was `headless`. The run then quietly skipped the window and reported a full set
of `--perf` numbers for a pipeline that never drew, which is a wonderfully
misleading thing to measure. Compiles now pass `-MMD -MP` and the Makefile
includes the `.d` files. If any single change in this session will save the
most time later, it is that one.

### 12.11 What M3 needs

In order:

1. **Per-draw GL state caching** (§12.7). 230 µs of fixed cost per draw is the
   whole performance story, and menus draw more than the title does.
2. **The 3D layer at the title** (§12.4). The draws happen; the pixels do not.
   `--drawlog` on the first model draw, then the depth configuration.
3. **PAD.** `port/src/pad/pad_none.c` is still a stub, so nothing can press
   START and the title screen is where the boot ends. SDL2 game controller plus
   the SBK ports' IOKit Xbox One driver, per §2.3.
4. **CARD.** `card_none.c` likewise; the memory card is mandatory past the
   title (`port/ref/notes.md`), so M3 cannot finish without it.
5. The four `GX_TF_Z24X8`/`GX_TF_Z8` depth copies, which have no obvious GL 1.3
   home now that `ARB_depth_texture` is known absent (§12.2).

---

## 13. M3 log — the 3D layer, a controller, a memory card, and Party Mode *(2026-09-13)*

M3's done-means was "the title screen's 3D layer appears and runs at speed, the
game has a pad and a memory card, and the menus walk into Party Mode". All
four happened, and the two that looked hardest — the invisible models and the
230 µs per draw — were each one wrong line rather than a missing subsystem.

§12.11 listed five things in order. This log follows that order, except that
the first two swapped places once the port was profiled on the machine it runs
on rather than reasoned about from the source.

### 13.1 Per-draw GL state caching, and why it was not the answer

`gl13.c` now holds a shadow of the GL state and emits only the difference:
texture binds, the whole texture-environment chain, the raster state, the
projection, the client arrays. Everything goes through one door (`glc_*`), so
there is exactly one place the shadow can drift from the driver;
`glTexParameter` is cached per texture **object** rather than per unit, because
that is what it belongs to; and `glc_invalidate()` forgets the lot after
anything that changes GL behind its back (the once-a-frame clear, an EFB copy).

It works exactly as advertised. In a 1,000-frame run it elides **98.9%** of the
state calls the backend used to make — 18,281,772 of 18,490,466.

And the frame barely moved: **11.8 fps to 13.8**.

So the port was profiled on the G4 with `sample`, which is what should have
happened first. Of about 4,300 in-thread samples:

| | samples |
|---|---:|
| `transform_and_store` | 1,727 |
| **`sqrt`** (`__sqrt` + stubs + `sqrtf`) | **768** |
| `read_component` | 516 |
| `indexed` | 479 |
| `gx_tex_bind` | 448 |
| every GL entry point, together | ~40 |

`--headless`, which runs the whole GX pipeline and never calls GL, was 57 ms of
gx per frame against 70 with the window open. **The driver was never the
problem.** §12.7's "230 µs of fixed cost per draw is the whole performance
story" was measured correctly and attributed wrongly: the fixed cost is per
*vertex*, and it is arithmetic.

### 13.2 Three PowerPC-shaped fixes in the vertex path

- **`sqrtf` was a call into double-precision `sqrt` through a dyld stub.** The
  74xx does not implement the optional `fsqrt`/`fsqrts` at all, so the 10.4
  libm cannot inline them and every normal normalisation and every light
  distance left the port through the PLT. What the 74xx does have is
  `frsqrte`, a five-bit reciprocal-square-root estimate; two Newton-Raphson
  steps take it to about twenty bits, which is far more than a byte-quantised
  vertex colour can show. And a *reciprocal* square root is what both callers
  wanted — they divide by the length — so three `fdivs` went with each one.
  [`port/src/gx/gx_math.h`](../src/gx/gx_math.h).
- **`fdivs` is 14–21 cycles and does not pipeline**, and the indexed-attribute
  reader was doing one per component to apply the vertex-attribute table's
  fractional shift — up to eight per vertex. The shift is a power of two, so a
  table of exact reciprocals costs nothing in precision.
- **CPU lighting divided bytes by 255.0f up to thirteen times per vertex** —
  four for the material, three for the ambient, three per light. A 256-entry
  table built with the real division removes the divide *and* the
  integer-to-float conversion, which on pre-2.06 PowerPC is a store-load-add
  dance through memory.

Plus: `transform_and_store` no longer copies the whole 96-byte `Vtx` per vertex
(only the two colours survive the transform; `tex_slots` covers the gap a TEV
stage is allowed to name), 16- and 32-bit attributes are loaded rather than
assembled byte by byte on a big-endian machine, and the texture content hash's
sample budget drops from 4 KB to 1 KB — 427 binds a frame times four kilobytes
of FNV was four megabytes a frame of pointer chasing on a 1 GHz machine.

**Result, by `--perf`'s own "effective fps" over a 1,000-frame `--seed 12345`
run ending on the title:**

| | M2b | M3 |
|---|---:|---:|
| gx, mean | 83.00 ms | 39.88 ms |
| gx, p95 | 301.26 ms | 142.75 ms |
| frame, mean | 84.83 ms | 42.14 ms |
| clocks ratio | 5.757 | 2.526 |
| effective fps | 10.4 | **23.7** |

Frames 120, 250, 350 and 400 of that run are **byte-identical to the build
before any of this**, which is the whole point of doing it in this order.

The 60 fps target is not met and the remaining gap is now understood rather
than suspected: the title screen alone (the 300 frames after the logos) runs at
**7.4 fps**, 135 ms a frame, and `transform_and_store` is still half of it.
§13.9 says what to do about it.

### 13.3 One real bug found by the profiler's absence, not its presence

`--perf` reported a mean frame time of **minus 485 milliseconds**.

```c
return mach_absolute_time() * tb.numer / tb.denom;   // NO
```

On the G4 `mach_timebase_info` answers numer = 1,000,000,000 and denom = the
timebase frequency, about 33 MHz, so the multiply overflows 64 bits after
2^64 / 1e9 = 1.84e10 ticks — **about nine minutes of machine uptime**. Past
that the product wraps and the clock jumps backwards by a couple of centuries'
worth of nanoseconds, every nine minutes, forever.

`OSGetTick` is downstream of this and a dozen places in the game pace
themselves off it (§12.3 bug 4 is one of them), so what this would have looked
like in the game is a logo that hangs forever or a wipe that finishes
instantly, at random, on a machine that had been on for a while. The counter
is now read relative to the port's own first reading and scaled as
quotient-plus-remainder, which cannot overflow for any run length that fits in
the counter at all; `OSGetTime`'s *origin* comes from the host calendar
instead, which is both what the console does — ticks since 2000-01-01 — and
what keeps the two clock-seeded RNGs (§12.8) seeded differently per run.

### 13.4 The invisible 3D layer: a sign

§12.4 left the title screen submitting 27,041 vertices in 358 display lists,
with no GL error, and drawing nothing, and listed seven candidates. It was
none of them. It was one character in `gl13_apply_transform`.

GX maps eye z to [-1, 0] and GL to [-1, 1], so the port adds one row operation
on the way through. Under a perspective projection **w_clip is `-z_eye`** —
which is what `M[3][2] = -1` says two lines further down — so

```
z_clip_gl = 2*z_gx + w_clip = 2*(m22*z + m23) + (-z) = (2*m22 - 1)*z + 2*m23
```

and `M[2][2]` is `2*m22 - 1`. The port had `2*m22 + 1`. At the title screen's
projection — m22 = -3.05e-06, near 0.1, far 32768 — that is **+0.99999389 in
place of -1.00000610**: very nearly the right magnitude and exactly the wrong
sign. Every perspective vertex therefore came out at a normalised z a couple
of ten-thousandths past -1 and GL's near plane took the lot. A vertex at
z_eye = -949 landed at z_ndc = -1.0002.

It hid only the 3D layer because the orthographic branch has w_clip = 1 and its
`+ 1` was right all along — which is exactly the shape of the M2b symptom, a
pixel-perfect 2D title screen with nothing behind it.

[`docs/screenshots/mp4-title-3d.png`](screenshots/mp4-title-3d.png) against
[`ref/frames/boot-5025.png`](../ref/frames/boot-5025.png): the character cluster
on the present box, the two foreground characters, the boxes with their
ribbons, the starburst, the logo and PRESS START, all in the right places and
the right colours. The cast differs between the two shots because the title
cycles its characters and the two sides do not agree on absolute frame numbers.

Found with a new **`--drawlog-at F`**, which points `--drawlog` at a presented
frame instead of at the first draws of the boot. An unqualified `--drawlog`
explains the Nintendo logo eight times and stops, six hundred frames before the
question.

### 13.5 PAD — the Xbox One pad, on the G4, with rumble

`port/src/pad/` replaces `pad_none.c` with the real thing, lifted from the two
Snowboard Kids ports as §2.3 planned:

- **`pad_xone.c`** — the Xbox One controller over IOUSBLib, ported nearly
  verbatim from `snowboardkids-decomp/port/src/platform/input_xone.c`. Left
  stick → main stick, right stick → C stick, A/B/X/Y straight across, LT/RT →
  `triggerL`/`triggerR` with the digital `PAD_TRIGGER_*` bits past ~200/255,
  LB → Z, dpad and Start as themselves, **rumble through `PADControlMotor`**.
- **`pad_sdl.c`** — `SDL_GameController` for anything else, with a raw
  `SDL_Joystick` fallback and a keyboard map. The keyboard is polled with
  `SDL_GetKeyboardState` rather than a second event loop, because `gl13.c`
  owns the event pump and two pumps fight.
- **`pad_play.c`** — `--play SCRIPT` and `--record FILE`, feeding raw
  `PADStatus` at `PADRead` and never synthesising edges, which `ref/notes.md`
  §6.3 is explicit about: the game's own repeat and edge logic has to run.
- **`port/tools/gecko2play.py`** — converts a `ref/tools/mkgecko.py` reference
  script into the port's format.

On the real machine:

```
port> pad: Xbox One controller (045e:02ea) via IOUSBLib (pipes in 2 out 1)
port> PADInit: controller 1 = Xbox One controller (045e:02ea) (driver: Xbox-One-IOUSBLib)
port> PADInit: rumble available
port> PADInit: controllers 2-4 unplugged (reference config: CPU players)
```

Ports 2–4 report `PAD_ERR_NO_CONTROLLER` on purpose: `ref/notes.md` §4 says the
game derives human-versus-CPU from which ports answer, and the reference rig
has one pad, which is what makes the walk end with 1P and three COM.

GCC 14 rejects the 10.4u SDK's `IOKit/usb/USB.h` (unbalanced
`#pragma options align=reset`), so `pad_xone.c` compiles against the same
patched copy the Snowboard Kids ports use, under one static pattern rule in the
Makefile.

### 13.6 CARD — one real memory card, in one host file

`ref/notes.md` §5 is blunt: a card is mandatory to get past the title, and with
both slots empty the reference rig reaches SELECT A FILE, says "No valid Memory
Card is inserted." and stops for as long as you hold A.

[`port/src/card/card_file.c`](../src/card/card_file.c) is a 512 KB **"Memory
Card 59" image in the console's own format** — CARDID header, directory and
backup, allocation table and backup, 59 data blocks, big-endian, checksummed
the way `__CARDCheckSum` does it — in
`~/Library/Application Support/MarioParty4/memcard-slot-a.raw`. It would have
been quicker to keep one host file per save file and answer the API over a
directory; the console format means a save can be carried between Dolphin and
the G4 in either direction, which for a port whose whole test method is
comparison against a Dolphin rig is worth the extra afternoon. The layout came
from the decompilation's own `src/dolphin/card/` — `CARDFormat.c` lays out the
five system blocks, `CARDCheck.c` gives the checksum and its two ranges,
`CARDPriv.h` names the allocation table's five header slots, `CARDStat.c` gives
the banner and icon offset arithmetic — and no emulator source was read for any
of it. The game's own save format is untouched.

Slot B stays empty, which is the rig's configuration. `--nocard` empties both
and reproduces the dead end on purpose.

It works: the walk's new-file scene ends with

```
port> CARD: created "MarioParty4", 8192 bytes (1 block)
port> CARD: 0 reads, 1 writes, 1 files created, 0 deleted, 4 image flushes, 57 of 59 blocks free
```

### 13.7 Two bugs between the title screen and Party Mode

**Unlinking a REL must not unmap it.** `dlclose` really does unload a bundle —
M2a's `--reltest` proved it twice on the real machine, and that was the right
answer to risk 2. It is the wrong answer for the game. On the console
`objdll.c` frees the module's heap block, and freed heap is still readable and
still executable, so a pointer left behind into a just-unlinked module keeps
working until something allocates over it. Mario Party 4 leaves exactly such a
pointer: accepting the title unlinks `bootDll` and the very next thing the game
does is call into its dead text. Here that is `signal 11` with a program
counter no image claims. The port now drops the reference and keeps the
mapping, re-entering a resident module with its bss zeroed by hand — the
console's behaviour, only more reliably. `--reldlclose` restores the strict
close, and `--reltest` runs with it so the unload path stays proven.

**The one place the address MEM1 lives at is load-bearing.** `window.c` asks
"is this a message id or a pointer to a string?" four times and answers by
testing against 0x80000000: exact on a console whose RAM starts there, and
wrong here, where MEM1 is at 0x02100000, the REL bundles at 0x0b000000 and the
executable's rodata at 0x1000. Every real pointer read as an id, came back
NULL from `MessData_MesPtrGet`, and killed the first screen that shows the
player a string the game built itself — the file select, naming a card slot
"A" through `MAKE_MESSID_PTR`, which is a bare cast. A range check cannot fix
it: those pointers come from all three regions, and `saveload.c`'s
`SlotNameTbl` lives at addresses a message id can also have. So the tag is
made real — `MAKE_MESSID_PTR` sets the bit the game already tests and the four
places that turn the value back into a pointer clear it — leaving all four of
the game's own tests untouched and exactly as correct as they are on hardware.
§1.5's "no pinned-globals scheme is needed" survives with one named exception.

Both were found by a new **hand-walked PowerPC backtrace in the fault
handler**. No unwinder can follow this stack — the game runs on one the port
allocated and its HUPROCESS coroutines swap `sp` with a hand-written
`gcsetjmp` — but the linkage convention is simple enough to walk without one
and every frame is named through `dladdr`. The first crash printed
`no image claims this pc`, which is the whole diagnosis in six words.

### 13.8 A writer's name does not say which attribute it fills

This one is worth its own section, because it is a class rather than a bug.

On the console `GXPosition2f32` and `GXTexCoord2f32` are the same two stores
into the write-gather pipe at 0xCC008000. The pipe has no idea what an
attribute is: the command processor consumes whatever arrives, in the order the
vertex descriptor names, and the writers are named for readability and nothing
else. So a game is free to reach for whichever one has the right shape, and
Mario Party 4 does — `src/game/window.c`, which draws **every line of message
text in the game**, emits each glyph's texture coordinate with
`GXPosition2f32`, because a texcoord is two floats and so is a 2D position.
`printfunc.c` and four minigame modules do the same, 28 calls in all.

The port had believed the names, so those texcoords went into the position, the
descriptor's real last attribute was never written, and the vertex never
completed. Every window in the game drew as a handful of enormous untextured
quads. The same naming assumption meant a second `GXTexCoord2f32` overwrote
the first instead of filling TEX1.

`gx_draw.c` now keeps a cursor into the descriptor and each writer fills
whichever attribute is next, converting its payload to what that attribute
needs and using *that* attribute's fractional shift. That is what the hardware
does, it costs nothing, and it makes the whole class impossible rather than one
bug at a time. The title screen's four golden frames are unchanged by it,
because there the game used the writers the obvious way.

### 13.9 The menu walk

Replayed on the G4 from `ref/movies/menu-walk-port.play` — the reference
schedule rebased on the port's own clock, because the console spends 186 frames
on the cold-boot logos and 4,195 on a movie the port skips, so the reference's
START pulses at 380/430/480 are long spent by the time the port arrives at the
title around frame 700. `ref/movies/menu-walk.play`, the direct conversion of
the Dolphin script, is kept next to it as the thing that was converted.

| port frame | screen | against | notes |
|---|---|---|---|
| 900 | the title, with its 3D layer | `boot-5025` | matches |
| 1200 | **SELECT A FILE**, slot A, three files | `menu-0300`.. | the card is found and named; the message window's background is wrong (§13.10) |
| 1500 | mode select, the cube over Peach's castle | `menu-0700`.. | matches |
| 2700 | the new-file card fan | `menu-1500`.. | the panel and cards are right; the stage behind is black |
| 3000 | **character select**, 1P on Mario | `menu-2800` | the eight portraits, the badge and the cursor are right; the stage behind is black |
| 5100 | **board settings** — Toad's Midway Madness, 1P + 3 COM EASY, 20 TURNS, ALL, ON, "Are these settings OK?" | `menu-3900` | every label, value and prompt is right; the stage is black and the window backgrounds are pale blocks |

Screenshots are `docs/screenshots/mp4-menu-*.png`. That is the whole of
`ref/notes.md` §3's walk, and the game creates a save file on the way through.

**Determinism still holds across it.** Two independent runs,
`--frames 5200 --seed 12345 --play menu-walk-port.play`, with the card deleted
before each:

```
run A  4a83c956466e4207d5fb83621d7889f8  frame-01200.ppm
       3811bda1bdf836053829d74062219075  frame-03000.ppm
       3b017cbdd9272e465f6495b950dce830  frame-05100.ppm
run B  4a83c956466e4207d5fb83621d7889f8  frame-01200.ppm
       3811bda1bdf836053829d74062219075  frame-03000.ppm
       3b017cbdd9272e465f6495b950dce830  frame-05100.ppm
```

### 13.10 What `--gxwarn` names now, and what is still wrong on screen

Five distinct degradations across the whole walk, in order of how much of the
screen they cover:

| warning | count in 5,199 frames | what it means |
|---|---:|---|
| `TEV: a stage needs two different constants; the first wins` | 1,340,300 | one `GL_TEXTURE_ENV_COLOR` per unit against GX's four konst registers |
| `GXSetTevSwapMode: a non-identity swap table is ignored` | 136,522 | §3.4 fallback 2; new at the menus, not seen at the title |
| `indirect texturing … direct stage only` + `GXSetTevIndTile: dropped` | 20,818 each | `HuSprDisp`'s background tiling — this is the window backgrounds |
| `GXInitSpecularDir: specular is approximated by the diffuse term` | 12,630 | model highlights |

**Both alpha-compare warnings are gone.** M2b counted 79,488 OR cases and
280,241 AND cases at the title alone and reported each as a degradation; they
never were. A `GX_AOP_OR` (or AND) of a comparison with *itself* — the game's
own idiom for "just this one", because GX has no way to say it — reduces
exactly, and so does a half that is `GX_ALWAYS` under AND or `GX_NEVER` under
OR.

`GXCopyTex` is no longer a no-op: colour copies land in the texture cache as a
`glCopyTexSubImage2D` into a power-of-two GL texture, keyed on the destination
address the game will later wrap in a `GXTexObj`, with the NPOT fold in the
unit's texture matrix like any other texture. 4,313 of them in the walk. The
**depth** copies still have no GL 1.3 home: the Radeon 9000 has no
`ARB_depth_texture` (§12.2), there is no FBO to read from and no depth internal
format to copy into, so the four sites are skipped with a warning. A colour
proxy was considered and rejected: it would put a picture of the scene where
the reader expects a depth ramp, which is worse than a flat neutral value.

Two things are still visibly wrong, and they are M4's opening in the same way
the 3D layer was M3's:

1. **The theatre-stage backdrop is black** behind the new-file,
   character-select and board-settings screens, where `menu-2900` has
   curtains, stars and garlands. The foreground of those screens is correct,
   so this is one scene that is not drawing rather than a broken pipeline, and
   `--drawlog-at 5100` (which now prints the position matrix as well) narrows
   it a long way: the stage geometry **is** submitted, 317 draws of it a frame
   through a 640x480 perspective projection, and it is transformed clean off
   the side of the world.

   ```
   v0 pos -68320.26   279.57  -872.29
   posmtx0     1.000    0.000    0.000  -67720.26
               0.000    0.996   -0.087     -94.02
               0.000    0.087    0.996   -2109.56
   ```

   The rotation is sensible, the y and z translations are sensible, and the x
   translation is -67,720 -- so the camera and the model disagree about where
   in the world this scene sits by about 67,720 units, with the camera holding
   the offset and the model not. The same log at frame 3500 shows nothing
   like it (every position matrix there has a translation under 80), so
   whatever introduces the offset happens between the character-select screen
   and the board settings. That is where M4 should pick it up.
2. **Window backgrounds draw as pale blocks.** That is `HuSprDisp`'s
   `sprite->bg` tiling path, which is indirect texturing — the 20,818 warnings
   above — and §3.4 case 3 already names the three ways out. A tiled window
   background is the easiest of them: the tile is a plain repeat, so it can be
   expressed as a second unit with a scaled texture matrix rather than needing
   a dependent read at all.

### 13.11 Tooling added

| flag | what |
|---|---|
| `--drawlog-at F` | restrict `--drawlog` to presented frame F |
| (`--drawlog` itself) | now prints the loaded position matrix too, which is what turned "the background is black" into "the modelview's x translation is -67,720" |
| `--nocard` | both card slots read empty |
| `--reldlclose` | really `dlclose` a REL the game unlinks (what `--reltest` uses) |
| `--play SCRIPT` / `--record FILE` | scripted and recorded controller 1 |
| `--nopad` / `--paddbg` | no controller at all; log raw pad reports |

and the fault handler's backtrace, and `port> GL state:` at shutdown, which is
how a regression in the state shadow shows up as a number rather than as a slow
afternoon.

### 13.12 What M4 needs

1. **The stage backdrop**, §13.10 item 1, which is now a specific question
   rather than a symptom: where the modelview's -67,720 in x comes from, and
   why the camera has the scene's world offset and the model does not. The
   last thing between the menus and a screenshot that matches the reference
   outright.
2. **The window background tiling**, §13.10 item 2.
3. **Speed, again, and it is `transform_and_store`.** The title screen is 7.4
   fps and half the samples are in one function. The next three things to try,
   in order: hoist the per-primitive invariants (the matrices, the channel
   control, the texgen list) out of the per-vertex loop, which is pure
   bookkeeping; give the position and normal transforms an AltiVec path, which
   is what §1.12 already says MTX is the first place to reach for; and skip
   `light_channel` outright for the very common case of one channel with
   lighting disabled and a register material.
4. **A minigame.** `instDll` (the rules screen) and one `m4xxDll`, which will
   be the first code to ask for timing and input at speed rather than at menu
   speed — and the first thing the `--play` layer will have to drive
   frame-accurately rather than approximately.
5. **The pad's shutdown path.** `pad: xone: read failed (e00002eb), controller
   gone` is printed at exit; harmless, but it is the driver noticing its own
   teardown and it should not have to.

## 14. M4 log — indirect tiling, the stage offset's real address, and one step of the vertex loop *(2026-09-13)*

M4's done-means was four things: the theatre-stage backdrop, the window
backgrounds, the menus at 60 fps, and a minigame. **Two landed, one did not
move, and one got as far as the mode it lives in and stopped on a locked
door.** This log says which is which, because the two that did not land are
both further forward than they were and the evidence is worth more than the
guess §13.12 opened with.

### 14.1 Window backgrounds: indirect tiling, composed on the CPU

§13.10 item 2 is fixed, exactly rather than approximately, and it is the
change that most visibly moves the port toward the reference frames.

`HuSprDisp` draws every message window's background as **one quad with an
indirect texture stage** (`src/game/sprput.c:99`):

```c
HuSprTexLoad(sprite->bg, layer->bmpNo, 1, GX_CLAMP, GX_CLAMP, GX_NEAR);
GXSetNumIndStages(1);
GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, bg_bmp->sizeX*16, bg_bmp->sizeY*16);
GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP1);
GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_16, GX_ITS_16);
GXSetTevIndTile(GX_TEVSTAGE0, GX_INDTEXSTAGE0, 16,16, 16,16, GX_ITF_4, GX_ITM_0, ...);
```

which on the hardware means, for a texel (X, Y) of the background:

```
(vs, vt) = the indirect texture's texel at (X/16, Y/16)
result   = the tile sheet's texel at (vs*16 + X%16, vt*16 + Y%16)
```

There is no dependent texture read in GL 1.3, no fragment program on a Radeon
9000 (§12.2), and §3.4 case 3 had given up and drawn the direct stage alone —
which is why every window background came out as a pale block of whatever tile
the quad happened to land on.

**But nothing in that formula is per-pixel state.** Both textures are ordinary
images in main memory and the composition is a pure function of their
contents, so it is done once on the CPU into a cache keyed on the content hash
of both images plus the tile geometry, and bound as an ordinary texture. It is
*exact*, not an approximation, and after the first frame it costs one hash
lookup. `port/src/gx/gx_tex.c`'s `gx_tex_bind_tiled`; `gx_tev.c` reaches for it
only when the stage is a tile stage and falls back to the old warning for every
other indirect form (`m405Dll` still warps, and still warns).

Over the reference menu walk: **18 composed backgrounds, 20,450 cache hits**,
and the two indirect warnings — 20,818 each — are gone.

**The one thing that had to be measured rather than reasoned about was which
two numbers a tile-map texel carries.** The obvious reading, and what an
indirect *bump* map uses, is components 0 and 1 — red and green of the decoded
texel. That gives zero everywhere here and one tile stretched over the window,
which is indistinguishable from the old bug. The maps are `GX_TF_IA4`, one
byte a tile, and the byte is `SSSS TTTT` — so S is the **alpha** nibble and T
the intensity nibble, because that is how IA4 splits a byte. The file-select
window's map is 17x6 and reads

```
0 7 7 ... 7 1        the nine-slice, S in the alpha nibble, T zero throughout,
4 8 8 ... 8 5        against a 256x32 sheet whose nine tiles are a single row
2 6 6 ... 6 3
```

which composes to a rounded panel with a gold border — and that is what the
file select, the board settings, the "Pick a card to get this party started!"
prompt and the Mini-Game room's text windows all now draw.

### 14.2 The stage backdrop: four suspects eliminated, and the real address

§13.10 item 1 said the modelview's x translation was **-67,720** at frame 5100
and that the camera and the model disagreed about the world offset. Every part
of that sentence except the symptom turns out to be wrong, and the four
candidates M4 opened with — a mishandled `GXLoadPosMtxImm`, `C_MTXConcat`
precision or aliasing, a pointer-vs-`u32` widening in the camera struct, a
matrix index the port ignores — are all eliminated. So is `-malign-natural`.

A new **`--scenelog F`** reads the game's own globals at a presented frame
(`src/game/hsfman.c` is DOL code compiled into this binary, so there is nothing
to hook and nothing to patch — the port simply looks), and a new
**`--ovllog`** names the scene every time `omcurovl` changes. Between them:

| what | measured at frame 5100 | verdict |
|---|---|---|
| `Hu3DCamera[0]` | pos (0.00, 277.52, 1743.34), target (0, 125, 0), fov 42, near 20, far 5000 | sane |
| `Hu3DCameraMtx` | translation (-0.00, -124.52, -1760.89) | sane, and it is `C_MTXLookAt` of the above |
| all 158 live `Hu3DData` models | every `pos` under 4,600 (the drifting confetti), every `mtx` the identity | sane |
| every HSF object transform in every model | no `base.pos.x` or `curr.pos.x` over 2,000 | sane |
| the HSF trees re-walked with `objMesh`'s own T*R*S order | nothing concatenates past 3,000 in x | sane |

And yet **1,264 of the frame's 1,756 draws carry a position matrix with an x
translation between -15,000 and -31,000**, with eye-space z *positive* — behind
the camera — which is why the stage is not merely displaced but absent.

Two more port-side instruments closed in on where they come from. The first
captures `__builtin_return_address(0)` in `GXLoadPosMtxImm` and symbolises it
with `dladdr` — the same trick §13.7's hand-walked backtrace uses — which puts
every one of them in `ObjDraw` (a static function, so `dladdr` reports it as
`Hu3DDrawPost+2468`). The second is better: `GXLoadPosMtxImm` is handed
`drawObj->matrix`, and that is a *member of a `HU3DDRAWOBJ`*, so subtracting
the field offset recovers the whole draw object and with it the model index and
the HSF object's name. That turns "1,264 draws are off the side of the world"
into a list:

```
108 BIG model 142 "obj61"      70 ok / 70 BIG model 154 "all"
108 BIG model 142 "obj52"      56 ok / 56 BIG model 156 "noko"
 84 BIG model 142 "obj66"      43 ok / 43 BIG model 155 "zen"
 26 BIG model 139 "pillar"     37 ok / 37 BIG model 144 "body"
```

Models 139 and 142 are the stage (a pillar and sixteen `objNN` pieces) and are
*always* off-world; models 144 and 153–157 are the characters and are drawn
**twice, once correctly and once off-world**, in equal numbers. So this is not
a bad model and not a bad camera: it is a second pass over the same objects
whose matrix is wrong, and the stage happens to be drawn only in that pass.

That is where M5 picks it up, and the remaining candidates are now three rather
than seven: the envelope-matrix path in `objMesh`
(`hsf->matrix->data[i + base_idx]`, which bypasses the object transform
entirely and is the only input `--scenelog` has not yet read back), `objReplica`
/ `objMap`, and the `constData->hookMdlId` chain. `--scenelog` already prints
`base_idx`, `count` and the largest translation in each model's matrix buffer;
that line is the next thing to read.

### 14.3 Speed: the per-primitive hoist, and an honest number

§13.12's first step is done. `transform_and_store` no longer re-derives, per
vertex, which matrix slot is current, whether the colour channel is lit, which
texgen reads what through which matrix, or which array and format an indexed
attribute uses. All of it moves into a `PrimInv` filled once per `GXBegin`
(and once per display-list primitive — both paths go through
`begin_attr_order`). §13.12's third step comes with it: the channel is
classified into "writes nothing", "splats the register material" and "is
genuinely lit", and only the third calls `light_channel` at all.

This is sound only because a GX primitive cannot change any of it mid-stream.
The matrix index is a per-vertex attribute on real hardware and a display list
may carry XF register writes — Mario Party 4 uses neither (§3.2), the port
warns on the descriptor if it ever sees one, and `GXSetCurrentMtx` is a state
call the game only makes between primitives.

It is arithmetically identical by construction: the same operations in the same
order, only looked up earlier.

**And it did not make the frame faster.** A 1,000-frame `--seed 12345` run
ending on the title:

| | M3 | M4 |
|---|---:|---:|
| gx, mean | 39.88 ms | 41.12 ms |
| frame, mean | 42.14 ms | 43.62 ms |
| clocks ratio | 2.526 | 2.615 |
| effective fps | 23.7 | **22.9** |

That is inside run-to-run variance on a machine that has been up for three
days, and the reading is that the bookkeeping §13.12 assumed was expensive was
not: the 7450 was already hoisting most of it, and what remains in
`transform_and_store` is the arithmetic itself. **The 60 fps target is not
met**, and the honest next step is another `sample` profile on the character
select rather than another guess — which is exactly the lesson §13.1 already
paid for once.

**The AltiVec path was not attempted.** The reason is worth recording rather
than leaving as an omission: the transform is one 3x4 matrix against one
3-vector at a time, and a single vec3 through AltiVec costs more in loads,
`vec_perm` for the unaligned operand and the store-back to scalar than the nine
multiplies save. To win it has to be *batched* — store model-space positions
and normals into the vertex array during attribute assembly and transform the
whole `verts[]` run in one pass at `GXEnd` — which is a real restructuring of
the two-phase vertex path and belongs with the profile that justifies it. It
would also change rounding (`vec_madd` is fused), so the golden md5s would have
to be re-based deliberately. Neither happened; nothing in the tree is
`#ifdef __ALTIVEC__` yet.

### 14.4 A minigame: Mini-Game mode runs, and Free Play is empty

`mgmodedll` loads and runs. `port/ref/movies/minigame-select.play` reaches it
at port frame 2,015, and `--ovllog` says so in one line:

```
port> frame  879: overlay 74 (next -1) event 0     modeseldll
port> frame 2015: overlay 72 (next -1) event 0     mgmodedll
```

Getting there was the awkward part and the script's comment explains it. The
mode-select ring (`src/REL/modeseldll/modesel.c:113`) moves on
`HuPadDStkRep` LEFT/RIGHT and clamps at 0..5, and Mini-Game is index 2 — but
the frame the ring starts taking input on moves with how long the new-file card
scene ran, and a stick push that lands before it opens is simply lost. Ten
pushes spread over 600 frames all were. Two things were verified along the way
rather than assumed: `--paddbg` now prints the raw stick alongside the pad's
own `HuPadDStk`/`HuPadDStkRep`, and a `dstk:RIGHT` in a `--play` script does
produce `dstk 02 rep 02` for exactly one frame, which is what the ring wants.
So the walk stops trying to find the frame and makes **every A press a
candidate**: A, then two pushes right 20 and 50 frames later, then the next A
90 frames on. Whichever A opens the ring, the two pushes that follow it are
inside it and the A after that accepts Mini-Game.

What is behind the door:

> "You won 'em! Now, you can play 'em!"
> "Excellent! Say, how do you want to play these here Mini-Games?"
> "Free Play — Play any one you want."
> **"You haven't opened any games!"**

Free Play lists only minigames the save file has unlocked, and a save the port
created three minutes ago has none. So `instDll` and `m4xxDll` are still
unproved: the route to them is a board, which is M5, or Story mode. The
windows on those screens are all correct, which is the tiling fix earning its
keep on a screen that is nothing but windows; the room behind them is flat grey,
which is §14.2 again.

Screenshots: `docs/screenshots/mp4-minigame-select.png`.

### 14.5 What `--gxwarn` names now

Over the same 5,150-frame walk:

| warning | M3 | M4 |
|---|---:|---:|
| `TEV: a stage needs two different constants; the first wins` | 1,340,300 | 1,319,950 |
| `GXSetTevSwapMode: a non-identity swap table is ignored` | 136,522 | 135,622 |
| `indirect texturing … direct stage only` | 20,818 | **0** |
| `GXSetTevIndTile: dropped` | 20,818 | **0** |
| `GXInitSpecularDir: specular is approximated by the diffuse term` | 12,630 | 12,630 |

Three distinct degradations left, down from five. The two-konst case is the
big one and it is a real GL 1.3 limit (one `GL_TEXTURE_ENV_COLOR` per unit
against GX's four konst registers); §3.9's `GL_ATI_text_fragment_shader` escape
hatch is where it goes, and that is an M8 decision, not an M5 one.

### 14.6 Tooling added

| flag | what |
|---|---|
| `--scenelog F[,F…]` | every live camera, every live model's placement and matrix, every model's HSF object transforms and envelope-matrix header, on presented frame F |
| `--ovllog` | one line whenever `omcurovl` changes: which scene owns which frames |
| (`--drawlog`) | now also names the draw's model index and HSF object, recovered from the `HU3DDRAWOBJ` the loaded matrix is a member of, and symbolises the caller of `GXLoadPosMtxImm` through `dladdr` |
| (`--paddbg`) | now prints the raw stick and the pad layer's own `HuPadDStk`/`HuPadDStkRep`, which is how "the script's stick push is not reaching the game" was ruled out in one 200-frame run |
| (`--dumptex`) | now also writes the composed tile background, its tile sheet and its raw tile map |

`port/ref/movies/minigame-select.play` is the walk into Mini-Game mode.

### 14.7 What M5 needs

1. **The second pass with the wrong matrix** (§14.2). It is one of three named
   places now, and `--scenelog`'s envelope line is the first thing to read.
   Everything visual on three menu screens is downstream of it.
2. **A board with CPU players**, which is M5's own headline, and which is also
   the only route to `instDll` and an `m4xxDll` — Free Play is locked until a
   board has been played (§14.4). The board-settings screen already accepts, so
   the next step is the walk past it into `w01dll`.
3. **A `sample` profile of the character-select scene**, before any more
   vertex-path work. §14.3 spent a step on bookkeeping that was not the cost.
4. **Batched AltiVec**, once (3) says where the time is, as the two-phase
   restructuring §14.3 describes rather than a per-vertex intrinsic.

## 15. M5 log — two wrong C bodies in the SDK's own matrix library, and a board with three CPU players *(2026-09-13)*

M5's headline was a board. It got one, and a minigame after it, and the two
things standing in the way turned out to be the same bug twice: a `C_*` body in
`src/dolphin/mtx/` that the GameCube never called, that the decomp is right to
have written the way it is, and that `-DMTX_USE_C` makes the one that runs.
One of them was M4's off-world second pass. The other was why Toad's Midway
Madness rendered as a black screen with a working HUD over it.

Neither was findable by reading the port. Both were findable in one run each by
an instrument that asks the game what it believes.

### 15.1 The off-world pass: `C_MTXIdentity` never wrote the translation column

§14.2 left three candidates. `--scenelog`'s envelope line, which §14.7 said to
read first, eliminated the first of them outright — at frame 5100 every
`hsf->matrix->data[]` in every skinned model has a translation under 439:

```
cenv mdl144 cenvNum 6 base_idx  6 count 53  max|mtx[0][3]|  188.79
cenv mdl157 cenvNum 6 base_idx  6 count 37  max|mtx[0][3]|  438.18
```

The answer was not in any of the three. It is in `src/dolphin/mtx/mtx.c`:

```c
void C_MTXIdentity(Mtx mtx) {
    mtx[0][0] = 1.0f; mtx[0][1] = 0.0f; mtx[0][2] = 0.0f;
    mtx[1][0] = 0.0f; mtx[1][1] = 1.0f; mtx[1][2] = 0.0f;
    mtx[2][0] = 0.0f; mtx[2][1] = 0.0f; mtx[2][2] = 1.0f;
}
```

Nine elements of twelve. The translation column is not written at all.

On the console that is invisible and the decomp is not wrong about it:
`MTXIdentity` is `PSMTXIdentity`, six paired-single stores covering all twelve
elements (`psq_st` writes pairs, and 0/8/16/24/32/40 is the whole 3x4), so the
translation is always zeroed and no caller ever had to zero it itself. The DOL
never reaches `C_MTXIdentity`, so nothing in the decomp's matching build
depends on the difference.

`-DMTX_USE_C` makes the C body the one that runs, and `src/game/hsfdraw.c`'s
`mtxRot` hands it an **uninitialised stack `Mtx`**:

```c
void mtxRot(Mtx mtx, float x, float y, float z) {
    if (x != 0.0f) { MTXRotRad(mtx, 'X', MTXDegToRad(x)); }
    else           { MTXIdentity(mtx); }
    if (y != 0.0f) { MTXRotRad(rotY, 'Y', MTXDegToRad(y)); MTXConcat(rotY, mtx, mtx); }
    ...
```

so with a zero x rotation the matrix keeps whatever translation was in that
stack slot, `MTXConcat` carries it through the y and z rotations, and
`mtxTransCat` then *adds* the model's position to it. The slot is the same
address on every call from the same caller, so the value does not merely
start wrong, it **accumulates**: `Hu3DPostExec`'s `sp40` and
`Hu3DShadowExec`'s `sp58` walk into the tens of thousands over a few hundred
models in a frame.

Every part of §14.2's measurement falls out of that:

| §14.2 observed | why |
|---|---|
| the stage (models 139, 142) *always* off-world | its objects have `rot.x == 0`, so they always take the `MTXIdentity` branch |
| the characters drawn **twice**, once right and once off-world, in equal numbers | the main pass and the shadow pass take different branches of the same `if` |
| x translations of −15,000 to −31,000, drifting | the accumulation, one model's `pos.x` at a time |
| camera, model placements and envelope matrices all provably sane | none of them goes through `mtxRot` |
| the port's own re-walk of the HSF trees concatenating to nothing over 3,000 | `--scenelog` builds its matrices with `C_MTXScale`/`C_MTXRotRad`, both of which *do* write all twelve, and then sets the translation itself |

The fix is one line in `patches.txt`, and `port/tests/mtx_test.c` is the new
thing that makes the class impossible to have again: it poisons the
destination with a bit pattern no matrix element carries and demands every
constructor write all twelve elements, runs every in-place form
(`MTXConcat(m, x, m)`, which `mtxRotCat` does three times a call) against the
out-of-place answer, diffs every `PS*` replacement in `psmtx_c.c` against the
`C_*` body it stands in for, and round-trips `PSMTXReorder` +
`PSMTXROMultVecArray` — the two with no C original at all — against
`C_MTXMultVecArray`. It found this in one run and reported nothing else
wrong. `make -C port TARGET=host mtxtest`.

**Proof rather than assertion.** `--gxwarn` is no longer the only permanent
counter: `port> GX draw: N primitive(s) off-world` counts every primitive whose
position matrix puts it more than 10,000 units sideways, which is two compares
per primitive and stays in. Over the 5,150-frame reference menu walk on the
G4: **0**, against M4's 1,264 of 1,756 on one frame. Primitive and vertex
counts are unchanged to the digit — 5,779,320 and 255,242,235 before and after
— the same geometry, different matrices.

`docs/screenshots/mp4-charselect-stage.png` is the theatre stage, present.

*(The counter is x and y only, not z. The board's camera sits 14,000 units
back with a far plane at 23,000, so on Toad's Midway Madness a modelview z of
−15,000 is an ordinary distant space; counting z flagged 498,060 perfectly
good primitives on the first board run. Nothing in this game is ever ten
thousand units to the *side* of its own camera, which is what the bug did.)*

### 15.2 The board's black screen: `C_VECScale` normalises instead of scaling

`w01dll` loaded on the first try. The board intro played, the turn order was
rolled, Toad explained where the first Star was — and the screen was black
behind the sprite HUD for all of it. The board was running the whole time; it
was drawing into nothing.

`--nanwatch`, added for this, names the first frame each camera, each model and
the board's own `boardCamera` turns NaN, and prints the board camera for that
frame and the one before. Two lines:

```
frame 4984  target -150 100 300   pos -150 13255.7 5088.28  moving 0
frame 4985  target nan nan nan    pos nan nan nan           moving 1
```

`rot`, `zoom`, `fov`, `near`, `far`, `target_mdl`, and the entire focus block
are identical across those two frames. One field changed, and it is a flag.

`src/game/board/main.c:CalcCameraTarget` is three lines long in the part that
matters:

```c
VECSubtract(&pos, &camera->target, &offset);
if (camera->moving) { VECScale(&offset, &offset, 0.15f); }
VECAdd(&offset, &camera->target, &camera->target);
```

and on the frame `moving` is first set the camera is already sitting on its
target, so `offset` is the zero vector. `src/dolphin/mtx/vec.c`:

```c
void C_VECScale(const Vec *src, Vec *dst, f32 scale) {
    f32 s;
    s = 1.0f / sqrtf(src->z*src->z + src->x*src->x + src->y*src->y);
    dst->x = src->x * s;  dst->y = src->y * s;  dst->z = src->z * s;
}
```

That is `C_VECNormalize`'s body under the wrong name — `scale` is not read at
all — and on the zero vector it is `1.0f / sqrtf(0.0f)`, which is `inf`, and
`0.0f * inf`, which is NaN. Same story as §15.1: on the console `VECScale` is
`PSVECScale`, four paired-single instructions that are right, so the DOL never
called this one and the decomp matches either way.

A NaN in a camera position is completely silent. Nothing crashes, nothing
warns, every comparison against it is false, and every transformed vertex is
NaN, so the 3D layer stops drawing and the 2D sprite layer carries on
perfectly. That is exactly what the first board capture looked like.

`mtx_test.c` grows a fifth section for it: every `C_VEC*` against the
arithmetic written out by hand, and every one of them fed the zero vector,
because that is the input that turns a wrong body from a wrong answer into a
NaN that spreads. It fails the old `C_VECScale` twice over.

**The lesson worth keeping** is that `-DMTX_USE_C` is not a neutral switch. It
promotes 74 functions from "present in the decomp, never executed, matching by
construction" to "the thing the port runs", and two of them were wrong. The
test now covers the whole family; anything else in `src/dolphin/` that the DOL
never called deserves the same suspicion.

### 15.3 The walk into the board, and what one START costs

`ref/movies/board-start.play` and `ref/movies/board-start.txt` are the same
walk on the two sides, and getting them there took five captures, all of which
are worth recording because none of them was a port bug.

The shape is a **metronome**: START through the boot, then A for four frames
out of every sixty-four, forever. Nothing is pinned to a prompt, because no
prompt in this run is at a frame either side can predict — the new-file card
scene's length moves everything after it, and the port and the console do not
agree on absolute frames anyway (the port skips the DVD seek and the 4,195-frame
opening movie and reaches the title around frame 700 against the console's 380).
A metronome does not need to know.

Four things had to be learned:

1. **Dolphin silently drops a Gecko code list that does not fit.** The obvious
   translation of the metronome is one `at` per press, five code lines each,
   which for 13,000 frames is 715 lines — past the end of the region Dolphin
   injects behind the code handler. The capture then looks *exactly* like a
   capture with no codes: the attract loop, no error anywhere, in the log or
   on screen. `mkgecko.py` grew an `every <period> <frames> <from> <btn>
   [<phase>] [<until>]` directive that compiles a metronome to a single masked
   bit test on the low half of `GlobalCounter` (Gecko `28`, "if `(u16 & ~mask)
   == value`"), which is five lines for the whole run instead of 715.
2. **The minigame instruction screen ends on START and nothing else.**
   `src/REL/instDll/main.c:293` is `btnDown == PAD_BUTTON_START`; the
   auto-start beside it needs all four players to be CPU and this walk leaves
   one human. A alone stops there forever, on both sides.
3. **That START cannot share a frame with an A.** The test is an equality, and
   A+START is `0x1100`. Hence the phase offset.
4. **And it cannot be a metronome over the whole run.** A START anywhere else
   opens the *board* pause menu (`src/game/board/pause.c:1493`), which sleeps
   four frames before it reads input — so a four-frame press opens the pause
   and throws the rest of itself away, and the next A walks into "Please choose
   which character's settings to change", where the walk stays for the rest of
   the capture. **Both the port and Dolphin got stuck there, identically**,
   which is a small piece of evidence in its own right.

So the START is a short metronome inside a window, placed from an observation
in an A-only capture rather than a guess: the screen does not take input until
its entry animation finishes (`while (instMode != 1)`, a few hundred frames
after it first appears), so a tap or two at the frame it appears does nothing
either. That window is the one pinned thing in the walk.

### 15.4 How far the board got

Port, on the G4, `--seed 12345`, 15,400 frames, `--play board-start.play`:

| what | port frame | screenshot |
|---|---:|---|
| `w01dll` loads (`--ovllog`: overlay 89) | 4,974 | |
| the board intro — "Toad's Midway Madness" over the map | 5,200 | `mp4-board-intro.png` |
| Toad: "Please enjoy the fun rides, and leave your worries behind!" | 6,000 | |
| the turn-order roll: four blocks, "Great. The order is set! Mario is first!" | 6,200–6,400 | |
| "Hey, let me show you the first Star of the game!" / "The Star is right here. Get here with 20 coins" | 7,700–8,000 | |
| the board proper: dice, movement, item spaces, the ferris wheel | 8,200–11,900 | `mp4-board-map.png`, `mp4-board-dice.png` |
| "You got a Mini Mushroom." | 9,200 | |
| the 4-Player Mini-Game VS screen and the roulette box | 12,000–12,300 | |
| the instruction screen — **Take a Breather** | 12,600 | `mp4-minigame-inst.png` |
| the minigame itself, on the raft | 13,200–14,100 | `mp4-minigame-play.png` |
| "FINISH!" / "PEACH YOSHI WON!" | 14,400–14,700 | |
| the results screen, coins paid out | 15,000 | `mp4-minigame-result.png` |

Dolphin, same walk, same shape, `port/ref/tools/capture.sh 750`:

| what | console frame |
|---|---:|
| the board map | 4,800 |
| the turn-order roll | 5,400–6,600 |
| "The Star is right here" | 7,200 |
| turns, dice, movement | 7,800–11,000 |
| the instruction screen — **Mr. Blizzard's Brigade** | 11,500 |
| the minigame, "START!", the timer, "PEACH WON!" | 12,100–14,200 |
| the results screen | 14,500 |
| back to the board, a second minigame (**Photo Finish**) | 14,800–19,900 |

Every milestone matches, in the same order, with the same on-screen furniture:
the same HUD panels, the same dice numbers rendered as 3D digits over the
board, the same window frames from M4's tiling fix, the same results table.
**The two sides play different minigames** — Take a Breather against Mr.
Blizzard's Brigade — and diverge in the dice they roll, because the board's RNG
is seeded from `OSGetTime` (`BoardRandInit`) and the port's `--seed` and
Dolphin's `CustomRTCValue` are different clocks. That is a determinism gap
between the two *rigs*, not between the two *builds*: each is reproducible on
its own (§12.8), and closing it means giving the port a `--rtc` that matches
the pinned `CustomRTCValue`, which is an M7 job and one line.

One artefact is shared and is the walk's own fault: a later START from the
window lands inside the running minigame and pauses it for a moment — port
frame 13,500, Dolphin frame 13,300. Both recover.

`instDll` and an `m4xxDll` are therefore both proved, which is what §14.4's
locked door was hiding.

### 15.5 Profile

`sample` on the G4, 1 ms, main thread, idle threads dropped. Percentages are
of in-thread busy samples.

**Character select (`mentdll`, overlay 70), 10 s, 7,077 busy samples**

| # | symbol | samples | % |
|---:|---|---:|---:|
| 1 | `transform_and_store` | 1,817 | 25.7 |
| 2 | `indexed` | 1,164 | 16.4 |
| 3 | `read_component` | 973 | 13.7 |
| 4 | `gx_tex_bind` | 770 | 10.9 |
| 5 | `GXCallDisplayList` | 303 | 4.3 |
| 6 | `saveGPR` | 279 | 3.9 |
| 7 | `restGPRx` | 237 | 3.3 |
| 8 | `gldInitDispatch` | 156 | 2.2 |
| 9 | `gldCreateQuery` | 92 | 1.3 |
| 10 | `PSMTXROMultVecArray` | 83 | 1.2 |
| 11 | `glc_texenvi` | 57 | 0.8 |
| 12 | `FaceDraw` | 52 | 0.7 |
| 13 | `tex_content_hash` | 50 | 0.7 |
| 14 | `emit_channel` | 46 | 0.6 |
| 15 | `begin_attr_order` | 41 | 0.6 |

**Toad's Midway Madness (`w01dll`, overlay 89), 12 s, 8,192 busy samples**

| # | symbol | samples | % |
|---:|---|---:|---:|
| 1 | `transform_and_store` | 2,533 | 30.9 |
| 2 | `indexed` | 1,062 | 13.0 |
| 3 | `gx_tex_bind` | 965 | 11.8 |
| 4 | `read_component` | 956 | 11.7 |
| 5 | `GXCallDisplayList` | 307 | 3.7 |
| 6 | `saveGPR` | 236 | 2.9 |
| 7 | `gldInitDispatch` | 196 | 2.4 |
| 8 | `restGPRx` | 178 | 2.2 |
| 9 | `PSMTXROMultVecArray` | 118 | 1.4 |
| 10 | `gldCreateQuery` | 107 | 1.3 |
| 11 | `C_MTXConcat` | 85 | 1.0 |
| 12 | (`libGL` internals) | 76 | 0.9 |
| 13 | `Hu3DMotionExec` | 60 | 0.7 |
| 14 | `glc_texenvi` | 53 | 0.6 |
| 15 | `__sqrt` | 48 | 0.6 |

`transform_and_store` is the top item on both, as §14.3 guessed it would be and
as §13.1 insisted on measuring rather than guessing. The next two,
`indexed` + `read_component`, are the *attribute reader* rather than the
transform, and together they are as large again: the whole vertex path is about
two thirds of the frame on both screens. `gx_tex_bind` at 11–12% is a texture
cache *hit* path, 10.9 million of them a run, and is the obvious next target
after this one.

**Frame rates**, measured as wall clock over a segment of a `--turbo` run
(the port's own `--frames N reached` line):

| segment | frames | wall | fps |
|---|---:|---:|---:|
| boot, the menu walk, and the first 500 frames of the board | 5,600 | 370.8 s | 15.1 |
| the board and its minigame (frames 5,600–15,400 of the same walk) | 9,800 | 670.9 s | 14.6 |
| the reference menu walk on its own (boot to board settings) | 5,150 | 351.6 s | 14.6 |
| the title screen alone (M4 §14.3, unchanged) | — | — | 22.9 |

*(Segments, because `--perf` reports one mean for a whole run; the board figure
is the difference between two runs of the same script with the same seed, one
stopped at 5,600 frames and one at 15,400.)*

The 60 fps target is not met and is not close. The board is a heavier scene
than any menu — 10.9 million primitives and 568 million vertices over
15,400 frames, against 5.8 million and 255 million over the 5,150-frame menu
walk — and it holds roughly the same frame rate, which says the cost is
per-vertex and scales with what is on screen, exactly as the profile says.

### 15.6 The batched AltiVec transform: built, measured, and left switched off

§14.7 item 4 said batched AltiVec, "as the two-phase restructuring §14.3
describes rather than a per-vertex intrinsic", once the profile named the
place. The profile named it, so it was built.

**The restructuring.** `transform_and_store` was called once per vertex from
the attribute cursor, so the modelview and the normal matrix were loaded from
memory for every vertex and nothing could stay in a register. It is now two
phases. Phase 1 (`transform_and_store`) runs per vertex and does only what
does not need a matrix: the colours, including the two constant-per-primitive
overrides that used to be re-tested per vertex, the **model-space** position
and normal, and the raw texture coordinates a texgen will read back. Phase 2
(`finish_vertices`) runs once per primitive from `draw_now`, with the whole run
in hand, and is the only place a matrix is touched.

This is arithmetically identical to what it replaced — the same operations on
the same inputs in the same order per element, only grouped differently — so
the reference md5s did not move and did not need to.

**The vector path.** Behind `#ifdef __ALTIVEC__`, `finish_vertices` builds the
four columns of the modelview and the three of the normal matrix once into
vector registers and keeps them for the whole run. `verts[]` is
`__attribute__((aligned(16)))` and `sizeof(Vtx)` is 96, so every vertex's
position starts on a quadword boundary and two aligned loads give
`(px, py, pz, nx)` and `(ny, nz, colour, colour)`. Each vertex is then three
`vec_madd`s for the position, three for the normal, a `vec_rsqrte` with one
Newton step and a `vec_sel` to keep the zero-length guard, and two `vec_perm`s
to put the results back without disturbing the two colour words that share the
second quadword. Nine multiplies and six adds become three fused ones. Only
`src/gx/gx_draw.o` is compiled with `-maltivec`, deliberately: turning it on
for the whole tree would let GCC vectorise the game's own translation units
too, which is a much bigger change than this one earns.

**And it bought nothing.** Two 12-second `sample` runs on the same board scene,
same seed, same script, same frame:

| | scalar two-phase | AltiVec |
|---|---:|---:|
| `draw_now` (phase 2, inlined) | 2,151 (26.2%) | 2,179 (26.5%) |
| `transform_and_store` (phase 1) | 423 (5.2%) | 447 (5.4%) |
| in-thread busy samples | 8,210 | 8,236 |

Inside run-to-run variance, in the wrong direction. For completeness, the
one-phase M4 code on the same scene was `transform_and_store` 2,533 of 8,192
(30.9%), so the split itself is also free — as it should be, being the same
arithmetic.

**What that measurement actually says** is more useful than a speed-up would
have been. M4 predicted AltiVec would not pay because of loads, permutes and
the store-back *per vec3*; batching removes exactly those, and the answer did
not change. So the cost in phase 2 is not the arithmetic and not the matrix
loads — it is the memory traffic. A `Vtx` is 96 bytes, the run walks a 6.3 MB
array, and 329 million vertices a run is 31 GB of reads and writes on a machine
with a 133 MHz bus. Vector arithmetic over a memory-bound loop is free and
worth nothing.

That points the next step somewhere else entirely: make the vertex smaller
(eight texcoord slots are reserved and one or two are ever used), or stop
staging into `verts[]` at all for the display-list path, which is 92% of the
board's primitives. Neither is an M5 job.

So the path is **kept and switched off**: `port/build-ppc.sh ALTIVEC=1 -j8`
builds it, the default does not. It renders correctly — a screenshot of the
board taken from an AltiVec run is indistinguishable from the scalar one — but
`vec_madd` is fused and `vec_rsqrte` is a different reciprocal square root, so
enabling it *would* mean re-basing the goldens, and there is no reason to pay
that for a change that is not faster.

**One crash, and it is not this.** The 15,400-frame board walk with a cleared
memory card reaches a *second* minigame — `instdll` at frame 13,035, `m425dll`
at 13,325 — and dies there with SIGBUS at 0x04800000, inside the module's own
draw hook (`Hu3DDrawPost` -> a `HU3DMODELHOOK` in `m425Dll.bundle`). The
AltiVec build and the scalar build crash at the *same frame in the same
function*, which is how it was attributed: it is a real port bug in the hook
path, it is not the vertex path, and it is the first thing M6 or M5b should
pick up. The first minigame — Take a Breather, §15.4 — runs to its results
screen on both builds.

### 15.7 What `--gxwarn` names now

Over the 15,400-frame board walk, which is a much bigger sample than M4's menu
walk and the first one that includes a board and a minigame:

| warning | M4 (menu walk) | M5 (board walk) |
|---|---:|---:|
| `TEV: a stage needs two different constants; the first wins` | 1,319,950 | 1,789,688* |
| `GXSetTevSwapMode: a non-identity swap table is ignored` | 135,622 | 135,802* |
| `GXInitSpecularDir: specular is approximated by the diffuse term` | 12,630 | 34,353* |
| `indirect texturing … direct stage only` | 0 | 0 |
| `GXSetTevIndTile: dropped` | 0 | 0 |
| `TEV: a four-input stage with no GL 1.3 combiner; the d term wins` | — | first seen in `m425dll` |

*\* over the 11,000 frames of a board walk, which is the shape of the sample
rather than a comparable total: M4's column is the 5,150-frame menu walk.*

Three distinct degradations over the board itself, and a **fourth** that only
the second minigame reaches: a TEV stage with four different inputs, which GL
1.3's `COMBINE` cannot express at all. So the board adds nothing new and the
minigames add one — which is the more useful reading of that table than the counts. The
two-konst case remains the big one and remains a real GL 1.3 limit (one
`GL_TEXTURE_ENV_COLOR` per unit against GX's four konst registers); §3.9's
`GL_ATI_text_fragment_shader` escape hatch is still where it goes, and still an
M8 decision.

### 15.8 Tooling added

| flag / tool | what |
|---|---|
| `--nanwatch` | the first frame each camera, each `Hu3DData` model and the board's own `boardCamera` turns NaN, with the board camera printed for that frame *and the one before* — which is how §15.2 became one field |
| `port> GX draw: N primitive(s) off-world` | always on, two compares per primitive: how many draws land more than 10,000 units sideways. Zero is the answer §15.1 is judged by. With `--gxwarn`, the first eight are named through the `HU3DDRAWOBJ` behind the loaded matrix |
| `port/tests/mtx_test.c`, `make -C port TARGET=host mtxtest` | the matrix library against itself: every constructor writes all twelve elements, every in-place form matches the out-of-place answer, every `PS*` replacement matches its `C_*` body, `PSMTXReorder`+`PSMTXROMultVecArray` round-trips against `C_MTXMultVecArray`, every `C_VEC*` against arithmetic written out by hand and fed the zero vector |
| `mkgecko.py every` | a metronome as one masked bit test on `GlobalCounter` instead of one five-line block per press, with an optional phase and an optional end frame. Five lines instead of 715, which is the difference between codes Dolphin installs and codes it silently drops |
| `ref/movies/board-start.play`, `ref/movies/board-start.txt` | the two sides of the walk into the board and its first minigame |

### 15.9 What M6 needs

M6 is audio: MusyX above the SAL, the ADPCM stream path first.

1. **The SAL is the whole job and the ADPCM stream is the way in.** `src/msm`
   and `extern/musyx` above the SAL compile already and are linked into the
   binary today — the 23 stubs the boot still hits are all `snd*` and `AI*`
   (§12.9's list, unchanged), and every one of them is a SAL entry point:
   `sndInit`, `sndSetHooks`, `sndStreamAllocEx`, `sndStreamADPCMParameter`,
   `sndStreamARAMUpdate`, `sndStreamActivate`, `sndFXStartParaInfo`,
   `sndFXCtrl`, `sndFXKeyOff`, `sndPushGroup`, `sndOutputMode`, `sndVolume`,
   the three `sndAuxCallback*` reverb/delay pairs, `AIInit`,
   `AIRegisterDMACallback`. The stream path is the smaller half and the one
   with an audible pass/fail: `sndStreamAllocEx` + `sndStreamADPCMParameter` +
   `sndStreamARAMUpdate` + `sndStreamActivate` is a four-call contract over
   data the port already has in ARAM, and it is what the board music goes
   through.
2. **ARAM is already honest** and the stream path depends on it:
   `port/src/audio/aram.c` is a memcpy over 16 MB of host memory and
   `HuAR_MRAMtoARAM`/`HuAR_DVDtoARAM` are exercised on every screen (the board
   alone moves 25 MB through it). `sndStreamARAMUpdate` reads out of the same
   offsets, so nothing new has to be plumbed.
3. **`#########SE Entry Error<SE nn:ErrorNo -110>` is not a port bug and will
   stop on its own.** It is `HuAudFXPlay` failing because `sndFXStartParaInfo`
   is a stub returning zero; the board prints hundreds of them a turn. Worth
   knowing so the first real SAL run is not read as having broken something.
4. **The output device.** SDL2 is already linked and initialised on the G4;
   `port/src/audio/audio_none.c` is the file it replaces. The DSP-command
   interpreter is the far side of M6 and does not block the stream path.
5. **Do not let audio into the frame budget.** §15.5 says the port is at
   ~14.9 fps on the board with the CPU entirely in the vertex path; M6's own
   done-means allows 1.5 ms a frame, which is 2% of the current frame. Mixing
   on the main thread would be visible immediately, so the SAL's mixer belongs
   on the SDL audio callback thread from the first commit, and `--perf` should
   grow an audio row at the same time.

And two things M5 leaves that are not M6:

6. **`gx_tex_bind` is 11–12% of both screens** (§15.5) and it is the *hit*
   path — 10.9 million binds over a board run against 550 cache misses. That
   is the next real speed item after the vertex path, and unlike the vertex
   path it is likely to be bookkeeping rather than arithmetic.
7. **The port and Dolphin do not roll the same dice** (§15.4), because
   `BoardRandInit` seeds from `OSGetTime` and the two rigs pin different
   clocks. A `--rtc` that takes the same value as the reference
   `CustomRTCValue` would make the two walks comparable frame by frame instead
   of milestone by milestone, which is what M7's self-play harness wants
   anyway.

## 16. M6 log — MusyX above a real SAL, first sound on the G4, and the texture-bind hit path *(2026-09-13)*

M6's done-means was three things: board music, minigame music, voices and SFX
all playing; a `--wav` capture that matches a Dolphin capture of the same
segment to the ear; and the audio path under 1.5 ms a frame in `--perf`. Plus
the two things §15.9 left over: the `m425dll` SIGBUS and `gx_tex_bind`.

### 16.1 The route: there is no command list to interpret

§1.8 offered two routes for the DSP and recommended the first: a CPU
implementation of the `dspSlave` command set, "the direct analogue of the N64
ports' `aspMain` interpreter", estimated at 1,000–1,500 lines.

**The premise turned out to be wrong, and the correction makes the job
smaller.** `salBuildCommandList` in `hw_dspctrl.c` is 1,270 lines, and every
one of them is inside `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN`. So is every
write to the `_PB` parameter blocks. On `MUSY_TARGET_PC` the PC arm of that
function is, in full:

```c
#else
  // TODO implement for PC
#endif
```

There is therefore no command list to interpret and no `_PB` to read: the
per-voice state a DSP would have been handed is never assembled. Reproducing
`salBuildCommandList` in order to consume what it produced would mean writing
a marshaller and an unmarshaller for a wire format with no wire in between.

The mixer reads `DSPvoice` and `DSPstudioinfo` directly instead. That is not a
shortcut around the console's semantics — it is the same state, one hop
earlier, and it deletes a whole category of work: the `_PBUPDATE` patch-word
stream (a list of `(pbWordOffset, value)` pairs applied at sub-frame
boundaries) exists *only* because the DSP ran asynchronously and needed its
edits batched. A synchronous mixer that loops sub-frame by sub-frame applies
them by assignment.

So: **route (a), but against the voice state rather than the command list.**

### 16.2 What MusyX needed, exactly

`extern/musyx` compiles for `MUSY_TARGET_PC` **untouched**. 32 of its 34
runtime translation units build clean on the first try, on both clang/arm64
and gcc 14 / PowerPC / the 10.4u SDK. Nothing was mirrored, nothing was
patched, and `patches.txt` did not grow — the decomp's own checkout is
compiled where it sits.

Link the 26 that matter and the undefined set is thirteen symbols:

| group | symbols |
|---|---|
| the audio interface | `salInitAi` `salStartAi` `salExitAi` `salAiGetDest` |
| the DSP | `salInitDsp` `salExitDsp` `salCtrlDsp` |
| the interrupt controller | `hwInitIrq` `hwExitIrq` `hwEnableIrq` `hwDisableIrq` `hwIRQEnterCritical` `hwIRQLeaveCritical` |

That is the whole SAL. `hw_pc.c` in the tree is a skeleton of the first two
groups with every `AIInitDMA` and `AIRegisterDMACallback` commented out and
`salAiGetDest` ending in `return NULL;`, so nothing has ever driven it; it is
dropped from the build rather than patched, because a SAL that has to be
honest about a DMA ring, a deterministic tick and a software mixer shares
almost no code with it.

**The frame geometry, all read out of the source rather than assumed:**

| | |
|---|---|
| `DMA_BUFFER_LEN` | `0x280` bytes = 160 frames of interleaved stereo s16 |
| rate | 32000 Hz, so one AI buffer is 5 ms and the interrupt fires at 200 Hz |
| `synthInfo.numSamples` | `0x20` = 32 samples — the *sub*-frame, not the frame |
| the frame | 5 sub-frames × 32 = **160**, which is why `snd_handle_irq` runs `seqHandle`/`synthHandle` five times per interrupt and why every volume ramp divides by 160 |
| the ring | four buffers; the DSP fills the one two slots ahead of the one being played, so output latency is 2 × 5 = 10 ms |
| the studio buses | `main[2]`, `auxA[3]`, `auxB[3]`, each 480 `s32` = three de-interleaved 160-sample channels (L, R, surround), double-buffered by `salFrame` and triple-buffered by `salAuxFrame` |

### 16.3 The one thing that was not in the plan: MusyX's ARAM layer is stubbed too

`hw_aramdma.c` has a full Dolphin implementation — a bump allocator for
samples growing up, stream buffers growing down, a 16-deep ARQ transfer queue,
64 stream-buffer slots with used/free/idle lists. Its PC arm is eleven empty
bodies: `aramStoreData` returns `NULL`, `aramAllocateStreamBuffer` returns 0,
`aramGetStreamBufferAddress` returns 0, `aramUploadData` does nothing.

Those eleven functions **are** the ADPCM stream path §15.9 item 1 named as the
way in: `sndStreamAllocEx` → `hwInitStream` → `aramAllocateStreamBuffer`;
`sndStreamARAMUpdate` → `hwFlushStream` → `aramUploadData`;
`hwGetStreamPlayBuffer` → `aramGetStreamBufferAddress`. With them stubbed the
music has nowhere to live, and it would have failed silently — every call
returns a plausible zero.

So `hw_aramdma.c` is dropped too and the Dolphin arm is ported onto the port's
flat 16 MB ARAM (`port/src/audio/musyx_aram.c`, 589 lines). The allocator is
faithful — including a reclaim-sweep bug in the original, reproduced
deliberately rather than quietly fixed. The transfer queue is not: on a host
where "DMA" is a `memcpy` that has already finished, `aramUploadData` copies
and calls the completion callback before returning, and `aramSyncTransferQueue`
is empty. `stream.c` only requires that the callback fire strictly after the
bytes land, which synchronous completion satisfies with less latency, not more.

One detail worth recording: `aramGetZeroBuffer()` on the console returns
`ARGetBaseAddress()` — a region of guaranteed-zero ARAM the DSP reads when a
non-looping voice runs off its end. The port reserves 1,280 bytes at the base
of MusyX's region and starts the sample heap above it, so a voice that
overruns reads silence rather than someone else's samples.

MusyX owns `[0, 0x808000)` (8.03 MB); `HuAudInit` sets
`msmAram.aramEnd = HU_AMEM_BASE` and `src/game/armem.c` owns everything above.

### 16.4 The tick, and why the mixer is on the game thread

§15.9 item 5 said the mixer belongs on the SDL callback thread so audio does
not enter the frame budget. **It is on the game thread instead, and the reason
is the same reason the flag `--seed` exists.**

MusyX's per-interrupt work is not just mixing. `snd_handle_irq` runs the
*sequencer* five times per 5 ms frame, and the sequencer fires the game's own
callbacks, allocates and steals voices, and reads state the game thread is
writing. Run that on SDL's callback thread and the number of sequencer steps
between two video frames becomes a function of the host's audio clock — which
is the definition of non-determinism. `--seed 12345 --play board-start.play`
would stop reproducing, and it is the only way this port is tested.

So the cadence is driven from the retrace gate by an integer accumulator:
32000 × 100 units of credit per retrace, 160 × 5994 spent per frame, no
floating point and no clock. At 59.94 Hz that is 3.3367 frames a retrace,
delivered as a fixed 3-3-4-3-3-4 pattern. The SDL callback keeps the one job
that is genuinely its own: draining a 64 KB ring and padding with silence.

The cost of that decision is that the mix is in the frame, so it has to be
cheap and it has to be *visible* — which is why `--perf` grew an `aud` phase
in the same commit, subtracted from `game` rather than hidden inside it.

### 16.5 First sound

`g4 run --seed 12345 --frames 1400 --perf --wav mp4-boot.wav`, no script, the
boot straight to the title screen:

```
port> MusyX SAL: 32000 Hz, 4 x 640-byte AI buffers (20 ms), 32-sample sub-frame
port> musyx_mix: CPU mixer up, 50 voices, 1 studios, 32000 Hz
port> musyx_aram: region [0x000000, 0x800000) zero_buf [0x000000, 0x000500)
port> audio: first non-silent sample at retrace 726 (12.03 s of mixed audio)
port> musyx_aram: stream buffers active=2 peak=2 of 64 slots
port> musyx_aram: bytes uploaded=475904 -- rejected: store=0 stream=0 upload=0
port> musyx_mix: 4657 frames mixed, 2 voices started, 0 voices ended,
                 peak |sample| 18888, 0 clamped ARAM reads
```

Two things in there are the whole milestone. `stream buffers active=2` and
`bytes uploaded=475904` say the four-call ADPCM contract
(`sndStreamAllocEx` → `ADPCMParameter` → `ARAMUpdate` → `Activate`) completed
against real data. `0 clamped ARAM reads` says the mixer never once had to
refuse an address — every sample pointer the game handed it was inside the
region it claimed.

**And `#########SE Entry Error<SE nn:ErrorNo -110>` is gone.** §15.9 item 3
predicted it would stop on its own once `sndFXStartParaInfo` was real. It did.

### 16.6 The capture, against Dolphin's

Dolphin dumps audio: `DumpAudio = True` under `[DSP]` in the pinned user
directory writes `Dump/Audio/*_dspdump.wav`, the DSP-HLE mix at 32028 Hz — the
same mix MusyX asks the hardware for, so it is a like-for-like reference and
not an approximation. Its companion `*_dtkdump.wav` is the AI streaming path
and is **silent from end to end**, which is a useful fact in its own right:
Mario Party 4 puts everything, music included, through MusyX, and the port's
`AISetStreamVol*` no-ops are correct rather than merely harmless.

`port/tools/wavstat.py` describes a capture in the terms a claim can be judged
by. The same ten seconds from each side, aligned on first sound:

| | port, on the G4 | Dolphin |
|---|---:|---:|
| peak | 18,888 (−4.8 dBFS) | 24,122 (−2.7 dBFS) |
| rms | 3,583 | 3,403 |
| steps over half full scale | **0** | **0** |
| first sound | retrace 726 | retrace ~656 |

**The loudness matches to 5%** and neither side has a single sample-to-sample
step large enough to be a click. The spectra agree in shape — both are
bass-dominant with almost nothing above 2 kHz — but the port has visibly less
energy between 250 Hz and 4 kHz. Two candidates, in order of likelihood: the
mixer resamples with linear interpolation rather than the console's 4-tap
polyphase filter, which is a low-pass; and the two windows may not be the same
*musical* moment, because the port skips the opening movie (§12.5) and Dolphin
does not, so the 70-retrace offset in first sound may be a different piece of
music rather than the same one late. That is not resolved and should not be
claimed as resolved.

### 16.7 The board, and the two bugs only hardware could find

The full walk runs with audio: `bootdll` -> `modeseldll` -> `mentdll` ->
`w01dll` (Toad's Midway Madness) -> `instdll` -> `m456dll` -> `resultdll` ->
back to the board. 15,400 frames, a complete turn, a minigame and its results,
with music and SFX throughout. `--seed 12345` still reproduces, and the walk
reaches the same places it did in §15.4 — audio did not perturb it.

**`#########SE Entry Error<SE nn:ErrorNo -110>` went from hundreds a turn to
398 over the whole 15,400-frame run**, and 0 over a 1,400-frame boot. §15.9
item 3 was right that it would stop on its own once `sndFXStartParaInfo` was
real; the residue is genuine failures (an SE whose group is not loaded), not
the stub.

Then two bugs that only the hardware could have found, because the
little-endian host cannot get `msmSysInit` past the sound bank at all (§9.6).

**One: the sample data was never in ARAM.** The first board run reported
**104,826,913 clamped ARAM reads** — about 2,000 per 160-sample frame. A bare
count says nothing, so the clamp learned to describe itself, and one menu walk
named the cause in two lines:

```
ARAM read refused at 0x02c7fb50 ... voice: compType 0, addrBase 0x02c7fb50,
  curSample 2 of length 51371, loop off, frameOffset 2
OSInit: MEM1 24 MB [0x2000000, 0x3800000)
```

0x02c7fb50 is inside MEM1. The cause is one line and it is a `#if`
(hardware.c:566, inside `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN`):

```c
*((u32*)data) = (u32)aramStoreData((void*)*((u32*)data), len);
```

`dataAddSampleReference` sets `sdir->addr = offset + base`, a main-memory
pointer into the loaded bank, and `hwSaveSample` is what normally replaces it
with the ARAM address the bytes were copied to. It never runs here. So every
sequenced sample reached the mixer as a MEM1 pointer used as an ARAM offset —
while the *stream* path was untouched, because streams get their address from
`aramAllocateStreamBuffer`, which §16.3 really did implement. That is exactly
why the title music was right and everything sequenced was reading nothing.

The mixer resolves each address once at voice start now: below
`PORT_ARAM_SIZE` is an ARAM offset, inside `[port_mem1_lo(), port_mem1_hi())`
is a host pointer used as it stands, anything else refuses the voice rather
than guessing. Nothing is copied — a CPU mixer has no reason to move 8 MB into
ARAM to satisfy a constraint that existed only because the DSP could not read
main memory. Clamped reads: **104,826,913 -> 0**.

A hexdump at the resolved address confirmed it rather than assuming it:
`34 95 2f 1f ff 02 1f f0 ...` — header byte 0x34, predictor 3, scale 4. Valid
DSPADPCM.

**Two: and then the mix pinned at full scale.** RMS went from 3,057 to 16,203
and the capture filled with clicks. `adsrHandle` (synth_adsr.c:153) returns a
volume and a delta through two `u16*`, and they are not the same kind of
number:

```c
*adsr_start = old_volume >> 16;            /* 0..0x8000, UNSIGNED */
*adsr_delta = -(-currentDelta >> 21);      /* SIGNED, in a u16    */
```

A voice *starts* at 0x8000 (hw_dspctrl.c:895), so reading the volume as `s16`
turns unity gain into minus unity. Worse, every release ramp's delta is
negative: added unsigned, a voice fading out by -3 per sample instead ramps
*up* by 65,533 per sample and clips the rest of the mix out with it.

The three states, measured on the G4 over the same 6,000-frame walk, against
Dolphin's 3,400–4,400 RMS for comparable material:

| | before addressing | after addressing | after envelope |
|---|---:|---:|---:|
| capture RMS | 3,057 | 16,203 | **3,688** |
| steps over half full scale | 11,498 | 434,234 | **25,306** |
| clamped ARAM reads | 104,826,913 | 0 | 0 |

**What is still wrong, precisely.** 25,306 discontinuities in 100 seconds is
not clean. They are not spread evenly: seconds 12–42 carry exactly one, and
then 43–45 burst (603, 5,598, 4,709) and 46 onward settles to a steady ~300 a
second. Something that starts around frame 2,580 introduces a persistent
defect; the title music and the early menus before it are clean. The two
candidates already on the record are the host depop path
(`DSPhostDPop`/`hostDPopSum`), which is not implemented and whose named
symptom is exactly a click on voice cut-off, and the linear resampler standing
in for the console's 4-tap polyphase filter. Neither is confirmed. This is the
first thing M6b or M7 should pick up, and the localisation above is where to
start.

**Cost.** `--perf` on the G4 over the 6,000-frame walk, with 23 concurrent
voices at peak:

| phase | mean | median | p95 | worst |
|---|---:|---:|---:|---:|
| game | 7.50 | 9.73 | 10.75 | 918.26 |
| gx | 43.41 | 47.81 | 87.98 | 355.00 |
| **aud** | **1.61** | 1.72 | 3.13 | 5.08 |
| frame | 53.14 | 60.41 | 98.58 | 969.79 |

M6's budget was 1.5 ms a frame and the mean is **1.61 ms** — over it, by 7%.
Honestly: missed, narrowly. The mixer's own counter puts one 160-sample frame
at 486 us and there are 3.34 of them per video frame, which is where the
figure comes from. The cheapest remaining win is the one the console itself
used and this mixer does not: `salCheckVolErrorAndResetDelta`'s bus-liveness
policy is implemented, but the 4-tap resampler is not, so every voice pays a
multiply-and-shift per sample that a table lookup would replace.

### 16.8 The `m425dll` SIGBUS: not reproduced, and the reason is worth more than the attempt

§15.6 left a SIGBUS at 0x04800000 in a `HU3DMODELHOOK` inside `m425Dll.bundle`,
reached by the 15,400-frame board walk at frame 13,325, on both the scalar and
the AltiVec builds. **Four board runs this milestone did not reach that module
once**, and the reason is the useful finding:

**`--seed` does not pin which minigame comes up.** The four runs, all
`--seed 12345 --play board-start.play`:

| run | card state | second minigame |
|---|---|---|
| 1 | the save the M5 session left | `m456dll` |
| 2 | `--nocard` | never reached the board — the walk depends on the card flow |
| 3 | card file deleted | `m456dll` |
| 4 | as run 3 left it, audio now on | `m456dll` |

The roulette reads the save file's played-minigame set as well as the RNG, and
the save file is rewritten by every run, so "the same seed" is not the same
experiment twice. That is an M7 problem with an M7 solution (§16.10 item 3):
a harness that can set `GWPlayerCfg[i].diff` can set the minigame directly,
and then reproducing a crash in a named module stops being a twenty-minute
dice roll.

**What the source review did settle.** Both of `m425dll`'s draw hooks build a
display list at runtime with `GXBeginDisplayList` and replay it with a
descriptor set immediately before, and the interesting one
(`main.c:1495 fn_1_57D4` → `fn_1_5C20`) is also where §15.7's "four-input TEV
stage" lives. The suspicious-looking thing there is real and is *not* a bug:
the recorded stream writes five indices per vertex in the order
`GXPosition1x16, GXColor1x16, GXTexCoord1x16, GXNormal1x16, GXNormal1x16`,
while the descriptor at replay is POS, CLR0, TEX0, TEX1, NRM. The writer names
do not match the slots — but they are not supposed to. GX assigns indices by
the descriptor's own fixed attribute order (POS, NRM, CLR0, CLR1, TEX0…), and
under *that* order every index lands in bounds, including the one that looks
worst: the `NRM` array is a single `Vec` (`unk_1C = malloc(unk_2A * sizeof(Vec))`
with `unk_2A == 1`) and the index that reaches it is the constant 0, because
the writer that supplied it was `GXColor1x16(var_r30)`. The port's
`begin_attr_order()` (gx_draw.c:345) uses exactly that order, so the port
agrees with the hardware here and this is not the fault.

**And one hypothesis is now disproved rather than merely unconfirmed.**
0x04800000 looked like a read one element off the end of a small allocation
sitting at the top of the heap — the first byte past MEM1. `OSInit` now prints
MEM1's real bounds, and on this machine they are **[0x2000000, 0x3800000)**.
0x04800000 is 16 MB above the top of MEM1, so it is not that. It is not ARAM
either (ARAM is a separate `calloc`). Whatever it is, it is not "one past the
arena", and the next person should not spend the hour this one did on that
idea.

The diagnostic that would have answered it in one line is now in the tree
anyway (§16.7's `aram_clamp_report` is the audio-side twin of it, and the MEM1
bounds are the graphics-side one), so the next occurrence describes itself.

### 16.9 `gx_tex_bind`: the hit path stops hashing, and stops scanning

§15.9 item 6: `gx_tex_bind` at 11–12% of the frame on both screens, and it is
the *hit* path — 10.9 million binds over a board run against 550 misses. The
guess was that it would be bookkeeping rather than arithmetic. It was both, in
four places, all of them paid on every hit:

1. a linear scan of the whole cache looking for an EFB-copy entry;
2. `seen_before()`, a linear scan of up to **4,096** image pointers, run on
   every bind of any texture larger than 1 KB;
3. the sampled content hash — 1 KB of FNV, plus the palette;
4. a second linear scan to find the slot.

The two scans become one hash-table lookup chained through the cache array
itself and keyed on the image address, with EFB entries keeping their
priority. `seen_before` disappears rather than being sped up: "has this buffer
ever been hashed in full" is already answered by whether the key has a slot —
and answered *more* precisely, because the key is the whole
`(image, format, w, h, tlut)` tuple rather than a bare pointer, so an address
later reused for a different format gets its own exhaustive baseline instead
of inheriting an unrelated one.

The hash itself moves off the hit path behind a **validation epoch**. The
signal is the frame counter, and deliberately *not* `GXInvalidateTexAll`: this
file already recorded that `HuSprDispInit` fires that call on every sprite
pass, so honouring it would revalidate everything several times a frame and
buy nothing. A slot is re-verified once per frame; a hit inside that epoch is
a lookup and a bind, touching no texel bytes at all.

That trades exactness in one specific, bounded way: **an in-place rewrite that
is rebound within the same frame is not caught until the next frame.** Never
more than one frame, never accumulating. `--texhash-full` now bypasses the
epoch as well as the sampling, and a new `--texvalidate-every-bind` bypasses
only the epoch, so a suspected staleness bug can be bisected between the two.

One bug fell out on the way: `gx_tex_copy` created EFB slots without inserting
them into the new index, which would have turned every EFB bind into a miss
and a decode of a buffer with nothing in it. Caught by a harness, not by the
screen, which is the useful part.

**Byte-identical**: `--gxdemo` writes the same `gxdemo.ppm`
(`577735b51beb3112fe63ad75ebe50a72`) before and after.

**On the G4**, the same 5,600-frame segment §15.5 measured at **15.1 fps** now
runs at **19.9 fps** — a 32% gain, audio off so the two are comparable. Over
that segment the cache took 4,020,081 hits against 518 misses and revalidated
486,822 times, so **87.9% of binds touch no texel bytes at all**; 448 MB were
hashed where the old code would have sampled about 4.1 GB.

On a synthetic host benchmark shaped like the profile's reuse — 300 textures,
90% of binds landing in a 40-texture hot set:

| | before | after |
|---|---:|---:|
| ns per bind | 579.7 | **96.3** |
| bytes hashed | 584 MB | **119 MB** |
| hash computations | 1,707,987 | **315,921** |

### 16.11 Tooling added

| flag / tool | what |
|---|---|
| `--wav FILE` | the mix as a 32 kHz stereo WAV, written on the producer side *before* the output ring, so it is complete whatever the device does. The only way an audio claim can be checked at all on a machine nobody can listen to |
| `--mute` | every voice started, decoded, advanced and retired as usual; silence emitted. Separates "the audio path broke it" from "the audio broke it" without moving the game's timing by one frame |
| `--audiolog` | one line per second of mixed audio: peak sample and ring fill. Those two numbers separate the three ways this can be wrong — nothing being mixed, something being mixed but the ring starving, or both fine and the fault downstream |
| `aud` in `--perf` | the mix is on the game thread by design (§16.4), so it is subtracted from `game` rather than hidden inside it |
| `port/tools/wavstat.py` | describes a capture in the terms a claim can be judged by: first sound as a retrace number, peak and RMS, worst sample-to-sample step and how many exceed half full scale, and an octave-spaced Goertzel spectrum. `--compare A B` lines two up. It is what turned "it sounds wrong" into "RMS 16,203 against Dolphin's 3,400" |
| Dolphin `DumpAudio` | `DumpAudio = True` under **`[DSP]`** (not `[Movie]`) in the pinned user directory writes `Dump/Audio/*_dspdump.wav`, the DSP-HLE mix at 32028 Hz — like-for-like with what MusyX asks the hardware for. Its `*_dtkdump.wav` companion is silent end to end for this game, which is how we know the AI streaming path is genuinely unused |
| `musyx_mix`'s clamp report | names the voice behind a refused read — compType, resolved memory kind, base, position, length, loop bounds, pitch, resampler — for the first eight, then counts. One menu walk with it turned 104 million anonymous clamps into a one-line diagnosis |
| `OSInit` prints MEM1's bounds | a fault address is otherwise a riddle; §16.8 spent an hour on a hypothesis that one printed range disproved |

### 16.10 What M7 needs

1. **`--rtc` is now the cheapest thing on the list and it unblocks two
   others.** §15.9 item 7 wanted it so the port and Dolphin roll the same
   dice. M6 adds a second reason: §16.6's spectral comparison is inconclusive
   partly *because* the two rigs are not at the same musical moment, and a
   `--rtc` that takes Dolphin's pinned `CustomRTCValue` would make the audio
   comparison frame-for-frame instead of window-for-window. It is one value
   into `BoardRandInit`'s `OSGetTime`.

2. **The self-play harness now has a second thing to assert.** §4's M7
   done-means is "`--autoplay` completes a full four-player board unattended
   and prints one result line". That line should carry the audio's own
   invariants — frames mixed, max concurrent voices, clamped ARAM reads,
   voices refused — because all four are zero-or-monotonic on a healthy run
   and none of them is visible in a screenshot.

3. **The minigame roulette is the obstacle to reproducing a named minigame,
   and M7 is where it gets solved.** §16.8 spent four board runs trying to
   land on `m425dll` and got `m456dll` and `Take a Breather` instead, because
   the selection depends on the save file's played-minigame set as well as the
   RNG. A harness that can set `GWPlayerCfg[i].diff` can equally set the
   minigame directly, and then "reproduce the crash in module X" stops being a
   twenty-minute dice roll.

4. **The `aud` phase belongs in the regression line.** `--perf` now reports it
   and M6's budget was 1.5 ms; a `regress` mode that replays input scripts
   should fail on a budget regression, not only on a pixel one.

5. **Determinism has a new input and it should be asserted, not assumed.**
   The audio tick is an integer function of the retrace count and nothing
   else, which is what makes `--seed` still work — but that is a property of
   the code, not a tested one. Two runs of the same seed with `--dumpframe`
   over a spread, md5-compared, is the assertion, and it costs nothing to run.

---

## 17. M7 log — a run that repeats itself, a game that plays itself, and the click that was two clicks *(2026-09-13)*

M6 ended with four things it could not measure and one it could not reproduce.
M7's job was to make the rig capable of answering all five, and the order was
forced: nothing else is worth measuring until two runs of the same command
produce the same bytes.

### 17.1 `--rtc`, and the second input nobody had noticed

§15.9 item 7 and §16.10 item 1 both asked for `--rtc` and both called it one
line. It is one line. `--rtc SECS` is `--seed` written in the units the
reference rig is already configured in — Unix seconds, the number Dolphin's
`CustomRTCValue` holds — converted to 40.5 MHz ticks since 2000-01-01 and used
as the deterministic clock's origin. `--rtc dolphin` is the pinned reference
value, 1041472800.

The interesting part is what it revealed about the *other* input.

§16.8 spent four board runs failing to reach `m425dll` and concluded that the
roulette reads the save file's played set. That is true, and it means the
memory card is an input to every run — an input that every run also *writes*.
So before M7 there was no way to run the same experiment twice, and "the same
seed" was a statement about one of two inputs.

`--card FILE` points slot A at a named image and `--freshcard` formats it at
boot. With both, plus `--rtc`, a run is a function of its command line.

### 17.2 Determinism, proved rather than asserted

§16.10 item 5 asked for exactly this test and said it costs nothing. It cost
two runs.

Both runs, on the G4, the same command, audio **on** — which is the part that
was never tested, because M6 asserted by design that the audio tick is an
integer function of the retrace count and never checked:

```
g4 run --rtc dolphin --card ~/mp4-det.raw --freshcard \
       --play board-start.play --turbo --frames 2000 \
       --dumpframe 400,800,1200,1600 --shotdir ~/detN --wav ~/detN.wav
```

| artefact | run 1 | run 2 |
|---|---|---|
| `frame-00400.ppm` | `2e439de0b45927a6905565c49c9130c0` | *identical* |
| `frame-00800.ppm` | `45da1034bf9692bb6dc04a74ac665dd6` | *identical* |
| `frame-01200.ppm` | `b206514268de6a18416e7f725e0ff7aa` | *identical* |
| `frame-01600.ppm` | `27faa18da48f3d0158c79d9c4d748931` | *identical* |
| the whole `--wav` capture | `cedc7196840becbf0e63cf40639a58f5` | *identical* |

Four frames spread over the boot and the menus, and 5,116,204 bytes of mixed
audio, byte for byte. §16.4's design claim — the mixer is on the game thread
so that the number of sequencer steps between two video frames is a function
of the frame number and not of the host's audio clock — is now a tested
property rather than an argument.

*(A note for the next person: `--dumpframe` counts **presented** frames and
`--frames` counts **retraces**, and the two are not equal. A run asked for
`--frames 2400 --dumpframe ...,2400` writes three files, not four. Ask for a
dump point comfortably inside the run.)*

### 17.3 Do the port and Dolphin now deal the same minigame? No — and the
### reason is not the clock

They share the clock's *origin* and they still diverge, because neither RNG
seed site reads the origin. `BoardRandInit` reads `OSGetTime()` **at board
setup**, which is the origin plus however many ticks have elapsed since boot,
and the two rigs do not agree about that: the port skips the DVD seek and the
opening movie and reaches the title around frame 700 against the console's
380. Three hundred frames is five seconds is 200 million ticks, and
`boardRandSeed = OSGetTime()` turns five seconds into a completely different
sequence.

So `--rtc` does what §16.10 item 1 wanted for the *audio* comparison — the two
rigs are on the same clock, and a capture from each is comparable
window-for-window in a way it was not — and it does **not** close §15.4's dice
gap on its own. What would: applying the offset as well, so that the port's
`OSGetTime` at `BoardRandInit` equals the console's. That is measurable (the
port's own `--ovllog` names the frame `w01dll` loads on each side) and it is
one subtraction, but it is a different flag from this one and it should be
called what it is.

The harness makes the question much less urgent, which is why it is being left
here rather than chased: `--minigame` reproduces a named minigame directly,
and that was the only thing the dice gap was actually blocking.

### 17.4 The harness: park the state, do not press the button

The two Snowboard Kids ports' `menu_nav.c` is the model, and its rule is the
whole of `port/src/debug/selfplay.c`: **name the live screen, and park the
state the game would have set, rather than driving the buttons that would have
set it.**

`--play` is the other way, and `port/ref/movies/board-start.play` is the
receipt: 300 lines of metronome, five captures to place two STARTs (§15.3),
and a walk that breaks the moment a screen's entry animation changes length.
Parking costs a line per fact and does not care how long an animation is.

| flag | what it parks | where the game would have set it |
|---|---|---|
| `--com4` | `GWPlayerCfg[i].iscom = 1`, mirrored into `GWPlayer[i].com` | `mentDll/main.c:824-834`, the game's own attract loop, which sets exactly these fields |
| `--turns N` | `GWSystem.max_turn` | `BoardPartyConfigSet`, `board/main.c:382` |
| `--minigame` | `GWSystem.mg_next`, plus `GWPlayerCfg[i].group` for the chosen type | `mg_setup.c:292` and `E3setupDLL/mgselect.c:219-250` |
| `--status` | nothing; reads `omcurovl`, `GWSystem`, `GWPlayer[]` | — |
| `--stuckwatch` | nothing; watches `omcurovl` + `omovlevtno` | — |

Nothing in `src/` is patched. `port/patches.txt` did not grow a line, because
every one of these is a global the port already links.

**`--com4` is worth more than it looks.** It is not only "four CPUs"; it is
what makes the game dismiss its own instruction screen. `instDll/main.c:281-294`
counts the CPU players and auto-starts after 60 frames when the count is four
— which is precisely the START that cost §15.3 three captures to place. With
`--com4` there is no START to place, on any screen, ever.

**`--minigame` took two wrong turns on paper before the right one.** Both are
worth recording because both look better than the answer:

1. *Bias the candidate pool.* `GWSystem.mg_list = 2` makes `DetermineMGList`
   draw only from `GWGameStat.mg_custom[]` (`mg_setup.c:331`) **and** skip the
   recently-played filter (`:359`), so a one-entry custom pack looks like
   exactly the right lever. It hangs the game. The draw loop
   (`mg_setup.c:352-376`) runs `while (var_r30 < arg0->field01_bit0)` and only
   advances `var_r30` on a candidate whose `name_mess` it has not already
   taken. With a pool of one and a roulette wanting four, there is no exit.
2. *Call `omOvlCallEx` directly*, the way `selmenuDll`'s debug launcher does
   (`selmenuDll/main.c:696-720`). That is the right call from inside a
   `HUPROCESS`; from the retrace gate it re-enters the object manager from
   outside every process the object manager knows about.

The answer is duller and safe: let the roulette run, and overwrite
`GWSystem.mg_next` before `instDll`'s `ObjectSetup` reads it
(`instDll/main.c:60`). The harness also logs what the roulette *would* have
dealt, so the override costs no information:

```
port> roulette: frame 12163 dealt mg 456 (m456dll, type 0)  -- overridden by --minigame
```

The team split travels with the override, because a 2-vs-2 module reads
`GWPlayerCfg[i].group` and m425 is a 2-vs-2.

**The stuck-screen watchdog costs one compare a frame.** The N64 ports named
the live overlay through `dladdr` and paid 27% of the frame for it, which is
the lesson `sbk-port-speed-lesson` records. Here the live screen is an
integer (`omcurovl`) and the name table is re-derived from the same
`include/ovl_table.h` the game builds its own `_ovltbl` from, so the two
cannot drift and the watch is a comparison:

```
port> STUCK: frame 9840, 91 s with no scene change.  live screen w01dll
  (overlay 89, event 0, previous instdll), turn 3/10, mg_next 425
```

**The status line** carries what a screenshot cannot, including the four audio
invariants §16.10 item 2 asked for:

```
port> status f5520    w01dll  board 0 turn 1/10  mg 425 (m425dll)
      coins/stars 0/0c 0/0c 0/0c 0/0c  aud 0.55 ms  10.8 fps
```

### 17.5 The clicks: two defects, both implemented, both switchable

§16.7 left 25,306 sample-to-sample steps over half full scale in 100 seconds
and named two suspects without confirming either. Both are now built, and
both are on by default and switched *off* by a flag — `--nodepop`,
`--resample1` — so an A/B is one word on a command line rather than a rebuild.

**Depop is the one M6 called "not implemented" and it is the more interesting
of the two, because the defect is not in the mixer's arithmetic at all.** A
voice that stops does not stop at zero: it stops at whatever its last output
sample times its bus gain happened to be, and every later sample of that frame
is missing exactly that value. That is a step, and a step is a click, and no
amount of care inside the mix loop can prevent it — the decision to end the
voice is made between frames.

The console's answer, which the port now copies, is not to fade the voice but
to inject the step *back* into the bus as a DC offset and then ramp that to
zero: `AddDpop` (`hw_dspctrl.c:626`) accumulates, `DoDepopFade` (`:631`) ramps
at no more than 20 units a sample, `HandleDepopVoice` (`:644`) is called on
every kill path, and `DSPstudioinfo::hostDPopSum` — a field this port has
always had in its headers and never once written — is where they meet. One
discontinuity of arbitrary size becomes 160 samples of at most 20.

One deliberate difference from the console: below 160 units `DoDepopFade`
computes a delta of zero and leaves the offset in the bus **forever**. A
permanent DC under 160 units is inaudible, but it would also make two runs of
the same seed differ in their accumulated residue rather than in anything
audible, which §17.2 has just made a property worth protecting. The port
retires it in one frame instead.

**The resampler.** The mixer interpolated linearly between two samples; the
console used a 4-tap polyphase filter, and `_PBSRC`'s `u16 last_samples[4]`
(`musyx/include/musyx/voice.h:98`) is the state it kept for it. Linear
interpolation is both a low-pass — §16.6's missing energy between 250 Hz and
4 kHz — and a first-derivative discontinuity at every sample boundary.

**The coefficients are not in this tree and were not taken from an emulator.**
`dsp_import.c` is the assembled `dspSlave[]` ucode as hex; there is no
symbolic SRC table anywhere in `extern/musyx`, and §5.1's rule is that Dolphin
is a reference *runtime* and never a source of code. So the port supplies a
kernel of the same shape and the same cost: a Catmull-Rom 4-point cubic over
256 phases, tabulated in Q14. Q14 rather than Q15 buys a bound rather than a
hope — Catmull-Rom's coefficients sum to 1 and their absolute values sum to at
most 1.25, so the accumulator provably stays in `s32` and the inner loop needs
no saturating add. Four 32x16 multiplies replace one 64-bit multiply and a
shift, which on a 32-bit PowerPC is not obviously the more expensive of the
two.

`MixVoice`'s two-sample lookahead becomes the four-sample window, which is the
change its own comment had been anticipating since M6.

**And a counter, so the claim does not need a capture to check.**
`--clickstat` applies `wavstat.py`'s exact rule — a left-channel
sample-to-sample step over half full scale — in process, so a soak reports the
number a `--wav` would have been measured at, and the report names which
resampler and which depop setting produced it.

### 17.6 What the harness costs, and what the board now measures

`--status`' `aud` column, over the 119 seconds of the `--minigame m425` run
below (boot, the menus, and the first turn of Toad's Midway Madness), with the
4-tap resampler and the depop ramp both on:

| | mean | median | p95 | worst |
|---|---:|---:|---:|---:|
| `aud`, M6 (§16.7, a 6,000-frame walk) | 1.61 | 1.72 | 3.13 | 5.08 |
| `aud`, M7 (this run, 119 s) | **1.53** | 1.78 | 2.61 | 2.84 |

The two segments are not the same segment, so this is a comparison and not a
controlled A/B, and it should be read as one: the mean moved the right way and
the tail moved a long way the right way (p95 3.13 -> 2.61, worst 5.08 -> 2.84),
which is what replacing a 64-bit multiply per sample with a table lookup and
four 32-bit ones would be expected to do on this machine. **1.53 is still over
M6's 1.5 ms budget**, by 2%, and calling that "met" would be a rounding
error dressed as a result. It is not met; it is much closer, and the variance
is gone.

The board's own numbers are the more useful reading: `aud` on `w01dll` sits at
0.55-3.00 ms depending on how many voices the turn has running, and on the
character-select screen — the heaviest — it is 1.3-2.7. The mixer's cost is a
function of concurrent voices and nothing else, which is what a per-sample
inner loop should look like.

### 17.7 The `m425dll` SIGBUS: reproduced on demand, and it is the other hook

§16.8 spent four board runs failing to reach `m425dll`. With `--minigame` it
took one, and the log says exactly what happened:

```
port> --minigame m425: parking the roulette on m425dll (mg 425, type 2)
...
port> roulette: frame 10501 dealt mg 412 (m412dll, type 0)  -- overridden by --minigame
port> status f10800   instdll   board 0 turn 1/10  mg 425 (m425dll)
      coins/stars 10/0c 13/0c 13/0c 13/0c  aud 2.04 ms  19.6 fps
*** port: fault: signal 10 at address 0x4800000
    in   _epilog  (m425Dll.bundle)
    backtrace:
      #0  _epilog + 49068  [m425Dll.bundle]
      #1  Hu3DDrawPost + 1772  [isle]
      #2  Hu3DExec + 1380  [isle]
```

The board dealt Snow Throw; the harness parked Air Dossun; `instDll` dismissed
its own instruction screen because all four players are CPU; and the module
crashed. **A twenty-minute dice roll is now one flag**, which was §16.10 item
3's whole point.

**Three things the reproduction settles.**

*It is the other hook.* §16.8 reviewed `main.c:1495 fn_1_57D4` -> `fn_1_5C20`,
the display-list hook, at length and cleared it — correctly, as it turns out.
Symbolising the fault against the bundle's own `nm` (only `_prolog` and
`_epilog` are exported, so the arithmetic is
`pc - _epilog + nm(_epilog)`) puts it at bundle offset `0xc318`, inside
**`fn_1_E914`**, `thwomp.c:1810` — the *Thwomp deformation* hook, a completely
different function reached through the same `Hu3DDrawPost`.

*It is a store from a runaway pointer walk, not a bad base.* The faulting
instruction is

```
0000c314	stw	r23,0xfffc(r17)
0000c318	stw	r24,0x2c(r17)      <-- SIGBUS
0000c31c	stfs	f0,0x14(r17)
0000c320	b	0xc2fc
```

`r17` is advanced by `addi r17,r17,0x4` on every pass of a loop whose only
exit is a float compare (`lfs f0,0x18(r17); fcmpu cr0,f0,f25`), and it had
reached about `0x47FFFD4`. This is GCC's strength-reduced form of one of
`fn_1_E914`'s three `for (var_r30 = 0; var_r30 < var_r31->unk_110; var_r30++)`
loops over `unk_178` / `unk_180` / `unk_194` — the arrays allocated from
`unk_110` at `thwomp.c:421-442` — and the `_sqrtf` stub call eight
instructions later pins it to the second of them (`thwomp.c:1959-1975`).

*And it explains the address, which was never a clue.* `0x04800000` is
**16 MB past the top of MEM1**, which `OSInit` prints as
`[0x2000000, 0x3800000)`. §16.8 disproved "one past the arena" and stopped
there; the fuller answer is that the runaway had already written through 16 MB
of whatever the host had mapped above MEM1 before it reached a page that was
not mapped at all. The fault address is where the *host's* address space runs
out, not where the bug is, and it is a round number for the same reason.

**What has to happen next, in this order.** Two of these are cheap and one is
the actual fix:

1. **A guard region above MEM1.** The single most useful thing this milestone
   found is that a pointer can walk 16 MB past the top of MEM1 and corrupt
   host memory silently before anything notices. `port_mem_init` should
   `mmap` MEM1 with `PROT_NONE` pages either side, so that this class of bug
   faults at `0x3800000` — one page past the arena, where the diagnosis is
   immediate — instead of wherever the address space happens to end. That
   turns every future overrun of this shape into a one-line answer, and it is
   a port-side change with no game-source patch.
2. **Print `unk_110` and the four array bases** when the hook is entered. The
   loop bound is a sum of twenty-five sub-counts including `var_r24 * var_r24`
   stored in an `s16` (`thwomp.c:394-421`), and every array is allocated from
   the same `unk_110`, so a consistent bound cannot overrun by itself. Either
   the bound and the allocation disagree, or one of the allocations came back
   short. `HuMemDirectMallocNum(HEAP_MODEL, ...)` under a nearly-full model
   heap is the obvious candidate and the port has never checked it.
3. Then fix whichever of those it is.

This is where M7 stopped on the crash. The reproduction is the deliverable and
it is done; the fix is one instrumented run away and did not fit tonight.

### 17.8 Tooling added

| flag / tool | what |
|---|---|
| `--rtc SECS` \| `--rtc dolphin` | the deterministic clock's origin as a Unix time — the number Dolphin calls `CustomRTCValue`. `OSInit` prints what the run was pinned to, because a log that does not say the number cannot be compared with anything |
| `--card FILE`, `--freshcard` | the *other* input to a run. The roulette reads the save's played set and every run rewrites the save; before this, the same seed was not the same experiment |
| `--com4` | four CPU players, which is also what makes `instDll` dismiss its own instruction screen — and therefore what removes every screen-specific button press from every walk |
| `--minigame NAME\|ID` | `m425dll`, `425` and `24` all name the same minigame. Reproducing a crash in a named module went from four board runs to one |
| `--turns N` | the board's turn count |
| `--status` | one line a second: live screen by name, board, turn, the module the roulette dealt, coins and stars per player, `aud` ms and fps. §16.10 item 2's "the result line should carry the audio's own invariants", once a second instead of once a run |
| `--stuckwatch SEC` | the live screen has not changed in SEC seconds: name it, and the one before it. One compare a frame, against the N64 ports' 27% `dladdr` |
| `--soak` | all of the above, from boot, restarting boards, logging every module entered and left |
| `--nodepop`, `--resample1` | switch off each of §17.5's two repairs independently, so the A/B is a word on a command line |
| `--clickstat` | `wavstat.py`'s discontinuity rule, evaluated in process, so a soak reports the number a capture would have been measured at |
| `port> roulette: frame N dealt mg X` | what the board's own RNG chose, logged whether or not `--minigame` overrides it — so the override costs no information |
| `port/docs/m7-soak.log` | what the soak found, and how to read it |

### 17.9 What M8 needs

1. **A guard region above and below MEM1, before anything else.** §17.7's
   runaway wrote through 16 MB of host memory before it hit a page that was
   not mapped. `PROT_NONE` pages either side of the MEM1 mapping turn that
   into a fault at `0x3800000` with an obvious diagnosis, and turn every
   future bug of this shape into a one-line answer instead of an evening.
   It is the cheapest thing on this list and it has the largest effect on
   how long the next bug takes.

2. **Finish `m425dll`.** §17.7 item 2: print `unk_110` and the four array
   bases on hook entry, and check `HuMemDirectMallocNum`'s return. The
   reproduction is a single `--minigame m425` run away, which is the part
   that used to be hard.

3. **The audio budget is 1.53 ms against 1.5 and the comparison is not
   controlled.** §17.6 compares two different segments. The controlled
   version is two runs of the same command differing only by
   `--nodepop --resample1`, which the flags now make trivial and which this
   session did not have the G4 time for. Do that before claiming the 4-tap
   filter bought anything, and measure `--clickstat` on both — that number,
   not the millisecond, is what §16.7's 25,306 was about.

4. **`--rtcoffset`, if the Dolphin comparison is still wanted.** §17.3: the
   two rigs share the clock's origin and still diverge, because
   `BoardRandInit` reads `OSGetTime` at board setup and the port arrives
   there some 300 frames earlier than the console does. One subtraction,
   measurable from `--ovllog` on each side. It is now a nice-to-have rather
   than a blocker, because `--minigame` solved the problem it was blocking.

5. **The soak is the regression suite now.** §16.10 item 4 wanted a `regress`
   mode that fails on a budget regression as well as a pixel one. Between
   `--rtc`/`--freshcard` (a run is a function of its command line, §17.2),
   `--status`' `aud` column and `--clickstat`, all three assertions exist as
   numbers; what is missing is a script that runs the fixed walk, extracts
   them and compares against a recorded baseline. That script is small and
   it should be written before the next optimisation, not after.
