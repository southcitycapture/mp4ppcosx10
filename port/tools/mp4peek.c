/* mp4peek -- read the game-side state out of a *running* port process.
 *
 * Mac OS X 10.4/10.5 on the G4 has no gdb (no Developer tools installed) and
 * no `gcore`, so when a soak stalls the only two things in the box are
 * `sample` -- which shows the render thread and nothing about the game's own
 * coroutines -- and `vmmap`, which shows no contents at all.  That is not
 * enough to answer "what is the game waiting on", which is the only question a
 * stall asks.
 *
 * This reads the answer directly: task_for_pid + vm_read_overwrite against the
 * live process, decoding the two structures that matter.
 *
 *   mp4peek <pid> procs <addr of _processtop> [addr of _processcur]
 *   mp4peek <pid> se    <addr of _se>
 *   mp4peek <pid> words <addr> <count>
 *
 * The addresses come from the *exact binary that is running*:
 *
 *   nm -arch ppc -a port/build-ppc-darwin/marioparty4 | grep ' b _processtop'
 *
 * PowerPC Mach-O executables are not position independent, so a symbol's link
 * address is its runtime address and there is no slide to add.
 *
 * task_for_pid on 10.5 is root-only for a process you do not own outright, so
 * run it under sudo.  Nothing here writes to the target: it is a reader, and a
 * stalled process survives being read.
 *
 * Built by port/tools/build-peek.sh (it is not part of the port binary).
 */
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <mach/mach.h>

#include "dolphin/types.h"
#include "game/process.h"

/* msmse.c's SE_PLAYER and its file-static `se` are not in any header -- the
 * decomp keeps them local to the translation unit.  Copied verbatim (the
 * decomp's sources are never edited; this is port/ code that has to agree with
 * them), and the sizes are checked against the build below. */
#include "msm/msmse.h"

#define SE_PLAYER_EMIT (1 << 0)

typedef struct SePlayer_s {
    SND_VOICEID vid;
    s32 no;
    s16 seId;
    s8 status;
    s8 busyF;
    s8 vol;
    s8 pan;
    s16 pitch;
    s8 span;
    s8 auxAVol;
    s8 auxBVol;
    u8 flag;
    SND_PARAMETER_INFO paramInfo;
    SND_PARAMETER param[5];
    s8 baseVol;
    s8 basePan;
    s16 basePitch;
    volatile s32 fadeMaxTime;
    s32 fadeTime;
    s8 fadeVol;
    volatile s32 pauseMaxTime;
    s32 pauseTime;
    s8 pauseVol;
    SND_EMITTER emitter;
    SND_FVECTOR emiPos;
    SND_FVECTOR emiDir;
    s8 emitterF;
} SE_PLAYER;

typedef struct SeWork_s {
    s32 seMax;
    s8 sfx;
    s8 baseGrpNumPlay;
    s8 numPlay;
    s32 no;
    void *seData;
    SE_PLAYER *player;
    SND_LISTENER listener;
    SND_FVECTOR listenerPos;
    SND_FVECTOR listenerDir;
    SND_FVECTOR listenerHeading;
    SND_FVECTOR listenerUp;
    float sndDist;
    u16 groupId;
    u16 listenerF;
} SeWork;

static task_t task;

static int peek(unsigned long addr, void *out, unsigned long len) {
    vm_size_t got = 0;
    kern_return_t kr = vm_read_overwrite(task, (vm_address_t)addr, (vm_size_t)len,
                                         (vm_address_t)(uintptr_t)out, &got);
    if (kr != KERN_SUCCESS || got != len) {
        fprintf(stderr, "mp4peek: cannot read %lu bytes at %08lx (%s)\n", len, addr,
                mach_error_string(kr));
        return 0;
    }
    return 1;
}

static const char *exec_name(u16 e) {
    switch (e) {
        case HUPRC_EXEC_NORMAL:     return "NORMAL";
        case HUPRC_EXEC_SLEEP:      return "SLEEP";
        case HUPRC_EXEC_CHILDWATCH: return "CHILDWATCH";
        case HUPRC_EXEC_KILLED:     return "KILLED";
        default:                    return "?";
    }
}

static void dump_procs(unsigned long top_addr, unsigned long cur_addr) {
    unsigned long top = 0, cur = 0, p;
    int n;

    if (!peek(top_addr, &top, 4)) return;
    if (cur_addr) peek(cur_addr, &cur, 4);

    printf("processtop = %08lx   processcur = %08lx   sizeof(HUPROCESS) = %lu\n",
           top, cur, (unsigned long)sizeof(HUPROCESS));
    printf("  offsets: exec %lu stat %lu prio %lu sleep_time %lu base_sp %lu jump %lu\n",
           (unsigned long)offsetof(HUPROCESS, exec), (unsigned long)offsetof(HUPROCESS, stat),
           (unsigned long)offsetof(HUPROCESS, prio), (unsigned long)offsetof(HUPROCESS, sleep_time),
           (unsigned long)offsetof(HUPROCESS, base_sp), (unsigned long)offsetof(HUPROCESS, jump));
    printf("\n%-10s %-4s %-11s %-6s %-5s %-10s %-9s %-9s %-9s %s\n",
           "process", "prio", "exec", "stat", "sleep", "resume lr", "jump sp", "base_sp",
           "parent", "child");

    for (p = top, n = 0; p && n < 64; n++) {
        HUPROCESS pr;
        if (!peek(p, &pr, sizeof(pr))) break;
        printf("%08lx   %-4u %-11s %-6u %-5d %08x   %08x  %08x  %08lx  %08lx%s\n",
               p, pr.prio, exec_name(pr.exec), pr.stat, (int)pr.sleep_time,
               pr.jump.lr, pr.jump.sp, pr.base_sp,
               (unsigned long)(uintptr_t)pr.parent, (unsigned long)(uintptr_t)pr.first_child,
               (unsigned long)(uintptr_t)p == cur ? "   <- current" : "");
        p = (unsigned long)(uintptr_t)pr.next;
        if (p == top) break;
    }
    printf("\n%d processes.  Symbolise the resume lr with:\n", n);
    printf("  atos -arch ppc -o port/build-ppc-darwin/marioparty4 <lr>\n");
    printf("  (an lr with no image is inside a REL bundle: "
           "atos -arch ppc -o port/build-ppc-darwin/rels/<mod>.bundle -l <base> <lr>)\n");
}

static void dump_se(unsigned long se_addr) {
    SeWork w;
    int i, busy = 0;

    if (!peek(se_addr, &w, sizeof(w))) return;
    printf("se @ %08lx: seMax %d  sfx (player slots) %d  numPlay %d  baseGrpNumPlay %d\n",
           se_addr, (int)w.seMax, (int)w.sfx, (int)w.numPlay, (int)w.baseGrpNumPlay);
    printf("  player[] @ %08lx  sizeof(SE_PLAYER) = %lu  groupId %u  listenerF %u\n\n",
           (unsigned long)(uintptr_t)w.player, (unsigned long)sizeof(SE_PLAYER),
           w.groupId, w.listenerF);
    printf("%-4s %-6s %-6s %-7s %-6s %-6s %-5s %-6s %-8s %-8s\n",
           "slot", "status", "busyF", "seId", "no", "vid", "flag", "emitF", "fadeMax", "pauseMax");
    for (i = 0; i < w.sfx && i < 128; i++) {
        SE_PLAYER pl;
        unsigned long a = (unsigned long)(uintptr_t)w.player + (unsigned long)i * sizeof(SE_PLAYER);
        if (!peek(a, &pl, sizeof(pl))) break;
        printf("%-4d %-6d %-6d %-7d %-6d %-6d %-5u %-6d %-8d %-8d\n", i, pl.status, pl.busyF,
               pl.seId, (int)pl.no, (int)pl.vid, pl.flag, pl.emitterF,
               (int)pl.fadeMaxTime, (int)pl.pauseMaxTime);
        if (pl.status != 0) busy++;
    }
    printf("\n%d of %d SE player slots are not free (status != 0).%s\n", busy, (int)w.sfx,
           busy == w.sfx ? "  ALL BUSY: msmSePlay returns MSM_ERR_CHANLIMIT (-110)." : "");
}

static void dump_words(unsigned long addr, int count) {
    int i;
    for (i = 0; i < count; i += 4) {
        u32 v[4];
        int j, n = count - i < 4 ? count - i : 4;
        if (!peek(addr + (unsigned long)i * 4, v, (unsigned long)n * 4)) return;
        printf("%08lx:", addr + (unsigned long)i * 4);
        for (j = 0; j < n; j++) printf(" %08x", v[j]);
        printf("\n");
    }
}

int main(int argc, char **argv) {
    pid_t pid;
    kern_return_t kr;

    if (argc < 4) {
        fprintf(stderr, "usage: mp4peek <pid> procs <processtop> [processcur]\n"
                        "       mp4peek <pid> se    <se>\n"
                        "       mp4peek <pid> words <addr> <count>\n");
        return 2;
    }
    pid = (pid_t)strtol(argv[1], NULL, 0);
    kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mp4peek: task_for_pid(%d) failed: %s (run me under sudo)\n", pid,
                mach_error_string(kr));
        return 1;
    }
    if (!strcmp(argv[2], "procs")) {
        dump_procs(strtoul(argv[3], NULL, 16), argc > 4 ? strtoul(argv[4], NULL, 16) : 0);
    } else if (!strcmp(argv[2], "se")) {
        dump_se(strtoul(argv[3], NULL, 16));
    } else if (!strcmp(argv[2], "words")) {
        dump_words(strtoul(argv[3], NULL, 16), argc > 4 ? atoi(argv[4]) : 16);
    } else {
        fprintf(stderr, "mp4peek: unknown command %s\n", argv[2]);
        return 2;
    }
    return 0;
}
