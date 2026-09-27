// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - fit and availability queries.
//
// Proves the rack-unit search from the free-space arithmetic rather than from a
// guess: the complement of the blocked intervals is computed in the test and
// the exact candidates the engine returns are asserted against it. Proves the
// planar search's candidates are inside the plane, clear of every obstacle the
// ledger reports and aligned, and then proves the candidate set is COMPLETE by
// brute force: for every one of more than thirty obstacle layouts generated
// from a fixed seed, an exhaustive scan of the plane is compared against the
// engine's verdict, and the two must agree exactly. Proves an area-only request
// compares area and never claims a placement, that every assessment carries the
// revision it was taken at and grants nothing, and that an exclusion which
// blocks only service access blocks a service-access occupant and not a rack.

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "fixture.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;
using sc_fixture::Ids;

// ---------------------------------------------------------------------------
// The obstacles of the hall plane, gathered from the records themselves. The
// ledger's obstacle set is exactly this: active exclusions, enforceable
// clearance bands, placements, committed claims, reservations still in force
// and earmarking zones, all attributed to the hall floor.
// ---------------------------------------------------------------------------

std::vector<PlanarRect> hall_obstacles(const Snapshot& snapshot, const SpaceNodeId& hall,
                                       const FitContext& context) {
  std::vector<PlanarRect> obstacles;
  const auto attributed = [&snapshot, &hall](const SpaceNodeId& node) {
    SpaceNodeId cursor = node;
    std::uint32_t guard = 0;
    while (!cursor.empty()) {
      const SpaceNode* record = snapshot.find_node(cursor);
      if (record == nullptr) return SpaceNodeId{};
      if (record->own_planar.is_declared()) return record->id;
      if (++guard > 32u) return SpaceNodeId{};
      cursor = record->parent;
    }
    return SpaceNodeId{};
  };

  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.placement.has_base_rect || node.id == hall) continue;
    if (attributed(node.id) != hall) continue;
    obstacles.push_back(node.placement.base_rect);
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now() || region.blocks.empty()) continue;
    if (context.occupant != OccupantKind::unknown && !region.blocks.contains(context.occupant)) {
      continue;
    }
    if (attributed(region.node) != hall) continue;
    if (region.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : region.scope.rects.rects()) obstacles.push_back(rect);
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.has_band || !clearance.enforceable) continue;
    if (attributed(clearance.node) != hall) continue;
    obstacles.push_back(clearance.band);
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    if (!claim_state_consumes(claim.state)) continue;
    if (attributed(claim.node) != hall) continue;
    if (claim.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : claim.scope.rects.rects()) obstacles.push_back(rect);
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (attributed(reservation.node) != hall) continue;
    if (reservation.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : reservation.scope.rects.rects()) obstacles.push_back(rect);
  }
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    if (attributed(zone.node) != hall) continue;
    if (zone.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : zone.scope.rects.rects()) obstacles.push_back(rect);
  }
  return obstacles;
}

// ---------------------------------------------------------------------------
// A synthetic plane on a 100 mm grid: one site, one building that declares a
// WIDTH x DEPTH plane, and one active exclusion per obstacle cell.
// ---------------------------------------------------------------------------

SpaceCapacityRegistry make_grid_plane(const std::string& store, std::int64_t width,
                                      std::int64_t depth, SpaceNodeId& building_out) {
  SpaceCapacityRegistry registry =
      std::move(SpaceCapacityRegistry::create_in_memory(*StoreId::parse(store)).value());

  SpaceNode site;
  site.id = *SpaceNodeId::parse("grid-site");
  site.generation = EntityGeneration{1};
  site.kind = SpaceNodeKind::site;
  site.spatial_class = SpatialClass::outdoor;
  site.lifecycle = NodeLifecycle::available;
  site.label = *DisplayLabel::parse("Grid site");
  site.depth = 0;
  CreateNodeRequest create_site;
  create_site.node = site;
  if (!registry.apply(create_site)) std::abort();

  SpaceNode building;
  building.id = *SpaceNodeId::parse("grid-bldg");
  building.generation = EntityGeneration{1};
  building.kind = SpaceNodeKind::building;
  building.spatial_class = SpatialClass::enclosed;
  building.lifecycle = NodeLifecycle::available;
  building.label = *DisplayLabel::parse("Grid building");
  building.parent = site.id;
  building.depth = 1;
  CreateNodeRequest create_building;
  create_building.node = building;
  if (!registry.apply(create_building)) std::abort();

  SetNodeEnvelopeRequest envelope;
  envelope.node = building.id;
  PlanarEnvelope planar;
  planar.declared_area = SquareMillimeters{width * depth};
  planar.rects = *RectSet::build({PlanarRect::make(0, 0, width, depth)});
  envelope.planar = planar;
  if (!registry.apply(envelope)) std::abort();

  building_out = building.id;
  return registry;
}

void add_obstacle(SpaceCapacityRegistry& registry, const SpaceNodeId& plane, int index,
                  PlanarRect rect) {
  CreateExclusionRequest request;
  request.region.id = *ExclusionRegionId::parse("grid-obs-" + std::to_string(index));
  request.region.generation = EntityGeneration{1};
  request.region.node = plane;
  request.region.reason = ExclusionReason::structural;
  request.region.state = ExclusionState::active;
  request.region.blocks = default_mask_for_reason(ExclusionReason::structural);
  request.region.scope.kind = FootprintScopeKind::planar;
  request.region.scope.rects = *RectSet::build({rect});
  if (!registry.apply(request)) std::abort();
}

// An exhaustive scan of the plane: does ANY axis-aligned placement of
// `width` x `depth` exist, stepping `step` millimetres, without meeting an
// obstacle and without leaving the plane?
bool brute_force_fits(const std::vector<PlanarRect>& obstacles, PlanarRect plane,
                      std::int64_t width, std::int64_t depth, std::int64_t step) {
  for (std::int64_t x = 0; x + width <= plane.width.value(); x += step) {
    for (std::int64_t y = 0; y + depth <= plane.height.value(); y += step) {
      const PlanarRect candidate = PlanarRect::make(x, y, width, depth);
      if (!rect_is_within(candidate, plane)) continue;
      bool blocked = false;
      for (const PlanarRect& obstacle : obstacles) {
        if (rect_intersects(obstacle, candidate)) {
          blocked = true;
          break;
        }
      }
      if (!blocked) return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
  const Ids id = sc_fixture::ids();
  const FitContext plain{};
  const SnapshotPtr snapshot = registry.snapshot();

  SC_CASE("the rack unit search starts at the first aligned start of the first free run");
  {
    // The complement of the blocked intervals of rack-01 inside [1,43):
    //   blocked  [1,5)   band-a
    //   blocked  [10,16) claim-units-1
    //   free     [5,10)  5 units
    //   free     [16,43) 27 units
    // The lowest free run that can hold four units is therefore [5,10), and the
    // lowest aligned start in it is unit 5, so the first candidate is [5,9).
    // It is NOT [16,20): that run is free but it is not the lowest one.
    const IntervalSet blocked =
        *IntervalSet::build({RackUnitInterval::of_count(1, 4), RackUnitInterval::of_count(10, 6)});
    const IntervalSet free_space = *IntervalSet::complement(blocked, 42);
    SC_CHECK_EQ(free_space.size(), static_cast<std::size_t>(2));
    SC_CHECK(free_space.intervals().at(0) == RackUnitInterval::make(5, 10));
    SC_CHECK(free_space.intervals().at(1) == RackUnitInterval::make(16, 43));

    RackUnitFitRequest request;
    request.rack = id.rack1;
    request.needed = RackUnits{4};
    request.alignment = 1;
    const Result<FitAssessment> assessment = registry.assess(request);
    SC_CHECK(assessment.ok());
    SC_CHECK(assessment.value().verdict == FitVerdict::fits);
    SC_CHECK_EQ(assessment.value().candidates.size(), static_cast<std::size_t>(1));
    SC_CHECK(assessment.value().candidates.at(0).units == RackUnitInterval::make(5, 9));
    SC_CHECK(assessment.value().candidates.at(0).has_units);
    SC_CHECK(!assessment.value().candidates.at(0).has_rect);
    SC_CHECK(assessment.value().candidates.at(0).node == id.rack1);
    SC_CHECK(assessment.value().explanations.contains(ReasonCode::fits_contiguous));

    // The reported largest free run is the widest run, [16,43), which is 27
    // units. It is reported even when the request would not fit in it.
    SC_CHECK(assessment.value().largest_free_run.has_value());
    SC_CHECK(*assessment.value().largest_free_run == RackUnitInterval::make(16, 43));
    SC_CHECK_EQ(assessment.value().largest_free_run->count(), 27);
    SC_CHECK_EQ(assessment.value().units.occupied.value(), 10);
    SC_CHECK_EQ(assessment.value().units.available.value(), 32);
  }

  SC_CASE("candidate lists are bounded, ascending and free of duplicates");
  {
    RackUnitFitRequest request;
    request.rack = id.rack1;
    request.needed = RackUnits{4};
    request.alignment = 1;
    request.max_candidates = 3;
    const Result<FitAssessment> assessment = registry.assess(request);
    SC_CHECK(assessment.ok());
    SC_CHECK(assessment.value().verdict == FitVerdict::fits);
    SC_CHECK_EQ(assessment.value().candidates.size(), static_cast<std::size_t>(3));

    // Ascending start, then ascending extent: [5,9), [6,10) from the first run,
    // then [16,20) from the second.
    SC_CHECK(assessment.value().candidates.at(0).units == RackUnitInterval::make(5, 9));
    SC_CHECK(assessment.value().candidates.at(1).units == RackUnitInterval::make(6, 10));
    SC_CHECK(assessment.value().candidates.at(2).units == RackUnitInterval::make(16, 20));
    for (std::size_t i = 1; i < assessment.value().candidates.size(); ++i) {
      SC_CHECK(assessment.value().candidates.at(i - 1).units.first <
               assessment.value().candidates.at(i).units.first);
    }
    for (std::size_t i = 0; i < assessment.value().candidates.size(); ++i) {
      for (std::size_t j = i + 1; j < assessment.value().candidates.size(); ++j) {
        SC_CHECK(assessment.value().candidates.at(i).units !=
                 assessment.value().candidates.at(j).units);
      }
    }

    // A request for more candidates than the bound is clamped, never refused.
    request.max_candidates = 0;  // zero means "the lowest one only"
    const Result<FitAssessment> one = registry.assess(request);
    SC_CHECK(one.ok());
    SC_CHECK_EQ(one.value().candidates.size(), static_cast<std::size_t>(1));
  }

  SC_CASE("alignment constrains every reported start");
  {
    RackUnitFitRequest request;
    request.rack = id.rack1;
    request.needed = RackUnits{4};
    request.alignment = 4;
    request.max_candidates = 8;
    const Result<FitAssessment> assessment = registry.assess(request);
    SC_CHECK(assessment.ok());
    SC_CHECK(assessment.value().verdict == FitVerdict::fits);
    SC_CHECK(assessment.value().candidates.size() >= static_cast<std::size_t>(2));
    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      // Positions are 1-based: a start p is aligned when (p - 1) % 4 == 0.
      SC_CHECK_EQ((candidate.units.first - 1) % 4, 0);
      SC_CHECK(candidate.units.count() == 4);
      SC_CHECK(candidate.units.first >= 1);
      SC_CHECK(candidate.units.last <= 43);
    }
    // The first aligned start at or after the free run [5,10) is unit 5, and
    // the next free run [16,43) offers its first aligned start at unit 17.
    SC_CHECK(assessment.value().candidates.at(0).units == RackUnitInterval::make(5, 9));
    SC_CHECK(assessment.value().candidates.at(1).units == RackUnitInterval::make(17, 21));
  }

  SC_CASE("a request larger than the envelope, and one larger than any free run");
  {
    RackUnitFitRequest request;
    request.rack = id.rack1;
    request.needed = RackUnits{43};
    request.alignment = 1;
    const Result<FitAssessment> too_big = registry.assess(request);
    SC_CHECK(too_big.ok());
    SC_CHECK(too_big.value().verdict == FitVerdict::does_not_fit);
    SC_CHECK(too_big.value().candidates.empty());
    SC_CHECK(too_big.value().explanations.contains(ReasonCode::envelope_too_small));
    SC_CHECK(!too_big.value().explanations.contains(ReasonCode::fragmented_free_space));

    // 27 units is exactly the largest free run [16,43), so it fits.
    request.needed = RackUnits{27};
    const Result<FitAssessment> exact = registry.assess(request);
    SC_CHECK(exact.ok());
    SC_CHECK(exact.value().verdict == FitVerdict::fits);
    SC_CHECK(exact.value().candidates.at(0).units == RackUnitInterval::make(16, 43));

    // 28 units does not: the envelope holds 32 free units, but the largest
    // contiguous run is 27.
    request.needed = RackUnits{28};
    const Result<FitAssessment> fragmented = registry.assess(request);
    SC_CHECK(fragmented.ok());
    SC_CHECK(fragmented.value().verdict == FitVerdict::does_not_fit);
    SC_CHECK(fragmented.value().candidates.empty());
    SC_CHECK(fragmented.value().explanations.contains(ReasonCode::fragmented_free_space));
    SC_CHECK(!fragmented.value().explanations.contains(ReasonCode::envelope_too_small));
    SC_CHECK(fragmented.value().largest_free_run.has_value());
    SC_CHECK_EQ(fragmented.value().largest_free_run->count(), 27);
    SC_CHECK_EQ(fragmented.value().units.available.value(), 32);
  }

  SC_CASE("refusals: no envelope, no such node, impossible alignment");
  {
    RackUnitFitRequest request;
    request.rack = id.row1;  // a row declares no vertical envelope
    request.needed = RackUnits{4};
    const Result<FitAssessment> no_envelope = registry.assess(request);
    SC_CHECK(!no_envelope);
    SC_CHECK(no_envelope.code() == ErrorCode::envelope_not_declared);

    request.rack = *SpaceNodeId::parse("no-such-rack");
    const Result<FitAssessment> unknown = registry.assess(request);
    SC_CHECK(!unknown);
    SC_CHECK(unknown.code() == ErrorCode::not_found);

    request.rack = id.rack1;
    request.alignment = 0;
    const Result<FitAssessment> zero_alignment = registry.assess(request);
    SC_CHECK(!zero_alignment);
    SC_CHECK(zero_alignment.code() == ErrorCode::invalid_range);

    request.alignment = 2049;  // above the representable bound
    const Result<FitAssessment> huge_alignment = registry.assess(request);
    SC_CHECK(!huge_alignment);
    SC_CHECK(huge_alignment.code() == ErrorCode::invalid_range);

    request.alignment = 1;
    request.needed = RackUnits{0};
    const Result<FitAssessment> nothing = registry.assess(request);
    SC_CHECK(!nothing);
    SC_CHECK(nothing.code() == ErrorCode::invalid_range);
  }

  SC_CASE("planar candidates are inside the plane, clear of every obstacle and aligned");
  {
    PlanarRectFitRequest request;
    request.node = id.hall;
    request.width = Millimeters{600};
    request.depth = Millimeters{1200};
    request.alignment = Millimeters{0};  // zero means "any position"
    request.max_candidates = 8;
    const Result<FitAssessment> assessment = registry.assess(request);
    SC_CHECK(assessment.ok());
    SC_CHECK(assessment.value().verdict == FitVerdict::fits);
    SC_CHECK(assessment.value().candidates.size() >= static_cast<std::size_t>(1));

    const SpaceNode* hall = snapshot->find_node(id.hall);
    SC_CHECK(hall != nullptr);
    const std::vector<PlanarRect>& declared = hall->own_planar.rects.rects();
    const std::vector<PlanarRect> obstacles = hall_obstacles(*snapshot, id.hall, plain);
    SC_CHECK(obstacles.size() >= static_cast<std::size_t>(6));  // 4 racks, band, claim, zone
    const std::int64_t alignment = request.alignment.value() <= 1 ? 1 : request.alignment.value();

    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      SC_CHECK(candidate.has_rect);
      SC_CHECK(candidate.node == id.hall);
      SC_CHECK_EQ(candidate.rect.width.value(), 600);
      SC_CHECK_EQ(candidate.rect.height.value(), 1200);

      // (a) entirely inside the declared rectangles of the plane.
      std::int64_t covered = 0;
      for (const PlanarRect& plane_rect : declared) {
        const Checked<SquareMillimeters> piece = rect_intersection_area(plane_rect, candidate.rect);
        SC_CHECK(piece.ok());
        covered += piece.value.value();
      }
      const Checked<SquareMillimeters> own_area = candidate.rect.area();
      SC_CHECK(own_area.ok());
      SC_CHECK_EQ(covered, own_area.value.value());

      // (b) clear of every obstacle the ledger reports.
      for (const PlanarRect& obstacle : obstacles) {
        SC_CHECK(!rect_intersects(obstacle, candidate.rect));
      }

      // (c) aligned.
      SC_CHECK_EQ(candidate.rect.x.value() % alignment, 0);
      SC_CHECK_EQ(candidate.rect.y.value() % alignment, 0);
    }

    // The candidates are in canonical order: ascending y, then ascending x.
    for (std::size_t i = 1; i < assessment.value().candidates.size(); ++i) {
      const PlanarRect& previous = assessment.value().candidates.at(i - 1).rect;
      const PlanarRect& current = assessment.value().candidates.at(i).rect;
      SC_CHECK(previous.y < current.y || (previous.y == current.y && previous.x < current.x));
    }
  }

  SC_CASE("the planar candidate set is complete: brute force agrees on every layout");
  {
    constexpr std::int64_t kCell = 100;
    constexpr std::int64_t kCellsX = 8;
    constexpr std::int64_t kCellsY = 6;
    constexpr std::int64_t kWidth = 200;
    constexpr std::int64_t kDepth = 100;
    constexpr int kLayouts = 36;

    sc_test::Rng rng(0x5CA9E1ull);
    int mismatches = 0;
    int fitted = 0;
    int refused = 0;

    for (int layout = 0; layout < kLayouts; ++layout) {
      // Obstacle density sweeps from sparse to nearly full, so both verdicts
      // are exercised; the obstacles themselves come from the fixed seed.
      const std::uint64_t density = 20u + static_cast<std::uint64_t>(layout % 6) * 14u;
      std::vector<PlanarRect> obstacles;
      for (std::int64_t j = 0; j < kCellsY; ++j) {
        for (std::int64_t i = 0; i < kCellsX; ++i) {
          if (rng.bounded(100u) < density) {
            obstacles.push_back(PlanarRect::make(i * kCell, j * kCell, kCell, kCell));
          }
        }
      }

      SpaceNodeId building;
      SpaceCapacityRegistry plane_registry = make_grid_plane(
          "fit-grid-" + std::to_string(layout), kCellsX * kCell, kCellsY * kCell, building);
      int index = 0;
      for (const PlanarRect& obstacle : obstacles) {
        add_obstacle(plane_registry, building, index++, obstacle);
      }

      const PlanarRect plane = PlanarRect::make(0, 0, kCellsX * kCell, kCellsY * kCell);
      // The obstacles are unions of whole 100 mm cells and the requested
      // rectangle is a whole number of cells on each axis, so a placement fits
      // at some millimetre position exactly when it fits at that position's
      // cell-aligned corner. Every grid corner is tried; for the first four
      // layouts every single millimetre is tried as well, which checks that
      // reduction directly.
      const bool brute = brute_force_fits(obstacles, plane, kWidth, kDepth, kCell);
      if (layout < 4) {
        SC_CHECK_EQ(brute_force_fits(obstacles, plane, kWidth, kDepth, 1), brute);
      }

      PlanarRectFitRequest request;
      request.node = building;
      request.width = Millimeters{kWidth};
      request.depth = Millimeters{kDepth};
      request.alignment = Millimeters{1};
      request.max_candidates = 16;
      const Result<FitAssessment> assessment = plane_registry.assess(request);
      SC_CHECK(assessment.ok());
      const bool search = assessment.value().verdict == FitVerdict::fits;
      if (search) {
        ++fitted;
      } else {
        ++refused;
      }

      // The engine's verdict is true if and only if brute force found a
      // placement.
      SC_CHECK_EQ(search, brute);
      if (search != brute) ++mismatches;

      // And every candidate it did report survives the same exhaustive test.
      for (const PlacementCandidate& candidate : assessment.value().candidates) {
        SC_CHECK(rect_is_within(candidate.rect, plane));
        for (const PlanarRect& obstacle : obstacles) {
          SC_CHECK(!rect_intersects(obstacle, candidate.rect));
        }
      }
      if (search) {
        SC_CHECK(!assessment.value().candidates.empty());
        SC_CHECK(assessment.value().explanations.contains(ReasonCode::fits_contiguous));
      } else {
        SC_CHECK(assessment.value().candidates.empty());
        SC_CHECK(assessment.value().explanations.contains(ReasonCode::no_contiguous_run));
      }
    }

    SC_CHECK_EQ(mismatches, 0);
    // Both outcomes were reached, so the equivalence was not proved on one
    // branch only.
    SC_CHECK(fitted > 0);
    SC_CHECK(refused > 0);
    SC_CHECK_EQ(fitted + refused, kLayouts);
  }

  SC_CASE("an area request compares area and never claims a placement");
  {
    const std::int64_t available = snapshot->capacity_of(id.hall, plain).area.available.value();
    SC_CHECK_EQ(available, 566'580'000);

    PlanarAreaFitRequest request;
    request.node = id.hall;

    request.needed = SquareMillimeters{available};
    const Result<FitAssessment> exactly = registry.assess(request);
    SC_CHECK(exactly.ok());
    SC_CHECK(exactly.value().verdict == FitVerdict::fits);
    SC_CHECK(exactly.value().candidates.empty());  // an area answer claims no position
    SC_CHECK(exactly.value().explanations.contains(ReasonCode::fits_contiguous));
    SC_CHECK_EQ(exactly.value().area.available.value(), available);

    request.needed = SquareMillimeters{available + 1};
    const Result<FitAssessment> one_more = registry.assess(request);
    SC_CHECK(one_more.ok());
    SC_CHECK(one_more.value().verdict == FitVerdict::does_not_fit);
    SC_CHECK(one_more.value().candidates.empty());
    SC_CHECK(one_more.value().explanations.contains(ReasonCode::insufficient_free_area));

    request.needed = SquareMillimeters{0};
    const Result<FitAssessment> nothing = registry.assess(request);
    SC_CHECK(nothing.ok());
    SC_CHECK(nothing.value().verdict == FitVerdict::fits);

    request.needed = SquareMillimeters{-1};
    const Result<FitAssessment> negative = registry.assess(request);
    SC_CHECK(!negative);
    SC_CHECK(negative.code() == ErrorCode::invalid_range);
  }

  SC_CASE("every assessment carries its revision and grants nothing");
  {
    const RegistryRevision revision = registry.revision();
    const AttemptId attempt = registry.attempt();

    RackUnitFitRequest rack_request;
    rack_request.rack = id.rack1;
    rack_request.needed = RackUnits{4};
    const Result<FitAssessment> rack = registry.assess(rack_request);
    SC_CHECK(rack.ok());
    SC_CHECK_EQ(rack.value().revision.value(), revision.value());
    SC_CHECK(rack.value().attempt == attempt);
    SC_CHECK(!rack.value().grants_placement());
    SC_CHECK(rack.value().subject == id.rack1);
    SC_CHECK(rack.value().attributed_plane == id.hall);

    PlanarRectFitRequest planar_request;
    planar_request.node = id.row1;  // a grouping node: the question resolves upward
    planar_request.width = Millimeters{600};
    planar_request.depth = Millimeters{1200};
    const Result<FitAssessment> planar = registry.assess(planar_request);
    SC_CHECK(planar.ok());
    SC_CHECK_EQ(planar.value().revision.value(), revision.value());
    SC_CHECK(!planar.value().grants_placement());
    SC_CHECK(planar.value().subject == id.row1);
    SC_CHECK(planar.value().attributed_plane == id.hall);
    SC_CHECK_EQ(planar.value().units.state, MeasureState::unknown);

    PlanarAreaFitRequest area_request;
    area_request.node = id.hall;
    area_request.needed = SquareMillimeters{1};
    const Result<FitAssessment> area = registry.assess(area_request);
    SC_CHECK(area.ok());
    SC_CHECK_EQ(area.value().revision.value(), revision.value());
    SC_CHECK(!area.value().grants_placement());

    // Nothing above changed the registry: a query observes and never acts.
    SC_CHECK_EQ(registry.revision().value(), revision.value());
  }

  SC_CASE("an exclusion that blocks only service access blocks one occupant and not another");
  {
    // The fixture's thermal exclusion blocks every occupant kind EXCEPT service
    // access; this one blocks service access and nothing else.
    CreateExclusionRequest request;
    request.region.id = *ExclusionRegionId::parse("svc-only-1");
    request.region.generation = EntityGeneration{1};
    request.region.node = id.hall;
    request.region.reason = ExclusionReason::service_clearance;
    request.region.state = ExclusionState::active;
    request.region.blocks = default_mask_for_reason(ExclusionReason::service_clearance);
    request.region.scope.kind = FootprintScopeKind::planar;
    request.region.scope.rects = *RectSet::build({PlanarRect::make(10000, 10000, 1000, 1000)});
    const Result<MutationOutcome> applied = registry.apply(request);
    SC_CHECK(applied.ok());
    SC_CHECK(request.region.blocks.contains(OccupantKind::service_access));
    SC_CHECK(!request.region.blocks.contains(OccupantKind::rack));

    FitContext as_rack{};
    as_rack.occupant = OccupantKind::rack;
    FitContext as_service{};
    as_service.occupant = OccupantKind::service_access;

    const SnapshotPtr after = registry.snapshot();
    // A rack occupant: the service-only region does not block it, so only the
    // thermal exclusion and the enforceable clearance band are subtracted.
    SC_CHECK_EQ(after->capacity_of(id.hall, as_rack).area.excluded.value(), 25'540'000);
    // A service-access occupant: the thermal region no longer blocks it, and
    // the service-only region does. 540,000 + 1,000,000.
    SC_CHECK_EQ(after->capacity_of(id.hall, as_service).area.excluded.value(), 1'540'000);
    // An unspecified occupant is blocked by both, so both are subtracted.
    SC_CHECK_EQ(after->capacity_of(id.hall, plain).area.excluded.value(), 26'540'000);

    // The same reading through the fit path: the assessment's own ledger is the
    // ledger of the context it was asked with.
    PlanarAreaFitRequest fit;
    fit.node = id.hall;
    fit.needed = SquareMillimeters{0};
    fit.context = as_rack;
    const Result<FitAssessment> rack_view = registry.assess(fit);
    SC_CHECK(rack_view.ok());
    SC_CHECK_EQ(rack_view.value().area.excluded.value(), 25'540'000);
    fit.context = as_service;
    const Result<FitAssessment> service_view = registry.assess(fit);
    SC_CHECK(service_view.ok());
    SC_CHECK_EQ(service_view.value().area.excluded.value(), 1'540'000);
    SC_CHECK(service_view.value().area.available.value() >
              rack_view.value().area.available.value());
  }

  return ::sc_test::summary("fit");
}
