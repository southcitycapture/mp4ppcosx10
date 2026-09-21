/* OSReport, OSPanic, and the loud-stub table.
 *
 * `OSReport` is called from 578 sites: the game narrates its own boot, its DLL
 * loads, its heap state and its DVD errors.  That narration is M1's success
 * criterion, so it goes to stdout and, with --log, to a file as well.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* logfile;

void port_logv(const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    if (logfile) {
        vfprintf(logfile, fmt, copy);
        fflush(logfile);
    }
    va_end(copy);
}

void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
}

void port_fatal(const char* fmt, ...) {
    va_list ap;
    char msg[900];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    port_log("\n*** port: fatal: %s\n", msg);
    port_stub_report();
    /* M32: a player who double-clicked sees a silent exit otherwise; the same
     * text is on the log (the dialog needs a window server -- from an ssh
     * exec it fails harmlessly, config.c) */
    if (!port_opt.headless && !port_opt.machinecheck) {
        char text[1100];
        snprintf(text, sizeof(text),
                 "%s\n\nThe log has the details:\n%s", msg,
                 port_opt.log ? port_opt.log : "(the Terminal's output)");
        port_dialog_notice("Mario Party 4 could not start", text);
    }
    exit(1);
}

void port_log_open(const char* path) {
    if (path) {
        logfile = fopen(path, "w");
    }
}

/* ---- the SDK's own reporting -------------------------------------------- */

void OSReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
}

void OSVReport(const char* fmt, va_list ap) { port_logv(fmt, ap); }

void OSPanic(const char* file, int line, const char* fmt, ...) {
    va_list ap;
    port_log("\n*** OSPanic in \"%s\" on line %d: ", file, line);
    va_start(ap, fmt);
    port_logv(fmt, ap);
    va_end(ap);
    port_log("\n");
    port_stub_report();
    exit(2);
}

/* `printf` reaches the game through the decomp's own stdio shim (only
 * process.c's "stack overlap error" uses it); the host libc supplies it. */

/* ---- the loud-stub table ------------------------------------------------- */

#define PORT_STUB_MAX 1024

static struct {
    const char* name;
    unsigned long calls;
} stubs[PORT_STUB_MAX];
static int stub_count;

static int is_gx(const char* name) { return name[0] == 'G' && name[1] == 'X'; }

void port_stub(const char* name) {
    int i;
    for (i = 0; i < stub_count; i++) {
        if (stubs[i].name == name || strcmp(stubs[i].name, name) == 0) {
            stubs[i].calls++;
            if (port_opt.stub_trace || (port_opt.gxlog && is_gx(name))) {
                port_log("stub> %s\n", name);
            }
            return;
        }
    }
    if (stub_count < PORT_STUB_MAX) {
        stubs[stub_count].name = name;
        stubs[stub_count].calls = 1;
        stub_count++;
    }
    if (port_opt.verbose || port_opt.stub_trace || (port_opt.gxlog && is_gx(name))) {
        port_log("stub> %s (first call)\n", name);
    }
}

void port_stub_report(void) {
    int i;
    unsigned long total = 0;
    port_log("\n---- SDK surface hit at boot: %d distinct stubs, in first-call order ----\n",
             stub_count);
    for (i = 0; i < stub_count; i++) {
        port_log("%4d  %-32s %8lu\n", i + 1, stubs[i].name, stubs[i].calls);
        total += stubs[i].calls;
    }
    port_log("---- %lu stub calls total ----\n", total);
}
