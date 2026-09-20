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
 * The bss was only half of point 5 (M20, PLAN.md §35).  On the console the
 * `Link DLL` path reads the REL off the disc again, so its initialised data
 * starts every play as the linker wrote it; a bundle kept mapped keeps the
 * last play's `.data`, and m406dll's intro countdown -- an initialised
 * global counted down past zero by its outro -- began its second play at
 * -333 and never reached zero again.  So a re-open of a kept module also puts
 * `__data` back to a copy taken at its first dlopen (`dll_data_reset`).
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
    void* image;        /* the loaded mach_header, once it has been opened  */
    void* first_handle; /* what dlopen returned the first time: the module's
                         * identity, which a later dlopen of the same path
                         * returns again.  `handle` is NULL while the game has
                         * the module unlinked, so it is the wrong thing for a
                         * snapshot to compare (PLAN.md 24.2). */
    int ordered;        /* it is in load_order */
    /* M20: the module's `__data` as dlopen left it -- the REL's on-disc
     * initialised globals, relocated.  On the console every `Link DLL` reads
     * the REL from the disc again, so a module's `.data` starts each play as
     * the linker wrote it; a bundle the port keeps mapped keeps the last
     * play's values instead (m406dll's intro countdown, PLAN.md §35).  A
     * re-open copies this back, next to the bss it zeroes. */
    unsigned char* data_init;
    void* data_addr;
    unsigned long data_len;
} DllModule;

/* The order in which modules were first mapped.  A snapshot replays exactly
 * this list (PLAN.md §24.2): the game keeps pointers into module text and
 * holds the dlopen handle itself in `omDllData.bss`, so a restore is only
 * sound if every module comes back at the address it had.  Since the port
 * never really unloads a module (see portDLLClose), that address is decided
 * by the order of first loads and nothing else. */
static DllModule* load_order[256];
static int load_order_n;

static DllModule* mods;
static int mod_count;
static int scanned;

/* counters, printed by port_dll_report() */
static int stat_open, stat_close, stat_reenter, stat_stuck;
static int stat_datareset, stat_datachanged; /* re-opens; of them, .data was dirty */

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

/* The token the *game* holds (in `omDllData.bss`) is the module's loaded
 * mach_header, not dyld's `dlopen` handle.  The handle is a malloc'd object
 * inside dyld, so its address differs from run to run even when everything
 * else about the run is identical -- which made a snapshot unrestorable, since
 * the game's own memory carries the token and a restore cannot rewrite it
 * (PLAN.md 24.2).  The header address is fixed by the bundle's own
 * `-seg1addr` and is therefore the same in every run of the same build.  The
 * real handle stays here, in the loader's table. */
static DllModule* by_token(void* token) {
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].image == token && token != NULL) {
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
            const struct section* sec =
                getsectbynamefromheader((struct mach_header*)mh, "__DATA", names[k]);
            if (sec && sec->size) {
                memset((void*)((char*)(long)sec->addr + slide), 0, sec->size);
                zeroed += (int)sec->size;
            }
        }
    }
#endif
    port_log("port> %s: dlclose did not unload it; zeroed %d bytes of bss by hand\n",
             name, zeroed);
}

/* The loaded image a handle belongs to, found the same way `zero_bss` finds
 * it: through the module's own `_prolog`, so no name lookup and no dladdr in
 * a hot path (this runs once per module, at its first load). */
static void* dll_image_of(void* handle) {
    void* prolog = dlsym(handle, "_prolog");
    Dl_info info;
    if (!prolog || !dladdr(prolog, &info)) {
        return NULL;
    }
    return info.dli_fbase;
}

/* ---- .data across plays (M20, PLAN.md §35) --------------------------------
 * `__DATA,__data` of a loaded image: its address and size, slide applied. */
static void dll_data_section(void* image, void** addr, unsigned long* size) {
    *addr = NULL;
    *size = 0;
    if (!image) {
        return;
    }
#ifdef __LP64__
    {
        unsigned long n = 0;
        char* p = (char*)getsectiondata((const struct mach_header_64*)image, "__DATA",
                                        "__data", &n);
        if (p && n) {
            *addr = p;
            *size = n;
        }
    }
#else
    {
        const struct mach_header* mh = (const struct mach_header*)image;
        const struct segment_command* seg = NULL;
        const struct load_command* lc =
            (const struct load_command*)((const char*)mh + sizeof(*mh));
        const struct section* sec;
        uint32_t ci;
        long slide;
        for (ci = 0; ci < mh->ncmds; ci++) {
            if (lc->cmd == LC_SEGMENT &&
                !strncmp(((const struct segment_command*)lc)->segname, "__TEXT", 16)) {
                seg = (const struct segment_command*)lc;
                break;
            }
            lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
        }
        slide = seg ? (long)((char*)mh - (long)seg->vmaddr) : 0;
        sec = getsectbynamefromheader((struct mach_header*)mh, "__DATA", "__data");
        if (sec && sec->size) {
            *addr = (char*)(long)sec->addr + slide;
            *size = sec->size;
        }
    }
#endif
}

/* Once, at the module's first dlopen in this process, before its prolog has
 * run: what the REL's initialised data holds when it is freshly linked. */
static void dll_data_capture(DllModule* m) {
    if (m->data_init) {
        return;
    }
    dll_data_section(m->image, &m->data_addr, &m->data_len);
    if (!m->data_addr || !m->data_len) {
        return;
    }
    m->data_init = malloc(m->data_len);
    if (!m->data_init) {
        m->data_len = 0;
        return;
    }
    memcpy(m->data_init, m->data_addr, m->data_len);
    if (port_opt.verbose) {
        port_log("port> REL %s: %lu bytes of .data captured at first load\n", m->name,
                 m->data_len);
    }
}

/* A `Link DLL` of a module the port kept mapped: the console would be reading
 * the REL off the disc again, so `.data` goes back to what it held.  The line
 * this logs is the guard for the next stall of the m406 kind -- it names the
 * module and how much of its `.data` the last play had changed. */
static void dll_data_reset(DllModule* m) {
    unsigned long i, changed = 0;
    if (!m->data_init || !m->data_len) {
        return;
    }
    for (i = 0; i < m->data_len; i++) {
        if (m->data_init[i] != ((unsigned char*)m->data_addr)[i]) {
            changed++;
        }
    }
    if (port_opt.nodatareset) {
        if (changed) {
            port_log("port> %s: --nodatareset: %lu of %lu .data bytes carried over "
                     "from the last play (not reset)\n", m->name, changed, m->data_len);
        }
        return;
    }
    stat_datareset++;
    if (changed) {
        stat_datachanged++;
        memcpy(m->data_addr, m->data_init, m->data_len);
        port_log("port> %s: re-opened: %lu of %lu .data bytes had been changed by "
                 "the last play; reset to the REL's contents\n", m->name, changed,
                 m->data_len);
    }
}

/* ---- snapshots (PLAN.md 24.2) --------------------------------------------
 * A module's `__data`, `__bss` and `__common` are its entire state: the REL's
 * own globals.  The other sections of `__DATA` are dyld's (the lazy and
 * non-lazy symbol pointers), and those are deliberately left out of a
 * snapshot -- they are bindings into libSystem and into the main binary, and
 * they belong to the process, not to the run. */
#ifdef __LP64__
static void dll_data_bounds(void* image, void** lo, unsigned long* size) {
    const struct mach_header_64* mh = (const struct mach_header_64*)image;
    static const char* const names[3] = { "__data", "__bss", "__common" };
    char *blo = NULL, *bhi = NULL;
    int k;
    *lo = NULL;
    *size = 0;
    if (!mh) {
        return;
    }
    for (k = 0; k < 3; k++) {
        unsigned long n = 0;
        char* p = (char*)getsectiondata(mh, "__DATA", names[k], &n);
        if (p && n) {
            if (!blo || p < blo) {
                blo = p;
            }
            if (!bhi || p + n > bhi) {
                bhi = p + n;
            }
        }
    }
    if (blo && bhi > blo) {
        *lo = blo;
        *size = (unsigned long)(bhi - blo);
    }
}
#else
static void dll_data_bounds(void* image, void** lo, unsigned long* size) {
    const struct mach_header* mh = (const struct mach_header*)image;
    static const char* const names[3] = { "__data", "__bss", "__common" };
    const struct segment_command* seg = NULL;
    const struct load_command* lc;
    uint32_t ci;
    long slide = 0;
    char* blo = NULL;
    char* bhi = NULL;
    int k;

    *lo = NULL;
    *size = 0;
    if (!mh) {
        return;
    }
    lc = (const struct load_command*)((const char*)mh + sizeof(*mh));
    for (ci = 0; ci < mh->ncmds; ci++) {
        if (lc->cmd == LC_SEGMENT &&
            !strncmp(((const struct segment_command*)lc)->segname, "__TEXT", 16)) {
            seg = (const struct segment_command*)lc;
            break;
        }
        lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
    }
    slide = seg ? (long)((char*)mh - (long)seg->vmaddr) : 0;
    for (k = 0; k < 3; k++) {
        const struct section* sec =
            getsectbynamefromheader((struct mach_header*)mh, "__DATA", names[k]);
        if (sec && sec->size) {
            char* s = (char*)(long)sec->addr + slide;
            char* e = s + sec->size;
            if (!blo || s < blo) {
                blo = s;
            }
            if (!bhi || e > bhi) {
                bhi = e;
            }
        }
    }
    if (blo && bhi > blo) {
        *lo = blo;
        *size = (unsigned long)(bhi - blo);
    }
}
#endif

int port_dll_snap_count(void) { return load_order_n; }

int port_dll_snap_get(int i, const char** name, void** handle, void** image,
                      void** data_lo, unsigned long* data_size, int* open,
                      int* stuck) {
    DllModule* m;
    if (i < 0 || i >= load_order_n) {
        return 0;
    }
    m = load_order[i];
    *name = m->name;
    *handle = m->first_handle;
    *image = m->image;
    *open = m->handle != NULL;
    *stuck = m->stuck;
    dll_data_bounds(m->image, data_lo, data_size);
    return 1;
}

/* --restore: map a module again, in the snapshot's order.  The caller checks
 * that it came back at the same address; this only has to be the same call the
 * original run made, in the same sequence, so that dyld makes the same
 * decisions. */
int port_dll_snap_reopen(const char* name, void** handle, void** image,
                         void** data_lo, unsigned long* data_size, int open,
                         int stuck) {
    DllModule* m = find(name);
    void* h;
    if (!m) {
        return 0;
    }
    h = dlopen(m->path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        port_log("port> --restore: dlopen %s failed: %s\n", m->path, dlerror());
        return 0;
    }
    m->opens++;
    if (!m->ordered) {
        m->image = dll_image_of(h);
        m->first_handle = h;
        m->ordered = 1;
        if (load_order_n < (int)(sizeof(load_order) / sizeof(load_order[0]))) {
            load_order[load_order_n++] = m;
        }
        /* fresh in this process: the REL's own .data, before the snapshot's
         * is written over it */
        dll_data_capture(m);
    }
    /* The module is mapped either way -- the port never really unloads one --
     * but the game's own view of it comes back from the snapshot, so the
     * loader's table has to agree: unlinked stays unlinked. */
    m->handle = open ? h : NULL;
    m->stuck = stuck;
    *handle = m->first_handle;
    *image = m->image;
    dll_data_bounds(m->image, data_lo, data_size);
    return 1;
}

/* ---- the four entry points objdll.c calls -------------------------------- */

/* `fresh`: this is objdll.c's `Link DLL` path, where the console reads the REL
 * off the disc again -- so a kept mapping gets its .data put back as well as
 * its bss zeroed.  The "Already Loaded" re-entry (portDLLReenter) memsets the
 * bss only, on the console too, and keeps .data. */
static void* dll_open(const char* relpath, int fresh) {
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
    if (!m->ordered) {
        m->image = dll_image_of(h);
        m->first_handle = h;
        m->ordered = 1;
        if (load_order_n < (int)(sizeof(load_order) / sizeof(load_order[0]))) {
            load_order[load_order_n++] = m;
        }
        dll_data_capture(m);
    }
    if (m->stuck || port_opt.relzerobss) {
        zero_bss(h, m->name);
        if (fresh) {
            dll_data_reset(m);
        }
    }
    if (port_opt.verbose) {
        port_log("port> REL %s: dlopen ok (%s), open #%d\n", m->name, m->path, m->opens);
    }
    if (!m->image) {
        port_log("port> REL %s: loaded, but its image address is unknown -- "
                 "snapshots of this run cannot be restored\n", m->name);
        return h;
    }
    return m->image;
}

double port_frame_dll_s; /* M24: time in portDLLOpen/Reenter this frame (perf.c's stall line) */
void* portDLLOpen(const char* relpath) {
    double t0 = port_now_seconds();
    void* h = dll_open(relpath, 1);
    port_frame_dll_s += port_now_seconds() - t0;
    return h;
}

s32 portDLLProlog(void* token) {
    DllModule* m = by_token(token);
    void* handle = m && m->handle ? m->handle : token;
    DLLProlog fn = (DLLProlog)dlsym(handle, "_prolog");
    if (!fn) {
        port_log("port> REL: no _prolog exported: %s\n", dlerror());
        return 0;
    }
    return fn();
}

void portDLLEpilog(void* token) {
    DllModule* m = by_token(token);
    void* handle = m && m->handle ? m->handle : token;
    DLLEpilog fn = (DLLEpilog)dlsym(handle, "_epilog");
    if (fn) {
        fn();
    }
}

/* Unlinking a module does not unmap it, and that is deliberate.
 *
 * `dlclose` really does unload a bundle -- M2a's `--reltest` proved it, twice,
 * on the real machine, and that was the right answer to risk 2.  It is the
 * wrong answer for the game.  On the console `OSUnlink` relocates nothing away
 * and `objdll.c` then `HuMemDirectFree`s the module's memory: freed heap is
 * still readable and still executable, so a pointer the game left behind into
 * a module it has just unlinked keeps working until something allocates over
 * it.  Mario Party 4 leaves exactly such a pointer.  Accepting the title
 * screen unlinks `bootDll` and the very next thing the game does is call into
 * its dead text -- M3 caught it as `signal 11 at 0x9ff004` with a program
 * counter no image claimed, one instruction into a bundle dyld had genuinely
 * thrown away.  On the console that call lands on code that is still there.
 *
 * So the port keeps the mapping and drops only the reference: the module is
 * marked unlinked, and a later `omDLLStart` re-enters it through the same
 * path a resident module always took, with its bss zeroed by hand.  That is
 * the console's behaviour, only more reliably.  It costs address space --
 * ninety-nine bundles, the largest 36 KB -- which a 32-bit machine has plenty
 * of.  `--reldlclose` restores the strict close, which is what `--reltest`
 * runs with, so the unload path stays proven rather than merely remembered.
 */
s32 portDLLClose(void* token) {
    DllModule* m = by_token(token);
    void* handle = m ? m->handle : token;
    if (!token) {
        return TRUE;
    }
    if (!port_opt.reldlclose) {
        stat_close++;
        if (m) {
            m->handle = NULL;
            if (!m->stuck) {
                m->stuck = 1; /* so a re-open zeroes the bss by hand */
                if (port_opt.verbose) {
                    port_log("port> REL %s: unlinked, kept mapped (see "
                             "portDLLClose)\n", m->name);
                }
            }
        }
        return TRUE;
    }
    if (handle == NULL || dlclose(handle) != 0) {
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
void* portDLLReenter(const char* name, void* token) {
    stat_reenter++;
    portDLLClose(token);
    return dll_open(name, 0); /* bss only: the console's memset, .data kept */
}

/* How many modules are mapped but unlinked, for the shutdown report -- the
 * number that says what keeping them costs. */
int port_dll_resident_count(void) {
    int i, n = 0;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].stuck && !mods[i].handle) {
            n++;
        }
    }
    return n;
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
    if (stat_datareset || port_opt.nodatareset) {
        port_log("port> REL .data: %d re-opens of a kept module, %d of them with "
                 ".data the last play had changed (reset%s)\n",
                 stat_datareset, stat_datachanged,
                 port_opt.nodatareset ? " OFF: --nodatareset" : "");
    }
}
