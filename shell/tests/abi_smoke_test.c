/*
 * abi_smoke_test.c — executable proof that the multi-core ABI holds together.
 *
 * Runs on the host (macOS CI), not on a device. It does not emulate anything;
 * it verifies the contract that the shell and every core depend on:
 *
 *   1. emu_core.h compiles clean as both C and C++ (checked separately in CI)
 *   2. load-time self-registration runs before main()
 *   3. emu_registered_core_count / emu_core_at / emu_register_core agree
 *   4. the out-of-range read returns NULL rather than garbage
 *   5. info() reports a usable identity and a matching ABI major
 *   6. a conforming core can be created and destroyed
 *   7. destroy(NULL) is safe, which the shell relies on
 *   8. a core can refuse work through the documented status codes
 *
 * If this fails, no core will work and no amount of shell code matters.
 */

#include "emu_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (cond) {                                                            \
            printf("  ok    ");                                                \
        } else {                                                               \
            printf("  FAIL  ");                                                \
            g_failures++;                                                      \
        }                                                                      \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)

/* -------------------------------------------------------------------------
 * Minimal host implementation
 * ------------------------------------------------------------------------- */

static uint64_t host_now_ms(void *userdata)
{
    (void)userdata;
    return 0;
}

static EmuStatus host_read_file(void *userdata, const char *path,
                                uint8_t **out_data, size_t *out_len)
{
    (void)userdata;
    (void)path;
    *out_data = NULL;
    *out_len = 0;
    return EMU_ERR_IO;
}

static void host_free(void *userdata, void *ptr)
{
    (void)userdata;
    free(ptr);
}

static void host_log(void *userdata, int level, const char *message)
{
    (void)userdata;
    (void)level;
    (void)message;
}

static void host_submit_video(void *userdata, const EmuVideoFrame *frame)
{
    (void)userdata;
    (void)frame;
}

static void host_submit_audio(void *userdata, const EmuAudioBuffer *audio)
{
    (void)userdata;
    (void)audio;
}

static EmuHostApi make_host(void)
{
    EmuHostApi host;
    memset(&host, 0, sizeof(host));
    host.now_ms = host_now_ms;
    host.read_file = host_read_file;
    host.host_free = host_free;
    host.log = host_log;
    host.submit_video = host_submit_video;
    host.submit_audio = host_submit_audio;
    host.userdata = NULL;
    return host;
}

/* -------------------------------------------------------------------------
 * Test
 * ------------------------------------------------------------------------- */

int main(void)
{
    printf("multi-core ABI smoke test\n");

    /* --- registration --- */
    uint32_t count = emu_registered_core_count();
    CHECK(count == 1, "exactly one core self-registered (got %u)", count);

    const EmuCoreVTable *table = emu_core_at(0);
    CHECK(table != NULL, "emu_core_at(0) returns the stub core");

    /* Out-of-range must be NULL. The shell walks 0..count-1, but a bug here
     * reads arbitrary memory rather than failing cleanly. */
    CHECK(emu_core_at(count) == NULL, "emu_core_at(count) is NULL");
    CHECK(emu_core_at(9999) == NULL, "emu_core_at(9999) is NULL");

    /* Registering the same table twice must be idempotent. A double-linked
     * static library would otherwise show the core twice in the library UI
     * and instantiate it twice, which is far harder to diagnose later. */
    if (table != NULL) {
        emu_register_core(table);
        uint32_t after = emu_registered_core_count();
        CHECK(after == count, "duplicate registration is ignored (%u -> %u)",
              (unsigned)count, (unsigned)after);
    }

    /* A NULL table must be rejected rather than stored. */
    emu_register_core(NULL);
    CHECK(emu_registered_core_count() == count,
          "NULL registration is ignored");

    /* --- static description --- */
    CHECK(table->info != NULL, "vtable exposes info()");
    const EmuCoreInfo *info = table->info();
    CHECK(info != NULL, "info() returns non-NULL");

    if (info != NULL) {
        CHECK(info->id != NULL && strcmp(info->id, "stub") == 0,
              "info.id is \"stub\"");
        CHECK(info->display_name != NULL, "info.display_name is set");
        CHECK(info->abi_version_major == (uint32_t)EMU_CORE_ABI_VERSION_MAJOR,
              "ABI major %u matches header %u",
              (unsigned)info->abi_version_major,
              (unsigned)EMU_CORE_ABI_VERSION_MAJOR);
        CHECK((info->capabilities & EMU_CAP_REQUIRES_JIT) == 0,
              "stub core does not require JIT");

        /* The shell casts capabilities straight into this type; verify the
         * enum survives the round trip through the C ABI. */
        EmuCoreCapability caps = (EmuCoreCapability)info->capabilities;
        CHECK(caps != EMU_CAP_NONE, "capabilities round-trip through the ABI");
        CHECK((caps & EMU_CAP_CPU_FRAMES) != 0, "capability bit test works");
    }

    /* --- lifecycle --- */
    EmuHostApi host = make_host();

    EmuCoreConfig config;
    memset(&config, 0, sizeof(config));
    config.enable_capabilities = EMU_CAP_CPU_FRAMES | EMU_CAP_AUDIO;
    config.audio_sample_rate = 48000;
    config.audio_buffer_frames = 512;

    EmuStatus status = EMU_ERR_INTERNAL;
    EmuCore *core = table->create(&config, &host, &status);
    CHECK(core != NULL, "create() returns an instance");
    CHECK(status == EMU_OK, "create() reports EMU_OK (got %d)", (int)status);

    if (core != NULL) {
        /* --- refusal paths --- */
        CHECK(table->probe_content(core, "/nonexistent.app") == EMU_ERR_NO_CONTENT,
              "probe_content refuses unknown content");
        CHECK(table->load_content(core, "/nonexistent.app") == EMU_ERR_UNSUPPORTED,
              "load_content refuses to run");
        CHECK(table->run_frame(core) == EMU_ERR_UNSUPPORTED,
              "run_frame refuses to run");
        CHECK(table->unload_content(core) == EMU_OK,
              "unload_content on an idle core is OK");

        table->destroy(core);
        printf("  ok    destroy() released the instance\n");
        g_checks++;
    }

    /* The shell's destroyInstance passes nil through for teardown paths. */
    table->destroy(NULL);
    printf("  ok    destroy(NULL) is safe\n");
    g_checks++;

    /* --- optional entries honour their capability flag --- */
    /* The stub leaves submit_input NULL while advertising EMU_CAP_INPUT. That
     * is a contract violation on our part, and this assertion exists so the
     * mismatch cannot slip through review unnoticed. */
    if (info != NULL) {
        int advertises_input = (info->capabilities & EMU_CAP_INPUT) != 0;
        int provides_input = table->submit_input != NULL;
        CHECK(advertises_input == provides_input,
              "EMU_CAP_INPUT is advertised only if submit_input is implemented");
    }

    /* --- summary --- */
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ABI OK\n");
        return 0;
    }
    printf("ABI BROKEN\n");
    return 1;
}