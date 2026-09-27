// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the test harness.
//
// Deliberately tiny and dependency free. Every test executable links only the
// library and this header. There is no timeout anywhere: a hanging test is a
// defect to diagnose, not a test to abort.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace sc_test {

inline int checks = 0;
inline int failures = 0;
inline const char* current_case = "";

inline void begin_case(const char* name) { current_case = name; }

inline void record(bool ok, const char* expression, const char* file, int line) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s:%d in case [%s]: %s\n", file, line, current_case, expression);
}

inline int summary(const char* suite) {
  std::fprintf(stdout, "%s: %d checks, %d failures\n", suite, checks, failures);
  if (failures == 0) {
    std::fprintf(stdout, "RESULT: PASS\n");
    return 0;
  }
  std::fprintf(stdout, "RESULT: FAIL\n");
  return 1;
}

// A deterministic pseudo-random generator. Every randomized test fixes its
// seed, prints it, and can be replayed exactly.
class Rng final {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {
    std::fprintf(stdout, "seed: %llu\n", static_cast<unsigned long long>(state_));
  }

  std::uint64_t next() noexcept {
    // splitmix64
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t bounded(std::uint64_t limit) noexcept { return limit == 0 ? 0 : next() % limit; }

  std::int64_t between(std::int64_t low, std::int64_t high) noexcept {
    if (high <= low) return low;
    return low + static_cast<std::int64_t>(bounded(static_cast<std::uint64_t>(high - low + 1)));
  }

  bool coin() noexcept { return (next() & 1u) != 0; }

 private:
  std::uint64_t state_;
};

}  // namespace sc_test

#define SC_CHECK(expression) \
  ::sc_test::record(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define SC_CHECK_EQ(actual, expected)                                                          \
  do {                                                                                         \
    const auto sc_actual = (actual);                                                           \
    const auto sc_expected = (expected);                                                       \
    ::sc_test::record(sc_actual == sc_expected, #actual " == " #expected, __FILE__, __LINE__);  \
  } while (false)

#define SC_CHECK_NE(actual, unexpected)                                                        \
  do {                                                                                         \
    const auto sc_actual = (actual);                                                           \
    const auto sc_unexpected = (unexpected);                                                   \
    ::sc_test::record(sc_actual != sc_unexpected, #actual " != " #unexpected, __FILE__,         \
                      __LINE__);                                                               \
  } while (false)

#define SC_CASE(name) ::sc_test::begin_case(name)
