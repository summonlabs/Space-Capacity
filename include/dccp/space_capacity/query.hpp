// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - fit and availability queries.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A query observes. It answers two questions and no others:
//
//   * what is the physical footprint situation of this node now, at this
//     generation, after occupancy, incompatibilities, reserved footprint,
//     clearance and expansion constraints have been applied;
//   * would a footprint of this shape fit, and if so where are the placements
//     that would take it.
//
// A query never grants placement, never reserves space and never authorises a
// workload, an asset or a rack to be put anywhere. Taking the space requires a
// separate, explicitly preconditioned mutation that records a claim, and that
// mutation is refused when the space is not free. An assessment carries the
// revision and attempt it was made at so a caller can detect that the answer
// went stale before it acted on it.
//
// Completeness of the planar search
//   The planar placement search enumerates candidate corners derived from the
//   obstacles and the plane boundary, rounded up to the requested alignment.
//   That candidate set is provably complete: any feasible axis-aligned
//   placement can be slid up-left within its own free interval until it rests
//   against the plane boundary or against an obstacle, and the first aligned
//   position at or after that resting point is in the candidate set. The
//   property test checks the claim against a brute-force search over small
//   integer grids.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

enum class FitVerdict : std::uint8_t {
  // At least one placement exists and is listed.
  fits = 1,
  // No placement exists anywhere inside the declared, usable envelope.
  does_not_fit = 2,
  // The question cannot be answered from the state as it stands: an envelope is
  // undeclared, an input could not be read, or the candidate search budget was
  // exhausted before the space was covered. It is not a "no".
  indeterminate = 3,
};

SC_API std::string_view fit_verdict_name(FitVerdict value) noexcept;
SC_API bool parse_fit_verdict(std::string_view text, FitVerdict& out) noexcept;

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

// Why a caller is asking. Used to apply exclusions that name specific occupant
// kinds and to explain refusals. It never widens what is permitted.
struct SC_API FitContext final {
  OccupantKind occupant = OccupantKind::unknown;

  // When set, planned and submitted claims are treated as if they were
  // committed for this query, so a caller can see the answer a future commit
  // would face. When clear, only consuming claims consume.
  bool count_pending = false;

  // Reading used to evaluate reservation expiry. A reservation past its
  // `not_after` at this reading holds nothing. When absent, no reservation is
  // treated as expired.
  std::optional<Timestamp> now{};

  [[nodiscard]] friend bool operator==(const FitContext& a, const FitContext& b) noexcept {
    return a.occupant == b.occupant && a.count_pending == b.count_pending && a.now == b.now;
  }
  [[nodiscard]] friend bool operator!=(const FitContext& a, const FitContext& b) noexcept {
    return !(a == b);
  }
};

// A request for a contiguous run of whole rack units inside one rack.
struct SC_API RackUnitFitRequest final {
  SpaceNodeId rack{};
  RackUnits needed{};

  // Start positions must be congruent to 1 modulo this value. 1 means any
  // start. Positions are 1-based rack unit numbers.
  std::int32_t alignment = 1;

  // Maximum number of candidate placements to report. Zero means "the lowest
  // one only". Bounded by Limits::kMaxCandidatesPerReport.
  std::uint32_t max_candidates = 1;

  FitContext context{};
};

// A request for an axis-aligned rectangle of floor inside one plane.
struct SC_API PlanarRectFitRequest final {
  SpaceNodeId node{};
  Millimeters width{};
  Millimeters depth{};

  // Both coordinates of a placement must be multiples of this value. Zero or
  // one means any position.
  Millimeters alignment{};

  std::uint32_t max_candidates = 1;
  FitContext context{};
};

// A request that only asks whether the total free area is at least `needed`.
// It makes no contiguity claim and never reports a placement.
struct SC_API PlanarAreaFitRequest final {
  SpaceNodeId node{};
  SquareMillimeters needed{};
  FitContext context{};
};

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

struct SC_API PlacementCandidate final {
  // The node whose envelope the placement is inside: the rack for a vertical
  // candidate, the plane owner for a planar one.
  SpaceNodeId node{};

  PlanarRect rect{};
  bool has_rect = false;

  RackUnitInterval units{};
  bool has_units = false;

  [[nodiscard]] friend bool operator==(const PlacementCandidate& a,
                                       const PlacementCandidate& b) noexcept {
    return a.node == b.node && a.rect == b.rect && a.has_rect == b.has_rect &&
           a.units == b.units && a.has_units == b.has_units;
  }
  [[nodiscard]] friend bool operator!=(const PlacementCandidate& a,
                                       const PlacementCandidate& b) noexcept {
    return !(a == b);
  }
};

struct SC_API FitAssessment final {
  FitVerdict verdict = FitVerdict::indeterminate;

  // The node the question was asked about.
  SpaceNodeId subject{};
  EntityGeneration subject_generation{};

  // Where the space is actually accounted, when the subject is a grouping
  // node. Equal to `subject` when the subject owns its own plane.
  SpaceNodeId attributed_plane{};

  // Candidates in deterministic order: ascending start position, then
  // ascending extent.
  std::vector<PlacementCandidate> candidates;

  // The ledger the verdict was computed from.
  AreaLedger area{};
  UnitLedger units{};

  // The largest free run for a rack-unit question, even when it is too small
  // for the request, and the total free area for a planar question. Reported so
  // an operator can see how close the fit was.
  std::optional<RackUnitInterval> largest_free_run{};

  ExplanationSet explanations{};

  // The revision and attempt this observation was taken at. A caller that acts
  // on the assessment must fence with this token; a later revision refuses the
  // action rather than merging it.
  RegistryRevision revision{};
  AttemptId attempt{};

  [[nodiscard]] bool grants_placement() const noexcept { return false; }

  [[nodiscard]] friend bool operator==(const FitAssessment& a,
                                       const FitAssessment& b) noexcept;
  [[nodiscard]] friend bool operator!=(const FitAssessment& a,
                                       const FitAssessment& b) noexcept {
    return !(a == b);
  }
};

// ---------------------------------------------------------------------------
// Capacity report
// ---------------------------------------------------------------------------

struct SC_API CapacityReport final {
  SpaceNodeId subject{};
  EntityGeneration subject_generation{};
  SpaceNodeId attributed_plane{};
  NodeLifecycle lifecycle = NodeLifecycle::planned;

  bool owns_plane = false;
  bool owns_rack_envelope = false;

  // The subject's own ledger.
  AreaLedger area{};
  UnitLedger units{};

  // The subject's own ledger followed by each descendant plane owner's ledger,
  // in canonical node order. Each plane appears exactly once.
  std::vector<NodeCapacity> planes{};

  ExplanationSet explanations{};

  RegistryRevision revision{};
  AttemptId attempt{};

  [[nodiscard]] friend bool operator==(const CapacityReport& a,
                                       const CapacityReport& b) noexcept;
  [[nodiscard]] friend bool operator!=(const CapacityReport& a,
                                       const CapacityReport& b) noexcept {
    return !(a == b);
  }
};

}  // namespace dccp::space_capacity
