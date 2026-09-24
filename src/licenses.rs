/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Prints copyright, authorship and license information.

const IDARED_FORK_NOTICE: &str = "
iDared 32bit is a fork of touchHLE, with a few differences in this build:

  - CPU emulation uses an ARM interpreter derived from mGBA's (replaces
    Dynarmic JIT)
  - The guest-facing OpenAL uses SDL audio (replaces OpenAL Soft library)

The maintainer (@apexad) contributed to touchHLE until this App Store
release, which the touchHLE project did not want, and is no longer part of
that project. This build includes changes that are not in touchHLE.
iDared 32bit is not affiliated with or endorsed by the touchHLE project.

Source code for iDared 32bit, including the version of touchHLE it is based
on, is available at <https://github.com/iDared32bit-emu/iDared32bit>.

The copyright, authorship and license information below applies to iDared 32bit
in full.
";

const MAIN_COPYRIGHT: &str = "
iDared 32bit © 2026 iDared 32bit project contributors.

touchHLE © 2023–2026 touchHLE project contributors.
";

const MAIN_LICENSE: &str = "
This program, iDared 32bit, is licensed under the Mozilla Public License,
version 2.0 (MPL-2.0). You can obtain a copy of the license at
<https://mozilla.org/MPL/2.0/>, and the source code at the address given
above.

This executable is not licensed under the GNU General Public License (GPL).
Its own code, and every library compiled into it, is licensed under MPL-2.0 or
under more permissive terms, as listed below. None of it is GPL-licensed.

Under the MPL-2.0, this program is provided on an \"as is\" basis, without
warranty of any kind; see sections 6 and 7 of the license.

Separately from the program itself, it carries copies of the libgcc and
libstdc++ libraries from Apple's iPhone OS SDK, which apps running inside the
emulated device link against. They are separate programs, licensed under the
GNU GPL version 2 or later. They run only inside the emulated device, are not
linked with iDared 32bit, and do not change its license. Their license notices,
and a written offer for their source code, are included with this program.
";

// See android/app/src/main/java/org/alexmartin/idared32bit/DocumentsProvider.kt
#[cfg(target_os = "android")]
const SKYLINE: &str = "
touchHLE for Android incorporates code originally from the Skyline emulator
project, licensed under MPL-2.0:

Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
";

const RUST_DESCRIPTION: &str = "
touchHLE, and therefore this executable, incorporates the following Rust
libraries, which are copyright their respective authors and other contributors,
and licensed as follows:
";

/// Names, versions, authors and licenses of Rust crates depended on by release
/// binaries. See `build.rs` for how this is generated.
const RUST_DEPENDENCIES: &str = include_str!(concat!(env!("OUT_DIR"), "/rust_dependencies.txt"));

const MGBA_DESCRIPTION: &str = "
iDared 32bit, and therefore this executable, incorporates a modified copy of
the ARM interpreter from the mGBA Game Boy Advance emulator (in
vendor/mgba_arm/), which this build extends from ARMv4T to ARMv7s.
mGBA is copyright © 2013–2024 Jeffrey Pfau, and is available under the Mozilla
Public License, version 2.0 -- the same license as the rest of this program.
The modified source files are published with the rest of iDared 32bit's source
code (see above). iDared 32bit is not affiliated with or endorsed by the mGBA
project.
";

const SDL2_DESCRIPTION: &str = "
touchHLE, and therefore this executable, incorporates the library SDL 2, which
is available under the following license:
";

const SDL2_LICENSE: &str = "
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
";

const STB_IMAGE: &str = "
touchHLE, and therefore this executable, incorporates the library stb_image,
which is available either as Public Domain or under the terms of the MIT
license.
";

const PVRTD_DESCRIPTION: &str = "
touchHLE, and therefore this executable, incorporates PVRTC decompression code
from the PowerVR SDK, which is available under the following license:
";

const PVRTD_LICENSE: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/vendor/PVRTDecompress/LICENSE.md"
));

// When resource files are bundled with touchHLE in such a way that the user can
// read their license files directly, use this caveat.

const EXTERNAL_FILES_CAVEAT: &str = "
The following authorship, copyright and license information relates to this
executable. Please note that different licensing terms apply to the bundled
dynamic libraries (in touchHLE_dylibs/) and fonts (in touchHLE_fonts/). Please
consult the respective directories for more information.
";

// When resource files are bundled with touchHLE in such a way that only
// touchHLE can read the license file, use these notices.

// Apple's own offer is reproduced below for provenance, but GPLv2 3(c) only
// allows passing it along for noncommercial distribution, so this build makes
// its own 3(b) offer.
const INTERNAL_FSF_DYLIBS_DESCRIPTION: &str = "
This distribution of iDared 32bit includes binaries for the libgcc and libstdc++
libraries from the Free Software Foundation, as originally distributed by Apple:
";

const INTERNAL_FSF_DYLIBS_OFFER: &str = "
Written offer, valid for three years from the date you received this
distribution: on request, the maintainer of iDared 32bit will give any third
party a complete machine-readable copy of the corresponding source code for
the libgcc and libstdc++ binaries included here, for no more than the cost of
performing the distribution. That source is also published at:

  libgcc    <https://github.com/iDared32bit-emu/gcc> (tag gcc-5664)
  libstdc++ <https://github.com/iDared32bit-emu/libstdcxx> (tag libstdcxx-39)

Requests: admin@idared32bit-emu.com
";

const INTERNAL_ZLIB_DYLIB_DESCRIPTION: &str = "
This distribution of iDared 32bit includes binaries for zlib (libz), available under
the following license:
";

// We have a COPYING file for SQLite, but as the main source and resulting
// binary are simply Public Domain, we can save some space here.
const INTERNAL_SQLITE3_DYLIB_DESCRIPTION: &str = "
This distribution of iDared 32bit includes binaries for SQLite (libsqlite3),
available under Public Domain.
";

const INTERNAL_XML2_DYLIB_DESCRIPTION: &str = "
This distribution of iDared 32bit includes binaries for libxml2, available under
the following license:
";

const INTERNAL_LIBERATION_FONTS_DESCRIPTION: &str = "
This distribution of iDared 32bit includes Liberation Sans fonts, available under
the following license:
";

const INTERNAL_NOTO_FONTS_DESCRIPTION: &str = "
This distribution of iDared 32bit includes Noto Sans CJK fonts, available under the
following license:
";

fn read_bundled_file(path: &str) -> String {
    use std::io::Read;
    let mut res = String::new();
    crate::paths::ResourceFile::open(path)
        .unwrap()
        .get()
        .read_to_string(&mut res)
        .unwrap();
    res
}

fn divider(out: &mut String) -> Result<(), std::fmt::Error> {
    use std::fmt::Write;
    writeln!(out, "---")
}

fn print(out: &mut String, resources_are_external_files: bool) -> Result<(), std::fmt::Error> {
    use std::fmt::Write;
    writeln!(out, "{IDARED_FORK_NOTICE}")?;
    divider(out)?;
    if resources_are_external_files {
        writeln!(out, "{EXTERNAL_FILES_CAVEAT}")?;
        divider(out)?;
    }
    writeln!(out, "{MAIN_COPYRIGHT}")?;
    divider(out)?;
    writeln!(out, "{MAIN_LICENSE}")?;
    divider(out)?;
    #[cfg(target_os = "android")]
    {
        writeln!(out, "{SKYLINE}")?;
        divider(out)?;
    }
    writeln!(out, "{RUST_DESCRIPTION}")?;
    writeln!(out, "{RUST_DEPENDENCIES}")?;
    divider(out)?;
    writeln!(out, "{MGBA_DESCRIPTION}")?;
    divider(out)?;
    writeln!(out, "{SDL2_DESCRIPTION}")?;
    writeln!(out, "{SDL2_LICENSE}")?;
    divider(out)?;
    writeln!(out, "{STB_IMAGE}")?;
    divider(out)?;
    writeln!(out, "{PVRTD_DESCRIPTION}")?;
    writeln!(out, "{}", PVRTD_LICENSE.trim_end())?;
    if !resources_are_external_files {
        divider(out)?;
        writeln!(out, "{INTERNAL_FSF_DYLIBS_DESCRIPTION}")?;
        writeln!(
            out,
            "{}.",
            read_bundled_file(&format!("{}/README.md", crate::paths::DYLIBS_DIR))
                // Skip preamble
                .split_once("## Original Apple license acknowledgements")
                .unwrap()
                .1
                // Don't include the whole GPL
                .split_once("; a copy of the GPL is included below.")
                .unwrap()
                .0
                // Remove one level of Markdown quoting
                .replace("\n> ", "\n")
                .trim_start()
        )?;
        writeln!(out, "{INTERNAL_FSF_DYLIBS_OFFER}")?;
        divider(out)?;
        writeln!(out, "{INTERNAL_ZLIB_DYLIB_DESCRIPTION}")?;
        writeln!(
            out,
            "{}",
            read_bundled_file(&format!("{}/COPYING.libz", crate::paths::DYLIBS_DIR))
        )?;
        divider(out)?;
        writeln!(out, "{INTERNAL_SQLITE3_DYLIB_DESCRIPTION}")?;
        divider(out)?;
        writeln!(out, "{INTERNAL_XML2_DYLIB_DESCRIPTION}")?;
        writeln!(
            out,
            "{}",
            read_bundled_file(&format!("{}/COPYING.libxml2", crate::paths::DYLIBS_DIR))
        )?;
        divider(out)?;
        writeln!(out, "{INTERNAL_LIBERATION_FONTS_DESCRIPTION}")?;
        writeln!(
            out,
            "{}",
            read_bundled_file(&format!("{}/LICENSE.liberation", crate::paths::FONTS_DIR))
        )?;
        divider(out)?;
        writeln!(out, "{INTERNAL_NOTO_FONTS_DESCRIPTION}")?;
        writeln!(
            out,
            "{}",
            read_bundled_file(&format!("{}/LICENSE.noto", crate::paths::FONTS_DIR))
        )?;
    }
    Ok(())
}

pub fn get_text() -> String {
    let mut string = String::new();
    print(&mut string, crate::paths::RESOURCES_ARE_EXTERNAL_FILES).unwrap();
    string
}

#[cfg(test)]
#[test]
fn test_external_files_license_text() {
    // The code above makes assumptions about the content of external files that
    // aren't checked at build time, and the runtime check only happens when you
    // view the license text on Android! This seems likely to silently break,
    // so it's a good candidate for a unit test.
    print(
        &mut String::new(),
        /* resources_are_external_files: */ true,
    )
    .unwrap();
}
