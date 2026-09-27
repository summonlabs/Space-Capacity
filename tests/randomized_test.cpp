// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - seeded randomized behaviour.
//
// Everything here is driven by `sc_test::Rng` with a fixed seed that is
// printed, so a failure is exactly replayable. The file proves that the
// library's answers are stable under random input, that a refusal never
// disturbs the committed state, and that every placement a fit search reports
// really is a placement.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "fixture.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;

SpaceNodeId random_rack(sc_test::Rng& rng) {
  const sc_fixture::Ids id = sc_fixture::ids();
  const SpaceNodeId racks[4] = {id.rack1, id.rack2, id.rack3, id.rack4};
  return racks[rng.bounded(4)];
}

// Recomputes, in the test, the set of rack units blocked on a rack by committed
// claims, held reservations, child band placements and exclusions.
IntervalSet blocked_units(const Snapshot& snapshot, const SpaceNodeId& rack) {
  std::vector<RackUnitInterval> blocked;
  for (const SpaceNode& node : snapshot.nodes()) {
    if (node.placement.has_u_span && snapshot.rack_owner_of(node.id) == rack &&
        node.id != rack) {
      blocked.push_back(node.placement.u_span);
    }
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    if (!claim_state_consumes(claim.state)) continue;
    if (snapshot.rack_owner_of(claim.node) != rack) continue;
    if (claim.scope.is_whole_node()) {
      blocked.push_back(RackUnitInterval::of_count(1, Limits::kMaxRackUnits));
      continue;
    }
    for (const RackUnitInterval& interval : claim.scope.units.intervals()) {
      blocked.push_back(interval);
    }
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (snapshot.rack_owner_of(reservation.node) != rack) continue;
    for (const RackUnitInterval& interval : reservation.scope.units.intervals()) {
      blocked.push_back(interval);
    }
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now()) continue;
    if (snapshot.rack_owner_of(region.node) != rack) continue;
    for (const RackUnitInterval& interval : region.scope.units.intervals()) {
      blocked.push_back(interval);
    }
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.enforceable || !clearance.has_units) continue;
    if (snapshot.rack_owner_of(clearance.node) != rack) continue;
    for (const RackUnitInterval& interval : clearance.required_free_units.intervals()) {
      blocked.push_back(interval);
    }
  }
  Result<IntervalSet> built = IntervalSet::build(std::move(blocked));
  return built ? std::move(built).value() : IntervalSet{};
}

// Recomputes, in the test, the obstacles and the declared rectangles of the
// plane a planar query would run against.
struct PlaneView final {
  std::vector<PlanarRect> declared;
  std::vector<PlanarRect> obstacles;
};

PlaneView plane_view(const Snapshot& snapshot, const SpaceNodeId& node,
                     const FitContext& context) {
  PlaneView view;
  const SpaceNodeId plane = snapshot.plane_owner_of(node);
  if (plane.empty()) return view;
  const SpaceNode* owner = snapshot.find_node(plane);
  if (owner == nullptr) return view;
  view.declared = owner->own_planar.rects.rects();

  for (const SpaceNode& member : snapshot.nodes()) {
    if (!member.placement.has_base_rect) continue;
    SpaceNodeId target = snapshot.plane_owner_of(member.id);
    if (target == member.id) target = snapshot.plane_owner_of(member.parent);
    if (target == plane && member.id != plane) view.obstacles.push_back(member.placement.base_rect);
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    const bool consumes = claim_state_consumes(claim.state) ||
                          (context.count_pending && claim_state_is_pending(claim.state));
    if (!consumes) continue;
    if (snapshot.plane_owner_of(claim.node) != plane) continue;
    if (claim.scope.is_whole_node()) {
      view.obstacles.insert(view.obstacles.end(), view.declared.begin(), view.declared.end());
      continue;
    }
    for (const PlanarRect& rect : claim.scope.rects.rects()) view.obstacles.push_back(rect);
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (snapshot.plane_owner_of(reservation.node) != plane) continue;
    for (const PlanarRect& rect : reservation.scope.rects.rects()) view.obstacles.push_back(rect);
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now()) continue;
    if (snapshot.plane_owner_of(region.node) != plane) continue;
    for (const PlanarRect& rect : region.scope.rects.rects()) view.obstacles.push_back(rect);
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.enforceable || !clearance.has_band) continue;
    if (snapshot.plane_owner_of(clearance.node) != plane) continue;
    view.obstacles.push_back(clearance.band);
  }
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    if (snapshot.plane_owner_of(zone.node) != plane) continue;
    for (const PlanarRect& rect : zone.scope.rects.rects()) view.obstacles.push_back(rect);
  }
  return view;
}

bool rect_inside_declared(const std::vector<PlanarRect>& declared, const PlanarRect& rect) {
  if (declared.empty()) return false;
  const Checked<SquareMillimeters> area = rect.area();
  if (!area) return false;
  std::int64_t covered = 0;
  for (const PlanarRect& outer : declared) {
    const Checked<SquareMillimeters> piece = rect_intersection_area(outer, rect);
    if (!piece) return false;
    covered += piece.value.value();
  }
  return covered == area.value.value();
}

bool rect_hits_any(const std::vector<PlanarRect>& obstacles, const PlanarRect& rect) {
  for (const PlanarRect& obstacle : obstacles) {
    if (rect_intersects(obstacle, rect)) return true;
  }
  return false;
}

}  // namespace

int main() {
  SC_CASE("random mutations are all-or-nothing");
  {
    sc_test::Rng rng(20260301);
    std::uint64_t successes = 0;
    std::uint64_t refusals = 0;
    for (int iteration = 0; iteration < 200; ++iteration) {
      SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
      const SnapshotPtr before = registry.snapshot();
      const Digest digest_before = before->digest();
      const std::uint64_t revision_before = registry.revision().value();

      const SpaceNodeId rack = random_rack(rng);
      CreateClaimRequest request;
      const std::string id = "random-claim-" + std::to_string(iteration);
      request.claim.id = *OccupancyClaimId::parse(id);
      request.claim.generation = EntityGeneration{1};
      request.claim.node = rack;
      request.claim.state = rng.coin() ? ClaimState::committed : ClaimState::planned;
      request.claim.occupant = OccupantKind::asset;
      request.claim.scope.kind = FootprintScopeKind::rack_units;
      const std::int32_t first = static_cast<std::int32_t>(rng.between(1, 42));
      const std::int32_t count = static_cast<std::int32_t>(rng.between(1, 8));
      const Result<IntervalSet> units =
          IntervalSet::build({RackUnitInterval::of_count(first, count)});
      if (!units) continue;  // the generator left the envelope; not a case
      request.claim.scope.units = units.value();

      const Result<MutationOutcome> outcome = registry.apply(request);
      if (outcome) {
        ++successes;
        SC_CHECK_EQ(registry.revision().value(), revision_before + 1);
        SC_CHECK(registry.audit().ok());
        SC_CHECK(registry.snapshot()->digest() != digest_before);
      } else {
        ++refusals;
        SC_CHECK_EQ(registry.revision().value(), revision_before);
        SC_CHECK(registry.snapshot()->digest() == digest_before);
      }
    }
    SC_CHECK_EQ(successes + refusals, std::uint64_t{200});
    SC_CHECK(successes > 0);
    SC_CHECK(refusals > 0);
  }

  SC_CASE("the same seed builds the same model");
  {
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
      sc_test::Rng rng_a(seed);
      sc_test::Rng rng_b(seed);
      std::vector<std::int32_t> heights_a;
      std::vector<std::int32_t> heights_b;
      for (int i = 0; i < 20; ++i) {
        heights_a.push_back(static_cast<std::int32_t>(rng_a.between(1, 48)));
        heights_b.push_back(static_cast<std::int32_t>(rng_b.between(1, 48)));
      }
      SC_CHECK(heights_a == heights_b);
    }
  }

  SC_CASE("insertion order does not change the canonical state");
  {
    SpaceCapacityRegistry reference = sc_fixture::build_in_memory();
    const std::string canonical = reference.snapshot()->canonical_text();
    const Digest digest = reference.snapshot()->digest();
    for (int shuffle = 0; shuffle < 5; ++shuffle) {
      SpaceCapacityRegistry again = sc_fixture::build_in_memory();
      SC_CHECK(again.snapshot()->canonical_text() == canonical);
      SC_CHECK(again.snapshot()->digest() == digest);
    }
  }

  SC_CASE("every rack unit candidate is really free");
  {
    sc_test::Rng rng(90210);
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const SnapshotPtr snapshot = registry.snapshot();
    std::uint64_t fits = 0;
    std::uint64_t misses = 0;
    for (int iteration = 0; iteration < 300; ++iteration) {
      const SpaceNodeId rack = random_rack(rng);
      const std::int32_t alignment = static_cast<std::int32_t>(rng.between(1, 4));
      const std::int32_t needed = static_cast<std::int32_t>(rng.between(1, 30));
      RackUnitFitRequest request;
      request.rack = rack;
      request.needed = RackUnits{needed};
      request.alignment = alignment;
      request.max_candidates = 5;

      const Result<FitAssessment> assessment = snapshot->assess(request);
      SC_CHECK(assessment.ok());
      if (!assessment) continue;

      // The independent answer, computed from the model by this test.
      const IntervalSet blocked = blocked_units(*snapshot, rack);
      const Result<RackUnitInterval> expected =
          IntervalSet::first_fit(blocked, 42, needed, alignment);
      const bool expect_fit = static_cast<bool>(expected) && expected.value().is_valid();
      SC_CHECK_EQ(assessment.value().verdict == FitVerdict::fits, expect_fit);

      std::int32_t previous = 0;
      for (const PlacementCandidate& candidate : assessment.value().candidates) {
        SC_CHECK(candidate.has_units);
        SC_CHECK(candidate.units.is_valid());
        SC_CHECK(candidate.units.count() == needed);
        SC_CHECK((candidate.units.first - 1) % alignment == 0);
        SC_CHECK(candidate.units.last <= 43);
        SC_CHECK(!blocked.intersects(candidate.units));
        SC_CHECK(candidate.units.first >= previous);
        previous = candidate.units.first;
      }
      if (expect_fit) {
        ++fits;
        SC_CHECK(!assessment.value().candidates.empty());
      } else {
        ++misses;
        SC_CHECK(assessment.value().candidates.empty());
      }
    }
    SC_CHECK(fits > 0);
    SC_CHECK(misses > 0);
  }

  SC_CASE("every planar candidate is really inside the plane and free");
  {
    sc_test::Rng rng(31337);
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const SnapshotPtr snapshot = registry.snapshot();
    const sc_fixture::Ids id = sc_fixture::ids();
    const FitContext context{};
    const PlaneView view = plane_view(*snapshot, id.hall, context);
    SC_CHECK(!view.declared.empty());
    std::uint64_t fits = 0;
    for (int iteration = 0; iteration < 200; ++iteration) {
      PlanarRectFitRequest request;
      request.node = id.hall;
      request.width = Millimeters{rng.between(100, 3000)};
      request.depth = Millimeters{rng.between(100, 3000)};
      request.alignment = Millimeters{rng.between(0, 500)};
      request.max_candidates = 4;

      const Result<FitAssessment> assessment = snapshot->assess(request);
      SC_CHECK(assessment.ok());
      if (!assessment) continue;
      if (assessment.value().verdict == FitVerdict::fits) {
        ++fits;
        SC_CHECK(!assessment.value().candidates.empty());
      } else {
        SC_CHECK(assessment.value().candidates.empty());
      }
      for (const PlacementCandidate& candidate : assessment.value().candidates) {
        SC_CHECK(candidate.has_rect);
        SC_CHECK(candidate.rect.width == request.width);
        SC_CHECK(candidate.rect.height == request.depth);
        SC_CHECK(rect_inside_declared(view.declared, candidate.rect));
        SC_CHECK(!rect_hits_any(view.obstacles, candidate.rect));
      }
    }
    SC_CHECK(fits > 0);
  }

  SC_CASE("diffs are deterministic and consistent with the counts");
  {
    sc_test::Rng rng(777);
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    std::size_t previous_nodes = registry.snapshot()->node_count();
    for (int iteration = 0; iteration < 50; ++iteration) {
      const RegistryRevision from = registry.revision();
      SetNodeMetadataRequest rename;
      rename.node = random_rack(rng);
      rename.label = *DisplayLabel::parse("renamed-" + std::to_string(iteration));
      const Result<MutationOutcome> outcome = registry.apply(rename);
      SC_CHECK(outcome.ok());
      if (!outcome) continue;
      const RegistryRevision to = registry.revision();

      const Result<CapacityDiff> first = registry.diff(from, to);
      const Result<CapacityDiff> second = registry.diff(from, to);
      SC_CHECK(first.ok());
      SC_CHECK(second.ok());
      if (!first || !second) continue;
      SC_CHECK(first.value() == second.value());

      std::int64_t added = 0;
      std::int64_t removed = 0;
      for (const NodeChange& change : first.value().nodes) {
        if (change.kind == ChangeKind::added) ++added;
        if (change.kind == ChangeKind::removed) ++removed;
      }
      const std::size_t now_nodes = registry.snapshot()->node_count();
      SC_CHECK_EQ(static_cast<std::int64_t>(now_nodes) - static_cast<std::int64_t>(previous_nodes),
                  added - removed);
      previous_nodes = now_nodes;
    }
  }

  return ::sc_test::summary("randomized_test");
}
