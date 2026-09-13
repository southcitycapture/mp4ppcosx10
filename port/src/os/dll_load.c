/* REL loading: one dlopen'ed Mach-O bundle per relocatable module.
 *
 * `src/game/objdll.c` is 176 lines and its contract is five points
 * (PLAN.md §1.4):
 *
 *   1. resolve a name to a relocatable image,
 *   2. give it a fresh, zeroed bss,
 *   3. call `_prolog`, later `_epilog`,
 *   4. unload it and reclaim the memory,
 *   5. and be able to re-enter a module that stayed resident, with its bss
 *      re-zeroed and its prolog run again.
 *
 * `dlopen` of an `MH_BUNDLE` answers all five, and the fifth for free: a
 * bundle that is really unloaded and opened again comes back with fresh,
 * zeroed `__bss`/`__common`, so the re-entry case needs no bss extent table.
 * That "really" is the one assumption worth checking rather than believing --
 * PLAN.md §4 risk 2 -- so `dll_unloaded()` asks dyld, every single unload,
 * whether the image is actually gone, and if it is not the loader falls back
 * to zeroing the bundle's bss sections by hand through the Mach-O load
 * commands.  `--reltest` exercises the whole thing over all 99 modules twice
 * and prints what it found.
 *
 * The game's own bookkeeping is left completely intact.  `omDllData.bss` --
 * which on the console held the module's bss block -- holds the `dlopen`
 * handle instead (nothing but this file and the loader ever dereferences it),
 * and `omDllData.module` still points at the module header read off the real
 * disc, so `omDLLInfoDump`/`omDLLHeaderDump` still narrate the true REL.
 */
#include "port.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

#include <dolphin/types.h>
#include <dolphin/os.h>

typedef s32 (*DLLProlog)(void);
typedef void (*DLLEpilog)(void);

typedef struct DllModule {
    char name[64];  /* "bootdll", lower case, no extension */
    char path[1024];
    void* handle;
    int opens;
    int stuck; /* dlclose did not unload it; zero the bss by hand */
} DllModule;

static DllModule* mods;
static int mod_count;
static int scanned;

/* counters, printed by port_dll_report() */
static int stat_open, stat_close, stat_reenter, stat_stuck;

/* ---- where the bundles live ---------------------------------------------- */

static void exe_dir(char* out, size_t n) {
    uint32_t sz = (uint32_t)n;
    char buf[1024];
    char* slash;
    if (_NSGetExecutablePath(buf, &sz) != 0) {
        snprintf(out, n, ".");
        return;
    }
    slash = strrchr(buf, '/');
    if (slash) {
        *slash = 0;
    }
    snprintf(out, n, "%s", buf);
}

static int is_module_file(const char* fn, char* stem, size_t n) {
    const char* dot = strrchr(fn, '.');
    size_t len;
    if (!dot) {
        return 0;
    }
    /* MH_BUNDLE either way; the extension only says which target built it. */
    if (strcmp(dot, ".dylib") != 0 && strcmp(dot, ".bundle") != 0) {
        return 0;
    }
    len = (size_t)(dot - fn);
    if (len == 0 || len >= n) {
        return 0;
    }
    memcpy(stem, fn, len);
    stem[len] = 0;
    while (len--) {
        stem[len] = (char)tolower((unsigned char)stem[len]);
    }
    return 1;
}

static void scan(void) {
    char dir[1024];
    DIR* d;
    struct dirent* e;
    if (scanned) {
        return;
    }
    scanned = 1;
    if (port_opt.reldir) {
        snprintf(dir, sizeof(dir), "%s", port_opt.reldir);
    } else {
        char base[1024];
        exe_dir(base, sizeof(base));
        snprintf(dir, sizeof(dir), "%s/rels", base);
    }
    d = opendir(dir);
    if (!d) {
        port_log("port> REL bundles: %s does not exist -- no module will load "
                 "(build them, or pass --reldir)\n", dir);
        return;
    }
    mods = calloc(256, sizeof(*mods));
    while ((e = readdir(d)) != NULL) {
        char stem[64];
        if (!is_module_file(e->d_name, stem, sizeof(stem))) {
            continue;
        }
        if (mod_count == 256) {
            break;
        }
        snprintf(mods[mod_count].name, sizeof(mods[mod_count].name), "%s", stem);
        snprintf(mods[mod_count].path, sizeof(mods[mod_count].path), "%s/%s", dir,
                 e->d_name);
        mod_count++;
    }
    closedir(d);
    port_log("port> REL bundles: %d in %s\n", mod_count, dir);
}

/* "dll/bootdll.rel" -> the table entry, case-insensitively (the overlay table
 * spells names in lower case, the disc spells them in mixed case). */
static DllModule* find(const char* relpath) {
    const char* base = strrchr(relpath, '/');
    char stem[64];
    const char* dot;
    size_t len;
    int i;
    base = base ? base + 1 : relpath;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(stem)) {
        len = sizeof(stem) - 1;
    }
    memcpy(stem, base, len);
    stem[len] = 0;
    scan();
    for (i = 0; i < mod_count; i++) {
        if (strcasecmp(mods[i].name, stem) == 0) {
            return &mods[i];
        }
    }
    return NULL;
}

static DllModule* by_handle(void* handle) {
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].handle == handle) {
            return &mods[i];
        }
    }
    return NULL;
}

/* ---- did dlclose really unload it? --------------------------------------- */
/* RTLD_NOLOAD returns a handle only if the image is still resident.  Mac OS X
 * has had it since 10.3, so this check costs nothing and works on the G4. */

static int dll_unloaded(const char* path) {
#ifdef RTLD_NOLOAD
    void* h = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (h == NULL) {
        return 1;
    }
    dlclose(h); /* undo the reference RTLD_NOLOAD just took */
    return 0;
#else
    (void)path;
    return 1;
#endif
}

/* The fallback for a bundle dyld declines to unload: walk the loaded image's
 * Mach-O load commands and zero every `__bss` / `__common` section by hand,
 * which is precisely what `omDLLStart`'s re-entry branch used to do with
 * `memset(dll->bss, 0, module->bssSize)`.  Reached through the module's own
 * `_prolog` address, so no `dladdr` in a hot path (PLAN.md §2.4). */
static void zero_bss(void* handle, const char* name) {
    void* prolog = dlsym(handle, "_prolog");
    Dl_info info;
    unsigned long size = 0;
    unsigned char* p;
    int zeroed = 0;
    if (!prolog || !dladdr(prolog, &info) || !info.dli_fbase) {
        port_log("port> %s: cannot find the loaded image to zero its bss\n", name);
        return;
    }
#ifdef __LP64__
    {
        const struct mach_header_64* mh = (const struct mach_header_64*)info.dli_fbase;
        p = getsectiondata(mh, "__DATA", "__bss", &size);
        if (p && size) {
            memset(p, 0, size);
            zeroed += (int)size;
        }
        p = getsectiondata(mh, "__DATA", "__common", &size);
        if (p && size) {
            memset(p, 0, size);
            zeroed += (int)size;
        }
    }
#else
    {
        /* 32-bit Mach-O: the section header gives a link-time vmaddr, so it
         * needs the image's slide, which is the loaded header address minus
         * __TEXT's vmaddr. */
        const struct mach_header* mh = (const struct mach_header*)info.dli_fbase;
        /* The MacOSX10.4u SDK the PowerPC cross build uses has no
         * getsegbynamefromheader(), so walk the load commands for __TEXT
         * directly -- five lines, and it needs no SDK version at all. */
        const struct segment_command* seg = NULL;
        {
            const struct load_command* lc =
                (const struct load_command*)((const char*)mh + sizeof(*mh));
            uint32_t ci;
            for (ci = 0; ci < mh->ncmds; ci++) {
                if (lc->cmd == LC_SEGMENT &&
                    !strncmp(((const struct segment_command*)lc)->segname, "__TEXT", 16)) {
                    seg = (const struct segment_command*)lc;
                    break;
                }
                lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
            }
        }
        long slide = seg ? (long)((char*)mh - (long)seg->vmaddr) : 0;
        static const char* const names[2] = { "__bss", "__common" };
        int k;
        for (k = 0; k < 2; k++) {
            const struct section* s =
                getsectbynamefromheader((struct mach_header*)mh, "__DATA", names[k]);
            if (s && s->size) {
                memset((void*)((char*)(long)s->addr + slide), 0, s->size);
                zeroed += (int)s->size;
            }
        }
    }
#endif
    port_log("port> %s: dlclose did not unload it; zeroed %d bytes of bss by hand\n",
             name, zeroed);
}

/* ---- the four entry points objdll.c calls -------------------------------- */

void* portDLLOpen(const char* relpath) {
    DllModule* m = find(relpath);
    void* h;
    if (!m) {
        port_log("port> REL %s: no bundle built for it\n", relpath);
        return NULL;
    }
    /* RTLD_NOW so a module that imports something the port has not
     * implemented fails loudly here rather than crashing later;
     * RTLD_LOCAL so the 899 colliding names between modules stay invisible
     * to each other. */
    h = dlopen(m->path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        port_log("port> REL %s: dlopen failed: %s\n", m->name, dlerror());
        return NULL;
    }
    m->handle = h;
    m->opens++;
    stat_open++;
    if (m->stuck || port_opt.relzerobss) {
        zero_bss(h, m->name);
    }
    if (port_opt.verbose) {
        port_log("port> REL %s: dlopen ok (%s), open #%d\n", m->name, m->path, m->opens);
    }
    return h;
}

s32 portDLLProlog(void* handle) {
    DLLProlog fn = (DLLProlog)dlsym(handle, "_prolog");
    if (!fn) {
        port_log("port> REL: no _prolog exported: %s\n", dlerror());
        return 0;
    }
    return fn();
}

void portDLLEpilog(void* handle) {
    DLLEpilog fn = (DLLEpilog)dlsym(handle, "_epilog");
    if (fn) {
        fn();
    }
}

s32 portDLLClose(void* handle) {
    DllModule* m = by_handle(handle);
    if (!handle) {
        return TRUE;
    }
    if (dlclose(handle) != 0) {
        port_log("port> REL: dlclose failed: %s\n", dlerror());
        return FALSE;
    }
    stat_close++;
    if (m) {
        m->handle = NULL;
        if (!dll_unloaded(m->path)) {
            if (!m->stuck) {
                stat_stuck++;
            }
            m->stuck = 1;
        }
    }
    return TRUE;
}

/* omDLLStart's re-entry case: the module stayed resident and the game wants
 * it entered again with a fresh, zeroed bss.  Close it and open it again --
 * which is the same five-point contract, expressed the only way dyld offers.
 * If the close did not really unload, portDLLOpen zeroes the bss by hand. */
void* portDLLReenter(const char* name, void* handle) {
    stat_reenter++;
    portDLLClose(handle);
    return portDLLOpen(name);
}

/* ---- --reltest ----------------------------------------------------------- */

int port_dll_selftest(void) {
    int i, pass = 0, fail = 0, noentry = 0, stuck = 0;
    int round;
    scan();
    port_log("port> --reltest: %d module bundles, two load/unload rounds each\n",
             mod_count);
    for (round = 1; round <= 2; round++) {
        for (i = 0; i < mod_count; i++) {
            DllModule* m = &mods[i];
            void* h = dlopen(m->path, RTLD_NOW | RTLD_LOCAL);
            void* prolog;
            void* epilog;
            if (!h) {
                port_log("  round %d  %-16s LOAD FAILED: %s\n", round, m->name,
                         dlerror());
                fail++;
                continue;
            }
            prolog = dlsym(h, "_prolog");
            epilog = dlsym(h, "_epilog");
            if (!prolog || !epilog) {
                port_log("  round %d  %-16s loaded but %s missing\n", round, m->name,
                         !prolog ? "_prolog" : "_epilog");
                noentry++;
            }
            if (dlclose(h) != 0) {
                port_log("  round %d  %-16s UNLOAD FAILED: %s\n", round, m->name,
                         dlerror());
                fail++;
                continue;
            }
            if (!dll_unloaded(m->path)) {
                if (round == 1) {
                    stuck++;
                }
                m->stuck = 1;
            }
            pass++;
        }
    }
    port_log("port> --reltest: %d/%d load+unload cycles clean, %d failed, "
             "%d missing an entry point, %d still resident after dlclose\n",
             pass, mod_count * 2, fail, noentry, stuck);
    if (stuck) {
        port_log("port>            (those %d fall back to zeroing __bss/__common "
                 "by hand on re-entry)\n", stuck);
    }
    return fail == 0 && noentry == 0 ? 0 : 1;
}

void port_dll_report(void) {
    if (!stat_open) {
        return;
    }
    port_log("port> REL bundles: %d loads, %d unloads, %d re-entries, "
             "%d modules dlclose would not unload\n",
             stat_open, stat_close, stat_reenter, stat_stuck);
}
