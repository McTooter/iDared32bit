/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `dlfcn.h` (`dlopen()` and friends)

use crate::dyld::{export_c_func, FunctionExports};
use crate::mem::{ConstPtr, ConstVoidPtr, MutPtr, MutVoidPtr, Ptr, SafeRead};
use crate::Environment;
use std::collections::HashMap;

const RTLD_DEFAULT: MutVoidPtr = Ptr::from_bits(-2 as _);

#[derive(Default)]
pub struct State {
    /// C strings returned by `dladdr()`, which the caller doesn't free, so
    /// that each one is only allocated once.
    dladdr_strings: HashMap<String, ConstPtr<u8>>,
}

#[allow(non_camel_case_types)]
#[repr(C, packed)]
struct Dl_info {
    dli_fname: ConstPtr<u8>,
    dli_fbase: ConstVoidPtr,
    dli_sname: ConstPtr<u8>,
    dli_saddr: ConstVoidPtr,
}
unsafe impl SafeRead for Dl_info {}

fn is_known_library(path: &str) -> bool {
    crate::dyld::DYLIB_LIST
        .iter()
        .any(|dylib| dylib.path == path || dylib.aliases.contains(&path))
}

fn dlopen(env: &mut Environment, path: ConstPtr<u8>, _mode: i32) -> MutVoidPtr {
    if path.is_null() {
        return RTLD_DEFAULT;
    }
    // TODO: dlopen() support for real dynamic libraries.
    assert!(is_known_library(env.mem.cstr_at_utf8(path).unwrap()));
    // For convenience, use the path as the handle.
    // TODO: Find out whether the handle is truly opaque on iPhone OS, and if
    // not, where it points.
    path.cast_mut().cast()
}

fn dlsym(env: &mut Environment, handle: MutVoidPtr, symbol: ConstPtr<u8>) -> MutVoidPtr {
    assert!(
        handle == RTLD_DEFAULT || is_known_library(env.mem.cstr_at_utf8(handle.cast()).unwrap())
    );
    // For some reason, the symbols passed to dlsym() don't have the leading _.
    let symbol = format!("_{}", env.mem.cstr_at_utf8(symbol).unwrap());
    // TODO: error handling. dlsym() should just return NULL in this case, but
    // currently it's probably more useful to have the emulator crash if there's
    // no symbol found, since it most likely indicates a missing host function.
    // TODO: Symbol lookup should be scoped to the specific library requested,
    // where appropriate!
    let addr = env
        .dyld
        .create_proc_address(&mut env.mem, &mut env.cpu, &symbol)
        .unwrap_or_else(|_| panic!("dlsym() for unimplemented function {symbol}"));
    Ptr::from_bits(addr.addr_with_thumb_bit())
}

/// Get a C string for `dladdr()` to return, allocating it the first time.
fn dladdr_string(env: &mut Environment, string: String) -> ConstPtr<u8> {
    if let Some(&ptr) = env.libc_state.dlfcn.dladdr_strings.get(&string) {
        return ptr;
    }
    let ptr = env.mem.alloc_and_write_cstr(string.as_bytes()).cast_const();
    env.libc_state.dlfcn.dladdr_strings.insert(string, ptr);
    ptr
}

/// Find which loaded binary contains `addr`, and the nearest symbol at or
/// before it. Returns 0 if no binary contains it.
fn dladdr(env: &mut Environment, addr: ConstVoidPtr, info: MutPtr<Dl_info>) -> i32 {
    let addr = addr.to_bits();
    let Some((bin_idx, base)) = env.bins.iter().enumerate().find_map(|(i, bin)| {
        let base = bin.text_segment_base?;
        (base <= addr && addr < bin.last_segment_end).then_some((i, base))
    }) else {
        log!("dladdr({:#x}): not in any loaded binary", addr);
        return 0;
    };

    let bin = &env.bins[bin_idx];
    let file_name = if bin_idx == 0 {
        // The app's own executable.
        env.bundle.executable_path().as_str().to_string()
    } else {
        bin.name.clone()
    };
    // Thumb function symbols have the Thumb bit set, so ignore it when
    // comparing, but return the address with it, as it's how the function is
    // called.
    let symbol = bin
        .exported_symbols
        .iter()
        .filter(|(_, &sym_addr)| (sym_addr & !1) <= addr)
        .max_by_key(|(_, &sym_addr)| sym_addr & !1)
        .map(|(name, &sym_addr)| {
            // Symbol names are reported without the leading underscore, as C
            // code would refer to them.
            (name.strip_prefix('_').unwrap_or(name).to_string(), sym_addr)
        });

    let dli_fname = dladdr_string(env, file_name);
    let (dli_sname, dli_saddr) = match symbol {
        Some((name, sym_addr)) => (dladdr_string(env, name), Ptr::from_bits(sym_addr)),
        None => (Ptr::null(), Ptr::null()),
    };
    env.mem.write(
        info,
        Dl_info {
            dli_fname,
            dli_fbase: Ptr::from_bits(base),
            dli_sname,
            dli_saddr,
        },
    );
    1
}

fn dlclose(env: &mut Environment, handle: MutVoidPtr) -> i32 {
    assert!(
        handle == RTLD_DEFAULT || is_known_library(env.mem.cstr_at_utf8(handle.cast()).unwrap())
    );
    0 // success
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(dlopen(_, _)),
    export_c_func!(dlsym(_, _)),
    export_c_func!(dlclose(_)),
    export_c_func!(dladdr(_, _)),
];
