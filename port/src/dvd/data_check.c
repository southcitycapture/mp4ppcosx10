/* M53 (PLAN.md 68): the data check -- a data file's bytes in memory against
 * the disc's, the moment before the game decodes them.
 *
 * M52's scoreboard saw Dungeon Duos (m432) fault once in nine runs at its
 * first model load: LoadHSF's DispObject followed a child pointer that no
 * HSF of the disc can produce (0x517c6018).  The model is decoded fresh from
 * the minigame's data directory image (m432.bin, 2.6 MB, read into the DVD
 * heap by instDll ~170 frames earlier), so the bytes the decoder read were
 * not the disc's, or the decode went wrong, or something wrote the model
 * afterwards.  This file answers the first question every time it matters:
 *
 *   HuDecodeData (patches.txt: the M53 hook) hands its source here first.
 *   The read that put those bytes in MEM1 is the newest dvd_fs.c read
 *   covering the address (a record is forgotten when its block is freed or an
 *   ARAM transfer writes over it: those bytes are no read's); the disc's own
 *   bytes for the same range come again from the image (never the resident
 *   set's copy: that copy is a suspect too), and the two are compared over the member's raw size plus 64
 *   (most of an LZ member).  A difference is logged loudly -- the file, the
 *   offsets, the frames of the read and of the decode, the bytes either side,
 *   any later read that landed on the range.
 *
 * And for the crash itself, port_data_crash_report (crash.c, every fault)
 * prints the last decode and, when the fault is inside LoadHSF, the loader's
 * own globals (fileptr, NSymIndex, objtop, the header's sections) and what
 * the faulting address is in objtop's terms.
 *
 * --nodatacheck turns the check off. */
#include <dolphin/types.h>
#include "port.h"

#include "game/hsfformat.h"

#include <stdlib.h>
#include <string.h>

unsigned gl13_frame_number(void);
int port_dvd_read_of(const void* p, const void** addr, unsigned* len, int* entry, unsigned* off,
                     unsigned* frame);
int port_dvd_reads_over(const void* p, unsigned n, const void* except, unsigned* f, int* e);
unsigned port_dvd_truth(int n, unsigned off, void* dst, unsigned len);
int port_dvd_resident_copy(int n, unsigned off, void* dst, unsigned len);
void port_wb_disarm(const void* ptr, size_t n);

/* the game's loader globals (hsfload.c) */
extern HSFHEADER head;
extern void* fileptr;
extern void** NSymIndex;
extern HSFOBJECT* objtop;

static u8* truth;
static unsigned truth_cap;
static unsigned truth_len;  /* the last check's disc bytes in `truth` */
static u8* second;          /* --hsfcheck's second decode */
static unsigned second_cap;
static unsigned long st_hsf_checks, st_hsf_bad;
void HuDecodeData(void* src, void* dst, u32 size, s32 decodeType);
static int in_second;       /* the second decode is running: not a game decode */
static unsigned long st_checks, st_unchecked, st_bad, st_bytes;
static double st_s;

static struct {
    const void* src;
    void* dst;
    u32 size;
    s32 type;
    int entry;
    unsigned off, read_frame, frame;
    int known;
} last;

static void hexline(const char* tag, const u8* p, unsigned n) {
    char buf[3 * 32 + 1];
    unsigned i, k = 0;
    for (i = 0; i < n && i < 32; i++) {
        k += (unsigned)snprintf(buf + k, sizeof(buf) - k, "%02x ", p[i]);
    }
    buf[k] = '\0';
    port_log("port>     %s %s\n", tag, buf);
}

void port_decode_check(void* src, void* dst, u32 size, s32 type) {
    const void* addr;
    unsigned len, off, frame, n, got, i, first = 0, last_d = 0, ndiff = 0;
    int entry;
    double t0;
    if (in_second) {
        return;
    }
    last.src = src;
    last.dst = dst;
    last.size = size;
    last.type = type;
    last.frame = gl13_frame_number();
    last.known = 0;
    if (port_opt.nodatacheck || !src || !size) {
        return;
    }
    if (!port_dvd_read_of(src, &addr, &len, &entry, &off, &frame)) {
        st_unchecked++; /* not a disc read's bytes (ARAM's, or long gone from the ring) */
        return;
    }
    t0 = port_now_seconds();
    last.entry = entry;
    last.off = off + (unsigned)((const u8*)src - (const u8*)addr);
    last.read_frame = frame;
    last.known = 1;
    n = (unsigned)((const u8*)addr + len - (const u8*)src);
    if (n > size + 64) {
        n = size + 64;
    }
    if (n > truth_cap) {
        free(truth);
        truth_cap = n + 65536;
        truth = (u8*)malloc(truth_cap);
        if (!truth) {
            truth_cap = 0;
            return;
        }
    }
    got = port_dvd_truth(entry, last.off, truth, n);
    truth_len = got;
    st_checks++;
    st_bytes += got;
    if (got && memcmp(src, truth, got) != 0) {
        const u8* m = (const u8*)src;
        unsigned f = 0;
        int e = -1;
        int later;
        for (i = 0; i < got; i++) {
            if (m[i] != truth[i]) {
                if (!ndiff) {
                    first = i;
                }
                last_d = i;
                ndiff++;
            }
        }
        st_bad++;
        later = port_dvd_reads_over((const u8*)src + first, last_d - first + 1, addr, &f, &e);
        port_log("\nport> DATA CHECK FAILED (M53): %s +%u: %u of %u bytes in memory are not the disc's "
                 "(first +%u, last +%u; MEM1 %p..%p); read at frame %u, decoded at frame %u "
                 "(type %d, raw %u bytes into %p)\n",
                 port_dvd_entry_path(entry), last.off, ndiff, got, first, last_d,
                 (void*)(m + first), (void*)(m + last_d), frame, last.frame, (int)type, size, dst);
        if (later) {
            port_log("port>     %d later read(s) landed on that range, the newest %s at frame %u\n",
                     later, port_dvd_entry_path(e), f);
        }
        i = first >= 8 ? first - 8 : 0;
        hexline("memory:", m + i, got - i);
        hexline("disc:  ", truth + i, got - i);
        {
            /* the resident set's copy, if it holds one: the disc's, or what
             * memory has (the copy itself was written over)? */
            u8* rc = (u8*)malloc(got);
            if (rc && port_dvd_resident_copy(entry, last.off, rc, got)) {
                port_log("port>     the resident set's copy of the range: %s\n",
                         memcmp(rc, truth, got) == 0 ? "the disc's bytes"
                         : memcmp(rc, m, got) == 0 ? "the SAME wrong bytes (the copy was written over)"
                                                   : "different from both");
            }
            free(rc);
        }
        port_log("port>     (logged only; the game decodes what is in memory)\n");
    }
    st_s += port_now_seconds() - t0;
}

/* --hsfcheck: LoadHSF's entry.  The model it is handed is the last decode's
 * output; decode the disc's bytes (kept by the check above) a second time
 * into host memory and compare, before the loader rewrites anything. */
void port_hsf_load_check(void* data) {
    const u8* m = (const u8*)data;
    unsigned i, nd = 0, first = 0, lastd = 0;
    if (!port_opt.hsfcheck || in_second || !last.known || last.dst != data || !truth_len ||
        truth_len < 8) {
        return;
    }
    if (last.size + 64 > second_cap) {
        free(second);
        second_cap = last.size + 65536;
        second = (u8*)malloc(second_cap);
        if (!second) {
            second_cap = 0;
            return;
        }
    }
    memset(second, 0, last.size);
    in_second = 1;
    HuDecodeData(truth, second, last.size, last.type);
    in_second = 0;
    st_hsf_checks++;
    if (memcmp(m, second, last.size) == 0) {
        return;
    }
    for (i = 0; i < last.size; i++) {
        if (m[i] != second[i]) {
            if (!nd) {
                first = i;
            }
            lastd = i;
            nd++;
        }
    }
    st_hsf_bad++;
    port_log("\nport> HSF CHECK FAILED (M53): the model at %p (%s +%u, raw %u, type %d, decoded at "
             "frame %u) differs from a second decode of the disc's bytes in %u bytes (first +%u, "
             "last +%u)\n",
             data, port_dvd_entry_path(last.entry), last.off, last.size, (int)last.type, last.frame,
             nd, first, lastd);
    i = first >= 8 ? first - 8 : 0;
    hexline("memory:", m + i, last.size - i);
    hexline("second:", second + i, last.size - i);
}

void port_data_check_report(void) {
    if (port_opt.nodatacheck) {
        port_log("port> data check (M53): off (--nodatacheck)\n");
        return;
    }
    port_log("port> data check (M53): %lu decodes checked against the disc (%.1f MB, %.0f ms), "
             "%lu not from a disc read, %lu FAILED\n",
             st_checks, st_bytes / 1048576.0, st_s * 1000.0, st_unchecked, st_bad);
    if (port_opt.hsfcheck) {
        port_log("port> hsf check (M53): %lu models compared with a second decode, %lu FAILED\n",
                 st_hsf_checks, st_hsf_bad);
    }
}

/* the crash handler: the last decode, and LoadHSF's state when inside it */
void port_data_crash_report(const void* fault_addr) {
    if (last.src) {
        port_log("    last decode: frame %u, %p -> %p, raw %u, type %d", last.frame, last.src, last.dst,
                 last.size, (int)last.type);
        if (last.known) {
            port_log(", %s +%u (read at frame %u)\n", port_dvd_entry_path(last.entry), last.off,
                     last.read_frame);
        } else {
            port_log("\n");
        }
    }
    if (objtop) {
        /* LoadHSF clears objtop on the way out: non-NULL = inside a load */
        uintptr_t a = (uintptr_t)fault_addr, o = (uintptr_t)objtop;
        port_log("    inside LoadHSF: fileptr %p NSymIndex %p objtop %p (objects %d at +%u, symbols %d "
                 "at +%u, strings +%u, magic %.8s)\n",
                 fileptr, (void*)NSymIndex, (void*)objtop, (int)head.object.count,
                 (unsigned)head.object.ofs, (int)head.symbol.count, (unsigned)head.symbol.ofs,
                 (unsigned)head.string.ofs, head.magic);
        port_log("    the fault address is objtop %+ld bytes (%ld objects of %u, remainder %ld)\n",
                 (long)(a - o), (long)(a - o) / (long)sizeof(HSFOBJECT), (unsigned)sizeof(HSFOBJECT),
                 (long)(a - o) % (long)sizeof(HSFOBJECT));
        if (last.known && last.dst == fileptr) {
            port_log("    the model being loaded is the last decode's\n");
            /* --hsfcheck kept a clean decode: each object's children as the
             * disc has them against what the loader's table holds now */
            if (port_opt.hsfcheck && second && second_cap >= last.size) {
                const HSFHEADER* h = (const HSFHEADER*)second;
                u32 k, c;
                for (k = 0; k < (u32)h->object.count && k < 64; k++) {
                    const HSFOBJECT* o = (const HSFOBJECT*)(second + h->object.ofs) + k;
                    u32 cc = o->mesh.childrenCount, ch = (u32)(uintptr_t)o->mesh.children;
                    if (o->type > 9 || o->type == 7 || o->type == 8 || !cc) {
                        continue;
                    }
                    for (c = 0; c < cc && c < 8; c++) {
                        u32 idx = ((const u32*)(second + h->symbol.ofs))[ch + c];
                        u32 now = (u32)(uintptr_t)NSymIndex[ch + c];
                        port_log("    object %u child %u: symbol %u = object %u on the disc; the table "
                                 "holds %08x now (objtop + %u x %u = %08x)\n",
                                 k, c, ch + c, idx, now, idx, (unsigned)sizeof(HSFOBJECT),
                                 (u32)(uintptr_t)(objtop + idx));
                    }
                }
            }
        }
    }
}
