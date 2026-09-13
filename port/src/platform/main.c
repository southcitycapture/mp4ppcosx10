/* The port's entry point.
 *
 * Bring up MEM1, ARAM, the disc and VI, then hand control to the game's own
 * `main()` -- renamed to `mp4_game_main` by -Dmain=mp4_game_main so the host's
 * entry point can keep the name -- and never come back: the game's frame loop
 * is `while (1)`, and the port leaves it through OSPanic, OSResetSystem or
 * --frames.
 *
 * The game runs on a stack the port allocated next to MEM1, not on the host's
 * thread stack, so that every stack pointer it truncates into a u32 lives in
 * one known 4 GB window.  See port/src/os/jmp_arm64.s.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PortOptions port_opt;

void port_log_open(const char* path);
void* port_game_stack_top(void);
void port_call_on_stack(void (*fn)(void), void* stack_top);
void port_dvd_stats(void);
void port_crash_handler_install(void);
void port_watchdog_arm(int seconds);

static void usage(const char* argv0) {
    fprintf(stderr,
            "Mario Party 4, native port (milestone M1: it links and it talks)\n"
            "\n"
            "usage: %s --image <disc.iso | dir> [options]\n"
            "\n"
            "  --image PATH      the user's own disc image, or a directory holding an\n"
            "                    extracted files/ tree.  Required: the game resolves all\n"
            "                    138 data files at boot and panics on the first miss.\n"
            "  --log PATH        also write the OSReport narration to PATH\n"
            "  --frames N        stop after N retraces and print the stub report\n"
            "  --watchdog SEC    give up after SEC seconds and report where\n"
            "  --turbo           run the host loop flat out instead of at 60 Hz\n"
            "  --gxlog           log every GX call, not just the first of each\n"
            "  --stub-trace      log every stub call, not just the first of each\n"
            "  --deterministic   fixed 60 Hz tick and no wall-clock pacing\n"
            "  --verbose         print each stub the first time it is called\n"
            "\n"
            "  --reldir DIR      where the 99 REL bundles live (default: <exe dir>/rels)\n"
            "  --reltest         load and unload all 99 REL bundles twice and report\n"
            "  --relzerobss      always zero a module's bss by hand on load\n"
            "  --noaudio         HuAudInit and msm succeed as silent stubs\n"
            "  --headless        decode and log GX, but open no window\n"
            "  --glcheck         assert that no GL call leaves the GL 1.3 subset\n"
            "  --gxwarn          name every GX feature the backend degraded\n"
            "  --dumpframe N     write the presented frame N as a PPM\n"
            "  --shotdir DIR     where --dumpframe writes (default: .)\n"
            "  --scale N         window scale over 640x480 (default 1)\n",
            argv0);
}

int port_parse_args(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--image") && i + 1 < argc) {
            port_opt.image = argv[++i];
        } else if (!strcmp(a, "--log") && i + 1 < argc) {
            port_opt.log = argv[++i];
        } else if (!strcmp(a, "--watchdog") && i + 1 < argc) {
            port_opt.watchdog = atoi(argv[++i]);
        } else if (!strcmp(a, "--frames") && i + 1 < argc) {
            port_opt.max_frames = atoi(argv[++i]);
        } else if (!strcmp(a, "--turbo")) {
            port_opt.turbo = 1;
        } else if (!strcmp(a, "--gxlog")) {
            port_opt.gxlog = 1;
        } else if (!strcmp(a, "--stub-trace")) {
            port_opt.stub_trace = 1;
        } else if (!strcmp(a, "--deterministic")) {
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--reldir") && i + 1 < argc) {
            port_opt.reldir = argv[++i];
        } else if (!strcmp(a, "--reltest")) {
            port_opt.reltest = 1;
        } else if (!strcmp(a, "--relzerobss")) {
            port_opt.relzerobss = 1;
        } else if (!strcmp(a, "--noaudio")) {
            port_opt.noaudio = 1;
        } else if (!strcmp(a, "--glcheck")) {
            port_opt.glcheck = 1;
        } else if (!strcmp(a, "--gxwarn")) {
            port_opt.gxwarn = 1;
        } else if (!strcmp(a, "--headless")) {
            port_opt.headless = 1;
        } else if (!strcmp(a, "--dumpframe") && i + 1 < argc) {
            port_opt.dumpframe = atoi(argv[++i]);
        } else if (!strcmp(a, "--shotdir") && i + 1 < argc) {
            port_opt.shotdir = argv[++i];
        } else if (!strcmp(a, "--scale") && i + 1 < argc) {
            port_opt.scale = atoi(argv[++i]);
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            port_opt.verbose = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

static void run_game(void) {
    port_log("port> entering the game's own main()\n\n");
    mp4_game_main();
    port_log("\nport> the game's main() returned\n");
    port_dvd_stats();
    port_dll_report();
    port_stub_report();
    exit(0);
}

int main(int argc, char** argv) {
    if (!port_parse_args(argc, argv)) {
        return 1;
    }
    port_log_open(port_opt.log);
    port_crash_handler_install();
    if (port_opt.watchdog) {
        port_watchdog_arm(port_opt.watchdog);
    }
    port_log("Mario Party 4 -- native port, milestone M2a\n");
    if (port_opt.reltest) {
        return port_dll_selftest();
    }
    port_mem_init();
    port_vi_init();
    port_dvd_init();
    port_gx_init();
    port_call_on_stack(run_game, port_game_stack_top());
    return 0;
}
