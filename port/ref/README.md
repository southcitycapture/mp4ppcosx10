# `port/ref/` — Dolphin reference capture for the native port

Frame-exact reference material for Mario Party 4 USA Rev 1 (`GMPE01`), captured from the
retail disc in Dolphin so the native port can be diffed against it frame by frame. This
is the GameCube equivalent of the mupen64plus rig used for the Snowboard Kids ports.

| Path | What |
| --- | --- |
| `../docs/reference-dolphin.md` | How the rig works: CLI, config, frame dumping, `.dtm` format, input injection, memory reading. Read this first. |
| `notes.md` | What the game actually does — attract loop, measured boot timing, menu tree, CPU difficulty, memory card, and the determinism hazards the port must reproduce. |
| `tools/` | The rig itself (see below). |
| `dolphin-user/Config/` | The pinned Dolphin configuration. Copy this to a scratch directory and point `MP4_USERDIR` at it. |
| `frames/boot-NNNN.png` | Unattended boot + attract capture, 320×264. `NNNN` is the Dolphin frame-dump index. |
| `frames/menu-NNNN.png` | Title → file select → Party Mode → character select → board settings, driven by `movies/menu-walk.txt`. |
| `movies/` | Input scripts and the artifacts compiled from them. |

Full-size 640×528 frames are **not** committed — they are ~150 KB each and a single boot
capture is 14,557 of them. Regenerate them with `tools/capture.sh`.

## Quick start

```sh
export MP4_USERDIR=/tmp/mp4-ref/dolphin-user
mkdir -p "$(dirname "$MP4_USERDIR")" && cp -R port/ref/dolphin-user "$MP4_USERDIR"

# 8 minutes of unattended boot -> ~14,500 PNGs
port/ref/tools/capture.sh 480 /tmp/mp4-ref/boot

# scan the result
port/ref/tools/contact.sh /tmp/mp4-ref/boot/frames 1 14000 200 /tmp/cs.png 6

# drive the menus
port/ref/tools/mkgecko.py port/ref/movies/menu-walk.txt \
    "$MP4_USERDIR/GameSettings/GMPE01.ini" --name RefWalk
port/ref/tools/capture.sh 150 /tmp/mp4-ref/walk -C Dolphin.Core.EnableCheats=True
```

## Tools

| Tool | Purpose |
| --- | --- |
| `capture.sh <secs> <outdir> [args…]` | Headless Dolphin run with the pinned config; one PNG per frame into `<outdir>/frames/`. |
| `mkgecko.py <script> <out.ini>` | Compile a frame-numbered input script into Gecko codes that inject pad state. **This is the working input path** — Dolphin's `-m <movie.dtm>` is silently ignored in this build. |
| `mkdtm.py <script> <out.dtm>` | Write a Dolphin `.dtm` input movie from a text script. Format-correct, but unusable until `.dtm` playback works here. |
| `contact.sh <dir> <a> <b> <step> <out> [cols]` | Numbered contact sheet for scanning thousands of frames. |
| `pick.sh <dir> <dest> <prefix> <a> <b> <step>` | Downscale selected frames to 320×264, named by original frame number. |

## The two things that surprised us

1. **`-m movie.dtm` does nothing** on Dolphin 2506-433 for macOS. The file is read and
   header-validated, but the movie never activates — proven with a `videoBackend=Null`
   header that failed to blank the output. Input goes through Gecko codes instead; see
   `../docs/reference-dolphin.md` §5.
2. **The game needs a memory card to get past SELECT A FILE.** With both slots empty it
   prints "No valid Memory Card is inserted." and stops there.

## Determinism

Two independent captures of the first 4,000 frames were **byte-identical** (md5 on the
PNGs at frames 100/300/1000/2000/3000/4000), so the rig itself is reproducible. That
depends entirely on `CustomRTCEnable`/`CustomRTCValue` in the pinned config: Mario Party
4 seeds both of its RNGs from `OSGetTime()`. See `notes.md` §6.1.
