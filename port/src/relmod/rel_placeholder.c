/* The six placeholder modules.
 *
 * `m300Dll`, `m302Dll`, `m303Dll`, `m330Dll`, `m333Dll` and `msetupDll` are
 * 144-byte RELs on the disc holding nothing but empty `.ctors`/`.dtors`: no
 * code, no `_prolog`, no `_epilog`.  (Four of them share a SHA-1 in
 * config.yml.)  The port still builds a bundle for each so that all 99 load
 * and unload through one path and `--reltest` means something; this is what
 * goes inside.  A prolog that returns 0 is what the console's loader would
 * have done had the game ever entered one.
 */
#include "dolphin/types.h"

s32 _prolog(void);
void _epilog(void);

s32 _prolog(void) { return 0; }
void _epilog(void) {}
