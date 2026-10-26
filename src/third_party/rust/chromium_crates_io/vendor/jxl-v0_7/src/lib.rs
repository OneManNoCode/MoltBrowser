// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#![deny(unsafe_code)]
pub mod api;
pub mod bit_reader;
pub mod color;
pub mod entropy_coding;
pub mod error;
pub mod features;
pub mod frame;
pub mod headers;
pub mod icc;
pub mod image;
// MoltBrowser: one process-wide switch to code that decodes alike on every
// CPU and OS.
pub mod moltbrowser_math;
pub mod render;
pub mod util;

pub use moltbrowser_math::moltbrowser_set_portable;

#[cfg(test)]
mod tests;

// TODO: Move these to a more appropriate location.
const GROUP_DIM: usize = 256;
const BLOCK_DIM: usize = 8;
const BLOCK_SIZE: usize = BLOCK_DIM * BLOCK_DIM;
#[allow(clippy::excessive_precision)]
const MIN_SIGMA: f32 = -3.90524291751269967465540850526868;
