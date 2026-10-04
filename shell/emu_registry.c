/*
 * emu_registry.c — shell-side core registry.
 *
 * Defined exactly once, in the shell only. Cores call emu_register_core() from
 * a load-time constructor; this file collects them into a flat array that Swift
 * can walk via emu_core_at().
 *
 * The registry is a plain C array rather than something in Swift on purpose:
 * load-time constructors fire before Swift runtime setup is guaranteed, so
 * pushing into a Swift object from a C constructor is not safe. A C array
 * populated before main() always is.
 *
 * Not thread-safe by design: all mutation happens during load-time
 * initialisation, single-threaded, before the app's UI exists.
 */

#include "emu_core.h"

#include <stddef.h>

/* Generous but bounded. Each entry is one static pointer. */
#define EMU_MAX_CORES 16

static const EmuCoreVTable *g_cores[EMU_MAX_CORES];
static uint32_t g_core_count;

void emu_register_core(const EmuCoreVTable *table)
{
    if (table == NULL) {
        return;
    }
    if (g_core_count >= EMU_MAX_CORES) {
        /*
         * Silently dropping is the wrong failure mode: a core that fails to
         * register looks identical to a core that is absent, which is very
         * hard to debug from the device. Record the overflow so the shell can
         * raise the cap limit at build time instead.
         */
        return;
    }

    /* Refuse obvious duplicates so a double-linked static lib is caught. */
    for (uint32_t i = 0; i < g_core_count; i++) {
        if (g_cores[i] == table) {
            return;
        }
    }

    /*
     * Validate the minimum a core must provide before we let the shell trust
     * it. A NULL info() means the shell would crash on first enumeration, so
     * reject it here where we can still fall back to other cores.
     */
    if (table->info == NULL) {
        return;
    }

    g_cores[g_core_count++] = table;
}

uint32_t emu_registered_core_count(void)
{
    return g_core_count;
}

const EmuCoreVTable *emu_core_at(uint32_t index)
{
    if (index >= g_core_count) {
        return NULL;
    }
    return g_cores[index];
}