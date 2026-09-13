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
| Input movie playback (`.dtm`) | **Yes, from the CLI** — `-m <file>` |
| Load savestate from CLI | **Yes** — `-s <file>` |
| Save savestate from CLI | **No** — hotkey/GUI only |
| Take screenshot from CLI | **No** — hotkey (F9) or GUI only |
| Lua / Python scripting | **No.** Upstream Dolphin has never had one; only forks (Dolphin Lua Core, Slippi) do |
| `dolphin-tool` CLI | **Not shipped.** `Dolphin.app/Contents/MacOS/` contains only `Dolphin`, `platforms/`, `styles/` |
| `DolphinNoGUI` frontend | **Not shipped** — the macOS `.app` bundles only the Qt frontend |
| Debugger UI (`-d`) | **Yes** — memory view, watches, breakpoints, "Dump MRAM" |
| MemoryWatcher UNIX socket | Compiled path present in the binary; **needs verification** (see §7) |

Consequences for the rig: **PNG-sequence frame dumping plus CLI `.dtm` playback is the
whole automation surface.** There is no scripting hook, so anything that needs to be
"pressed" has to arrive as movie input, and anything that needs to be read out of RAM
has to come from the debugger UI or an external memory reader.

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

### Playing one

```sh
Dolphin -u "$MP4_USERDIR" -b -e "$ISO" -m /path/to/movie.dtm
```

The header carries a `bSaveConfig` flag; when set, Dolphin applies the emulation
settings recorded in the header (dual-core, DSPHLE, CPU core, EFB/XFB options, memcards,
language, RTC) on top of everything else, which is what makes a movie reproducible
across machines. `Dolphin.Movie.PauseMovie=True` pauses at the end of playback rather
than letting the game run on with dead input.

### Recording one

Only through the GUI: **Movie ▸ Start Recording Input**, play, then **Movie ▸ Export
Recording…**. There is no CLI or scripted recorder.

### Authoring one — the approach this rig uses

Because the format is stable, documented, and trivially packable, the rig **generates**
`.dtm` files from a text script instead of recording them:
`port/ref/tools/mkdtm.py`. That makes an input sequence a diffable, reviewable,
regenerable source file rather than an opaque binary, and it removes the GUI from the
loop entirely.

```
frames 900          # 900 polls of neutral input
mark   title
press  START
frames 120
hold   LEFT
frames 10
release all
stick  128 255      # main stick fully up
tap    A 3
```

`mkdtm.py script.txt out.dtm` writes the movie and a sidecar `out.dtm.marks` mapping
labels to poll numbers, which is what the frame-comparison list is keyed on.

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

**Practical rule:** compare on `framedump_N` ↔ the port's presented-frame index, and
sanity-check with `GlobalCounter`/`VCounter` at the marked frames. If those two globals
diverge in the reference capture, the segment is not a valid frame-exact baseline.

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

**c. MemoryWatcher.** Upstream Dolphin has `Source/Core/Core/MemoryWatcher.cpp`, which
reads `<userdir>/MemoryWatcher/Locations.txt` — one pointer-chain per line, whitespace-
separated hex offsets, e.g. `801D3A54` for a plain global — and pushes
`"<line>\n<value_hex>\n"` datagrams to a UNIX `SOCK_DGRAM` socket at
`<userdir>/MemoryWatcher/MemoryWatcher` whenever a value changes. The strings for those
paths are present in this binary, but MemoryWatcher is behind the `USE_MEMORYWATCHER`
CMake option upstream and it could not be confirmed enabled here. **Test before
relying on it:** create the two files, bind a listener, and see whether datagrams
arrive. If they do, this is the clean way to log `GlobalCounter`, `omcurovl`, and
`frand_seed` alongside a capture.

A fourth option, if the port ever needs a large RAM baseline, is a savestate: `-s` can
load one, and states are written to `<userdir>/StateSaves/GMPE01.s01`…`.s10`. They are
compressed and versioned to the exact Dolphin build, so they are a convenience for
iteration, not an archival artifact — do not commit them.

## 8. The rig

Everything lives in `port/ref/tools/`.

| Tool | Purpose |
| --- | --- |
| `capture.sh <secs> <outdir> [args…]` | Headless run with the pinned config; dumps PNGs, SIGTERMs the emulator, moves `Dump/Frames` to `<outdir>/frames`. Pass `-m movie.dtm` through to replay a movie. |
| `mkdtm.py <script> <out.dtm>` | Compile a text input script to a `.dtm` plus a `.marks` sidecar. |
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

# replay a scripted movie
port/ref/tools/mkdtm.py port/ref/movies/first-minigame.txt /tmp/first.dtm
port/ref/tools/capture.sh 600 /tmp/mp4-ref/mg -m /tmp/first.dtm

# promote the frames that matter into the repo
port/ref/tools/pick.sh /tmp/mp4-ref/boot/frames port/ref/frames boot 1 6000 100
```

Full-size 640×528 PNGs stay outside the repo (they are ~150 KB each); only the 320×264
selections are committed.

## 9. Known limitations

* **Wall-clock termination.** There is no "run N frames then quit" option. `capture.sh`
  sleeps for a wall-clock duration and then SIGTERMs, so the exact last frame of a run
  is not reproducible. Frame *numbering* is reproducible; only the end point drifts.
  Anything that must end at a precise frame should be driven by a `.dtm` of exactly
  that length with `Dolphin.Movie.PauseMovie=True`.
* **No CLI screenshot and no CLI savestate save**, so a capture cannot checkpoint itself.
* **No scripting**, so the input script cannot branch on what is on screen. Authoring a
  movie is an offline loop: write script → replay → look at frames → adjust.
* **A movie is only valid against one disc revision and one set of emulation settings.**
  Changing `SIDevice*`, dual-core, or DSP settings between authoring and replay will
  desynchronise it.
* **The disc is an NKit image.** Dolphin reads it natively but requires
  `Interface.SkipNKitWarning=True`; the recovered image is bit-identical for emulation
  purposes but its MD5 does not match the original ISO, so leave the `.dtm` `md5` field
  zeroed.
