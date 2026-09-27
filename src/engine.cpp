// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the accounting engine.
//
// One pass over the model attributes every node placement, occupancy claim,
// reservation, exclusion, clearance and expansion zone to the plane or rack
// envelope it consumes. Each family is then measured by union, so the ledger
// cannot count the same square millimetre or the same rack unit twice, and the
// available figure is the declared envelope less the union of everything that
// blocks it. Overlap between two records is separately refused at commit time;
// it is never silently merged.

#include "engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "geometry.hpp"

namespace dccp::space_capacity::internal {
namespace {

SquareMillimeters square(std::int64_t value) { return SquareMillimeters{value}; }
RackUnits units(std::int32_t value) { return RackUnits{value}; }

// Adds the rectangles of a scope to a destination vector, when the scope is
// planar. A whole-node scope adds nothing here.
bool scope_is_planar(const FootprintScope& scope) {
  return scope.kind == FootprintScopeKind::planar;
}

bool scope_is_units(const FootprintScope& scope) {
  return scope.kind == FootprintScopeKind::rack_units;
}

void append_rects(const RectSet& source, std::vector<PlanarRect>& destination) {
  destination.insert(destination.end(), source.rects().begin(), source.rects().end());
}

// Measures the union of a rectangle list, reporting overflow through the
// shared flag.
SquareMillimeters union_of(const std::vector<PlanarRect>& rects, bool& overflowed) {
  const Checked<SquareMillimeters> measured = measure_union_area(rects);
  if (!measured) {
    overflowed = true;
    return SquareMillimeters{0};
  }
  return measured.value;
}

// A mutable accumulator that collects the raw rectangle and interval sets
// before the measures are taken.
struct PlaneRaw final {
  std::vector<PlanarRect> excluded;
  std::vector<PlanarRect> structural;
  std::vector<PlanarRect> subplane_rects;
  std::vector<PlanarRect> claimed;
  std::vector<PlanarRect> held;
  std::vector<PlanarRect> earmarked;
  std::vector<PlanarRect> pending;
  std::vector<PlanarRect> planned;
  std::vector<PlanarRect> clearance_enforced;
  std::vector<PlanarRect> clearance_reported;
  bool whole_node_claimed = false;
  bool whole_node_held = false;
  bool whole_node_earmarked = false;
  bool whole_node_excluded = false;
};

struct RackRaw final {
  std::vector<RackUnitInterval> excluded;
  std::vector<RackUnitInterval> occupied;
  std::vector<RackUnitInterval> pending;
  std::vector<RackUnitInterval> planned;
  bool whole_node_claimed = false;
  bool whole_node_held = false;
};

ModelMeasure measure_model_impl(const Snapshot& snapshot, const FitContext& context) {
  ModelMeasure model;

  // Establish every plane and rack envelope that the model declares. A node
  // that declares neither is a grouping node and owns no ledger.
  for (const SpaceNode& node : snapshot.nodes()) {
    if (node.own_planar.is_declared()) {
      PlaneMeasure measure;
      measure.plane = node.id;
      measure.usable = lifecycle_is_usable_now(node.lifecycle);
      measure.planned = lifecycle_is_planned(node.lifecycle);
      measure.declared = node.own_planar.declared_area;
      model.planes.emplace(node.id, std::move(measure));
    }
    if (node.own_rack.is_declared() && kind_may_declare_rack_envelope(node.kind)) {
      RackMeasure measure;
      measure.rack = node.id;
      measure.usable = lifecycle_is_usable_now(node.lifecycle);
      measure.planned = lifecycle_is_planned(node.lifecycle);
      measure.declared = node.own_rack.height;
      model.racks.emplace(node.id, std::move(measure));
    }
  }

  std::map<SpaceNodeId, PlaneRaw> plane_raw;
  std::map<SpaceNodeId, RackRaw> rack_raw;

  // ---------------------------------------------------------------- nodes
  for (const SpaceNode& node : snapshot.nodes()) {
    // A node that declares a plane of its own consumes the plane ABOVE it: a
    // nested plane is a SUBDIVISION of the plane above, so its area is removed
    // from the enclosing plane's own declared area rather than added to that
    // plane's consumption. That is what makes the sum of every plane's declared
    // area equal the area that physically exists, at every level, with nothing
    // counted twice. A node that declares no plane consumes the nearest
    // plane-owning ancestor, which is what attributes a rack standing on a hall
    // floor to the hall.
    SpaceNodeId plane = snapshot.plane_owner_of(node.id);
    if (plane == node.id && node.placement.has_base_rect) {
      plane = snapshot.plane_owner_of(node.parent);
    }
    if (!plane.empty() && plane != node.id && node.placement.has_base_rect) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (node.own_planar.is_declared()) {
          raw.subplane_rects.push_back(node.placement.base_rect);
        } else {
          raw.structural.push_back(node.placement.base_rect);
          if (lifecycle_is_planned(node.lifecycle)) {
            raw.planned.push_back(node.placement.base_rect);
          }
        }
      }
    }
    // The same rule for vertical envelopes: a node that declares its own rack
    // envelope consumes the envelope above it, and a band consumes its rack.
    SpaceNodeId rack = snapshot.rack_owner_of(node.id);
    if (rack == node.id && node.placement.has_u_span) {
      rack = snapshot.rack_owner_of(node.parent);
    }
    if (!rack.empty() && rack != node.id && node.placement.has_u_span) {
      const auto it = model.racks.find(rack);
      if (it != model.racks.end()) {
        RackRaw& raw = rack_raw[rack];
        raw.occupied.push_back(node.placement.u_span);
        if (lifecycle_is_planned(node.lifecycle)) {
          raw.planned.push_back(node.placement.u_span);
        }
      }
    }
  }

  // --------------------------------------------------------------- claims
  for (const OccupancyClaim& claim : snapshot.claims()) {
    const bool consuming = claim_consumes_for(claim, context);
    const bool pending_here = claim_state_is_pending(claim.state);
    if (!consuming && !pending_here) continue;

    const SpaceNodeId plane = snapshot.plane_owner_of(claim.node);
    if (!plane.empty()) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (claim.scope.is_whole_node()) {
          if (consuming) raw.whole_node_claimed = true;
        } else if (scope_is_planar(claim.scope)) {
          if (consuming) {
            append_rects(claim.scope.rects, raw.claimed);
          } else {
            append_rects(claim.scope.rects, raw.pending);
          }
        }
      }
    }
    const SpaceNodeId rack = snapshot.rack_owner_of(claim.node);
    if (!rack.empty()) {
      const auto it = model.racks.find(rack);
      if (it != model.racks.end()) {
        RackRaw& raw = rack_raw[rack];
        if (claim.scope.is_whole_node()) {
          if (consuming) raw.whole_node_claimed = true;
        } else if (scope_is_units(claim.scope)) {
          const std::vector<RackUnitInterval>& source = claim.scope.units.intervals();
          if (consuming) {
            raw.occupied.insert(raw.occupied.end(), source.begin(), source.end());
          } else {
            raw.pending.insert(raw.pending.end(), source.begin(), source.end());
          }
        }
      }
    }
  }

  // ---------------------------------------------------------- reservations
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(effective_reservation_state(reservation, context))) continue;
    const SpaceNodeId plane = snapshot.plane_owner_of(reservation.node);
    if (!plane.empty()) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (reservation.scope.is_whole_node()) {
          raw.whole_node_held = true;
        } else if (scope_is_planar(reservation.scope)) {
          append_rects(reservation.scope.rects, raw.held);
        }
      }
    }
    const SpaceNodeId rack = snapshot.rack_owner_of(reservation.node);
    if (!rack.empty()) {
      const auto it = model.racks.find(rack);
      if (it != model.racks.end()) {
        RackRaw& raw = rack_raw[rack];
        if (reservation.scope.is_whole_node()) {
          raw.whole_node_held = true;
        } else if (scope_is_units(reservation.scope)) {
          const std::vector<RackUnitInterval>& source = reservation.scope.units.intervals();
          raw.occupied.insert(raw.occupied.end(), source.begin(), source.end());
        }
      }
    }
  }

  // ------------------------------------------------------------ exclusions
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now()) continue;
    if (!exclusion_blocks_for(region, context)) continue;
    const SpaceNodeId plane = snapshot.plane_owner_of(region.node);
    if (!plane.empty()) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (region.scope.is_whole_node()) {
          raw.whole_node_excluded = true;
        } else if (scope_is_planar(region.scope)) {
          append_rects(region.scope.rects, raw.excluded);
        }
      }
    }
    const SpaceNodeId rack = snapshot.rack_owner_of(region.node);
    if (!rack.empty()) {
      const auto it = model.racks.find(rack);
      if (it != model.racks.end() && scope_is_units(region.scope)) {
        RackRaw& raw = rack_raw[rack];
        const std::vector<RackUnitInterval>& source = region.scope.units.intervals();
        raw.excluded.insert(raw.excluded.end(), source.begin(), source.end());
      }
    }
  }

  // ------------------------------------------------------------- clearances
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    const SpaceNodeId plane = snapshot.plane_owner_of(clearance.node);
    if (clearance.has_band && !plane.empty()) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (clearance.enforceable) {
          raw.clearance_enforced.push_back(clearance.band);
        } else {
          raw.clearance_reported.push_back(clearance.band);
        }
      }
    }
    if (clearance.has_units && clearance.enforceable) {
      const SpaceNodeId rack = snapshot.rack_owner_of(clearance.node);
      if (!rack.empty()) {
        const auto it = model.racks.find(rack);
        if (it != model.racks.end()) {
          RackRaw& raw = rack_raw[rack];
          const std::vector<RackUnitInterval>& source = clearance.required_free_units.intervals();
          raw.excluded.insert(raw.excluded.end(), source.begin(), source.end());
        }
      }
    }
  }

  // ----------------------------------------------------------- expansions
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    const SpaceNodeId plane = snapshot.plane_owner_of(zone.node);
    if (!plane.empty()) {
      const auto it = model.planes.find(plane);
      if (it != model.planes.end()) {
        PlaneRaw& raw = plane_raw[plane];
        if (zone.scope.is_whole_node()) {
          raw.whole_node_earmarked = true;
        } else if (scope_is_planar(zone.scope)) {
          append_rects(zone.scope.rects, raw.earmarked);
        }
      }
    }
    const SpaceNodeId rack = snapshot.rack_owner_of(zone.node);
    if (!rack.empty()) {
      const auto it = model.racks.find(rack);
      if (it != model.racks.end() && scope_is_units(zone.scope)) {
        RackRaw& raw = rack_raw[rack];
        const std::vector<RackUnitInterval>& source = zone.scope.units.intervals();
        raw.occupied.insert(raw.occupied.end(), source.begin(), source.end());
      }
    }
  }

  // ------------------------------------------------------------- reduction
  for (auto& [id, measure] : model.planes) {
    const PlaneRaw& raw = plane_raw[id];
    bool overflowed = false;

    measure.clearance_bands = raw.clearance_enforced;
    measure.reported_clearances = raw.clearance_reported;
    measure.exclusion_rects = raw.excluded;

    // The area a plane offers at its own level is what it declares less the
    // area its nested sub-planes already account for. A nested plane is a
    // subdivision, so its area belongs to exactly one ledger and never to two.
    const SquareMillimeters own_declared = measure.declared;
    measure.subplanes = union_of(raw.subplane_rects, overflowed);
    if (measure.subplanes.value() > own_declared.value()) {
      measure.subplanes = own_declared;
    }
    measure.declared = square(saturating_sub(own_declared.value(), measure.subplanes.value()));

    if (!measure.usable && !measure.planned) {
      // Terminal: the envelope is still declared, and all of it is out of
      // service. Nothing is available and nothing is over-committed.
      measure.excluded = measure.declared;
      measure.usable_area = SquareMillimeters{0};
      measure.available = SquareMillimeters{0};
      measure.planned_area = SquareMillimeters{0};
      measure.overflowed = overflowed;
      continue;
    }

    std::vector<PlanarRect> excluded_set = raw.excluded;
    excluded_set.insert(excluded_set.end(), raw.clearance_enforced.begin(),
                        raw.clearance_enforced.end());
    if (raw.whole_node_excluded) {
      measure.excluded = measure.declared;
    } else {
      measure.excluded = union_of(excluded_set, overflowed);
    }
    if (measure.excluded.value() > measure.declared.value()) {
      measure.excluded = measure.declared;
    }

    measure.structural = union_of(raw.structural, overflowed);
    measure.claimed = union_of(raw.claimed, overflowed);
    measure.held = union_of(raw.held, overflowed);
    measure.earmarked = union_of(raw.earmarked, overflowed);
    measure.pending = union_of(raw.pending, overflowed);
    measure.planned_area = union_of(raw.planned, overflowed);
    if (raw.whole_node_claimed) measure.claimed = measure.declared;
    if (raw.whole_node_held) measure.held = measure.declared;
    if (raw.whole_node_earmarked) measure.earmarked = measure.declared;

    if (measure.planned) {
      measure.usable_area = SquareMillimeters{0};
      measure.available = SquareMillimeters{0};
      measure.planned_area = measure.declared;
      measure.overflowed = overflowed;
      continue;
    }

    measure.usable_area = square(saturating_sub(measure.declared.value(), measure.excluded.value()));

    // Obstacles are everything a placement must avoid. The sub-plane rectangles
    // are obstacles even though they are not blockers: their area was already
    // removed from the declared area above, so a placement inside one would be
    // counted twice.
    std::vector<PlanarRect> blockers = raw.excluded;
    blockers.insert(blockers.end(), raw.clearance_enforced.begin(), raw.clearance_enforced.end());
    blockers.insert(blockers.end(), raw.structural.begin(), raw.structural.end());
    blockers.insert(blockers.end(), raw.claimed.begin(), raw.claimed.end());
    blockers.insert(blockers.end(), raw.held.begin(), raw.held.end());
    blockers.insert(blockers.end(), raw.earmarked.begin(), raw.earmarked.end());
    if (context.count_pending) {
      blockers.insert(blockers.end(), raw.pending.begin(), raw.pending.end());
    }
    measure.obstacles = blockers;
    measure.obstacles.insert(measure.obstacles.end(), raw.subplane_rects.begin(),
                             raw.subplane_rects.end());

    SquareMillimeters blocked{};
    if (raw.whole_node_excluded || raw.whole_node_claimed || raw.whole_node_held ||
        raw.whole_node_earmarked) {
      blocked = measure.declared;
    } else {
      blocked = union_of(blockers, overflowed);
    }
    measure.over_committed = blocked.value() > measure.declared.value();
    measure.available =
        square(saturating_sub(measure.declared.value(), std::min(blocked.value(),
                                                                  measure.declared.value())));
    measure.overflowed = overflowed;
  }

  for (auto& [id, measure] : model.racks) {
    const RackRaw& raw = rack_raw[id];
    bool overflowed = false;

    std::vector<RackUnitInterval> blocked_ranges = raw.excluded;
    if (measure.usable || measure.planned) {
      blocked_ranges.insert(blocked_ranges.end(), raw.occupied.begin(), raw.occupied.end());
      if (context.count_pending) {
        blocked_ranges.insert(blocked_ranges.end(), raw.pending.begin(), raw.pending.end());
      }
    }
    if (raw.whole_node_claimed || raw.whole_node_held) {
      blocked_ranges.push_back(measure.declared.is_zero()
                                   ? RackUnitInterval{}
                                   : RackUnitInterval::of_count(1, measure.declared.value()));
    }
    if (raw.whole_node_claimed) {
      measure.occupied = measure.declared;
    } else {
      Result<IntervalSet> occupied_set = IntervalSet::build(raw.occupied);
      if (!occupied_set) {
        overflowed = true;
      } else {
        const Checked<RackUnits> occupied_total = occupied_set.value().total();
        if (!occupied_total) {
          overflowed = true;
        } else if (occupied_total.value.value() > Limits::kMaxRackUnits) {
          overflowed = true;
        } else {
          measure.occupied = occupied_total.value;
        }
      }
    }
    if (raw.whole_node_held && measure.occupied.value() < measure.declared.value()) {
      measure.occupied = measure.declared;
    }

    Result<IntervalSet> excluded_set = IntervalSet::build(raw.excluded);
    if (!excluded_set) {
      overflowed = true;
    } else {
      const Checked<RackUnits> excluded_total = excluded_set.value().total();
      if (!excluded_total) {
        overflowed = true;
      } else {
        measure.excluded = excluded_total.value;
        if (measure.excluded.value() > measure.declared.value()) {
          measure.excluded = measure.declared;
        }
      }
    }

    Result<IntervalSet> pending_set = IntervalSet::build(raw.pending);
    if (!pending_set) {
      overflowed = true;
    } else {
      const Checked<RackUnits> pending_total = pending_set.value().total();
      if (!pending_total) {
        overflowed = true;
      } else {
        measure.pending = pending_total.value;
      }
    }

    Result<IntervalSet> planned_set = IntervalSet::build(raw.planned);
    if (!planned_set) {
      overflowed = true;
    } else {
      const Checked<RackUnits> planned_total = planned_set.value().total();
      if (!planned_total) {
        overflowed = true;
      } else {
        measure.planned_units = planned_total.value;
      }
    }

    if (!measure.usable && !measure.planned) {
      measure.excluded = measure.declared;
      measure.usable_units = units(0);
      measure.available = units(0);
      measure.planned_units = units(0);
      measure.free_runs = 0;
      measure.largest_free_run = units(0);
      measure.fragmentation_ppm = 0;
      measure.overflowed = overflowed;
      continue;
    }

    if (measure.planned) {
      measure.usable_units = units(0);
      measure.available = units(0);
      measure.planned_units = measure.declared;
      measure.free_runs = 0;
      measure.largest_free_run = units(0);
      measure.fragmentation_ppm = 0;
      measure.overflowed = overflowed;
      continue;
    }

    measure.usable_units =
        units(saturating_sub(measure.declared.value(), measure.excluded.value()));

    Result<IntervalSet> blocked_set = IntervalSet::build(blocked_ranges);
    if (!blocked_set) {
      overflowed = true;
      measure.available = units(0);
    } else {
      measure.blocked = blocked_set.value();
      const Checked<RackUnits> blocked_total = measure.blocked.total();
      if (!blocked_total) {
        overflowed = true;
        measure.available = units(0);
      } else {
        measure.over_committed = blocked_total.value.value() > measure.declared.value();
        measure.available =
            units(saturating_sub(measure.declared.value(), blocked_total.value.value()));
      }
      Result<IntervalSet::FreeRunStats> stats =
          IntervalSet::free_run_stats(measure.blocked, measure.declared.value());
      if (!stats) {
        overflowed = true;
      } else {
        measure.free_runs = stats.value().run_count;
        measure.largest_free_run = units(stats.value().largest);
      }
      Result<std::uint32_t> ppm =
          IntervalSet::fragmentation_ppm(measure.blocked, measure.declared.value());
      if (!ppm) {
        overflowed = true;
      } else {
        measure.fragmentation_ppm = ppm.value();
      }
    }
    measure.overflowed = overflowed;
  }

  return model;
}

// A ledger is `known` when every measure that fed it was formed from declared
// input, and `unknown` when checked arithmetic refused a total: an unknown
// number is never reported as a zero.
MeasureState state_for(bool overflowed) {
  return overflowed ? MeasureState::unknown : MeasureState::known;
}

}  // namespace

ReservationState effective_reservation_state(const FootprintReservation& reservation,
                                             const FitContext& context) {
  if (context.now.has_value()) {
    return reservation_state_at(reservation, *context.now);
  }
  return reservation.state;
}

bool claim_consumes_for(const OccupancyClaim& claim, const FitContext& context) {
  if (claim_state_consumes(claim.state)) return true;
  if (context.count_pending && claim_state_is_pending(claim.state)) return true;
  return false;
}

bool exclusion_blocks_for(const ExclusionRegion& region, const FitContext& context) {
  if (region.blocks.empty()) return false;
  if (context.occupant == OccupantKind::unknown) return true;
  return region.blocks.contains(context.occupant);
}

void collect_scope_rects(const FootprintScope& scope, std::vector<PlanarRect>& out) {
  if (!scope_is_planar(scope)) return;
  append_rects(scope.rects, out);
}

const IntervalSet& scope_units(const FootprintScope& scope) { return scope.units; }

ModelMeasure measure_model(const Snapshot& snapshot, const FitContext& context) {
  return measure_model_impl(snapshot, context);
}

PlaneMeasure measure_plane(const Snapshot& snapshot, const SpaceNodeId& plane,
                           const FitContext& context) {
  const ModelMeasure model = measure_model(snapshot, context);
  const auto it = model.planes.find(plane);
  if (it == model.planes.end()) return PlaneMeasure{};
  return it->second;
}

RackMeasure measure_rack(const Snapshot& snapshot, const SpaceNodeId& rack,
                         const FitContext& context) {
  const ModelMeasure model = measure_model(snapshot, context);
  const auto it = model.racks.find(rack);
  if (it == model.racks.end()) return RackMeasure{};
  return it->second;
}

// ---------------------------------------------------------------------------
// Ledger construction
// ---------------------------------------------------------------------------

AreaLedger ledger_from(const PlaneMeasure& measure) {
  AreaLedger ledger;
  ledger.state = state_for(measure.overflowed);
  ledger.declared = measure.declared;
  ledger.excluded = measure.excluded;
  ledger.usable = measure.usable_area;
  ledger.structural = measure.structural;
  ledger.claimed = measure.claimed;
  ledger.held = measure.held;
  ledger.earmarked = measure.earmarked;
  ledger.available = measure.available;
  ledger.pending = measure.pending;
  ledger.planned = measure.planned_area;
  ledger.over_committed = measure.over_committed;
  return ledger;
}

UnitLedger ledger_from(const RackMeasure& measure) {
  UnitLedger ledger;
  ledger.state = state_for(measure.overflowed);
  ledger.declared = measure.declared;
  ledger.excluded = measure.excluded;
  ledger.usable = measure.usable_units;
  ledger.occupied = measure.occupied;
  ledger.available = measure.available;
  ledger.pending = measure.pending;
  ledger.planned = measure.planned_units;
  ledger.free_runs = measure.free_runs;
  ledger.largest_free_run = measure.largest_free_run;
  ledger.fragmentation_ppm = measure.fragmentation_ppm;
  ledger.over_committed = measure.over_committed;
  return ledger;
}

}  // namespace dccp::space_capacity::internal
