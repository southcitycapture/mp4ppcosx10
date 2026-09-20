/* The self-play harness: --com4, --minigame, --turns, --status, --soak.
 *
 * The model is the two Snowboard Kids ports' `menu_nav.c`, and the rule it
 * taught is the whole design of this file:
 *
 *     name the live screen, and park the state the game would have set,
 *     rather than pressing the buttons that would have set it.
 *
 * Pressing buttons is what `--play` does, and `port/ref/movies/board-start.play`
 * shows what it costs: a 300-line metronome, five captures to place two
 * STARTs, and a walk that breaks the moment a screen's entry animation
 * changes length.  Parking state costs a line per fact and does not care how
 * long an animation is.
 *
 * Everything here writes the game's own globals, which the port links
 * statically -- `GWPlayerCfg`, `GWPlayer`, `GWSystem`, `GWGameStat`.  Nothing
 * in `src/` is patched: PLAN.md's rule is that game-source changes go through
 * the mirror and `port/patches.txt`, and none was needed.
 *
 * What each flag parks, and where the game would otherwise have set it:
 *
 *   --com4      `GWPlayerCfg[i].iscom = 1` for all four, mirrored into
 *               `GWPlayer[i].com`.  mentDll's own attract path does exactly
 *               this (src/REL/mentDll/main.c:824-834), which is the evidence
 *               that all-CPU is a state the game supports rather than one the
 *               harness invents.  It is also what makes the minigame
 *               instruction screen dismiss itself: instDll counts the CPU
 *               players and auto-starts after 60 frames when the count is
 *               four (src/REL/instDll/main.c:281-294), so a soak needs no
 *               START at all.
 *
 *   --turns N   `GWSystem.max_turn`, which `BoardTurnNext` compares
 *               `GWSystem.turn` against (src/game/board/main.c:558).
 *
 *   --minigame  `GWSystem.mg_next`, the index into `mgInfoTbl` that instDll
 *               reads in its ObjectSetup (src/REL/instDll/main.c:60) and
 *               launches at :392.  The roulette is left to run and its answer
 *               is overwritten, which is the one intervention that is both
 *               reliable and safe:
 *
 *                 * biasing the *candidate pool* instead (GWSystem.mg_list = 2
 *                   plus a one-entry GWGameStat.mg_custom) looks cleaner and
 *                   hangs the game -- DetermineMGList (mg_setup.c:352-376)
 *                   loops until it has N *distinct* candidates and there is
 *                   no exit if the pool is smaller than N;
 *                 * calling omOvlCallEx from the retrace gate would re-enter
 *                   the object manager from outside any HUPROCESS.
 *
 *               The team split the chosen minigame needs is parked with it:
 *               `GWPlayerCfg[i].group` exactly as E3setupDLL/mgselect.c:219-250
 *               assigns it, because a 2-vs-2 module reads the groups and m425
 *               is a 2-vs-2.
 *
 *   --status    one line a second, from the same globals.
 *   --stuckwatch SEC  the live screen has not changed in SEC seconds: say
 *               which one it is, by name, and what the overlay stack looks
 *               like.  The N64 ports used dladdr for this and paid 27% for it
 *               (PLAN.md §15.5's lesson, and memory of the SBK port); here
 *               `omcurovl` is an integer and `_ovltbl` is a name table, so the
 *               watchdog costs one compare a frame.
 *
 *   --soak      the above, plus: keep the board's turn counter from ever
 *               ending the session for good -- when a board finishes, the
 *               game returns to the mode-select menus, and the same `--play`
 *               metronome that walked in the first time walks in again.  The
 *               harness's own contribution is that no screen needs a
 *               *specific* button any more, so the metronome is sufficient.
 */
#include "port.h"

#include "game/gamework_data.h"
#include "game/pad.h"
#include "game/object.h"
#include "game/objsub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp, for --cast's names */

/* Overlay id -> name, built from the same header the game's own `_ovltbl`
 * is built from, so the two cannot drift. */
#define DLL(name) #name,
/* the --play script's activity, for the title guard below */
u32 pad_play_press_count(void);

static const char* const ovl_name[] = {
#include "ovl_table.h"
    NULL
};
#undef DLL
#define OVL_COUNT ((int)(sizeof(ovl_name) / sizeof(ovl_name[0])) - 1)

static const char* screen_name(int ovl) {
    if (ovl < 0 || ovl >= OVL_COUNT) {
        return "(none)";
    }
    return ovl_name[ovl];
}

/* ---- --minigame ------------------------------------------------------------
 *
 * The argument is either an mgInfoTbl index, a minigame number (401..463, the
 * numbering every debug label in the game uses), or a module name
 * ("m425dll"), because all three appear in this project's own notes and there
 * is no reason to make the caller convert.
 */
static int forced_mg = -1; /* mgInfoTbl index */
static int forced_mg_type;

/* A comma-separated argument -- `--minigame m453,m443,m428,m449` -- parks the
 * roulette on each in turn, advancing when the one in force has been played.
 * The reason is arithmetic: on the G4 a boot into a board costs about eight
 * minutes and a turn about twelve, so four modules witnessed one command each
 * is two hours and four modules witnessed in one board is one.  A single name
 * behaves exactly as before. */
#define FORCED_MG_MAX 16
static int forced_mg_list[FORCED_MG_MAX];
static int forced_mg_len;
static int forced_mg_at;
/* The list may not move on the frame the minigame ends.  `resultDll` decides
 * which directory image to free by reading `GWSystem.mg_next` at its own
 * `ObjectSetup` (src/REL/resultDll/main.c:102,109) -- and instDll's untagged
 * preload of the minigame that just finished is exactly what it is there to
 * free (PLAN.md 27.3).  Advance the moment `omcurovl` leaves the minigame and
 * the parked value has already become the *next* name, so resultDll closes the
 * next minigame's directory and leaks the one that just played; two or three
 * of those exhaust HEAP_DVD.  So leaving the minigame only arms the advance,
 * and the advance happens when `resultDll` itself is left -- after it has read
 * mg_next, before instDll preloads from it.  PLAN.md 28.4. */
static int forced_mg_pending;

static int mg_index_from_arg(const char* a) {
    int i;
    char want[32];

    /* a module name, with or without the "dll" */
    if ((a[0] == 'm' || a[0] == 'M') && a[1] >= '0' && a[1] <= '9') {
        int n = atoi(a + 1);
        for (i = 0; i < OVL_COUNT; i++) {
            snprintf(want, sizeof(want), "m%ddll", n);
            if (!strcasecmp(ovl_name[i], want)) {
                int idx = omMgIndexGet((s16)i);
                if (idx >= 0) {
                    return idx;
                }
            }
        }
        /* fall through: treat m425 as the number 425 */
        a = a + 1;
    }
    {
        int n = atoi(a);
        if (n >= 0x191) {
            return n - 0x191; /* 401 -> index 0 */
        }
        return n; /* already an index */
    }
}

/* Take entry `n` of the list into force.  Returns 0 if there is no such entry
 * or it does not name a minigame. */
static int forced_mg_select(int n) {
    int idx;
    if (n < 0 || n >= forced_mg_len) {
        return 0;
    }
    idx = forced_mg_list[n];
    if (idx < 0 || idx >= 64 || mgInfoTbl[idx].ovl == 0xFFFF) {
        return 0;
    }
    forced_mg_at = n;
    forced_mg = idx;
    forced_mg_type = mgInfoTbl[idx].type;
    return 1;
}

/* ---- parking ---------------------------------------------------------------- */

/* --cast: which four characters a --com4 run plays.
 *
 * `--com4` on its own parks character `i` on player `i`, which is
 * Mario/Luigi/Peach/Yoshi -- and that cast is 221,376 bytes too heavy for
 * `HEAP_DVD` in m453, on the retail disc as much as here (PLAN.md 27.5).
 * Naming the cast is what lets a run ask whether a lighter four fits.
 * The eight are `resultCharMdlTbl`'s order, src/REL/resultDll/main.c:1039. */
static const char* const CAST_NAMES[8] = { "mario", "luigi",  "peach", "yoshi",
                                           "wario", "donkey", "daisy", "waluigi" };
static s16 cast_char[4] = { -1, -1, -1, -1 };

static void cast_parse(void) {
    const char* p = port_opt.cast;
    int n = 0;
    if (!p) {
        return;
    }
    while (*p && n < 4) {
        const char* start = p;
        size_t len;
        int j, got = -1;
        while (*p && *p != ',') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 1 && start[0] >= '0' && start[0] <= '7') {
            got = start[0] - '0';
        } else {
            for (j = 0; j < 8; j++) {
                if (strlen(CAST_NAMES[j]) == len &&
                    strncasecmp(CAST_NAMES[j], start, len) == 0) {
                    got = j;
                    break;
                }
            }
        }
        if (got < 0) {
            port_log("\nport> --cast: \"%.*s\" is not a character (0-7, or one of "
                     "mario luigi peach yoshi wario donkey daisy waluigi)\n",
                     (int)len, start);
            exit(2);
        }
        cast_char[n++] = (s16)got;
        if (*p == ',') {
            p++;
        }
    }
    if (n != 4) {
        port_log("\nport> --cast: needs four characters, got %d\n", n);
        exit(2);
    }
    port_log("port> --cast: %s / %s / %s / %s\n", CAST_NAMES[cast_char[0]],
             CAST_NAMES[cast_char[1]], CAST_NAMES[cast_char[2]],
             CAST_NAMES[cast_char[3]]);
}

static void park_players(void) {
    int i;
    /* --mghold (M25, PLAN.md 40): inside a minigame's own overlay all four
     * players are human with idle controllers, so they hold still -- the way
     * to a four-way DRAW on demand (Avalanche!, PLAN.md 35.2), which four
     * COMs never give.  The minigames read `GWPlayerCfg[].iscom` every frame
     * (m406Dll/player.c:562) and an idle pad reads zero; outside the overlay
     * (the board, instDll, resultDll) the --com4 rule below holds as before. */
    int hold = port_opt.mghold && (int)omcurovl >= 0 && omMgIndexGet((s16)omcurovl) >= 0;
    for (i = 0; i < 4; i++) {
        if (hold) {
            GWPlayerCfg[i].pad_idx = (s16)i;
            GWPlayerCfg[i].iscom = 0;
            GWPlayer[i].com = 0;
            continue;
        }
        if (port_opt.com4) {
            if (cast_char[i] >= 0) {
                /* A named cast is parked every frame, exactly the way the rest
                 * of --com4's player state is: character select is walked past
                 * by the metronome and would otherwise pick its own four. */
                GWPlayerCfg[i].character = cast_char[i];
            }
            /* `character` is -1 out of bootDll (src/REL/bootDll/main.c:294)
             * and selmenuDll resets an out-of-range one to the player index
             * (selmenuDll/main.c:160-167).  Do the same rather than force a
             * cast of four, so a run that *has* been through character select
             * keeps the characters it chose. */
            if (GWPlayerCfg[i].character < 0 || GWPlayerCfg[i].character > 7) {
                GWPlayerCfg[i].character = (s16)i;
            }
            GWPlayerCfg[i].pad_idx = (s16)i;
            GWPlayerCfg[i].iscom = 1;
            /* GWPlayer's copy is what the board's turn logic reads
             * (src/game/board/com.c:285); player.c:327 copies Cfg -> Player at
             * board start, but a board already running needs both. */
            GWPlayer[i].com = 1;
        }
    }
    if (port_opt.turns) {
        GWSystem.max_turn = (u8)port_opt.turns;
    }
}

/* What the roulette itself decided, whether or not it is being overridden.
 *
 * This is the number the Dolphin comparison needs: with `--rtc` the two rigs
 * share the RNG's *origin*, and whether they then draw the same minigame is a
 * question about everything between the origin and `BoardRandInit`, which
 * reads `OSGetTime` at board setup rather than at boot.  Logging the draw
 * costs nothing and answers it on whatever run happens to reach a board. */
static void watch_roulette(u32 frame) {
    static int last = -1;
    int mg = (int)GWSystem.mg_next;
    /* The parked value is our own writing, not a draw; reporting it would turn
     * this into a one-line-a-frame ping-pong between what the game wrote and
     * what we wrote back. */
    if (mg == last || mg == forced_mg) {
        return;
    }
    last = mg;
    if (mg < 0 || mg >= 64 || mgInfoTbl[mg].ovl == 0xFFFF) {
        return;
    }
    port_log("port> roulette: frame %u dealt mg %d (%s, type %d)%s\n", frame,
             mg + 0x191, screen_name(mgInfoTbl[mg].ovl), (int)mgInfoTbl[mg].type,
             forced_mg >= 0 && mg != forced_mg ? "  -- overridden by --minigame" : "");
}

static void park_minigame(void) {
    int i;
    if (forced_mg < 0) {
        return;
    }
    GWSystem.mg_next = (u16)forced_mg;
    /* The team split, as E3setupDLL/mgselect.c:219-250 assigns it. */
    switch (forced_mg_type) {
    case 1: /* 1 vs 3 */
        for (i = 0; i < 4; i++) {
            GWPlayerCfg[i].group = (s16)(i == 0 ? 0 : 1);
        }
        break;
    case 2: /* 2 vs 2 */
        for (i = 0; i < 4; i++) {
            GWPlayerCfg[i].group = (s16)(i < 2 ? 0 : 1);
        }
        break;
    default: /* 4-player and the specials */
        for (i = 0; i < 4; i++) {
            GWPlayerCfg[i].group = (s16)i;
        }
        break;
    }
}

/* ---- the status line ---------------------------------------------------------
 *
 * One line a second, and every number on it is one a screenshot cannot show:
 * where the game is, how far into the board it is, which module the minigame
 * roulette last dealt, what each player has, and the two costs M6 left open.
 * PLAN.md §16.10 item 2 asked for the audio's own invariants to travel with
 * the harness's result line; they are the tail of this one.
 */
static void status_line(u32 frame) {
    double fps = 0.0, aud = 0.0;
    int mg = (int)GWSystem.mg_next;
    int mg_ovl = -1;
    char players[128];
    int i, n = 0;
    double speed = 0.0, pfps = 0.0;
    unsigned tex_n = 0, tex_kb = 0;

    port_perf_window(&fps, &aud, &speed, &pfps);
    gx_tex_cache_stats(&tex_n, &tex_kb);
    if (mg >= 0 && mg < 64 && mgInfoTbl[mg].ovl != 0xFFFF) {
        mg_ovl = mgInfoTbl[mg].ovl;
    }
    players[0] = '\0';
    for (i = 0; i < 4; i++) {
        n += snprintf(players + n, sizeof(players) - (size_t)n, "%s%d/%d%s",
                      i ? " " : "", (int)GWPlayer[i].coins, (int)GWPlayer[i].stars,
                      GWPlayer[i].com ? "c" : "h");
        if (n >= (int)sizeof(players)) {
            break;
        }
    }
    if (port_framemode_active()) {
        /* --realtime: the retrace rate is the speed, the frames that reached
         * the screen are the fps, and they are different numbers now */
        port_log("port> status f%-7u %-12s board %d turn %d/%d  mg %d (%s)  "
                 "coins/stars %s  aud %.2f ms  speed %.0f%%  %.1f fps presented  "
                 "tex %u/%u KB  rss %u MB  cpu %d  machine %s\n",
                 frame, screen_name((int)omcurovl), (int)GWSystem.board,
                 (int)GWSystem.turn, (int)GWSystem.max_turn, mg + 0x191,
                 screen_name(mg_ovl), players, aud, speed, pfps, tex_n, tex_kb,
                 port_rss_mb(), port_threads_on() ? 2 : 1, port_machine_verdict());
        return;
    }
    port_log("port> status f%-7u %-12s board %d turn %d/%d  mg %d (%s)  "
             "coins/stars %s  aud %.2f ms  %.1f fps\n",
             frame, screen_name((int)omcurovl), (int)GWSystem.board,
             (int)GWSystem.turn, (int)GWSystem.max_turn, mg + 0x191,
             screen_name(mg_ovl), players, aud, fps);
}

/* ---- the stuck-screen watchdog -----------------------------------------------
 *
 * A soak that hangs is worth nothing unless it says where.  The signal is
 * `omcurovl` plus `omovlevtno`: between them they change at every screen
 * transition the game makes, and neither changes while a screen is waiting
 * for input nobody is going to give it.  A hang inside one screen (a minigame
 * that never ends) is therefore reported by the same rule as a menu waiting
 * on a button, which is what we want -- both are "the soak is not making
 * progress".
 */
/* The soak's first run fired this ten times and every one was a lie
 * (PLAN.md §17.10).  `omcurovl` + `omovlevtno` is the right signal for a menu
 * waiting on a button nobody will press, and the wrong one for a board: four
 * CPU players walking a board at 10 fps stay in the same overlay and the same
 * event for well over ninety seconds at a time, between a minigame's results
 * and the next roulette, while being entirely healthy.  Ten false positives
 * are exactly how a real one gets missed.
 *
 * The fix is to ask the game whether it is *doing* anything rather than
 * whether it has changed screen.  These are the words that move whenever the
 * game is alive and stop moving when it is not: the overlay and its event, the
 * turn and whose turn it is, and each player's coins, stars and current space.
 * A piece moving one space, a coin changing hands, a turn ending -- any of
 * them re-arms the watch.  A board between events now looks the way it is.
 *
 * A minigame is the case this does not cover: it legitimately runs for a
 * minute with none of those words moving.  So the limit is per screen rather
 * than global -- a minigame gets the multiplier, because the only thing that
 * can be said about it from out here is that it has an end. */
static u32 progress_stamp(void) {
    u32 h = 2166136261u;
    int i;
#define PORT_MIX(v)                  \
    do {                             \
        h ^= (u32)(s32)(v);          \
        h *= 16777619u;              \
    } while (0)
    PORT_MIX(omcurovl);
    PORT_MIX(omovlevtno);
    PORT_MIX(GWSystem.turn);
    PORT_MIX(GWSystem.player_curr);
    for (i = 0; i < 4; i++) {
        PORT_MIX(GWPlayer[i].coins);
        PORT_MIX(GWPlayer[i].stars);
        PORT_MIX(GWPlayer[i].space_curr);
    }
#undef PORT_MIX
    return h;
}

/* A minigame has no progress signal visible from here, so it gets four times
 * the patience.  Everything else -- boards, menus, results -- is covered by
 * progress_stamp and gets the number that was asked for. */
static u32 stuck_limit(int ovl) {
    u32 limit = (u32)port_opt.stuckwatch * 60u;
    if (ovl >= 0 && omMgIndexGet((s16)ovl) >= 0) {
        return limit * 4u;
    }
    return limit;
}

/* mg_next between turns is not an index into anything: the board leaves it at
 * whatever the last draw was, or at a value the roulette has not finished
 * writing, and the watchdog printed it raw -- which is where "mg 65936" in the
 * first soak came from.  The status line already range-checks it; so does this
 * now, and an out-of-range value is reported as itself rather than dressed up
 * as a minigame number. */
static void format_mg_next(char* buf, size_t n) {
    int mg = (int)GWSystem.mg_next;
    if (mg >= 0 && mg < 64 && mgInfoTbl[mg].ovl != 0xFFFF) {
        snprintf(buf, n, "%d (%s)", mg + 0x191, screen_name(mgInfoTbl[mg].ovl));
    } else {
        snprintf(buf, n, "none (raw %d)", mg);
    }
}

static void stuck_watch(u32 frame) {
    static u32 last_stamp;
    static int primed;
    static u32 last_change;
    static u32 last_report;
    u32 stamp = progress_stamp();
    u32 limit = stuck_limit((int)omcurovl);
    char mg[64];

    if (!primed || stamp != last_stamp) {
        primed = 1;
        last_stamp = stamp;
        last_change = frame;
        return;
    }
    if (frame - last_change < limit) {
        return;
    }
    if (frame - last_report < limit) {
        return; /* one report per window, not one a frame */
    }
    last_report = frame;
    format_mg_next(mg, sizeof(mg));
    port_log("port> STUCK: frame %u, %u s with no progress (limit %u s here).  "
             "live screen %s (overlay %d, event %d, previous %s), turn %d/%d, "
             "mg_next %s\n",
             frame, (frame - last_change) / 60u, limit / 60u,
             screen_name((int)omcurovl), (int)omcurovl, (int)omovlevtno,
             screen_name((int)omprevovl), (int)GWSystem.turn,
             (int)GWSystem.max_turn, mg);
}


/* ---- the prompt navigator (M11; PLAN.md 24.6 item 1, 25) --------------------
 *
 * A `--com4` soak has nobody at any controller, and the game has screens that
 * wait for one.  The M10 soak played a whole 20-turn board, ran `resultDll`
 * cleanly, and then stopped dead on `mstory3dll`'s end-of-game prompt --
 * `A: Detailed Results / B: Skip` -- for eleven hours, because four CPU
 * players press nothing (PLAN.md §24.0).
 *
 * This is the one case the file's own rule -- *park the state, do not press
 * the button* -- cannot serve.  `fn_1_16924` (src/REL/mstory3dll/result.c:327)
 * is a `while (TRUE)` around `fn_1_938()` whose only two exits are
 * `HuPadBtnDown & PAD_BUTTON_A` and `HuPadBtnDown & PAD_BUTTON_MENU`; there is
 * no flag to park, and its third exit -- a 300-frame timeout -- is guarded by
 * `unk14 == -1`, which is not the soak's case.  The state that would end it is
 * a local variable in a loop this process is currently inside.
 *
 * So the harness presses.  What keeps that from being a licence to mash
 * buttons at the game is the *signal*: the only thing it presses on is the
 * watchdog's own `progress_stamp` standing still.  In a `--com4` run nothing
 * reads the pad legitimately -- that is the entire premise of `--com4` -- so a
 * screen that has made no progress for `SOAK_NUDGE_S` seconds is by
 * construction a screen waiting for input that is never coming, and a button
 * is the only thing that can help it.  A board mid-turn, a minigame mid-play
 * and a wipe all move the stamp, so none of them is ever nudged.
 *
 * Minigame overlays are excluded outright, for the same reason `stuck_limit`
 * gives them four times the patience: from out here nothing can be said about
 * a minigame's pacing, and a minigame that has genuinely hung is a bug to
 * report rather than a prompt to answer.
 *
 * The press goes in at `PADRead`'s raw `PortPadRaw` -- `port_pad_inject()` --
 * and nowhere else.  **The first version wrote `HuPadBtnDown[]`/`HuPadBtn[]`
 * for all four pad indices, and not one of those presses ever reached the
 * game** (PLAN.md 29.1): `port_selfplay_tick` runs from the VI post-retrace
 * callback, and the next thing the game does is src/game/main.c:101's
 * `HuPadRead()`, which overwrites both arrays from `_PadBtn*` and zeroes
 * `_PadBtnDown` -- all of it before `HuPrcCall(1)` dispatches a single
 * coroutine.  The derived globals are the game's output, not its input.
 *
 * The raw layer is the one a `--play` script has always used, which is the
 * argument for it: `board-start-com4.play`'s A metronome walks this exact
 * mode-select menu at every boot.  The cost is that `PADRead` fills channel 0
 * only -- a soak that reported four connected controllers would be telling
 * the game something untrue about the console -- so a prompt that reads a pad
 * index other than 0 is not answered by this.  The one prompt known to do
 * that is the results screen, and 28.1 fixed it at the source: `fn_1_373C`
 * now returns -1 in a four-CPU game and `fn_1_16924` takes its timeout.
 *
 * Three buttons are tried in rotation, a second apart, because the screens
 * between the results and the title do not all want the same one: `B` is the
 * prompt's advertised skip, `A` is every "continue" in the results sequence
 * and the save prompt, and `MENU` (START) is `fn_1_16924`'s other exit and the
 * mode-select menus' accept.  Whichever one lands, the stamp moves and the
 * navigator disarms itself until the next screen that needs it.
 */
/* How long to wait before pressing, and in what order.  Both numbers were
 * decided by the first witness run rather than guessed, and both are worth
 * the paragraph:
 *
 * **When.**  The first version used eight seconds and pressed on `w01dll`, a
 * board window that was going to resolve on its own -- the M10 soak played a
 * whole 20-turn board with nothing pressing anything.  A press there is not
 * merely useless, it is the harness making a *choice* on the game's behalf and
 * quietly changing the run.  So the trigger is the watchdog's own
 * `stuck_limit`, which is the number this project already calibrated for
 * exactly this question ("long enough that every legitimate wait has passed",
 * §17.10): 90 s from `--soak`, and four times that inside a minigame -- which
 * does not arise, because minigames are excluded outright below.
 *
 * **Which button, in what order.**  The first version rotated B, A, START and
 * *oscillated*: at the results screen `A` opens the detailed results
 * (result.c `fn_1_16924`) and `B` leaves them again (`fn_1_16AD4`), so the two
 * alternating is a loop -- and one `progress_stamp` cannot see, because
 * entering and leaving a results page moves no turn, no coin and no star.  The
 * witness run pressed 24 times, "answered", and stalled again at the same
 * screen.  So the rotation is **advance-only first**: B and START, which are
 * every skip and every dismiss in the game, tried three times each, and only
 * then A, which is the only one that can open something.  A loop of B/START
 * cannot ping-pong, because neither of them ever enters anything. */
#define SOAK_NUDGE_GAP 60u /* frames between presses */

static unsigned soak_nudges;
static unsigned soak_nudge_screens;

static void prompt_nav(u32 frame) {
    static u32 last_stamp;
    static int primed;
    static u32 last_change;
    static u32 last_press;
    static int rotation;
    static int armed_here;
    static const struct {
        u16 bit;
        const char* name;
    } BTN[7] = {
        { PAD_BUTTON_B, "B" },         { PAD_BUTTON_MENU, "START" },
        { PAD_BUTTON_B, "B" },         { PAD_BUTTON_MENU, "START" },
        { PAD_BUTTON_B, "B" },         { PAD_BUTTON_MENU, "START" },
        { PAD_BUTTON_A, "A" },
    };
    static u32 hold_until;
    u32 stamp = progress_stamp();

    /* A press held for four frames, not one.  Now that the press enters at the
     * raw layer the game derives its own edge from it, and a one-frame raw
     * blip is lost whenever two retraces fall between two `HuPadRead()` calls
     * -- which a 10 fps board does all the time.  Four frames is what the boot
     * walk holds. */
    if (hold_until && frame < hold_until) {
        port_pad_inject(BTN[rotation ? rotation - 1 : 6].bit);
        return;
    }

    if (!primed || stamp != last_stamp) {
        primed = 1;
        last_stamp = stamp;
        last_change = frame;
        if (armed_here) {
            port_log("port> soak: %s answered after %u press(es); the run "
                     "continues\n",
                     screen_name((int)omcurovl), soak_nudges - armed_here + 1);
            armed_here = 0;
            rotation = 0;
        }
        return;
    }
    /* a minigame's pacing is not this file's business */
    if ((int)omcurovl >= 0 && omMgIndexGet((s16)omcurovl) >= 0) {
        return;
    }
    /* The mode-select menu wants the walk's own metronome, not the rotation.
     *
     * A board that ends now *returns* -- 28.1 -- and it returns to
     * `modeseldll` event 1, the main menu, where the run has to pick Party
     * again to chain a second board.  The rotation cannot do it: it is
     * B, START, B, START, B, START, A, so every A that moves the menu on is
     * followed by a B that backs it out again, and the pair ping-pongs exactly
     * the way 26.1's screen did.  `board-start-com4.play` walks this same menu
     * at the boot without trouble, and what it does there is A for four frames
     * out of every sixty-four -- so do that here.  The script's own metronome
     * stops at frame 29,960 and the second board is long past it.  PLAN.md
     * 28.3. */
    if ((int)omcurovl == DLL_modeseldll || (int)omcurovl == DLL_mentdll) {
        if (frame - last_change < stuck_limit((int)omcurovl)) {
            return;
        }
        if ((frame & 63u) < 4u) {
            port_pad_inject(PAD_BUTTON_A);
            if (!armed_here) {
                armed_here = (int)soak_nudges + 1;
                soak_nudge_screens++;
                port_log("port> soak: %s (overlay %d, event %d) has waited %u s; "
                         "walking it with the A metronome\n",
                         screen_name((int)omcurovl), (int)omcurovl,
                         (int)omovlevtno, (frame - last_change) / 60u);
            }
            soak_nudges++;
        }
        return;
    }
    if (frame - last_change < stuck_limit((int)omcurovl)) {
        return;
    }
    if (last_press && frame - last_press < SOAK_NUDGE_GAP) {
        return;
    }
    last_press = frame;
    if (!armed_here) {
        armed_here = (int)soak_nudges + 1;
        soak_nudge_screens++;
        port_log("port> soak: %s (overlay %d, event %d) has waited %u s for a "
                 "button nobody is going to press; answering it\n",
                 screen_name((int)omcurovl), (int)omcurovl, (int)omovlevtno,
                 (frame - last_change) / 60u);
    }
    soak_nudges++;
    port_pad_inject(BTN[rotation].bit);
    port_log("port> soak: press %s on %s at frame %u\n", BTN[rotation].name,
             screen_name((int)omcurovl), frame);
    rotation = (rotation + 1) % 7;
    hold_until = frame + 4u;
}

/* ---- the module trace ---------------------------------------------------------
 *
 * `--soak`'s log of record: every minigame module entered and left, with the
 * frame.  It is the same `omcurovl` watch as the watchdog, filtered to the
 * overlays `omMgIndexGet` recognises, which is exactly the m4xx set.
 */
static void module_trace(u32 frame) {
    static int last = -2;
    int cur = (int)omcurovl;
    if (cur == last) {
        return;
    }
    if (last >= 0 && omMgIndexGet((s16)last) >= 0) {
        port_log("port> soak: left  minigame %-9s at frame %u\n", screen_name(last),
                 frame);
        /* The one in force has been played: arm the move, so a run given
         * several names witnesses them all in one board.  The move itself
         * waits for resultDll (see `forced_mg_pending`). */
        if (forced_mg >= 0 && omMgIndexGet((s16)last) == forced_mg) {
            forced_mg_pending = 1;
        }
    }
    if (forced_mg_pending && last == DLL_resultdll && cur != DLL_resultdll) {
        forced_mg_pending = 0;
        {
            if (forced_mg_select(forced_mg_at + 1)) {
                port_log("port> --minigame: next is %s (mg %d, type %d), %d of %d\n",
                         screen_name(mgInfoTbl[forced_mg].ovl), forced_mg + 0x191,
                         forced_mg_type, forced_mg_at + 1, forced_mg_len);
            } else if (forced_mg_len > 1) {
                /* The list is done.  Let the roulette have its own answers back
                 * rather than pinning every remaining turn on the last name:
                 * `--soak --minigame a,b,c,d` then means "these four first, then
                 * whatever you like", which is the shape an overnight soak that
                 * has four specific modules to witness wants. */
                forced_mg = -1;
                port_log("port> --minigame: list done after %d; the roulette is "
                         "released\n",
                         forced_mg_len);
            }
        }
    }
    if (cur >= 0 && omMgIndexGet((s16)cur) >= 0) {
        port_log("port> soak: enter minigame %-9s at frame %u (mg %d)\n",
                 screen_name(cur), frame, omMgIndexGet((s16)cur) + 0x191);
    }
    last = cur;
}

/* ---- entry points ------------------------------------------------------------ */

void port_selfplay_init(void) {
    cast_parse();
    if (port_opt.minigame) {
        char buf[256];
        char* save = NULL;
        char* tok;
        snprintf(buf, sizeof(buf), "%s", port_opt.minigame);
        for (tok = strtok_r(buf, ",", &save); tok != NULL && forced_mg_len < FORCED_MG_MAX;
             tok = strtok_r(NULL, ",", &save)) {
            forced_mg_list[forced_mg_len++] = mg_index_from_arg(tok);
        }
        if (!forced_mg_select(0)) {
            port_log("port> --minigame %s: no such minigame; the roulette is left "
                     "alone\n",
                     port_opt.minigame);
            forced_mg = -1;
            forced_mg_len = 0;
        } else {
            port_log("port> --minigame %s: parking the roulette on %s (mg %d, "
                     "type %d)%s\n",
                     port_opt.minigame, screen_name(mgInfoTbl[forced_mg].ovl),
                     forced_mg + 0x191, forced_mg_type,
                     forced_mg_len > 1 ? ", then the rest of the list, one a turn" : "");
        }
    }
    if (port_opt.com4) {
        port_log("port> --com4: all four players are CPU; instDll will dismiss its "
                 "own instruction screen (src/REL/instDll/main.c:294)\n");
    }
    if (port_opt.soak) {
        port_log("port> --soak: playing itself; every minigame module entered and "
                 "left is logged\n");
    }
}

/* ---- the soak's title-screen guard -------------------------------------------
 *
 * The 2026-09-14 overnight soak spent seven hours on the attract loop because
 * nothing pressed Start (PLAN.md 21.8).  `--soak` now implies the menu walk,
 * which makes that exact mistake impossible -- but the failure it caused is
 * worth a second, independent check, because "a script was named" and "the
 * script is pressing anything" are different facts and only the second one
 * gets the soak off the title.
 *
 * So: if the run is still in `bootdll` -- the boot logos, the title and the
 * attract demo -- and the --play script has driven no button at all, the soak
 * says so and stops rather than burning the night.  Sixty seconds is the
 * number asked for and it is comfortable: the port reaches the title in about
 * twelve seconds of wall clock and `board-start-com4.play` presses Start well
 * before a minute is out.  The second, longer limit is for the case where a
 * script *is* pressing buttons and the title is eating them anyway; four
 * minutes is past the slowest boot this port has ever taken to the file
 * select, so reaching it means the walk is not working.
 */
#define SOAK_TITLE_SILENT_S 60.0
#define SOAK_TITLE_STUCK_S 240.0

static void title_guard(u32 frame) {
    static double t0;
    static int done;
    double now;
    int on_title;

    if (done) {
        return;
    }
    if (t0 == 0.0) {
        t0 = port_now_seconds();
        return;
    }
    on_title = (int)omcurovl >= 0 && (int)omcurovl < OVL_COUNT &&
               !strcasecmp(ovl_name[(int)omcurovl], "bootdll");
    if (!on_title) {
        done = 1; /* the walk worked; never look again */
        return;
    }
    now = port_now_seconds() - t0;
    if (now < SOAK_TITLE_SILENT_S) {
        return;
    }
    if (now < SOAK_TITLE_STUCK_S && pad_play_press_count() != 0) {
        return; /* something is pressing something: give it the longer limit */
    }
    done = 1;
    port_log("\n*** port> --soak: still in bootdll (the title/attract loop) after "
             "%.0f s, frame %u.\n", now, frame);
    port_log("    the --play script has pressed a button on %u frames%s.\n",
             pad_play_press_count(),
             pad_play_press_count() == 0 ? " -- nothing is walking the menus" : "");
    port_log("    a soak that never leaves the title learns nothing, so this run "
             "stops here rather than\n"
             "    spending the night on it.  The walk is "
             "Contents/Resources/movies/board-start-com4.play;\n"
             "    --soak uses it unless --play names another (PLAN.md 22.3).\n");
    fflush(stdout);
    fflush(stderr);
    exit(3);
}

/* Once per retrace, from the gate in port/src/platform/vi.c, after the game's
 * own PadReadVSync post-callback -- so a parked value is the last word on the
 * frame the game is about to run. */
void port_selfplay_tick(u32 frame) {
    if (!port_opt.com4 && !port_opt.turns && !port_opt.minigame &&
        !port_opt.status && !port_opt.stuckwatch && !port_opt.soak) {
        return;
    }
    park_players();
    watch_roulette(frame);
    park_minigame();
    if (port_opt.soak) {
        module_trace(frame);
        title_guard(frame);
        prompt_nav(frame);
    }
    if (port_opt.stuckwatch) {
        stuck_watch(frame);
    }
    if (port_opt.status && (frame % 60u) == 0u) {
        status_line(frame);
    }
}

/* ---- snapshots ------------------------------------------------------------
 * The harness's own position in the run: which minigame it is parking on and
 * how far down a `--minigame a,b,c` list it has got.  Everything else in this
 * file is diagnostics (the stuck watch's timers, the status line's cadence)
 * and is allowed to restart with the process. */
void port_selfplay_snap_register(void) {
    port_snap_register("selfplay.forced_mg", &forced_mg, sizeof(forced_mg));
    port_snap_register("selfplay.forced_mg_type", &forced_mg_type,
                       sizeof(forced_mg_type));
    port_snap_register("selfplay.forced_mg_list", forced_mg_list,
                       sizeof(forced_mg_list));
    port_snap_register("selfplay.forced_mg_len", &forced_mg_len, sizeof(forced_mg_len));
    port_snap_register("selfplay.forced_mg_at", &forced_mg_at, sizeof(forced_mg_at));
    port_snap_register("selfplay.forced_mg_pending", &forced_mg_pending,
                       sizeof(forced_mg_pending));
}
