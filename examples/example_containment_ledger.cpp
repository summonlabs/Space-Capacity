// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity example: build a contained facility and read its ledger.
//
// What this shows:
//   * a containment tree where a nested plane is a subdivision of the plane
//     above it, and why that is what keeps a rollup from double counting;
//   * the difference between a node's own ledger and the subtree rollup;
//   * that a grouping node reports `unknown` area rather than a zero.
//
// This example writes nothing. It runs entirely in memory.

#include <cstdint>
#include <cstdio>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

void show(const char* what, const AreaLedger& ledger) {
  std::printf("%-22s state=%s declared=%s usable=%s structural=%s claimed=%s available=%s\n",
              what, std::string(measure_state_name(ledger.state)).c_str(),
              to_text(ledger.declared).c_str(), to_text(ledger.usable).c_str(),
              to_text(ledger.structural).c_str(), to_text(ledger.claimed).c_str(),
              to_text(ledger.available).c_str());
}

}  // namespace

int main() {
  Result<SpaceCapacityRegistry> created =
      SpaceCapacityRegistry::create_in_memory(*StoreId::parse("example-facility"));
  if (!created) {
    std::fprintf(stderr, "cannot start: %s\n", created.error().to_string().c_str());
    return 1;
  }
  SpaceCapacityRegistry registry = std::move(created).value();

  const SpaceNodeId site = *SpaceNodeId::parse("site-1");
  const SpaceNodeId building = *SpaceNodeId::parse("bldg-1");
  const SpaceNodeId hall = *SpaceNodeId::parse("hall-1");
  const SpaceNodeId row = *SpaceNodeId::parse("row-1");
  const SpaceNodeId rack = *SpaceNodeId::parse("rack-1");

  auto add = [&](const SpaceNodeId& id, SpaceNodeKind kind, SpatialClass spatial,
                 const SpaceNodeId& parent, std::uint32_t depth, std::int64_t width,
                 std::int64_t depth_mm, std::int32_t height_u) {
    SpaceNode node;
    node.id = id;
    node.generation = EntityGeneration{1};
    node.kind = kind;
    node.spatial_class = spatial;
    node.lifecycle = NodeLifecycle::available;
    node.parent = parent;
    node.depth = depth;
    if (width > 0 && depth_mm > 0) {
      node.own_planar.declared_area = SquareMillimeters{width * depth_mm};
      node.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, width, depth_mm)});
    }
    if (height_u > 0) node.own_rack.height = RackUnits{height_u};
    if (!parent.empty() && width > 0 && depth_mm > 0) {
      // A node that declares a plane inside another plane is a subdivision of
      // it: the placement must match the declared area exactly.
      node.placement.has_base_rect = true;
      node.placement.base_rect = PlanarRect::make(0, 0, width, depth_mm);
    }
    CreateNodeRequest request;
    request.node = node;
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "cannot create %s: %s\n", id.str().c_str(),
                   outcome.error().to_string().c_str());
      std::exit(1);
    }
  };

  // Building 1 declares the whole 60 m x 40 m envelope.
  add(site, SpaceNodeKind::site, SpatialClass::outdoor, SpaceNodeId{}, 0, 0, 0, 0);
  add(building, SpaceNodeKind::building, SpatialClass::enclosed, site, 1, 60000, 40000, 0);
  // Hall 1 is a 30 m x 20 m subdivision of it.
  add(hall, SpaceNodeKind::hall, SpatialClass::floor, building, 2, 30000, 20000, 0);
  // A row declares no plane: it is a grouping node.
  add(row, SpaceNodeKind::row, SpatialClass::aisle, hall, 3, 0, 0, 0);
  // A rack declares a 42 U envelope and sits on the hall floor.
  add(rack, SpaceNodeKind::rack, SpatialClass::rack, row, 4, 0, 0, 42);
  {
    SetNodePlacementRequest request;
    request.node = rack;
    request.placement.has_base_rect = true;
    request.placement.base_rect = PlanarRect::make(1000, 1000, 600, 1200);
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "cannot place the rack: %s\n", outcome.error().to_string().c_str());
      return 1;
    }
  }

  // Occupy 6 rack units.
  {
    CreateClaimRequest request;
    request.claim.id = *OccupancyClaimId::parse("claim-1");
    request.claim.generation = EntityGeneration{1};
    request.claim.node = rack;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::asset;
    request.claim.scope.kind = FootprintScopeKind::rack_units;
    request.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 6)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "cannot claim: %s\n", outcome.error().to_string().c_str());
      return 1;
    }
  }

  const SnapshotPtr snapshot = registry.snapshot();

  std::puts("-- one node's own ledger, at its own level --");
  show("building-1 own", snapshot->capacity_of(building, FitContext{}).area);
  show("hall-1 own", snapshot->capacity_of(hall, FitContext{}).area);
  show("row-1 (grouping node)", snapshot->capacity_of(row, FitContext{}).area);
  std::printf("row-1 attributed plane is %s\n\n",
              snapshot->capacity_of(row, FitContext{}).attributed_plane.str().c_str());

  std::puts("-- subtree rollup: each plane is counted exactly once --");
  const Result<CapacityRollup> whole = snapshot->rollup(site, FitContext{});
  if (!whole) {
    std::fprintf(stderr, "rollup failed: %s\n", whole.error().to_string().c_str());
    return 1;
  }
  std::printf("nodes=%llu planes=%llu racks=%llu\n",
              static_cast<unsigned long long>(whole.value().node_count),
              static_cast<unsigned long long>(whole.value().plane_owner_count),
              static_cast<unsigned long long>(whole.value().rack_count));
  show("site-1 subtree", whole.value().area);
  std::printf("rack units: declared=%s occupied=%s available=%s free-runs=%u largest=%s\n",
              to_text(whole.value().units.declared).c_str(),
              to_text(whole.value().units.occupied).c_str(),
              to_text(whole.value().units.available).c_str(), whole.value().units.free_runs,
              to_text(whole.value().units.largest_free_run).c_str());
  std::puts("");
  std::puts("The building declares 2 400 000 000 mm2 and the hall inside it declares");
  std::puts("600 000 000 mm2. The rollup reports 2 400 000 000, not 3 000 000 000, because");
  std::puts("the hall plane is a subdivision of the building plane, and the building");
  std::puts("already counts the hall placement as consumed floor.");
  return 0;
}
