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

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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
            "                    extracted files/ tree.  The game resolves all 138 data\n"
            "                    files at boot and panics on the first miss.  Without it,\n"
            "                    $MARIOPARTY4_IMAGE, then the .app's Contents/Resources,\n"
            "                    then ~/MarioParty4 are searched for an .iso or files/.\n"
            "  --log PATH        also write the OSReport narration to PATH\n"
            "  --frames N        stop after N retraces and print the stub report\n"
            "  --watchdog SEC    give up after SEC seconds and report where\n"
            "  --turbo           run the host loop flat out instead of at 60 Hz\n"
            "  --gxlog           log every GX call, not just the first of each\n"
            "  --stub-trace      log every stub call, not just the first of each\n"
            "  --deterministic   fixed 60 Hz tick and no wall-clock pacing\n"
            "  --seed N          the deterministic clock's origin: the RNG seed\n"
            "  --verbose         print each stub the first time it is called\n"
            "\n"
            "  --reldir DIR      where the 99 REL bundles live (default: <exe dir>/rels)\n"
            "  --reltest         load and unload all 99 REL bundles twice and report\n"
            "  --gxdemo          draw the GX self-test frame instead of the game\n"
            "  --relzerobss      always zero a module's bss by hand on load\n"
            "  --reldlclose      really dlclose a REL when the game unlinks it.  The\n"
            "                    port keeps it mapped by default, because the game\n"
            "                    calls into bootDll after unlinking it and the\n"
            "                    console's freed heap is still executable\n"
            "  --noaudio         HuAudInit and msm succeed as silent stubs\n"
            "  --headless        decode and log GX, but open no window\n"
            "  --glcheck         assert that no GL call leaves the GL 1.3 subset\n"
            "  --glinfo          dump the driver's GL strings, limits and extensions\n"
            "  --perf            per-frame game/gx/present timing, both clocks\n"
            "  --drawlog N       explain the first N draws: geometry, texture, state\n"
            "  --drawlog-at F    ...but only on presented frame F, which is how you\n"
            "  --scenelog F      the cameras and camera-bearing models on frame F\n"
            "  --ovllog          name the scene (omcurovl) every time it changes\n"
            "                    point --drawlog at a screen rather than at the boot\n"
            "  --nocard          both memory-card slots read empty.  The game then\n"
            "                    stops at SELECT A FILE, exactly as a console with no\n"
            "                    card does; without it slot A holds a 512 KB image in\n"
            "                    ~/Library/Application Support/MarioParty4/\n"
            "  --dumptex         write every decoded texture (colour + alpha) to shotdir\n"
            "  --texhash-full    hash whole textures on every bind (slow; a correctness check)\n"
            "  --gxwarn          name every GX feature the backend degraded\n"
            "  --dumpframe SPEC  write these presented frames as PPMs:\n"
            "                    N, or a,b,c, or first-last/step (e.g. 1-400/20)\n"
            "  --shotdir DIR     where --dumpframe writes (default: .)\n"
            "  --scale N         window scale over 640x480 (default 1)\n"
            "\n"
            "  --nopad           no controller 1 at all, not even the keyboard\n"
            "  --paddbg          log raw pad reports/buttons/axes as they arrive\n"
            "  --play SCRIPT     scripted controller 1 input (port/src/pad/pad_play.c\n"
            "                    format); see port/tools/gecko2play.py to convert a\n"
            "                    port/ref/tools/mkgecko.py reference script\n"
            "  --record FILE     record controller 1's raw input in the --play format\n",
            argv0);
}

/* Finding the disc without being told where it is.
 *
 * On the development Mac the image is always passed with --image.  On the G4
 * the game is launched by the isle-ppc-tools console runner, which hands the
 * bundle a fixed argument line, so the bundle has to be able to find its own
 * disc.  Three places are searched, in order, and the first `*.iso`/`*.nkit.iso`
 * or `files/` tree found wins:
 *
 *   1. $MARIOPARTY4_IMAGE          -- an explicit override, for scripts
 *   2. <the .app>/Contents/Resources  -- a self-contained bundle
 *   3. ~/MarioParty4               -- the shared folder, which survives
 *                                     replacing the .app (the same arrangement
 *                                     the Snowboard Kids ports use for ROMs)
 *
 * Nothing here is G4-specific in itself; it is just the only way a
 * double-clicked bundle can work on any Mac.
 */
static char default_image[1024];

static int dir_holds_disc(const char* dir) {
    DIR* d;
    struct dirent* e;
    struct stat st;
    char probe[1024];

    snprintf(probe, sizeof(probe), "%s/files", dir);
    if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(default_image, sizeof(default_image), "%s", dir);
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        const char* dot = strrchr(e->d_name, '.');
        if (dot && !strcasecmp(dot, ".iso")) {
            snprintf(default_image, sizeof(default_image), "%s/%s", dir, e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static const char* port_find_default_image(void) {
    const char* env = getenv("MARIOPARTY4_IMAGE");
    const char* home;
    char buf[1024];

    if (env && *env) {
        return env;
    }
#if defined(__APPLE__)
    {
        char exe[1024];
        uint32_t n = (uint32_t)sizeof(exe);
        if (_NSGetExecutablePath(exe, &n) == 0) {
            /* .../Foo.app/Contents/MacOS/isle -> .../Foo.app/Contents/Resources */
            char* slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                slash = strrchr(exe, '/'); /* strip MacOS */
                if (slash) {
                    *slash = '\0';
                    snprintf(buf, sizeof(buf), "%s/Resources", exe);
                    if (dir_holds_disc(buf)) {
                        return default_image;
                    }
                }
            }
        }
    }
#endif
    home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, sizeof(buf), "%s/MarioParty4", home);
        if (dir_holds_disc(buf)) {
            return default_image;
        }
    }
    return NULL;
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
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            /* --seed implies --deterministic: it *is* the deterministic
             * clock, started at a chosen reading.  See os_misc.c. */
            port_opt.seed = strtoll(argv[++i], NULL, 0);
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--reldir") && i + 1 < argc) {
            port_opt.reldir = argv[++i];
        } else if (!strcmp(a, "--gxdemo")) {
            port_opt.gxdemo = 1;
        } else if (!strcmp(a, "--reltest")) {
            port_opt.reltest = 1;
        } else if (!strcmp(a, "--relzerobss")) {
            port_opt.relzerobss = 1;
        } else if (!strcmp(a, "--noaudio")) {
            port_opt.noaudio = 1;
        } else if (!strcmp(a, "--glcheck")) {
            port_opt.glcheck = 1;
        } else if (!strcmp(a, "--glinfo")) {
            port_opt.glinfo = 1;
        } else if (!strcmp(a, "--gxwarn")) {
            port_opt.gxwarn = 1;
        } else if (!strcmp(a, "--perf")) {
            port_opt.perf = 1;
        } else if (!strcmp(a, "--drawlog") && i + 1 < argc) {
            port_opt.drawlog = atoi(argv[++i]);
        } else if (!strcmp(a, "--drawlog-at") && i + 1 < argc) {
            port_opt.drawlog_frame = atoi(argv[++i]);
        } else if (!strcmp(a, "--ovllog")) {
            port_opt.ovllog = 1;
        } else if (!strcmp(a, "--nanwatch")) {
            port_opt.nanwatch = 1;
        } else if (!strcmp(a, "--scenelog") && i + 1 < argc) {
            port_opt.scenelog = argv[++i];
        } else if (!strcmp(a, "--nocard")) {
            port_opt.nocard = 1;
        } else if (!strcmp(a, "--reldlclose")) {
            port_opt.reldlclose = 1;
        } else if (!strcmp(a, "--dumptex")) {
            port_opt.dumptex = 1;
        } else if (!strcmp(a, "--texhash-full")) {
            port_opt.texhash_full = 1;
        } else if (!strcmp(a, "--headless")) {
            port_opt.headless = 1;
        } else if (!strcmp(a, "--dumpframe") && i + 1 < argc) {
            port_opt.dumpframe = argv[++i];
        } else if (!strcmp(a, "--shotdir") && i + 1 < argc) {
            port_opt.shotdir = argv[++i];
        } else if (!strcmp(a, "--scale") && i + 1 < argc) {
            port_opt.scale = atoi(argv[++i]);
        } else if (!strcmp(a, "--nopad")) {
            port_opt.nopad = 1;
        } else if (!strcmp(a, "--paddbg")) {
            port_opt.pad_debug = 1;
        } else if (!strcmp(a, "--play") && i + 1 < argc) {
            port_opt.pad_play = argv[++i];
        } else if (!strcmp(a, "--record") && i + 1 < argc) {
            port_opt.pad_record = argv[++i];
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

void port_gx_shutdown(void);
void port_gx_demo(void);
void gl13_write_ppm(const char* path);
void GXInit_demo_bootstrap(void);

/* One exit path, so a quit through the game's own reset, through --frames and
 * through the end of main() all report the same things in the same order. */
void port_shutdown(int code) {
    port_perf_report();
    port_clock_report();
    port_gx_shutdown();
    port_dvd_stats();
    port_thp_report();
    port_card_report();
    port_dll_report();
    port_reset_report();
    port_stub_report();
    exit(code);
}

static void run_game(void) {
    port_clock_mark();
    port_log("port> entering the game's own main()\n\n");
    mp4_game_main();
    port_log("\nport> the game's main() returned\n");
    port_shutdown(0);
}

int main(int argc, char** argv) {
    if (!port_parse_args(argc, argv)) {
        return 1;
    }
    if (!port_opt.image) {
        port_opt.image = port_find_default_image();
    }
    port_log_open(port_opt.log);
    port_crash_handler_install();
    if (port_opt.watchdog) {
        port_watchdog_arm(port_opt.watchdog);
    }
    port_log("Mario Party 4 -- native port, milestone M2a\n");
    if (port_opt.reltest) {
        port_opt.reldlclose = 1; /* the self-test is *about* the unload path */
        return port_dll_selftest();
    }
    if (port_opt.gxdemo) {
        char path[1024];
        port_mem_init();
        port_vi_init();
        port_gx_init();
        GXInit_demo_bootstrap();
        port_gx_demo();
        snprintf(path, sizeof(path), "%s/gxdemo.ppm",
                 port_opt.shotdir ? port_opt.shotdir : ".");
        gl13_write_ppm(path);
        port_gx_present();
        port_shutdown(0);
    }
    port_reset_init();
    port_mem_init();
    port_vi_init();
    port_dvd_init();
    port_gx_init();
    port_call_on_stack(run_game, port_game_stack_top());
    return 0;
}
