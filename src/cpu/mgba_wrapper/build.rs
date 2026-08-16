/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
use std::path::Path;

fn main() {
    let package_root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let workspace_root = package_root.join("../../..");
    let mgba_root = workspace_root.join("vendor/mgba_arm");

    let mut build = cc::Build::new();
    build
        .std("c11")
        // VFPv3 does not fuse multiply-accumulate, so VMLA must round the
        // product before adding it. GCC contracts by default.
        .flag_if_supported("-ffp-contract=off")
        // mGBA's ALU relies on signed arithmetic wrapping, as its own build
        // does with this flag. Without it, overflow is undefined behaviour
        // and the compiler may fold away the overflow checks behind the V
        // flag: NEGS of 0x80000000 came out with V clear.
        .flag("-fwrapv")
        // vfp.c changes the host's rounding mode to match FPSCR's, which the
        // compiler must not assume is always round-to-nearest.
        .flag_if_supported("-frounding-math")
        .include(mgba_root.join("include"))
        .file(package_root.join("lib.c"))
        .file(package_root.join("vfp.c"))
        .file(package_root.join("neon.c"))
        .file(package_root.join("cp15.c"))
        .file(mgba_root.join("src/arm.c"))
        .file(mgba_root.join("src/isa-arm.c"))
        .file(mgba_root.join("src/isa-thumb.c"))
        .file(mgba_root.join("src/isa-thumb2.c"));

    build.compile("mgba_wrapper");

    for file in ["lib.c", "vfp.c", "cp15.c", "wrapper.h"] {
        println!(
            "cargo:rerun-if-changed={}",
            package_root.join(file).to_str().unwrap()
        );
    }
    println!("cargo:rerun-if-changed={}", mgba_root.to_str().unwrap());
}
