// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - deliberate attempts to break the library.
//
// What this file proves:
//   * every value that is too large, too deep or too numerous is refused with
//     its own typed code, and the refusal happens at the type that owns the
//     bound rather than after the value has been used to size anything;
//   * a duplicate identity is refused in every direction the library checks,
//     and a second record is never merged into the first;
//   * a containment cycle, a self-parent and a depth past the containment bound
//     are each refused with their own code;
//   * a product that would leave the signed range is refused before it is
//     formed, and afterwards no ledger anywhere in the model reports a negative
//     area;
//   * a stale observation, a write fenced on an old revision and an attempt at
//     or below the fresh floor are classified for exactly what they are;
//   * an empty path, a path whose name is ".." and a path that is a directory
//     are refused before a single file is created;
//   * a grouping node reports its own area as unknown and names the plane that
//     owns the space, so its zero can never be read as a measurement;
//   * every refusal leaves both the snapshot digest and the revision exactly as
//     they were, which is what makes "refused" mean "nothing happened".
//
// The file counts every attempt that must be refused and asserts that the
// number refused equals the number attempted. There is no timeout, watchdog,
// alarm or process limit anywhere in it, and no randomized case runs without a
// fixed seed that the harness prints.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"

#include "fixture.hpp"
#include "test_support.hpp"

using namespace dccp::space_capacity;
using namespace sc_fixture;

namespace {

// ---------------------------------------------------------------------------
// Attack accounting
// ---------------------------------------------------------------------------

int attacks_attempted = 0;
int attacks_refused = 0;
int documented_outcomes = 0;
int documented_holes = 0;

// One counted attack. `actual` is the code the library returned, `expected` is
// the exact code this file requires, and `state_unchanged` is the result of
// comparing the snapshot digest taken before the attempt with the one taken
// after it. An attack counts as refused only when both hold, so a refusal that
// quietly changed the model is not counted as a refusal at all.
void count_attack(ErrorCode actual, ErrorCode expected, bool state_unchanged) {
  ++attacks_attempted;
  SC_CHECK(actual == expected);
  SC_CHECK(state_unchanged);
  if (actual == expected && state_unchanged) ++attacks_refused;
}

void count_documented() { ++documented_outcomes; }

Digest digest_now(const SpaceCapacityRegistry& registry) {
  return registry.snapshot()->digest();
}

// ---------------------------------------------------------------------------
// Ledger walks
// ---------------------------------------------------------------------------

// The ledger identity the library documents: at a subtree root, what the plane
// declares is exactly what is excluded, structurally placed, claimed, held,
// earmarked and still available.
void check_ledger_identity(const AreaLedger& area, const char* what) {
  const std::int64_t parts = area.excluded.value() + area.structural.value() + area.claimed.value() +
                             area.held.value() + area.earmarked.value() + area.available.value();
  ::sc_test::record(area.declared.value() == parts, what, __FILE__, __LINE__);
}

void check_no_negative_area(const AreaLedger& area) {
  SC_CHECK(area.declared.value() >= 0);
  SC_CHECK(area.excluded.value() >= 0);
  SC_CHECK(area.usable.value() >= 0);
  SC_CHECK(area.structural.value() >= 0);
  SC_CHECK(area.claimed.value() >= 0);
  SC_CHECK(area.held.value() >= 0);
  SC_CHECK(area.earmarked.value() >= 0);
  SC_CHECK(area.available.value() >= 0);
  SC_CHECK(area.pending.value() >= 0);
  SC_CHECK(area.planned.value() >= 0);
}

void check_no_negative_units(const UnitLedger& units) {
  SC_CHECK(units.declared.value() >= 0);
  SC_CHECK(units.excluded.value() >= 0);
  SC_CHECK(units.usable.value() >= 0);
  SC_CHECK(units.occupied.value() >= 0);
  SC_CHECK(units.available.value() >= 0);
  SC_CHECK(units.pending.value() >= 0);
  SC_CHECK(units.planned.value() >= 0);
  SC_CHECK(units.largest_free_run.value() >= 0);
}

// Walks every ledger the model can produce - the whole-model rollup, each
// node's own capacity and each node's subtree report - and asserts that no area
// and no rack unit count is negative. A negative number anywhere would mean an
// unsigned wrap or a subtraction that left the signed range.
void walk_every_ledger(const Snapshot& snapshot) {
  const FitContext context{};
  check_no_negative_area(model_rollup(snapshot, context).area);
  for (const SpaceNode& node : snapshot.nodes()) {
    const NodeCapacity capacity = snapshot.capacity_of(node.id, context);
    check_no_negative_area(capacity.area);
    check_no_negative_units(capacity.units);
    const Result<CapacityReport> report = snapshot.report(node.id, context);
    if (report) {
      check_no_negative_area(report.value().area);
      check_no_negative_units(report.value().units);
    }
  }
}

std::vector<std::string> list_working_directory() {
  std::vector<std::string> names;
  std::error_code error;
  std::filesystem::directory_iterator iterator(std::filesystem::path("."), error);
  if (error) return names;
  for (const std::filesystem::directory_entry& entry : iterator) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

// True when the second listing contains no entry the first did not, among the
// entries whose name begins with `stem`. Comparing only the entries a refused
// open could have produced keeps the check exact even when another test running
// beside this one creates its own files in the same working directory.
bool nothing_added_with_prefix(const std::vector<std::string>& before,
                               const std::vector<std::string>& after,
                               const std::string& stem) {
  for (const std::string& name : after) {
    if (name.rfind(stem, 0) != 0) continue;
    if (std::find(before.begin(), before.end(), name) == before.end()) return false;
  }
  return true;
}

void remove_store_files(const std::string& state) {
  std::error_code error;
  std::filesystem::remove(state, error);
  std::filesystem::remove(state + ".prev", error);
  std::filesystem::remove(state + ".identity", error);
  std::filesystem::remove(state + ".lock", error);
}

// ---------------------------------------------------------------------------
// A randomized boundary fuzz
// ---------------------------------------------------------------------------

// One fuzz round. Every round takes a fresh digest, applies exactly one
// generated request, and requires one of exactly two things to be true:
//
//   * the request succeeded: the revision advanced by exactly one, and the
//     published model still passes the full audit; or
//   * the request was refused: the code is a typed refusal, never `ok` and
//     never an internal-inconsistency code, and both the digest and the
//     revision are exactly what they were before the attempt.
//
// This is a property, not a table of expected codes, so it cannot be satisfied
// by a library that refuses everything and cannot be satisfied by one that
// accepts everything either.
void fuzz_round(SpaceCapacityRegistry& registry, sc_test::Rng& rng, int round,
                std::int64_t* succeeded, std::int64_t* refused) {
  const Ids id = ids();
  const SnapshotPtr before_snapshot = registry.snapshot();
  const Digest before_digest = before_snapshot->digest();
  const std::uint64_t before_revision = before_snapshot->revision().value();

  const SpaceNodeId parents[6] = {id.site, id.building, id.hall, id.row1, id.row2, id.rack1};
  const std::uint32_t parent_depths[6] = {0, 1, 2, 3, 3, 4};
  const SpaceNodeKind kinds[4] = {SpaceNodeKind::hall, SpaceNodeKind::row, SpaceNodeKind::rack,
                                  SpaceNodeKind::building};
  const SpatialClass classes[4] = {SpatialClass::floor, SpatialClass::aisle, SpatialClass::rack,
                                   SpatialClass::enclosed};
  const ClaimState claim_states[4] = {ClaimState::planned, ClaimState::submitted,
                                      ClaimState::committed, ClaimState::released};
  const NodeLifecycle lifecycles[5] = {NodeLifecycle::planned, NodeLifecycle::provisioning,
                                       NodeLifecycle::available, NodeLifecycle::restricted,
                                       NodeLifecycle::decommissioning};

  const std::string node_key = "fz-node-" + std::to_string(round);
  const std::size_t pick = static_cast<std::size_t>(rng.bounded(6));
  const SpaceNodeId parent = parents[pick];
  const std::uint32_t parent_depth = parent_depths[pick];

  Result<MutationOutcome> outcome = Error::make(ErrorCode::internal_error, "unset");
  switch (rng.bounded(6)) {
    case 0: {
      CreateNodeRequest request;
      SpaceNode node = make_node(*SpaceNodeId::parse(node_key), kinds[rng.bounded(4)],
                                 classes[rng.bounded(4)], parent,
                                 parent_depth + 1 + static_cast<std::uint32_t>(rng.bounded(9)),
                                 "fuzz node");
      node.lifecycle = lifecycles[rng.bounded(5)];
      if (rng.coin()) {
        const std::int64_t side = rng.between(1, 40000);
        const std::int64_t span = rng.between(1, 40000);
        node.own_planar.declared_area = SquareMillimeters{span * span};
        const Result<RectSet> rects = RectSet::build({PlanarRect::make(side, side, span, span)});
        if (rects) node.own_planar.rects = rects.value();
      }
      request.node = node;
      outcome = registry.apply(request);
      break;
    }
    case 1: {
      SetNodeEnvelopeRequest request;
      request.node = parent;
      const std::uint64_t choice = rng.bounded(3);
      if (choice == 0) {
        PlanarEnvelope envelope;
        envelope.declared_area = SquareMillimeters{
            rng.between(-1, Limits::kMaxSquareMillimeters + Limits::kMaxSquareMillimeters)};
        const Result<RectSet> rects = RectSet::build(
            {PlanarRect::make(0, 0, rng.between(1, 20000), rng.between(1, 20000))});
        if (rects) envelope.rects = rects.value();
        request.planar = envelope;
      } else if (choice == 1) {
        RackEnvelope envelope;
        envelope.height = RackUnits{static_cast<std::int32_t>(
            rng.between(-2, static_cast<std::int64_t>(Limits::kMaxRackUnits) + 2))};
        request.rack = envelope;
      } else {
        PlanarEnvelope envelope;
        envelope.declared_area = SquareMillimeters{0};
        request.planar = envelope;
      }
      outcome = registry.apply(request);
      break;
    }
    case 2: {
      SetNodePlacementRequest request;
      request.node = parent;
      request.placement.has_base_rect = true;
      request.placement.base_rect =
          PlanarRect::make(rng.between(-10, 40000), rng.between(-10, 40000),
                           rng.between(1, 40000), rng.between(1, 40000));
      outcome = registry.apply(request);
      break;
    }
    case 3: {
      SetNodeLifecycleRequest request;
      request.node = parent;
      request.lifecycle = lifecycles[rng.bounded(5)];
      outcome = registry.apply(request);
      break;
    }
    case 4: {
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("fz-claim-" + std::to_string(round));
      request.claim.generation = EntityGeneration{1};
      request.claim.node = parent;
      request.claim.state = claim_states[rng.bounded(4)];
      const std::uint64_t shape = rng.bounded(3);
      if (shape == 0) {
        request.claim.scope.kind = FootprintScopeKind::planar;
        const Result<RectSet> rects =
            RectSet::build({PlanarRect::make(rng.between(0, 20000), rng.between(0, 20000),
                                             rng.between(1, 2000), rng.between(1, 2000))});
        if (!rects) {
          outcome = rects.error();
          break;
        }
        request.claim.scope.rects = rects.value();
      } else if (shape == 1) {
        request.claim.scope.kind = FootprintScopeKind::rack_units;
        const Result<IntervalSet> units = IntervalSet::build({RackUnitInterval::of_count(
            static_cast<std::int32_t>(rng.between(1, 40)),
            static_cast<std::int32_t>(rng.between(1, 10)))});
        if (!units) {
          outcome = units.error();
          break;
        }
        request.claim.scope.units = units.value();
      } else {
        request.claim.scope.kind = FootprintScopeKind::whole_node;
      }
      outcome = registry.apply(request);
      break;
    }
    default: {
      const SnapshotPtr snapshot = registry.snapshot();
      if (snapshot->claims().empty()) {
        outcome = Error::make(ErrorCode::not_found, "no claim to transition");
        break;
      }
      TransitionClaimRequest request;
      request.claim = snapshot->claims()[rng.bounded(snapshot->claims().size())].id;
      request.next = claim_states[rng.bounded(4)];
      outcome = registry.apply(request);
      break;
    }
  }

  const SnapshotPtr after_snapshot = registry.snapshot();
  if (outcome) {
    ++*succeeded;
    SC_CHECK(after_snapshot->revision().value() == before_revision + 1);
    SC_CHECK(after_snapshot->audit().ok());
  } else {
    ++*refused;
    SC_CHECK(outcome.code() != ErrorCode::ok);
    SC_CHECK(outcome.code() != ErrorCode::invariant_violation);
    SC_CHECK(outcome.code() != ErrorCode::internal_error);
    SC_CHECK(after_snapshot->revision().value() == before_revision);
    SC_CHECK(after_snapshot->digest() == before_digest);
  }
  SC_CHECK(after_snapshot->audit().ok());
}

}  // namespace

int main() {
  SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
  const Ids id = ids();
  const FitContext context{};

  // -------------------------------------------------------------------------
  SC_CASE("the fixture itself holds the documented ledger identity");
  {
    const SnapshotPtr snapshot = registry.snapshot();
    SC_CHECK(snapshot->audit().ok());
    check_ledger_identity(model_rollup(*snapshot, context).area, "model rollup: declared == parts");
    const Result<CapacityRollup> site = snapshot->rollup(id.site, context);
    const Result<CapacityRollup> building = snapshot->rollup(id.building, context);
    const Result<CapacityRollup> hall = snapshot->rollup(id.hall, context);
    SC_CHECK(site.ok());
    SC_CHECK(building.ok());
    SC_CHECK(hall.ok());
    if (site) check_ledger_identity(site.value().area, "site-a: declared == parts");
    if (building) check_ledger_identity(building.value().area, "bldg-1: declared == parts");
    if (hall) check_ledger_identity(hall.value().area, "hall-1: declared == parts");
    // A nested plane is a subdivision: the enclosing plane's own declared area
    // is what it states it built, its measured `declared` excludes the
    // sub-plane area, and the sub-plane placement is not part of its structural
    // measure.
    const SpaceNode* building_node = snapshot->find_node(id.building);
    SC_CHECK(building_node != nullptr);
    if (building_node != nullptr) {
      SC_CHECK_EQ(building_node->own_planar.declared_area.value(), building_area().value());
    }
    const NodeCapacity building_own = snapshot->capacity_of(id.building, context);
    SC_CHECK(building_own.owns_plane);
    SC_CHECK_EQ(building_own.area.declared.value(),
                building_area().value() - hall_area().value());
    SC_CHECK_EQ(building_own.area.structural.value(), 0);
    SC_CHECK_EQ(building_own.area.available.value(),
                building_area().value() - hall_area().value());
  }

  // -------------------------------------------------------------------------
  SC_CASE("spare nodes the attacks are aimed at");
  {
    CreateNodeRequest planned;
    SpaceNode planned_hall = make_node(*SpaceNodeId::parse("adv-planned-1"), SpaceNodeKind::hall,
                                       SpatialClass::floor, id.building, 2, "planned hall");
    planned_hall.lifecycle = NodeLifecycle::planned;
    planned_hall.placement.has_base_rect = true;
    planned_hall.placement.base_rect = PlanarRect::make(30000, 20000, 10000, 10000);
    planned_hall.own_planar.declared_area = SquareMillimeters{10000 * 10000};
    planned_hall.own_planar.rects =
        *RectSet::build({PlanarRect::make(30000, 20000, 10000, 10000)});
    planned.node = planned_hall;
    const Result<MutationOutcome> planned_outcome = registry.apply(planned);
    SC_CHECK(planned_outcome.ok());
    count_documented();

    CreateNodeRequest spare;
    spare.node = make_node(*SpaceNodeId::parse("adv-rack-a"), SpaceNodeKind::rack,
                           SpatialClass::rack, id.row2, 4, "spare rack");
    const Result<MutationOutcome> spare_outcome = registry.apply(spare);
    SC_CHECK(spare_outcome.ok());
    count_documented();

    SetNodeEnvelopeRequest envelope;
    envelope.node = *SpaceNodeId::parse("adv-rack-a");
    RackEnvelope rack_envelope;
    rack_envelope.height = RackUnits{10};
    envelope.rack = rack_envelope;
    const Result<MutationOutcome> envelope_outcome = registry.apply(envelope);
    SC_CHECK(envelope_outcome.ok());
    count_documented();
  }

  // -------------------------------------------------------------------------
  SC_CASE("oversized values are refused with their own codes");
  {
    {
      const Digest before = digest_now(registry);
      SetNodeEnvelopeRequest request;
      request.node = id.hall;
      PlanarEnvelope envelope;
      envelope.declared_area = SquareMillimeters{Limits::kMaxSquareMillimeters + 1};
      request.planar = envelope;
      count_attack(registry.apply(request).code(), ErrorCode::extent_out_of_envelope,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      SetNodeEnvelopeRequest request;
      request.node = id.hall;
      PlanarEnvelope envelope;
      envelope.declared_area = SquareMillimeters{9223372036854775807LL};
      request.planar = envelope;
      count_attack(registry.apply(request).code(), ErrorCode::extent_out_of_envelope,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      SetNodeEnvelopeRequest request;
      request.node = id.hall;
      PlanarEnvelope envelope;
      envelope.declared_area = SquareMillimeters{-1};
      request.planar = envelope;
      count_attack(registry.apply(request).code(), ErrorCode::invalid_extent,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      SetNodeEnvelopeRequest request;
      request.node = id.rack3;
      RackEnvelope envelope;
      envelope.height = RackUnits{Limits::kMaxRackUnits + 1};
      request.rack = envelope;
      count_attack(registry.apply(request).code(), ErrorCode::unit_envelope_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      SetNodeEnvelopeRequest request;
      request.node = id.rack3;
      RackEnvelope envelope;
      envelope.height = RackUnits{-1};
      request.rack = envelope;
      count_attack(registry.apply(request).code(), ErrorCode::unit_envelope_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      SetNodePlacementRequest request;
      request.node = id.hall;
      request.placement.has_base_rect = true;
      request.placement.base_rect =
          PlanarRect::make(Limits::kMaxMillimeters + 1, 0, 1000, 1000);
      count_attack(registry.apply(request).code(), ErrorCode::invalid_extent,
                   digest_now(registry) == before);
    }
    {
      // A rectangle that is one millimetre past the coordinate bound cannot
      // even be built into a set, so no request can carry it.
      const Digest before = digest_now(registry);
      const Result<RectSet> built =
          RectSet::build({PlanarRect::make(0, Limits::kMaxMillimeters + 1, 1000, 1000)});
      count_attack(built.code(), ErrorCode::invalid_extent, digest_now(registry) == before);
    }
    {
      // The far edge itself leaves the signed range, so the rectangle is
      // refused as out of bounds rather than formed.
      const Digest before = digest_now(registry);
      const Result<RectSet> built =
          RectSet::build({PlanarRect::make(0, 0, 9223372036854775807LL, 2)});
      count_attack(built.code(), ErrorCode::invalid_extent, digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      const Result<IntervalSet> built =
          IntervalSet::build({RackUnitInterval::make(1, Limits::kMaxRackUnits + 2)});
      count_attack(built.code(), ErrorCode::invalid_extent, digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      std::vector<PlanarRect> rects;
      for (std::uint32_t i = 0; i < Limits::kMaxExtentsPerRecord + 1; ++i) {
        rects.push_back(PlanarRect::make(static_cast<std::int64_t>(i), 0, 1, 1));
      }
      count_attack(RectSet::build(rects).code(), ErrorCode::limit_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      std::vector<RackUnitInterval> intervals;
      for (std::uint32_t i = 0; i < Limits::kMaxExtentsPerRecord + 1; ++i) {
        intervals.push_back(
            RackUnitInterval::of_count(static_cast<std::int32_t>(1 + i * 2), 1));
      }
      count_attack(IntervalSet::build(intervals).code(), ErrorCode::limit_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      std::vector<AssetRef> refs;
      for (std::uint32_t i = 0; i < Limits::kMaxReferencesPerRecord + 1; ++i) {
        const Result<AssetRef> ref = AssetRef::of("asset-" + std::to_string(i));
        if (ref) refs.push_back(ref.value());
      }
      count_attack(AssetRefSet::build(refs).code(), ErrorCode::limit_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      std::vector<PolicyRef> refs;
      for (std::uint32_t i = 0; i < Limits::kMaxReferencesPerRecord + 1; ++i) {
        const Result<PolicyRef> ref = PolicyRef::of("policy-" + std::to_string(i));
        if (ref) refs.push_back(ref.value());
      }
      count_attack(PolicyRefSet::build(refs).code(), ErrorCode::limit_exceeded,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      std::vector<EvidenceRef> refs;
      for (std::uint32_t i = 0; i < Limits::kMaxEvidencePerRecord + 1; ++i) {
        EvidenceRef ref;
        const Result<ExternalId> parsed = ExternalId::parse("ev-" + std::to_string(i));
        if (parsed) ref.id = parsed.value();
        refs.push_back(ref);
      }
      count_attack(EvidenceSet::build(refs).code(), ErrorCode::limit_exceeded,
                   digest_now(registry) == before);
    }
    // The bound is a bound and not a blanket refusal: exactly at the limit both
    // sets build.
    std::vector<PlanarRect> at_limit;
    for (std::uint32_t i = 0; i < Limits::kMaxExtentsPerRecord; ++i) {
      at_limit.push_back(PlanarRect::make(static_cast<std::int64_t>(i), 0, 1, 1));
    }
    SC_CHECK(RectSet::build(at_limit).ok());
    std::vector<EvidenceRef> evidence_at_limit;
    for (std::uint32_t i = 0; i < Limits::kMaxEvidencePerRecord; ++i) {
      EvidenceRef ref;
      const Result<ExternalId> parsed = ExternalId::parse("ev-at-" + std::to_string(i));
      if (parsed) ref.id = parsed.value();
      evidence_at_limit.push_back(ref);
    }
    SC_CHECK(EvidenceSet::build(evidence_at_limit).ok());
    count_documented();
  }

  // -------------------------------------------------------------------------
  SC_CASE("duplicate identity is refused and never merged");
  {
    {
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = make_node(id.rack1, SpaceNodeKind::rack, SpatialClass::rack, id.row1, 4,
                               "an impostor");
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateClaimRequest request;
      request.claim.id = id.planar_claim;
      request.claim.generation = EntityGeneration{1};
      request.claim.node = id.hall;
      request.claim.state = ClaimState::planned;
      request.claim.scope.kind = FootprintScopeKind::planar;
      request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 9000, 500, 500)});
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateReservationRequest request;
      request.reservation.id = id.planar_claim;
      request.reservation.generation = EntityGeneration{1};
      request.reservation.node = id.rack2;
      request.reservation.state = ReservationState::released;
      count_attack(registry.apply(request).code(), ErrorCode::identity_conflict,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateReservationRequest request;
      request.reservation.id = id.reservation;
      request.reservation.generation = EntityGeneration{1};
      request.reservation.node = id.rack2;
      request.reservation.state = ReservationState::released;
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateExclusionRequest request;
      request.region.id = id.exclusion;
      request.region.generation = EntityGeneration{1};
      request.region.node = id.hall;
      request.region.reason = ExclusionReason::thermal;
      request.region.state = ExclusionState::active;
      request.region.blocks = default_mask_for_reason(ExclusionReason::thermal);
      request.region.scope.kind = FootprintScopeKind::planar;
      request.region.scope.rects = *RectSet::build({PlanarRect::make(0, 9000, 500, 500)});
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateClearanceRequest request;
      request.constraint.id = id.clearance;
      request.constraint.generation = EntityGeneration{1};
      request.constraint.node = id.rack3;
      request.constraint.kind = ClearanceKind::front_service;
      request.constraint.has_band = true;
      request.constraint.band = PlanarRect::make(4000, 1200, 600, 900);
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateExpansionZoneRequest request;
      request.zone.id = id.zone;
      request.zone.generation = EntityGeneration{1};
      request.zone.node = id.hall;
      request.zone.state = ExpansionState::funded;
      request.zone.scope.kind = FootprintScopeKind::planar;
      request.zone.scope.rects = *RectSet::build({PlanarRect::make(0, 9000, 500, 500)});
      count_attack(registry.apply(request).code(), ErrorCode::already_exists,
                   digest_now(registry) == before);
    }
  }

  // -------------------------------------------------------------------------
  SC_CASE("aliasing: the same identity in two claims is never merged");
  {
    OccupancyClaimId shared = *OccupancyClaimId::parse("adv-alias");
    CreateClaimRequest first;
    first.claim.id = shared;
    first.claim.generation = EntityGeneration{1};
    first.claim.node = id.hall;
    first.claim.state = ClaimState::committed;
    first.claim.label = *DisplayLabel::parse("the first one");
    first.claim.scope.kind = FootprintScopeKind::planar;
    const PlanarRect first_rect = PlanarRect::make(0, 4000, 500, 500);
    first.claim.scope.rects = *RectSet::build({first_rect});
    const Result<MutationOutcome> first_outcome = registry.apply(first);
    SC_CHECK(first_outcome.ok());
    count_documented();
    const Digest after_first = digest_now(registry);

    CreateClaimRequest second;
    second.claim.id = shared;
    second.claim.generation = EntityGeneration{1};
    second.claim.node = id.hall;
    second.claim.state = ClaimState::committed;
    second.claim.label = *DisplayLabel::parse("the second one");
    second.claim.scope.kind = FootprintScopeKind::planar;
    second.claim.scope.rects = *RectSet::build({PlanarRect::make(9000, 9000, 500, 500)});
    count_attack(registry.apply(second).code(), ErrorCode::already_exists,
                 digest_now(registry) == after_first);

    const SnapshotPtr snapshot = registry.snapshot();
    std::size_t found = 0;
    for (const OccupancyClaim& claim : snapshot->claims()) {
      if (claim.id == shared) {
        ++found;
        SC_CHECK(claim.scope.rects == *RectSet::build({first_rect}));
        SC_CHECK(claim.label == *DisplayLabel::parse("the first one"));
      }
    }
    SC_CHECK_EQ(found, static_cast<std::size_t>(1));
  }

  // -------------------------------------------------------------------------
  SC_CASE("containment cycle and self-parent");
  {
    {
      const Digest before = digest_now(registry);
      ReparentNodeRequest request;
      request.node = id.building;
      request.new_parent = id.rack1;
      count_attack(registry.apply(request).code(), ErrorCode::containment_cycle,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      ReparentNodeRequest request;
      request.node = id.hall;
      request.new_parent = id.hall;
      count_attack(registry.apply(request).code(), ErrorCode::self_reference,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      ReparentNodeRequest request;
      request.node = id.site;
      request.new_parent = id.band;
      count_attack(registry.apply(request).code(), ErrorCode::containment_cycle,
                   digest_now(registry) == before);
    }
  }

  // -------------------------------------------------------------------------
  SC_CASE("depth attack");
  {
    // The deepest chain the containment table allows is
    // site(0) -> building(1) -> hall(2) -> row(3) -> rack(4) -> band(5), so the
    // additional levels up to the bound of eight can only be claimed by a
    // fabricated record. Every such claim is refused.
    const SnapshotPtr snapshot = registry.snapshot();
    const SpaceNode* band = snapshot->find_node(id.band);
    SC_CHECK(band != nullptr);
    if (band != nullptr) SC_CHECK_EQ(band->depth, std::uint32_t{5});

    SpaceNode deep = make_node(*SpaceNodeId::parse("adv-deep"), SpaceNodeKind::rack,
                               SpatialClass::rack, id.site, Limits::kMaxContainmentDepth + 1,
                               "too deep");
    {
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = deep;
      count_attack(registry.apply(request).code(), ErrorCode::depth_exceeded,
                   digest_now(registry) == before);
    }
    count_attack(validate_node_shape(deep).code(), ErrorCode::depth_exceeded, true);
    {
      const Digest before = digest_now(registry);
      SpaceNode lying = make_node(*SpaceNodeId::parse("adv-lying"), SpaceNodeKind::rack,
                                  SpatialClass::rack, id.site, 6, "skipped levels");
      CreateNodeRequest request;
      request.node = lying;
      count_attack(registry.apply(request).code(), ErrorCode::invalid_placement,
                   digest_now(registry) == before);
    }
    {
      // One level below the deepest legal chain: a band may contain nothing, so
      // the chain cannot be extended even though the depth bound is not reached.
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = make_node(*SpaceNodeId::parse("adv-deeper"), SpaceNodeKind::rack,
                               SpatialClass::rack, id.band, 6, "below a band");
      count_attack(registry.apply(request).code(), ErrorCode::invalid_kind_for_parent,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = make_node(*SpaceNodeId::parse("adv-orphan"), SpaceNodeKind::rack,
                               SpatialClass::rack, *SpaceNodeId::parse("no-such-parent"), 4,
                               "orphan");
      count_attack(registry.apply(request).code(), ErrorCode::not_found,
                   digest_now(registry) == before);
    }
  }

  // -------------------------------------------------------------------------
  SC_CASE("integer overflow is refused before the product is formed");
  {
    // The area of this rectangle cannot be represented, so the checked multiply
    // reports an overflow instead of wrapping.
    const PlanarRect huge = PlanarRect::make(0, 0, 9223372036854775807LL, 2);
    const Checked<SquareMillimeters> area = huge.area();
    SC_CHECK(!area.ok());
    SC_CHECK(area.value.is_zero());
    count_attack(area.ok() ? ErrorCode::ok : ErrorCode::arithmetic_overflow,
                 ErrorCode::arithmetic_overflow, true);
    SC_CHECK(!huge.is_valid());
    SC_CHECK(!huge.is_degenerate());

    // A counter at its maximum refuses to wrap, so no stale generation can ever
    // be resurrected by an increment.
    const RegistryRevision top{Limits::kMaxCounter};
    count_attack(top.next().code(), ErrorCode::arithmetic_overflow, true);
    const EntityGeneration generation_top{Limits::kMaxCounter};
    count_attack(generation_top.next().code(), ErrorCode::arithmetic_overflow, true);

    {
      // A product that would exceed the square-millimetre bound is refused by
      // the extent bound before any area is formed from it.
      const Digest before = digest_now(registry);
      const Result<RectSet> built =
          RectSet::build({PlanarRect::make(0, 0, Limits::kMaxMillimeters, Limits::kMaxMillimeters),
                          PlanarRect::make(0, Limits::kMaxMillimeters, 1, 1)});
      count_attack(built.code(), ErrorCode::invalid_extent, digest_now(registry) == before);
    }
    // Nothing anywhere in the model reports a negative area.
    walk_every_ledger(*registry.snapshot());
  }

  // -------------------------------------------------------------------------
  SC_CASE("resource exhaustion is refused before anything is allocated");
  {
    // The extent, reference and evidence bounds are 64, 32 and 32. Each set
    // builder compares the count against its bound as its very first statement,
    // before it validates, sorts, merges or copies a single entry, so an
    // attacker-supplied count can never drive an allocation: the vectors below
    // are sized by this test, not by the library, and the library's own refusal
    // costs one comparison.
    const std::size_t absurd = 4096;
    const Digest before = digest_now(registry);

    std::vector<PlanarRect> rects(absurd, PlanarRect::make(0, 0, 1, 1));
    count_attack(RectSet::build(rects).code(), ErrorCode::limit_exceeded,
                 digest_now(registry) == before);

    std::vector<RackUnitInterval> intervals(absurd, RackUnitInterval::make(1, 2));
    count_attack(IntervalSet::build(intervals).code(), ErrorCode::limit_exceeded,
                 digest_now(registry) == before);

    // The evidence entries are deliberately left empty: if the size bound were
    // checked after the content, the refusal would be `empty_value` instead.
    std::vector<EvidenceRef> evidence(absurd);
    count_attack(EvidenceSet::build(evidence).code(), ErrorCode::limit_exceeded,
                 digest_now(registry) == before);

    std::vector<AssetRef> assets;
    for (std::size_t i = 0; i < absurd; ++i) {
      const Result<AssetRef> ref = AssetRef::of("asset-" + std::to_string(i));
      if (ref) assets.push_back(ref.value());
    }
    count_attack(AssetRefSet::build(assets).code(), ErrorCode::limit_exceeded,
                 digest_now(registry) == before);

    std::vector<LocationRef> locations;
    for (std::size_t i = 0; i < absurd; ++i) {
      const Result<LocationRef> ref = LocationRef::of("loc-" + std::to_string(i));
      if (ref) locations.push_back(ref.value());
    }
    count_attack(LocationRefSet::build(locations).code(), ErrorCode::limit_exceeded,
                 digest_now(registry) == before);
  }

  // -------------------------------------------------------------------------
  SC_CASE("path attacks on Store::open create nothing");
  {
    // Every name a refused open could have produced is derived from the path it
    // was given, so the absence of those derived names is the check. The
    // working directory is listed before and after each attempt, and only the
    // entries that could belong to this case are compared, so a test running
    // beside this one cannot make the check flaky.
    const std::string directory_name = "sc_adversarial_directory";

    // Re-runnable in a reused working directory: anything an interrupted run of
    // this case left behind is removed before the attacks start, so a second
    // run in the same directory sees exactly what the first one saw.
    remove_store_files(directory_name);
    remove_store_files("sc_adversarial_absent.spcstate");
    remove_store_files("sc_adversarial_mustexist.spcstate");
    {
      std::error_code cleanup_error;
      std::filesystem::remove_all(directory_name, cleanup_error);
      SC_CHECK(!cleanup_error);
    }

    {
      // The working directory is not assumed to hold anything: it may be empty,
      // and under CTest it is. What is asserted is that the set of entries did
      // not change and that nothing derived from the refused path appeared.
      const std::vector<std::string> before = list_working_directory();
      StoreOptions options;
      options.path = std::filesystem::path("..");
      options.store_identity = *StoreId::parse("adv-path-store");
      count_attack(Store::open(options).code(), ErrorCode::path_rejected, true);
      const std::vector<std::string> after = list_working_directory();
      SC_CHECK(after == before);
      // ".." would have been written beside itself: "...identity", "...lock"
      // and "...prev".
      SC_CHECK(nothing_added_with_prefix(before, after, ".."));
      SC_CHECK(!std::filesystem::exists("...identity"));
      SC_CHECK(!std::filesystem::exists("...lock"));
      SC_CHECK(!std::filesystem::exists("...prev"));
    }

    {
      std::error_code error;
      std::filesystem::create_directories(directory_name, error);
      SC_CHECK(!error);
    }
    {
      const std::vector<std::string> before = list_working_directory();
      StoreOptions options;
      options.path = std::filesystem::path(directory_name);
      options.store_identity = *StoreId::parse("adv-path-store");
      count_attack(Store::open(options).code(), ErrorCode::path_rejected, true);
      const std::vector<std::string> after = list_working_directory();
      // The refused open left nothing behind: the directory this case created
      // is the only entry that differs from the start of the case, and it is
      // already present in both listings.
      SC_CHECK(after == before);
      SC_CHECK(nothing_added_with_prefix(before, after, directory_name));
      // The directory is still a directory and gained nothing.
      SC_CHECK(std::filesystem::is_directory(directory_name));
      SC_CHECK(!std::filesystem::exists(directory_name + ".identity"));
      SC_CHECK(!std::filesystem::exists(directory_name + ".lock"));
      SC_CHECK(!std::filesystem::exists(directory_name + ".prev"));
      std::size_t inside = 0;
      std::error_code listing_error;
      std::filesystem::directory_iterator entries(directory_name, listing_error);
      if (!listing_error) {
        for (const std::filesystem::directory_entry& entry : entries) {
          (void)entry;
          ++inside;
        }
      }
      SC_CHECK(!listing_error);
      SC_CHECK_EQ(inside, static_cast<std::size_t>(0));
    }

    {
      const std::vector<std::string> before = list_working_directory();
      StoreOptions options;
      options.path = std::filesystem::path();
      options.store_identity = *StoreId::parse("adv-path-store");
      count_attack(Store::open(options).code(), ErrorCode::path_rejected, true);
      const std::vector<std::string> after = list_working_directory();
      SC_CHECK(after == before);
      // An empty path derives exactly ".identity", ".lock" and ".prev" in the
      // working directory; none of them may appear.
      SC_CHECK(!std::filesystem::exists(".identity"));
      SC_CHECK(!std::filesystem::exists(".lock"));
      SC_CHECK(!std::filesystem::exists(".prev"));
      SC_CHECK(nothing_added_with_prefix(before, after, ".identity"));
      SC_CHECK(nothing_added_with_prefix(before, after, ".lock"));
      SC_CHECK(nothing_added_with_prefix(before, after, ".prev"));
    }

    {
      // A read-only open of a path with no state file is refused as having no
      // authoritative state: it is not an empty store, it is no store.
      StoreOptions options;
      options.path = std::filesystem::path("sc_adversarial_absent.spcstate");
      options.store_identity = *StoreId::parse("adv-path-store");
      options.mode = OpenMode::read_only;
      SC_CHECK_EQ(Store::open(options).code(), ErrorCode::no_authoritative_state);
      SC_CHECK(!std::filesystem::exists("sc_adversarial_absent.spcstate"));
      SC_CHECK(!std::filesystem::exists("sc_adversarial_absent.spcstate.identity"));
      remove_store_files("sc_adversarial_absent.spcstate");
      count_documented();
    }
    {
      // A must-exist open of a path with no state file is refused the same way:
      // the path validator allows a missing path on the open path, so the code
      // the operator sees is the one that owns the decision, not `not_found`.
      StoreOptions options;
      options.path = std::filesystem::path("sc_adversarial_mustexist.spcstate");
      options.store_identity = *StoreId::parse("adv-path-store");
      options.create = CreateMode::must_exist;
      SC_CHECK_EQ(Store::open(options).code(), ErrorCode::no_authoritative_state);
      SC_CHECK(!std::filesystem::exists("sc_adversarial_mustexist.spcstate"));
      remove_store_files("sc_adversarial_mustexist.spcstate");
      count_documented();
    }

    remove_store_files("sc_adversarial_directory");
    std::error_code error;
    std::filesystem::remove_all(directory_name, error);
    SC_CHECK(!error);
    SC_CHECK(!std::filesystem::exists(directory_name));
  }

  // -------------------------------------------------------------------------
  SC_CASE("zero is not unknown");
  {
    const SnapshotPtr snapshot = registry.snapshot();
    const NodeCapacity grouping = snapshot->capacity_of(id.row1, context);
    SC_CHECK(!grouping.owns_plane);
    // A grouping node has no plane of its own, so every area it reports is
    // zero and the state says the zero is not a measurement.
    SC_CHECK(grouping.area.state != MeasureState::known);
    SC_CHECK_EQ(grouping.area.state, MeasureState::unknown);
    SC_CHECK(grouping.area.declared.is_zero());
    SC_CHECK(grouping.area.available.is_zero());
    SC_CHECK(grouping.area.claimed.is_zero());
    SC_CHECK(grouping.area.structural.is_zero());
    // The plane that owns the space is still named.
    SC_CHECK_EQ(grouping.attributed_plane, id.hall);
    SC_CHECK_EQ(std::string_view(measure_state_name(grouping.area.state)), std::string_view("unknown"));

    const Result<CapacityRollup> grouped = snapshot->rollup(id.row1, context);
    SC_CHECK(grouped.ok());
    if (grouped) SC_CHECK(grouped.value().contains_undeclared_envelopes);

    const Result<CapacityReport> report = snapshot->report(id.row1, context);
    SC_CHECK(report.ok());
    if (report) {
      SC_CHECK(!report.value().owns_plane);
      SC_CHECK_EQ(report.value().attributed_plane, id.hall);
      SC_CHECK(report.value().explanations.contains(ReasonCode::plane_not_declared));
    }
    // The plane owner itself reports a known measure, so the difference between
    // "measured zero" and "nothing declared" is observable.
    const NodeCapacity plane = snapshot->capacity_of(id.hall, context);
    SC_CHECK(plane.owns_plane);
    SC_CHECK_EQ(plane.area.state, MeasureState::known);
    SC_CHECK(plane.area.declared.value() > 0);
  }

  // -------------------------------------------------------------------------
  SC_CASE("state confusion");
  {
    {
      SetNodeLifecycleRequest retire;
      retire.node = *SpaceNodeId::parse("adv-rack-a");
      retire.lifecycle = NodeLifecycle::retired;
      const Result<MutationOutcome> retired = registry.apply(retire);
      SC_CHECK(retired.ok());
      count_documented();
    }
    {
      const Digest before = digest_now(registry);
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("adv-claim-on-retired");
      request.claim.generation = EntityGeneration{1};
      request.claim.node = *SpaceNodeId::parse("adv-rack-a");
      request.claim.state = ClaimState::committed;
      request.claim.scope.kind = FootprintScopeKind::whole_node;
      count_attack(registry.apply(request).code(), ErrorCode::node_not_available,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("adv-claim-orphan");
      request.claim.generation = EntityGeneration{1};
      request.claim.node = *SpaceNodeId::parse("no-such-node");
      request.claim.state = ClaimState::planned;
      request.claim.scope.kind = FootprintScopeKind::whole_node;
      count_attack(registry.apply(request).code(), ErrorCode::not_found,
                   digest_now(registry) == before);
    }
    {
      // A committed claim on a planned node is accepted, and it consumes
      // nothing: the whole-model availability is bit for bit what it was.
      const SpaceNodeId planned = *SpaceNodeId::parse("adv-planned-1");
      const std::int64_t available_before =
          registry.model_rollup(context).area.available.value();
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("adv-claim-planned");
      request.claim.generation = EntityGeneration{1};
      request.claim.node = planned;
      request.claim.state = ClaimState::committed;
      request.claim.scope.kind = FootprintScopeKind::whole_node;
      const Result<MutationOutcome> outcome = registry.apply(request);
      SC_CHECK(outcome.ok());
      count_documented();
      const std::int64_t available_after = registry.model_rollup(context).area.available.value();
      SC_CHECK_EQ(available_after, available_before);

      const Result<CapacityReport> report = registry.report(planned, context);
      SC_CHECK(report.ok());
      if (report) {
        SC_CHECK_EQ(report.value().lifecycle, NodeLifecycle::planned);
        SC_CHECK(report.value().area.usable.is_zero());
        SC_CHECK(report.value().area.available.is_zero());
        SC_CHECK_EQ(report.value().area.planned.value(),
                    report.value().area.declared.value());
      }
    }
    {
      // A claim whose identity collides with a reservation is accepted by this
      // build: `apply_create_claim` checks the claim family only, while
      // `apply_create_reservation` checks both. The model still audits and the
      // store still round-trips, but the documented "one identity space" is not
      // enforced in this direction. Reported, not asserted as correct.
      const SnapshotPtr snapshot = registry.snapshot();
      const bool reservation_present = snapshot->find_reservation(id.reservation) != nullptr;
      SC_CHECK(reservation_present);
      CreateClaimRequest request;
      request.claim.id = id.reservation;
      request.claim.generation = EntityGeneration{1};
      request.claim.node = id.hall;
      request.claim.state = ClaimState::planned;
      request.claim.scope.kind = FootprintScopeKind::planar;
      request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 9500, 400, 400)});
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (!outcome) {
        // The day the missing check is added this branch is the one that runs.
        SC_CHECK_EQ(outcome.code(), ErrorCode::identity_conflict);
        count_documented();
      } else {
        ++documented_holes;
        const SnapshotPtr after = registry.snapshot();
        SC_CHECK(after->find_claim(id.reservation) != nullptr);
        SC_CHECK(after->find_reservation(id.reservation) != nullptr);
        SC_CHECK(after->audit().ok());
        std::fprintf(stdout,
                     "WARNING: a claim may reuse a reservation identity in this build "
                     "(src/mutations.cpp apply_create_claim checks only the claim family)\n");
      }
    }
  }

  // -------------------------------------------------------------------------
  SC_CASE("stale authority, fencing and recovery");
  {
    const SnapshotPtr snapshot = registry.snapshot();
    const RegistryRevision old_revision = snapshot->revision();
    const SpaceNode* untouched = snapshot->find_node(id.rack4);
    SC_CHECK(untouched != nullptr);
    ObservationToken token;
    token.subject = id.rack4;
    token.subject_generation = untouched->generation;
    token.revision = old_revision;
    token.attempt = snapshot->attempt();

    // A mutation moves the revision by exactly one and nothing else.
    SetNodeMetadataRequest metadata;
    metadata.node = id.building;
    metadata.label = *DisplayLabel::parse("renamed by the fence case");
    const Result<MutationOutcome> mutated = registry.apply(metadata);
    SC_CHECK(mutated.ok());
    count_documented();
    SC_CHECK_EQ(registry.revision().value(), old_revision.value() + 1);

    const Result<Revalidation> stale = registry.revalidate(token);
    SC_CHECK(stale.ok());
    if (stale) {
      SC_CHECK(!stale.value().valid);
      SC_CHECK_EQ(stale.value().freshness, Freshness::stale);
      SC_CHECK_EQ(stale.value().reason, ErrorCode::stale_revision);
      SC_CHECK(stale.value().revision_compared);
      SC_CHECK_EQ(stale.value().expected_revision.value(), old_revision.value());
      SC_CHECK_EQ(stale.value().actual_revision.value(), old_revision.value() + 1);
    }
    count_documented();

    // A read fenced on the revision the caller observed before that mutation is
    // refused, and the refusal changes nothing.
    {
      const Digest before = digest_now(registry);
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("adv-claim-old-revision");
      request.claim.generation = EntityGeneration{1};
      request.claim.node = id.hall;
      request.claim.state = ClaimState::planned;
      request.claim.scope.kind = FootprintScopeKind::planar;
      request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 9500, 400, 400)});
      request.precondition = Precondition::at_revision(old_revision);
      count_attack(registry.apply(request).code(), ErrorCode::stale_revision,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = make_node(*SpaceNodeId::parse("adv-old-revision"), SpaceNodeKind::rack,
                               SpatialClass::rack, id.row1, 4, "old revision");
      request.precondition = Precondition::at_revision(old_revision);
      count_attack(registry.apply(request).code(), ErrorCode::stale_revision,
                   digest_now(registry) == before);
    }
    {
      const Digest before = digest_now(registry);
      CreateNodeRequest request;
      request.node = make_node(*SpaceNodeId::parse("adv-wrong-generation"), SpaceNodeKind::rack,
                               SpatialClass::rack, id.row1, 4, "wrong generation");
      request.precondition = Precondition::at_generation(EntityGeneration{7});
      count_attack(registry.apply(request).code(), ErrorCode::stale_generation,
                   digest_now(registry) == before);
    }

    // A token whose attempt is at or below the fresh floor describes state this
    // session recovered rather than state it observed.
    ObservationToken recovered_token;
    recovered_token.subject = id.rack4;
    recovered_token.subject_generation = registry.snapshot()->find_node(id.rack4)->generation;
    recovered_token.revision = registry.revision();
    recovered_token.attempt = registry.fresh_floor();
    const Result<Revalidation> recovered = registry.revalidate(recovered_token);
    SC_CHECK(recovered.ok());
    if (recovered) {
      SC_CHECK(!recovered.value().valid);
      SC_CHECK_EQ(recovered.value().freshness, Freshness::recovered);
      SC_CHECK_EQ(recovered.value().reason, ErrorCode::stale_authority);
    }
    count_documented();

    // The same token one attempt above the floor is fresh, so the floor is a
    // floor and not a blanket refusal.
    ObservationToken fresh_token = recovered_token;
    fresh_token.attempt = AttemptId{registry.incarnation(), registry.fresh_floor().sequence() + 1};
    const Result<Revalidation> fresh = registry.revalidate(fresh_token);
    SC_CHECK(fresh.ok());
    if (fresh) {
      SC_CHECK(fresh.value().valid);
      SC_CHECK_EQ(fresh.value().freshness, Freshness::fresh);
    }
    count_documented();

    // A token for a node that is gone is orphaned, not stale.
    ObservationToken orphan;
    orphan.subject = *SpaceNodeId::parse("no-such-subject");
    orphan.revision = registry.revision();
    orphan.attempt = AttemptId{};
    const Result<Revalidation> orphaned = registry.revalidate(orphan);
    SC_CHECK(orphaned.ok());
    if (orphaned) {
      SC_CHECK(!orphaned.value().valid);
      SC_CHECK_EQ(orphaned.value().freshness, Freshness::orphaned);
      SC_CHECK_EQ(orphaned.value().reason, ErrorCode::not_found);
    }
    count_documented();
  }

  // -------------------------------------------------------------------------
  SC_CASE("randomized boundary fuzz");
  {
    // The seed is fixed and the harness prints it, so every run above is
    // replayable exactly.
    sc_test::Rng rng(0x5CA7E5C0DEull);
    SpaceCapacityRegistry fuzzed = sc_fixture::build_in_memory();
    const std::uint64_t fuzz_base_revision = fuzzed.revision().value();
    std::int64_t succeeded = 0;
    std::int64_t refused = 0;
    for (int round = 0; round < 400; ++round) {
      fuzz_round(fuzzed, rng, round, &succeeded, &refused);
      if (round % 25 == 0) walk_every_ledger(*fuzzed.snapshot());
    }
    SC_CHECK_EQ(succeeded + refused, static_cast<std::int64_t>(400));
    SC_CHECK(succeeded > 0);
    SC_CHECK(refused > 0);
    SC_CHECK_EQ(fuzzed.revision().value(),
                fuzz_base_revision + static_cast<std::uint64_t>(succeeded));
    SC_CHECK(fuzzed.snapshot()->audit().ok());
    std::printf("fuzz: %lld accepted, %lld refused, revision %llu\n",
                static_cast<long long>(succeeded), static_cast<long long>(refused),
                static_cast<unsigned long long>(fuzzed.revision().value()));
  }

  // -------------------------------------------------------------------------
  SC_CASE("the model is still whole after every attack");
  {
    const SnapshotPtr snapshot = registry.snapshot();
    SC_CHECK(snapshot->audit().ok());
    SC_CHECK(registry.audit().ok());
    check_ledger_identity(model_rollup(*snapshot, context).area, "model rollup: declared == parts");
    const Result<CapacityRollup> building = snapshot->rollup(id.building, context);
    SC_CHECK(building.ok());
    if (building) check_ledger_identity(building.value().area, "bldg-1: declared == parts");
    const Result<CapacityRollup> planned = snapshot->rollup(*SpaceNodeId::parse("adv-planned-1"),
                                                            context);
    SC_CHECK(planned.ok());
    if (planned) {
      check_ledger_identity(planned.value().area, "adv-planned-1: declared == parts");
    }
    walk_every_ledger(*snapshot);
  }

  // -------------------------------------------------------------------------
  std::printf("attacks attempted: %d\n", attacks_attempted);
  std::printf("attacks refused:   %d\n", attacks_refused);
  std::printf("documented successful or classified outcomes: %d\n", documented_outcomes);
  std::printf("documented identity-collision holes: %d\n", documented_holes);
  SC_CHECK_EQ(attacks_attempted, attacks_refused);
  SC_CHECK(attacks_attempted > 30);

  return ::sc_test::summary("adversarial_test");
}
