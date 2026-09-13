/* PAD: "no controller".
 *
 * The smallest console interface in the whole port -- 9 symbols, 18 call sites
 * -- and M1 only needs it to be honest: `PADRead` must fill all four
 * `PADStatus` slots and mark them PAD_ERR_NO_CONTROLLER, because src/game/pad.c
 * reads the array unconditionally and a generated stub would leave it whatever
 * the stack happened to hold.  Real input (SDL2 plus the IOKit Xbox One driver
 * the Snowboard Kids ports already have) arrives with M3.
 */
#include "port.h"

#include <string.h>

#include <dolphin/types.h>
#include <dolphin/pad.h>

u32 __PADFixBits;

BOOL PADInit(void) {
    port_log("port> PADInit: no controllers (M3 brings SDL2 + the Xbox One driver)\n");
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    int i;
    memset(status, 0, sizeof(PADStatus) * PAD_CHANMAX);
    for (i = 0; i < PAD_CHANMAX; i++) {
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0; /* no channel bits set */
}

BOOL PADReset(u32 mask) {
    (void)mask;
    return TRUE;
}
BOOL PADRecalibrate(u32 mask) {
    (void)mask;
    return TRUE;
}
void PADClamp(PADStatus* status) { (void)status; }
void PADClampCircle(PADStatus* status) { (void)status; }
void PADControlMotor(s32 chan, u32 cmd) {
    (void)chan;
    (void)cmd;
}
void PADSetSpec(u32 spec) { (void)spec; }
void PADSetAnalogMode(u32 mode) { (void)mode; }
void PADControlAllMotors(const u32* cmdArr) { (void)cmdArr; }
PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback cb) {
    (void)cb;
    return NULL;
}

u32 SISetSamplingRate(u32 msec) {
    (void)msec;
    return 0;
}
