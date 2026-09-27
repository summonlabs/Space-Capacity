// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal accounting engine. Not installed, not part of the public API.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/query.hpp"
#include "dccp/space_capacity/snapshot.hpp"

namespace dccp::space_capacity::internal {

// The obstacles and measures attributed to one plane.
//
// Every family is kept separate so a report can say where each square
// millimetre went, and every measure is taken by union, so the ledger cannot
// count the same area twice even if the input overlaps.
struct PlaneMeasure final {
  SpaceNodeId plane{};
  bool usable = false;
  bool planned = false;
  bool overflowed = false;

  SquareMillimeters declared{};
  SquareMillimeters excluded{};
  SquareMillimeters subplanes{};
  SquareMillimeters usable_area{};
  SquareMillimeters structural{};
  SquareMillimeters claimed{};
  SquareMillimeters held{};
  SquareMillimeters earmarked{};
  SquareMillimeters pending{};
  SquareMillimeters planned_area{};
  SquareMillimeters available{};
  bool over_committed = false;
  bool whole_node_claimed = false;

  // Rectangles that block a placement. Used by the fit search; the measures
  // above are computed from the same sets.
  std::vector<PlanarRect> obstacles{};
  // Enforceable clearance bands, already included in the obstacles.
  std::vector<PlanarRect> clearance_bands{};
  // Non-enforceable clearance bands: reported, subtracted from nothing.
  std::vector<PlanarRect> reported_clearances{};
  // Exclusion rectangles that were applied.
  std::vector<PlanarRect> exclusion_rects{};
};

// The obstacles and measures attributed to one rack envelope.
struct RackMeasure final {
  SpaceNodeId rack{};
  bool usable = false;
  bool planned = false;
  bool overflowed = false;

  RackUnits declared{};
  RackUnits excluded{};
  RackUnits usable_units{};
  RackUnits occupied{};
  RackUnits available{};
  RackUnits pending{};
  RackUnits planned_units{};

  std::uint32_t free_runs = 0;
  RackUnits largest_free_run{};
  std::uint32_t fragmentation_ppm = 0;
  bool over_committed = false;
  bool whole_node_claimed = false;

  IntervalSet blocked{};
};

struct ModelMeasure final {
  std::map<SpaceNodeId, PlaneMeasure> planes;
  std::map<SpaceNodeId, RackMeasure> racks;
};

// One pass over the model. Every plane and rack envelope the model declares
// gets a measure; a grouping node gets none.
ModelMeasure measure_model(const Snapshot& snapshot, const FitContext& context);

// Builds the ledger of one plane. A plane the model does not declare yields a
// measure with `declared` zero and both flags false.
PlaneMeasure measure_plane(const Snapshot& snapshot, const SpaceNodeId& plane,
                           const FitContext& context);

// Builds the measure of one rack envelope.
RackMeasure measure_rack(const Snapshot& snapshot, const SpaceNodeId& rack,
                         const FitContext& context);

// Converts a measure into the public ledger types.
AreaLedger ledger_from(const PlaneMeasure& measure);
UnitLedger ledger_from(const RackMeasure& measure);

// Whether a reservation holds at the reading carried by the context.
ReservationState effective_reservation_state(const FootprintReservation& reservation,
                                             const FitContext& context);

// Whether a claim consumes for the given context. A pending claim consumes
// only when the context asks for it.
bool claim_consumes_for(const OccupancyClaim& claim, const FitContext& context);

// Whether an exclusion blocks an occupant of the context's kind.
bool exclusion_blocks_for(const ExclusionRegion& region, const FitContext& context);

// The planar scope of a record, appended to `out`. A whole-node scope appends
// nothing: it covers the declared area rather than an explicit rectangle.
void collect_scope_rects(const FootprintScope& scope, std::vector<PlanarRect>& out);

// The unit scope of a record.
const IntervalSet& scope_units(const FootprintScope& scope);

}  // namespace dccp::space_capacity::internal
