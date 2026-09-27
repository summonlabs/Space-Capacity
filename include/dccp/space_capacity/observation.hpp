// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - observations, freshness and revalidation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Space Capacity separates three things that are easy to conflate:
//
//   * an observation - "at revision R, node N, generation G, the free area was
//     A". An observation is a fact about a moment, not a permission.
//   * authority - the right to change state. Authority is carried by explicit
//     preconditions on generations and revisions, and a stale precondition is
//     refused rather than merged.
//   * freshness - whether an observation still describes the state committed
//     now.
//
// Reopening a store does not make anything fresh. Evidence observed before the
// restart keeps its original attempt identity, and the registry reports it as
// recovered until a caller revalidates it against current evidence. A recovered
// number is never presented as a current one.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

enum class Freshness : std::uint8_t {
  // Observed against the state committed now, after the most recent restart
  // and after the most recent mutation that touched the subject.
  fresh = 1,
  // Read out of a store and not yet revalidated against current evidence. The
  // value is what was persisted and it is reported with this mark.
  recovered = 2,
  // The state moved on after the observation was taken.
  stale = 3,
  // A newer observation of the same subject exists.
  superseded = 4,
  // The subject is no longer in the model.
  orphaned = 5,
};

SC_API std::string_view freshness_name(Freshness value) noexcept;
SC_API bool parse_freshness(std::string_view text, Freshness& out) noexcept;

// A token that identifies one observation of one subject at one revision.
struct SC_API ObservationToken final {
  SpaceNodeId subject{};
  EntityGeneration subject_generation{};
  RegistryRevision revision{};
  AttemptId attempt{};

  [[nodiscard]] bool empty() const noexcept { return subject.empty(); }

  [[nodiscard]] friend bool operator==(const ObservationToken& a,
                                       const ObservationToken& b) noexcept {
    return a.subject == b.subject && a.subject_generation == b.subject_generation &&
           a.revision == b.revision && a.attempt == b.attempt;
  }
  [[nodiscard]] friend bool operator!=(const ObservationToken& a,
                                       const ObservationToken& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const ObservationToken& a,
                                      const ObservationToken& b) noexcept {
    if (a.subject != b.subject) return a.subject < b.subject;
    if (a.subject_generation != b.subject_generation) {
      return a.subject_generation < b.subject_generation;
    }
    if (a.revision != b.revision) return a.revision < b.revision;
    return a.attempt < b.attempt;
  }
};

// The outcome of revalidating a token against current state.
struct SC_API Revalidation final {
  bool valid = false;
  Freshness freshness = Freshness::stale;
  ErrorCode reason = ErrorCode::ok;

  // The generations that were compared, so an operator can see exactly how far
  // behind the observation was.
  EntityGeneration expected_generation{};
  EntityGeneration actual_generation{};
  RegistryRevision expected_revision{};
  RegistryRevision actual_revision{};
  bool generation_compared = false;
  bool revision_compared = false;

  [[nodiscard]] bool ok() const noexcept { return valid; }
};

// Classification of one piece of evidence relative to the current session.
//
// `fresh_floor` is the persisted attempt sequence the store had reached when it
// was last opened. Evidence observed at or below that floor was written by an
// earlier session and is recovered; evidence above it was observed by the
// current session.
struct SC_API EvidenceFreshness final {
  AttemptId attempt{};
  Freshness freshness = Freshness::recovered;

  [[nodiscard]] friend bool operator==(const EvidenceFreshness& a,
                                       const EvidenceFreshness& b) noexcept {
    return a.attempt == b.attempt && a.freshness == b.freshness;
  }
};

SC_API Freshness classify_evidence(const EvidenceRef& evidence, AttemptId fresh_floor) noexcept;

}  // namespace dccp::space_capacity
