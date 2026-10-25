// Symphonia
// Copyright (c) 2019-2026 The Project Symphonia Developers.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The `fft` module implements the Fast Fourier Transform (FFT).
//!
//! The complex (I)FFT in this module supports a size up-to 65536. The FFT is implemented using the
//! radix-2 Cooley-Tukey algorithm.

use std::{f32, sync::Arc};
// MoltBrowser: see `moltbrowser_set_portable_fft()`.
use std::sync::atomic::{AtomicBool, Ordering};

use crate::dsp::complex::Complex;
use crate::dsp::fft::MAX_SIZE;

// MoltBrowser: rustfft's default planner picks AVX and FMA, SSE4.1 or NEON
// code for the CPU it runs on, and each rounds differently. Its scalar planner
// gives the same results on every CPU.
static PORTABLE_FFT: AtomicBool = AtomicBool::new(false);

/// MoltBrowser: with `portable`, (I)FFTs created afterwards use rustfft's
/// scalar code, whose results are the same on every CPU; without it (the
/// default), the fastest code for this CPU.
pub fn moltbrowser_set_portable_fft(portable: bool) {
    PORTABLE_FFT.store(portable, Ordering::Relaxed);
}

// MoltBrowser: the planner every (I)FFT is created with.
fn moltbrowser_plan(n: usize, direction: rustfft::FftDirection) -> Arc<dyn rustfft::Fft<f32>> {
    if PORTABLE_FFT.load(Ordering::Relaxed) {
        rustfft::FftPlannerScalar::<f32>::new().plan_fft(n, direction)
    } else {
        rustfft::FftPlanner::<f32>::new().plan_fft(n, direction)
    }
}

/// The complex Fast Fourier Transform (FFT).
pub struct Fft {
    scratch: Box<[Complex<f32>]>,
    fft: Arc<dyn rustfft::Fft<f32>>,
}

impl Fft {
    pub fn new(n: usize) -> Self {
        // Assert the limits of the FFT interface (shared by SIMD and no-SIMD implementations).
        assert!(n.is_power_of_two());
        assert!(n <= MAX_SIZE);

        // MoltBrowser: rustfft::FftPlanner, or the scalar one.
        let fft = moltbrowser_plan(n, rustfft::FftDirection::Forward);

        // Create a scratch buffer to
        let scratch = vec![Default::default(); fft.get_inplace_scratch_len()].into_boxed_slice();

        Self { scratch, fft }
    }

    /// Get the size of the FFT.
    pub fn size(&self) -> usize {
        self.fft.len()
    }

    /// Calculate the FFT.
    pub fn fft(&mut self, x: &[Complex<f32>], y: &mut [Complex<f32>]) {
        assert_eq!(x.len(), y.len());
        assert_eq!(x.len(), self.fft.len());

        // Copy the input buffer, `x`, to the output buffer, `y`, to perform an in-place transform
        // on the output buffer. This keeps the input immutable.
        y.copy_from_slice(x);
        self.fft.process_with_scratch(y, &mut self.scratch);
    }

    /// Calculate the FFT in-place.
    pub fn fft_inplace(&mut self, x: &mut [Complex<f32>]) {
        assert_eq!(x.len(), x.len());
        assert_eq!(x.len(), self.fft.len());
        self.fft.process_with_scratch(x, &mut self.scratch);
    }
}

/// The complex inverse Fast Fourier Transform (IFFT).
pub struct Ifft {
    scratch: Box<[Complex<f32>]>,
    ifft: Arc<dyn rustfft::Fft<f32>>,
}

impl Ifft {
    pub fn new(n: usize) -> Self {
        // Assert the limits of the FFT interface (shared by SIMD and no-SIMD implementations).
        assert!(n.is_power_of_two());
        assert!(n <= MAX_SIZE);

        // MoltBrowser: rustfft::FftPlanner, or the scalar one.
        let ifft = moltbrowser_plan(n, rustfft::FftDirection::Inverse);

        let scratch = vec![Default::default(); ifft.get_inplace_scratch_len()].into_boxed_slice();

        Self { scratch, ifft }
    }

    /// Get the size of the FFT.
    pub fn size(&self) -> usize {
        self.ifft.len()
    }

    /// Calculate the inverse FFT.
    pub fn ifft(&mut self, x: &[Complex<f32>], y: &mut [Complex<f32>]) {
        assert_eq!(x.len(), y.len());
        assert_eq!(x.len(), self.ifft.len());

        // Copy the input buffer, `x`, to the output buffer, `y`, to perform an in-place transform
        // on the output buffer. This keeps the input immutable.
        y.copy_from_slice(x);
        self.ifft.process_with_scratch(y, &mut self.scratch);

        // Output scaling.
        let c = 1.0 / y.len() as f32;

        for y in y.iter_mut() {
            *y *= c;
        }
    }

    /// Calculate the inverse FFT in-place.
    pub fn ifft_inplace(&mut self, x: &mut [Complex<f32>]) {
        assert_eq!(x.len(), self.ifft.len());
        self.ifft.process_with_scratch(x, &mut self.scratch);

        // Output scaling.
        let c = 1.0 / x.len() as f32;

        for x in x.iter_mut() {
            *x *= c;
        }
    }
}
