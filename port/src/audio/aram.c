/* ARAM: 16 MB of host memory addressed by offset.
 *
 * This is the easiest piece of the whole console to host, because the game
 * never holds an ARAM pointer: `src/game/armem.c` runs a 64-block first-fit
 * allocator over plain `u32` offsets (`AMEM_PTR`) above HU_AMEM_BASE, and every
 * transfer goes through `ARQPostRequest`, which becomes a memcpy.
 *
 * The one thing that is not free on a 64-bit host is that a `u32` in this
 * interface is sometimes an ARAM offset and sometimes a main-memory address;
 * patches.txt widens the latter to `uintptr_t` in the SDK's own headers, which
 * is the identity on the G4.
 *
 * MusyX owns everything below HU_AMEM_BASE (8.03 MB of sample data) and the
 * game the 8.39 MB above it, and neither cares that the "DMA" completed before
 * the call returned -- armem.c counts outstanding requests in `arqCnt` and
 * waits on it, and a count that is already zero satisfies that immediately.
 */
#include "port.h"

#include <string.h>

#include <dolphin/types.h>
#include <dolphin/ar.h>
#include <dolphin/arq.h>

static BOOL ar_inited;
static ARCallback dma_callback;
static unsigned long dma_bytes;

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    (void)stack_index_addr;
    (void)num_entries;
    ar_inited = TRUE;
    port_log("port> ARInit: %u MB of ARAM\n", PORT_ARAM_SIZE >> 20);
    return 0;
}

BOOL ARCheckInit(void) { return ar_inited; }
void ARReset(void) { ar_inited = FALSE; }
u32 ARGetSize(void) { return PORT_ARAM_SIZE; }
u32 ARGetInternalSize(void) { return PORT_ARAM_SIZE; }
u32 ARGetBaseAddress(void) { return 0; }
void ARSetSize(void) {}
void ARClear(u32 flag) {
    (void)flag;
    memset(port_aram(), 0, PORT_ARAM_SIZE);
}
u32 ARGetDMAStatus(void) { return 0; }

ARCallback ARRegisterDMACallback(ARCallback cb) {
    ARCallback old = dma_callback;
    dma_callback = cb;
    return old;
}

void ARStartDMA(u32 type, uintptr_t mainmem_addr, u32 aram_addr, u32 length) {
    u8* aram = (u8*)port_aram();
    if (aram_addr + length > PORT_ARAM_SIZE) {
        port_fatal("ARStartDMA out of range: %08x + %u", aram_addr, length);
    }
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        port_audio_join(); /* M24: the mixer's worker reads ARAM as of its retrace */
        memcpy(aram + aram_addr, (void*)mainmem_addr, length);
    } else {
        memcpy((void*)mainmem_addr, aram + aram_addr, length);
    }
    dma_bytes += length;
    if (dma_callback) {
        dma_callback();
    }
}

/* ---- ARQ ----------------------------------------------------------------- */
/* `source` and `dest` are a main-memory address and an ARAM offset, which way
 * round depending on `type`.  The game's own allocator keeps them straight. */

static BOOL arq_inited;
static u32 chunk_size = ARQ_CHUNK_SIZE_DEFAULT;

void ARQInit(void) { arq_inited = TRUE; }
void ARQReset(void) { arq_inited = FALSE; }
BOOL ARQCheckInit(void) { return arq_inited; }
void ARQSetChunkSize(u32 size) { chunk_size = size; }
u32 ARQGetChunkSize(void) { return chunk_size; }
void ARQRemoveRequest(ARQRequest* task) { (void)task; }
void ARQRemoveOwnerRequest(u32 owner) { (void)owner; }
void ARQFlushQueue(void) {}

void ARQPostRequest(ARQRequest* task, u32 owner, u32 type, u32 priority, uintptr_t source,
                    uintptr_t dest, u32 length, ARQCallback callback) {
    task->next = NULL;
    task->owner = owner;
    task->type = type;
    task->priority = priority;
    task->source = source;
    task->dest = dest;
    task->length = length;
    task->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        ARStartDMA(ARAM_DIR_MRAM_TO_ARAM, source, dest, length);
    } else {
        ARStartDMA(ARAM_DIR_ARAM_TO_MRAM, dest, source, length);
    }
    if (callback) {
        callback((uintptr_t)task);
    }
}

void port_arq_service(void) { /* nothing pending: transfers complete inline */ }

/* ---- snapshots ------------------------------------------------------------
 * ARAM's *contents* travel with the arena; what is here is the port's own
 * view of the hardware: whether AR/ARQ have been initialised, the DMA
 * completion callback the game installed, and ARQ's chunk size.  Transfers
 * themselves complete inline (see port_arq_service), so there is never one in
 * flight at the top of a retrace, which is the only place a snapshot is
 * taken. */
void port_aram_snap_register(void) {
    port_snap_register("aram.ar_inited", &ar_inited, sizeof(ar_inited));
    port_snap_register("aram.dma_callback", &dma_callback, sizeof(dma_callback));
    port_snap_register("aram.dma_bytes", &dma_bytes, sizeof(dma_bytes));
    port_snap_register("aram.arq_inited", &arq_inited, sizeof(arq_inited));
    port_snap_register("aram.chunk_size", &chunk_size, sizeof(chunk_size));
}
