/* MusyX ARAM: the Dolphin ARAM/ARQ-backed arm of extern/musyx's hw_aramdma.c,
 * ported onto the port's flat 16 MB host ARAM (see port/src/audio/aram.c).
 *
 * hw_aramdma.c's MUSY_TARGET_PC arm is eleven stub bodies -- aramStoreData
 * returns NULL, aramAllocateStreamBuffer returns 0xFF -- which is exactly the
 * path sndStreamAllocEx -> hwInitStream -> aramAllocateStreamBuffer and
 * sndStreamARAMUpdate -> hwFlushStream -> aramUploadData take, so the ADPCM
 * stream (the game's music) would have nowhere to live and no way to fill.
 * hw_aramdma.c is excluded from the build (see port/Makefile) and this file
 * defines the same eleven symbols in its place, built at MUSY_VERSION 2.0.0
 * (extern/musyx/include/musyx/version.h), which is the "<= 2.0.0" arm of the
 * Dolphin original: aramStoreData/aramRemoveData take no ARAMInfo*, and
 * neither aramGetAvailableBytes nor aramGetFirstUserAddress/aramGetUserBytes
 * are declared at this version, so none of the three are defined here.
 *
 * ---- what is ported faithfully vs. simplified ------------------------------
 *
 * The allocator logic is ported essentially unchanged, because MusyX's own
 * sample and stream bookkeeping (synthdata.c, synth_vsamples.c) depends on
 * the addresses it gets back and would misbehave if this heap didn't shrink
 * and grow the way the real one does:
 *
 *   - `aramWrite` bump-allocates up from just past a reserved zero buffer at
 *     the base, for `aramStoreData`/`aramRemoveData` (sample data, LIFO).
 *   - `aramStream` bump-allocates down from the top, for stream buffers,
 *     with the same used/free/idle three-list scheme and the same "is this
 *     the top buffer, so can idle-buffers below it be reclaimed too" dance
 *     in `aramFreeStreamBuffer` -- including a quirk in the original: the
 *     inner loop that sweeps `aramFreeStreamBuffers` for now-reclaimable
 *     buffers never advances `lastSb` past a kept node, so a later removal
 *     in the same sweep unlinks from the list head instead of from that
 *     kept node.  That looks like a bug, but it is the actual behaviour of
 *     the code being ported, so it is reproduced here rather than "fixed"
 *     out from under a comment I can't verify against real hardware.
 *
 * The transfer *queue* is not ported at all, and does not need to be: on
 * Dolphin, `aramUploadData` posts an `ARQRequest` onto one of two 16-deep
 * queues and returns immediately, with the real work happening on an
 * interrupt sometime later and `aramSyncTransferQueue` spinning until the
 * queue drains. On this port, `ARQPostRequest` in aram.c already collapses
 * that whole model to a synchronous memcpy -- there is no DMA engine, ARAM
 * is a flat host buffer, and every "transfer" is done before the call that
 * started it returns. `aramUploadData` here does the same: it validates the
 * request, copies the bytes, and calls the completion callback with `user`
 * before returning. `aramSyncTransferQueue` is therefore an empty function.
 *
 * Whether that is *safe* -- not just convenient -- for stream.c specifically:
 * stream.c's ARAM-side update path is `sndStreamARAMUpdate` -> `hwFlushStream`
 * -> `aramUploadData(..., callback, user)`, where `callback` is a small
 * bookkeeping hook (stream.c uses it to mark a decoded block as landed and
 * advance its read position) and `user` identifies which stream/block it is.
 * The original queues that call and expects the callback to fire from an
 * ARAM-DMA interrupt at some later point, strictly after the bytes are
 * actually in ARAM -- stream.c never reads the destination before the
 * callback fires, and never assumes a *particular* delay, only that the
 * callback eventually arrives once the copy is done. Firing it synchronously,
 * immediately after the (now-immediate) copy completes, satisfies exactly
 * that contract with a delay of zero instead of one interrupt latency: the
 * ordering stream.c depends on (bytes in ARAM, *then* callback) is preserved,
 * nothing reads stale data, and nothing races the callback against the copy
 * because there is no concurrency here to race. The one thing synchronous
 * completion changes is stack depth -- `hwFlushStream` now returns only after
 * running whatever stream.c's callback does -- which is a non-issue at the
 * call depths involved here.
 *
 * For the data-movement primitive itself, this file goes through the port's
 * existing `ARStartDMA` (port/src/audio/aram.c) rather than memcpy'ing
 * against `port_aram()` directly, so a bad ARAM offset gets the same
 * bounds-checked path every other "DMA" in the port uses instead of a
 * second, separately-maintained copy of that check. It deliberately does
 * *not* go through `ARQPostRequest`: that layer's completion callback is
 * invoked with a pointer to the `ARQRequest` itself (for the game's own
 * armem.c, which pulls its fields back out), not with the `user` value
 * MusyX's callback contract requires, which would mean faking a translation
 * table just to get `user` back out on the other side of a call that is
 * synchronous anyway. Calling `ARStartDMA` directly and then invoking the
 * MusyX-supplied callback with `user` myself gets the shared bounds-checked
 * copy without that indirection. Note that `ARStartDMA` fails out of bounds
 * requests by calling `port_fatal` (it must not scribble past a 16 MB host
 * buffer either), which is unacceptable for a request that originated from
 * game data rather than port code -- so every entry point below re-validates
 * against *this* heap's own (tighter) bounds first and only calls into
 * `ARStartDMA` once a request is already known to fit; `ARStartDMA`'s own
 * check is then a backstop that should never actually trip.
 *
 * `hw_dolphin.c`'s `u32 aramSize; u8* aramBase;` globals are not defined
 * here: nothing outside hw_dolphin.c/hw_aramdma.c references the *globals*
 * (there are same-named local parameters in s_data.c/snd_init.c, which are
 * unrelated), and hw_dolphin.c itself is excluded from this build.
 */
#include "port.h"

#include <stdint.h>
#include <string.h>

#include "musyx/hardware.h"

#include <dolphin/ar.h>
#include <dolphin/arq.h>

/* ---- the region and its bookkeeping --------------------------------------
 *
 * `HuAudInit` (src/game/audio.c) sets `msmAram.aramEnd = HU_AMEM_BASE`
 * (0x808000, ~8.03 MB -- include/game/armem.h) and `skipARInit = TRUE`, so
 * MusyX ends up being handed a `length` of at most `HU_AMEM_BASE` bytes
 * starting at `ARGetBaseAddress()` (always 0 on this port -- see aram.c),
 * and `src/game/armem.c` runs its own 64-block allocator over everything
 * from `HU_AMEM_BASE` up to `ARGetSize()`.  This allocator must never hand
 * out, or accept as valid, an address outside [aramBase, aramTop) -- doing
 * so would either collide with the game's own region above HU_AMEM_BASE or
 * (going the other way) run outside the 16 MB host buffer entirely.
 */
#define ARAM_STREAM_BUFFER_COUNT 64u

/* Dolphin's aramInit() zeroes 640 s16 samples (1280 bytes, already a multiple
 * of 32) at the base of the region and hands that address out as
 * aramGetZeroBuffer() -- the block the DSP/CPU mixer reads from when a
 * non-looping voice's playback position runs past the end of its sample.
 * That address is never returned by aramStoreData/aramAllocateStreamBuffer
 * (aramWrite starts just past it), so it stays zero for the process's whole
 * lifetime once aramInit has zeroed it here. */
#define ARAM_ZERO_BUFFER_BYTES 1280ul /* 640 * sizeof(s16), 32-byte aligned */

#define ARAM_ALIGN32(x) (((unsigned long)(x) + 31ul) & ~31ul)

typedef struct AramStreamBuffer {
    struct AramStreamBuffer* next;
    unsigned long aram;
    unsigned long length;
    unsigned long allocLength;
} AramStreamBuffer;

static int g_initialized;
static unsigned long g_aramBase;  /* == ARGetBaseAddress(), always 0 here    */
static unsigned long g_aramTop;   /* end of the region aramInit() was given  */
static unsigned long g_aramWrite; /* bump-up cursor: next free sample byte   */
static unsigned long g_aramStream; /* bump-down cursor: top of free stream area */

static ARAMUploadCallback g_uploadCallback;
static unsigned long g_uploadChunkSize;

static AramStreamBuffer g_streamBuffers[ARAM_STREAM_BUFFER_COUNT];
static AramStreamBuffer* g_usedStreamBuffers;
static AramStreamBuffer* g_freeStreamBuffers;
static AramStreamBuffer* g_idleStreamBuffers;

/* ---- stats for port_musyx_aram_report() ----------------------------------- */
static unsigned long g_bytesUploaded;
static unsigned long g_highWaterWrite;      /* highest g_aramWrite has ever reached */
static unsigned long g_streamBuffersActive; /* currently allocated stream buffers   */
static unsigned long g_streamBuffersPeak;   /* highest g_streamBuffersActive seen   */
static unsigned long g_storeRejected;       /* aramStoreData calls that didn't fit  */
static unsigned long g_streamRejected;      /* stream-buffer requests that failed   */
static unsigned long g_uploadRejected;      /* aramUploadData calls out of bounds   */
static unsigned long g_removeMismatches;    /* aramRemoveData calls with a bad addr */
static int g_removeUnderflowNamed;          /* the empty-heap case, explained once */

static void InitStreamBuffers(void) {
    unsigned long i;

    g_usedStreamBuffers = NULL;
    g_freeStreamBuffers = NULL;
    g_idleStreamBuffers = &g_streamBuffers[0];
    for (i = 1; i < ARAM_STREAM_BUFFER_COUNT; ++i) {
        g_streamBuffers[i - 1].next = &g_streamBuffers[i];
    }
    g_streamBuffers[ARAM_STREAM_BUFFER_COUNT - 1].next = NULL;
    g_aramStream = g_aramTop;
}

void aramInit(unsigned long length) {
    unsigned long base = ARGetBaseAddress();
    unsigned long cap = ARGetSize();

    g_aramBase = base;
    g_aramTop = base + length;
    if (g_aramTop > cap) {
        g_aramTop = cap;
    }

    if (g_aramTop < base + ARAM_ZERO_BUFFER_BYTES) {
        /* The original MUSY_ASSERT_MSG()s if `length` can't even hold the
         * zero buffer -- fatal on hardware.  HuAudInit always hands us
         * ~8 MB, so this should never trigger; if it somehow does, disable
         * the sample heap instead of letting g_aramWrite run past g_aramTop
         * below. */
        port_log("port> musyx_aram: aramInit(%lu) too small for the %lu-byte zero "
                 "buffer -- sample storage disabled\n",
                 length, ARAM_ZERO_BUFFER_BYTES);
        g_aramTop = base;
    }

    /* On Dolphin the CPU can't poke ARAM directly, so aramInit() builds a
     * temporary buffer of zero samples in main memory and DMAs it in.  Here
     * ARAM is just `port_aram()`, ordinary host memory, so a plain memset is
     * the exact same effect (a genuinely zeroed region nothing else will
     * ever allocate) without needing a scratch buffer or a fake round trip
     * through the DMA path. */
    memset((uint8_t*)port_aram() + g_aramBase, 0, ARAM_ZERO_BUFFER_BYTES);

    g_aramWrite = g_aramBase + ARAM_ZERO_BUFFER_BYTES;
    if (g_aramWrite > g_aramTop) {
        g_aramWrite = g_aramTop;
    }

    g_uploadCallback = NULL;
    g_uploadChunkSize = 0;

    InitStreamBuffers();

    g_bytesUploaded = 0;
    g_highWaterWrite = g_aramWrite;
    g_streamBuffersActive = 0;
    g_streamBuffersPeak = 0;
    g_storeRejected = 0;
    g_streamRejected = 0;
    g_uploadRejected = 0;
    g_removeMismatches = 0;
    g_initialized = 1;

    port_log("port> musyx_aram: region [0x%06lx, 0x%06lx) zero_buf [0x%06lx, 0x%06lx) "
             "sample heap starts at 0x%06lx\n",
             g_aramBase, g_aramTop, g_aramBase, g_aramBase + ARAM_ZERO_BUFFER_BYTES,
             g_aramWrite);
}

void aramExit(void) {
    /* The Dolphin original is a literal no-op: ARAM is memory-mapped
     * hardware and there is nothing to tear down.  We drop the initialized
     * flag so a stale port_musyx_aram_report() after shutdown reads as
     * empty instead of showing the last session's numbers; port_aram()
     * itself is owned by the platform layer and is not touched here. */
    g_initialized = 0;
}

unsigned long aramGetZeroBuffer(void) {
    /* The Dolphin original returns ARGetBaseAddress() -- the same address
     * aramInit() just zeroed above and that nothing else in this file will
     * ever hand out (aramWrite starts past it, and stream buffers only ever
     * grow down from aramTop). */
    return g_aramBase;
}

void aramSetUploadCallback(ARAMUploadCallback callback, unsigned long chunckSize) {
    unsigned long minChunk;

    if (callback != NULL) {
        chunckSize = ARAM_ALIGN32(chunckSize);
        minChunk = ARQGetChunkSize();
        g_uploadChunkSize = chunckSize < minChunk ? minChunk : chunckSize;
    }

    g_uploadCallback = callback;
}

void aramUploadData(void* mram, unsigned long aram, unsigned long len, unsigned long highPrio,
                    void (*callback)(size_t), unsigned long user) {
    (void)highPrio; /* no queue to prioritize: this always completes inline */

    if (len == 0) {
        if (callback != NULL) {
            callback((size_t)user);
        }
        return;
    }

    if (mram == NULL || aram > g_aramTop || len > g_aramTop - aram) {
        /* Never scribble past this heap's own region, and never call the
         * completion callback for a transfer that didn't happen -- MusyX's
         * stream/sample code takes that callback as "the bytes have landed",
         * so firing it here would be worse than dropping the request. */
        g_uploadRejected++;
        port_log("port> musyx_aram: aramUploadData refused: aram=0x%06lx len=%lu "
                 "top=0x%06lx mram=%p\n",
                 aram, len, g_aramTop, mram);
        return;
    }

    /* Route the actual copy through the port's existing DMA entry point
     * (port/src/audio/aram.c) so this heap shares the same bounds-checked
     * memcpy every other ARAM transfer in the port uses -- see the file
     * banner for why that's a backstop rather than the only check. */
    ARStartDMA(ARAM_DIR_MRAM_TO_ARAM, (uintptr_t)mram, (u32)aram, (u32)len);
    g_bytesUploaded += len;

    if (callback != NULL) {
        callback((size_t)user);
    }
}

void aramSyncTransferQueue(void) {
    /* aramUploadData() above already completes synchronously -- the memcpy
     * and the completion callback both happen before it returns, exactly
     * like ARQPostRequest in aram.c.  There is never anything left in
     * flight for this to wait on. */
}

void* aramStoreData(void* src, unsigned long len) {
    unsigned long addr;

    if (!g_initialized) {
        g_storeRejected++;
        return NULL;
    }

    len = ARAM_ALIGN32(len);
    if (len == 0) {
        return (void*)g_aramWrite;
    }

    if (len > g_aramStream - g_aramWrite) {
        /* Doesn't fit below the stream buffers growing down from the top.
         * The original MUSY_ASSERT_MSG()s here; a sample that can't be
         * loaded must come back as NULL, not corrupt the heap. */
        g_storeRejected++;
        port_log("port> musyx_aram: aramStoreData(%lu) refused -- %lu bytes free\n", len,
                 g_aramStream - g_aramWrite);
        return NULL;
    }

    addr = g_aramWrite;

    if (g_uploadCallback == NULL) {
        aramUploadData(src, g_aramWrite, len, 0, NULL, 0);
        g_aramWrite += len;
    } else {
        /* Chunked path: aramUploadCallback hands back a (possibly
         * transformed, e.g. decompressed) buffer for each block instead of
         * DMA'ing straight out of `src`. */
        unsigned char* cursor = (unsigned char*)src;
        unsigned long remaining = len;

        while (remaining != 0) {
            unsigned long blockSize = remaining >= g_uploadChunkSize ? g_uploadChunkSize : remaining;
            void* buffer;

            if ((uintptr_t)cursor > (uintptr_t)(u32)(uintptr_t)cursor) {
                /* ARAMUploadCallback's signature (void*(*)(u32, u32)) comes
                 * from extern/musyx/include/musyx/hardware.h and can't be
                 * changed from here; on a 64-bit host it truncates a source
                 * pointer above 4 GB.  Nothing in this build allocates game
                 * data that high, but flag it loudly rather than silently
                 * feeding the callback a garbage address. */
                port_log("port> musyx_aram: upload-callback source pointer %p truncates "
                         "through u32 -- refusing\n",
                         (void*)cursor);
                g_storeRejected++;
                return NULL;
            }

            buffer = g_uploadCallback((u32)(uintptr_t)cursor, (u32)blockSize);
            aramUploadData(buffer, g_aramWrite, blockSize, 0, NULL, 0);
            remaining -= blockSize;
            g_aramWrite += blockSize;
            cursor += blockSize;
        }
    }

    if (g_aramWrite > g_highWaterWrite) {
        g_highWaterWrite = g_aramWrite;
    }

    return (void*)addr;
}

void aramRemoveData(void* aram, unsigned long len) {
    unsigned long newWrite;
    unsigned long floorAddr = g_aramBase + ARAM_ZERO_BUFFER_BYTES;

    len = ARAM_ALIGN32(len);

    if (g_aramWrite < floorAddr || len > g_aramWrite - floorAddr) {
        /* Removing more than was ever stored would walk the write cursor
         * back into (or past) the reserved zero buffer.  The original
         * MUSY_ASSERT_MSG()s here; clamp instead so one bad call can't
         * corrupt the cursor for every store that follows it.
         *
         * On this target that is not a fault, it is the *normal* case, and
         * the asymmetry is worth naming once rather than warning about a
         * thousand times.  `hwSaveSample` in hardware.c calls `aramStoreData`
         * only inside `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN`, so MusyX
         * never uploads a sample through this heap here -- `src/msm` does its
         * own ARAM loading and `dataGetSample` hands MusyX addresses that
         * already point at it.  But `hwRemoveSample` calls `aramRemoveData`
         * *unconditionally*, so every group unload asks this heap to free
         * something it never allocated.  Nothing is corrupted: the cursor
         * clamps to the floor it is already sitting on.  The count is kept so
         * that if the sample heap ever does start being used, a genuine
         * underflow is still visible in the report. */
        if (!g_removeUnderflowNamed) {
            g_removeUnderflowNamed = 1;
            port_log("port> musyx_aram: aramRemoveData(%lu) with an empty sample heap.  "
                     "Expected: hwSaveSample only stores on the Dolphin target, so msm "
                     "owns sample upload and MusyX only ever frees.  Clamped; further "
                     "occurrences are counted, not printed.\n",
                     len);
        }
        g_removeMismatches++;
        g_aramWrite = floorAddr;
        return;
    }

    newWrite = g_aramWrite - len;
    if ((unsigned long)(uintptr_t)aram != newWrite) {
        /* aramStoreData/aramRemoveData are used as a stack: MusyX only ever
         * frees the most recently stored block first.  A mismatch means
         * that discipline was violated somewhere upstream; the Dolphin
         * original trusts the caller unconditionally and pops by length
         * regardless, so this does the same but counts the mismatch for
         * port_musyx_aram_report() instead of silently trusting it. */
        g_removeMismatches++;
    }

    g_aramWrite = newWrite;
}

u8 aramAllocateStreamBuffer(u32 len) {
    AramStreamBuffer* best = NULL;
    AramStreamBuffer* bestPrev = NULL;
    AramStreamBuffer* prev = NULL;
    AramStreamBuffer* sb;
    unsigned long minLen = (unsigned long)-1;
    unsigned long alignedLen = ARAM_ALIGN32(len);

    if (!g_initialized) {
        g_streamRejected++;
        return 0xFF;
    }

    /* First choice: reuse a same-size free buffer exactly, or otherwise the
     * smallest free buffer that's still big enough -- identical to the
     * Dolphin original's best-fit scan over aramFreeStreamBuffers. */
    for (sb = g_freeStreamBuffers; sb != NULL; sb = sb->next) {
        if (sb->allocLength == alignedLen) {
            best = sb;
            bestPrev = prev;
            break;
        }
        if (sb->allocLength > alignedLen && sb->allocLength < minLen) {
            best = sb;
            bestPrev = prev;
            minLen = sb->allocLength;
        }
        prev = sb;
    }

    if (best == NULL) {
        /* No free buffer fits: carve a new one off the idle list, growing
         * down from aramStream, as long as it still leaves room above
         * aramWrite (the sample heap growing up from the bottom). */
        if (g_idleStreamBuffers != NULL && alignedLen <= g_aramStream - g_aramWrite) {
            best = g_idleStreamBuffers;
            g_idleStreamBuffers = best->next;
            best->allocLength = alignedLen;
            best->length = alignedLen;
            g_aramStream -= alignedLen;
            best->aram = g_aramStream;
            best->next = g_usedStreamBuffers;
            g_usedStreamBuffers = best;
        }
    } else {
        if (bestPrev != NULL) {
            bestPrev->next = best->next;
        } else {
            g_freeStreamBuffers = best->next;
        }
        best->length = alignedLen;
        best->next = g_usedStreamBuffers;
        g_usedStreamBuffers = best;
    }

    if (best == NULL) {
        g_streamRejected++;
        port_log("port> musyx_aram: no stream buffer slots or ARAM for a %lu-byte "
                 "request\n",
                 alignedLen);
        return 0xFF;
    }

    g_streamBuffersActive++;
    if (g_streamBuffersActive > g_streamBuffersPeak) {
        g_streamBuffersPeak = g_streamBuffersActive;
    }

    return (u8)(best - g_streamBuffers);
}

size_t aramGetStreamBufferAddress(u8 id, size_t* len) {
    if (id >= ARAM_STREAM_BUFFER_COUNT) {
        /* The Dolphin original only MUSY_ASSERT_MSG()s that id != 0xFF (the
         * "allocation failed" sentinel) and otherwise trusts it -- fatal on
         * hardware if violated.  Treat any out-of-range id, 0xFF included,
         * as "no buffer" instead of indexing g_streamBuffers out of bounds. */
        if (len != NULL) {
            *len = 0;
        }
        return 0;
    }

    if (len != NULL) {
        *len = g_streamBuffers[id].length;
    }

    return g_streamBuffers[id].aram;
}

void aramFreeStreamBuffer(u8 id) {
    AramStreamBuffer* fsb;
    AramStreamBuffer* sb;
    AramStreamBuffer* lastSb;
    AramStreamBuffer* nextSb;
    unsigned long minAddr;

    if (id >= ARAM_STREAM_BUFFER_COUNT) {
        return;
    }

    fsb = &g_streamBuffers[id];
    lastSb = NULL;
    sb = g_usedStreamBuffers;
    while (sb != NULL) {
        if (sb == fsb) {
            if (lastSb != NULL) {
                lastSb->next = fsb->next;
            } else {
                g_usedStreamBuffers = fsb->next;
            }
            break;
        }
        lastSb = sb;
        sb = sb->next;
    }

    if (sb == NULL) {
        /* Not currently allocated -- a double free, or an id that was never
         * handed out.  The original has no such guard and would fall
         * through using fsb's stale fields; refusing instead keeps the
         * free/idle lists from being corrupted by it. */
        return;
    }

    if (g_streamBuffersActive > 0) {
        g_streamBuffersActive--;
    }

    if (fsb->aram == g_aramStream) {
        /* This buffer sits at the current top of the stream region: freeing
         * it can expose free-list buffers below it that are now also at the
         * top, so sweep for those too.  This block, including the inner
         * loop not advancing lastSb past a kept node, matches the Dolphin
         * original exactly -- see the file banner. */
        fsb->next = g_idleStreamBuffers;
        g_idleStreamBuffers = fsb;

        minAddr = (unsigned long)-1;
        sb = g_usedStreamBuffers;
        while (sb != NULL) {
            if (sb->aram <= minAddr) {
                minAddr = sb->aram;
            }
            sb = sb->next;
        }

        lastSb = NULL;
        sb = g_freeStreamBuffers;
        while (sb != NULL) {
            nextSb = sb->next;
            if (sb->aram < minAddr) {
                if (lastSb != NULL) {
                    lastSb->next = sb->next;
                } else {
                    g_freeStreamBuffers = sb->next;
                }
                sb->next = g_idleStreamBuffers;
                g_idleStreamBuffers = sb;
            }
            sb = nextSb;
        }

        g_aramStream = (minAddr != (unsigned long)-1) ? minAddr : g_aramTop;
        return;
    }

    fsb->next = g_freeStreamBuffers;
    g_freeStreamBuffers = fsb;
}

/* ---- diagnostics ----------------------------------------------------------
 *
 * Declared only here -- add the prototype to port.h to call this from
 * elsewhere (e.g. a debug key or a periodic report alongside the other
 * port_*_report() calls). */
void port_musyx_aram_report(void) {
    port_log("port> musyx_aram: region=[0x%06lx,0x%06lx) (%lu bytes) zero_buf=[0x%06lx,0x%06lx) "
             "initialized=%d\n",
             g_aramBase, g_aramTop, g_aramTop - g_aramBase, g_aramBase,
             g_aramBase + ARAM_ZERO_BUFFER_BYTES, g_initialized);
    port_log("port> musyx_aram: sample heap write=0x%06lx high_water=0x%06lx (%lu bytes at "
             "peak), stream cursor=0x%06lx\n",
             g_aramWrite, g_highWaterWrite,
             g_highWaterWrite - (g_aramBase + ARAM_ZERO_BUFFER_BYTES), g_aramStream);
    port_log("port> musyx_aram: stream buffers active=%lu peak=%lu of %u slots\n",
             g_streamBuffersActive, g_streamBuffersPeak, ARAM_STREAM_BUFFER_COUNT);
    port_log("port> musyx_aram: bytes uploaded=%lu -- rejected: store=%lu stream=%lu "
             "upload=%lu, remove mismatches=%lu\n",
             g_bytesUploaded, g_storeRejected, g_streamRejected, g_uploadRejected,
             g_removeMismatches);
}
