/* DVD: a real file system, because the game panics without one.
 *
 * `HuDataInit()` resolves all 138 `data/[*].bin` names through
 * `DVDConvertPathToEntrynum` at boot and `OSPanic`s at data.c:65 on the first
 * miss, so this cannot be a stub.  It reads the disc's own FST -- either
 * straight out of the disc image (preferred: nothing to install, and the
 * NKit-trimmed ISO keeps every file at its original offset, verified against
 * `dtk vfs cp`) or out of an extracted `files/` tree, which is the fallback
 * for a user who already has one.
 *
 * Two deliberate differences from the console:
 *
 *  - Reads are synchronous and the completion callback fires before
 *    `DVDReadAsync` returns.  The game waits for a read with
 *    `while (!CallBackStatus) HuDvdErrorWatch();` and `HuDvdErrorWatch` does
 *    *not* wait for a retrace, so deferring the callback to the host loop's
 *    retrace service -- the plan's original shape -- would deadlock.
 *  - `DVDGetDriveStatus` is always DVD_STATE_END, so the game's "no disc /
 *    cover open / wrong disc" error screen never triggers.
 */
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/types.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

typedef struct Entry {
    char* path; /* "data/board.bin", no leading slash, as the FST spells it */
    u32 offset; /* byte offset into the image, or 0 for the tree backend */
    u32 length;
} Entry;

static Entry* entries;
static int entry_count;
static FILE* image;
static char tree_root[1024];
static unsigned long bytes_read;
static unsigned long reads;
static unsigned long slow_reads; /* M24: reads over 100 ms */
static double slow_read_s;       /* M24: every read's wall time */

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int ci_equal(const char* a, const char* b) {
    /* the SDK's own path compare is case insensitive, and the game relies on
     * it: ovl_table.h asks for "dll/bootdll.rel", the disc holds
     * "dll/bootDll.rel". */
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- backend 1: the disc image ------------------------------------------- */

static int load_fst_from_image(const char* path) {
    u8 hdr[16];
    u8* fst;
    u32 fst_off, fst_size, count, names, i;
    /* one open directory per nesting level, holding its name prefix and the
     * entry index at which it ends */
    struct {
        u32 end;
        char prefix[512];
    } stack[16];
    int depth = 0;

    image = fopen(path, "rb");
    if (!image) {
        return 0;
    }
    if (fseek(image, 0x420, SEEK_SET) != 0 || fread(hdr, 1, 16, image) != 16) {
        fclose(image);
        image = NULL;
        return 0;
    }
    fst_off = be32(hdr + 4);
    fst_size = be32(hdr + 8);
    if (!fst_off || !fst_size || fst_size > 0x100000) {
        fclose(image);
        image = NULL;
        return 0;
    }
    fst = malloc(fst_size);
    if (fseek(image, (long)fst_off, SEEK_SET) != 0 || fread(fst, 1, fst_size, image) != fst_size) {
        free(fst);
        fclose(image);
        image = NULL;
        return 0;
    }
    count = be32(fst + 8);
    names = count * 12;
    entries = calloc(count, sizeof(Entry));
    stack[0].end = count;
    stack[0].prefix[0] = 0;

    for (i = 1; i < count; i++) {
        const u8* e = fst + i * 12;
        u32 name_off = ((u32)e[1] << 16) | ((u32)e[2] << 8) | e[3];
        u32 a = be32(e + 4);
        u32 b = be32(e + 8);
        const char* name = (const char*)(fst + names + name_off);
        char full[512];
        while (depth > 0 && i >= stack[depth].end) {
            depth--;
        }
        snprintf(full, sizeof(full), "%s%s", stack[depth].prefix, name);
        if (e[0]) { /* directory */
            if (depth + 1 < (int)(sizeof(stack) / sizeof(stack[0]))) {
                depth++;
                stack[depth].end = b;
                snprintf(stack[depth].prefix, sizeof(stack[depth].prefix), "%s/", full);
            }
        } else {
            entries[entry_count].path = strdup(full);
            entries[entry_count].offset = a;
            entries[entry_count].length = b;
            entry_count++;
        }
    }
    free(fst);
    return 1;
}

/* ---- backend 2: an extracted files/ tree --------------------------------- */

static void walk_tree(const char* base, const char* rel);

#include <dirent.h>
#include <sys/stat.h>

static void walk_tree(const char* base, const char* rel) {
    char dirpath[1024];
    DIR* d;
    struct dirent* de;
    snprintf(dirpath, sizeof(dirpath), "%s%s%s", base, *rel ? "/" : "", rel);
    d = opendir(dirpath);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char child[1024];
        char full[1024];
        struct stat st;
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(child, sizeof(child), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        snprintf(full, sizeof(full), "%s/%s", base, child);
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk_tree(base, child);
        } else {
            entries = realloc(entries, (entry_count + 1) * sizeof(Entry));
            entries[entry_count].path = strdup(child);
            entries[entry_count].offset = 0;
            entries[entry_count].length = (u32)st.st_size;
            entry_count++;
        }
    }
    closedir(d);
}

/* ---- init ---------------------------------------------------------------- */

void port_dvd_init(void) {
    const char* p = port_opt.image;
    struct stat st;
    if (!p) {
        port_fatal("no disc image: pass --image <disc.iso> or --image <dir containing files/>");
    }
    if (stat(p, &st) != 0) {
        port_fatal("--image %s: no such file", p);
    }
    if (S_ISDIR(st.st_mode)) {
        char files[1024];
        snprintf(files, sizeof(files), "%s/files", p);
        if (stat(files, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(tree_root, sizeof(tree_root), "%s", files);
        } else {
            snprintf(tree_root, sizeof(tree_root), "%s", p);
        }
        walk_tree(tree_root, "");
        port_log("port> DVD: extracted tree %s, %d files\n", tree_root, entry_count);
    } else {
        if (!load_fst_from_image(p)) {
            port_fatal("--image %s: not a GameCube disc image (no FST at 0x424)", p);
        }
        port_log("port> DVD: disc image %s, %d files in the FST\n", p, entry_count);
    }
    if (entry_count == 0) {
        port_fatal("--image %s: no files found", p);
    }
}

void DVDInit(void) { port_log("port> DVDInit\n"); }

s32 DVDConvertPathToEntrynum(char* path) {
    int i;
    const char* p = path;
    while (*p == '/') {
        p++;
    }
    for (i = 0; i < entry_count; i++) {
        if (ci_equal(entries[i].path, p)) {
            return i;
        }
    }
    return -1;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fi) {
    if (entrynum < 0 || entrynum >= entry_count) {
        return FALSE;
    }
    memset(fi, 0, sizeof(*fi));
    fi->cb.command = (u32)entrynum;
    fi->cb.state = DVD_STATE_END;
    fi->startAddr = entries[entrynum].offset;
    fi->length = entries[entrynum].length;
    return TRUE;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fi) {
    s32 n = DVDConvertPathToEntrynum(fileName);
    if (n < 0) {
        return FALSE;
    }
    return DVDFastOpen(n, fi);
}

BOOL DVDClose(DVDFileInfo* fi) {
    (void)fi;
    return TRUE;
}

void port_wb_disarm(const void* ptr, size_t n); /* M42: gx_wb.c */

static s32 do_read(DVDFileInfo* fi, void* addr, s32 length, s32 offset) {
    int n = (int)fi->cb.command;
    size_t got;
    if (port_opt.verbose) { port_log("port> dvd read entry %d -> %p len %d ofs %d\n", n, addr, length, offset); }
    if (n < 0 || n >= entry_count) {
        return DVD_RESULT_FATAL_ERROR;
    }
    if ((u32)offset >= entries[n].length) {
        return 0;
    }
    if ((u32)(offset + length) > entries[n].length) {
        /* the game rounds every read up to 32 bytes; zero the tail */
        memset((u8*)addr + (entries[n].length - offset), 0,
               (size_t)(length - (s32)(entries[n].length - offset)));
        length = (s32)(entries[n].length - offset);
    }
    /* M42 (PLAN.md 57.5): the kernel cannot write a page the vertex cache's
     * write barrier protected (fread would fail with EFAULT), and a copy
     * would fault on every page: the destination is unprotected first */
    port_wb_disarm(addr, (size_t)length);
    /* M36 (PLAN.md 51): the resident set answers first.  Same bytes, no
     * disk; the completion below lands at the same instant either way. */
    if (port_dvd_cache_serve(n, (u32)offset, addr, (u32)length)) {
        got = (size_t)length;
    } else if (image) {
        /* M24: a read over 100 ms is named -- the 1.7 s game-side stall at
         * the results screen's last frame (PLAN.md 38.3, 39.1) is not the
         * card flush, and the disc image's reads are the other synchronous
         * thing a game frame does */
        double t0 = port_now_seconds(), dt;
        if (fseek(image, (long)(entries[n].offset + (u32)offset), SEEK_SET) != 0) {
            return DVD_RESULT_FATAL_ERROR;
        }
        got = fread(addr, 1, (size_t)length, image);
        dt = port_now_seconds() - t0;
        slow_read_s += dt;
        if (dt > 0.1) {
            unsigned gl13_frame_number(void);
            slow_reads++;
            port_log("port> DVD: read of %s (%d bytes at %d) took %.0f ms (frame %u)\n",
                     entries[n].path, length, offset, dt * 1000.0, gl13_frame_number());
        }
        port_dvd_cache_disk_read(n, (u32)offset, addr, (u32)got, dt);
    } else {
        char full[1200];
        FILE* f;
        snprintf(full, sizeof(full), "%s/%s", tree_root, entries[n].path);
        f = fopen(full, "rb");
        if (!f) {
            return DVD_RESULT_FATAL_ERROR;
        }
        fseek(f, offset, SEEK_SET);
        got = fread(addr, 1, (size_t)length, f);
        fclose(f);
    }
    reads++;
    bytes_read += got;
    if (port_opt.verbose) { port_log("port> dvd read done, got %u\n", (unsigned)got); }
    return (s32)got;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
    s32 r;
    (void)prio;
    r = do_read(fi, addr, length, offset);
    fi->cb.state = DVD_STATE_END;
    fi->cb.transferredSize = (u32)(r > 0 ? r : 0);
    if (callback) {
        callback(r, fi);
    }
    return TRUE;
}

BOOL DVDReadPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, s32 prio) {
    /* The SDK header declares this BOOL, not the transfer length. */
    (void)prio;
    return do_read(fi, addr, length, offset) >= 0 ? TRUE : FALSE;
}

s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }
s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    (void)block;
    return DVD_STATE_END;
}
s32 DVDCancel(DVDCommandBlock* block) {
    (void)block;
    return 0;
}

void port_dvd_service(void) {
    /* nothing pending: reads complete inline.  M36: the loader's watches and
     * its one-CPU slice run here, at the retrace, and change no completion. */
    port_dvd_cache_service();
}

/* ---- M36: what the loader (dvd_cache.c) needs of the table --------------- */

int port_dvd_entry_count(void) { return entry_count; }
const char* port_dvd_entry_path(int n) { return (n >= 0 && n < entry_count) ? entries[n].path : NULL; }
unsigned port_dvd_entry_length(int n) { return (n >= 0 && n < entry_count) ? entries[n].length : 0; }
unsigned port_dvd_entry_offset(int n) { return (n >= 0 && n < entry_count) ? entries[n].offset : 0; }
/* the image's path, or NULL for the extracted tree (then the file is
 * tree_root/path) */
const char* port_dvd_image_path(void) { return image ? port_opt.image : NULL; }
const char* port_dvd_tree_root(void) { return image ? NULL : tree_root; }

void port_dvd_stats(void) {
    port_log("port> DVD: %lu reads, %lu bytes, %.0f ms in reads, %lu over 100 ms\n", reads, bytes_read,
             slow_read_s * 1000.0, slow_reads);
}
