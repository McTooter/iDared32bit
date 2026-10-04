/*
 * emu_core.h — Multi-core emulator ABI for the unified iOS shell.
 *
 * Purpose
 * -------
 * The shell is a single iOS app that hosts several emulator cores. Cores are
 * written in different languages (Rust, C++, C) and must not leak those
 * differences into the shell. Every core therefore compiles to a native static
 * library for arm64-apple-ios and exports exactly one flat C symbol:
 *
 *     const EmuCoreVTable *emu_core_vtable(void);
 *
 * The shell links that symbol at build time and talks to nothing else. This is
 * the same host/core split libretro uses, and it keeps the "one app" boundary
 * language-neutral.
 *
 * This header is an interface only. It carries no license obligation on core
 * implementations, which remain under their own licenses (Vita3K is GPL-2.0,
 * touchHLE/iDared 32bit is MPL-2.0, FEX-Emu/Wine carry their own terms).
 *
 * Threading contract
 * ------------------
 * Every function below is called on the shell's single "core thread". Cores
 * must not block it for longer than one frame budget without yielding through
 * emu_core_run_frame returning. Video and audio delivery is synchronous inside
 * emu_core_run_frame via the EmuHostApi sink, which keeps cores free of any
 * platform threading assumptions.
 */

#ifndef EMU_CORE_H
#define EMU_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Versioning
 * ------------------------------------------------------------------------- */

/*
 * Bump EMU_CORE_ABI_VERSION on any incompatible change. Cores report the
 * version they were built against in EmuCoreInfo::abi_version; the shell
 * refuses to register a core whose major version it does not understand.
 */
#define EMU_CORE_ABI_VERSION_MAJOR 1
#define EMU_CORE_ABI_VERSION_MINOR 0

/* -------------------------------------------------------------------------
 * Capabilities
 * ------------------------------------------------------------------------- */

/*
 * A core advertises what it needs and what it can do. The shell uses
 * EMU_CAP_REQUIRES_JIT to decide whether a core is usable on this device before
 * ever showing it in the UI.
 *
 * This flag exists because the three cores genuinely differ in kind:
 *   - touchHLE uses an interpreter (an ARM interpreter derived from mGBA), so
 *     it runs on stock iOS with no JIT at all.
 *   - Vita3K is recompiler-only. There is no interpreter fallback, so the core
 *     cannot run at all without JIT entitlement on the process.
 * Getting this wrong is the difference between a core that is merely slow and a
 * core that cannot boot, so the distinction is explicit in the ABI.
 */
/*
 * Typedef'd rather than left anonymous so Swift imports it as a real type with
 * a .rawValue, instead of as a set of untyped globals.
 */
typedef enum EmuCoreCapability {
    EMU_CAP_NONE            = 0,
    /* Core cannot run unless the host process has JIT enabled. */
    EMU_CAP_REQUIRES_JIT    = 1u << 0,
    /* Core produces frames on the CPU as BGRA8. */
    EMU_CAP_CPU_FRAMES      = 1u << 1,
    /* Core renders into a caller-supplied native surface (IOSurface/Metal). */
    EMU_CAP_NATIVE_SURFACE  = 1u << 2,
    /* Core emits audio. */
    EMU_CAP_AUDIO           = 1u << 3,
    /* Core accepts input events. */
    EMU_CAP_INPUT           = 1u << 4,
    /* Core has no persistent writable storage and can be restarted freely. */
    EMU_CAP_STATELESS       = 1u << 5
} EmuCoreCapability;

/* -------------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------------- */

typedef enum EmuStatus {
    EMU_OK                  =  0,
    EMU_ERR_UNSUPPORTED     = -1,  /* core does not implement this call */
    EMU_ERR_INVALID_ARG     = -2,
    EMU_ERR_OUT_OF_MEMORY   = -3,
    EMU_ERR_NO_JIT          = -4,  /* EMU_CAP_REQUIRES_JIT set but JIT absent */
    EMU_ERR_NO_CONTENT      = -5,  /* nothing loadable found */
    EMU_ERR_LOAD_FAILED     = -6,  /* content present but rejected */
    EMU_ERR_UNSUPPORTED_GUEST = -7,/* guest arch/version this core will never run */
    EMU_ERR_IO              = -8,
    EMU_ERR_INTERNAL        = -9
} EmuStatus;

/* -------------------------------------------------------------------------
 * Static description
 * ------------------------------------------------------------------------- */

typedef struct EmuCoreInfo {
    /* Stable machine identifier, e.g. "touchHLE", "vita3k", "fex". Persisted in
     * the shell's library database, so do not change it casually. */
    const char *id;
    /* Human-facing name for the library UI, e.g. "iDared 32bit". */
    const char *display_name;
    /* Core's own version string. */
    const char *version;
    /* EMU_CORE_ABI_VERSION_* this core was compiled against. */
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    /* Bitmask of enum EmuCoreCapability. */
    uint32_t capabilities;
    /* Short sentence shown in the UI describing what this core runs, e.g.
     * "iPhone OS 2.x-4.x apps and games". May be NULL. */
    const char *summary;
} EmuCoreInfo;

/* -------------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------------- */

/*
 * CPU framebuffer. Stride is in bytes and may exceed width * 4, so always use
 * stride when advancing rows rather than recomputing it.
 */
typedef struct EmuVideoFrame {
    const uint8_t *pixels;   /* BGRA8, premultiplied first */
    uint32_t width;
    uint32_t height;
    uint32_t stride;         /* bytes per row */
    /* Rotation the shell should apply for display, in degrees clockwise. */
    uint32_t rotation_degrees;
    /* Monotonic frame counter; the shell uses this to pace emulation. */
    uint64_t frame_index;
} EmuVideoFrame;

/* -------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------- */

typedef struct EmuAudioBuffer {
    const int16_t *samples;  /* interleaved stereo */
    uint32_t frames;         /* sample pairs, not samples */
    uint32_t sample_rate;    /* Hz, typically 48000 */
} EmuAudioBuffer;

/* -------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

typedef enum EmuInputType {
    EMU_INPUT_TOUCH_DOWN = 0,
    EMU_INPUT_TOUCH_MOVE,
    EMU_INPUT_TOUCH_UP,
    EMU_INPUT_BUTTON_DOWN,
    EMU_INPUT_BUTTON_UP
} EmuInputType;

/*
 * Touch coordinates are in guest-native space, not screen space. A core that
 * emulates a 320x480 device receives 0..320 / 0..480 here and is responsible
 * for its own scaling, because only the core knows its guest's coordinate
 * system and any scaling it applied while rendering.
 */
typedef struct EmuInputEvent {
    EmuInputType type;
    uint32_t touch_id;
    float x;                 /* guest-native units */
    float y;
    uint32_t button;         /* core-defined button code for BUTTON_* */
} EmuInputEvent;

/* -------------------------------------------------------------------------
 * Host services
 * ------------------------------------------------------------------------- */

/*
 * Services the shell provides to cores. Cores must use these rather than
 * calling platform file or logging APIs directly, which is what keeps a core
 * portable across the iOS host and a future desktop host.
 *
 * All function pointers must be non-NULL. The shell guarantees this for the
 * lifetime of the core.
 */
typedef struct EmuHostApi {
    /* Monotonic milliseconds, for pacing and timing. */
    uint64_t (*now_ms)(void *userdata);

    /* Read a whole file into a caller-allocated buffer.
     * Returns EMU_OK and sets out_data and out_len (caller frees via
     * host_free), or EMU_ERR_IO. The host allocates, so cores need no
     * matching allocator. */
    EmuStatus (*read_file)(void *userdata, const char *path,
                           uint8_t **out_data, size_t *out_len);

    /* Free a buffer handed out by read_file. */
    void (*host_free)(void *userdata, void *ptr);

    /* Log sink. The shell routes this to os_log/NSLog. */
    void (*log)(void *userdata, int level, const char *message);

    /* Deliver one video frame. Called synchronously from emu_core_run_frame. */
    void (*submit_video)(void *userdata, const EmuVideoFrame *frame);

    /* Deliver audio. Called zero or more times per emu_core_run_frame. */
    void (*submit_audio)(void *userdata, const EmuAudioBuffer *audio);

    void *userdata;
} EmuHostApi;

/* -------------------------------------------------------------------------
 * Core instance
 * ------------------------------------------------------------------------- */

/* Opaque, core-defined. The shell never dereferences this. */
typedef struct EmuCore EmuCore;

/* Which of a core's capabilities the caller intends to use. Set at create time
 * so a core can reject an unsupported combination early rather than crashing. */
typedef struct EmuCoreConfig {
    uint32_t enable_capabilities; /* bitmask from enum EmuCoreCapability */
    uint32_t audio_sample_rate;   /* 0 for the core's default */
    uint32_t audio_buffer_frames; /* 0 for the core's default */
} EmuCoreConfig;

/* -------------------------------------------------------------------------
 * The vtable
 * ------------------------------------------------------------------------- */

/*
 * Function table. A core must return a pointer to a single static instance of
 * this table from emu_core_vtable(). All pointers must be non-NULL unless the
 * corresponding capability flag is unset.
 */
typedef struct EmuCoreVTable {
    /* --- Required --- */

    /* Static description. Never NULL. */
    const EmuCoreInfo *(*info)(void);

    /*
     * Instantiate a core. Returns NULL on failure; if *status is non-NULL it
     * receives the reason. Returns EMU_ERR_NO_JIT when the core requires JIT
     * and emu_core_jit_available() is false.
     */
    EmuCore *(*create)(const EmuCoreConfig *config, const EmuHostApi *host,
                       EmuStatus *status);

    /* Destroy an instance created by create. Must tolerate NULL. */
    void (*destroy)(EmuCore *core);

    /* --- Content --- */

    /*
     * Fast check: could this core ever run this file? Used to route a file to
     * the right core without attempting a full load. Must be cheap.
     * Return EMU_OK if the core claims the content.
     */
    EmuStatus (*probe_content)(EmuCore *core, const char *path);

    /* Load guest content and reset to a runnable state. */
    EmuStatus (*load_content)(EmuCore *core, const char *path);

    /* Unload content, leaving the core idle but reusable. */
    EmuStatus (*unload_content)(EmuCore *core);

    /* --- Execution --- */

    /*
     * Advance emulation by roughly one display frame, delivering video and
     * audio through the host sink as a side effect. Returns EMU_OK normally,
     * or a negative EmuStatus on a fatal condition.
     */
    EmuStatus (*run_frame)(EmuCore *core);

    /* --- Optional; only present if the matching capability flag is set --- */

    /* Feed an input event to the running guest. */
    EmuStatus (*submit_input)(EmuCore *core, const EmuInputEvent *event);

    /* Persist and restore core state, e.g. save states. */
    EmuStatus (*save_state)(EmuCore *core, const char *path);
    EmuStatus (*load_state)(EmuCore *core, const char *path);

    /* Which guest architectures/versions this core will accept. Used by the
     * shell to filter its library without loading anything. May be NULL. */
    EmuStatus (*guest_support)(EmuCore *core, uint32_t *out_flags);
} EmuCoreVTable;

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

/*
 * How a core hands its table to the shell.
 *
 * Note on why this is not simply "the shell declares emu_core_vtable()":
 * every core naturally wants to export that same symbol, and statically linking
 * two cores that both define it is a duplicate-symbol link failure. Cores are
 * statically linked here precisely to keep bundled-dylib re-signing out of the
 * picture, so this has to be solved rather than avoided.
 *
 * Resolution: the shell defines emu_register_core(), and each core calls it
 * from a load-time constructor. The shell owns one global table, cores self-
 * register, and no two cores ever collide on a symbol name. Adding a fifth core
 * means linking one more static library and nothing else.
 *
 * The shell must define emu_register_core exactly once:
 *
 *     void emu_register_core(const EmuCoreVTable *table) {
 *         emu_core_registry_push(table);   // shell-side
 *     }
 *
 * Each core then needs a 5-line translation unit:
 *
 *     #include "emu_core.h"
 *     static const EmuCoreVTable TABLE = { ... };
 *     const EmuCoreVTable *emu_core_vtable(void) { return &TABLE; }
 *     __attribute__((constructor))
 *     static void emu_core_register(void) { emu_register_core(&TABLE); }
 *
 * Rust cores can either use the same C shim or self-register directly via a
 * #[used] static in .init_array; the C shim is the more portable of the two.
 */

void emu_register_core(const EmuCoreVTable *table);

/* Number of cores registered so far. The shell calls this once after startup
 * to confirm the count matches what it linked. */
uint32_t emu_registered_core_count(void);

/* Registered core at index i, or NULL if out of range. Provided by the shell so
 * that Swift can enumerate cores without a C callback into managed code during
 * load-time initialisation. */
const EmuCoreVTable *emu_core_at(uint32_t index);

/*
 * Host-side helper for cores: returns nonzero if the host process currently has
 * JIT available. Cores declaring EMU_CAP_REQUIRES_JIT must call this from
 * create() and return EMU_ERR_NO_JIT when it is false, rather than booting and
 * then failing.
 *
 * The host supplies this; it is not part of the vtable.
 */
int emu_host_jit_available(void);

/* -------------------------------------------------------------------------
 * Convenience typedefs
 * ------------------------------------------------------------------------- */

typedef const EmuCoreVTable *(*EmuCoreVTableFn)(void);
typedef EmuStatus (*EmuLogLevelHint)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EMU_CORE_H */