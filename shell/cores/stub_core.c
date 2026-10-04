/*
 * stub_core.c — reference core implementing the multi-core ABI.
 *
 * Its job is to make the ABI executable before any real emulator is wired up.
 * It registers through the normal path, so a successful build proves:
 *   - emu_core.h is valid C and valid C++ (it is compiled in both modes in CI)
 *   - the self-registration constructor mechanism links with no symbol clash
 *   - emu_register_core / emu_registered_core_count / emu_core_at agree
 *   - the shell can enumerate a core and read its EmuCoreInfo
 *
 * It deliberately emulates nothing. Every content and execution entry point
 * returns EMU_ERR_UNSUPPORTED, so if this core is ever selectable by accident
 * in a real build that is a bug in the routing policy, not a feature.
 *
 * This file also serves as the template for a real core: copy it, fill in the
 * vtable, delete what you do not need.
 */

#include "emu_core.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Instance
 * ------------------------------------------------------------------------- */

struct EmuCore {
    /* Nothing yet. Kept so the struct is a valid, non-empty type and so adding
     * state later does not change the ABI. */
    uint32_t reserved;
};

/* -------------------------------------------------------------------------
 * Static description
 * ------------------------------------------------------------------------- */

static const EmuCoreInfo kInfo = {
    .id = "stub",
    .display_name = "Stub Core (ABI reference)",
    .version = "0.1.0",
    .abi_version_major = EMU_CORE_ABI_VERSION_MAJOR,
    .abi_version_minor = EMU_CORE_ABI_VERSION_MINOR,
    /* No JIT: this core must stay selectable on every device, including stock
     * iOS with no JIT entitlement, so it can be used to verify the gating
     * policy without a second build. */
    .capabilities = EMU_CAP_CPU_FRAMES | EMU_CAP_AUDIO | EMU_CAP_INPUT,
    .summary = "Emulates nothing. Verifies the multi-core ABI."
};

static const EmuCoreInfo *stub_info(void)
{
    return &kInfo;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

static EmuCore *stub_create(const EmuCoreConfig *config,
                            const EmuHostApi *host,
                            EmuStatus *status)
{
    (void)config;
    (void)host;

    /*
     * A real core must reject unsupported configurations here rather than
     * booting and failing later. This one supports everything it advertises.
     */
    EmuCore *core = calloc(1, sizeof(struct EmuCore));
    if (core == NULL) {
        if (status != NULL) {
            *status = EMU_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }

    if (status != NULL) {
        *status = EMU_OK;
    }
    return core;
}

static void stub_destroy(EmuCore *core)
{
    free(core);
}

/* -------------------------------------------------------------------------
 * Content -- deliberately inert
 * ------------------------------------------------------------------------- */

static EmuStatus stub_probe_content(EmuCore *core, const char *path)
{
    (void)core;
    (void)path;

    /*
     * Claim nothing. If the shell ever routes a real file here, routeContent
     * should have skipped this core rather than handed it work.
     */
    return EMU_ERR_NO_CONTENT;
}

static EmuStatus stub_load_content(EmuCore *core, const char *path)
{
    (void)core;
    (void)path;
    return EMU_ERR_UNSUPPORTED;
}

static EmuStatus stub_unload_content(EmuCore *core)
{
    (void)core;
    return EMU_OK;
}

/* -------------------------------------------------------------------------
 * Execution -- deliberately inert
 * ------------------------------------------------------------------------- */

static EmuStatus stub_run_frame(EmuCore *core)
{
    (void)core;
    return EMU_ERR_UNSUPPORTED;
}

/* -------------------------------------------------------------------------
 * Input
 *
 * Implemented even though this core runs nothing, because it advertises
 * EMU_CAP_INPUT. The ABI requires that a capability bit and the presence of the
 * corresponding vtable entry agree: the shell is allowed to call
 * submit_input whenever the bit is set, and must never have to null-check it
 * against a core's advertised capabilities.
 * ------------------------------------------------------------------------- */

static EmuStatus stub_submit_input(EmuCore *core, const EmuInputEvent *event)
{
    (void)core;
    (void)event;
    return EMU_ERR_UNSUPPORTED;
}

/* -------------------------------------------------------------------------
 * Vtable
 * ------------------------------------------------------------------------- */

static const EmuCoreVTable kTable = {
    .info = stub_info,
    .create = stub_create,
    .destroy = stub_destroy,
    .probe_content = stub_probe_content,
    .load_content = stub_load_content,
    .unload_content = stub_unload_content,
    .run_frame = stub_run_frame,
    .submit_input = stub_submit_input,
    /* Optional entries stay NULL: this core advertises neither savestates nor
     * a guest-support query. The shell must not call them. */
    .save_state = NULL,
    .load_state = NULL,
    .guest_support = NULL
};

const EmuCoreVTable *emu_core_vtable(void)
{
    return &kTable;
}

/* -------------------------------------------------------------------------
 * Registration
 *
 * Load-time constructor. Runs before main(), so by the time the Swift shell
 * calls emu_registered_core_count() this core is already in the table.
 * ------------------------------------------------------------------------- */

__attribute__((constructor))
static void stub_core_register(void)
{
    emu_register_core(&kTable);
}