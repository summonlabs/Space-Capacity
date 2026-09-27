// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - strongly typed identities, generations and epochs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two rules are enforced by the type system:
//
//   * identity is not address. A 128-byte identifier names a thing; it never
//     encodes where that thing sits, how it is reached, or which registry row
//     it came from.
//   * families do not convert. A claim identity is not a node identity is not
//     a store identity, and no implicit conversion exists between them.
//
// The identifier grammar is the one the DCCP sibling registries use:
//
//   identifiers are 1..128 bytes, start and end with [0-9A-Za-z], and contain
//   only [0-9A-Za-z._:-]
//
// Space Capacity adds no second spelling. One value has exactly one canonical
// form, a parser that accepts it accepts nothing else, and a comparison is
// byte-wise over that canonical form.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"

namespace dccp::space_capacity {

// The exact syntax help text the sibling registries publish, reproduced so a
// caller can print one sentence about identity in the whole family.
SC_API std::string_view identifier_syntax_help() noexcept;

// True when `text` satisfies the DCCP identifier grammar.
SC_API bool is_valid_identifier(std::string_view text) noexcept;

// True when `text` is valid UTF-8, contains no byte below 0x20 and no 0x7F,
// and is at most kMaxLabelBytes. Used for operator-facing labels only, never as
// identity, and it is deliberately wider than the identifier grammar because a
// label may contain spaces and non-ASCII text.
SC_API bool is_valid_label(std::string_view text) noexcept;
SC_API bool is_utf8(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Identity families
// ---------------------------------------------------------------------------

struct SpaceNodeTag final {
  static constexpr std::string_view kind_name = "node";
};
struct OccupancyClaimTag final {
  static constexpr std::string_view kind_name = "claim";
};
struct ExclusionRegionTag final {
  static constexpr std::string_view kind_name = "exclusion";
};
struct ClearanceConstraintTag final {
  static constexpr std::string_view kind_name = "clearance";
};
struct ExpansionZoneTag final {
  static constexpr std::string_view kind_name = "zone";
};
struct StoreTag final {
  static constexpr std::string_view kind_name = "store";
};

// A text-backed identity in one family. Values of different families are
// unrelated types: there is no constructor, conversion operator or comparison
// that would let one stand in for another.
template <class Tag>
class StrongId final {
 public:
  using tag_type = Tag;

  StrongId() noexcept = default;

  // The only way to make one from text. The empty string is not an identity.
  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    if (text.empty()) {
      return Error::make(ErrorCode::empty_value, "an identity must not be empty");
    }
    if (text.size() > Limits::kMaxIdentifierBytes) {
      return Error::make(ErrorCode::identity_too_long,
                         "identity exceeds " + std::to_string(Limits::kMaxIdentifierBytes) +
                             " bytes");
    }
    if (!is_valid_identifier(text)) {
      return Error::make(ErrorCode::malformed_identity,
                         "identity must satisfy: " + std::string(identifier_syntax_help()));
    }
    StrongId result;
    result.value_.assign(text.data(), text.size());
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string_view value() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const StrongId& a, const StrongId& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend bool operator!=(const StrongId& a, const StrongId& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const StrongId& a, const StrongId& b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend bool operator<=(const StrongId& a, const StrongId& b) noexcept {
    return !(b < a);
  }
  [[nodiscard]] friend bool operator>(const StrongId& a, const StrongId& b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend bool operator>=(const StrongId& a, const StrongId& b) noexcept {
    return !(a < b);
  }

 private:
  std::string value_;
};

using SpaceNodeId = StrongId<SpaceNodeTag>;
using OccupancyClaimId = StrongId<OccupancyClaimTag>;
using ExclusionRegionId = StrongId<ExclusionRegionTag>;
using ClearanceConstraintId = StrongId<ClearanceConstraintTag>;
using ExpansionZoneId = StrongId<ExpansionZoneTag>;
using StoreId = StrongId<StoreTag>;

// ---------------------------------------------------------------------------
// Monotonic counters
// ---------------------------------------------------------------------------

// A per-record generation. Every mutation of a record's mutable metadata
// produces the next generation; a request that names an older generation is
// refused rather than merged.

template <typename Tag>
class CounterValue final {
 public:
  using tag_type = Tag;

  constexpr CounterValue() noexcept = default;
  constexpr explicit CounterValue(std::uint64_t value) noexcept : value_(value) {}

  // Parses a decimal rendering. Rejects signs, whitespace and leading zeros so
  // that one value has exactly one spelling.
  [[nodiscard]] static Result<CounterValue> parse(std::string_view text);

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_max() const noexcept {
    return value_ == Limits::kMaxCounter;
  }

  [[nodiscard]] Result<CounterValue> next() const {
    if (is_max()) {
      return Error::make(ErrorCode::arithmetic_overflow,
                         "counter " + std::string(Tag::kind_name) + " reached its maximum");
    }
    return CounterValue(value_ + 1);
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  [[nodiscard]] friend constexpr bool operator==(CounterValue a, CounterValue b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(CounterValue a, CounterValue b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(CounterValue a, CounterValue b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<=(CounterValue a, CounterValue b) noexcept {
    return a.value_ <= b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>(CounterValue a, CounterValue b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend constexpr bool operator>=(CounterValue a, CounterValue b) noexcept {
    return b <= a;
  }

 private:
  std::uint64_t value_ = 0;
};

struct EntityGenerationName final {
  static constexpr std::string_view kind_name = "generation";
};
struct RegistryRevisionName final {
  static constexpr std::string_view kind_name = "revision";
};
struct StoreIncarnationName final {
  static constexpr std::string_view kind_name = "incarnation";
};

using EntityGeneration = CounterValue<EntityGenerationName>;
using RegistryRevision = CounterValue<RegistryRevisionName>;
using StoreIncarnation = CounterValue<StoreIncarnationName>;

// The identity of one observation made by one incarnation of one store.
//
// `sequence` is persisted in the store and continues across a reopen, so the
// pair is totally ordered within a store: an attempt made before a restart
// always sorts below every attempt made after it. That is what lets the
// registry treat evidence recovered from a store as recovered rather than as
// freshly observed.
class SC_API AttemptId final {
 public:
  constexpr AttemptId() noexcept = default;
  constexpr AttemptId(StoreIncarnation incarnation, std::uint64_t sequence) noexcept
      : incarnation_(incarnation), sequence_(sequence) {}

  [[nodiscard]] constexpr StoreIncarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] constexpr std::uint64_t sequence() const noexcept { return sequence_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return sequence_ == 0; }

  [[nodiscard]] friend constexpr bool operator==(AttemptId a, AttemptId b) noexcept {
    return a.incarnation_ == b.incarnation_ && a.sequence_ == b.sequence_;
  }
  [[nodiscard]] friend constexpr bool operator!=(AttemptId a, AttemptId b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(AttemptId a, AttemptId b) noexcept {
    if (a.incarnation_ != b.incarnation_) return a.incarnation_ < b.incarnation_;
    return a.sequence_ < b.sequence_;
  }
  [[nodiscard]] friend constexpr bool operator<=(AttemptId a, AttemptId b) noexcept {
    return !(b < a);
  }
  [[nodiscard]] friend constexpr bool operator>(AttemptId a, AttemptId b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend constexpr bool operator>=(AttemptId a, AttemptId b) noexcept {
    return !(a < b);
  }

  [[nodiscard]] std::string to_string() const;

 private:
  StoreIncarnation incarnation_{};
  std::uint64_t sequence_ = 0;
};

}  // namespace dccp::space_capacity

namespace std {

template <class Tag>
struct hash<dccp::space_capacity::StrongId<Tag>> {
  std::size_t operator()(const dccp::space_capacity::StrongId<Tag>& value) const noexcept {
    return std::hash<std::string_view>{}(value.value());
  }
};

template <typename Tag>
struct hash<dccp::space_capacity::CounterValue<Tag>> {
  std::size_t operator()(const dccp::space_capacity::CounterValue<Tag>& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};

}  // namespace std
