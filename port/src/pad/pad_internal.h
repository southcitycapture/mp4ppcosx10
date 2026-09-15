/* PAD internals shared between pad.c and its three input sources.
 *
 * pad.c owns the Dolphin-shaped public API (PADRead, PADClamp, ...) and picks,
 * once at PADInit, exactly one live source for controller port 1: the Xbox
 * One pad over IOUSBLib (pad_xone.c), an SDL joystick/game controller
 * (pad_sdl.c), or -- if neither is there -- the keyboard, which pad_sdl.c
 * also serves. A `--play` script (pad_play.c) is layered on top of whatever
 * source is live: it never replaces port 1's raw state, it is fed into
 * PADRead as if it *were* the raw state, so the game's own repeat/edge logic
 * in src/game/pad.c runs unchanged (see port/ref/notes.md §6.3). Recording
 * captures that same merged raw state.
 *
 * Ports 2-3-4 never report a controller: PAD_ERR_NO_CONTROLLER on every read,
 * which is what the reference rig's pinned config produces and what makes
 * `PlayerConfig.iscom` come out CPU for players 2-4 (notes.md §4).
 */
#ifndef PORT_PAD_INTERNAL_H
#define PORT_PAD_INTERNAL_H

#include "port.h"

#include <dolphin/types.h>
#include <dolphin/pad.h>

/* One frame's worth of raw controller-1 state, before PADClamp. Signed stick
 * axes are the pot's own -100..100-ish range (not yet deadzoned); triggers
 * are 0..255. `button` carries only the digital bits a real pad has:
 * PAD_BUTTON_{LEFT,RIGHT,DOWN,UP,A,B,X,Y,START}, PAD_TRIGGER_{Z,L,R}. */
typedef struct PortPadRaw {
    u16 button;
    s8 stickX, stickY;
    s8 substickX, substickY;
    u8 triggerL, triggerR;
} PortPadRaw;

/* ---- pad_sdl.c: SDL joystick/game-controller + keyboard ------------------ */
void pad_sdl_init(void);
void pad_sdl_shutdown(void);
int pad_sdl_present(void);            /* an SDL pad (not just the keyboard) is open */
const char* pad_sdl_name(void);       /* pad name, or "keyboard" */
void pad_sdl_poll(PortPadRaw* out);   /* keyboard, OR'd with the pad if present */
int pad_sdl_rumble_supported(void);
void pad_sdl_rumble(int on);

/* ---- pad_xone.c: Xbox One controller over IOUSBLib ----------------------- */
int pad_xone_open(void);              /* 1 if an Xbox One pad was claimed */
int pad_xone_present(void);
const char* pad_xone_name(void);
void pad_xone_poll(PortPadRaw* out);
void pad_xone_rumble(int on);
void pad_xone_close(void);

/* --paddbg: log raw button/axis/report numbers from whichever driver claims
 * the pad, for working out a mapping for an unrecognised one. */
extern int port_pad_debug;

/* ---- pad_play.c: --play scripted input and --record ---------------------- */
void pad_play_init(const char* script_path, const char* record_path);
void pad_play_shutdown(void);
/* Called once per PADRead with the frame's merged raw state (whatever the
 * live source produced): a script overwrites *raw* with its own state for the
 * frame's duration; a recording is appended either way. `frame` is the port's
 * own retrace count (VIGetRetraceCount()), which tracks the game's own
 * GlobalCounter frame-for-frame along the boot/menu paths the reference rig's
 * scripts are captured on (notes.md §6.2). */
void pad_play_step(u32 frame, PortPadRaw* raw);
/* frames on which the --play script drove a non-zero button */
u32 pad_play_press_count(void);

#endif
