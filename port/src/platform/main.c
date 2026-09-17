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
void gx_tex_set_validate_every_bind(int v);

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
            "  --rtc SECS        the same origin written as the console's real-time\n"
            "                    clock, in Unix seconds -- the number Dolphin calls\n"
            "                    CustomRTCValue.  `--rtc dolphin' is the pinned\n"
            "  --rtcoffset SECS  shift that origin, so the port reaches\n"
            "                    BoardRandInit at the console's reading rather\n"
            "                    than 300 frames earlier.  1 frame = 1/60 s.\n"
            "  --memmap          print the MEM1/ARAM/stack map and its guards\n"
            "  --guardtest WHERE mem1-hi|mem1-lo|aram-hi|aram-lo|stack-lo: write\n"
            "                    one byte past that edge and expect a fault\n"
            "                    reference value (1041472800, 2003-01-02).  Implies\n"
            "                    --deterministic, and both of the game's RNG seed\n"
            "                    sites read it through OSGetTime\n"
            "  --card FILE       use FILE as slot A's 512 KB card image\n"
            "  --freshcard       format the card image at boot, so the save file's\n"
            "                    played-minigame set does not carry between runs.\n"
            "                    With no --card it formats a scratch image, never\n"
            "                    the player's own save file\n"
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
            "  --wav FILE        capture the 32 kHz stereo mix to a WAV file\n"
            "  --mute            mix and time it as usual; emit silence\n"
            "  --audiolog        narrate MusyX stream, voice and studio events\n"
            "  --headless        decode and log GX, but open no window\n"
            "  --glcheck         assert that no GL call leaves the GL 1.3 subset\n"
            "  --glinfo          dump the driver's GL strings, limits and extensions\n"
            "  --vprobe          dump the ARB_vertex_program limits and a trivial load\n"
            "  --cpuxf           phase 2 on the CPU (the pre-M11 path), for the A/B\n"
            "  --vprogstats      GPU-path vs CPU-fallback draws, and why a variant died\n"
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
            "  --texhash-full    hash whole textures on every bind, bypassing the\n"
            "                    validation epoch entirely (slow; a correctness check)\n"
            "  --texvalidate-every-bind\n"
            "                    keep the sampled content hash but check it on every\n"
            "                    bind instead of once per validation epoch -- for\n"
            "                    telling an epoch bug from a sampling bug\n"
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
            "  --record FILE     record controller 1's raw input in the --play format\n"
            "\n"
            "  self-play (M7):\n"
            "  --minigame N|NAME park the minigame roulette on this minigame; a\n"
            "                    comma-separated list parks on each in turn, moving\n"
            "                    on when the one in force has been played, and\n"
            "                    releasing the roulette when the list is done\n"
            "  --com4            all four players are CPU\n"
            "  --turns N         the board's turn count (10/20/30/50)\n"
            "  --status          one state line a second: screen, turn, minigame,\n"
            "                    coins and stars per player, aud ms, fps\n"
            "  --stuckwatch SEC  name the live screen if it has not changed in SEC\n"
            "  --soak            boot, walk into a four-CPU board, play it, and\n"
            "                    start another one when it ends, forever\n"
            "  --nodepop         switch off the voice cut-off ramp\n"
            "  --resample4       the 4-tap Catmull-Rom resampler instead of the\n"
            "                    default linear interpolation (PLAN.md 20.5)\n"
            "  --resample1       linear interpolation; the default, kept so that an\n"
            "                    A/B can name both sides\n"
            "  --clickstat       count mix discontinuities as they are produced\n"
            "  --noaicb          do not call the game's AI DMA callback, which\n"
            "                    leaves msmSe/Mus/StreamPeriodicProc dead as\n"
            "                    the stub did before M9b (PLAN.md 22.4)\n"
            "  --olddecode       decode display-list vertices with the old\n"
            "                    call-per-attribute cursor instead of the\n"
            "                    per-primitive plan (PLAN.md 21.4); for A/B\n"
            "  --dlcache         replay a display list's cached decoded vertices\n"
            "                    when its bytes and the arrays it indexes have\n"
            "                    not moved.  Off: it is faster on the title and\n"
            "                    slower on the character select (PLAN.md 21.3)\n"
            "  teleport to the bug (M10):\n"
            "  --nodraw          consume the game's GX command streams and emit\n"
            "                    no GL: no vertex decode, no texture decode, no\n"
            "                    present.  The game logic reads none of it, so\n"
            "                    the run is the same run, roughly three times\n"
            "                    faster.  The window keeps the last frame drawn\n"
            "  --ffto N          --nodraw until frame N, then switch drawing on\n"
            "                    and carry on at the normal pace: the way to be\n"
            "                    at frame N of a deterministic run in a fraction\n"
            "                    of the time.  --dumpframe N then still writes\n"
            "                    the byte-identical frame\n"
            "  --ffto-warm K     render K frames before N so the texture cache\n"
            "                    and the EFB are warm when N is drawn (default 1)\n"
            "  --snap-every K    write a snapshot every K frames\n"
            "  --snap-keep N     keep the newest N snapshots (default 3)\n"
            "  --snap-at N       one snapshot at frame N\n"
            "  --snap-dir DIR    where the ring lives (default ~/MarioParty4/snaps)\n"
            "  --restore FILE    resume the run in FILE: same binary, same arena\n"
            "                    addresses, same modules.  Every other flag on\n"
            "                    the line still applies, so a restore can be\n"
            "                    --dumpframe'd or run under gdb\n"
            "  --snapdiff        print a per-region digest of the arenas every\n"
            "                    snapshot, to byte-diff a restored run against a\n"
            "                    straight one at the same frame\n"
            "\n"
            "  --perfwin SPEC    with --perf, also report fps over named frame\n"
            "                    windows: A-B[:NAME][,A-B[:NAME]...].  One boot\n"
            "                    then answers \"how fast is the title/the menu/the\n"
            "                    board\" instead of three\n",
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
    /* The two audio repairs are on by default and switched *off* by a flag, so
     * that every unadorned run -- including every run of the self-play soak --
     * exercises them, and an A/B is one word on the command line. */
    port_opt.depop = 1;
    /* Linear, since the G4 measured both on the same walk (PLAN.md §20.5):
     * 1.75 ms mean against the 4-tap's 2.00, a worst frame of 10.92 ms against
     * 28.30, and fewer discontinuities, not more -- 33,002 against 36,328.
     * The 4-tap was there to buy quality and on this hardware it buys none, so
     * it is the flag now and linear is the default. */
    port_opt.resample4 = 0;
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
        } else if (!strcmp(a, "--rtc") && i + 1 < argc) {
            /* Dolphin pins CustomRTCValue and the game seeds both of its RNGs
             * from OSGetTime; --rtc is the same number on this side, converted
             * to the console's 40.5 MHz ticks since 2000-01-01.  It is --seed
             * in the units the reference rig is configured in. */
            const char* v = argv[++i];
            port_opt.rtc = !strcmp(v, "dolphin") ? PORT_RTC_DOLPHIN
                                                 : strtoll(v, NULL, 0);
            port_opt.rtc_seen = 1;
            port_opt.deterministic = 1;
        } else if (!strcmp(a, "--rtcoffset") && i + 1 < argc) {
            /* §17.3: --rtc gives the two rigs the same clock *origin* and they
             * still deal different minigames, because `BoardRandInit`
             * (board/main.c:1432) seeds from OSGetTime at board setup rather
             * than at boot, and the port arrives there some 300 frames earlier
             * than the console -- it skips the DVD seek and the opening movie.
             * This is that difference, in seconds, added to the origin.  One
             * frame is 1/60 s, so the 300-frame gap is --rtcoffset 5.0; the
             * exact number is a subtraction between the two rigs' --ovllog
             * timestamps at the board's first frame. */
            port_opt.rtc_offset = strtod(argv[++i], NULL);
        } else if (!strcmp(a, "--card") && i + 1 < argc) {
            port_opt.card = argv[++i];
        } else if (!strcmp(a, "--freshcard")) {
            port_opt.freshcard = 1;
        } else if (!strcmp(a, "--minigame") && i + 1 < argc) {
            port_opt.minigame = argv[++i];
        } else if (!strcmp(a, "--com4")) {
            port_opt.com4 = 1;
        } else if (!strcmp(a, "--turns") && i + 1 < argc) {
            port_opt.turns = atoi(argv[++i]);
        } else if (!strcmp(a, "--status")) {
            port_opt.status = 1;
        } else if (!strcmp(a, "--stuckwatch") && i + 1 < argc) {
            port_opt.stuckwatch = atoi(argv[++i]);
        } else if (!strcmp(a, "--soak")) {
            port_opt.soak = 1;
            port_opt.com4 = 1;
            port_opt.status = 1;
            if (!port_opt.stuckwatch) {
                port_opt.stuckwatch = 90;
            }
        } else if (!strcmp(a, "--nodepop")) {
            port_opt.depop = 0;
        } else if (!strcmp(a, "--resample1")) {
            port_opt.resample4 = 0;
        } else if (!strcmp(a, "--resample4")) {
            port_opt.resample4 = 1;
        } else if (!strcmp(a, "--dlcache")) {
            port_opt.dlcache = 1;
        } else if (!strcmp(a, "--olddecode")) {
            port_opt.olddecode = 1;
        } else if (!strcmp(a, "--noaicb")) {
            port_opt.noaicb = 1;
        } else if (!strcmp(a, "--perfwin") && i + 1 < argc) {
            port_opt.perfwin = argv[++i];
            port_opt.perf = 1;
        } else if (!strcmp(a, "--nodraw")) {
            port_opt.nodraw = 1;
        } else if (!strcmp(a, "--ffto") && i + 1 < argc) {
            port_opt.ffto = atoi(argv[++i]);
        } else if (!strcmp(a, "--ffto-warm") && i + 1 < argc) {
            port_opt.ffto_warm = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-every") && i + 1 < argc) {
            port_opt.snap_every = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-keep") && i + 1 < argc) {
            port_opt.snap_keep = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-at") && i + 1 < argc) {
            port_opt.snap_now = atoi(argv[++i]);
        } else if (!strcmp(a, "--snap-dir") && i + 1 < argc) {
            port_opt.snap_dir = argv[++i];
        } else if (!strcmp(a, "--restore") && i + 1 < argc) {
            port_opt.restore = argv[++i];
        } else if (!strcmp(a, "--snapdiff")) {
            port_opt.snapdiff = 1;
        } else if (!strcmp(a, "--clickstat")) {
            port_opt.clickstat = 1;
        } else if (!strcmp(a, "--reldir") && i + 1 < argc) {
            port_opt.reldir = argv[++i];
        } else if (!strcmp(a, "--gxdemo")) {
            port_opt.gxdemo = 1;
        } else if (!strcmp(a, "--reltest")) {
            port_opt.reltest = 1;
        } else if (!strcmp(a, "--guardtest") && i + 1 < argc) {
            port_opt.guardtest = argv[++i];
        } else if (!strcmp(a, "--memmap")) {
            port_opt.memmap = 1;
        } else if (!strcmp(a, "--relzerobss")) {
            port_opt.relzerobss = 1;
        } else if (!strcmp(a, "--noaudio")) {
            port_opt.noaudio = 1;
        } else if (!strcmp(a, "--wav") && i + 1 < argc) {
            port_opt.wav = argv[++i];
        } else if (!strcmp(a, "--mute")) {
            port_opt.mute = 1;
        } else if (!strcmp(a, "--audiolog")) {
            port_opt.audiolog = 1;
        } else if (!strcmp(a, "--glcheck")) {
            port_opt.glcheck = 1;
        } else if (!strcmp(a, "--glinfo")) {
            port_opt.glinfo = 1;
        } else if (!strcmp(a, "--vprobe")) {
            port_opt.vprobe = 1;
        } else if (!strcmp(a, "--cpuxf")) {
            port_opt.cpuxf = 1;
        } else if (!strcmp(a, "--vprogstats")) {
            port_opt.vprogstats = 1;
        } else if (!strcmp(a, "--vproglog")) {
            port_opt.vproglog = 1;
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
        } else if (!strcmp(a, "--texvalidate-every-bind")) {
            gx_tex_set_validate_every_bind(1);
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
    /* --soak implies the menu walk.
     *
     * `--soak` alone boots into the attract loop and stays there: nothing
     * presses Start, so the board it is supposed to soak never begins.  That
     * is not a hypothetical -- it cost the 2026-09-14 overnight run seven
     * hours of title screen and 280 STUCK lines (PLAN.md 21.8), and it is an
     * easy mistake to make because every other soak flag is self-contained.
     * The walk is shipped in the bundle next to the disc image, so naming it
     * here costs nothing and there is no case where a soak wants the attract
     * loop instead. An explicit --play still wins. */
    if (port_opt.soak && port_opt.pad_play == NULL) {
        port_opt.pad_play = "board-start-com4.play";
        fprintf(stderr, "port> --soak implies --play %s (the menu walk); "
                        "pass --play explicitly to override\n",
                port_opt.pad_play);
    }

    /* M10 defaults, after the loop so the flags may come in any order.
     * --ffto *is* --nodraw with an end: one frame of warm-up before N, because
     * the texture cache is flushed when drawing comes back on and the first
     * drawn frame has to re-upload what it binds (PLAN.md 24.1). */
    if (port_opt.ffto) {
        port_opt.nodraw = 1;
    }
    if (port_opt.ffto_warm <= 0) {
        port_opt.ffto_warm = 1;
    }
    if (port_opt.snap_keep <= 0) {
        port_opt.snap_keep = 3;
    }

    /* After the loop, so --rtc and --rtcoffset may be given in either order. */
    if (port_opt.rtc_seen) {
        port_opt.rtc_set = 1;
        port_opt.seed =
            (long long)((double)(port_opt.rtc - PORT_GC_EPOCH_UNIX) *
                            (double)PORT_TIMER_CLOCK +
                        port_opt.rtc_offset * (double)PORT_TIMER_CLOCK);
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
    port_audio_shutdown(); /* first: it closes the WAV, which must be complete */
    port_perf_report();
    port_audio_report();
    port_clock_report();
    port_gx_shutdown();
    port_dvd_stats();
    port_thp_report();
    port_card_report();
    port_dll_report();
    port_snap_report();
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
    if (port_opt.memmap || port_opt.guardtest) {
        port_mem_regions_dump();
    }
    if (port_opt.guardtest) {
        port_guard_selftest(port_opt.guardtest);
        port_shutdown(0);
    }
    port_vi_init();
    port_dvd_init();
    port_gx_init();
    /* After port_gx_init, which is what brings SDL up.  --noaudio keeps the
     * whole path switched off, including the tick, so the boot behaves exactly
     * as it did before M6 -- which is what makes an audio regression bisectable
     * against a silent run of the same seed. */
    if (port_opt.noaudio) {
        port_audio_enabled = 0;
    } else {
        port_audio_out_init();
        if (port_opt.wav) {
            port_audio_wav_start(port_opt.wav);
        }
    }
    port_selfplay_init();
    /* After every subsystem, because --ffto asks the GX backend to switch the
     * renderer off and the snapshot registry has to see the buffers the audio
     * and card layers just allocated. */
    port_ffto_init();
    port_snap_init();
    if (port_snap_restore_pending()) {
        /* Does not return: it copies the snapshot over this process's arenas,
         * globals and modules and longjmps into the saved retrace.  The boot
         * above was only ever there to build the host side -- window, GL
         * context, audio device, disc, pad -- that a snapshot deliberately
         * does not carry. */
        port_snap_restore();
    }
    port_call_on_stack(run_game, port_game_stack_top());
    return 0;
}
