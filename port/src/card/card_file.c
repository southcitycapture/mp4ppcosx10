/* CARD: one real memory card, in one host file.
 *
 * `port/ref/notes.md` §5 is blunt about why this exists: **a card is mandatory
 * to get past the title.**  With both slots empty the reference rig reaches
 * SELECT A FILE, prints "No valid Memory Card is inserted.", and stops there
 * for as long as you care to hold A.  So M3 cannot finish without one, and
 * `card_none.c`'s honest "no card in either slot" -- correct for M1, and a
 * state the game does handle -- is a dead end in practice.
 *
 * **The image is the console's own format, not a convenience format.**  It
 * would have been quicker to keep one host file per save file and answer the
 * API over a directory, and it would have been wrong in a way that costs
 * later: the file the port writes is a 512 KB "Memory Card 59" image, the same
 * thing Dolphin makes, so a save can be carried between the emulator and the
 * G4 in either direction.  For a port whose whole test method is comparison
 * against a Dolphin reference rig, that is worth an afternoon.
 *
 *   block 0        the CARDID header
 *   blocks 1, 2    the directory and its backup: 127 x 64-byte CARDDir
 *                  entries, then a CARDDirCheck
 *   blocks 3, 4    the block-allocation table and its backup
 *   blocks 5..63   59 data blocks -- which is where the "59" in the card's
 *                  name comes from, and the number the game shows the player
 *
 * All of it is big-endian, because that is what the console wrote; on the G4
 * that is the machine's own order and the accessors below cost nothing, and on
 * the little-endian development host they are what keeps the image portable.
 * The layout, the checksum rule and the reserved BAT slots are not guessed:
 * they are read off the decompilation's own `src/dolphin/card/` -- CARDFormat.c
 * lays out the five system blocks, CARDCheck.c gives `__CARDCheckSum` and the
 * two ranges it covers, CARDPriv.h names the BAT's five header slots, and
 * CARDStat.c gives the banner/icon offset arithmetic reproduced in
 * `fill_stat`.  No emulator source was consulted for any of it.
 *
 * The game's own save format is untouched.  `src/game/saveload.c` writes a
 * `SaveBufData` plus a two-byte checksum and the port never looks inside it.
 *
 * Slot A holds the card; slot B is empty, which is the reference rig's
 * configuration (`Dolphin.SlotA = MemoryCard`, `SlotB` unset) and therefore
 * what the menu walk was captured against.  `--nocard` empties both, which is
 * how a scripted run reproduces the "no valid Memory Card" branch on purpose.
 */
#include "port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <dolphin/types.h>
#include <dolphin/card.h>
/* Only the handful of layout constants are wanted from CARDPriv.h, and that
 * header drags in DSP, OS-thread, alarm and DVD types the port has no reason
 * to pull into a file that talks to a host FILE*.  They are repeated here
 * with the header named, which is also a place to say where the format came
 * from -- the decompilation's own SDK sources, not an emulator's. */
#define CARD_FAT_AVAIL 0x0000u
#define CARD_FAT_CHECKSUM 0x0000u
#define CARD_FAT_CHECKSUMINV 0x0001u
#define CARD_FAT_CHECKCODE 0x0002u
#define CARD_FAT_FREEBLOCKS 0x0003u
#define CARD_FAT_LASTSLOT 0x0004u
#define CARD_NUM_SYSTEM_BLOCK 5

#define CARD_BLOCK_SIZE 8192
#define CARD_BLOCKS 64                     /* 512 KB */
#define CARD_DATA_BLOCK0 CARD_NUM_SYSTEM_BLOCK /* 5 */
#define CARD_FREE_BLOCKS (CARD_BLOCKS - CARD_NUM_SYSTEM_BLOCK) /* 59 */
#define CARD_IMAGE_SIZE ((size_t)CARD_BLOCKS * CARD_BLOCK_SIZE)
#define CARD_SIZE_MBIT 4
#define CARD_SLOTS 2

/* The disc this port plays.  CARDOpen matches a directory entry on the game
 * and company code as well as the name, exactly as the console does, so a card
 * carrying another game's files is not confused for this one. */
static const char CARD_GAME_NAME[4] = { 'G', 'M', 'P', 'E' };
static const char CARD_COMPANY[2] = { '0', '1' };

typedef struct Slot {
    int present;
    int mounted;
    u8* img;
    char path[1024];
    int dirty;
} Slot;

static Slot slot[CARD_SLOTS];
static unsigned stat_reads, stat_writes, stat_creates, stat_deletes, stat_flushes;

/* ---- big-endian accessors -------------------------------------------------- */

static u16 rd16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void wr16(u8* p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
static void wr32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* ---- the on-image structures ----------------------------------------------- */
/* A directory entry is 64 bytes with this layout (CARDPriv.h's CARDDir):
 *   0x00 gameName[4]  0x04 company[2]  0x06 pad  0x07 bannerFormat
 *   0x08 fileName[32] 0x28 time        0x2C iconAddr
 *   0x30 iconFormat   0x32 iconSpeed   0x34 permission  0x35 copyTimes
 *   0x36 startBlock   0x38 length      0x3A pad[2]      0x3C commentAddr  */
#define DIRENT_SIZE 64
#define DIR_ENTRIES CARD_MAX_FILE /* 127; the 128th slot holds the CARDDirCheck */

static u8* dir_block(Slot* s) { return s->img + 1 * CARD_BLOCK_SIZE; }
static u8* bat_block(Slot* s) { return s->img + 3 * CARD_BLOCK_SIZE; }
static u8* dirent(Slot* s, int n) { return dir_block(s) + (size_t)n * DIRENT_SIZE; }

static int dirent_used(const u8* e) {
    /* A free entry is 0xFF filled; the console tests the game name's first
     * byte, and so does everything that reads these images. */
    return e[0] != 0xFF;
}

/* __CARDCheckSum, from src/dolphin/card/CARDCheck.c, over big-endian halfwords.
 * The sum of the halfwords and the sum of their complements, with 0xFFFF
 * folded to zero in both -- that fold is not decoration, it is what stops a
 * legitimately-checksummed block from looking like erased flash. */
static void card_checksum(const u8* p, int length, u16* sum, u16* suminv) {
    int i;
    u16 a = 0, b = 0;
    for (i = 0; i < length; i += 2) {
        u16 v = rd16(p + i);
        a = (u16)(a + v);
        b = (u16)(b + (u16)~v);
    }
    if (a == 0xFFFF) {
        a = 0;
    }
    if (b == 0xFFFF) {
        b = 0;
    }
    *sum = a;
    *suminv = b;
}

static void reseal_dir(Slot* s) {
    int i;
    u16 sum, suminv;
    for (i = 0; i < 2; i++) {
        u8* d = s->img + (size_t)(1 + i) * CARD_BLOCK_SIZE;
        if (i == 1) {
            memcpy(d, dir_block(s), CARD_BLOCK_SIZE);
        }
        /* the CARDDirCheck lives in the last 64 bytes: checkCode at +0x3A,
         * then the two checksums */
        wr16(d + CARD_BLOCK_SIZE - 6, (u16)i);
        card_checksum(d, CARD_BLOCK_SIZE - 4, &sum, &suminv);
        wr16(d + CARD_BLOCK_SIZE - 4, sum);
        wr16(d + CARD_BLOCK_SIZE - 2, suminv);
    }
}

static void reseal_bat(Slot* s) {
    int i;
    u16 sum, suminv;
    for (i = 0; i < 2; i++) {
        u8* f = s->img + (size_t)(3 + i) * CARD_BLOCK_SIZE;
        if (i == 1) {
            memcpy(f, bat_block(s), CARD_BLOCK_SIZE);
        }
        wr16(f + 2 * CARD_FAT_CHECKCODE, (u16)i);
        card_checksum(f + 2 * CARD_FAT_CHECKCODE, CARD_BLOCK_SIZE - 4, &sum, &suminv);
        wr16(f + 2 * CARD_FAT_CHECKSUM, sum);
        wr16(f + 2 * CARD_FAT_CHECKSUMINV, suminv);
    }
}

static void reseal(Slot* s) {
    reseal_dir(s);
    reseal_bat(s);
    s->dirty = 1;
}

/* ---- the image on disc ------------------------------------------------------ */

static void card_dir_make(char* out, size_t n) {
    const char* home = getenv("HOME");
    char buf[700];
    snprintf(buf, sizeof(buf), "%s/Library/Application Support",
             home && *home ? home : ".");
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/Library/Application Support/MarioParty4",
             home && *home ? home : ".");
    mkdir(buf, 0755);
    snprintf(out, n, "%s", buf);
}

static void card_format_image(Slot* s) {
    u8* id = s->img;
    u8* bat;
    u16 sum, suminv;
    int i;

    memset(s->img, 0xFF, CARD_IMAGE_SIZE);

    /* Block 0, the CARDID.  The console's serial is the flash id scrambled
     * against a timebase; nothing here verifies it and neither does anything
     * that reads these images, so what matters is that it is stable across
     * runs -- a card whose serial changed every boot would make the game think
     * it had been swapped. */
    memset(id, 0xFF, CARD_BLOCK_SIZE);
    for (i = 0; i < 32; i++) {
        id[i] = (u8)(0x4D + i * 7); /* stable, and not all one value */
    }
    wr16(id + 0x20, 0);                /* deviceID: 0 == a normal card       */
    wr16(id + 0x22, CARD_SIZE_MBIT);   /* size, in Mbit                      */
    wr16(id + 0x24, CARD_ENCODE_ANSI); /* the font encoding the card carries */
    memset(id + 0x26, 0, 512 - 0x26);
    card_checksum(id, 512 - 4, &sum, &suminv);
    wr16(id + 512 - 4, sum);
    wr16(id + 512 - 2, suminv);

    /* Blocks 1 and 2: an empty directory, 0xFF filled.  Blocks 3 and 4: an
     * empty allocation table -- zeroed, then the five header slots
     * CARDPriv.h names, of which only free-block count and last-allocated
     * matter to an empty card. */
    memset(s->img + 1 * CARD_BLOCK_SIZE, 0xFF, 2 * CARD_BLOCK_SIZE);
    memset(s->img + 3 * CARD_BLOCK_SIZE, 0x00, 2 * CARD_BLOCK_SIZE);
    bat = bat_block(s);
    wr16(bat + 2 * CARD_FAT_FREEBLOCKS, CARD_FREE_BLOCKS);
    wr16(bat + 2 * CARD_FAT_LASTSLOT, CARD_NUM_SYSTEM_BLOCK - 1);

    memset(s->img + (size_t)CARD_DATA_BLOCK0 * CARD_BLOCK_SIZE, 0xFF,
           CARD_IMAGE_SIZE - (size_t)CARD_DATA_BLOCK0 * CARD_BLOCK_SIZE);
    reseal(s);
}

static void card_flush(Slot* s) {
    FILE* f;
    if (!s->present || !s->dirty) {
        return;
    }
    f = fopen(s->path, "wb");
    if (!f) {
        port_log("port> CARD: cannot write %s (%s); this session's saves are "
                 "in memory only\n",
                 s->path, strerror(errno));
        s->dirty = 0;
        return;
    }
    fwrite(s->img, 1, CARD_IMAGE_SIZE, f);
    fclose(f);
    s->dirty = 0;
    stat_flushes++;
}

static void card_load(int chan) {
    Slot* s = &slot[chan];
    char dir[768];
    FILE* f;
    struct stat st;

    if (port_opt.nocard || chan != 0) {
        /* Slot B stays empty on purpose: the reference rig has one card, and
         * the game's file-select screen probes both and complains about the
         * one it cannot find, which is a difference we want to reproduce
         * rather than paper over. */
        return;
    }
    card_dir_make(dir, sizeof(dir));
    snprintf(s->path, sizeof(s->path), "%s/memcard-slot-%c.raw", dir, 'a' + chan);
    s->img = (u8*)malloc(CARD_IMAGE_SIZE);
    if (!s->img) {
        port_log("port> CARD: out of memory for a 512 KB card image\n");
        return;
    }
    f = fopen(s->path, "rb");
    if (f && stat(s->path, &st) == 0 && (size_t)st.st_size == CARD_IMAGE_SIZE &&
        fread(s->img, 1, CARD_IMAGE_SIZE, f) == CARD_IMAGE_SIZE) {
        fclose(f);
        port_log("port> CARD: slot %c = %s (existing 512 KB image)\n", 'A' + chan,
                 s->path);
    } else {
        if (f) {
            fclose(f);
            port_log("port> CARD: %s is not a 512 KB card image; reformatting\n",
                     s->path);
        }
        card_format_image(s);
        port_log("port> CARD: slot %c = %s (new, formatted, %d free blocks)\n",
                 'A' + chan, s->path, CARD_FREE_BLOCKS);
    }
    s->present = 1;
    card_flush(s);
}

/* ---- the allocation table --------------------------------------------------- */

static u16 bat_get(Slot* s, int block) { return rd16(bat_block(s) + 2 * block); }
static void bat_set(Slot* s, int block, u16 v) { wr16(bat_block(s) + 2 * block, v); }
static u16 bat_free(Slot* s) { return rd16(bat_block(s) + 2 * CARD_FAT_FREEBLOCKS); }
static void bat_set_free(Slot* s, u16 v) {
    wr16(bat_block(s) + 2 * CARD_FAT_FREEBLOCKS, v);
}

/* Walk a file's block chain to its `n`th block.  The chain terminator is
 * 0xFFFF and a free block is 0. */
static int chain_nth(Slot* s, int start, int n) {
    int b = start;
    while (n-- > 0) {
        if (b < CARD_DATA_BLOCK0 || b >= CARD_BLOCKS) {
            return -1;
        }
        b = bat_get(s, b);
        if (b == 0xFFFF) {
            return -1;
        }
    }
    return (b >= CARD_DATA_BLOCK0 && b < CARD_BLOCKS) ? b : -1;
}

static int alloc_chain(Slot* s, int count, int* first_out) {
    int got = 0, prev = -1, first = -1, b;
    if (bat_free(s) < count) {
        return 0;
    }
    for (b = CARD_DATA_BLOCK0; b < CARD_BLOCKS && got < count; b++) {
        if (bat_get(s, b) != CARD_FAT_AVAIL) {
            continue;
        }
        bat_set(s, b, 0xFFFF);
        if (prev >= 0) {
            bat_set(s, prev, (u16)b);
        } else {
            first = b;
        }
        prev = b;
        got++;
    }
    if (got < count) {
        return 0; /* cannot happen while bat_free is honest, but be safe */
    }
    bat_set_free(s, (u16)(bat_free(s) - count));
    wr16(bat_block(s) + 2 * CARD_FAT_LASTSLOT, (u16)prev);
    *first_out = first;
    return 1;
}

static void free_chain(Slot* s, int start) {
    int b = start, n = 0;
    while (b >= CARD_DATA_BLOCK0 && b < CARD_BLOCKS) {
        int next = bat_get(s, b);
        bat_set(s, b, CARD_FAT_AVAIL);
        n++;
        if (next == 0xFFFF) {
            break;
        }
        b = next;
    }
    bat_set_free(s, (u16)(bat_free(s) + n));
}

/* ---- helpers ---------------------------------------------------------------- */

static Slot* slot_ready(s32 chan) {
    if (chan < 0 || chan >= CARD_SLOTS) {
        return NULL;
    }
    return slot[chan].present ? &slot[chan] : NULL;
}

static int find_file(Slot* s, const char* name) {
    int i;
    for (i = 0; i < DIR_ENTRIES; i++) {
        const u8* e = dirent(s, i);
        if (!dirent_used(e)) {
            continue;
        }
        if (memcmp(e, CARD_GAME_NAME, 4) != 0 || memcmp(e + 4, CARD_COMPANY, 2) != 0) {
            continue;
        }
        if (strncmp((const char*)e + 8, name, CARD_FILENAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

/* Seconds since 2000-01-01, which is what the console's directory stores. */
static u32 card_time_now(void) {
    time_t t = time(NULL);
    long long secs = (long long)t - 946684800LL;
    return secs > 0 ? (u32)secs : 0u;
}

/* ---- the API ---------------------------------------------------------------- */

void CARDInit(void) {
    int i;
    for (i = 0; i < CARD_SLOTS; i++) {
        card_load(i);
    }
    if (port_opt.nocard) {
        port_log("port> CARDInit: --nocard, both slots empty (the game stops at "
                 "SELECT A FILE, which is the console's behaviour too)\n");
    }
}

void port_card_report(void) {
    int i;
    for (i = 0; i < CARD_SLOTS; i++) {
        if (slot[i].present) {
            card_flush(&slot[i]);
        }
    }
    if (stat_reads || stat_writes || stat_creates || stat_deletes) {
        port_log("port> CARD: %u reads, %u writes, %u files created, %u deleted, "
                 "%u image flushes, %u of %d blocks free\n",
                 stat_reads, stat_writes, stat_creates, stat_deletes, stat_flushes,
                 slot[0].present ? bat_free(&slot[0]) : 0, CARD_FREE_BLOCKS);
    }
}

BOOL CARDProbe(s32 chan) { return slot_ready(chan) ? TRUE : FALSE; }

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    Slot* s = slot_ready(chan);
    if (!s) {
        if (memSize) {
            *memSize = 0;
        }
        if (sectorSize) {
            *sectorSize = 0;
        }
        return CARD_RESULT_NOCARD;
    }
    if (memSize) {
        *memSize = CARD_SIZE_MBIT;
    }
    if (sectorSize) {
        /* 8192, and it has to be: modeseldll/filesel.c prints "wrong device"
         * for anything else (`result > 0 && result != 8192`), and so does
         * saveload.c in four places. */
        *sectorSize = CARD_BLOCK_SIZE;
    }
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, void* workArea, CARDCallback detachCallback) {
    Slot* s = slot_ready(chan);
    (void)workArea;
    (void)detachCallback; /* it is a *detach* callback: nothing detaches here */
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    s->mounted = 1;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback,
                   CARDCallback attachCallback) {
    s32 r = CARDMount(chan, workArea, detachCallback);
    if (attachCallback) {
        attachCallback(chan, r);
    }
    return r;
}

s32 CARDUnmount(s32 chan) {
    Slot* s = slot_ready(chan);
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    card_flush(s);
    s->mounted = 0;
    return CARD_RESULT_READY;
}

s32 CARDCheck(s32 chan) { return slot_ready(chan) ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }

s32 CARDCheckEx(s32 chan, s32* xferBytes) {
    if (xferBytes) {
        *xferBytes = 0;
    }
    return CARDCheck(chan);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    Slot* s = slot_ready(chan);
    int i, files = 0;
    if (!s) {
        if (byteNotUsed) {
            *byteNotUsed = 0;
        }
        if (filesNotUsed) {
            *filesNotUsed = 0;
        }
        return CARD_RESULT_NOCARD;
    }
    for (i = 0; i < DIR_ENTRIES; i++) {
        if (!dirent_used(dirent(s, i))) {
            files++;
        }
    }
    if (byteNotUsed) {
        *byteNotUsed = (s32)bat_free(s) * CARD_BLOCK_SIZE;
    }
    if (filesNotUsed) {
        *filesNotUsed = files;
    }
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    if (size) {
        *size = CARD_BLOCK_SIZE;
    }
    return slot_ready(chan) ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

s32 CARDGetMemSize(s32 chan, u16* size) {
    if (size) {
        *size = CARD_SIZE_MBIT;
    }
    return slot_ready(chan) ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

s32 CARDGetSerialNo(s32 chan, u64* serialNo) {
    Slot* s = slot_ready(chan);
    if (!s) {
        if (serialNo) {
            *serialNo = 0;
        }
        return CARD_RESULT_NOCARD;
    }
    if (serialNo) {
        /* The console folds the 32-byte serial into a u64 by xoring its eight
         * words together; saveload.c only ever compares one reading with
         * another, so what matters is that it is the same every boot. */
        u64 acc = 0;
        int i;
        for (i = 0; i < 32; i += 8) {
            u64 w = ((u64)rd32(s->img + i) << 32) | rd32(s->img + i + 4);
            acc ^= w;
        }
        *serialNo = acc;
    }
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo) {
    Slot* s = slot_ready(chan);
    int n;
    const u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    n = find_file(s, fileName);
    if (n < 0) {
        return CARD_RESULT_NOFILE;
    }
    e = dirent(s, n);
    fileInfo->chan = chan;
    fileInfo->fileNo = n;
    fileInfo->offset = -1;
    fileInfo->length = (s32)rd16(e + 0x38) * CARD_BLOCK_SIZE;
    fileInfo->iBlock = rd16(e + 0x36);
    return CARD_RESULT_READY;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo) {
    Slot* s = slot_ready(chan);
    const u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES || !dirent_used(dirent(s, fileNo))) {
        return CARD_RESULT_NOFILE;
    }
    e = dirent(s, fileNo);
    fileInfo->chan = chan;
    fileInfo->fileNo = fileNo;
    fileInfo->offset = -1;
    fileInfo->length = (s32)rd16(e + 0x38) * CARD_BLOCK_SIZE;
    fileInfo->iBlock = rd16(e + 0x36);
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo) {
    Slot* s;
    if (!fileInfo) {
        return CARD_RESULT_FATAL_ERROR;
    }
    s = slot_ready(fileInfo->chan);
    fileInfo->fileNo = -1;
    if (s) {
        card_flush(s);
    }
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo) {
    Slot* s = slot_ready(chan);
    int n, i, first = -1, blocks;
    u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if (size == 0 || (size % CARD_BLOCK_SIZE) != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (find_file(s, fileName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    blocks = (int)(size / CARD_BLOCK_SIZE);
    n = -1;
    for (i = 0; i < DIR_ENTRIES; i++) {
        if (!dirent_used(dirent(s, i))) {
            n = i;
            break;
        }
    }
    if (n < 0) {
        return CARD_RESULT_NOENT;
    }
    if (!alloc_chain(s, blocks, &first)) {
        return CARD_RESULT_INSSPACE;
    }
    e = dirent(s, n);
    memset(e, 0, DIRENT_SIZE);
    memcpy(e, CARD_GAME_NAME, 4);
    memcpy(e + 4, CARD_COMPANY, 2);
    e[6] = 0xFF;
    e[7] = 0; /* bannerFormat, filled in later by CARDSetStatus */
    memset(e + 8, 0, CARD_FILENAME_MAX);
    strncpy((char*)e + 8, fileName, CARD_FILENAME_MAX);
    wr32(e + 0x28, card_time_now());
    wr32(e + 0x2C, 0xFFFFFFFFu); /* iconAddr: none yet    */
    wr16(e + 0x30, 0);           /* iconFormat            */
    wr16(e + 0x32, 0);           /* iconSpeed             */
    e[0x34] = CARD_ATTR_PUBLIC;  /* permission            */
    e[0x35] = 0;                 /* copyTimes             */
    wr16(e + 0x36, (u16)first);
    wr16(e + 0x38, (u16)blocks);
    wr32(e + 0x3C, 0xFFFFFFFFu); /* commentAddr           */
    /* A new file's data blocks are zeroed rather than left at the erased
     * 0xFF: the game writes its whole SaveBufData immediately, but a torn
     * first write should read back as an empty save, not as a checksum made
     * of erased flash. */
    for (i = 0; i < blocks; i++) {
        int b = chain_nth(s, first, i);
        if (b > 0) {
            memset(s->img + (size_t)b * CARD_BLOCK_SIZE, 0, CARD_BLOCK_SIZE);
        }
    }
    reseal(s);
    card_flush(s);
    stat_creates++;
    fileInfo->chan = chan;
    fileInfo->fileNo = n;
    fileInfo->offset = -1;
    fileInfo->length = (s32)size;
    fileInfo->iBlock = (u16)first;
    port_log("port> CARD: created \"%s\", %u bytes (%d blocks)\n", fileName,
             (unsigned)size, blocks);
    return CARD_RESULT_READY;
}

/* Read and write walk the block chain rather than assuming the file is
 * contiguous, because a card that has had files deleted off it will not hand
 * out contiguous blocks and the game does delete files (saveload.c's
 * SLFileDelete, and the file-select screen's Erase). */
static s32 card_rw(CARDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                   int writing) {
    Slot* s;
    u8* p = (u8*)addr;
    s32 done = 0;
    if (!fileInfo) {
        return CARD_RESULT_FATAL_ERROR;
    }
    s = slot_ready(fileInfo->chan);
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (fileInfo->fileNo < 0 || fileInfo->fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_NOFILE;
    }
    if (length < 0 || offset < 0 || offset + length > fileInfo->length) {
        return CARD_RESULT_LIMIT;
    }
    while (done < length) {
        s32 at = offset + done;
        int bi = (int)(at / CARD_BLOCK_SIZE);
        int within = (int)(at % CARD_BLOCK_SIZE);
        s32 n = CARD_BLOCK_SIZE - within;
        int b = chain_nth(s, fileInfo->iBlock, bi);
        if (b < 0) {
            return CARD_RESULT_LIMIT;
        }
        if (n > length - done) {
            n = length - done;
        }
        if (writing) {
            memcpy(s->img + (size_t)b * CARD_BLOCK_SIZE + within, p + done, (size_t)n);
        } else {
            memcpy(p + done, s->img + (size_t)b * CARD_BLOCK_SIZE + within, (size_t)n);
        }
        done += n;
    }
    if (writing) {
        s->dirty = 1;
        card_flush(s); /* a save the player was shown must survive the power */
        stat_writes++;
    } else {
        stat_reads++;
    }
    fileInfo->offset = offset + length;
    return CARD_RESULT_READY;
}

s32 CARDRead(CARDFileInfo* fileInfo, void* addr, s32 length, s32 offset) {
    return card_rw(fileInfo, addr, length, offset, 0);
}

s32 CARDWrite(CARDFileInfo* fileInfo, const void* addr, s32 length, s32 offset) {
    return card_rw(fileInfo, (void*)addr, length, offset, 1);
}

s32 CARDDelete(s32 chan, const char* fileName) {
    Slot* s = slot_ready(chan);
    int n;
    u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    n = find_file(s, fileName);
    if (n < 0) {
        return CARD_RESULT_NOFILE;
    }
    e = dirent(s, n);
    free_chain(s, rd16(e + 0x36));
    memset(e, 0xFF, DIRENT_SIZE);
    reseal(s);
    card_flush(s);
    stat_deletes++;
    port_log("port> CARD: deleted \"%s\"\n", fileName);
    return CARD_RESULT_READY;
}

s32 CARDFastDelete(s32 chan, s32 fileNo) {
    Slot* s = slot_ready(chan);
    u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES || !dirent_used(dirent(s, fileNo))) {
        return CARD_RESULT_NOFILE;
    }
    e = dirent(s, fileNo);
    free_chain(s, rd16(e + 0x36));
    memset(e, 0xFF, DIRENT_SIZE);
    reseal(s);
    card_flush(s);
    stat_deletes++;
    return CARD_RESULT_READY;
}

/* The banner/icon offset arithmetic, straight out of CARDStat.c's
 * UpdateIconOffsets: the offsets the game reads back are derived from the
 * formats it set, not stored. */
static void fill_stat(const u8* e, CARDStat* st) {
    u32 offset = rd32(e + 0x2C);
    u8 bannerFormat = e[7];
    u16 iconFormat = rd16(e + 0x30);
    int iconTlut = 0;
    int i;

    memset(st, 0, sizeof(*st));
    memcpy(st->fileName, e + 8, CARD_FILENAME_MAX);
    st->length = (u32)rd16(e + 0x38) * CARD_BLOCK_SIZE;
    st->time = rd32(e + 0x28);
    memcpy(st->gameName, e, 4);
    memcpy(st->company, e + 4, 2);
    st->bannerFormat = bannerFormat;
    st->iconAddr = rd32(e + 0x2C);
    st->iconFormat = iconFormat;
    st->iconSpeed = rd16(e + 0x32);
    st->commentAddr = rd32(e + 0x3C);

    if (offset == 0xFFFFFFFFu) {
        st->bannerFormat = 0;
        st->iconFormat = 0;
        st->iconSpeed = 0;
        offset = 0;
        bannerFormat = 0;
        iconFormat = 0;
    }
    switch (bannerFormat & CARD_STAT_BANNER_MASK) {
        case CARD_STAT_BANNER_C8:
            st->offsetBanner = offset;
            offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
            st->offsetBannerTlut = offset;
            offset += 2 * 256;
            break;
        case CARD_STAT_BANNER_RGB5A3:
            st->offsetBanner = offset;
            offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
            st->offsetBannerTlut = 0xFFFFFFFFu;
            break;
        default:
            st->offsetBanner = 0xFFFFFFFFu;
            st->offsetBannerTlut = 0xFFFFFFFFu;
            break;
    }
    for (i = 0; i < CARD_ICON_MAX; i++) {
        switch ((iconFormat >> (2 * i)) & CARD_STAT_ICON_MASK) {
            case CARD_STAT_ICON_C8:
                st->offsetIcon[i] = offset;
                offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
                iconTlut = 1;
                break;
            case CARD_STAT_ICON_RGB5A3:
                st->offsetIcon[i] = offset;
                offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
                break;
            default:
                st->offsetIcon[i] = 0xFFFFFFFFu;
                break;
        }
    }
    if (iconTlut) {
        st->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        st->offsetIconTlut = 0xFFFFFFFFu;
    }
    st->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    Slot* s = slot_ready(chan);
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!dirent_used(dirent(s, fileNo))) {
        return CARD_RESULT_NOFILE;
    }
    fill_stat(dirent(s, fileNo), stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    Slot* s = slot_ready(chan);
    u8* e;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_FATAL_ERROR;
    }
    e = dirent(s, fileNo);
    if (!dirent_used(e)) {
        return CARD_RESULT_NOFILE;
    }
    e[7] = stat->bannerFormat;
    wr32(e + 0x2C, stat->iconAddr);
    wr16(e + 0x30, stat->iconFormat);
    wr16(e + 0x32, stat->iconSpeed);
    wr32(e + 0x3C, stat->commentAddr);
    wr32(e + 0x28, card_time_now());
    reseal(s);
    card_flush(s);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback) {
    s32 r = CARDSetStatus(chan, fileNo, stat);
    if (callback) {
        callback(chan, r);
    }
    return r;
}

s32 CARDFormat(s32 chan) {
    Slot* s = slot_ready(chan);
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    card_format_image(s);
    card_flush(s);
    port_log("port> CARD: slot %c formatted\n", 'A' + (int)chan);
    return CARD_RESULT_READY;
}

s32 CARDRename(s32 chan, const char* oldName, const char* newName) {
    Slot* s = slot_ready(chan);
    int n;
    if (!s) {
        return CARD_RESULT_NOCARD;
    }
    n = find_file(s, oldName);
    if (n < 0) {
        return CARD_RESULT_NOFILE;
    }
    if (find_file(s, newName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    memset(dirent(s, n) + 8, 0, CARD_FILENAME_MAX);
    strncpy((char*)dirent(s, n) + 8, newName, CARD_FILENAME_MAX);
    reseal(s);
    card_flush(s);
    return CARD_RESULT_READY;
}

BOOL CARDGetFastMode(void) { return FALSE; }
BOOL CARDSetFastMode(BOOL enable) {
    (void)enable;
    return FALSE;
}
void CARDSetDiskID(const void* id) { (void)id; }
