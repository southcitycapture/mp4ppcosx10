/* PAD: the real thing.
 *
 * `PADRead` fills all four `PADStatus` slots once per retrace (called from
 * the game's own VI post-retrace callback, src/game/pad.c:152). The ports
 * are laid out once, at PADInit, from what is on the machine (M34):
 *
 *   1. every Xbox One controller over IOUSBLib (pad_xone.c) -- the pad this
 *      port was actually developed against, plugged into the G4 -- in the
 *      order the USB registry lists them
 *   2. then every joystick or game controller SDL reports (pad_sdl.c)
 *   3. the keyboard (also pad_sdl.c): port 1's fallback when no pad holds
 *      port 1, OR'd under the pad that does otherwise (a player at the desk
 *      can press Start on either, M32) -- or, with --kbport N, a controller
 *      of its own on port N (a second player on the keys)
 *
 * A `--play` script (pad_play.c) sits on top of whatever port 1 read and
 * overwrites it for whichever frames the script covers; `--record` writes
 * back whatever port 1 actually read, script or not. Both work in raw,
 * pre-clamp state, per notes.md §6.3: nothing here synthesises a button-down
 * edge or a `_PadDStk` value -- that is `HuPadRead`'s and `PadADConv`'s job,
 * and they must see the same kind of state a real pad would produce.
 *
 * A port with nothing on it reports PAD_ERR_NO_CONTROLLER. With one pad that
 * is the reference configuration: notes.md §4 traces `PlayerConfig.iscom` to
 * which SI ports report a pad, and the rig this port is checked against pins
 * ports 2-3 empty so players 2-4 come up CPU. A second pad makes player 2
 * human, as it does on a console.
 *
 * `PADClamp` is the genuine GameCube clamp region and math, ported from the
 * decomp's own (unused-by-the-port-build) src/dolphin/pad/Padclamp.c, so a
 * raw stick push here goes through the same deadzone/diamond-clamp the game
 * expects. `PADClampCircle` is not called anywhere in this game (checked:
 * no call site in src/) -- it is a best-effort circular equivalent, not a
 * recovered original.
 */
#include "pad_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

u32 __PADFixBits;
int port_pad_debug;

/* M34: what each controller port reads from.  `idx` is the pad's index in
 * its driver; the keyboard is one source (KB) and, when it is not a port's
 * own source, it is OR'd under port 1 (`kb_under_port1`). */
enum { SRC_NONE, SRC_XONE, SRC_SDL, SRC_KB };
typedef struct PadSource {
    int kind;
    int idx;
} PadSource;
static PadSource src[PAD_CHANMAX];
static int kb_under_port1; /* the keyboard beside whatever pad holds port 1 */
static PADSamplingCallback sampling_cb;
static u32 pad_spec;
static u32 analog_mode;

/* Rumble: PADControlMotor is called once per port per retrace by
 * src/game/pad.c's own rumble sequencer; it goes to the pad on that port. */
static u32 motor_cmd[PAD_CHANMAX];

void port_pad_shutdown(void);

/* The game calls PADInit twice on its own (src/game/init.c:55, then again
 * from HuPadInit at src/game/pad.c:60) -- real hardware tolerates that, and
 * so must this: only the first call probes for a pad, loads a --play script
 * or opens a --record file. A second call is a harmless no-op, same as the
 * SDK's own PADInit re-running PADReset on an already-live channel. */
static int pad_inited;

BOOL PADInit(void) {
    if (pad_inited) {
        return TRUE;
    }
    pad_inited = 1;

    port_pad_debug = port_opt.pad_debug;

    memset(src, 0, sizeof(src));
    kb_under_port1 = 0;
    if (port_opt.nopad) {
        port_log("port> PADInit: --nopad, controller 1 unplugged\n");
    } else {
        PadSource pads[PORT_PAD_MAX * 2];
        int npads = 0, n = 0, i, last = 0;
        int kbport = (port_opt.kbport >= 1 && port_opt.kbport <= PAD_CHANMAX) ? port_opt.kbport : 0;
        for (i = 0; i < pad_xone_open(); i++) {
            pads[npads].kind = SRC_XONE;
            pads[npads++].idx = i;
        }
        pad_sdl_init();
        for (i = 0; i < pad_sdl_count(); i++) {
            pads[npads].kind = SRC_SDL;
            pads[npads++].idx = i;
        }
        /* the ports, in order: the pads fill them, the keyboard's own port
         * (--kbport) is skipped over; with no --kbport the keyboard is
         * controller 1 alone or beside the pad that holds it (M32) */
        if (kbport) {
            src[kbport - 1].kind = SRC_KB;
        }
        for (i = 0; i < npads; i++) {
            while (n < PAD_CHANMAX && src[n].kind != SRC_NONE) {
                n++;
            }
            if (n >= PAD_CHANMAX) {
                port_log("port> PADInit: %d pads found, only %d fit the four ports\n", npads, i);
                break;
            }
            src[n] = pads[i];
        }
        if (!kbport) {
            if (src[0].kind == SRC_NONE) {
                src[0].kind = SRC_KB;
            } else {
                kb_under_port1 = 1;
            }
        }
        for (i = 0; i < PAD_CHANMAX; i++) {
            const char* what = NULL;
            const char* drv = "";
            switch (src[i].kind) {
            case SRC_XONE: what = pad_xone_name(src[i].idx); drv = " (driver: Xbox-One-IOUSBLib)"; break;
            case SRC_SDL: what = pad_sdl_name(src[i].idx); drv = " (driver: SDL joystick)"; break;
            case SRC_KB: what = "keyboard"; drv = kbport ? " (--kbport)" : ""; break;
            default: break;
            }
            if (what) {
                port_log("port> PADInit: controller %d = %s%s\n", i + 1, what, drv);
                last = i + 1;
            }
        }
        if (kb_under_port1) {
            port_log("port> PADInit: the keyboard works beside controller 1\n");
        }
        {
            int rumble = src[0].kind == SRC_XONE ? 1
                       : src[0].kind == SRC_SDL ? pad_sdl_rumble_supported(src[0].idx) : 0;
            port_log("port> PADInit: rumble %savailable on controller 1\n", rumble ? "" : "not ");
        }
        if (last < PAD_CHANMAX) {
            port_log("port> PADInit: controllers %d-4 unplugged%s\n", last + 1,
                     last == 1 ? " (reference config: CPU players)" : "");
        }
    }

    pad_play_init(port_opt.pad_play, port_opt.pad_record);

    memset(motor_cmd, 0, sizeof(motor_cmd));

    /* Every exit path (the game's own return, port_shutdown(), a panic) goes
     * through exit(), so an atexit hook -- registered here rather than added
     * to main.c's own shutdown sequence -- is enough to close the pad and
     * flush a --record file without touching a file this lane does not own. */
    atexit(port_pad_shutdown);
    return TRUE;
}

/* ---- the harness's own thumb (--soak's prompt navigator) -------------------
 *
 * port/src/debug/selfplay.c arms this from the VI post-retrace callback; the
 * next PADRead consumes it.  It is deliberately the *raw* layer and not
 * `HuPadBtnDown[]`: src/game/main.c:101 runs `HuPadRead()` at the top of every
 * frame, so anything written to the derived globals after a retrace is gone
 * before `HuPrcCall(1)` dispatches a coroutine.  A `--play` script has always
 * entered here, which is why the boot walk's A metronome works and the
 * navigator's did not.  PLAN.md 29.1. */
static u16 inject_buttons;

void port_pad_inject(unsigned short buttons) { inject_buttons |= (u16)buttons; }

static void poll_source(const PadSource* p, PortPadRaw* raw) {
    switch (p->kind) {
    case SRC_XONE: pad_xone_poll(p->idx, raw); break;
    case SRC_SDL: pad_sdl_poll_pad(p->idx, raw); break;
    case SRC_KB: pad_sdl_poll_keys(raw); break;
    default: memset(raw, 0, sizeof(*raw)); break;
    }
}

/* `under` beneath `raw`: the buttons OR, a stick only where raw's rests, the
 * larger trigger. */
static void merge_under(PortPadRaw* raw, const PortPadRaw* under) {
    raw->button |= under->button;
    if (raw->stickX == 0 && raw->stickY == 0) {
        raw->stickX = under->stickX;
        raw->stickY = under->stickY;
    }
    if (raw->substickX == 0 && raw->substickY == 0) {
        raw->substickX = under->substickX;
        raw->substickY = under->substickY;
    }
    if (under->triggerL > raw->triggerL) {
        raw->triggerL = under->triggerL;
    }
    if (under->triggerR > raw->triggerR) {
        raw->triggerR = under->triggerR;
    }
}

u32 PADRead(PADStatus* status) {
    u32 chan_bits = 0;
    int i;
    extern u32 VIGetRetraceCount(void);
    u32 frame = VIGetRetraceCount();
    u16 inject = inject_buttons;

    inject_buttons = 0;

    memset(status, 0, sizeof(PADStatus) * PAD_CHANMAX);
    for (i = 0; i < PAD_CHANMAX; i++) {
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }

    /* `--nopad` says "controller 1 is unplugged", and it stays unplugged
     * unless the harness is actually pressing something this frame -- a soak
     * that has to answer a prompt is the one case where the port is allowed to
     * contradict it, and it says so in the run's own log the first time. */
    for (i = 0; i < PAD_CHANMAX; i++) {
        PortPadRaw raw;
        int live = !port_opt.nopad && src[i].kind != SRC_NONE;
        if (!live && !(i == 0 && inject)) {
            continue;
        }
        memset(&raw, 0, sizeof(raw));
        if (live) {
            poll_source(&src[i], &raw);
            if (i == 0 && kb_under_port1) {
                /* M32: the keyboard under the pad on port 1 -- a player at the
                 * desk can press Start on either.  The pad's sticks win when
                 * they are off centre; the keys' stick only when they rest. */
                PortPadRaw kb;
                pad_sdl_poll_keys(&kb);
                merge_under(&raw, &kb);
            }
        }
        if (i == 0) {
            if (!port_opt.nopad) {
                pad_play_step(frame, &raw); /* a --play script overwrites raw for its frames */
            }
            /* The harness's press is OR'd *after* the script, so the two can
             * never cancel: a script that is still running holds whatever it
             * holds and the navigator adds a button to it.  In practice they
             * do not overlap -- the navigator only fires on a screen that has
             * stopped moving, and board-start-com4.play stops at frame 29,960. */
            raw.button |= inject;
        }
        if (port_pad_debug && (raw.button || raw.stickX || raw.stickY)) {
            extern u8 HuPadDStk[4];
            extern u8 HuPadDStkRep[4];
            port_log("pad> frame %u: port %d btn %04x stick %d,%d  (last frame's dstk %02x "
                     "rep %02x)\n",
                     (unsigned)frame, i + 1, (unsigned)raw.button, (int)raw.stickX,
                     (int)raw.stickY, (unsigned)HuPadDStk[i],
                     (unsigned)HuPadDStkRep[i]);
        }

        status[i].button = raw.button;
        status[i].stickX = raw.stickX;
        status[i].stickY = raw.stickY;
        status[i].substickX = raw.substickX;
        status[i].substickY = raw.substickY;
        status[i].triggerL = raw.triggerL;
        status[i].triggerR = raw.triggerR;
        status[i].analogA = status[i].analogB = 0;
        status[i].err = PAD_ERR_NONE;
        chan_bits |= PAD_CHAN0_BIT >> i;
    }

    return chan_bits;
}

BOOL PADReset(u32 mask) {
    (void)mask;
    return TRUE;
}

BOOL PADRecalibrate(u32 mask) {
    (void)mask;
    return TRUE;
}

/* ---- clamping: ported from src/dolphin/pad/Padclamp.c (not built by the
 * port -- TARGET_PC never compiles src/dolphin), so a raw stick/trigger push
 * from any of the three sources above goes through the real deadzone and
 * diamond-shaped clamp the game was written against. ------------------------ */

struct PadClampRegion {
    u8 minTrigger, maxTrigger;
    s8 minStick, maxStick, xyStick;
    s8 minSubstick, maxSubstick, xySubstick;
};

static const struct PadClampRegion clamp_region = {
    30, 180,      /* triggers */
    15, 72, 40,   /* main stick */
    15, 59, 31,   /* substick */
};

static void clamp_stick(s8* px, s8* py, s8 max, s8 xy, s8 min) {
    int x = *px;
    int y = *py;
    int signX, signY, d;

    signX = (x >= 0) ? 1 : -1;
    x = (x >= 0) ? x : -x;
    signY = (y >= 0) ? 1 : -1;
    y = (y >= 0) ? y : -y;

    x = (x <= min) ? 0 : x - min;
    y = (y <= min) ? 0 : y - min;

    if (x == 0 && y == 0) {
        *px = *py = 0;
        return;
    }

    if (xy * y <= xy * x) {
        d = xy * x + (max - xy) * y;
        if (xy * max < d) {
            x = xy * max * x / d;
            y = xy * max * y / d;
        }
    } else {
        d = xy * y + (max - xy) * x;
        if (xy * max < d) {
            x = xy * max * x / d;
            y = xy * max * y / d;
        }
    }
    *px = (s8)(signX * x);
    *py = (s8)(signY * y);
}

void PADClamp(PADStatus* status) {
    int i;
    for (i = 0; i < PAD_CHANMAX; i++, status++) {
        if (status->err != PAD_ERR_NONE) {
            continue;
        }
        clamp_stick(&status->stickX, &status->stickY, clamp_region.maxStick, clamp_region.xyStick,
                    clamp_region.minStick);
        clamp_stick(&status->substickX, &status->substickY, clamp_region.maxSubstick, clamp_region.xySubstick,
                    clamp_region.minSubstick);
        if (status->triggerL <= clamp_region.minTrigger) {
            status->triggerL = 0;
        } else {
            if (status->triggerL > clamp_region.maxTrigger) {
                status->triggerL = clamp_region.maxTrigger;
            }
            status->triggerL = (u8)(status->triggerL - clamp_region.minTrigger);
        }
        if (status->triggerR <= clamp_region.minTrigger) {
            status->triggerR = 0;
        } else {
            if (status->triggerR > clamp_region.maxTrigger) {
                status->triggerR = clamp_region.maxTrigger;
            }
            status->triggerR = (u8)(status->triggerR - clamp_region.minTrigger);
        }
    }
}

/* Not called anywhere in this game (no PADClampCircle call site in src/): a
 * plain circular equivalent of clamp_stick, offered for completeness. */
static void clamp_circle(s8* px, s8* py, s8 max, s8 min) {
    double x = *px, y = *py;
    double len = sqrt(x * x + y * y);
    if (len <= min) {
        *px = *py = 0;
        return;
    }
    {
        double scale = (len - min) / (len);
        double nx = x * scale, ny = y * scale;
        double nlen = sqrt(nx * nx + ny * ny);
        if (nlen > max) {
            nx = nx * max / nlen;
            ny = ny * max / nlen;
        }
        *px = (s8)nx;
        *py = (s8)ny;
    }
}

void PADClampCircle(PADStatus* status) {
    int i;
    for (i = 0; i < PAD_CHANMAX; i++, status++) {
        if (status->err != PAD_ERR_NONE) {
            continue;
        }
        clamp_circle(&status->stickX, &status->stickY, clamp_region.maxStick, clamp_region.minStick);
        clamp_circle(&status->substickX, &status->substickY, clamp_region.maxSubstick, clamp_region.minSubstick);
    }
}

void PADControlMotor(s32 chan, u32 cmd) {
    if (chan < 0 || chan >= PAD_CHANMAX) {
        return;
    }
    motor_cmd[chan] = cmd;
    {
        int on = (cmd == PAD_MOTOR_RUMBLE);
        if (src[chan].kind == SRC_XONE) {
            pad_xone_rumble(src[chan].idx, on);
        } else if (src[chan].kind == SRC_SDL) {
            pad_sdl_rumble(src[chan].idx, on);
        }
    }
}

void PADSetSpec(u32 spec) { pad_spec = spec; }
void PADSetAnalogMode(u32 mode) { analog_mode = mode; }

void PADControlAllMotors(const u32* cmdArr) {
    int i;
    for (i = 0; i < PAD_CHANMAX; i++) {
        PADControlMotor(i, cmdArr[i]);
    }
}

PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback cb) {
    PADSamplingCallback old = sampling_cb;
    sampling_cb = cb;
    return old;
}

u32 SISetSamplingRate(u32 msec) {
    (void)msec;
    return 0;
}

void port_pad_shutdown(void) {
    pad_play_shutdown();
    pad_xone_close();
    pad_sdl_shutdown();
}

/* ---- snapshots ------------------------------------------------------------
 * The pad's *readings* are re-derived every frame (from the replay script,
 * which is a pure function of the frame number, or from a real controller).
 * What the game set and expects to still be set is here: the sampling
 * callback, the spec and analog modes, and the rumble commands in force. */
void port_pad_snap_register(void) {
    port_snap_register("pad.sampling_cb", &sampling_cb, sizeof(sampling_cb));
    port_snap_register("pad.spec", &pad_spec, sizeof(pad_spec));
    port_snap_register("pad.analog_mode", &analog_mode, sizeof(analog_mode));
    port_snap_register("pad.motor_cmd", motor_cmd, sizeof(motor_cmd));
}
