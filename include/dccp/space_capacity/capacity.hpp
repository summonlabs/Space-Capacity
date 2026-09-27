// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the capacity ledger, fragment statistics and rollups.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ---------------------------------------------------------------------------
// How the numbers are formed
// ---------------------------------------------------------------------------
//
// For one plane-owning node P the ledger is:
//
//   declared    what P states it built                         (P.own_planar)
//   excluded    the measure of the union of blocking exclusions, of enforceable
//               clearance bands, and of the parts of the plane that a node
//               lifecycle has taken out of service
//   usable      declared - excluded, saturated at zero
//   structural  the measure of the union of the placements of nodes attributed
//               to P
//   claimed     the measure of the union of consuming occupancy claims on P
//   held        the measure of the union of reservations in force on P
//   earmarked   the measure of the union of expansion zones that earmark space
//   available   usable less the union of structural, claimed, held, earmarked
//   pending     the measure of the union of planned and submitted claims; it is
//               reported and subtracted from nothing
//   planned     the measure of the envelopes of planned nodes attributed to P,
//               plus the measure of planned expansion zones
//
// Every set is measured by UNION, never by sum. Two exclusion rectangles that
// overlap are counted once; two claims that overlap are counted once and are
// separately refused as a conflict. Union measure is why the ledger cannot
// double count, and the union-area kernel is proved by a property test against
// a brute-force reference model over an integer grid.
//
// A rack's vertical envelope is measured the same way in whole rack units,
// where union is interval normalization and is exact by construction.
//
// ---------------------------------------------------------------------------
// Known, unknown and unavailable
// ---------------------------------------------------------------------------
//
// A measure is `known` when its inputs are declared, `unknown` when the inputs
// were never declared, and `unavailable` when the inputs exist but could not
// be read. Only `known` measures carry a number. A measure that is unknown or
// unavailable reports zero AND a state, and no caller may read the zero as a
// measurement without checking the state.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {

enum class MeasureState : std::uint8_t {
  known = 1,
  unknown = 2,
  unavailable = 3,
};

SC_API std::string_view measure_state_name(MeasureState value) noexcept;
SC_API bool parse_measure_state(std::string_view text, MeasureState& out) noexcept;

// ---------------------------------------------------------------------------
// Union measure
// ---------------------------------------------------------------------------

// Exact measure of the union of a set of rectangles, in square millimetres.
// Overlaps are counted once. Implemented as a sweep over x events with an
// interval multiset on y, so the cost is O(n log n) plus the cost of the
// overlaps that actually exist. Overflow of the running total is reported.
SC_API Checked<SquareMillimeters> rect_union_area(const std::vector<PlanarRect>& rects) noexcept;
SC_API Checked<SquareMillimeters> rect_set_union_area(const RectSet& rects) noexcept;

// Finds the first pair of rectangles in canonical order that overlap, if any,
// and reports their indices. Used by conflict detection and by the audit pass.
SC_API bool find_overlapping_rect_pair(const std::vector<PlanarRect>& rects, std::size_t& first,
                                       std::size_t& second) noexcept;

// ---------------------------------------------------------------------------
// Ledgers
// ---------------------------------------------------------------------------

struct SC_API AreaLedger final {
  MeasureState state = MeasureState::known;

  SquareMillimeters declared{};
  SquareMillimeters excluded{};
  SquareMillimeters usable{};
  SquareMillimeters structural{};
  SquareMillimeters claimed{};
  SquareMillimeters held{};
  SquareMillimeters earmarked{};
  SquareMillimeters available{};
  SquareMillimeters pending{};
  SquareMillimeters planned{};

  // True when the space used or set aside exceeds the space that exists. The
  // ledger still reports saturated values; this flag is what tells a caller
  // the model is over-subscribed rather than merely full.
  bool over_committed = false;

  [[nodiscard]] friend bool operator==(const AreaLedger& a, const AreaLedger& b) noexcept;
  [[nodiscard]] friend bool operator!=(const AreaLedger& a, const AreaLedger& b) noexcept {
    return !(a == b);
  }
};

struct SC_API UnitLedger final {
  MeasureState state = MeasureState::known;

  RackUnits declared{};
  RackUnits excluded{};
  RackUnits usable{};
  RackUnits occupied{};
  RackUnits available{};
  RackUnits pending{};
  RackUnits planned{};

  // Fragmentation of the free rack units, computed exactly:
  //   free_runs          number of maximal free runs in the usable envelope
  //   largest_free_run   the largest such run, zero when there is no free unit
  //   fragmentation_ppm  (available - largest_free_run) * 1'000'000 / available
  //                      zero when available is zero or when the free space is
  //                      a single run. Integer division; the remainder is
  //                      discarded rather than rounded up.
  std::uint32_t free_runs = 0;
  RackUnits largest_free_run{};
  std::uint32_t fragmentation_ppm = 0;

  bool over_committed = false;

  [[nodiscard]] friend bool operator==(const UnitLedger& a, const UnitLedger& b) noexcept;
  [[nodiscard]] friend bool operator!=(const UnitLedger& a, const UnitLedger& b) noexcept {
    return !(a == b);
  }
};

// The ledger of one node at its own level.
struct SC_API NodeCapacity final {
  SpaceNodeId node{};
  EntityGeneration generation{};
  NodeLifecycle lifecycle = NodeLifecycle::planned;

  // True when this node declares the plane its own placements and claims are
  // measured in.
  bool owns_plane = false;
  // True when this node declares a vertical rack envelope.
  bool owns_rack_envelope = false;

  // The node whose plane this node's area is attributed to. Empty when there is
  // none. Equal to `node` when this node owns its plane.
  SpaceNodeId attributed_plane{};

  AreaLedger area{};
  UnitLedger units{};

  [[nodiscard]] friend bool operator==(const NodeCapacity& a, const NodeCapacity& b) noexcept;
  [[nodiscard]] friend bool operator!=(const NodeCapacity& a, const NodeCapacity& b) noexcept {
    return !(a == b);
  }
};

// ---------------------------------------------------------------------------
// Rollup
// ---------------------------------------------------------------------------

// Totals over a subtree. Each plane contributes exactly once: `declared_area`
// sums every plane owner's own-level declared area, and a plane's declared area
// already EXCLUDES the area of the sub-planes it contains, so a nested plane is
// never counted twice. Every measure that describes free or consumed space is
// likewise a sum over the plane owners in the subtree, and those planes are
// pairwise disjoint by construction.
struct SC_API CapacityRollup final {
  SpaceNodeId root{};
  EntityGeneration root_generation{};
  RegistryRevision revision{};

  std::uint64_t node_count = 0;
  std::uint64_t plane_owner_count = 0;
  std::uint64_t rack_count = 0;
  std::uint64_t claim_count = 0;
  std::uint64_t reservation_count = 0;
  std::uint64_t exclusion_count = 0;
  std::uint64_t clearance_count = 0;
  std::uint64_t expansion_zone_count = 0;

  AreaLedger area{};
  UnitLedger units{};

  // Set when the subtree contains a node whose envelope is not declared, so a
  // zero in the wrong place cannot be read as a measurement.
  bool contains_undeclared_envelopes = false;
  // Set when the subtree contains a node that is retired, replaced or
  // decommissioning. Such a node contributes nothing usable, and every report
  // that includes one carries this flag and an explanation.
  bool contains_inactive_nodes = false;
  bool over_committed = false;

  [[nodiscard]] friend bool operator==(const CapacityRollup& a,
                                       const CapacityRollup& b) noexcept;
  [[nodiscard]] friend bool operator!=(const CapacityRollup& a,
                                       const CapacityRollup& b) noexcept {
    return !(a == b);
  }
};

}  // namespace dccp::space_capacity
