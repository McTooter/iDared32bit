# Unified multi-core iOS shell

## Goal

One iOS app, sideloadable on stock iOS 17+, that hosts the emulator cores the
account has forked, behind a single native UI. The user should not care which
language a core is written in, and adding a fourth core should not require
touching the shell.

## The core insight: language is not the constraint

The cores span Rust (touchHLE), C++ (Vita3K), and C/C++ (FEX-Emu, Wine). It is
tempting to treat that as the hard part. It is not. Every one of these can
already emit a flat C symbol table:

| Core | Language | C ABI route |
|---|---|---|
| touchHLE (iDared 32bit) | Rust | `crate-type = ["cdylib", "staticlib"]` is already declared in `Cargo.toml`; `#[no_mangle] pub extern "C"` |
| Vita3K | C++ | `extern "C"` wrappers over an opaque handle — standard C++ interop |
| FEX-Emu / Wine | C, C++ | already a C ABI |

So the shell is Swift and speaks **only C**. Each core is a static library
exporting one symbol, `emu_core_vtable()`. No per-language bridging, no Swift↔Rust
interop layer, no FFI glue that has to be rewritten per core.

The precedent is libretro: RetroArch is a single app hosting a dozen emulator
cores written in a dozen languages, because every core is just a shared library
with a C function table.

## Why JIT is a first-class part of the contract

This is the part that decides whether the Vita core can ship at all.

- **touchHLE** uses an *interpreter* — an ARM interpreter derived from mGBA,
  extended from ARMv4T to ARMv7s. No JIT. This is precisely why the iDared 32bit
  fork was able to pass App Store review, and it is why the core runs on a stock
  device today.
- **Vita3K** is *recompiler-only*. There is no interpreter fallback. Tsubomi's
  own README states it will show a banner and refuse to boot games when JIT is
  unavailable, and points at StikDebug.

On stock iOS, JIT only exists via an exploit-based enabler, and its support is
versioned:

| iOS | StikDebug status |
|---|---|
| 17.0 – 17.3 | not supported (different connection protocol) |
| 17.4 – 18.x | fully supported |
| 26.0+ | supported, but limited app availability |

Consequences the design has to respect:

1. The app's real floor is **iOS 17.4**, not 17.0, for the Vita core. The touchHLE
   core still works from 16.0 (`IPHONEOS_DEPLOYMENT_TARGET = 16.0`).
2. JIT availability is probed at launch and cores that need it are disabled when
   absent, so the app is never broken-feeling. It degrades to "one working core"
   rather than "won't start".
3. `EMU_CAP_REQUIRES_JIT` is a capability bit, not an `if` statement buried in
   Vita3K's port. Other cores will hit the same wall.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  SwiftUI shell  (host app, arm64-apple-ios)                  │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ Unified library UI  — one list across all cores        │  │
│  │ Core picker / settings / JIT status banner             │  │
│  └────────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ CoreRegistry                                          │  │
│  │  · registration, ABI version check                     │  │
│  │  · JIT probe → capability filtering                    │  │
│  │  · probe_content routing, availability state           │  │
│  └────────────────────────────────────────────────────────┘  │
│                          ▲                                   │
│                          │  C ABI only (core-api/include)    │
│         ┌────────────────┼──────────────────┐                │
│         │                │                  │                │
│  ┌──────┴──────┐  ┌──────┴──────┐  ┌────────┴───────┐        │
│  │ touchHLE    │  │ Vita3K      │  │ FEX-Emu/Wine   │        │
│  │ (Rust)      │  │ (C++)       │  │ (C/C++)        │        │
│  │ WORKS       │  │ NEEDS JIT   │  │ NOT STARTED    │        │
│  │ no JIT      │  │ iOS 17.4+   │  │ biggest lift   │        │
│  └─────────────┘  └─────────────┘  └────────────────┘        │
└──────────────────────────────────────────────────────────────┘
```

### Static linking, not `dlopen`

Cores are registered at link time, each exporting `emu_core_vtable()`, and the
shell holds the returned `const EmuCoreVTable *` for the app's lifetime.

This is deliberate. Under sideloading, every nested Mach-O in the bundle gets
re-signed, and `dlopen` of a bundled dylib is a recurring source of signing and
load failures. Static linking sidesteps the whole category. A future
`dlopen`-based plugin mode can be added behind the same vtable without changing
the shell.

### Host services, not platform calls

Cores receive an `EmuHostApi` (file reads, logging, timing, video/audio sinks)
at `create()`. Cores must not call iOS file or logging APIs directly. That
constraint is what makes a core portable to a future desktop host, and it keeps
memory ownership one-directional: the host allocates, the host frees.

### Threading

One core thread. All vtable calls happen on it. Video and audio are delivered
synchronously from `run_frame` through the host sink, so no core needs to model
iOS run loops or SDL threading. Touch coordinates are passed in *guest-native*
space, because only the core knows its guest's coordinate system.

## What the cores actually are

### touchHLE / iDared 32bit — ships today

Already a native iOS app, already building to an unsigned `.ipa` in CI, already
interpreter-based. Deployment target 16.0.

The complication is structural: **the iOS UI is not native.** The entire host is
a 476-byte SDL shim (`iphone/touchHLE/SDL_uikit_main.c` → `SDL_UIKitRunApp`),
and the whole picker/Home/license UI is ~1540 lines of Rust in
`src/environment/app_picker.rs` that draws itself through *emulated* UIKit and
CoreGraphics, rendered to a bitmap that SDL blits.

So there is no existing native layer to hang a unified shell on. Making
touchHLE drivable by a SwiftUI shell means:

1. `mod window;` and `mod environment` are currently private in `src/lib.rs`.
2. `main()` in `src/lib.rs` owns the SDL event loop and the whole lifecycle.
3. Those need splitting into create / load / run_frame / destroy, with the
   window decoupled from frame production.

That refactor is the bulk of the real work, and it is only possible on a Mac.

### Vita3K — gated on JIT, no upstream iOS target

Upstream Vita3K has no iOS build target at all. Tsubomi's `ios/` directory and
`gen-ios*.sh` scripts are *its own* contribution. So adopting it means porting
Vita3K's CMake build into the Xcode/CI flow, on top of the JIT dependency.

### FEX-Emu / Wine (Madeira) — the big one

FEX-Emu can fall back to an interpreter, but performance without JIT is poor, and
Wine has no iOS port. This is the largest single item and the one most likely to
be deferred indefinitely.

## Sequencing

Ordered by "value delivered per unit of unverifiable work":

1. **ABI header** — `core-api/include/emu_core.h`. Done. Language-neutral,
   reviewable without a Mac.
2. **Shell + registry** — Swift, self-contained, no emulator internals. Proves
   registration, ABI versioning, JIT gating, and content routing end to end.
3. **touchHLE headless refactor + shim** — needs a Mac. The Rust core's SDL
   coupling is the blocker.
4. **Vita3K port** — needs a Mac, needs JIT acceptance.
5. **FEX/Wine port** — largest, most likely deferred.

Steps 1–2 can be built and validated without any emulator core present, using a
conforming stub core. That is deliberate: it means the architecture is proven
before the expensive refactor starts, and step 3 becomes a drop-in rather than a
redesign.

## Verification status

Everything below step 2 is currently **unverified**. This machine is Windows
with no Xcode, no Theos, and no Rust iOS target, so nothing iOS-touching can be
compiled here. Upstream's own CI runs on `macos-15`; this fork already has a
working `build-ios-ipa.yml` on that runner.

The honest position: the header and registry are written to be correct by
inspection, and CI is the only thing that can convert them into a known-good
build. Do not treat step 3+ as done until a macOS CI run says so.