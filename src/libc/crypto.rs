/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! CommonCrypto and friends

use crate::dyld::FunctionExports;
use crate::mem::{ConstVoidPtr, MutPtr, SafeRead};
use crate::{export_c_func, Environment};
use digest::Digest;
use md5::Md5;
use sha1::Sha1;
use sha2::digest::generic_array::GenericArray;
use sha2::Sha256;

fn CC_MD5(env: &mut Environment, data: ConstVoidPtr, len: u32, md: MutPtr<u8>) -> MutPtr<u8> {
    let mut hasher = Md5::new();
    hasher.update(env.mem.bytes_at(data.cast(), len));
    let digest = hasher.finalize();
    env.mem.bytes_at_mut(md, 16).copy_from_slice(&digest[..]);
    md
}

fn CC_SHA1(env: &mut Environment, data: ConstVoidPtr, len: u32, md: MutPtr<u8>) -> MutPtr<u8> {
    let mut hasher = Sha1::new();
    hasher.update(env.mem.bytes_at(data.cast(), len));
    let digest = hasher.finalize();
    env.mem.bytes_at_mut(md, 20).copy_from_slice(&digest[..]);
    md
}

fn CC_SHA256(env: &mut Environment, data: ConstVoidPtr, len: u32, md: MutPtr<u8>) -> MutPtr<u8> {
    let mut hasher = Sha256::new();
    hasher.update(env.mem.bytes_at(data.cast(), len));
    let digest = hasher.finalize();
    env.mem.bytes_at_mut(md, 32).copy_from_slice(&digest[..]);
    md
}

/// All state lives in the app's context, so apps can copy it.
#[allow(non_camel_case_types)]
#[repr(C, packed)]
struct CC_SHA256_CTX {
    /// Length in bits, low word first.
    count: [u32; 2],
    hash: [u32; 8],
    wbuf: [u8; 64],
}
unsafe impl SafeRead for CC_SHA256_CTX {}

impl CC_SHA256_CTX {
    fn len_bits(&self) -> u64 {
        let count = self.count;
        (u64::from(count[1]) << 32) | u64::from(count[0])
    }

    fn update(&mut self, mut data: &[u8]) {
        let len_bits = self.len_bits();
        let mut used = (len_bits / 8 % 64) as usize;
        let len_bits = len_bits.wrapping_add(data.len() as u64 * 8);
        self.count = [len_bits as u32, (len_bits >> 32) as u32];

        let mut hash = self.hash;
        let mut wbuf = self.wbuf;
        while !data.is_empty() {
            let n = (64 - used).min(data.len());
            wbuf[used..used + n].copy_from_slice(&data[..n]);
            used += n;
            data = &data[n..];
            if used == 64 {
                sha2::compress256(&mut hash, &[GenericArray::clone_from_slice(&wbuf)]);
                used = 0;
            }
        }
        self.hash = hash;
        self.wbuf = wbuf;
    }

    fn finish(mut self) -> [u8; 32] {
        let len_bits = self.len_bits();
        let used = (len_bits / 8 % 64) as usize;
        let mut padding = vec![0x80];
        padding.resize(1 + (119 - used) % 64, 0);
        padding.extend_from_slice(&len_bits.to_be_bytes());
        self.update(&padding);
        self.hash
            .map(u32::to_be_bytes)
            .as_flattened()
            .try_into()
            .unwrap()
    }
}

fn CC_SHA256_Init(env: &mut Environment, c: MutPtr<CC_SHA256_CTX>) -> i32 {
    let ctx = CC_SHA256_CTX {
        count: [0; 2],
        hash: [
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
            0x5be0cd19,
        ],
        wbuf: [0; 64],
    };
    env.mem.write(c, ctx);
    1
}

fn CC_SHA256_Update(
    env: &mut Environment,
    c: MutPtr<CC_SHA256_CTX>,
    data: ConstVoidPtr,
    len: u32,
) -> i32 {
    let mut ctx = env.mem.read(c);
    ctx.update(env.mem.bytes_at(data.cast(), len));
    env.mem.write(c, ctx);
    1
}

fn CC_SHA256_Final(env: &mut Environment, md: MutPtr<u8>, c: MutPtr<CC_SHA256_CTX>) -> i32 {
    let digest = env.mem.read(c).finish();
    env.mem.bytes_at_mut(md, 32).copy_from_slice(&digest);
    1
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(CC_MD5(_, _, _)),
    export_c_func!(CC_SHA1(_, _, _)),
    export_c_func!(CC_SHA256(_, _, _)),
    export_c_func!(CC_SHA256_Init(_)),
    export_c_func!(CC_SHA256_Update(_, _, _)),
    export_c_func!(CC_SHA256_Final(_, _)),
];
