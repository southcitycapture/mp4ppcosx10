/* The keyboard and SDL joysticks/game controllers as GameCube controllers.
 *
 * Keyboard (pad.c polls it every frame: alone as controller 1 with no pad,
 * OR'd under the pad that holds port 1 otherwise, or a controller of its own
 * with --kbport -- the game is playable and testable with no pad plugged in
 * at all):
 *
 *   arrows / WASD    main stick        Return        Start
 *   Z X C V          A B X Y           Q / E         L / R triggers
 *   I J K L          C stick           Shift or Tab  Z trigger
 *   T F G H          the GC pad's own digital dpad
 *   F5 or F12  a screenshot onto the Desktop  Escape / Cmd-Q  quit (SDL events,
 *                                                            handled in gl13.c)
 *
 * (M32 settled this table for the shipped app -- `--keys` prints it; before
 * M32 Q was the Z trigger and there were no L/R or C-stick keys.)
 *
 * SDL pad: any controller SDL's own mapping table (or SDL_GAMECONTROLLERCONFIG)
 * recognises maps
 *
 *   left stick -> main stick      right stick -> C stick
 *   A B X Y    -> GC A B X Y      LB -> Z
 *   LT/RT      -> triggerL/triggerR, plus the PAD_TRIGGER_L/R click bits
 *   Start      -> Start           dpad -> the GC pad's own digital dpad
 *
 * An SDL joystick with no game-controller mapping falls back to a raw guess
 * (axes 0/1, buttons 0-3, hat 0) good enough to steer a menu.
 *
 * Both the keyboard and the pad are read with SDL_GetKeyboardState /
 * SDL_JoystickGetAxis-family calls, never SDL_PollEvent: the event pump
 * already lives in gl13.c's SwapBuffers path (see PLAN.md), and a second
 * consumer would starve it of events one frame in two. Hot-plug is therefore
 * not handled here -- the pads present at PADInit are the pads for the run,
 * which is fine for a port whose target session is "the pads, plugged in
 * before launch".  M34: every pad SDL reports is opened, up to PORT_PAD_MAX,
 * in SDL's order; pad.c lays them over the ports after the Xbox One pads.
 */
#include "pad_internal.h"

#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>

typedef struct SdlPad {
    SDL_GameController* pad;
    SDL_Joystick* joy; /* raw fallback, or the controller's own joystick */
    SDL_Haptic* haptic;
    char name[128];
} SdlPad;
static SdlPad pads[PORT_PAD_MAX];
static int npads;

static void open_pads(void) {
    int i;
    for (i = 0; i < SDL_NumJoysticks() && npads < PORT_PAD_MAX; i++) {
        SdlPad* p = &pads[npads];
        memset(p, 0, sizeof(*p));
        /* M34: an Xbox One pad is pad_xone.c's (IOUSBLib claims it before
         * SDL runs); should a HID layer list it too, it must not become a
         * second controller that follows the first.  Not seen on the G4
         * (GIP pads are not HID-class): a guard, not a finding. */
        if (pad_xone_count() > 0) {
            const char* jn = SDL_JoystickNameForIndex(i);
            if (jn != NULL && strstr(jn, "Xbox One") != NULL) {
                continue;
            }
        }
        if (SDL_IsGameController(i)) {
            p->pad = SDL_GameControllerOpen(i);
            if (p->pad != NULL) {
                p->joy = SDL_GameControllerGetJoystick(p->pad);
                snprintf(p->name, sizeof(p->name), "%s", SDL_GameControllerName(p->pad));
            }
        }
        if (p->joy == NULL) {
            p->joy = SDL_JoystickOpen(i);
            if (p->joy != NULL) {
                snprintf(p->name, sizeof(p->name), "%s (raw joystick, no SDL mapping)", SDL_JoystickName(p->joy));
            }
        }
        if (p->joy == NULL) {
            continue;
        }
        if (SDL_JoystickIsHaptic(p->joy)) {
            p->haptic = SDL_HapticOpenFromJoystick(p->joy);
            if (p->haptic != NULL && SDL_HapticRumbleInit(p->haptic) != 0) {
                SDL_HapticClose(p->haptic);
                p->haptic = NULL;
            }
        }
        npads++;
    }
}

void pad_sdl_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        port_log("port> pad: SDL joystick subsystem unavailable (%s); keyboard only\n", SDL_GetError());
        return;
    }
    open_pads();
}

void pad_sdl_shutdown(void) {
    int i;
    for (i = 0; i < npads; i++) {
        SdlPad* p = &pads[i];
        if (p->haptic != NULL) {
            SDL_HapticClose(p->haptic);
            p->haptic = NULL;
        }
        if (p->pad != NULL) {
            SDL_GameControllerClose(p->pad);
            p->pad = NULL;
        } else if (p->joy != NULL) {
            SDL_JoystickClose(p->joy);
        }
        p->joy = NULL;
    }
    npads = 0;
}

int pad_sdl_count(void) { return npads; }
const char* pad_sdl_name(int i) { return (i >= 0 && i < npads) ? pads[i].name : "keyboard"; }
int pad_sdl_rumble_supported(int i) { return i >= 0 && i < npads && pads[i].haptic != NULL; }

void pad_sdl_rumble(int i, int on) {
    if (i < 0 || i >= npads || pads[i].haptic == NULL) {
        return;
    }
    if (on) {
        SDL_HapticRumblePlay(pads[i].haptic, 0.7f, 5000);
    } else {
        SDL_HapticRumbleStop(pads[i].haptic);
    }
}

static s8 axis_key(const Uint8* k, SDL_Scancode neg, SDL_Scancode pos, SDL_Scancode neg2, SDL_Scancode pos2) {
    int v = 0;
    if (k[neg] || k[neg2]) {
        v -= 100;
    }
    if (k[pos] || k[pos2]) {
        v += 100;
    }
    return (s8)v;
}

/* SDL axis (-32768..32767) to the GC pot's own -100..100, with a dead zone
 * and a circular clamp so a full diagonal does not exceed the pot's range. */
static void map_stick(int ax, int ay, s8* x, s8* y) {
    const int dead = 5000;
    long mx = ax, my = -ay; /* SDL's Y is down; the pot's is up */
    long long len2;
    if (mx > -dead && mx < dead) {
        mx = 0;
    }
    if (my > -dead && my < dead) {
        my = 0;
    }
    if (mx == 0 && my == 0) {
        return; /* keep whatever the keyboard already put there */
    }
    mx = mx * 100 / 32767;
    my = my * 100 / 32767;
    len2 = (long long)mx * mx + (long long)my * my;
    if (len2 > 100 * 100) {
        double s = 100.0 / __builtin_sqrt((double)len2);
        mx = (long)(mx * s);
        my = (long)(my * s);
    }
    *x = (s8)mx;
    *y = (s8)my;
}

void pad_sdl_poll_keys(PortPadRaw* out) {
    const Uint8* k = SDL_GetKeyboardState(NULL);
    u16 b = 0;

    memset(out, 0, sizeof(*out));

    if (k[SDL_SCANCODE_Z]) {
        b |= PAD_BUTTON_A;
    }
    if (k[SDL_SCANCODE_X]) {
        b |= PAD_BUTTON_B;
    }
    if (k[SDL_SCANCODE_C]) {
        b |= PAD_BUTTON_X;
    }
    if (k[SDL_SCANCODE_V]) {
        b |= PAD_BUTTON_Y;
    }
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_TAB]) {
        b |= PAD_TRIGGER_Z;
    }
    if (k[SDL_SCANCODE_Q]) {
        b |= PAD_TRIGGER_L;
        out->triggerL = 255;
    }
    if (k[SDL_SCANCODE_E]) {
        b |= PAD_TRIGGER_R;
        out->triggerR = 255;
    }
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) {
        b |= PAD_BUTTON_START;
    }
    if (k[SDL_SCANCODE_T]) {
        b |= PAD_BUTTON_UP;
    }
    if (k[SDL_SCANCODE_G]) {
        b |= PAD_BUTTON_DOWN;
    }
    if (k[SDL_SCANCODE_F]) {
        b |= PAD_BUTTON_LEFT;
    }
    if (k[SDL_SCANCODE_H]) {
        b |= PAD_BUTTON_RIGHT;
    }

    out->stickX = axis_key(k, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_A, SDL_SCANCODE_D);
    out->stickY = axis_key(k, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, SDL_SCANCODE_S, SDL_SCANCODE_W);
    out->substickX = axis_key(k, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_L);
    out->substickY = axis_key(k, SDL_SCANCODE_K, SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_I);
    out->button = b;
}

/* The i-th SDL pad alone (pad.c merges it with the keyboard for port 1). */
void pad_sdl_poll_pad(int i, PortPadRaw* out) {
    SDL_GameController* pad;
    SDL_Joystick* joy;
    u16 b = 0;

    memset(out, 0, sizeof(*out));
    if (i < 0 || i >= npads) {
        return;
    }
    pad = pads[i].pad;
    joy = pads[i].joy;
    if (pad != NULL) {
        int rx, ry;
        map_stick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX),
                  SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), &out->stickX, &out->stickY);
        rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        map_stick(rx, ry, &out->substickX, &out->substickY);
        out->triggerL = (u8)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        out->triggerR = (u8)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) {
            b |= PAD_BUTTON_A;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) {
            b |= PAD_BUTTON_B;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) {
            b |= PAD_BUTTON_X;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) {
            b |= PAD_BUTTON_Y;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
            b |= PAD_BUTTON_START;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
            b |= PAD_TRIGGER_Z;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            b |= PAD_BUTTON_UP;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            b |= PAD_BUTTON_DOWN;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            b |= PAD_BUTTON_LEFT;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            b |= PAD_BUTTON_RIGHT;
        }
        if (out->triggerL >= 200) {
            b |= PAD_TRIGGER_L;
        }
        if (out->triggerR >= 200) {
            b |= PAD_TRIGGER_R;
        }
    } else if (joy != NULL) {
        map_stick(SDL_JoystickGetAxis(joy, 0), SDL_JoystickGetAxis(joy, 1), &out->stickX, &out->stickY);
        if (SDL_JoystickNumAxes(joy) > 3) {
            map_stick(SDL_JoystickGetAxis(joy, 2), SDL_JoystickGetAxis(joy, 3), &out->substickX, &out->substickY);
        }
        if (SDL_JoystickGetButton(joy, 0)) {
            b |= PAD_BUTTON_A;
        }
        if (SDL_JoystickGetButton(joy, 1)) {
            b |= PAD_BUTTON_B;
        }
        if (SDL_JoystickGetButton(joy, 2)) {
            b |= PAD_BUTTON_X;
        }
        if (SDL_JoystickGetButton(joy, 3)) {
            b |= PAD_BUTTON_Y;
        }
        if (SDL_JoystickGetButton(joy, 7) || SDL_JoystickGetButton(joy, 9)) {
            b |= PAD_BUTTON_START;
        }
        if (SDL_JoystickNumHats(joy) > 0) {
            Uint8 h = SDL_JoystickGetHat(joy, 0);
            if (h & SDL_HAT_UP) {
                b |= PAD_BUTTON_UP;
            }
            if (h & SDL_HAT_DOWN) {
                b |= PAD_BUTTON_DOWN;
            }
            if (h & SDL_HAT_LEFT) {
                b |= PAD_BUTTON_LEFT;
            }
            if (h & SDL_HAT_RIGHT) {
                b |= PAD_BUTTON_RIGHT;
            }
        }
    }
    out->button = b;
}

#else /* PORT_NO_SDL */

void pad_sdl_init(void) { port_log("port> pad: built without SDL; keyboard/pad unavailable\n"); }
void pad_sdl_shutdown(void) {}
int pad_sdl_count(void) { return 0; }
const char* pad_sdl_name(int i) { (void)i; return "(no SDL)"; }
void pad_sdl_poll_pad(int i, PortPadRaw* out) { (void)i; memset(out, 0, sizeof(*out)); }
void pad_sdl_poll_keys(PortPadRaw* out) { memset(out, 0, sizeof(*out)); }
int pad_sdl_rumble_supported(int i) { (void)i; return 0; }
void pad_sdl_rumble(int i, int on) { (void)i; (void)on; }

#endif
