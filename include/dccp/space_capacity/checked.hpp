// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - checked arithmetic for every authoritative quantity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Capacity, generation, revision, attempt, extent and byte-count arithmetic
// all flow through these helpers. A saturation or a wrap is never authority:
// it is reported as an overflow outcome and the caller must refuse the
// operation.

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "dccp/space_capacity/export.hpp"

namespace dccp::space_capacity {

// Result of a checked operation. On overflow `value` is zero and `overflowed`
// is true; callers must test `overflowed` before reading `value`.
template <typename T>
struct Checked final {
  T value{};
  bool overflowed = false;

  [[nodiscard]] constexpr bool ok() const noexcept { return !overflowed; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return !overflowed; }
};

namespace detail {

template <typename T>
[[nodiscard]] constexpr bool add_overflows(T a, T b, T& out) noexcept {
  if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) return true;
  if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) return true;
  out = static_cast<T>(a + b);
  return false;
}

template <typename T>
[[nodiscard]] constexpr bool sub_overflows(T a, T b, T& out) noexcept {
  if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) return true;
  if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) return true;
  out = static_cast<T>(a - b);
  return false;
}

// Every branch decides before the product is formed, so no intermediate signed
// overflow is ever evaluated.
template <typename T>
[[nodiscard]] constexpr bool mul_overflows(T a, T b, T& out) noexcept {
  constexpr T kMin = std::numeric_limits<T>::min();
  constexpr T kMax = std::numeric_limits<T>::max();
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  if (a == static_cast<T>(-1)) {
    if (b == kMin) return true;
    out = static_cast<T>(-b);
    return false;
  }
  if (b == static_cast<T>(-1)) {
    if (a == kMin) return true;
    out = static_cast<T>(-a);
    return false;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > static_cast<T>(kMax / b)) return true;
    } else {
      if (a > static_cast<T>(kMin / b)) return true;
    }
  } else {
    if (b > 0) {
      if (a < static_cast<T>(kMin / b)) return true;
    } else {
      if (a < static_cast<T>(kMax / b)) return true;
    }
  }
  out = static_cast<T>(a * b);
  return false;
}

}  // namespace detail

// Unsigned helpers, used for byte counts, generations, revisions, rack units
// and square millimetres.
template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr Checked<T> checked_add(T a, T b) noexcept {
  Checked<T> r{};
  if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
    r.overflowed = true;
    return r;
  }
  r.value = static_cast<T>(a + b);
  return r;
}

template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr Checked<T> checked_sub(T a, T b) noexcept {
  Checked<T> r{};
  if (b > a) {
    r.overflowed = true;
    return r;
  }
  r.value = static_cast<T>(a - b);
  return r;
}

template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr Checked<T> checked_mul(T a, T b) noexcept {
  Checked<T> r{};
  if (a != 0 && b > static_cast<T>(std::numeric_limits<T>::max() / a)) {
    r.overflowed = true;
    return r;
  }
  r.value = static_cast<T>(a * b);
  return r;
}

// Signed helpers, used for millimetre geometry and for signed deltas in diffs.
template <typename T>
  requires std::is_signed_v<T>
[[nodiscard]] constexpr Checked<T> checked_add_signed(T a, T b) noexcept {
  Checked<T> r{};
  T out = 0;
  if (detail::add_overflows(a, b, out)) {
    r.overflowed = true;
    return r;
  }
  r.value = out;
  return r;
}

template <typename T>
  requires std::is_signed_v<T>
[[nodiscard]] constexpr Checked<T> checked_sub_signed(T a, T b) noexcept {
  Checked<T> r{};
  T out = 0;
  if (detail::sub_overflows(a, b, out)) {
    r.overflowed = true;
    return r;
  }
  r.value = out;
  return r;
}

template <typename T>
  requires std::is_signed_v<T>
[[nodiscard]] constexpr Checked<T> checked_mul_signed(T a, T b) noexcept {
  Checked<T> r{};
  T out = 0;
  if (detail::mul_overflows(a, b, out)) {
    r.overflowed = true;
    return r;
  }
  r.value = out;
  return r;
}

// Saturating subtraction used by the ledger, where "used more than exists" is
// a reportable condition rather than an arithmetic failure. The caller is
// expected to set an over-committed flag when the result clamps.
template <typename T>
  requires std::is_signed_v<T>
[[nodiscard]] constexpr T saturating_sub(T a, T b) noexcept {
  T out = 0;
  if (detail::sub_overflows(a, b, out)) return 0;
  return out > 0 ? out : 0;
}

// Monotonic increment that refuses to wrap. Used for generations and
// revisions, where a silent wrap would resurrect stale authority.
template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr Checked<T> checked_increment(T a) noexcept {
  return checked_add(a, static_cast<T>(1));
}

}  // namespace dccp::space_capacity
