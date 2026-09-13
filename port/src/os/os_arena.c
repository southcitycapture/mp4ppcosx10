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

static u8* region; /* [ game stack | MEM1 ], one mmap, one 4 GB window */
static u8* mem1;
static u8* aram;
static void* arena_lo;
static void* arena_hi;

uintptr_t port_text_base_hi;
uintptr_t port_stack_base_hi;

void port_mem_init(void) {
    size_t total = PORT_GAME_STACK + PORT_MEM1_SIZE;
    region = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) {
        port_fatal("cannot map %zu bytes for the game stack + MEM1", total);
    }
    mem1 = region + PORT_GAME_STACK;
    aram = calloc(1, PORT_ARAM_SIZE);
    if (!aram) {
        port_fatal("cannot allocate %u bytes of ARAM", PORT_ARAM_SIZE);
    }
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
}

void* port_game_stack_top(void) { return region + PORT_GAME_STACK; }
void* port_mem1_lo(void) { return mem1; }
void* port_mem1_hi(void) { return mem1 + PORT_MEM1_SIZE; }
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
