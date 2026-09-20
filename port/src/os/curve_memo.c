/* M28 experiment (b): the motion curves, memoised (PLAN.md 43).
 *
 * Hu3DMotionExec (src/game/hsfmotion.c) evaluates one curve per track per
 * model per frame through GetCurve: a linear or Bezier segment search over
 * the keyframes and the interpolation.  A curve's value is a function of the
 * track (its keyframes never change between the motion's load and its free;
 * the one field GetBezier writes, `start`, is the segment it found, and it
 * reads it back as where to look first) and of the time, and the game asks
 * the same questions again: an idle loop replays the same times, and four
 * players idling on the board replay the same tracks.  So, in the shape of
 * the sin/cos memo (psmtx_c.c): a direct-mapped table keyed on the track's
 * address, the time's bits and the `start` on entry, holding the value's
 * bits and the `start` on exit; a hit returns the very bits GetLinear /
 * GetBezier computed on the same inputs and writes back the same `start`.
 *
 * The one thing that can make a key lie is the track's address being
 * reused: a motion freed and another loaded at the same place.  Every free
 * the game makes reaches port_mem_freed (M19's hook in HuMemMemoryFree),
 * which bumps a generation counter for every 4 KB page of the freed block;
 * an entry remembers its page's generation and a hit needs it unchanged.
 * That is the M19 skinning registry's lifetime argument, in one integer.
 *
 * --nocurvememo: every call to the game's own body; the hit rate is in the
 * shutdown report either way. */
#include "port.h"

#include <stdlib.h>
#include <string.h>

#include "game/hsfformat.h"

#define CURVE_SLOTS 65536 /* 20 bytes each: 1.3 MB */
#define PAGE_SHIFT 12

typedef struct {
    const HSFTRACK* track; /* NULL: empty */
    u32 time_bits;
    u32 value_bits;
    u32 page_gen;
    u8 start_in, start_out;
} CurveSlot;

static CurveSlot* slots;
static u32* page_gen; /* one per 4 KB page of MEM1 */
static unsigned long npages;
static const u8* mem1_lo;
static unsigned long hits, misses, stale;
static unsigned miss_slot; /* the slot the last miss will fill */
static u8 miss_start_in;
static u32 miss_page_gen;

static void memo_init(void) {
    if (slots) {
        return;
    }
    slots = calloc(CURVE_SLOTS, sizeof(*slots));
    mem1_lo = (const u8*)port_mem1_lo();
    npages = ((const u8*)port_mem1_hi() - mem1_lo + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
    page_gen = calloc(npages, sizeof(*page_gen));
}

static inline int page_of(const void* p, unsigned long* page) {
    unsigned long off;
    if ((const u8*)p < mem1_lo) {
        return 0;
    }
    off = ((const u8*)p - mem1_lo) >> PAGE_SHIFT;
    if (off >= npages) {
        return 0;
    }
    *page = off;
    return 1;
}

static inline unsigned slot_of(const HSFTRACK* t, u32 bits, u8 start) {
    u32 h = (u32)(uintptr_t)t;
    h ^= bits * 0x9E3779B1u;
    h ^= h >> 15;
    h ^= (u32)start << 7;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h & (CURVE_SLOTS - 1);
}

int port_curve_memo_get(HSFTRACK* t, float time, float* out) {
    union {
        float f;
        u32 u;
    } k;
    unsigned i;
    unsigned long page;
    CurveSlot* s;
    if (port_opt.nocurvememo) {
        return 0;
    }
    if (!slots) {
        memo_init();
    }
    if (!page_of(t, &page)) {
        return 0; /* a track outside MEM1 (a REL's static?) is not memoised */
    }
    k.f = time;
    i = slot_of(t, k.u, t->start);
    s = &slots[i];
    if (s->track == t && s->time_bits == k.u && s->start_in == t->start) {
        if (s->page_gen == page_gen[page]) {
            k.u = s->value_bits;
            *out = k.f;
            t->start = s->start_out;
            hits++;
            return 1;
        }
        stale++;
    }
    misses++;
    miss_slot = i;
    miss_start_in = t->start;
    miss_page_gen = page_gen[page];
    return 0;
}

float port_curve_memo_put(HSFTRACK* t, float time, float value) {
    union {
        float f;
        u32 u;
    } k, v;
    CurveSlot* s;
    if (port_opt.nocurvememo || !slots) {
        return value;
    }
    k.f = time;
    v.f = value;
    s = &slots[miss_slot];
    s->track = t;
    s->time_bits = k.u;
    s->value_bits = v.u;
    s->page_gen = miss_page_gen;
    s->start_in = miss_start_in;
    s->start_out = t->start;
    return value;
}

/* from port_mem_freed: every page the block touched moves on */
void port_curve_memo_freed(const void* data, unsigned long size) {
    unsigned long p0, p1;
    if (!page_gen || !size) {
        return;
    }
    if (!page_of(data, &p0)) {
        return;
    }
    if (!page_of((const u8*)data + size - 1, &p1)) {
        p1 = npages - 1;
    }
    for (; p0 <= p1; p0++) {
        page_gen[p0]++;
    }
}

void port_curve_memo_report(void) {
    if (hits + misses) {
        port_log("port> curve memo: %lu hits, %lu misses (%.1f%% hit), %lu stale by a free%s\n",
                 hits, misses, 100.0 * hits / (hits + misses), stale,
                 port_opt.nocurvememo ? " (--nocurvememo: every call to the game)" : "");
    }
}
