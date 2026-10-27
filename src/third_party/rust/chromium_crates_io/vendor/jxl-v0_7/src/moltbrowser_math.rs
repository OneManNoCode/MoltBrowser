//! MoltBrowser: the standard-library math the decoder uses outside the
//! SIMD kernels (dequantization weights, transfer functions, XYB biases,
//! splines), on one switch with `jxl_simd`'s scalar code.
//!
//! By default each function is the standard library's, which calls the
//! platform's C library (glibc picks some float functions by CPU feature,
//! and every OS has its own), exactly as before. After
//! `moltbrowser_set_portable(true)` each is the `libm` crate's, Rust ports
//! of musl's functions in plain arithmetic, the same on every CPU and OS,
//! and every `simd_function!` runs its scalar code
//! (`jxl_simd::moltbrowser_set_portable`). MoltBrowser's MoltNet renderers
//! set it before they decode any image.

/// Turns the portable mode on or off for the whole process.
pub fn moltbrowser_set_portable(portable: bool) {
    jxl_simd::moltbrowser_set_portable(portable);
}

#[inline]
fn portable() -> bool {
    jxl_simd::moltbrowser_portable()
}

/// The functions, for `f32` and `f64` alike.
pub(crate) trait PortableMath: Copy {
    fn std_powf(self, y: Self) -> Self;
    fn libm_powf(self, y: Self) -> Self;
    fn std_exp(self) -> Self;
    fn libm_exp(self) -> Self;
    fn std_ln(self) -> Self;
    fn libm_ln(self) -> Self;
    fn std_log2(self) -> Self;
    fn libm_log2(self) -> Self;
    fn std_cbrt(self) -> Self;
    fn libm_cbrt(self) -> Self;
    fn std_hypot(self, y: Self) -> Self;
    fn libm_hypot(self, y: Self) -> Self;
}

impl PortableMath for f32 {
    fn std_powf(self, y: f32) -> f32 { self.powf(y) }
    fn libm_powf(self, y: f32) -> f32 { libm::powf(self, y) }
    fn std_exp(self) -> f32 { self.exp() }
    fn libm_exp(self) -> f32 { libm::expf(self) }
    fn std_ln(self) -> f32 { self.ln() }
    fn libm_ln(self) -> f32 { libm::logf(self) }
    fn std_log2(self) -> f32 { self.log2() }
    fn libm_log2(self) -> f32 { libm::log2f(self) }
    fn std_cbrt(self) -> f32 { self.cbrt() }
    fn libm_cbrt(self) -> f32 { libm::cbrtf(self) }
    fn std_hypot(self, y: f32) -> f32 { self.hypot(y) }
    fn libm_hypot(self, y: f32) -> f32 { libm::hypotf(self, y) }
}

impl PortableMath for f64 {
    fn std_powf(self, y: f64) -> f64 { self.powf(y) }
    fn libm_powf(self, y: f64) -> f64 { libm::pow(self, y) }
    fn std_exp(self) -> f64 { self.exp() }
    fn libm_exp(self) -> f64 { libm::exp(self) }
    fn std_ln(self) -> f64 { self.ln() }
    fn libm_ln(self) -> f64 { libm::log(self) }
    fn std_log2(self) -> f64 { self.log2() }
    fn libm_log2(self) -> f64 { libm::log2(self) }
    fn std_cbrt(self) -> f64 { self.cbrt() }
    fn libm_cbrt(self) -> f64 { libm::cbrt(self) }
    fn std_hypot(self, y: f64) -> f64 { self.hypot(y) }
    fn libm_hypot(self, y: f64) -> f64 { libm::hypot(self, y) }
}

/// `x.powf(y)`.
pub(crate) fn powf<T: PortableMath>(x: T, y: T) -> T {
    if portable() { x.libm_powf(y) } else { x.std_powf(y) }
}

/// `x.exp()`.
pub(crate) fn exp<T: PortableMath>(x: T) -> T {
    if portable() { x.libm_exp() } else { x.std_exp() }
}

/// `x.ln()`.
pub(crate) fn ln<T: PortableMath>(x: T) -> T {
    if portable() { x.libm_ln() } else { x.std_ln() }
}

/// `x.log2()`.
pub(crate) fn log2<T: PortableMath>(x: T) -> T {
    if portable() { x.libm_log2() } else { x.std_log2() }
}

/// `x.cbrt()`.
pub(crate) fn cbrt<T: PortableMath>(x: T) -> T {
    if portable() { x.libm_cbrt() } else { x.std_cbrt() }
}

/// `x.hypot(y)`.
pub(crate) fn hypot<T: PortableMath>(x: T, y: T) -> T {
    if portable() { x.libm_hypot(y) } else { x.std_hypot(y) }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Off: the standard library's functions, bit for bit. (The portable
    // ones are libm's, tested there; switching them on here would race with
    // the crate's other tests, which run in the same process.)
    #[test]
    fn off_is_the_standard_library() {
        for x in [0.25f32, 0.5, 1.0, 1.5, 2.0, 7.25, 100.0] {
            assert_eq!(powf(x, 0.45).to_bits(), x.powf(0.45).to_bits());
            assert_eq!(exp(-x).to_bits(), (-x).exp().to_bits());
            assert_eq!(ln(x).to_bits(), x.ln().to_bits());
            assert_eq!(log2(x).to_bits(), x.log2().to_bits());
            assert_eq!(cbrt(x).to_bits(), x.cbrt().to_bits());
            assert_eq!(hypot(x, 3.0).to_bits(), x.hypot(3.0).to_bits());
            let d = f64::from(x);
            assert_eq!(powf(d, 1.25).to_bits(), d.powf(1.25).to_bits());
            assert_eq!(exp(d).to_bits(), d.exp().to_bits());
            assert_eq!(log2(d).to_bits(), d.log2().to_bits());
        }
    }
}
