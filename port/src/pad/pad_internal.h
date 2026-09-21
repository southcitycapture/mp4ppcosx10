/* PAD internals shared between pad.c and its three input sources.
 *
 * pad.c owns the Dolphin-shaped public API (PADRead, PADClamp, ...) and lays
 * the pads it finds at PADInit over the four controller ports in order: every
 * Xbox One pad on the USB bus (pad_xone.c), then every joystick/game
 * controller SDL reports (pad_sdl.c), up to four.  The keyboard (pad_sdl.c
 * as well) is port 1's fallback: alone on port 1 when no pad is there, beside
 * whatever pad holds port 1 otherwise -- or, with --kbport N, a controller
 * of its own on port N (a second player at the desk; M34).  A `--play`
 * script (pad_play.c) is layered on top of port 1's raw state: it never
 * replaces it, it is fed into PADRead as if it *were* the raw state, so the
 * game's own repeat/edge logic in src/game/pad.c runs unchanged (see
 * port/ref/notes.md §6.3).  Recording captures that same merged raw state.
 *
 * A port with no pad reports PAD_ERR_NO_CONTROLLER on every read, which is
 * what makes `PlayerConfig.iscom` come out CPU for that player (notes.md
 * §4); with one pad that is the reference rig's pinned configuration.
 */
#ifndef PORT_PAD_INTERNAL_H
#define PORT_PAD_INTERNAL_H

#include "port.h"

#include <dolphin/types.h>
#include <dolphin/pad.h>

/* The most pads one run lays over the four ports (PAD_CHANMAX). */
#define PORT_PAD_MAX 4

/* One frame's worth of one controller's raw state, before PADClamp. Signed stick
 * axes are the pot's own -100..100-ish range (not yet deadzoned); triggers
 * are 0..255. `button` carries only the digital bits a real pad has:
 * PAD_BUTTON_{LEFT,RIGHT,DOWN,UP,A,B,X,Y,START}, PAD_TRIGGER_{Z,L,R}. */
typedef struct PortPadRaw {
    u16 button;
    s8 stickX, stickY;
    s8 substickX, substickY;
    u8 triggerL, triggerR;
} PortPadRaw;

/* ---- pad_sdl.c: SDL joysticks/game controllers + the keyboard ------------ */
void pad_sdl_init(void);              /* opens every pad SDL reports, up to PORT_PAD_MAX */
void pad_sdl_shutdown(void);
int pad_sdl_count(void);              /* SDL pads open (the keyboard is not one) */
const char* pad_sdl_name(int i);      /* the i-th pad's name */
void pad_sdl_poll_pad(int i, PortPadRaw* out);
void pad_sdl_poll_keys(PortPadRaw* out); /* the keyboard alone */
int pad_sdl_rumble_supported(int i);
void pad_sdl_rumble(int i, int on);

/* ---- pad_xone.c: Xbox One controllers over IOUSBLib ---------------------- */
int pad_xone_open(void);              /* claims every Xbox One pad on the bus; the count */
int pad_xone_count(void);
int pad_xone_present(int i);          /* still answering (its reader has not lost it) */
const char* pad_xone_name(int i);
void pad_xone_poll(int i, PortPadRaw* out);
void pad_xone_rumble(int i, int on);
void pad_xone_close(void);            /* all of them */

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
