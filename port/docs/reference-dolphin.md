# Dolphin as the reference implementation for the Mario Party 4 port

This is the GameCube equivalent of the mupen64plus reference rig the Snowboard Kids
ports used (`snowboardkids-decomp/port/docs/reference-frames.md`): boot the retail disc
in an emulator, replay a scripted input movie, dump one PNG per frame, and diff those
frames against the native port at the same frame numbers.

Everything below was verified against the Dolphin build actually installed on this Mac.
Where upstream Dolphin differs from this build, that is called out.

* **Disc:** `/Users/zachjack/PowerPC Project/ROMs/Mario Party 4 (USA) (Rev 1).nkit.iso`
* **Game ID:** `GMPE01` (revision 1 — matches the decomp's `GMPE01_01` config).
  Do not confuse it with `GM4E01`, which is Mario Kart: Double Dash.
* **Emulator:** `/Applications/Dolphin.app` — **Dolphin 2506-433** (`CFBundleShortVersionString`),
  Qt frontend, arm64, Vulkan/MoltenVK on an M1 Max.

---

## 1. What this Dolphin build can and cannot do

| Capability | Status in build 2506-433 |
| --- | --- |
| Headless / batch launch (`-b`) | **Yes** — runs with no main window, exits on SIGTERM |
| Per-frame PNG frame dump | **Yes**, and it is the *only* dump mode (see §4) |
| Video (AVI/MP4) frame dump | **No** — the binary is built without FFmpeg |
| Audio dump (`Dolphin.DSP.DumpAudio`) | Untested; the FFmpeg-less build still writes raw WAV |
| Input movie record (`.dtm`) | GUI only (Movie ▸ Start Recording Input) |
| Input movie playback (`.dtm`) | **No — `-m <file>` is silently ignored in this build.** See §5.1 |
| Frame-exact input injection | **Yes**, via Gecko codes conditioned on the game's frame counter — see §5.2 |
| Load savestate from CLI | **Yes** — `-s <file>` |
| Save savestate from CLI | **No** — hotkey/GUI only |
| Take screenshot from CLI | **No** — hotkey (F9) or GUI only |
| Lua / Python scripting | **No.** Upstream Dolphin has never had one; only forks (Dolphin Lua Core, Slippi) do |
| `dolphin-tool` CLI | **Not shipped.** `Dolphin.app/Contents/MacOS/` contains only `Dolphin`, `platforms/`, `styles/` |
| `DolphinNoGUI` frontend | **Not shipped** — the macOS `.app` bundles only the Qt frontend |
| Debugger UI (`-d`) | **Yes** — memory view, watches, breakpoints, "Dump MRAM" |
| MemoryWatcher UNIX socket | **Yes — verified working 2026-09-17** (see §7 and PLAN.md §27) |

Consequences for the rig: **PNG-sequence frame dumping plus Gecko-code input injection
is the whole automation surface.** `.dtm` playback, the obvious choice, does not work
here (§5.1). Anything that needs to be read out of RAM has to come from the debugger UI
or an external memory reader.

## 2. Launching from the command line

Full option list (`Dolphin --help`):

```
-u, --user=USER          User folder path
-m, --movie=MOVIE        Play a movie file
-e, --exec=<file>        Load the specified file
-n, --nand_title=<id>    Launch a NAND title
-C, --config=<System>.<Section>.<Key>=<Value>
-s, --save_state=<file>  Load the initial save state
-d, --debugger           Show the debugger pane
-l, --logger             Open the logger
-b, --batch              Run Dolphin without the user interface (requires --exec)
-c, --confirm            Set Confirm on Stop
-v, --video_backend=X    Vulkan | Metal | Software Renderer | Null
-a, --audio_emulation=X  HLE | LLE
```

Minimal headless boot:

```sh
export LC_ALL=C.UTF-8   # otherwise Qt prints a locale warning on every run
/Applications/Dolphin.app/Contents/MacOS/Dolphin \
  -u  "$MP4_USERDIR" \
  -b \
  -e  "/Users/zachjack/PowerPC Project/ROMs/Mario Party 4 (USA) (Rev 1).nkit.iso" \
  -v  Vulkan
```

`-b` implies `Dolphin.Display.RenderToMain=False`. On macOS the Qt frontend still
creates an off-screen render surface, so a presenting backend (`Vulkan`) is required;
`Null` produces no frames at all.

### Always use a private user directory

`-u <dir>` redirects *everything* — config, dumps, savestates, memory cards — away from
`~/Library/Application Support/Dolphin/`. The user's own Dolphin is configured for
play (6× internal resolution, widescreen hack, FPS overlay, Wii U GC adapter), all of
which would corrupt a reference capture. The rig therefore ships its own pinned user
directory; the committed copy of that config is `port/ref/dolphin-user/Config/`.

### The `-C` config flag

Form: `-C <System>.<Section>.<Key>=<Value>`, repeatable. The `<System>` token is *not*
the ini filename — it is the name Dolphin's config system uses:

| `Config::System` | CLI token | ini file |
| --- | --- | --- |
| `Main` | `Dolphin` | `Config/Dolphin.ini` |
| `GFX` | **`Graphics`** | `Config/GFX.ini` |
| `GCPad` | `GCPad` | `Config/GCPadNew.ini` |
| `Logger` | `Logger` | `Config/Logger.ini` |
| `SYSCONF`, `FreeLook`, `Session`, `Achievements`, `DualShockUDPClient` | same as the enum name | — |

An unrecognised `<System>` token is **silently ignored**, so a typo produces no error
and no effect. Command-line settings sit in the `CommandLine` config layer, which beats
`Base` (the ini files) but is itself overridden by per-game ini files, the movie layer,
and anything changed at runtime.

Two keys that have moved and are easy to get wrong:

* `Main.Core.FrameLimit` **no longer exists**. Use
  `-C Dolphin.Core.EmulationSpeed=0` (float; `0` = unlimited, `1.0` = 100%).
* Frame-dump settings live under **`Graphics.Settings.*`**, not `Dolphin.Movie.*`.
  Only the on/off switch (`Dolphin.Movie.DumpFrames`) is a `Main` key.

## 3. The pinned reference configuration

`port/ref/dolphin-user/Config/Dolphin.ini` and `GFX.ini`. The settings that matter and
why:

**`Dolphin.ini [Core]`**

| Key | Value | Reason |
| --- | --- | --- |
| `CPUThread` | `False` | Single-core. Dual-core is non-deterministic; movies record this flag for a reason. |
| `CPUCore` | `1` | JIT. Interpreter (`0`) is ~50× slower but bit-identical; use it only to arbitrate a suspected JIT bug. |
| `DSPHLE` | `True` | LLE would need dumped DSP ROMs. |
| `DSPThread` | `False` | Determinism. |
| `SIDevice0` | `6` | `SIDEVICE_GC_CONTROLLER` — the *emulated* standard pad. The user's own config has `12` (`SIDEVICE_WIIU_ADAPTER`), which would read a physical adapter and make movies unusable. |
| `SIDevice1..3` | `0` | `SIDEVICE_NONE`. MP4 derives human-vs-CPU from pad presence (`modeseldll/main.c:145`), so ports 2–4 must be empty for a one-controller reference run. |
| `SkipIPL` | `True` | Skip the GameCube BIOS animation, which is not part of the game. |
| `EmulationSpeed` | `0.0` | Unlimited — capture is CPU-bound anyway. |
| `CustomRTCEnable` / `CustomRTCValue` | `True` / `1041472800` | **Critical.** Fixes the emulated real-time clock at 2003-01-02 00:00:00 UTC. MP4 seeds both of its RNGs from `OSGetTime()` (see `port/ref/notes.md` §RNG); without a pinned RTC no two captures agree. `.dtm` playback overrides this from the movie header anyway. |
| `SlotA` / `SlotB` | `255` | No memory card, so the game never writes a save and always takes the same "no card" path. |

**`GFX.ini [Settings]`**

| Key | Value | Reason |
| --- | --- | --- |
| `InternalResolution` | `1` | 1× native. Dolphin's "native" for GameCube is **640×528**, not 640×480 — see §4. |
| `wideScreenHack` | `False` | The disc is 4:3. |
| `ShowFPS`, `ShowNetPlayPing` | `False` | Overlays are composited into frame dumps. |
| `MSAA` / `SSAA` | `0x00000001` / `False` | No AA — the port will not have any. |
| `BackendMultithreading` | `False` | Determinism. |
| `SkipDuplicateXFBs` | `False` | **Critical.** With this on, a frame identical to the previous one is not presented and therefore not dumped, and the PNG numbering silently desynchronises from the game's frame count. |

`[Interface]`: `ConfirmStop`, `PauseOnFocusLost`, `UsePanicHandlers`,
`OnScreenDisplayMessages` all `False`, `SkipNKitWarning = True` (the disc is an NKit
image and Dolphin otherwise blocks on a modal warning).

## 4. Frame dumping

Switch it on with `Dolphin.Movie.DumpFrames=True` and
`Dolphin.Movie.DumpFramesSilent=True` (the second suppresses the "file exists,
overwrite?" modal, which would hang a headless run).

Upstream Dolphin then has two paths: an FFmpeg muxer (default, `Graphics.Settings.DumpFormat=avi`)
and a PNG-sequence fallback (`Graphics.Settings.DumpFramesAsImages=True`). **This build
has no FFmpeg**, so it always takes the fallback:

```
FrameDump: Dolphin was not compiled with FFmpeg, using fallback option.
           Frames will be saved as PNG images instead.
```

That is the good outcome. The FFmpeg path is unusable as a frame reference anyway:
its PTS is derived from emulated CPU ticks, it *silently drops* any frame whose PTS
delta rounds to zero, and it starts a new file on a resolution change or savestate
load. The PNG path writes one file per presented frame with a single monotonic counter
and no filtering.

**Output:** `<userdir>/Dump/Frames/framedump_N.png`, **N starting at 1**, one per
presented XFB frame, 8-bit RGB. The counter is neither game-ID- nor timestamp-namespaced,
so a second session overwrites the first; the capture script moves the directory
aside after every run.

**Resolution:** 640×528 at `InternalResolution=1`. 528 is Dolphin's notion of "native"
GameCube height (the VI's full half-line count), not the game's 480-line XFB; the
active picture measures rows 17–510. Reference frames are therefore stored at an exact
half, **320×264**, rather than being resampled to 320×240 — a non-integer resample
would put ringing into a comparison baseline. Crop to rows 17–510 first if you want
true 4:3 pixels.

**Cost:** ~24 emulated frames/second of wall clock on an M1 Max with PNG encoding on
(≈0.4× real time), and ~150 KB per frame. Budget ~2.5 minutes and ~500 MB per 1000
frames of game time.

**Screenshots** are a separate mechanism (F9 hotkey → `ScreenShots/<GameID>/<GameID>_<timestamp>.png`)
with no CLI trigger. Ignore them; the frame dump is strictly better for this purpose.

## 5. Input movies (`.dtm`)

### 5.1 `-m` does not work in this build

In principle:

```sh
Dolphin -u "$MP4_USERDIR" -b -e "$ISO" -m /path/to/movie.dtm
```

In practice, on Dolphin 2506-433 for macOS, **this silently does nothing.** The movie is
opened and its header is validated, but the movie never becomes active. Evidence, all
reproducible with `port/ref/tools/capture.sh`:

1. A movie holding START from poll 4460 through the title screen changes nothing — the
   captured frames are **md5-identical** to an unattended run at frames 4500/5000/6000.
2. A movie setting the `reset` bit does not reset the console (`PlayController` calls
   `ProcessorInterface::ResetButton_Tap()` on that bit).
3. `Dolphin.Movie.PauseMovie=True` with a 1,210-poll movie does not pause: the run kept
   dumping to 1,632 frames.
4. **The decisive test.** `ReadHeader()` installs a movie config layer that sets
   `MAIN_GFX_BACKEND` from the header's `videoBackend` field, and the movie layer
   outranks the command-line layer. Patching a movie's `videoBackend` to `Null` and
   launching with `-v Vulkan` should therefore produce **zero** frames. It produced
   1,447. The movie config layer is never installed, so `PlayInput()` is failing.
5. It is not a bad header: corrupting `filetype` to `XXXX` and enabling panic handlers
   *does* raise the "Invalid recording file" modal (44 frames before the modal blocked
   the run) while the valid file raises nothing. So the file is read and the magic
   passes.
6. It is not the RetroAchievements hardcore-mode gate (`PlayInput` returns false
   silently when `IsHardcoreModeActive()`): disabling it via `Config/RetroAchievements.ini`
   *and* `-C Achievements.Achievements.HardcoreEnabled=False` changed nothing, and in
   any case `ReadHeader()` runs *before* that check, so test 4 would still have fired.

Reading the 2506 tag's `MainWindow.cpp:301` and `Movie.cpp:904`, there is no remaining
silent-false path, so the cause is specific to this binary. **Do not build the rig on
`.dtm` playback until this is retested on a newer Dolphin.** Verifying it through the
GUI (Movie ▸ Play Input Recording…) would settle it in a minute and was not possible
here.

`port/ref/tools/mkdtm.py` is kept because the writer itself is correct and verified
against the format, and because the moment `-m` works (or a movie is loaded through the
GUI) it becomes the better mechanism — a `.dtm` pins the emulation settings and the RTC
in the file, which Gecko codes cannot.

### 5.2 What is used instead: Gecko-code input injection

`port/ref/tools/mkgecko.py` compiles a frame-numbered input script into Dolphin Gecko
codes that write the game's own pad globals. This works, and for a port reference it is
arguably the better mechanism, because the schedule is expressed in **the game's own
`GlobalCounter`** rather than an emulator-side poll index.

Why it is sound rather than a hack: `HuPadRead()` (`src/game/pad.c:130`) copies the
private `_Pad*` arrays into the public `HuPad*` globals once per frame and then clears
`_PadBtnDown`. Dolphin's Gecko handler runs at the VI hook, which lands between
`PadReadVSync()` filling `_Pad*` and the next `HuPadRead()` reading them, so a write to
`_PadBtn` / `_PadBtnDown` / `_PadDStk` is indistinguishable from a real press.

Writing the *public* `HuPadBtn` / `HuPadBtnDown` does **not** work — `HuPadRead` runs
after the hook and overwrites them. That was tested and produced no effect.

Script syntax:

```
at 10 320 START        # hold START for 320 frames from GlobalCounter == 10
at 560 6  A            # tap A for 6 frames at frame 560
at 900 4  dstk:DOWN    # menu cursor down (writes _PadDStk / _PadDStkRep)
mark 380 title_accept
```

```sh
port/ref/tools/mkgecko.py walk.txt "$MP4_USERDIR/GameSettings/GMPE01.ini" --name RefWalk
port/ref/tools/capture.sh 150 out/walk -C Dolphin.Core.EnableCheats=True
```

Each event compiles to a `24`/`26` unsigned compare pair on `GlobalCounter`
(`0x801D3A54`), the 8- or 16-bit writes, and an `E0000000 80008000` full terminator.

Limits: Gecko codes do **not** pin the emulation settings or the RTC the way a `.dtm`
header does, so the pinned config in `port/ref/dolphin-user/` is doing that job and must
travel with any capture. And the codes fire on `GlobalCounter`, so a run that drops a
frame relative to the reference shifts every subsequent input — which is a feature when
comparing against a port (the port's own counter drives it identically) and a nuisance
when the emulator hiccups.

### Recording a `.dtm`

Only through the GUI: **Movie ▸ Start Recording Input**, play, then **Movie ▸ Export
Recording…**. There is no CLI or scripted recorder. `port/ref/tools/mkdtm.py` generates
one from the same kind of text script instead:

```
frames 900          # 900 polls of neutral input
mark   title
press  START
frames 120
stick  128 255      # main stick fully up
tap    A 3
```

It writes the movie plus a sidecar `out.dtm.marks` mapping labels to poll numbers.

### The format

A `.dtm` is a 256-byte packed `DTMHeader` followed by packed 8-byte
`Movie::ControllerState` records, one per **controller poll**. Both structs are
`#pragma pack(1)` and are declared in `Source/Core/Core/Movie.h`
(dolphin-emu/dolphin, master); the header carries an explicit note that third-party
tools parse the format and that it is expected to stay stable.

`ControllerState`, 8 bytes, little-endian bitfields LSB-first:

| Byte | Bit | Field |
| --- | --- | --- |
| 0 | 0–5 | `Start`, `A`, `B`, `X`, `Y`, `Z` |
| 0 | 6–7 | `DPadUp`, `DPadDown` |
| 1 | 0–3 | `DPadLeft`, `DPadRight`, `L`, `R` |
| 1 | 4–7 | `disc`, `reset`, `is_connected`, `get_origin` |
| 2–3 | — | `TriggerL`, `TriggerR` (u8) |
| 4–5 | — | `AnalogStickX`, `AnalogStickY` (u8, 128 = centre) |
| 6–7 | — | `CStickX`, `CStickY` (u8) |

`is_connected` must be set on every record or the game sees the pad unplugged.

`DTMHeader`, 256 bytes:

| Off | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | `filetype` | `"DTM"` + `0x1A` |
| 4 | 6 | `gameID` | `GMPE01` |
| 10 | 1 | `bWii` | 0 |
| 11 | 1 | `controllers` | bitmask: GC pads 1–4 then Wiimotes 1–4. `0x01` here |
| 12 | 1 | `bFromSaveState` | 0 = movie starts from boot |
| 13 | 8 | `frameCount` | u64, VI frames |
| 21 | 8 | `inputCount` | u64, polls = number of records in the file |
| 29 | 8 | `lagCount` | u64, frames with no poll |
| 37 | 8 | `uniqueID` | unimplemented upstream |
| 45 | 4 | `numRerecords` | u32 |
| 49 | 32 | `author` | UTF-8 |
| 81 | 16 | `videoBackend` | |
| 97 | 16 | `audioEmulator` | |
| 113 | 16 | `md5` | MD5 of the disc image; all-zero = "don't check" |
| 129 | 8 | `recordingStartTime` | u64 Unix seconds — **this is the RTC forced during playback** |
| 137 | 1 | `bSaveConfig` | apply the flags below on playback |
| 138–142 | 5 | `bSkipIdle`, `bDualCore`, `bProgressive`, `bDSPHLE`, `bFastDiscSpeed` | |
| 143 | 1 | `CPUCore` | `PowerPC::CPUCore` value |
| 144–150 | 7 | `bEFBAccessEnable`, `bEFBCopyEnable`, `bSkipEFBCopyToRam`, `bEFBCopyCacheEnable`, `bEFBEmulateFormatChanges`, `bImmediateXFB`, `bSkipXFBCopyToRam` | |
| 151 | 1 | `memcards` | bits = slot A, slot B |
| 152 | 1 | `bClearSave` | create a fresh memory card on playback |
| 153 | 1 | `bongos` | |
| 154–156 | 3 | `bSyncGPU`, `bNetPlay`, `bPAL60` | |
| 157 | 1 | `language` | |
| 158 | 1 | `reserved3` | |
| 159–160 | 2 | `bFollowBranch`, `bUseFMA` | |
| 161 | 1 | `GBAControllers` | |
| 162 | 1 | `bWidescreen` | SYSCONF 16:9 |
| 163 | 1 | `countryCode` | |
| 164 | 5 | `reserved` | |
| 169 | 40 | `discChange` | second-disc image name |
| 209 | 20 | `revision` | Dolphin git hash |
| 229 | 4 | `DSPiromHash` | |
| 233 | 4 | `DSPcoefHash` | |
| 237 | 8 | `tickCount` | |
| 245 | 11 | `reserved2` | padding to 256 |

Pad records begin at file offset 256.

## 6. Frame counters: emulator vs game

Three different counters are in play. Confusing them is the main way a frame-exact
comparison goes wrong.

| Counter | Ticks on | Where |
| --- | --- | --- |
| `Movie::GetCurrentFrame()` | every **VI field boundary** (`VideoInterfaceManager::Update`, `m_half_line_count == odd/even_field_begin`) | Dolphin |
| `Movie::GetCurrentInputCount()` | every **controller poll** | Dolphin |
| `framedump_N.png` | every **presented XFB frame** | Dolphin |
| `GlobalCounter` @ `0x801D3A54` | every **main-loop iteration** | game (`src/game/main.c:119`) |
| `VCounter` @ `0x801D3A58` | every **VI retrace** | game (`src/game/pad.c:222`) |

For Mario Party 4 these line up 1:1 in the normal case, which is why the rig works:

* The game runs at 60 Hz with `minimumVcount == 1` and no frame-skip path, so one
  main-loop iteration = one retrace = one VI frame = one XFB present.
* `PADRead` is called exactly once per retrace from the VI post-retrace callback
  (`pad.c:152`), so **polls = frames** and `lagCount` should be 0. That is why
  `mkdtm.py` writes `frameCount == inputCount`.

They come apart in two situations, both of which must be reproduced by the port if
frame numbers are to keep matching:

1. `src/game/main.c:87` — a soft-reset check or a DVD error `continue`s the main loop
   *without* incrementing `GlobalCounter`, while `VCounter` keeps going.
2. Anything that stalls the game past one retrace (`worstVcount` records the worst
   observed span) makes `GlobalCounter` lag `VCounter`. On real hardware that is a
   dropped frame; the XFB is presented again, so the PNG count still advances.

### The 1:1 assumption above does not survive a long capture — measured 2026-09-17

The paragraph above is the theory. Measured in **one** run with the PNG dump and
MemoryWatcher both on, stopped at the same instant:

| counter | value |
|---|---|
| `framedump_N` files written | **7,177** |
| `GlobalCounter` `0x801D3A54` | **7,533** |
| `VCounter` `0x801D3A58` | **8,831** |

`GlobalCounter` lagging `VCounter` by 1,298 is the game's own documented behaviour
(`src/game/main.c:87`). The **PNG count lagging both is not**: with
`SkipDuplicateXFBs = False` there should be one file per presented frame. Over a
40,000-frame capture the deficit is several per cent and it is not linear, so a PNG
index cannot be converted back to a `GlobalCounter` after the fact.

Worse, **two captures of the same input schedule that differ only in
`Dolphin.Movie.DumpFrames` diverge.** Two runs of `port/ref/movies/mg-entries.txt`,
identical in every other respect, dealt different minigames: the dumping one played
m405, m408, m412, m443; the non-dumping one played m405, m408, m410, m443, m404,
m439. The input schedule alone does not determine the run, so numbers read out of a
non-dumping run do not describe a dumping run's pictures.

**Practical rules, revised:**

1. Anything that has to be compared frame-for-frame must come out of **one** run —
   the pictures and the memory reads together.
2. `omcurovl` + `GlobalCounter` from MemoryWatcher is the authoritative key; the PNG
   index is a filename.
3. To get a `GlobalCounter` onto a picture, **put it in the picture**: Gecko-poke a
   HUD field drawn every frame from the counter's low bits (`GWPlayer[0].coins` at
   `0x8018FC38 + 0x1C` is drawn on the board HUD and most minigame HUDs). Then every
   PNG carries the frame it was taken at and the `--ffto` comparison is a diff.

The committed `boot-*.png` set predates all of this. It was verified reproducible at
fixed PNG indices across two runs, so it is self-consistent, but its indices should
not be read as `GlobalCounter` values.

## 7. Reading game memory

There is no scripting API, so there are three options, in decreasing order of
usefulness:

**a. The decomp's symbol table.** `config/GMPE01_01/symbols.txt` (7,721 entries) gives
absolute addresses for the Rev 1 `main.dol`. The useful ones for this rig are listed in
`port/ref/notes.md`. RELs are relocatable and have no fixed addresses (only
`m444dll`, `mstoryDll`, `mstory2Dll`, `mstory3Dll` have per-REL symbol files).

**b. The debugger UI.** `Dolphin -d -e <iso>` opens the debugger panes: Memory view
(with a watch list that can be saved to `<userdir>/Maps/`), breakpoints, and a
"Dump MRAM" context action that writes the full 24 MB of MEM1 to `<userdir>/Dump/`.
This is manual, but it is the only supported read-out in this build and it is enough
to check a handful of globals at a marked frame.

**c. MemoryWatcher — this is the one to use. Verified working in this build
(2026-09-17).** Dolphin reads `<userdir>/MemoryWatcher/Locations.txt` and pushes
datagrams to a UNIX `SOCK_DGRAM` socket at `<userdir>/MemoryWatcher/MemoryWatcher`
whenever a watched value changes. Four things about it that the upstream description
does not tell you and that each cost time:

* **Bind the listener before Dolphin starts.** Dolphin connects to the path once, at
  boot. Unlinking and re-binding the socket afterwards leaves it writing to a dead
  inode and the stream simply stops.
* **The socket path must be short.** `sun_path` is 104 bytes; a long scratch
  directory silently overflows it.
* **The value is hex with thousands separators** — `ff,fff,fff`, not `ffffffff`.
  Strip the commas or every read above `0xFFF` fails to parse.
* **One datagram per frame, containing everything that changed that frame.** Since
  `GlobalCounter` changes every frame, the frame number travels *with* the data:
  no clock, no correlation step, no drift. Verified against the full frame range of
  a 100,000-frame capture with no dropped datagrams.

A line is a **pointer chain**: whitespace-separated hex offsets, chased with a
`Read32` at each step. `801D3A54` is a plain global; `801901E0 8 1894` is
`omDLLinfoTbl[0] -> omDllData.bss -> +0x1894`, which is how a relocatable REL's
`.bss` is read without knowing where it loaded (`omDllData` is
`{char *name; OSModuleHeader *module; void *bss; s32 ret;}`,
`include/game/object.h:65`; `omDLLinfoTbl` is 20 slots at `0x801901E0` and which one
a module lands in is not fixed, so watch all twenty and filter on `omcurovl`).

`port/ref/tools/mwball.py` and `port/ref/tools/mwovl.py` are the two listeners this
rig uses: the first logs `m444dll`'s ball state per frame, the second logs every
`omcurovl` change with the `GlobalCounter` it happened at. PLAN.md §27.1 has the
whole command sequence.

A fourth option, if the port ever needs a large RAM baseline, is a savestate: `-s` can
load one, and states are written to `<userdir>/StateSaves/GMPE01.s01`…`.s10`. They are
compressed and versioned to the exact Dolphin build, so they are a convenience for
iteration, not an archival artifact — do not commit them.

## 8. The rig

Everything lives in `port/ref/tools/`.

| Tool | Purpose |
| --- | --- |
| `capture.sh <secs> <outdir> [args…]` | Headless run with the pinned config; dumps PNGs, SIGTERMs the emulator, moves `Dump/Frames` to `<outdir>/frames`. Pass `-m movie.dtm` through to replay a movie. |
| `mkgecko.py <script> <out.ini>` | **The working input path.** Compile a frame-numbered input script to a Dolphin per-game Gecko code list. |
| `mkdtm.py <script> <out.dtm>` | Compile a text input script to a `.dtm` plus a `.marks` sidecar. Correct, but playback is broken in this build (§5.1). |
| `contact.sh <framedir> <a> <b> <step> <out.png> [cols]` | Numbered contact sheet, for scanning thousands of frames quickly. |
| `pick.sh <framedir> <dest> <prefix> <a> <b> <step>` | Downscale selected frames to 320×264 and name them by their original frame number. |

Typical session:

```sh
export MP4_USERDIR=/path/to/scratch/dolphin-user      # or port/ref/dolphin-user
cp -R port/ref/dolphin-user "$MP4_USERDIR"

# unattended boot capture
port/ref/tools/capture.sh 480 /tmp/mp4-ref/boot

# scan it
port/ref/tools/contact.sh /tmp/mp4-ref/boot/frames 1 6000 100 /tmp/cs.png 6

# replay a scripted input sequence
port/ref/tools/mkgecko.py port/ref/movies/menu-walk.txt \
    "$MP4_USERDIR/GameSettings/GMPE01.ini" --name RefWalk
port/ref/tools/capture.sh 150 /tmp/mp4-ref/walk -C Dolphin.Core.EnableCheats=True

# promote the frames that matter into the repo
port/ref/tools/pick.sh /tmp/mp4-ref/boot/frames port/ref/frames boot 1 6000 100
```

Full-size 640×528 PNGs stay outside the repo (they are ~150 KB each); only the 320×264
selections are committed.

## 8a. A memory card is required past the title

With both EXI slots empty the game reaches **SELECT A FILE**, prints "No valid Memory
Card is inserted.", and cannot go any further — no amount of A will get past it. The
pinned config therefore uses:

```ini
[Core]
SlotA = 1            # ExpansionInterface::EXIDeviceType::MemoryCard
MemcardAPath = <userdir>/GC/MemoryCardA.USA.raw
SlotB = 255          # None
```

Dolphin creates the `.raw` on first use. Keep it **out of the repo** and delete it to
return to a virgin save state — `GWGameStat` defaults (including `veryHardUnlock = 0`)
come back with it, which is what makes a capture reproducible. Note that a run which
gets as far as saving will mutate the card, so a capture script that must be repeatable
should delete the card before each run.

## 9. Known limitations

* **Wall-clock termination.** There is no "run N frames then quit" option. `capture.sh`
  sleeps for a wall-clock duration and then SIGTERMs, so the exact last frame of a run
  is not reproducible. Frame *numbering* is reproducible; only the end point drifts.
  Anything that must end at a precise frame should be driven by a `.dtm` of exactly
  that length with `Dolphin.Movie.PauseMovie=True`.
* **No CLI screenshot and no CLI savestate save**, so a capture cannot checkpoint itself.
* **No scripting**, so the input script cannot branch on what is on screen. Authoring an
  input sequence is an offline loop: write script → replay → look at frames → adjust,
  at roughly 30 emulated frames per second of wall clock. Reaching a board and a
  minigame from boot is on the order of 12,000 frames, i.e. ~7 minutes per attempt.
* **`.dtm` playback is broken in this build** (§5.1), so the reproducibility guarantees
  a movie header provides (pinned RTC and emulation settings travelling *with* the
  input) are not available. The pinned user directory has to carry them instead.
* **An input sequence is only valid against one disc revision and one set of emulation
  settings.** Changing `SIDevice*`, dual-core, DSP settings, or the memory-card contents
  between authoring and replay will desynchronise it.
* **Gecko codes require `Dolphin.Core.EnableCheats=True`**, which is a global switch; do
  not leave it on in a configuration used to validate anything else.
* **The disc is an NKit image.** Dolphin reads it natively but requires
  `Interface.SkipNKitWarning=True`; the recovered image is bit-identical for emulation
  purposes but its MD5 does not match the original ISO, so leave the `.dtm` `md5` field
  zeroed.
