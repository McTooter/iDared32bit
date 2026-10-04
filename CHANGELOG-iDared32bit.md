# iDared 32bit changelog

iDared 32bit is based on [touchHLE](https://touchhle.org/). This file lists only iDared 32bit's own changes. The touchHLE changes it includes are in [CHANGELOG.md](CHANGELOG.md), which is touchHLE's own changelog, kept exactly as it is upstream.

## NEXT

Based on touchHLE v0.3.0. Its changes are in [CHANGELOG.md](CHANGELOG.md).

Compatibility:

- Without network access, sockets behave as on a device in airplane mode, so apps that try to connect carry on instead of hanging. The socket API also handles more options and flags, non-blocking sockets and connection timeouts.
- Threads: `NSThread` reports its real state and supports `cancel` and `exit`, and `pthread_exit()` and the pthread scheduling functions are implemented. A thread waiting with a timeout is no longer mistaken for a deadlock. (`NSThread` `isExecuting` by alborrajo.)
- Audio: voice processing audio units, the audio route property, and the length and seeking functions of `ExtAudioFile`.
- Games built with the Airplay SDK no longer fail their integrity check at startup.
- `printf()` and `NSLog()` support left-justified fields.
- Implemented `dladdr()`, `inet_aton()` (by kylon), `CC_SHA256` and its incremental functions, `-[UIWindow windowLevel]`, `-[NSFileHandle synchronizeFile]` and `-[NSProcessInfo processorCount]`, and more OpenGL ES and Core Graphics functions and parameters.
- More accurate `-[UIFont leading]` (by abnormalmaps).
- Apps that support portrait now start in portrait.
- App icons listed with `CFBundleIcons`, as iOS 5 apps do, are shown in the app picker.
- Various smaller fixes.

Usability:

- Return to the app picker with Esc on macOS and Windows, Back on Android, or by holding a controller's Back button. On iOS, there's an on-screen Home button beside the app. Apps are told they're quitting first.
- The app picker pages by swiping instead of with arrow buttons, and highlights the icon you press.

Other:

- The iOS app requires iOS 16 or later.
- The app's version is set only in `iphone/Config.xcconfig`, and `dev-scripts/prepare-release.sh` prepares a release.

## v1.2.23 (2026-09-29)

Compatibility:

- Fixed a black screen on iOS in games that bind their renderbuffer once and then present every frame.

Usability:

- The log from the previous run is now kept as `touchHLE_log.previous.txt`, so relaunching after a hang or black screen no longer loses the log of what went wrong.

Documentation:

- `CONTRIBUTING.md` is rewritten for iDared 32bit: contributions are ordinary pull requests on GitHub, to the iDared 32bit repository. The rules on copyright and reverse engineering are kept.
- `CODE_OF_CONDUCT.md` is iDared 32bit's own, replacing touchHLE's.
- touchHLE's GitHub issue templates are removed.
- `dev-docs/building.md` now covers building the iOS app with Xcode, including signing through `Local.xcconfig`, no longer lists Boost as a prerequisite, and has fixed links.
- The README links iDared 32bit's website and shows its icon. It links touchHLE for reference only, and states that touchHLE is not the place for iDared 32bit support or app compatibility reports.
- The README and the license notices state that iDared 32bit is not affiliated with or endorsed by the touchHLE or mGBA projects.
- iDared 32bit's own changes are listed in this file. `CHANGELOG.md` is touchHLE's changelog, unchanged.

Other:

- The startup banner shows iDared 32bit and its website.
- The Android package is now `org.alexmartin.idared32bit`, matching iOS.
- The iOS Xcode project no longer needs Boost, which only the old Dynarmic CPU backend used.
- Windows builds link again: the mGBA CPU core used GCC built-in functions that MSVC doesn't have.
- CI runs on pushes and pull requests to the `idared32bit` branch.

## v1.1.23 (2026-09-24)

The first iDared 32bit release, based on touchHLE's development version after v0.2.3 (its NEXT section in [CHANGELOG.md](CHANGELOG.md)).

Compatibility:

- Photo Album support: `UIImageWriteToSavedPhotosAlbum` saves to `DCIM/100APPLE` like real iOS.

Documentation:

- The README is rebranded for iDared 32bit, the iOS app, and the in-app copyright information starts with a notice that iDared 32bit is a fork of touchHLE.

Other:

- Replaced the Dynarmic-based JIT CPU emulation backend with an ARM interpreter derived from [mGBA](https://github.com/mgba-emu/mgba)'s, vendored in `vendor/mgba_arm/` and extended from ARMv4T to ARMv7-A, Thumb-2 included, with VFPv3 (flush-to-zero, rounding modes and short vectors included) and Advanced SIMD (NEON), plus the armv7s additions (VFPv4 fused multiply-add, half-precision conversions and integer divide). Fat binaries' armv7s slices are now preferred, then armv7, then armv6. mGBA is licensed under the MPL-2.0, like touchHLE.
- Replaced OpenAL Soft with a built-in software audio mixer that outputs through SDL's audio subsystem. touchHLE's guest-facing `OpenAL.framework` and its Audio Toolbox playback (Audio Queue, Audio Services, Audio Unit) are now backed by this mixer instead of the OpenAL Soft C library, so OpenAL Soft is no longer a dependency or a submodule (`vendor/openal-soft/`).
- Distributed binaries are licensed under the MPL-2.0.
