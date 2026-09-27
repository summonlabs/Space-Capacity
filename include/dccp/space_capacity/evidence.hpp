// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - evidence references and caller-supplied instants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Evidence is what a caller offers to justify a statement about the physical
// world: a registry row it read, an inspection it performed, a policy it
// applied. Space Capacity does not verify the outside world. It records which
// evidence was offered, the generation of that evidence as observed, and the
// attempt at which Space Capacity itself observed it.
//
// An evidence reference is never treated as proof of current truth. Evidence
// carried across a restart is recovered evidence: it is reported as recovered
// and must be revalidated before it can support a new authoritative statement.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

// A point in time, as whole milliseconds since the Unix epoch in UTC. Space
// Capacity stores and compares timestamps; it reads a clock only when a caller
// asks it to stamp something, so no result depends on when it happened to run.
struct SC_API Timestamp final {
  std::int64_t unix_millis = 0;

  [[nodiscard]] bool is_zero() const noexcept { return unix_millis == 0; }
  [[nodiscard]] friend bool operator==(Timestamp a, Timestamp b) noexcept {
    return a.unix_millis == b.unix_millis;
  }
  [[nodiscard]] friend bool operator!=(Timestamp a, Timestamp b) noexcept { return !(a == b); }
  [[nodiscard]] friend bool operator<(Timestamp a, Timestamp b) noexcept {
    return a.unix_millis < b.unix_millis;
  }
  [[nodiscard]] friend bool operator<=(Timestamp a, Timestamp b) noexcept {
    return a.unix_millis <= b.unix_millis;
  }
  [[nodiscard]] friend bool operator>(Timestamp a, Timestamp b) noexcept { return b < a; }
  [[nodiscard]] friend bool operator>=(Timestamp a, Timestamp b) noexcept { return b <= a; }
};

// Reads the system UTC clock. The only place in this library that consults a
// clock; every other timestamp arrives from a caller or from a store.
SC_API Timestamp system_utc_now() noexcept;

// Deterministic rendering, e.g. "2026-03-01T12:00:00.000Z". Never locale
// dependent.
SC_API std::string to_text(Timestamp value);
SC_API bool parse_timestamp(std::string_view text, Timestamp& out) noexcept;

// One offered piece of evidence.
struct SC_API EvidenceRef final {
  // Which runtime the evidence came from.
  RegistryKind source = RegistryKind::none;
  // Identity of the evidence record in that runtime, byte-preserved.
  ExternalId id{};
  // Generation of the evidence record as observed, when the source has one.
  EntityGeneration generation{};
  // The attempt at which Space Capacity observed it. Set by the registry from
  // its own persisted counter, never taken from a request, so a caller cannot
  // forge freshness.
  AttemptId observed_at{};

  [[nodiscard]] bool empty() const noexcept { return id.empty(); }
  [[nodiscard]] bool observed() const noexcept { return !observed_at.is_zero(); }

  [[nodiscard]] friend bool operator==(const EvidenceRef& a, const EvidenceRef& b) noexcept {
    return a.source == b.source && a.id == b.id && a.generation == b.generation &&
           a.observed_at == b.observed_at;
  }
  [[nodiscard]] friend bool operator!=(const EvidenceRef& a, const EvidenceRef& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const EvidenceRef& a, const EvidenceRef& b) noexcept {
    if (a.source != b.source) return static_cast<int>(a.source) < static_cast<int>(b.source);
    if (a.id != b.id) return a.id < b.id;
    if (a.generation != b.generation) return a.generation < b.generation;
    return a.observed_at < b.observed_at;
  }
};

// An ordered, deduplicated, bounded set of evidence references.
class SC_API EvidenceSet final {
 public:
  EvidenceSet() = default;

  [[nodiscard]] static Result<EvidenceSet> build(std::vector<EvidenceRef> refs);

  [[nodiscard]] const std::vector<EvidenceRef>& refs() const noexcept { return refs_; }
  [[nodiscard]] bool empty() const noexcept { return refs_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }

  // True when every reference carries the attempt at which it was observed.
  // Evidence that this store never observed cannot support a fresh statement.
  [[nodiscard]] bool all_observed() const noexcept;

  [[nodiscard]] friend bool operator==(const EvidenceSet& a, const EvidenceSet& b) noexcept {
    return a.refs_ == b.refs_;
  }
  [[nodiscard]] friend bool operator!=(const EvidenceSet& a, const EvidenceSet& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const EvidenceSet& a, const EvidenceSet& b) noexcept {
    return a.refs_ < b.refs_;
  }

 private:
  std::vector<EvidenceRef> refs_;
};

// ---------------------------------------------------------------------------
// Operator-supplied metadata
// ---------------------------------------------------------------------------

// A bounded, validated free-text label. Not an identity.
class SC_API DisplayLabel final {
 public:
  DisplayLabel() noexcept = default;

  [[nodiscard]] static Result<DisplayLabel> parse(std::string_view text);

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string_view value() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const DisplayLabel& a, const DisplayLabel& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend bool operator!=(const DisplayLabel& a, const DisplayLabel& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const DisplayLabel& a, const DisplayLabel& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

// A bounded free-text note. Not an identity, never parsed.
class SC_API Note final {
 public:
  Note() noexcept = default;

  [[nodiscard]] static Result<Note> parse(std::string_view text);

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string_view value() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const Note& a, const Note& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend bool operator!=(const Note& a, const Note& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const Note& a, const Note& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

// An operator-supplied idempotency key. When a mutation carries one and the
// store has already applied a mutation with the same key at the same revision
// base, the stored outcome is returned and nothing is applied twice. The
// retained key table is bounded, so idempotency is bounded and explicit rather
// than unbounded in time.
template <class Tag>
class OperationKey final {
 public:
  OperationKey() noexcept = default;

  [[nodiscard]] static Result<OperationKey> parse(std::string_view text) {
    if (text.empty()) {
      return Error::make(ErrorCode::empty_value, "an operation key must not be empty");
    }
    if (text.size() > Limits::kMaxIdentifierBytes) {
      return Error::make(ErrorCode::identity_too_long, "operation key exceeds 128 bytes");
    }
    if (!is_valid_identifier(text)) {
      return Error::make(ErrorCode::malformed_identity,
                         "operation key must be " + std::string(identifier_syntax_help()));
    }
    OperationKey key;
    key.value_.assign(text.data(), text.size());
    return key;
  }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string_view value() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const OperationKey& a, const OperationKey& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend bool operator!=(const OperationKey& a, const OperationKey& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const OperationKey& a, const OperationKey& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

struct OperationKeyTag final {};
using RequestId = OperationKey<OperationKeyTag>;

}  // namespace dccp::space_capacity
