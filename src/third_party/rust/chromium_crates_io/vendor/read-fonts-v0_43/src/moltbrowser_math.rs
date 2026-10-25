//! MoltBrowser: the floating-point functions of glyph transforms, with one
//! process-wide choice of implementation.
//!
//! read-fonts and skrifa compute the rotations and skews of colour glyphs
//! (COLRv1 `PaintRotate` and `PaintSkew`), the rotations and skews of
//! variable composites (`VARC`) and the scale of composite glyphs' offsets
//! with `sin`, `cos`, `tan` and `hypot`. With the `std` feature those are the
//! platform's C library functions, whose last bits differ between operating
//! systems and CPUs. By default these functions are exactly what the call
//! sites called before; after `set_portable(true)` they are the `libm`
//! crate's, plain Rust ports of musl's functions that give the same result on
//! every CPU and OS. Skia's Fontations backend turns it on in MoltNet's
//! renderers.

use core::sync::atomic::{AtomicBool, Ordering};

#[cfg(feature = "libm")]
#[allow(unused_imports)]
use core_maths::*;

static PORTABLE: AtomicBool = AtomicBool::new(false);

/// Whether this process uses the portable functions. Set once, before any
/// glyph is loaded; any thread.
pub fn set_portable(portable: bool) {
    PORTABLE.store(portable, Ordering::Relaxed);
}

/// See [`set_portable`].
pub fn is_portable() -> bool {
    PORTABLE.load(Ordering::Relaxed)
}

/// `x.sin()`, or the portable sine.
pub fn sin(x: f32) -> f32 {
    if is_portable() {
        libm::sinf(x)
    } else {
        x.sin()
    }
}

/// `x.cos()`, or the portable cosine.
pub fn cos(x: f32) -> f32 {
    if is_portable() {
        libm::cosf(x)
    } else {
        x.cos()
    }
}

/// `x.sin_cos()`, or the portable sine and cosine.
pub fn sin_cos(x: f32) -> (f32, f32) {
    if is_portable() {
        (libm::sinf(x), libm::cosf(x))
    } else {
        x.sin_cos()
    }
}

/// `x.tan()`, or the portable tangent.
pub fn tan(x: f32) -> f32 {
    if is_portable() {
        libm::tanf(x)
    } else {
        x.tan()
    }
}

/// `x.hypot(y)`, or the portable hypotenuse.
pub fn hypot(x: f32, y: f32) -> f32 {
    if is_portable() {
        libm::hypotf(x, y)
    } else {
        x.hypot(y)
    }
}
