/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Compatibility with games made with Ideaworks3D's Airplay SDK (later
//! renamed Marmalade). Not to be confused with Apple's AirPlay.

use crate::mach_o::MachO;
use crate::mem::{GuestUSize, Mem, Ptr};

/// Airplay's `s3eGetCodeHash()`, whose result is the key `s3eDecodeData()`
/// decodes the game with.
const GET_CODE_HASH_SYMBOL: &str = "__Z14s3eGetCodeHashv";
/// The hash's starting value.
const HASH_SEED: u32 = 1234;
/// How much of the function to search for the values it uses.
const SEARCH_SIZE: GuestUSize = 0x300;

/// Airplay games decode themselves with a key made by hashing their own code
/// in memory, from the start of `__text` to the end of `__symbolstub1`
/// (which, despite its name, is where the game's code really is). That range
/// includes the symbol stubs, which touchHLE's dynamic linker rewrites, so
/// the hash would come out wrong and the game would decode itself into
/// garbage. So, before linking, work out the hash a real device would get,
/// and make `s3eGetCodeHash()` return it.
///
/// This must be called before the executable is linked.
pub fn fix_code_hash(executable: &MachO, mem: &mut Mem) {
    let Some(&function) = executable.exported_symbols.get(GET_CODE_HASH_SYMBOL) else {
        return;
    };
    let (Some(text), Some(code), Some(cstrings)) = (
        executable.get_section("__text"),
        executable.get_section("__symbolstub1"),
        executable.get_section("__cstring"),
    ) else {
        log!("Warning: Airplay SDK game without the sections s3eGetCodeHash() hashes, leaving it alone.");
        return;
    };

    // Only replace a version of the function known to hash this way: one
    // that uses the hash's starting value and both section names. Other
    // versions might hash something else.
    let is_cstring = |addr: u32, expected: &str| {
        (cstrings.addr..cstrings.addr + cstrings.size).contains(&addr)
            && mem.cstr_at_utf8(Ptr::<u8, false>::from_bits(addr)) == Ok(expected)
    };
    let words: Vec<u32> = (0..SEARCH_SIZE / 4)
        .map(|i| mem.read(Ptr::<u32, false>::from_bits(function + i * 4)))
        .collect();
    let is_known_version = function & 1 == 0 // ARM, not Thumb
        && words.contains(&HASH_SEED)
        && words.iter().any(|&w| is_cstring(w, "__text"))
        && words.iter().any(|&w| is_cstring(w, "__symbolstub1"));
    if !is_known_version {
        log!("Warning: Airplay SDK game with an unknown s3eGetCodeHash(), leaving it alone. It may fail to start.");
        return;
    }

    // As the game does it: hash = hash * 31 + byte, over the unmodified code.
    let start = text.addr;
    let end = code.addr + code.size;
    let hash = mem
        .bytes_at(Ptr::<u8, false>::from_bits(start), end - start)
        .iter()
        .fold(HASH_SEED, |hash, &byte| {
            hash.wrapping_mul(31).wrapping_add(byte.into())
        });

    // Replace the function with one that returns the hash:
    //     ldr r0, [pc]    @ the word after the next instruction
    //     bx lr
    //     .word hash
    let function = Ptr::<u32, true>::from_bits(function);
    mem.write(function, 0xe59f0000);
    mem.write(function + 1, 0xe12fff1e);
    mem.write(function + 2, hash);

    log!(
        "Airplay SDK game: s3eGetCodeHash() will return {:#x}, the hash of its unmodified code.",
        hash
    );
}
