// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: subnormal floats in the portable audio code
// (media/base/molt_portable_media.h has the rationale): the flush at the
// portable floor and the scope that turns the CPU's flush-to-zero modes off.
// Apart from molt_portable_media.cc so that it builds with nothing but the
// audio bus, for the standalone harnesses that check it on every CPU.

#include <cstdint>

#include "base/containers/span.h"
#include "build/build_config.h"
#include "media/base/audio_bus.h"
#include "media/base/molt_portable_media.h"

#if defined(ARCH_CPU_X86_FAMILY)
#include <xmmintrin.h>
#endif

namespace media {

namespace {

// The floating-point control register of the calling thread and its
// flush-to-zero bits.
#if defined(ARCH_CPU_X86_FAMILY)
// MXCSR: DAZ (bit 6) and FTZ (bit 15), as cc::ScopedSubnormalFloatDisabler
// sets them.
constexpr uint64_t kFlushBits = 0x8040;
uint64_t ReadFpControl() {
  return _mm_getcsr();
}
void WriteFpControl(uint64_t value) {
  _mm_setcsr(static_cast<unsigned int>(value));
}
#elif defined(ARCH_CPU_ARM64)
// FPCR.FZ (bit 24); FZ16 (bit 19) only applies to half precision, which
// audio code doesn't compute in.
constexpr uint64_t kFlushBits = uint64_t{1} << 24;
uint64_t ReadFpControl() {
  uint64_t value;
  asm volatile("mrs %x[value], fpcr" : [value] "=r"(value));
  return value;
}
void WriteFpControl(uint64_t value) {
  asm volatile("msr fpcr, %x[value]" : : [value] "r"(value));
}
#elif defined(ARCH_CPU_ARM_FAMILY)
// FPSCR.FZ (bit 24).
constexpr uint64_t kFlushBits = uint64_t{1} << 24;
uint64_t ReadFpControl() {
  uint32_t value;
  asm volatile("vmrs %[value], fpscr" : [value] "=r"(value));
  return value;
}
void WriteFpControl(uint64_t value) {
  const uint32_t word = static_cast<uint32_t>(value);
  asm volatile("vmsr fpscr, %[word]" : : [word] "r"(word));
}
#else
// Other architectures: no flush-to-zero mode this code knows of.
constexpr uint64_t kFlushBits = 0;
uint64_t ReadFpControl() {
  return 0;
}
void WriteFpControl(uint64_t) {}
#endif

}  // namespace

void FlushTinyPortableSamples(base::span<float> samples) {
  for (float& sample : samples) {
    sample = FlushTinyPortableSample(sample);
  }
}

void FlushTinyPortableSamples(AudioBus& bus) {
  for (int channel = 0; channel < bus.channels(); ++channel) {
    FlushTinyPortableSamples(bus.channel(channel));
  }
}

ScopedPortableSubnormals::ScopedPortableSubnormals() {
  if constexpr (kFlushBits != 0) {
    const uint64_t control = ReadFpControl();
    if (control & kFlushBits) {
      saved_ = control;
      restore_ = true;
      WriteFpControl(control & ~kFlushBits);
    }
  }
}

ScopedPortableSubnormals::~ScopedPortableSubnormals() {
  if (restore_) {
    WriteFpControl(saved_);
  }
}

// static
bool ScopedPortableSubnormals::FlushModeIsOn() {
  return (ReadFpControl() & kFlushBits) != 0;
}

}  // namespace media
