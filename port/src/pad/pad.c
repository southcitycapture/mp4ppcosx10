/* PAD: the real thing.
 *
 * `PADRead` fills all four `PADStatus` slots once per retrace (called from
 * the game's own VI post-retrace callback, src/game/pad.c:152). Port 1 gets
 * whichever live source PADInit found, in priority order:
 *
 *   1. an Xbox One controller over IOUSBLib (pad_xone.c) -- the pad this port
 *      was actually developed against, plugged into the G4
 *   2. an SDL joystick or game controller (pad_sdl.c)
 *   3. the keyboard alone (also pad_sdl.c)
 *
 * A `--play` script (pad_play.c) sits on top of whatever port 1 read and
 * overwrites it for whichever frames the script covers; `--record` writes
 * back whatever port 1 actually read, script or not. Both work in raw,
 * pre-clamp state, per notes.md §6.3: nothing here synthesises a button-down
 * edge or a `_PadDStk` value -- that is `HuPadRead`'s and `PadADConv`'s job,
 * and they must see the same kind of state a real pad would produce.
 *
 * Ports 2-4 always report PAD_ERR_NO_CONTROLLER. That is not a limitation,
 * it is the reference configuration: notes.md §4 traces `PlayerConfig.iscom`
 * to which SI ports report a pad, and the rig this port is checked against
 * pins ports 2-3 empty so players 2-4 come up CPU.
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

static int have_pad;      /* an Xbox One or SDL pad is open (not just the keyboard) */
static int use_xone;
static PADSamplingCallback sampling_cb;
static u32 pad_spec;
static u32 analog_mode;

/* Rumble: PADControlMotor is called once per port per retrace by
 * src/game/pad.c's own rumble sequencer; only port 0 can possibly rumble
 * here, since ports 1-3 never report a controller. */
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

    if (port_opt.nopad) {
        port_log("port> PADInit: --nopad, controller 1 unplugged\n");
    } else {
        use_xone = pad_xone_open();
        if (!use_xone) {
            pad_sdl_init();
        }
        have_pad = use_xone || pad_sdl_present();
        if (use_xone) {
            port_log("port> PADInit: controller 1 = %s (driver: Xbox-One-IOUSBLib)\n", pad_xone_name());
        } else if (pad_sdl_present()) {
            port_log("port> PADInit: controller 1 = %s (driver: SDL joystick)\n", pad_sdl_name());
        } else {
            port_log("port> PADInit: no pad found; controller 1 = keyboard\n");
        }
        {
            int rumble = use_xone ? 1 : pad_sdl_rumble_supported();
            port_log("port> PADInit: rumble %savailable\n", rumble ? "" : "not ");
        }
    }
    port_log("port> PADInit: controllers 2-4 unplugged (reference config: CPU players)\n");

    pad_play_init(port_opt.pad_play, port_opt.pad_record);

    memset(motor_cmd, 0, sizeof(motor_cmd));

    /* Every exit path (the game's own return, port_shutdown(), a panic) goes
     * through exit(), so an atexit hook -- registered here rather than added
     * to main.c's own shutdown sequence -- is enough to close the pad and
     * flush a --record file without touching a file this lane does not own. */
    atexit(port_pad_shutdown);
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    u32 chan_bits = 0;
    int i;
    extern u32 VIGetRetraceCount(void);
    u32 frame = VIGetRetraceCount();

    memset(status, 0, sizeof(PADStatus) * PAD_CHANMAX);
    for (i = 0; i < PAD_CHANMAX; i++) {
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }

    if (!port_opt.nopad) {
        PortPadRaw raw;
        memset(&raw, 0, sizeof(raw));
        if (use_xone) {
            pad_xone_poll(&raw);
        } else {
            pad_sdl_poll(&raw); /* keyboard, OR'd under an SDL pad if one is open */
        }
        pad_play_step(frame, &raw); /* a --play script overwrites raw for its frames */
        if (port_pad_debug && (raw.button || raw.stickX || raw.stickY)) {
            extern u8 HuPadDStk[4];
            extern u8 HuPadDStkRep[4];
            port_log("pad> frame %u: btn %04x stick %d,%d  (last frame's dstk %02x "
                     "rep %02x)\n",
                     (unsigned)frame, (unsigned)raw.button, (int)raw.stickX,
                     (int)raw.stickY, (unsigned)HuPadDStk[0],
                     (unsigned)HuPadDStkRep[0]);
        }

        status[0].button = raw.button;
        status[0].stickX = raw.stickX;
        status[0].stickY = raw.stickY;
        status[0].substickX = raw.substickX;
        status[0].substickY = raw.substickY;
        status[0].triggerL = raw.triggerL;
        status[0].triggerR = raw.triggerR;
        status[0].analogA = status[0].analogB = 0;
        status[0].err = PAD_ERR_NONE;
        chan_bits |= PAD_CHAN0_BIT;
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
    if (chan == PAD_CHAN0) {
        int on = (cmd == PAD_MOTOR_RUMBLE);
        if (use_xone) {
            pad_xone_rumble(on);
        } else {
            pad_sdl_rumble(on);
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
    if (use_xone) {
        pad_xone_close();
    } else {
        pad_sdl_shutdown();
    }
}
