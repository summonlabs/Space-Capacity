// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - ledger value semantics and measure states.
//
// Every ledger type is a value: two ledgers are equal when every measure in
// them is equal, including the measure state and the over-committed flag. That
// makes a ledger safe to compare, to store in a test expectation, and to diff.

#include "dccp/space_capacity/capacity.hpp"

#include <string_view>

#include "dccp/space_capacity/query.hpp"

namespace dccp::space_capacity {

std::string_view measure_state_name(MeasureState value) noexcept {
  switch (value) {
    case MeasureState::known:
      return "known";
    case MeasureState::unknown:
      return "unknown";
    case MeasureState::unavailable:
      return "unavailable";
  }
  return "unknown";
}

bool parse_measure_state(std::string_view text, MeasureState& out) noexcept {
  if (text == "known") {
    out = MeasureState::known;
    return true;
  }
  if (text == "unknown") {
    out = MeasureState::unknown;
    return true;
  }
  if (text == "unavailable") {
    out = MeasureState::unavailable;
    return true;
  }
  return false;
}

bool operator==(const AreaLedger& a, const AreaLedger& b) noexcept {
  return a.state == b.state && a.declared == b.declared && a.excluded == b.excluded &&
         a.usable == b.usable && a.structural == b.structural && a.claimed == b.claimed &&
         a.held == b.held && a.earmarked == b.earmarked && a.available == b.available &&
         a.pending == b.pending && a.planned == b.planned &&
         a.over_committed == b.over_committed;
}

bool operator==(const UnitLedger& a, const UnitLedger& b) noexcept {
  return a.state == b.state && a.declared == b.declared && a.excluded == b.excluded &&
         a.usable == b.usable && a.occupied == b.occupied && a.available == b.available &&
         a.pending == b.pending && a.planned == b.planned && a.free_runs == b.free_runs &&
         a.largest_free_run == b.largest_free_run &&
         a.fragmentation_ppm == b.fragmentation_ppm && a.over_committed == b.over_committed;
}

bool operator==(const NodeCapacity& a, const NodeCapacity& b) noexcept {
  return a.node == b.node && a.generation == b.generation && a.lifecycle == b.lifecycle &&
         a.owns_plane == b.owns_plane && a.owns_rack_envelope == b.owns_rack_envelope &&
         a.attributed_plane == b.attributed_plane && a.area == b.area && a.units == b.units;
}

bool operator==(const CapacityRollup& a, const CapacityRollup& b) noexcept {
  return a.root == b.root && a.root_generation == b.root_generation &&
         a.revision == b.revision && a.node_count == b.node_count &&
         a.plane_owner_count == b.plane_owner_count && a.rack_count == b.rack_count &&
         a.claim_count == b.claim_count && a.reservation_count == b.reservation_count &&
         a.exclusion_count == b.exclusion_count && a.clearance_count == b.clearance_count &&
         a.expansion_zone_count == b.expansion_zone_count && a.area == b.area &&
         a.units == b.units &&
         a.contains_undeclared_envelopes == b.contains_undeclared_envelopes &&
         a.contains_inactive_nodes == b.contains_inactive_nodes &&
         a.over_committed == b.over_committed;
}

bool operator==(const CapacityReport& a, const CapacityReport& b) noexcept {
  return a.subject == b.subject && a.subject_generation == b.subject_generation &&
         a.attributed_plane == b.attributed_plane && a.lifecycle == b.lifecycle &&
         a.owns_plane == b.owns_plane && a.owns_rack_envelope == b.owns_rack_envelope &&
         a.area == b.area && a.units == b.units && a.planes == b.planes &&
         a.revision == b.revision && a.attempt == b.attempt;
}

bool operator==(const FitAssessment& a, const FitAssessment& b) noexcept {
  return a.verdict == b.verdict && a.subject == b.subject &&
         a.subject_generation == b.subject_generation &&
         a.attributed_plane == b.attributed_plane && a.candidates == b.candidates &&
         a.area == b.area && a.units == b.units &&
         a.largest_free_run == b.largest_free_run && a.revision == b.revision &&
         a.attempt == b.attempt;
}

}  // namespace dccp::space_capacity
