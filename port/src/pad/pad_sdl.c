/* Keyboard and SDL joystick/game-controller as GameCube controller 1.
 *
 * Keyboard (always polled, OR'd under whatever pad is open -- the game is
 * playable and testable with no pad plugged in at all):
 *
 *   arrows / WASD    main stick        Return        Start
 *   Z X C V          A B X Y           Q             Z trigger
 *   T F G H          the GC pad's own digital dpad    Escape  quit (SDL event,
 *                                                       handled in gl13.c)
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
 * not handled here -- the pad present at PADInit is the pad for the run,
 * which is fine for a port whose target session is "one pad, plugged in
 * before launch".
 */
#include "pad_internal.h"

#include <string.h>

#ifndef PORT_NO_SDL
#include <SDL.h>

static SDL_GameController* pad;
static SDL_Joystick* joy; /* raw fallback, or the controller's own joystick */
static SDL_Haptic* haptic;
static char pad_name[128];

static void open_pad(void) {
    int i;
    for (i = 0; i < SDL_NumJoysticks() && joy == NULL; i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad != NULL) {
                joy = SDL_GameControllerGetJoystick(pad);
                snprintf(pad_name, sizeof(pad_name), "%s", SDL_GameControllerName(pad));
            }
        }
        if (joy == NULL) {
            joy = SDL_JoystickOpen(i);
            if (joy != NULL) {
                snprintf(pad_name, sizeof(pad_name), "%s (raw joystick, no SDL mapping)", SDL_JoystickName(joy));
            }
        }
    }
    if (joy != NULL && SDL_JoystickIsHaptic(joy)) {
        haptic = SDL_HapticOpenFromJoystick(joy);
        if (haptic != NULL && SDL_HapticRumbleInit(haptic) != 0) {
            SDL_HapticClose(haptic);
            haptic = NULL;
        }
    }
}

void pad_sdl_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        port_log("port> pad: SDL joystick subsystem unavailable (%s); keyboard only\n", SDL_GetError());
        return;
    }
    open_pad();
}

void pad_sdl_shutdown(void) {
    if (haptic != NULL) {
        SDL_HapticClose(haptic);
        haptic = NULL;
    }
    if (pad != NULL) {
        SDL_GameControllerClose(pad);
        pad = NULL;
    } else if (joy != NULL) {
        SDL_JoystickClose(joy);
    }
    joy = NULL;
}

int pad_sdl_present(void) { return joy != NULL; }
const char* pad_sdl_name(void) { return joy != NULL ? pad_name : "keyboard"; }
int pad_sdl_rumble_supported(void) { return haptic != NULL; }

void pad_sdl_rumble(int on) {
    if (haptic == NULL) {
        return;
    }
    if (on) {
        SDL_HapticRumblePlay(haptic, 0.7f, 5000);
    } else {
        SDL_HapticRumbleStop(haptic);
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

void pad_sdl_poll(PortPadRaw* out) {
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
    if (k[SDL_SCANCODE_Q]) {
        b |= PAD_TRIGGER_Z;
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
int pad_sdl_present(void) { return 0; }
const char* pad_sdl_name(void) { return "(no SDL)"; }
void pad_sdl_poll(PortPadRaw* out) { memset(out, 0, sizeof(*out)); }
int pad_sdl_rumble_supported(void) { return 0; }
void pad_sdl_rumble(int on) { (void)on; }

#endif
