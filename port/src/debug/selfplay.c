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
#include "game/object.h"
#include "game/objsub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Overlay id -> name, built from the same header the game's own `_ovltbl`
 * is built from, so the two cannot drift. */
#define DLL(name) #name,
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

/* ---- parking ---------------------------------------------------------------- */

static void park_players(void) {
    int i;
    for (i = 0; i < 4; i++) {
        if (port_opt.com4) {
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

    port_perf_window(&fps, &aud);
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
static void stuck_watch(u32 frame) {
    static int last_ovl = -2;
    static int last_evt = -2;
    static u32 last_change;
    static u32 last_report;
    u32 limit = (u32)port_opt.stuckwatch * 60u;

    if ((int)omcurovl != last_ovl || (int)omovlevtno != last_evt) {
        last_ovl = (int)omcurovl;
        last_evt = (int)omovlevtno;
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
    port_log("port> STUCK: frame %u, %u s with no scene change.  live screen "
             "%s (overlay %d, event %d, previous %s), turn %d/%d, mg_next %d\n",
             frame, (frame - last_change) / 60u, screen_name((int)omcurovl),
             (int)omcurovl, (int)omovlevtno, screen_name((int)omprevovl),
             (int)GWSystem.turn, (int)GWSystem.max_turn,
             (int)GWSystem.mg_next + 0x191);
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
    }
    if (cur >= 0 && omMgIndexGet((s16)cur) >= 0) {
        port_log("port> soak: enter minigame %-9s at frame %u (mg %d)\n",
                 screen_name(cur), frame, omMgIndexGet((s16)cur) + 0x191);
    }
    last = cur;
}

/* ---- entry points ------------------------------------------------------------ */

void port_selfplay_init(void) {
    if (port_opt.minigame) {
        forced_mg = mg_index_from_arg(port_opt.minigame);
        if (forced_mg < 0 || forced_mg >= 64 || mgInfoTbl[forced_mg].ovl == 0xFFFF) {
            port_log("port> --minigame %s: no such minigame; the roulette is left "
                     "alone\n",
                     port_opt.minigame);
            forced_mg = -1;
        } else {
            forced_mg_type = mgInfoTbl[forced_mg].type;
            port_log("port> --minigame %s: parking the roulette on %s (mg %d, "
                     "type %d)\n",
                     port_opt.minigame, screen_name(mgInfoTbl[forced_mg].ovl),
                     forced_mg + 0x191, forced_mg_type);
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
    }
    if (port_opt.stuckwatch) {
        stuck_watch(frame);
    }
    if (port_opt.status && (frame % 60u) == 0u) {
        status_line(frame);
    }
}
