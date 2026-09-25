/* MEM1, ARAM, the OS arena and OSAlloc's heaps.
 *
 * The game asks the console for 24 MB of MEM1 and gets a single heap over
 * whatever is left after the two external framebuffers, then runs its own
 * `HuMemHeap` allocator on top (src/game/memory.c).  So the port only has to
 * be honest about *sizes*: it hands out one 24 MB block, reports the retail
 * memory size and the retail console type -- which is what keeps the game off
 * the development-hardware `LoadMemInfo` path in init.c -- and implements
 * OSAlloc as a plain first-fit allocator over the range the game gives it.
 *
 * Keeping the arena the console's real size matters: the game prints its own
 * "Rest Memory" numbers, and a wrong arena would make every one of them lie.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <dolphin/types.h>
#include <dolphin/os.h>

#define OS_CONSOLE_RETAIL1 0x00000001

#define PORT_GAME_STACK 0x00800000u /* 8 MB, the stack the game itself runs on */

/* ---- guard pages --------------------------------------------------------
 *
 * PLAN.md §17.7 spent an evening on a fault at 0x4800000, which turned out to
 * be 16 MB above the top of MEM1 -- the place where the *host's* address space
 * happened to run out, not the place where the bug was.  A pointer had walked
 * off the end of a MEM1 array and written through 16 MB of whatever the
 * process had mapped above it before anything noticed.
 *
 * So every region the port hands to the game is now mapped with PROT_NONE
 * pages either side of it.  An overrun faults at the boundary, one page past
 * the end of the thing it overran, and `port_mem_region_name` turns the
 * address into a sentence.  The diagnosis becomes immediate and the corruption
 * never happens at all, which matters more: a run that dies at 0x3800000 is
 * worth more than a run that keeps going with a scribbled heap.
 *
 * The guards are 64 KB rather than one page on purpose.  A strength-reduced
 * loop striding a struct at a time can step over a single 4 KB page without
 * touching it; 64 KB is wide enough that nothing in this game jumps it.
 *
 * The layout is one mmap, because MEM1 and the game stack must share a 4 GB
 * window (see below), and guarding the stack as well is free:
 *
 *   [guard][ game stack 8 MB ][guard][ MEM1 24 MB ][guard]
 *
 * ARAM gets the same treatment in its own mapping.  It used to be a calloc,
 * where an overrun landed in the C heap and was invisible.
 */
#define PORT_GUARD_SIZE 0x00010000u /* 64 KB either side of every region */

static u8* region; /* [ guard | stack | guard | MEM1 | guard ], one 4 GB window */
static u8* mem1;
#ifndef PORT_MEM1_ALIGN
#define PORT_MEM1_ALIGN 0x100000u /* M43: MEM1's base alignment (1 MB) ... */
#endif
#ifndef PORT_MEM1_ALIGN_OFF
#define PORT_MEM1_ALIGN_OFF 0x70000u /* ... and offset: 0.9.11's low bits (0x3570000) */
#endif
static u8* aram;
static void* arena_lo;
static void* arena_hi;

uintptr_t port_text_base_hi;
uintptr_t port_stack_base_hi;

/* Every span the port knows the name of, in address order, including the
 * guards.  The crash handler walks this to say what was hit. */
#define PORT_REGION_MAX 8
static struct port_region {
    const char* name;
    const u8* lo;
    const u8* hi;
    int guard;          /* 1 if this span is PROT_NONE */
    const char* of;     /* for a guard: the region it protects */
} regions[PORT_REGION_MAX];
static int region_count;

static void region_add(const char* name, const u8* lo, const u8* hi, int guard,
                       const char* of) {
    if (region_count < PORT_REGION_MAX) {
        regions[region_count].name = name;
        regions[region_count].lo = lo;
        regions[region_count].hi = hi;
        regions[region_count].guard = guard;
        regions[region_count].of = of;
        region_count++;
    }
}

/* Make [lo, lo+len) unreadable and unwritable.  A failure here is not fatal:
 * the port runs exactly as it did before, it just stops catching this class of
 * bug, and saying so is better than refusing to start. */
static void guard_off(u8* lo, size_t len, const char* what) {
    if (mprotect(lo, len, PROT_NONE) != 0) {
        port_log("port> warning: cannot guard %s at %p -- overruns will not "
                 "fault at the boundary\n", what, (void*)lo);
    }
}

/* Name the region an address falls in.  Returns NULL if the port does not know
 * the address.  `off` is filled with the offset into whatever was named, and
 * `base`/`end` with its bounds, so the caller can print all three. */
const char* port_mem_region_name(const void* addr, long* off, const void** base,
                                 const void** end) {
    const u8* a = (const u8*)addr;
    int i;
    for (i = 0; i < region_count; i++) {
        if (a >= regions[i].lo && a < regions[i].hi) {
            if (off) {
                *off = (long)(a - regions[i].lo);
            }
            if (base) {
                *base = regions[i].lo;
            }
            if (end) {
                *end = regions[i].hi;
            }
            return regions[i].name;
        }
    }
    return NULL;
}

/* For the crash handler's second line: if the address is in a guard, which
 * region did it run off, and in which direction. */
const char* port_mem_guard_of(const void* addr, const void** rlo,
                              const void** rhi) {
    const u8* a = (const u8*)addr;
    int i;
    for (i = 0; i < region_count; i++) {
        if (regions[i].guard && a >= regions[i].lo && a < regions[i].hi) {
            int j;
            for (j = 0; j < region_count; j++) {
                if (!regions[j].guard && regions[j].name == regions[i].of) {
                    if (rlo) {
                        *rlo = regions[j].lo;
                    }
                    if (rhi) {
                        *rhi = regions[j].hi;
                    }
                    break;
                }
            }
            return regions[i].of;
        }
    }
    return NULL;
}

void port_mem_regions_dump(void) {
    int i;
    port_log("port> memory map:\n");
    for (i = 0; i < region_count; i++) {
        port_log("port>   %p-%p  %8lu KB  %s%s\n", (const void*)regions[i].lo,
                 (const void*)regions[i].hi,
                 (unsigned long)((regions[i].hi - regions[i].lo) >> 10),
                 regions[i].name, regions[i].guard ? "  (PROT_NONE)" : "");
    }
}

void port_mem_init(void) {
    /* [guard][ stack ][guard][ MEM1 ][guard] */
    size_t total = PORT_GUARD_SIZE + PORT_GAME_STACK + PORT_GUARD_SIZE +
                   PORT_MEM1_SIZE + PORT_GUARD_SIZE;
    size_t aram_total = PORT_GUARD_SIZE + PORT_ARAM_SIZE + PORT_GUARD_SIZE;
    u8* stack_lo;
    u8* aram_map;

    /* M43 (PLAN.md 58.10): MEM1's base at a fixed alignment and offset, not
     * wherever the kernel's first fit puts it -- the game's hot pages'
     * placement in the 7455's translation lookaside buffer follows the base,
     * and the base followed the size of the binary's own data (0x3570000 in
     * 0.9.11, 0x359a000 once M43's statics grew it: m432's game thread 0.9 ms
     * slower).  The mapping is over-sized by two alignments and the unused
     * head stays mapped (a megabyte or two, never touched). */
    {
        u8* raw = (u8*)mmap(NULL, total + 2 * (size_t)PORT_MEM1_ALIGN, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        uintptr_t m;
        if (raw == (u8*)MAP_FAILED) {
            port_fatal("cannot map %zu bytes for the game stack + MEM1", total);
        }
        m = (uintptr_t)raw + PORT_GUARD_SIZE + PORT_GAME_STACK + PORT_GUARD_SIZE;
        m = ((m + PORT_MEM1_ALIGN - 1) & ~(uintptr_t)(PORT_MEM1_ALIGN - 1)) + PORT_MEM1_ALIGN_OFF;
        region = (u8*)(m - (PORT_GUARD_SIZE + PORT_GAME_STACK + PORT_GUARD_SIZE));
    }
    stack_lo = region + PORT_GUARD_SIZE;
    mem1 = stack_lo + PORT_GAME_STACK + PORT_GUARD_SIZE;

    aram_map = mmap(NULL, aram_total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (aram_map == MAP_FAILED) {
        port_fatal("cannot map %zu bytes for ARAM", aram_total);
    }
    aram = aram_map + PORT_GUARD_SIZE;
    memset(aram, 0, PORT_ARAM_SIZE); /* calloc's zero, kept: ARInit relies on it */
    /* MEM1 must not straddle a 4 GB boundary: the game stores coroutine stack
     * pointers in u32 fields (HUPROCESS::base_sp, jmp_buf::sp) and the port
     * reconstructs them from this base.  On the G4 the base is zero. */
    if (((uintptr_t)region >> 32) != (((uintptr_t)region + total - 1) >> 32)) {
        port_fatal("the game stack + MEM1 region straddles a 4 GB boundary at %p",
                   (void*)region);
    }
    port_stack_base_hi = (uintptr_t)region & ~(uintptr_t)0xFFFFFFFFu;
    port_text_base_hi = (uintptr_t)&port_mem_init & ~(uintptr_t)0xFFFFFFFFu;

    arena_lo = mem1;
    arena_hi = mem1 + PORT_MEM1_SIZE;

    /* Order matters only for the dump; the lookup is a linear scan. */
    region_add("the guard below the game stack", region, stack_lo, 1,
               "the game stack");
    region_add("the game stack", stack_lo, stack_lo + PORT_GAME_STACK, 0, NULL);
    region_add("the guard between the game stack and MEM1",
               stack_lo + PORT_GAME_STACK, mem1, 1, "MEM1");
    region_add("MEM1", mem1, mem1 + PORT_MEM1_SIZE, 0, NULL);
    region_add("the guard above MEM1", mem1 + PORT_MEM1_SIZE,
               mem1 + PORT_MEM1_SIZE + PORT_GUARD_SIZE, 1, "MEM1");
    region_add("the guard below ARAM", aram_map, aram, 1, "ARAM");
    region_add("ARAM", aram, aram + PORT_ARAM_SIZE, 0, NULL);
    region_add("the guard above ARAM", aram + PORT_ARAM_SIZE,
               aram + PORT_ARAM_SIZE + PORT_GUARD_SIZE, 1, "ARAM");

    guard_off(region, PORT_GUARD_SIZE, "below the game stack");
    guard_off(stack_lo + PORT_GAME_STACK, PORT_GUARD_SIZE,
              "between the game stack and MEM1");
    guard_off(mem1 + PORT_MEM1_SIZE, PORT_GUARD_SIZE, "above MEM1");
    guard_off(aram_map, PORT_GUARD_SIZE, "below ARAM");
    guard_off(aram + PORT_ARAM_SIZE, PORT_GUARD_SIZE, "above ARAM");
}

void* port_game_stack_top(void) { return region + PORT_GUARD_SIZE + PORT_GAME_STACK; }
void* port_mem1_lo(void) { return mem1; }
void* port_mem1_hi(void) { return mem1 + PORT_MEM1_SIZE; }
/* M34: is `p` in the one mmap -- the game stack, MEM1 or a guard?  What is
 * not is the executable's own data (a DrawObjData entry, say). */
int port_in_game_region(const void* p) {
    return region != NULL && (const u8*)p >= region &&
           (const u8*)p < region + PORT_GUARD_SIZE + PORT_GAME_STACK + PORT_GUARD_SIZE + PORT_MEM1_SIZE + PORT_GUARD_SIZE;
}
void* port_aram(void) { return aram; }

/* ---- arena --------------------------------------------------------------- */

void* OSGetArenaLo(void) { return arena_lo; }
void* OSGetArenaHi(void) { return arena_hi; }
void OSSetArenaLo(void* lo) { arena_lo = lo; }
void OSSetArenaHi(void* hi) { arena_hi = hi; }

u32 OSGetPhysicalMemSize(void) { return PORT_MEM1_SIZE; }
u32 OSGetConsoleSimulatedMemSize(void) { return PORT_MEM1_SIZE; }
u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL1; }

/* ---- OSAlloc ------------------------------------------------------------- */
/* One free list per heap, first fit, 32-byte granularity -- the same alignment
 * the console's OSAlloc guarantees and that HuMem's 32-byte rounding assumes. */

#define OS_ALLOC_ALIGN 32
#define PORT_MAX_HEAPS 8

typedef struct Block {
    struct Block* next;
    u32 size; /* payload bytes, excluding this header */
    u32 used;
} Block;

typedef struct Heap {
    int valid;
    Block* first;
} Heap;

static Heap heaps[PORT_MAX_HEAPS];
volatile OSHeapHandle __OSCurrHeap = -1;

static u32 round_up(u32 x) { return (x + (OS_ALLOC_ALIGN - 1)) & ~(u32)(OS_ALLOC_ALIGN - 1); }

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
    int i;
    (void)arenaEnd;
    for (i = 0; i < PORT_MAX_HEAPS; i++) {
        heaps[i].valid = 0;
        heaps[i].first = NULL;
    }
    __OSCurrHeap = -1;
    if (maxHeaps > PORT_MAX_HEAPS) {
        port_fatal("OSInitAlloc: %d heaps requested, port supports %d", maxHeaps,
                   PORT_MAX_HEAPS);
    }
    /* the console reserves a descriptor array at the bottom of the arena */
    return (void*)(((uintptr_t)arenaStart + 31) & ~(uintptr_t)31);
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
    int i;
    uintptr_t lo = ((uintptr_t)start + 31) & ~(uintptr_t)31;
    uintptr_t hi = (uintptr_t)end & ~(uintptr_t)31;
    for (i = 0; i < PORT_MAX_HEAPS; i++) {
        if (!heaps[i].valid) {
            break;
        }
    }
    if (i == PORT_MAX_HEAPS || hi <= lo + sizeof(Block)) {
        port_fatal("OSCreateHeap(%p, %p) failed", start, end);
    }
    heaps[i].valid = 1;
    heaps[i].first = (Block*)lo;
    heaps[i].first->next = NULL;
    heaps[i].first->size = (u32)(hi - lo - sizeof(Block));
    heaps[i].first->used = 0;
    port_log("port> OSCreateHeap %d: %p..%p (%u KB)\n", i, (void*)lo, (void*)hi,
             heaps[i].first->size / 1024);
    return i;
}

void OSDestroyHeap(OSHeapHandle heap) {
    if (heap >= 0 && heap < PORT_MAX_HEAPS) {
        heaps[heap].valid = 0;
    }
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end) {
    (void)heap;
    (void)start;
    (void)end;
    port_stub("OSAddToHeap");
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
    OSHeapHandle old = __OSCurrHeap;
    __OSCurrHeap = heap;
    return old;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
    Block* b;
    u32 want = round_up(size ? size : 1);
    if (heap < 0 || heap >= PORT_MAX_HEAPS || !heaps[heap].valid) {
        return NULL;
    }
    for (b = heaps[heap].first; b; b = b->next) {
        if (b->used || b->size < want) {
            continue;
        }
        if (b->size >= want + sizeof(Block) + OS_ALLOC_ALIGN) {
            Block* rest = (Block*)((u8*)(b + 1) + want);
            rest->next = b->next;
            rest->size = b->size - want - (u32)sizeof(Block);
            rest->used = 0;
            b->next = rest;
            b->size = want;
        }
        b->used = 1;
        return (void*)(b + 1);
    }
    return NULL;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
    Block* b;
    if (!ptr || heap < 0 || heap >= PORT_MAX_HEAPS) {
        return;
    }
    b = ((Block*)ptr) - 1;
    b->used = 0;
    /* coalesce forward */
    for (b = heaps[heap].first; b; b = b->next) {
        while (b->next && !b->used && !b->next->used) {
            Block* n = b->next;
            b->size += n->size + (u32)sizeof(Block);
            b->next = n->next;
        }
    }
}

void* OSAllocFixed(void** rstart, void** rend) {
    /* Only ever called from init.c's development-hardware path, which the
     * retail console type keeps us out of. */
    port_stub("OSAllocFixed");
    return *rstart;
}

u32 OSReferentSize(void* ptr) { return ptr ? (((Block*)ptr) - 1)->size : 0; }

long OSCheckHeap(OSHeapHandle heap) {
    Block* b;
    long free_bytes = 0;
    if (heap < 0 || heap >= PORT_MAX_HEAPS || !heaps[heap].valid) {
        return -1;
    }
    for (b = heaps[heap].first; b; b = b->next) {
        if (!b->used) {
            free_bytes += b->size;
        }
    }
    /* Round down to the allocation granularity.  HuMemInitAll's last act is
     * `ptr = OSAlloc(OSCheckHeap(h))` -- it asks how much is left and then asks
     * for exactly that -- so an answer OSAlloc cannot honour is a wrong answer.
     * OSAllocFromHeap rounds requests *up* to 32, and on the 32-bit target the
     * block header is 12 bytes, so a free total is generically 20 (mod 32) and
     * the round-up overshoots by 12.  On the 64-bit host the 16-byte header
     * happened to leave the total already 32-aligned, which is why this only
     * showed up on the G4: heap 4 was never created there and the boot logged
     * "HuMem> Failed OSAlloc left space". */
    return free_bytes & ~(long)(OS_ALLOC_ALIGN - 1);
}

void OSDumpHeap(OSHeapHandle heap) {
    Block* b;
    port_log("port> heap %d:\n", heap);
    if (heap < 0 || heap >= PORT_MAX_HEAPS) {
        return;
    }
    for (b = heaps[heap].first; b; b = b->next) {
        port_log("  %p %8u %s\n", (void*)b, b->size, b->used ? "used" : "free");
    }
}

void OSVisitAllocated(OSAllocVisitor visitor) {
    int h;
    Block* b;
    for (h = 0; h < PORT_MAX_HEAPS; h++) {
        if (!heaps[h].valid) {
            continue;
        }
        for (b = heaps[h].first; b; b = b->next) {
            if (b->used) {
                visitor((void*)(b + 1), b->size);
            }
        }
    }
}

/* "Is this a message id or a pointer to a string?"
 *
 * src/game/window.c asks that four times -- HuWinMesSet, HuWinInsertMesSet,
 * GetMesMaxSizeSub, HuWinKeyWaitNumGet -- and answers it by testing the value
 * against 0x80000000.  On the console the test is exact: every RAM address is
 * at or above 0x80000000 and every id from `MAKE_MESSID(bank, mess)`, which is
 * `(bank << 16) + mess`, is far below it.  Here nothing is above the line.
 * MEM1 is wherever the port allocated it (around 0x02100000 on the G4), the
 * REL bundles are wherever dyld mapped them (around 0x0b000000), and the
 * executable's own rodata is at 0x1000 -- an address range a message id can
 * also occupy, which is why a simple "is it in MEM1" range check is not
 * enough: `saveload.c`'s `SlotNameTbl` and `SAVEWIN_MESS` are string literals
 * in the executable.
 *
 * So the port makes the tag real rather than inferring it.  `MAKE_MESSID_PTR`
 * -- a bare cast on the console -- sets the bit the game already tests, and
 * the four places that turn the value back into a pointer clear it again.
 * Every one of the game's own tests is then untouched and exactly as correct
 * as it is on hardware.
 *
 * This is the one place in the whole game where the *address* MEM1 lives at is
 * load-bearing, which is how PLAN.md 1.5's "no pinned-globals scheme is
 * needed" survived all the way to a screen that shows the player a string the
 * game built itself: the file-select screen naming a memory-card slot "A".
 */
#define PORT_MESS_TAG 0x80000000u

u32 portMessTag(const void* p) {
    uintptr_t v = (uintptr_t)p;
    /* M31 (PLAN.md 46, cause J): one caller hands MAKE_MESSID_PTR a message
     * *id* -- m435's result bubble puts the player's name in with
     * `MAKE_MESSID_PTR(character)` (main.c:3363), a bank-0 id of 0..7, which
     * the console's bare cast leaves an id.  Tagged, it became a pointer to
     * page zero and the bubble read "has 101 points!" without the name.  No
     * pointer lives below 0x1000 in a 32-bit Darwin process (__PAGEZERO), so
     * such a value is an id and keeps its shape; the 113 other callers pass
     * strings in .data, .bss or the stack, all far above it. */
    if (v < 0x1000u) {
        return (u32)v;
    }
    if (v & PORT_MESS_TAG) {
        /* A 32-bit Darwin process puts nothing up there, and a 64-bit host
         * would have been truncated long before reaching here, but a tag that
         * silently collided with an address would present as a missing string
         * on one screen and nothing else, so it says so. */
        static int said;
        if (!said) {
            said = 1;
            port_log("port> window: a message pointer %p already has the tag bit "
                     "set; the id/pointer test in window.c cannot tell them "
                     "apart\n", p);
        }
    }
    return (u32)(v | PORT_MESS_TAG);
}

u8* portMessPtr(u32 mess) { return (u8*)(uintptr_t)(mess & ~PORT_MESS_TAG); }

/* ---- --guardtest --------------------------------------------------------
 *
 * The guards only earn their place if they actually fault, and the machine
 * that finds the next bug is not always the machine that can reproduce it.
 * `--guardtest mem1-hi` writes one byte one past the top of MEM1 and expects
 * the crash handler to say so; the other names cover the other four edges.
 * It is the regression test for §18.1 and it runs anywhere the port builds.
 */
void port_guard_selftest(const char* where) {
    volatile u8* p = NULL;
    if (!strcmp(where, "mem1-hi")) {
        p = (volatile u8*)(mem1 + PORT_MEM1_SIZE);
    } else if (!strcmp(where, "mem1-lo")) {
        p = (volatile u8*)(mem1 - 1);
    } else if (!strcmp(where, "aram-hi")) {
        p = (volatile u8*)(aram + PORT_ARAM_SIZE);
    } else if (!strcmp(where, "aram-lo")) {
        p = (volatile u8*)(aram - 1);
    } else if (!strcmp(where, "stack-lo")) {
        p = (volatile u8*)(region + PORT_GUARD_SIZE - 1);
    } else {
        port_log("port> --guardtest: unknown place '%s' -- use mem1-hi, "
                 "mem1-lo, aram-hi, aram-lo or stack-lo\n", where);
        return;
    }
    port_log("port> --guardtest %s: writing one byte at %p, which should fault\n",
             where, (void*)p);
    *p = 0x5A;
    port_log("port> --guardtest %s: IT DID NOT FAULT -- the guard is not "
             "protecting this edge\n", where);
}
