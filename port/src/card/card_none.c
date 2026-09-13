/* CARD: "no card in either slot".
 *
 * 23 symbols, 37 call sites, all in the DOL.  src/game/card.c probes both slots
 * at boot and the port answers CARD_RESULT_NOCARD, which is a state the game
 * already handles (it is what a console with empty slots reports) and keeps M1
 * away from a save-file format it has not written yet.  Two host files under
 * ~/Library/Application Support/MarioParty4/ replace this at M3.
 */
#include "port.h"

#include <dolphin/types.h>
#include <dolphin/card.h>

void CARDInit(void) { port_log("port> CARDInit: no card in either slot\n"); }

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    (void)chan;
    if (memSize) {
        *memSize = 0;
    }
    if (sectorSize) {
        *sectorSize = 0;
    }
    return CARD_RESULT_NOCARD;
}

s32 CARDMount(s32 chan, void* workArea, CARDCallback detachCallback) {
    (void)chan;
    (void)workArea;
    (void)detachCallback;
    return CARD_RESULT_NOCARD;
}

s32 CARDUnmount(s32 chan) {
    (void)chan;
    return CARD_RESULT_NOCARD;
}

s32 CARDCheck(s32 chan) {
    (void)chan;
    return CARD_RESULT_NOCARD;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    (void)chan;
    if (byteNotUsed) {
        *byteNotUsed = 0;
    }
    if (filesNotUsed) {
        *filesNotUsed = 0;
    }
    return CARD_RESULT_NOCARD;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    (void)chan;
    if (size) {
        *size = 0x2000;
    }
    return CARD_RESULT_NOCARD;
}

s32 CARDGetSerialNo(s32 chan, u64* serialNo) {
    (void)chan;
    if (serialNo) {
        *serialNo = 0;
    }
    return CARD_RESULT_NOCARD;
}
