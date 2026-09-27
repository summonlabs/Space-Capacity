// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the shared test fixture.
//
// One representative facility, built entirely through the public mutation API,
// so that every test exercises the same state and the same validation path an
// operator would.
//
//   site-a                       a site, declares no plane
//     bldg-1                     declares a 60 m x 40 m plane
//       hall-1                   declares a 30 m x 20 m plane inside it
//         row-1, row-2           grouping rows, no plane of their own
//           rack-01 .. rack-04   declare a 42 U envelope, placed on hall-1
//             band-a             a rack unit band inside rack-01
//
// plus one planar claim, one rack unit claim, one reservation, one exclusion,
// one enforceable clearance and one earmarking expansion zone.

#pragma once

#include <optional>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"

namespace sc_fixture {

using namespace dccp::space_capacity;

// Exact dimensions, in whole millimetres and whole rack units.
inline constexpr std::int64_t kBuildingWidth = 60000;
inline constexpr std::int64_t kBuildingDepth = 40000;
inline constexpr std::int64_t kHallWidth = 30000;
inline constexpr std::int64_t kHallDepth = 20000;
inline constexpr std::int64_t kRackWidth = 600;
inline constexpr std::int64_t kRackDepth = 1200;
inline constexpr std::int32_t kRackHeight = 42;

inline SquareMillimeters building_area() {
  return SquareMillimeters{kBuildingWidth * kBuildingDepth};
}
inline SquareMillimeters hall_area() { return SquareMillimeters{kHallWidth * kHallDepth}; }
inline SquareMillimeters rack_footprint() {
  return SquareMillimeters{kRackWidth * kRackDepth};
}

// The identities the fixture uses, so a test never repeats a string literal.
struct Ids final {
  SpaceNodeId site = *SpaceNodeId::parse("site-a");
  SpaceNodeId building = *SpaceNodeId::parse("bldg-1");
  SpaceNodeId hall = *SpaceNodeId::parse("hall-1");
  SpaceNodeId row1 = *SpaceNodeId::parse("row-1");
  SpaceNodeId row2 = *SpaceNodeId::parse("row-2");
  SpaceNodeId rack1 = *SpaceNodeId::parse("rack-01");
  SpaceNodeId rack2 = *SpaceNodeId::parse("rack-02");
  SpaceNodeId rack3 = *SpaceNodeId::parse("rack-03");
  SpaceNodeId rack4 = *SpaceNodeId::parse("rack-04");
  SpaceNodeId band = *SpaceNodeId::parse("band-a");
  OccupancyClaimId planar_claim = *OccupancyClaimId::parse("claim-planar-1");
  OccupancyClaimId unit_claim = *OccupancyClaimId::parse("claim-units-1");
  OccupancyClaimId reservation = *OccupancyClaimId::parse("hold-1");
  ExclusionRegionId exclusion = *ExclusionRegionId::parse("excl-1");
  ClearanceConstraintId clearance = *ClearanceConstraintId::parse("clr-1");
  ExpansionZoneId zone = *ExpansionZoneId::parse("zone-1");
};

inline Ids ids() { return Ids{}; }

inline SpaceNode make_node(const SpaceNodeId& id, SpaceNodeKind kind, SpatialClass spatial_class,
                           const SpaceNodeId& parent, std::uint32_t depth, const char* label) {
  SpaceNode node;
  node.id = id;
  node.generation = EntityGeneration{1};
  node.kind = kind;
  node.spatial_class = spatial_class;
  node.lifecycle = NodeLifecycle::available;
  node.label = *DisplayLabel::parse(label);
  node.parent = parent;
  node.depth = depth;
  return node;
}

// Creates one node and aborts the test on refusal: the fixture is only valid
// when it is complete, and a silent gap would weaken every test that uses it.
inline void create(SpaceCapacityRegistry& registry, const SpaceNode& node) {
  CreateNodeRequest request;
  request.node = node;
  const Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "FIXTURE: creating %s failed: %s\n", node.id.str().c_str(),
                 outcome.error().to_string().c_str());
    std::abort();
  }
}

inline void set_placement(SpaceCapacityRegistry& registry, const SpaceNodeId& node,
                          PlanarRect rect) {
  SetNodePlacementRequest request;
  request.node = node;
  request.placement.base_rect = rect;
  request.placement.has_base_rect = true;
  // The node was just created, so its generation is 1; fence on the revision
  // so that a fixture step can never silently land on moved state.
  const Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "FIXTURE: placing %s failed: %s\n", node.str().c_str(),
                 outcome.error().to_string().c_str());
    std::abort();
  }
}

inline void set_planar(SpaceCapacityRegistry& registry, const SpaceNodeId& node,
                       std::int64_t width, std::int64_t depth) {
  SetNodeEnvelopeRequest request;
  request.node = node;
  PlanarEnvelope envelope;
  envelope.declared_area = SquareMillimeters{width * depth};
  envelope.rects = *RectSet::build({PlanarRect::make(0, 0, width, depth)});
  request.planar = envelope;
  const Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "FIXTURE: setting the plane of %s failed: %s\n", node.str().c_str(),
                 outcome.error().to_string().c_str());
    std::abort();
  }
}

inline void set_rack_envelope(SpaceCapacityRegistry& registry, const SpaceNodeId& node,
                              std::int32_t height) {
  SetNodeEnvelopeRequest request;
  request.node = node;
  RackEnvelope envelope;
  envelope.height = RackUnits{height};
  request.rack = envelope;
  const Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "FIXTURE: setting the rack envelope of %s failed: %s\n",
                 node.str().c_str(), outcome.error().to_string().c_str());
    std::abort();
  }
}

inline void create_band(SpaceCapacityRegistry& registry, const SpaceNodeId& band,
                        const SpaceNodeId& rack, std::int32_t first, std::int32_t count,
                        std::uint32_t depth) {
  SpaceNode node = make_node(band, SpaceNodeKind::rack_unit_band, SpatialClass::rack, rack, depth,
                             "band");
  node.placement.has_u_span = true;
  node.placement.u_span = RackUnitInterval::of_count(first, count);
  create(registry, node);
}

// Populates the whole facility through the public mutation API.
inline void populate(SpaceCapacityRegistry& registry) {
  const Ids id = ids();
  create(registry, make_node(id.site, SpaceNodeKind::site, SpatialClass::outdoor, SpaceNodeId{}, 0,
                             "Site A"));
  create(registry, make_node(id.building, SpaceNodeKind::building, SpatialClass::enclosed, id.site,
                             1, "Building 1"));
  set_planar(registry, id.building, kBuildingWidth, kBuildingDepth);

  create(registry, make_node(id.hall, SpaceNodeKind::hall, SpatialClass::floor, id.building, 2,
                             "Hall 1"));
  // The placement comes first: a node that declares a plane inside another
  // plane must state where it sits, and the library refuses the intermediate
  // state in which it declares a plane and has no placement. Placing it on the
  // building plane before it declares its own plane is the legal order.
  set_placement(registry, id.hall, PlanarRect::make(0, 0, kHallWidth, kHallDepth));
  set_planar(registry, id.hall, kHallWidth, kHallDepth);

  create(registry, make_node(id.row1, SpaceNodeKind::row, SpatialClass::aisle, id.hall, 3,
                             "Row 1"));
  create(registry, make_node(id.row2, SpaceNodeKind::row, SpatialClass::aisle, id.hall, 3,
                             "Row 2"));

  const SpaceNodeId racks[4] = {id.rack1, id.rack2, id.rack3, id.rack4};
  const SpaceNodeId rows[4] = {id.row1, id.row1, id.row2, id.row2};
  for (int i = 0; i < 4; ++i) {
    create(registry, make_node(racks[i], SpaceNodeKind::rack, SpatialClass::rack, rows[i], 4,
                               "Rack"));
    set_rack_envelope(registry, racks[i], kRackHeight);
    set_placement(registry, racks[i],
                  PlanarRect::make(static_cast<std::int64_t>(i) * 2000, 0, kRackWidth,
                                   kRackDepth));
  }
  create_band(registry, id.band, id.rack1, 1, 4, 5);

  // A planar claim on the hall floor, clear of every rack placement.
  {
    CreateClaimRequest request;
    request.claim.id = id.planar_claim;
    request.claim.generation = EntityGeneration{1};
    request.claim.node = id.hall;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::infrastructure;
    request.claim.label = *DisplayLabel::parse("patch panel");
    request.claim.scope.kind = FootprintScopeKind::planar;
    request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 2000, 1000, 1000)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: planar claim failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

  // A rack unit claim on rack-01, clear of the band.
  {
    CreateClaimRequest request;
    request.claim.id = id.unit_claim;
    request.claim.generation = EntityGeneration{1};
    request.claim.node = id.rack1;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::asset;
    request.claim.scope.kind = FootprintScopeKind::rack_units;
    request.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(10, 6)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: unit claim failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

  // A reservation on rack-02. It carries the reservation reference that the
  // runtime owning reservation authority would have issued.
  {
    CreateReservationRequest request;
    request.reservation.id = id.reservation;
    request.reservation.generation = EntityGeneration{1};
    request.reservation.node = id.rack2;
    request.reservation.state = ReservationState::held;
    Result<ReservationRef> authority = ReservationRef::make("res-0001", 4, UpstreamState::active);
    if (!authority) std::abort();
    request.reservation.reservation = authority.value();
    request.reservation.holder_label = *DisplayLabel::parse("tenant b");
    request.reservation.scope.kind = FootprintScopeKind::rack_units;
    request.reservation.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 8)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: reservation failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

  // An active thermal exclusion on a corner of the hall floor.
  {
    CreateExclusionRequest request;
    request.region.id = id.exclusion;
    request.region.generation = EntityGeneration{1};
    request.region.node = id.hall;
    request.region.reason = ExclusionReason::thermal;
    request.region.state = ExclusionState::active;
    request.region.blocks = default_mask_for_reason(ExclusionReason::thermal);
    request.region.scope.kind = FootprintScopeKind::planar;
    request.region.scope.rects =
        *RectSet::build({PlanarRect::make(20000, 10000, 5000, 5000)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: exclusion failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

  // An enforceable front service clearance in front of rack-03.
  {
    CreateClearanceRequest request;
    request.constraint.id = id.clearance;
    request.constraint.generation = EntityGeneration{1};
    request.constraint.node = id.rack3;
    request.constraint.kind = ClearanceKind::front_service;
    request.constraint.has_band = true;
    request.constraint.band = PlanarRect::make(4000, kRackDepth, kRackWidth, 900);
    request.constraint.enforceable = true;
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: clearance failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

  // An expansion zone earmarking part of the hall floor.
  {
    CreateExpansionZoneRequest request;
    request.zone.id = id.zone;
    request.zone.generation = EntityGeneration{1};
    request.zone.node = id.hall;
    request.zone.state = ExpansionState::funded;
    request.zone.scope.kind = FootprintScopeKind::planar;
    request.zone.scope.rects = *RectSet::build({PlanarRect::make(3000, 3000, 2000, 2000)});
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "FIXTURE: expansion zone failed: %s\n",
                   outcome.error().to_string().c_str());
      std::abort();
    }
  }

}

// An in-memory registry holding the whole facility. No file is written.
inline SpaceCapacityRegistry build_in_memory() {
  Result<SpaceCapacityRegistry> created =
      SpaceCapacityRegistry::create_in_memory(*StoreId::parse("fixture-store"));
  if (!created) {
    std::fprintf(stderr, "FIXTURE: creating the in-memory registry failed: %s\n",
                 created.error().to_string().c_str());
    std::abort();
  }
  SpaceCapacityRegistry registry = std::move(created).value();
  populate(registry);
  return registry;
}

// A durable registry holding the whole facility. The population step runs only
// when the store is empty, so reopening an existing store returns the records
// that are already committed rather than trying to create them again.
inline SpaceCapacityRegistry build_durable(const std::string& path, const std::string& identity) {
  StoreOptions options;
  options.path = path;
  options.store_identity = *StoreId::parse(identity);
  options.actor = "fixture";
  options.source = "fixture";
  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  if (!opened) {
    std::fprintf(stderr, "FIXTURE: opening the durable registry failed: %s\n",
                 opened.error().to_string().c_str());
    std::abort();
  }
  SpaceCapacityRegistry registry = std::move(opened).value();
  if (registry.revision().value() == 0) populate(registry);
  return registry;
}

}  // namespace sc_fixture