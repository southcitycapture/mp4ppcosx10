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

/* ---- pad_controls.c: PowerPCube's controls file (M46, PLAN.md 61) --------- */
/* a pad's state in SDL game-controller terms, for the table */
enum { PADIN_A, PADIN_B, PADIN_X, PADIN_Y, PADIN_BACK, PADIN_GUIDE, PADIN_START, PADIN_LSTICK,
       PADIN_RSTICK, PADIN_LSHOULDER, PADIN_RSHOULDER, PADIN_DPUP, PADIN_DPDOWN, PADIN_DPLEFT,
       PADIN_DPRIGHT, PADIN_NBUTTONS };
enum { PADIN_AX_LX, PADIN_AX_LY, PADIN_AX_RX, PADIN_AX_RY, PADIN_AX_LT, PADIN_AX_RT, PADIN_NAXES };
typedef struct PadInState {
    unsigned char button[PADIN_NBUTTONS];
    int axis[PADIN_NAXES]; /* SDL's ranges: sticks -32768..32767 (y down), triggers 0..32767 */
} PadInState;
enum { PADCTL_AUTO = -3, PADCTL_NONE = -2, PADCTL_KEYBOARD = -1, PADCTL_PAD0 = 0 };
void pad_controls_init(void);       /* PADInit: reads the file unless the run is scripted */
void pad_controls_sdl_db(void);     /* PowerPCube's gamecontrollerdb.txt into SDL */
int pad_controls_active(void);      /* a controls file was read */
int pad_controls_player(int port);  /* PADCTL_*, or the pad's index in the Xbox-first order */
void pad_controls_eval(const PadInState* st, PortPadRaw* out);
void pad_controls_keys(const unsigned char* kb, PortPadRaw* out);
/* a stick axis pair to the pot's range: pad_sdl.c's dead zone and clamp, or
 * the Xbox One driver's straight scale (each driver's own, as before) */
extern void (*pad_stick_map)(int ax, int ay, s8* x, s8* y);

/* ---- pad_sdl.c: SDL joysticks/game controllers + the keyboard ------------ */
void pad_sdl_init(void);              /* opens every pad SDL reports, up to PORT_PAD_MAX */
void pad_sdl_shutdown(void);
int pad_sdl_count(void);              /* SDL pads open (the keyboard is not one) */
const char* pad_sdl_name(int i);      /* the i-th pad's name */
void pad_sdl_poll_pad(int i, PortPadRaw* out);
void pad_sdl_poll_keys(PortPadRaw* out); /* the keyboard alone */
void pad_sdl_keytest(const char* names); /* M46: --keytest */
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
