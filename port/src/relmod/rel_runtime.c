/* Compiled into every one of the 99 REL bundles, never into the main binary.
 *
 * On the console the REL linker synthesised `.ctors` and `.dtors` sections and
 * `src/REL/executor.c` walks them from `_prolog`/`_epilog`.  Nothing in this
 * game is C++, so every module's list is empty -- the decomp's own per-module
 * symbols files show `_ctors`/`_dtors` at offset 0 of
 * a zero-length section in all 99 -- but the arrays still have to exist and
 * still have to be NULL-terminated, because `executor.c` walks until it reads
 * a zero.  Two words per module.
 *
 * They are deliberately *not* in the bundle's export list, so each module gets
 * its own pair and no module can see another's: exactly the private namespace
 * that made one-bundle-per-REL the right answer to the 899 colliding symbol
 * names (PLAN.md §2.4).
 */
typedef void (*VoidFunc)(void);

const VoidFunc _ctors[] = { 0 };
const VoidFunc _dtors[] = { 0 };

/* `_unresolved` is the third entry point in an OSModuleHeader.  The console's
 * REL linker synthesised it and pointed every import it could not satisfy at
 * it, so a module that called a missing DOL function trapped there instead of
 * branching into nothing.  A bundle linked with `-bundle_loader` cannot reach
 * that state -- an unsatisfied import is a link error at build time and, with
 * RTLD_NOW, a loud dlopen failure at run time -- but the symbol is exported
 * anyway so the module's three entry points are all present and the loader's
 * dump means the same thing it always did. */
void _unresolved(void);
void OSPanic(const char* file, int line, const char* msg, ...);

void _unresolved(void) {
    OSPanic(__FILE__, __LINE__, "REL module called an unresolved import");
}

/* A weak `ObjectSetup`, for the one module that has neither.
 *
 * `safDll` is 71 lines of save-file helpers with no entry thunk and no
 * `ObjectSetup` anywhere in its symbols -- on the disc its REL has no
 * `_prolog` either, so the game cannot ever have entered it through
 * `omDLLStart`.  The bundle still needs the symbol to link, and a weak
 * definition supplies it without hiding a real one: every other module's own
 * strong `ObjectSetup` wins.
 */
void ObjectSetup(void);
void OSReport(const char* fmt, ...);

__attribute__((weak)) void ObjectSetup(void) {
    OSReport("port> REL: entered a module with no ObjectSetup of its own\n");
}
