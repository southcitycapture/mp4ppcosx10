/* Audio, silently: the --noaudio path.
 *
 * M6 is the real audio milestone -- a CPU implementation of the MusyX
 * `dspSlave` command set behind `extern/musyx`'s SAL (PLAN.md §1.8).  Until
 * then `msmSysInit` fails, and `HuAudInit` answers a failure the way a console
 * with a broken disc should: `while (1);`.  That is correct on the hardware
 * and useless in a port, so `--noaudio` lets the boot continue with every
 * sound call a no-op.
 *
 * On the development host the flag is implied, because the failure there is
 * not a missing SAL but the endianness of the machine: the GameCube, the G4
 * and the disc are all big endian, so `msmSysInit` reads the sound bank's
 * version field as 0x02000000 and rejects the file.  Nothing about that is
 * fixable by writing more audio code, and nothing about it applies to the G4.
 */
#include "port.h"

#include <dolphin/types.h>

static int announced;

static int host_is_little_endian(void) {
    const unsigned long one = 1;
    return *(const unsigned char*)&one != 0;
}

int portNoAudioContinue(void) {
    if (port_opt.noaudio) {
        if (!announced) {
            port_log("port> --noaudio: the game's sound manager failed to start; "
                     "continuing with silence\n");
            announced = 1;
        }
        return 1;
    }
    if (host_is_little_endian()) {
        if (!announced) {
            port_log("port> little-endian host: the game's own MSM parser cannot read "
                     "the disc's big-endian sound bank; continuing without audio "
                     "(pass --noaudio to say so deliberately)\n");
            announced = 1;
        }
        return 1;
    }
    port_log("port> the sound manager failed to start and --noaudio was not given; "
             "the game will now hang in HuAudInit exactly as it would on the console\n");
    return 0;
}
