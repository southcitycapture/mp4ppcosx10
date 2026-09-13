/* The one place the development host cannot follow the G4: the game's own
 * file formats.
 *
 * PLAN.md §9.6 predicted endianness would stop the host build at the first
 * parse, and it does -- but the measurement at M2 found a second, larger
 * reason, and it is worth stating precisely because it decides how the rest
 * of the project is tested.
 *
 * The game's on-disc structures contain **32-bit fields that the game's own
 * headers declare as pointers**.  `ANIMDATA` is the smallest example:
 *
 *     typedef struct AnimData_s {
 *         s16 bankNum, patNum, bmpNum, useNum;
 *         ANIMBANK *bank;      // a 4-byte file offset, fixed up in place
 *         ANIMPAT  *pat;
 *         ANIMBMP  *bmp;
 *     } ANIMDATA;              // sizeof 0x14
 *
 * and `HuSprAnimRead` relocates them with `bank = (ANIMBANK *)((u32)anim->bank
 * + (u32)data)`.  On the GameCube and on the G4 that struct is 0x14 bytes and
 * the arithmetic is exact.  On a 64-bit host each pointer field is eight bytes,
 * so the struct is 0x20, every field after `bankNum` lands on the wrong bytes,
 * and no amount of byte swapping fixes it -- the layout is wrong before
 * endianness is even considered.  HSF models, the animation banks and the
 * message data all have the same shape.
 *
 * partyboard solves this with 1,087 lines of shadow "32b" struct definitions
 * (`AnimData32b`, `AnimBmpData32b`, `HsfCluster32b`, ...) that convert file
 * layout to host layout on read.  That work buys this port nothing: the G4 is
 * 32-bit **and** big-endian, so on the real target every one of these parses
 * is correct as written, and writing the shadow layer would be a thousand
 * lines that only ever run on the development Mac.
 *
 * So the decision PLAN.md §9.6 left open is taken here, with evidence: **the
 * host build is a plumbing harness, not a second reference implementation.**
 * It boots, it loads all 99 REL bundles, it runs the frame loop, it exercises
 * the GX state machine and the vertex and texture decoders on synthetic and
 * DOL-generated geometry -- and anything downstream of a disc-data parse is
 * skipped here and correct on the G4.  Every skip goes through this file so
 * the list is one grep long, and the "host vs G4" table in PLAN.md is kept
 * from it.
 */
#include "port.h"

#include <string.h>

#include <dolphin/types.h>
#include <game/animdata.h>

/* Both conditions have to hold for the game's own parsers to be right. */
static int data_usable(void) {
    const unsigned long one = 1;
    int little_endian = *(const unsigned char*)&one != 0;
    int wide_pointers = (int)(sizeof(void*) != 4);
    return !little_endian && !wide_pointers;
}

int portHostDataUsable(void) { return data_usable(); }

/* A big-endian u32 read, for the two or three places where a byteswap-on-read
 * shim is both cheap and unambiguously correct and buys the host build a real
 * step forward.  On the G4 this is the identity and the compiler deletes it.
 *
 * There is exactly one such place so far and it is the archive directory:
 * `GetFileInfo` in src/game/data.c reads three u32s -- a file's offset inside
 * its `data/*.bin` archive, its raw length and its compression type -- and
 * every data file in the game comes through it.  Those three fields are plain
 * scalars, not pointers, so unlike ANIMDATA there is nothing structural in the
 * way: swapping them is correct and unlocks the whole decode path
 * (`HuDecodeSlide` and friends already read their own headers a byte at a
 * time, so they are endian-clean as written). */
u32 portBE32(u32 v) {
    u32 r;
    if (data_usable()) {
        return v;
    }
    r = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) |
        ((v >> 24) & 0xFFu);
    if (port_opt.verbose) {
        port_log("port> be32 %08x -> %08x\n", v, r);
    }
    return r;
}

static int announced;

/* A shared, permanently empty animation bank, built out of the game's own
 * types so it is self-consistent from any direction the engine walks it:
 * `HuSprAnimRead`'s relocation, `HuSprCall`'s frame lookup, `HuSprFinish`'s
 * animation step and `HuSprDisp`'s layer loop all find a valid object with
 * zero layers, so every sprite on the host draws nothing rather than
 * dereferencing a file offset as a pointer.  `useNum` starts high so no
 * reference count ever reaches zero and tries to free a static. */
void* portHostEmptyAnim(void) {
    static ANIMLAYER layer;
    static ANIMPAT pat = { 0 /* layerNum */, 0, 0, 0, 0, &layer };
    static ANIMFRAME frame = { 0 /* pat */, 30000 /* time */, 0, 0, 0, 0 };
    static ANIMBANK bank = { 1 /* timeNum */, 0, &frame };
    static ANIMBMP bmp = { 0, 0, 0, 1, 1, 0, NULL, NULL };
    static ANIMDATA empty;
    if (!announced) {
        port_log("port> host: the game's sprite banks are 32-bit big-endian file "
                 "images and this host is neither; drawing them empty.  See "
                 "port/src/dvd/host_data.c and the host-vs-G4 table in PLAN.md\n");
        announced = 1;
    }
    empty.bankNum = 1;
    empty.patNum = 1;
    empty.bmpNum = 1;
    empty.useNum = 30000;
    empty.bank = &bank;
    empty.pat = &pat;
    empty.bmp = &bmp;
    return &empty;
}
