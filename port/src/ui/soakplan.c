/* M51 (PLAN.md 66): the soak planner's file, and `--soakplan FILE`.
 *
 * Developer Mode's third page (overlay.c) writes a plan -- plain
 * `key = value` lines a shell can read too (tools/soakplan.sh, the chain) --
 * and starts the game again as `isle --soakplan FILE`, which main() turns
 * into the soak's own flags in front of anything else on the line:
 *
 *   boards    = 1 .. 6, or 1+ (every board in turn)       --board
 *   turns     = N                                          --turns
 *   minigames = all | m401,m441,... (up to 16, in turn)    --minigame
 *   lite      = auto | on | off, liteopts = LIST           --lite* --liteopts
 *   water     = auto | off | cheap | full                  --water
 *   snapshots = 0 | N (every N frames, the last 3 kept)    --snap-every
 *   minutes   = N (0: until stopped)                       --frames
 *   flags     = anything else, verbatim
 *
 * and always `--soak --com4 --rtc dolphin --freshcard --status --perf
 * --stuckwatch 200 --ovllog` -- the release soak's line. */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* sp_get(const char* path, const char* key, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    char line[512];
    size_t kl = strlen(key);
    if (!f) {
        return NULL;
    }
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ') {
            p++;
        }
        if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '=')) {
            p += kl;
            while (*p == ' ' || *p == '=') {
                p++;
            }
            p[strcspn(p, "\r\n#")] = 0;
            while (*p && p[strlen(p) - 1] == ' ') {
                p[strlen(p) - 1] = 0;
            }
            snprintf(out, n, "%s", p);
            fclose(f);
            return out;
        }
    }
    fclose(f);
    return NULL;
}

const char* port_soakplan_path(void) {
    static char p[1024];
    if (!p[0]) {
        snprintf(p, sizeof(p), "%s/soak-plan.txt", port_app_support_dir());
    }
    return p;
}

int port_soakplan_argv(int argc, char** argv, int* out_argc, char*** out_argv) {
    static char* toks[96];
    static char store[4096];
    size_t used = 0;
    const char* path = NULL;
    char v[512];
    char** nv;
    int i, n = 0, k;
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--soakplan")) {
            path = argv[i + 1];
        }
    }
    if (!path) {
        return 0;
    }
#define TOK(s)                                                           \
    do {                                                                 \
        size_t l_ = strlen(s) + 1;                                       \
        if (n < 90 && used + l_ < sizeof(store)) {                        \
            memcpy(store + used, s, l_);                                 \
            toks[n++] = store + used;                                    \
            used += l_;                                                  \
        }                                                                \
    } while (0)
    TOK("--soak");
    TOK("--com4");
    TOK("--rtc");
    TOK("dolphin");
    TOK("--freshcard");
    TOK("--status");
    TOK("--perf");
    TOK("--stuckwatch");
    TOK("200");
    TOK("--ovllog");
    if (sp_get(path, "boards", v, sizeof(v)) && *v) {
        TOK("--board");
        TOK(v);
    }
    if (sp_get(path, "turns", v, sizeof(v)) && atoi(v) > 0) {
        TOK("--turns");
        TOK(v);
    }
    if (sp_get(path, "minigames", v, sizeof(v)) && *v && strcmp(v, "all")) {
        TOK("--minigame");
        TOK(v);
    }
    if (sp_get(path, "lite", v, sizeof(v))) {
        TOK(!strcmp(v, "on") ? "--lite" : !strcmp(v, "off") ? "--nolite" : "--liteauto");
    }
    if (sp_get(path, "liteopts", v, sizeof(v)) && *v) {
        TOK("--liteopts");
        TOK(v);
    }
    if (sp_get(path, "water", v, sizeof(v)) && *v) {
        TOK("--water");
        TOK(v);
    }
    if (sp_get(path, "snapshots", v, sizeof(v)) && atoi(v) > 0) {
        TOK("--snap-every");
        TOK(v);
        TOK("--snap-keep");
        TOK("3");
    }
    if (sp_get(path, "minutes", v, sizeof(v)) && atoi(v) > 0) {
        char fr[32];
        snprintf(fr, sizeof(fr), "%d", atoi(v) * 3600);
        TOK("--frames");
        TOK(fr);
    }
    if (sp_get(path, "flags", v, sizeof(v)) && *v) {
        char* s = strtok(v, " ");
        while (s) {
            TOK(s);
            s = strtok(NULL, " ");
        }
    }
#undef TOK
    nv = calloc((size_t)(argc + n + 1), sizeof(char*));
    k = 0;
    nv[k++] = argv[0];
    for (i = 0; i < n; i++) {
        nv[k++] = toks[i];
    }
    for (i = 1; i < argc; i++) {
        nv[k++] = argv[i];
    }
    nv[k] = NULL;
    *out_argc = k;
    *out_argv = nv;
    fprintf(stderr, "port> --soakplan %s:", path);
    for (i = 1; i <= n; i++) {
        fprintf(stderr, " %s", nv[i]);
    }
    fprintf(stderr, "\n");
    return 1;
}
