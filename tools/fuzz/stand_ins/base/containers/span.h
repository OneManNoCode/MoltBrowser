// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/containers/span.h, for
// tools/fuzz/molt_fuzz.py only: std::span with base::span's checked
// accessors (first / last / subspan / operator[] / copy_from CHECK their
// bounds, as Chromium's do) and its byte-span helpers.

#ifndef MOLT_FUZZ_STAND_INS_BASE_CONTAINERS_SPAN_H_
#define MOLT_FUZZ_STAND_INS_BASE_CONTAINERS_SPAN_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "base/check.h"

namespace base {

inline constexpr size_t dynamic_extent = std::dynamic_extent;

template <typename T, size_t N = dynamic_extent>
class span : public std::span<T, N> {
  using Base = std::span<T, N>;

 public:
  using Base::Base;
  constexpr span(const Base& other) noexcept : Base(other) {}  // NOLINT

  constexpr T& operator[](size_t i) const {
    CHECK(i < this->size());
    return this->data()[i];
  }

  template <size_t Count>
  constexpr span<T, Count> first() const {
    CHECK(Count <= this->size());
    return span<T, Count>(std::span<T, Count>(this->data(), Count));
  }
  constexpr span<T> first(size_t count) const {
    CHECK(count <= this->size());
    return span<T>(this->data(), count);
  }
  template <size_t Count>
  constexpr span<T, Count> last() const {
    CHECK(Count <= this->size());
    return span<T, Count>(
        std::span<T, Count>(this->data() + this->size() - Count, Count));
  }
  constexpr span<T> last(size_t count) const {
    CHECK(count <= this->size());
    return span<T>(this->data() + this->size() - count, count);
  }
  template <size_t Offset, size_t Count = dynamic_extent>
  constexpr auto subspan() const {
    CHECK(Offset <= this->size());
    if constexpr (Count == dynamic_extent) {
      return span<T>(this->data() + Offset, this->size() - Offset);
    } else {
      CHECK(Count <= this->size() - Offset);
      return span<T, Count>(std::span<T, Count>(this->data() + Offset, Count));
    }
  }
  constexpr span<T> subspan(size_t offset,
                            size_t count = dynamic_extent) const {
    CHECK(offset <= this->size());
    if (count == dynamic_extent) count = this->size() - offset;
    CHECK(count <= this->size() - offset);
    return span<T>(this->data() + offset, count);
  }

  template <typename U, size_t M>
  void copy_from(const std::span<U, M>& other) const {
    CHECK(this->size() == other.size());
    std::copy(other.begin(), other.end(), this->begin());
  }

  constexpr std::pair<span<T>, span<T>> split_at(size_t offset) const {
    return {first(offset), subspan(offset)};
  }
};

template <typename T, size_t N>
span(T (&)[N]) -> span<T, N>;
template <typename T, size_t N>
span(std::array<T, N>&) -> span<T, N>;
template <typename T, size_t N>
span(const std::array<T, N>&) -> span<const T, N>;
template <typename T, size_t N>
span(std::span<T, N>) -> span<T, N>;
template <typename It, typename EndOrSize>
span(It, EndOrSize) -> span<std::remove_reference_t<std::iter_reference_t<It>>>;
template <typename R>
  requires std::ranges::contiguous_range<R>
span(R&&) -> span<std::remove_reference_t<std::ranges::range_reference_t<R>>>;

template <typename T, size_t N>
span<const uint8_t> as_bytes(span<T, N> s) {
  return span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()),
                             s.size_bytes());
}
template <typename T, size_t N>
span<uint8_t> as_writable_bytes(span<T, N> s) {
  return span<uint8_t>(reinterpret_cast<uint8_t*>(s.data()), s.size_bytes());
}

inline span<const uint8_t> as_byte_span(std::string_view s) {
  return span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()),
                             s.size());
}
inline span<const uint8_t> as_byte_span(const std::string& s) {
  return as_byte_span(std::string_view(s));
}
inline span<const uint8_t> as_byte_span(const char* s) {
  return as_byte_span(std::string_view(s));
}
template <typename T, size_t N>
span<const uint8_t, N * sizeof(T)> as_byte_span(const std::array<T, N>& a) {
  return span<const uint8_t, N * sizeof(T)>(std::span<const uint8_t, N * sizeof(T)>(
      reinterpret_cast<const uint8_t*>(a.data()), N * sizeof(T)));
}
template <typename T>
  requires(std::ranges::contiguous_range<const T&> &&
           !std::is_convertible_v<const T&, std::string_view>)
span<const uint8_t> as_byte_span(const T& container) {
  return span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(std::ranges::data(container)),
      std::ranges::size(container) *
          sizeof(std::ranges::range_value_t<const T&>));
}
template <typename T>
  requires std::ranges::contiguous_range<T&>
span<uint8_t> as_writable_byte_span(T& container) {
  return span<uint8_t>(
      reinterpret_cast<uint8_t*>(std::ranges::data(container)),
      std::ranges::size(container) * sizeof(std::ranges::range_value_t<T&>));
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_CONTAINERS_SPAN_H_
