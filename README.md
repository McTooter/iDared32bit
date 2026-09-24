# <img src="https://idared32bit-emu.com/idared32bit-icon.png" alt="iDared 32bit icon" width="48" height="48"> iDared 32bit

**iDared 32bit** runs classic 32-bit iPhone OS apps and games on modern iPhones and iPads.

It is a fork of **[touchHLE](https://github.com/touchHLE/touchHLE)**, a high-level emulator for early iPhone OS apps, packaged as a native iOS app.
The emulation is touchHLE's work; this fork adds the iOS host and the build-specific changes noted below.

Website (including the privacy policy): <https://idared32bit-emu.com/>

## What's different in this build

- **iOS host app** which runs as a native app on iPhone and iPad.
- **An interpreter instead of Dynarmic** for CPU emulation. The CPU core is an ARM interpreter derived from [mGBA](https://github.com/mgba-emu/mgba)'s, extended from ARMv4T to ARMv7s, with Thumb-2, VFPv3/VFPv4 and NEON. The only code run by the [emulated CPU](vendor/mgba_arm/) is the app binary and [a handful of libraries](touchHLE_dylibs/).
- **OpenAL via SDL Audio** for the guest-facing `OpenAL.framework` and Audio Toolbox playback, instead of the OpenAL Soft C library.
- **Changes not in upstream touchHLE** — the maintainer ([@apexad](https://github.com/apexad)) contributed to touchHLE until this App Store release, which the touchHLE project did not want, and is no longer part of that project. This build includes changes, some originally written for touchHLE, that enable games not supported upstream.

## Based on touchHLE

For reference only:

- Upstream project: <https://github.com/touchHLE/touchHLE>
- touchHLE's website: <https://touchhle.org/>
- touchHLE app compatibility database: <https://appdb.touchhle.org/>

These belong to touchHLE, not iDared 32bit. **Do not ask touchHLE for support with iDared 32bit, and do not submit iDared 32bit app compatibility results to touchHLE's website or its app compatibility database.** Please bring questions, problems and compatibility reports for iDared 32bit here instead.

The goal of this project is to run games from the early days of iOS:

* Currently: iPhone and iPod touch apps for iPhone OS 2.x and iPhone OS 3.x.
* Longer term: iOS 4.x, and beyond.
* [Never](https://github.com/touchHLE/touchHLE/issues/181#issuecomment-1777098259): 64-bit iOS.

**This does not mean that all apps for these OS versions work.** The vast majority of iPhone OS 2.x and iPhone OS 3.x apps do not currently work in touchHLE or iDared 32bit, and the ones that do work are generally games (support for other apps isn't a priority: it's more complex and less fun). This improves gradually over time with contributions from various developers. touchHLE's app compatibility database tracks touchHLE only; it does not cover iDared 32bit. **We don't take requests, so please do not ask us to support your favourite game.**

Because iDared 32bit uses a different CPU backend, SDL-based audio, iOS-specific changes, and additional maintainer changes that are not in upstream touchHLE, compatibility may differ from touchHLE. Some games that work in touchHLE may not work in iDared 32bit, and vice versa.

## Platform support

* Officially supported: iOS, AArch64 macOS (built locally)

This fork's target is a native **iOS** app (AArch64) for iPhone and iPad; that is the platform it is built, tested, and released for. AArch64 macOS is used for development and works; the remaining host platforms below are inherited from touchHLE but have not all been re-verified in this fork.  Additionally, this fork removes the automated GitHub build scripts.

* Officially supported: x64 Windows, x64 macOS and AArch64 Android.
  * These are the platforms with binary releases.
  * If you're an Apple Silicon Mac user, the x64 build reportedly works in Rosetta.
* Probably works, but you must build it yourself: AArch64 macOS, x64 Linux, AArch64 Linux.

## Important disclaimers

This project is not affiliated with or endorsed by Apple Inc in any way. iPhone, iOS, iPod, iPod touch and iPad are trademarks of Apple Inc in the United States and other countries.

Only use iDared 32bit to emulate software you have obtained legally.

Not an official touchHLE release, and not affiliated with or endorsed by the touchHLE project — please direct questions or problems regarding iDared 32bit here, not to the touchHLE project.

Not affiliated with or endorsed by the mGBA project, whose ARM interpreter iDared 32bit's CPU core is derived from — please direct questions or problems regarding iDared 32bit here, not to the mGBA project.

## Graphical user interface

iDared 32bit uses the same built-in app picker as touchHLE. If you put your `.ipa` files and `.app` bundles in the `touchHLE_apps` directory, they will show up in the app picker when you run iDared 32bit. Note that the app binary must not be encrypted to be usable.
To access the `touchHLE_apps` directory, simply tap the “File manager” button in iDared 32bit.

To configure the options, you can edit the `touchHLE_options.txt` file. To get a list of options, look in the `OPTIONS_HELP.txt` file.

Any data saved by the app (e.g. **saved games**) are stored in the `touchHLE_sandbox` folder.

## License

iDared 32bit © 2026 iDared 32bit project contributors.

touchHLE © 2023–2026 touchHLE project contributors.

iDared 32bit is based on touchHLE. Its source code, including touchHLE's and the modifications made to it here, is licensed under the Mozilla Public License 2.0 (MPL-2.0).

**Distributed binaries are also licensed under the MPL-2.0. They are not licensed under the GNU General Public License (GPL).** Everything compiled into the executable is licensed under the MPL-2.0 or under more permissive terms; none of it is GPL-licensed.

The app carries copies of the libgcc and libstdc++ dynamic libraries from Apple's iPhone OS SDK (`touchHLE_dylibs/`), which apps running inside the emulated device link against. These are separate programs, licensed under the GNU GPL version 2 or later. They run only inside the emulated device, are not linked with iDared 32bit, and do not change its license. Their license notices, and a written offer for their source code, are included in the app's license information.

Copyright, authorship, and license notices for touchHLE, mGBA, and bundled components are preserved in the source and available through the app's license information. See `LICENSE`, `src/licenses.rs`, and the applicable bundled-component directories for details.

# Thanks

We stand on the shoulders of giants. Thank you to:

* Everyone who has contributed to the project or supported any of its contributors financially.
* The authors of and contributors to the many libraries used by this project: [mGBA](https://github.com/mgba-emu/mgba) (whose ARM interpreter core, modified, is vendored in [vendor/mgba_arm/](vendor/mgba_arm/)), [rust-macho](https://github.com/flier/rust-macho), [SDL](https://libsdl.org/), [rust-sdl2](https://github.com/Rust-SDL2/rust-sdl2), [stb\_image](https://github.com/nothings/stb), Imagination Technologies' [PVRTC decompressor](https://github.com/powervr-graphics/Native_SDK/blob/master/framework/PVRCore/texture/PVRTDecompress.cpp), [hound](https://github.com/ruuda/hound), [Symphonia](https://github.com/pdeljanov/Symphonia), [RustType](https://gitlab.redox-os.org/redox-os/rusttype), [the Liberation fonts](https://github.com/liberationfonts/liberation-fonts), [the Noto CJK fonts](https://github.com/googlefonts/noto-cjk), [rust-plist](https://github.com/ebarnard/rust-plist), [nibarchive](https://github.com/michaelwright235/nibarchive), [quick-xml](https://github.com/tafia/quick-xml), [gl-rs](https://github.com/brendanzab/gl-rs), [cargo-license](https://github.com/onur/cargo-license), [cc-rs](https://github.com/rust-lang/cc-rs), [cmake-rs](https://github.com/rust-lang/cmake-rs), [cargo-ndk](https://github.com/bbqsrc/cargo-ndk), [cargo-ndk-android-gradle](https://github.com/willir/cargo-ndk-android-gradle), [md-5 and sha1](https://github.com/RustCrypto/hashes), [encoding_rs](https://github.com/hsivonen/encoding_rs), [corosensei](https://github.com/Amanieu/corosensei), [uuid](https://github.com/uuid-rs/uuid) and the Rust standard library.
* The [Rust project](https://www.rust-lang.org/) generally.
* The various people out there who've documented the iPhone OS platform, officially or otherwise. Much of this documentation is linked to within this codebase!
* The iOS hacking/jailbreaking community.
* The Free Software Foundation, for making libgcc and libstdc++ copyleft and therefore saving this project from ABI hell.
* Many friends who took an interest in the project and gave suggestions and encouragement.
* Developers of early iPhone OS apps. What treasures you created!
* Apple, and NeXT before them, for creating such fantastic platforms.
