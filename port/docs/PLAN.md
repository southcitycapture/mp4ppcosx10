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

### M1 — it links and it talks
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

### M2 — the first frame
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

### M5 — a board with CPU players
A full board (`w01Dll` first — Toad's Midway Madness is the simplest) with four
players, three of them CPU, for a full 10-turn game including minigames, items,
the Star space and the Last 5 Turns event.
**Done means:** a complete game reaches the results screen without a crash, a
hang, or a wrong winner, three times running.

### M6 — audio
The MusyX SAL replacement and the DSP-command interpreter; `src/msm` and
`extern/musyx` above the SAL compiled as-is; SDL audio out.
**Done means:** board music, minigame music, character voices and SFX all play,
the `--wav` capture of a fixed 30-second board segment matches a Dolphin
capture of the same segment to the ear, and the audio path costs less than
1.5 ms/frame in `--perf`.

### M7 — the self-play harness
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
| `port/build-ppc-darwin/marioparty4` | `port/build-ppc.sh -j8` — GCC 14.2, `powerpc-apple-darwin8`, MacOSX10.4u SDK, `-malign-natural -mone-byte-bool` | 1,093,116 bytes |
| `port/build-host/marioparty4` | `make -C port TARGET=host -j8` — the Mac's clang, arm64 | 1,078,232 bytes |

93 game translation units (50 `src/game`, 30 `src/game/board`, 6 `src/msm`,
2 `src/libhu`, 5 `src/dolphin/mtx`), 10 port sources, one assembly file, and
one generated stub pair. Both targets stub **exactly the same 204 symbols** —
the two `missing.txt` files are identical, which is the cheapest possible check
that the host build is not diverging from the real one.

The G4 was not available on the day, so the PowerPC binary is verified only to
compile and link clean; everything below was run on the host build.

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

64 distinct stubs, 135 calls, in first-call order — 55 of them GX, which is the
best possible argument for §3 being the next milestone rather than anything
else:

```
   1  PADInit                                 2
   2  GXInit                                  1
   3  GXSetViewport                           6
   4  GXSetScissor                            4
   5  GXSetDispCopySrc                        1
   6  GXSetDispCopyDst                        1
   7  GXSetDispCopyYScale                     1
   8  GXSetCopyFilter                         1
   9  GXSetPixelFmt                           1
  10  GXCopyDisp                              2
  11  GXSetDispCopyGamma                      1
  12  PADRead                                 5
  13  sndIsInstalled                          1
  14  CARDInit                                1
  15  PADSetSpec                              1
  16  SISetSamplingRate                       1
  17  PADClamp                                4
  18  PADControlMotor                         6
  19  PADReset                                4
  20  GXSetFog                                1
  21  GXSetDrawSyncCallback                   1
  22  GXInvalidateVtxCache                    2
  23  GXInvalidateTexAll                      4
  24  GXSetGPMetric                           2
  25  GXClearGPMetric                         2
  26  GXSetVCacheMetric                       2
  27  GXClearVCacheMetric                     2
  28  GXClearPixMetric                        2
  29  GXClearMemMetric                        2
  30  GXSetCopyClear                          2
  31  GXSetDrawSync                           2
  32  GXSetCurrentMtx                         3
  33  GXSetProjection                         3
  34  GXClearVtxDesc                          3
  35  GXSetVtxDesc                            6
  36  GXSetVtxAttrFmt                         6
  37  GXSetCullMode                           1
  38  GXSetZMode                              4
  39  GXLoadPosMtxImm                         2
  40  GXSetChanMatColor                       2
  41  GXSetNumChans                           2
  42  GXSetChanCtrl                           2
  43  GXSetTevOrder                           2
  44  GXSetTevOp                              2
  45  GXSetNumTexGens                         2
  46  GXSetNumTevStages                       2
  47  GXSetAlphaUpdate                        2
  48  GXSetColorUpdate                        2
  49  GXSetAlphaCompare                       2
  50  GXSetBlendMode                          2
  51  GXBegin                                 1
  52  GXPosition2u16                          4
  53  GXEnd                                   1
  54  GXSetArray                              1
  55  GXInitTexObj                            1
  56  GXInitTexObjLOD                         1
  57  GXLoadTexObj                            1
  58  GXSetTexCoordGen2                       1
  59  GXSetZCompLoc                           1
  60  GXDrawDone                              1
  61  GXReadGPMetric                          1
  62  GXReadVCacheMetric                      1
  63  GXReadPixMetric                         1
  64  GXReadMemMetric                         1
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

### 9.8 Open for M2

1. **REL loading by `dlopen`** (§2.4b): one Mach-O bundle per REL, the
   `omDLLLink` patch seam is already cut, and the "load and unload all 99
   bundles twice" smoke test still has to be written.
2. **The first frame**: 55 GX entry points are already exercised before the
   port runs out of REL, and the list in §9.3 is the order to implement them in.
3. `kerent.c`'s trampolines, as above.
4. The soft-reset thread currently never runs. It blocks on `OSSleepThread`
   immediately, so this is behaviour-preserving until something posts to its
   queue; the host loop should poll it once per retrace.
