/* THP movies: not decoded yet, and skipped in a way the game can survive.
 *
 * `src/dolphin/thp/THPDec.c` is 352 paired-single intrinsic sites of JPEG
 * inverse DCT -- the single densest concentration of Gekko-only code in the
 * whole tree (PLAN.md §1.12) -- and the plan's answer is not to port it: decode
 * THP with a small MJPEG decoder, or pre-transcode the movies at install time,
 * behind the same `THPSimple*` API.  That is M8 work.  Until then the SDK's THP
 * entry points are generated stubs and `THPSimpleOpen` returns 0.
 *
 * The trouble is what the game does about it.  `THPTestProc` in
 * src/game/thpmain.c retries the open **forever**:
 *
 *     while (THPSimpleOpen(THPFileName) == 0) {
 *         OSReport("THPSimpleOpen fail");
 *         HuPrcVSleep();
 *     }
 *
 * and `BootExec` waits on `while (!HuTHPEndCheck())`, where `HuTHPEndCheck`
 * asks `THPSimpleGetTotalFrame()`.  A stub returning 0 makes that
 * `temp_r31 = -1`, and the function's own `if (temp_r31 <= 0) return FALSE;`
 * means the movie is never over.  So the boot reaches the opening movie and
 * stops there, yielding politely at 60 fps, printing "THPSimpleOpen fail"
 * a few thousand times a second.  Two processes, both waiting on the other's
 * impossible condition.
 *
 * Rather than fake a movie -- a frame count the port would have to advance, a
 * decode buffer it would have to size, an audio track it would have to
 * pretend to mix -- the port says plainly that it cannot play movies, and two
 * exact-text patches in patches.txt ask it:
 *
 *   - `HuTHPEndCheck` returns TRUE at once, so every `while (!HuTHPEndCheck())`
 *     in the game falls straight through;
 *   - `THPTestProc` tears itself down the same way its own tail does -- kill
 *     the sprite or model it was drawing into, clear `THPProc` so a later
 *     `HuTHPSprCreateVol` still works, and `HuPrcKill` itself.
 *
 * The result is that a movie takes zero frames and leaves nothing behind, which
 * is exactly what `--skipmovie` would do if it were a flag.  It is not a flag
 * because there is nothing to choose between yet; when the decoder lands,
 * `thp_available` becomes the flag's home and both patches start letting the
 * real path run.  Every skip is named in the log and counted at shutdown, so a
 * missing cut-scene is never a silent difference from the console.
 */
#include "port.h"

#include <stdio.h>
#include <string.h>

#include <dolphin/types.h>

static int skipped;
static char first[64];

/* 0 while THP decode is unimplemented.  The whole of the port's movie policy
 * is this function's return value. */
int portTHPAvailable(void) { return 0; }

void portTHPSkip(const char* path) {
    if (!skipped) {
        snprintf(first, sizeof(first), "%s", path ? path : "?");
    }
    skipped++;
    port_log("port> THP: %s skipped -- movie decode is M8 (see port/src/dvd/thp_stub.c)\n",
             path ? path : "?");
}

void port_thp_report(void) {
    if (!skipped) {
        return;
    }
    port_log("port> THP: %d movie(s) skipped, first %s\n", skipped, first);
}
