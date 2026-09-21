/* M25: packaging groundwork -- the config file, the Application Support
 * paths, the dialogs (PLAN.md 40.4).
 *
 * The config file is `~/Library/Application Support/MarioParty4/config`,
 * next to the memory card image card_file.c already keeps there.  Its format,
 * written down before the code:
 *
 *     # Mario Party 4 -- settings remembered from the command line and the
 *     # dialogs.  One `key = value` a line; `#' starts a comment; unknown
 *     # keys are kept as they are.
 *     image = /Users/zach/MarioParty4/mp4.nkit.iso
 *     fullscreen = 1
 *     machine = ok: PowerMac3,5, 2 x 1000 MHz 7450 (G4), ...
 *
 *   image       the disc image (or extracted files/ tree) the game last ran
 *               from: written when --image is given or the file dialog picks
 *               one; read when neither --image, $MARIOPARTY4_IMAGE nor the
 *               bundle's Resources names one (main.c's search order)
 *   fullscreen  0 or 1: written by --fullscreen / --windowed, read otherwise
 *   machine     the machine check's one-line summary the last time the
 *               first-run message was shown; the message shows again when
 *               the summary changes (a new card, a new OS), not every boot
 *
 * Every key is optional and a missing file is an empty config.  Nothing here
 * is read by game code. */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__) && !defined(PORT_NO_SDL)
#include <CoreFoundation/CoreFoundation.h>
#include <Carbon/Carbon.h>
#endif

/* ---- the Application Support directory --------------------------------- */

static char app_dir[700];

const char* port_app_support_dir(void) {
    if (!app_dir[0]) {
        const char* home = getenv("HOME");
        char buf[700];
        /* every level, in case a home has no Library yet (a test user's;
         * M32's fresh-user walk had none and the card could not be written) */
        snprintf(buf, sizeof(buf), "%s/Library", home && *home ? home : ".");
        mkdir(buf, 0755);
        snprintf(buf, sizeof(buf), "%s/Library/Application Support",
                 home && *home ? home : ".");
        mkdir(buf, 0755);
        snprintf(app_dir, sizeof(app_dir), "%s/MarioParty4", buf);
        mkdir(app_dir, 0755);
    }
    return app_dir;
}

/* ---- the config file ---------------------------------------------------- */

#define CFG_MAX 32
#define CFG_KEY 32
#define CFG_VAL 1024

typedef struct CfgEntry {
    char key[CFG_KEY];
    char val[CFG_VAL];
} CfgEntry;

static CfgEntry cfg[CFG_MAX];
static int cfg_n;
static int cfg_loaded;
static int cfg_dirty;

static void cfg_path(char* out, size_t n) {
    snprintf(out, n, "%s/config", port_app_support_dir());
}

void port_config_load(void) {
    char path[800];
    char line[CFG_VAL + CFG_KEY + 8];
    FILE* f;
    if (cfg_loaded) {
        return;
    }
    cfg_loaded = 1;
    cfg_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f) && cfg_n < CFG_MAX) {
        char* p = line;
        char* eq;
        char* end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        end = eq;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';
        eq++;
        while (*eq == ' ' || *eq == '\t') {
            eq++;
        }
        end = eq + strlen(eq);
        while (end > eq && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
        snprintf(cfg[cfg_n].key, CFG_KEY, "%s", p);
        snprintf(cfg[cfg_n].val, CFG_VAL, "%s", eq);
        cfg_n++;
    }
    fclose(f);
    port_log("port> config: %s: %d setting(s)\n", path, cfg_n);
}

const char* port_config_get(const char* key) {
    int i;
    port_config_load();
    for (i = 0; i < cfg_n; i++) {
        if (!strcmp(cfg[i].key, key)) {
            return cfg[i].val[0] ? cfg[i].val : NULL;
        }
    }
    return NULL;
}

void port_config_set(const char* key, const char* val) {
    int i;
    port_config_load();
    for (i = 0; i < cfg_n; i++) {
        if (!strcmp(cfg[i].key, key)) {
            if (strcmp(cfg[i].val, val) != 0) {
                snprintf(cfg[i].val, CFG_VAL, "%s", val);
                cfg_dirty = 1;
            }
            return;
        }
    }
    if (cfg_n < CFG_MAX) {
        snprintf(cfg[cfg_n].key, CFG_KEY, "%s", key);
        snprintf(cfg[cfg_n].val, CFG_VAL, "%s", val);
        cfg_n++;
        cfg_dirty = 1;
    }
}

void port_config_save(void) {
    char path[800];
    FILE* f;
    int i;
    if (!cfg_dirty) {
        return;
    }
    cfg_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        port_log("port> config: cannot write %s\n", path);
        return;
    }
    fprintf(f, "# Mario Party 4 -- settings remembered from the command line and the\n"
               "# dialogs.  One `key = value' a line; `#' starts a comment.\n");
    for (i = 0; i < cfg_n; i++) {
        fprintf(f, "%s = %s\n", cfg[i].key, cfg[i].val);
    }
    fclose(f);
    cfg_dirty = 0;
    port_log("port> config: wrote %s\n", path);
}

/* ---- the dialogs -------------------------------------------------------- */

/* A modal notice with one OK button.  CFUserNotification needs a window
 * server session; from a plain ssh exec it fails, which is reported and
 * harmless (the same text is on the log and stderr). */
int port_dialog_notice(const char* title, const char* text) {
#if defined(__APPLE__) && !defined(PORT_NO_SDL)
    CFStringRef t = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
    CFStringRef m = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CFOptionFlags response = 0;
    SInt32 err;
    if (!t || !m) {
        return 0;
    }
    err = CFUserNotificationDisplayAlert(0.0, kCFUserNotificationNoteAlertLevel, NULL, NULL,
                                         NULL, t, m, CFSTR("OK"), NULL, NULL, &response);
    CFRelease(t);
    CFRelease(m);
    if (err != 0) {
        port_log("port> dialog: could not show \"%s\" (%d): no window server?\n", title,
                 (int)err);
        return 0;
    }
    return 1;
#else
    (void)title;
    (void)text;
    return 0;
#endif
}

/* The disc image chooser: Navigation Services (Carbon, 10.4+).  Any file;
 * the port checks that it is a disc image or a files/ tree afterwards.
 * Returns 1 with the path in `out`, 0 if cancelled or unavailable. */
int port_dialog_choose_image(char* out, size_t n) {
#if defined(__APPLE__) && !defined(PORT_NO_SDL)
    NavDialogCreationOptions opts;
    NavDialogRef dlg = NULL;
    NavReplyRecord reply;
    OSStatus err;
    int ok = 0;

    out[0] = '\0';
    if (NavGetDefaultDialogCreationOptions(&opts) != noErr) {
        return 0;
    }
    opts.optionFlags |= kNavAllowOpenPackages; /* a files/ tree may be a folder */
    opts.optionFlags &= ~kNavAllowMultipleFiles;
    opts.windowTitle = CFSTR("Mario Party 4: choose your disc image");
    opts.message = CFSTR("Choose the Mario Party 4 (USA, Rev 1) disc image (.iso), "
                         "or a folder holding an extracted files/ tree.");
    opts.modality = kWindowModalityAppModal;
    /* M32: a process started from a shell (the lab's runner, an ssh exec) is
     * not a foreground application until SDL makes it one, and a dialog it
     * runs before that is behind everything and gets no keys -- which is
     * why M25 could not drive this one.  A Finder launch is already
     * foreground; this is a no-op there. */
    {
        ProcessSerialNumber psn = {0, kCurrentProcess};
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);
        SetFrontProcess(&psn);
    }
    err = NavCreateChooseFileDialog(&opts, NULL, NULL, NULL, NULL, NULL, &dlg);
    if (err != noErr || !dlg) {
        port_log("port> dialog: NavCreateChooseFileDialog failed (%d)\n", (int)err);
        return 0;
    }
    err = NavDialogRun(dlg);
    if (err == noErr && NavDialogGetUserAction(dlg) == kNavUserActionChoose &&
        NavDialogGetReply(dlg, &reply) == noErr) {
        AEKeyword kw;
        DescType type;
        Size sz;
        FSRef ref;
        if (AEGetNthPtr(&reply.selection, 1, typeFSRef, &kw, &type, &ref, sizeof(ref), &sz) ==
            noErr) {
            if (FSRefMakePath(&ref, (UInt8*)out, (UInt32)n) == noErr) {
                ok = out[0] != '\0';
            }
        }
        NavDisposeReply(&reply);
    }
    NavDialogDispose(dlg);
    return ok;
#else
    (void)n;
    out[0] = '\0';
    return 0;
#endif
}
