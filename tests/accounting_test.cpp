// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the capacity ledger.
//
// Proves the ledger of the fixture's hall-1 exactly, square millimetre by
// square millimetre: what is declared, what is excluded, what is structurally
// consumed, what is claimed, held and earmarked, and what is therefore
// available. Every family is measured by union, never by sum, and the five
// blocker sets of this model are pairwise disjoint, so the union of them is
// computed by hand here as an independent reference.
//
// It also proves the vertical ledger of a rack in whole rack units, that a
// refused mutation leaves the revision untouched, that a planned claim is
// reported as pending and deducts nothing until it is committed, that a
// reservation stops holding only against a reading at or past its declared end,
// that a non-enforceable clearance is reported and deducted from nothing, that a
// retired node contributes nothing usable while still reporting its declared
// envelope, and that a measure which was never formed reports zero AND a state
// rather than a number.

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
// The fixture geometry, restated here so every assertion below is traceable to
// a number a reader can check by hand.
// ---------------------------------------------------------------------------

constexpr std::int64_t kHallDeclared = 600'000'000;  // 30000 x 20000
constexpr std::int64_t kRackFootprint = 720'000;     // 600 x 1200
constexpr std::int64_t kRackCount = 4;
constexpr std::int64_t kStructural = kRackCount * kRackFootprint;  // 2'880'000
constexpr std::int64_t kExclusionArea = 25'000'000;                // 5000 x 5000
constexpr std::int64_t kClearanceArea = 540'000;                   // 600 x 900
constexpr std::int64_t kExcluded = kExclusionArea + kClearanceArea;  // 25'540'000
constexpr std::int64_t kClaimArea = 1'000'000;                       // 1000 x 1000
constexpr std::int64_t kZoneArea = 4'000'000;                        // 2000 x 2000
constexpr std::int64_t kBlockedSum =
    kExclusionArea + kClearanceArea + kStructural + kClaimArea + kZoneArea;  // 33'420'000
constexpr std::int64_t kAvailable = kHallDeclared - kBlockedSum;             // 566'580'000

// The six rectangles the hall plane's ledger is formed from, one set per family.
std::vector<PlanarRect> exclusion_rects() {
  return {PlanarRect::make(20000, 10000, 5000, 5000)};
}
std::vector<PlanarRect> clearance_rects() { return {PlanarRect::make(4000, 1200, 600, 900)}; }
std::vector<PlanarRect> structural_rects() {
  std::vector<PlanarRect> rects;
  for (std::int64_t i = 0; i < kRackCount; ++i) {
    rects.push_back(PlanarRect::make(i * 2000, 0, 600, 1200));
  }
  return rects;
}
std::vector<PlanarRect> claimed_rects() { return {PlanarRect::make(0, 2000, 1000, 1000)}; }
std::vector<PlanarRect> earmarked_rects() { return {PlanarRect::make(3000, 3000, 2000, 2000)}; }

// ---------------------------------------------------------------------------
// Small request builders, so a case reads as the mutation it performs.
// ---------------------------------------------------------------------------

Result<MutationOutcome> create_planar_claim(SpaceCapacityRegistry& registry, const char* claim_id,
                                            const SpaceNodeId& node, PlanarRect rect,
                                            ClaimState state) {
  CreateClaimRequest request;
  request.claim.id = *OccupancyClaimId::parse(claim_id);
  request.claim.generation = EntityGeneration{1};
  request.claim.node = node;
  request.claim.state = state;
  request.claim.occupant = OccupantKind::asset;
  request.claim.scope.kind = FootprintScopeKind::planar;
  request.claim.scope.rects = *RectSet::build({rect});
  return registry.apply(request);
}

Result<MutationOutcome> create_unit_claim(SpaceCapacityRegistry& registry, const char* claim_id,
                                          const SpaceNodeId& node, RackUnitInterval span) {
  CreateClaimRequest request;
  request.claim.id = *OccupancyClaimId::parse(claim_id);
  request.claim.generation = EntityGeneration{1};
  request.claim.node = node;
  request.claim.state = ClaimState::committed;
  request.claim.occupant = OccupantKind::asset;
  request.claim.scope.kind = FootprintScopeKind::rack_units;
  request.claim.scope.units = *IntervalSet::build({span});
  return registry.apply(request);
}

Result<MutationOutcome> create_whole_node_claim(SpaceCapacityRegistry& registry,
                                               const char* claim_id, const SpaceNodeId& node) {
  CreateClaimRequest request;
  request.claim.id = *OccupancyClaimId::parse(claim_id);
  request.claim.generation = EntityGeneration{1};
  request.claim.node = node;
  request.claim.state = ClaimState::committed;
  request.claim.occupant = OccupantKind::rack;
  request.claim.scope.kind = FootprintScopeKind::whole_node;
  return registry.apply(request);
}

Result<MutationOutcome> transition_claim(SpaceCapacityRegistry& registry,
                                         const OccupancyClaimId& claim, ClaimState next) {
  TransitionClaimRequest request;
  request.claim = claim;
  request.next = next;
  return registry.apply(request);
}

// The ledger of one node at its own level.
AreaLedger hall_area(const Snapshot& snapshot, const Ids& id) {
  return snapshot.capacity_of(id.hall, FitContext{}).area;
}

}  // namespace

int main() {
  SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
  const Ids id = sc_fixture::ids();
  const FitContext plain{};
  const SnapshotPtr snapshot = registry.snapshot();

  SC_CASE("hall-1: declared, excluded, usable, structural, claimed, held, earmarked, available");
  {
    const AreaLedger area = hall_area(*snapshot, id);

    // Declared: what hall-1 states it built. 30000 mm x 20000 mm.
    SC_CHECK_EQ(area.declared.value(), kHallDeclared);

    // Excluded: the active thermal exclusion, plus the ENFORCEABLE clearance
    // band in front of rack-03. The band is expressed in the plane of the
    // nearest plane-owning ancestor of the rack it protects, which is hall-1,
    // so it is subtracted from the hall floor: 25,000,000 + 540,000.
    SC_CHECK_EQ(area.excluded.value(), kExcluded);
    SC_CHECK_EQ(area.excluded.value(), 25'540'000);

    // Usable: declared less excluded. 600,000,000 - 25,540,000.
    SC_CHECK_EQ(area.usable.value(), kHallDeclared - kExcluded);
    SC_CHECK_EQ(area.usable.value(), 574'460'000);

    // Structural: the four rack placements standing on the hall floor.
    SC_CHECK_EQ(area.structural.value(), kStructural);
    SC_CHECK_EQ(area.structural.value(), 2'880'000);

    // Claimed: the committed planar claim. The band under rack-01 is a node
    // placement, not a claim, and is reported as structural instead.
    SC_CHECK_EQ(area.claimed.value(), kClaimArea);

    // Held: zero on the PLANE. hold-1 is a rack-unit reservation on rack-02, so
    // it is subtracted from that rack's vertical envelope and from nothing here.
    SC_CHECK_EQ(area.held.value(), 0);

    // Earmarked: the funded expansion zone.
    SC_CHECK_EQ(area.earmarked.value(), kZoneArea);

    // Available: declared less the union of every blocker. See the next case
    // for the union itself.
    SC_CHECK_EQ(area.available.value(), kAvailable);
    SC_CHECK_EQ(area.available.value(), 566'580'000);

    // Nothing is merely intended on this model, and no envelope is planned.
    SC_CHECK_EQ(area.pending.value(), 0);
    SC_CHECK_EQ(area.planned.value(), 0);

    // Every measure was formed from declared input, and nothing is
    // over-subscribed.
    SC_CHECK(area.state == MeasureState::known);
    SC_CHECK(!area.over_committed);

    // The ledger reconstructs exactly: what is left plus everything taken or
    // set aside is what was declared.
    SC_CHECK_EQ(area.excluded.value() + area.structural.value() + area.claimed.value() +
                    area.held.value() + area.earmarked.value() + area.available.value(),
                area.declared.value());

    // report() of a node whose subtree holds exactly one plane is that plane's
    // own ledger, field for field.
    const Result<CapacityReport> report = registry.report(id.hall, plain);
    SC_CHECK(report.ok());
    SC_CHECK(report.value().area == area);
    SC_CHECK(report.value().owns_plane);
    SC_CHECK(report.value().attributed_plane == id.hall);
    SC_CHECK(report.value().explanations.contains(ReasonCode::rolled_up));
  }

  SC_CASE("the five blocker sets are pairwise disjoint, so their union is their sum");
  {
    const std::vector<std::vector<PlanarRect>> families = {
        exclusion_rects(), clearance_rects(), structural_rects(), claimed_rects(),
        earmarked_rects()};

    // Pairwise disjoint: no rectangle of one family meets a rectangle of
    // another, so the measure of the union is the sum of the measures.
    std::int64_t hand_sum = 0;
    for (std::size_t a = 0; a < families.size(); ++a) {
      for (const PlanarRect& rect : families[a]) {
        const Checked<SquareMillimeters> measured = rect.area();
        SC_CHECK(measured.ok());
        hand_sum += measured.value.value();
      }
      for (std::size_t b = a + 1; b < families.size(); ++b) {
        for (const PlanarRect& left : families[a]) {
          for (const PlanarRect& right : families[b]) {
            SC_CHECK(!rect_intersects(left, right));
          }
        }
      }
    }
    SC_CHECK_EQ(hand_sum, kBlockedSum);
    SC_CHECK_EQ(hand_sum, 33'420'000);

    // The library's own union kernel agrees with the hand sum.
    std::vector<PlanarRect> all;
    for (const std::vector<PlanarRect>& family : families) {
      all.insert(all.end(), family.begin(), family.end());
    }
    const Checked<SquareMillimeters> measured = rect_union_area(all);
    SC_CHECK(measured.ok());
    SC_CHECK_EQ(measured.value.value(), hand_sum);

    // And the ledger's available figure is exactly declared less that union.
    const AreaLedger area = hall_area(*snapshot, id);
    SC_CHECK_EQ(area.available.value(), kHallDeclared - hand_sum);
  }

  SC_CASE("rack-01: the vertical ledger, in whole rack units");
  {
    const UnitLedger units = snapshot->capacity_of(id.rack1, plain).units;

    SC_CHECK_EQ(units.declared.value(), 42);
    SC_CHECK_EQ(units.excluded.value(), 0);
    SC_CHECK_EQ(units.usable.value(), 42);

    // Occupied: band-a covers [1,5) = 4 units and claim-units-1 covers
    // [10,16) = 6 units. The two are disjoint, so the union is 10.
    const IntervalSet band = *IntervalSet::build({RackUnitInterval::of_count(1, 4)});
    const IntervalSet claim = *IntervalSet::build({RackUnitInterval::of_count(10, 6)});
    const Checked<RackUnits> band_units = band.total();
    const Checked<RackUnits> claim_units = claim.total();
    SC_CHECK(band_units.ok());
    SC_CHECK(claim_units.ok());
    SC_CHECK_EQ(band_units.value.value(), 4);
    SC_CHECK_EQ(claim_units.value.value(), 6);
    SC_CHECK_EQ(band_units.value.value() + claim_units.value.value(), 10);
    SC_CHECK_EQ(units.occupied.value(), 10);

    SC_CHECK_EQ(units.available.value(), 32);

    // The free space is the complement of {[1,5), [10,16)} inside [1,43):
    // [5,10) and [16,43). That is two runs, and the larger is 27 units.
    const Result<IntervalSet> blocked = IntervalSet::build(
        {RackUnitInterval::of_count(1, 4), RackUnitInterval::of_count(10, 6)});
    SC_CHECK(blocked.ok());
    const Result<IntervalSet> free_space = IntervalSet::complement(blocked.value(), 42);
    SC_CHECK(free_space.ok());
    SC_CHECK_EQ(free_space.value().size(), static_cast<std::size_t>(2));
    SC_CHECK(free_space.value().intervals().at(0) == RackUnitInterval::make(5, 10));
    SC_CHECK(free_space.value().intervals().at(1) == RackUnitInterval::make(16, 43));
    const RackUnitInterval largest = free_space.value().intervals().at(1);
    SC_CHECK_EQ(largest.count(), 27);

    SC_CHECK_EQ(units.free_runs, 2u);
    SC_CHECK_EQ(units.largest_free_run.value(), largest.count());
    SC_CHECK_EQ(units.largest_free_run.value(), 27);

    // Fragmentation: (available - largest) * 1,000,000 / available, integer
    // division, remainder discarded. (32 - 27) * 1,000,000 / 32 = 156,250.
    SC_CHECK_EQ(units.fragmentation_ppm, 156'250u);
    SC_CHECK_EQ((units.available.value() - units.largest_free_run.value()) * 1'000'000 /
                    units.available.value(),
                156'250);

    SC_CHECK(units.state == MeasureState::known);
    SC_CHECK(!units.over_committed);
    SC_CHECK_EQ(units.excluded.value() + units.occupied.value() + units.available.value(),
                units.declared.value());
  }

  SC_CASE("the other three racks");
  {
    // rack-02: hold-1 reserves [1,9) = 8 units.
    const UnitLedger rack2 = snapshot->capacity_of(id.rack2, plain).units;
    SC_CHECK_EQ(rack2.declared.value(), 42);
    SC_CHECK_EQ(rack2.occupied.value(), 8);
    SC_CHECK_EQ(rack2.available.value(), 34);

    // rack-03 and rack-04 carry nothing vertical: the clearance in front of
    // rack-03 is planar and is subtracted from the hall floor, not from units.
    const UnitLedger rack3 = snapshot->capacity_of(id.rack3, plain).units;
    SC_CHECK_EQ(rack3.occupied.value(), 0);
    SC_CHECK_EQ(rack3.available.value(), 42);
    SC_CHECK_EQ(rack3.free_runs, 1u);
    SC_CHECK_EQ(rack3.fragmentation_ppm, 0u);

    const UnitLedger rack4 = snapshot->capacity_of(id.rack4, plain).units;
    SC_CHECK_EQ(rack4.occupied.value(), 0);
    SC_CHECK_EQ(rack4.available.value(), 42);

    // A rack owns a vertical envelope and no plane: its area measure is
    // unknown, not zero-by-measurement.
    SC_CHECK(snapshot->capacity_of(id.rack1, plain).area.state == MeasureState::unknown);
    SC_CHECK(!snapshot->capacity_of(id.rack1, plain).owns_plane);
    SC_CHECK(snapshot->capacity_of(id.rack1, plain).owns_rack_envelope);
  }

  SC_CASE("refusals: overlap, containment, envelope bounds, and whole-node uniqueness");
  {
    // A rack unit claim that overlaps claim-units-1 [10,16).
    const RegistryRevision before = registry.revision();
    const Result<MutationOutcome> overlapping = create_unit_claim(
        registry, "units-overlap-1", id.rack1, RackUnitInterval::of_count(12, 2));
    SC_CHECK(!overlapping);
    SC_CHECK(overlapping.code() == ErrorCode::overlap);
    SC_CHECK_EQ(registry.revision().value(), before.value());

    // A planar claim that overlaps a rack placement: (0,0) 600x1200 is
    // rack-01's own footprint on the hall floor.
    const Result<MutationOutcome> on_a_rack =
        create_planar_claim(registry, "planar-over-rack-1", id.hall,
                            PlanarRect::make(0, 0, 600, 1200), ClaimState::committed);
    SC_CHECK(!on_a_rack);
    SC_CHECK(on_a_rack.code() == ErrorCode::overlap);
    SC_CHECK_EQ(registry.revision().value(), before.value());

    // A planar claim that is not inside the plane's declared rectangles: the
    // hall declares exactly (0,0) 30000x20000, so a rectangle reaching past the
    // far corner cannot be contained.
    const Result<MutationOutcome> outside =
        create_planar_claim(registry, "planar-outside-1", id.hall,
                            PlanarRect::make(29000, 19000, 2000, 2000), ClaimState::committed);
    SC_CHECK(!outside);
    SC_CHECK(outside.code() == ErrorCode::extent_out_of_envelope);
    SC_CHECK_EQ(registry.revision().value(), before.value());

    // A rack unit claim whose span exceeds the envelope [1,43): [41,44) covers
    // unit 43, which the 42-unit envelope does not have.
    const Result<MutationOutcome> too_tall =
        create_unit_claim(registry, "units-too-tall-1", id.rack1,
                          RackUnitInterval::of_count(41, 3));
    SC_CHECK(!too_tall);
    SC_CHECK(too_tall.code() == ErrorCode::unit_envelope_exceeded);
    SC_CHECK_EQ(registry.revision().value(), before.value());

    // A rack unit claim on a node that has no rack envelope in its chain.
    const Result<MutationOutcome> no_envelope =
        create_unit_claim(registry, "units-no-envelope-1", id.hall,
                          RackUnitInterval::of_count(1, 1));
    SC_CHECK(!no_envelope);
    SC_CHECK(no_envelope.code() == ErrorCode::unit_envelope_exceeded);
    SC_CHECK_EQ(registry.revision().value(), before.value());
  }

  SC_CASE("a whole-node claim covers the envelope it names, and only one may");
  {
    // A rack that stands on a plane already consumes its footprint there, so a
    // whole-node claim on a PLACED rack collides with the plane's structural
    // occupancy. The case below therefore uses a rack that is declared but not
    // yet placed on any floor: it is placed directly under the site, which
    // declares no plane, so its whole-node claim is measured in its own
    // vertical envelope and nowhere else.
    SpaceNode rack5;
    rack5.id = *SpaceNodeId::parse("rack-05");
    rack5.generation = EntityGeneration{1};
    rack5.kind = SpaceNodeKind::rack;
    rack5.spatial_class = SpatialClass::rack;
    rack5.lifecycle = NodeLifecycle::available;
    rack5.label = *DisplayLabel::parse("Rack 5");
    rack5.parent = id.site;
    rack5.depth = 1;
    CreateNodeRequest create;
    create.node = rack5;
    const Result<MutationOutcome> created = registry.apply(create);
    SC_CHECK(created.ok());
    SC_CHECK(created.value().applied);
    SC_CHECK(created.code() == ErrorCode::ok);

    SetNodeEnvelopeRequest envelope_request;
    envelope_request.node = rack5.id;
    RackEnvelope envelope;
    envelope.height = RackUnits{42};
    envelope_request.rack = envelope;
    const Result<MutationOutcome> given = registry.apply(envelope_request);
    SC_CHECK(given.ok());

    // The whole-node claim on a rack that stands on a plane is refused: the
    // plane's occupancy already covers the footprint it would take.
    const Result<MutationOutcome> on_placed_rack =
        create_whole_node_claim(registry, "whole-placed-1", id.rack1);
    SC_CHECK(!on_placed_rack);
    SC_CHECK(on_placed_rack.code() == ErrorCode::overlap);

    // The first whole-node claim on the unplaced rack is accepted and consumes
    // the entire declared envelope.
    const Result<MutationOutcome> first = create_whole_node_claim(registry, "whole-1", rack5.id);
    SC_CHECK(first.ok());
    const SnapshotPtr after_first = registry.snapshot();
    const UnitLedger whole = after_first->capacity_of(rack5.id, plain).units;
    SC_CHECK_EQ(whole.occupied.value(), 42);
    SC_CHECK_EQ(whole.available.value(), 0);
    SC_CHECK(!whole.over_committed);

    // A second whole-node claim on the same envelope is refused, and the
    // revision does not move.
    const RegistryRevision before = registry.revision();
    const Result<MutationOutcome> second = create_whole_node_claim(registry, "whole-2", rack5.id);
    SC_CHECK(!second);
    SC_CHECK(second.code() == ErrorCode::overlap);
    SC_CHECK_EQ(registry.revision().value(), before.value());
  }

  SC_CASE("zero is not unknown: a grouping node reports a state, not a number");
  {
    // row-1 declares no plane of its own, so its area was never formed. It
    // reports zero AND `unknown`, and it names the plane its area is accounted
    // in rather than reporting a number nobody measured.
    const NodeCapacity row = snapshot->capacity_of(id.row1, plain);
    SC_CHECK(row.area.state == MeasureState::unknown);
    SC_CHECK(!row.owns_plane);
    SC_CHECK_EQ(row.area.declared.value(), 0);
    SC_CHECK_EQ(row.area.excluded.value(), 0);
    SC_CHECK_EQ(row.area.usable.value(), 0);
    SC_CHECK_EQ(row.area.structural.value(), 0);
    SC_CHECK_EQ(row.area.claimed.value(), 0);
    SC_CHECK_EQ(row.area.held.value(), 0);
    SC_CHECK_EQ(row.area.earmarked.value(), 0);
    SC_CHECK_EQ(row.area.available.value(), 0);
    SC_CHECK_EQ(row.area.pending.value(), 0);
    SC_CHECK_EQ(row.area.planned.value(), 0);
    SC_CHECK(row.attributed_plane == id.hall);
    SC_CHECK_EQ(row.node.str(), std::string("row-1"));
    SC_CHECK_EQ(row.generation.value(), 1ull);

    // The same holds of the vertical measure of a node that declares none, and
    // of a site that declares no plane.
    SC_CHECK(row.units.state == MeasureState::unknown);
    SC_CHECK_EQ(row.units.declared.value(), 0);
    SC_CHECK(snapshot->capacity_of(id.site, plain).area.state == MeasureState::unknown);
    SC_CHECK(snapshot->capacity_of(id.band, plain).units.state == MeasureState::unknown);

    // A node that does declare an envelope has a known measure, so the zero of
    // a grouping node can never be confused with a measured zero.
    SC_CHECK(snapshot->capacity_of(id.rack1, plain).units.state == MeasureState::known);
    SC_CHECK(snapshot->capacity_of(id.hall, plain).area.state == MeasureState::known);
  }

  SC_CASE("a planned claim is pending, then committed in two visible steps");
  {
    // Disjoint from every blocker of the hall plane, so nothing refuses it and
    // nothing else moves.
    const Result<MutationOutcome> planned =
        create_planar_claim(registry, "plan-1", id.hall, PlanarRect::make(0, 5000, 1000, 1000),
                            ClaimState::planned);
    SC_CHECK(planned.ok());

    const AreaLedger pending_area = hall_area(*registry.snapshot(), id);
    SC_CHECK_EQ(pending_area.pending.value(), 1'000'000);
    SC_CHECK_EQ(pending_area.available.value(), kAvailable);  // deducted from nothing
    SC_CHECK_EQ(pending_area.claimed.value(), kClaimArea);    // the committed claim only

    // A planned claim cannot jump straight to committed: space is taken in two
    // visible steps, and the state machine refuses the shortcut.
    const Result<MutationOutcome> shortcut =
        transition_claim(registry, *OccupancyClaimId::parse("plan-1"), ClaimState::committed);
    SC_CHECK(!shortcut);
    SC_CHECK(shortcut.code() == ErrorCode::claim_transition_illegal);
    SC_CHECK_EQ(hall_area(*registry.snapshot(), id).pending.value(), 1'000'000);

    // Submitted is still pending: still reported, still deducted from nothing.
    const Result<MutationOutcome> submitted =
        transition_claim(registry, *OccupancyClaimId::parse("plan-1"), ClaimState::submitted);
    SC_CHECK(submitted.ok());
    const AreaLedger submitted_area = hall_area(*registry.snapshot(), id);
    SC_CHECK_EQ(submitted_area.pending.value(), 1'000'000);
    SC_CHECK_EQ(submitted_area.available.value(), kAvailable);

    // Committed: the claim now consumes, pending clears, and available falls by
    // exactly its area.
    const Result<MutationOutcome> committed =
        transition_claim(registry, *OccupancyClaimId::parse("plan-1"), ClaimState::committed);
    SC_CHECK(committed.ok());
    const AreaLedger committed_area = hall_area(*registry.snapshot(), id);
    SC_CHECK_EQ(committed_area.pending.value(), 0);
    SC_CHECK_EQ(committed_area.claimed.value(), kClaimArea + 1'000'000);
    SC_CHECK_EQ(committed_area.available.value(), kAvailable - 1'000'000);
    SC_CHECK_EQ(committed_area.available.value(), 565'580'000);
  }

  SC_CASE("count_pending makes a planned claim deduct from available");
  {
    // A second planned claim, again clear of every blocker.
    const Result<MutationOutcome> planned =
        create_planar_claim(registry, "plan-2", id.hall, PlanarRect::make(0, 8000, 1000, 1000),
                            ClaimState::planned);
    SC_CHECK(planned.ok());

    const AreaLedger plain_area = hall_area(*registry.snapshot(), id);
    SC_CHECK_EQ(plain_area.pending.value(), 1'000'000);  // plan-2 alone: plan-1 is committed
    const std::int64_t without_pending = plain_area.available.value();

    // With count_pending the same state answers the question "what would be
    // free if this were committed", and available falls by the planned area.
    FitContext count_pending{};
    count_pending.count_pending = true;
    const AreaLedger counted = registry.snapshot()->capacity_of(id.hall, count_pending).area;
    SC_CHECK_EQ(counted.available.value(), without_pending - 1'000'000);
    SC_CHECK_EQ(counted.claimed.value(), 3'000'000);  // committed plus both planned
    SC_CHECK_EQ(counted.pending.value(), 0);
    SC_CHECK(counted.available.value() < without_pending);

    // The reading is per query: the default context still reports the space as
    // available, because nothing has committed it.
    SC_CHECK_EQ(hall_area(*registry.snapshot(), id).available.value(), without_pending);
  }

  SC_CASE("a reservation holds only until its declared end, against the caller's reading");
  {
    CreateReservationRequest request;
    request.reservation.id = *OccupancyClaimId::parse("hold-timed-1");
    request.reservation.generation = EntityGeneration{1};
    request.reservation.node = id.rack3;
    request.reservation.state = ReservationState::held;
    request.reservation.reservation =
        ReservationRef::make("res-0002", 1, UpstreamState::active).value();
    request.reservation.holder_label = *DisplayLabel::parse("tenant c");
    request.reservation.scope.kind = FootprintScopeKind::rack_units;
    request.reservation.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 5)});
    request.reservation.not_after = Timestamp{1'000'000};
    const Result<MutationOutcome> held = registry.apply(request);
    SC_CHECK(held.ok());

    const SnapshotPtr held_snapshot = registry.snapshot();
    const FitContext no_reading{};
    FitContext before_end{};
    before_end.now = Timestamp{999'999};
    FitContext at_end{};
    at_end.now = Timestamp{1'000'000};
    FitContext past_end{};
    past_end.now = Timestamp{1'000'001};

    // Without a reading no reservation is treated as expired, so the hold is in
    // force. So it is one millisecond before the declared end.
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, no_reading).units.occupied.value(), 5);
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, no_reading).units.available.value(), 37);
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, before_end).units.available.value(), 37);

    // At or past the declared end the hold stops holding, and the space is free
    // again.
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, at_end).units.occupied.value(), 0);
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, at_end).units.available.value(), 42);
    SC_CHECK_EQ(held_snapshot->capacity_of(id.rack3, past_end).units.available.value(), 42);

    // The stored record is untouched by a reading: the same state gives the
    // same answer at the same reading, and the state itself is still `held`.
    const FootprintReservation* stored =
        held_snapshot->find_reservation(*OccupancyClaimId::parse("hold-timed-1"));
    SC_CHECK(stored != nullptr);
    SC_CHECK(stored->state == ReservationState::held);
    SC_CHECK(stored->not_after.has_value());
    SC_CHECK_EQ(stored->not_after->unix_millis, 1'000'000);
  }

  SC_CASE("a non-enforceable clearance is reported and deducted from nothing");
  {
    const AreaLedger before = hall_area(*registry.snapshot(), id);
    const UnitLedger rack_before = registry.snapshot()->capacity_of(id.rack4, plain).units;

    CreateClearanceRequest request;
    request.constraint.id = *ClearanceConstraintId::parse("clr-soft-1");
    request.constraint.generation = EntityGeneration{1};
    request.constraint.node = id.rack4;
    request.constraint.kind = ClearanceKind::front_service;
    request.constraint.has_band = true;
    request.constraint.band = PlanarRect::make(6000, 1200, 600, 900);
    request.constraint.enforceable = false;
    const Result<MutationOutcome> applied = registry.apply(request);
    SC_CHECK(applied.ok());

    const SnapshotPtr after = registry.snapshot();
    const AreaLedger now = after->capacity_of(id.hall, plain).area;
    SC_CHECK_EQ(now.available.value(), before.available.value());
    SC_CHECK_EQ(now.excluded.value(), before.excluded.value());
    SC_CHECK_EQ(now.usable.value(), before.usable.value());
    SC_CHECK_EQ(after->capacity_of(id.rack4, plain).units.available.value(),
                rack_before.available.value());

    // It is not silently ignored either: a planar search on that plane says so.
    PlanarRectFitRequest fit;
    fit.node = id.hall;
    fit.width = Millimeters{600};
    fit.depth = Millimeters{1200};
    const Result<FitAssessment> assessment = registry.assess(fit);
    SC_CHECK(assessment.ok());
    SC_CHECK(assessment.value().explanations.contains(ReasonCode::clearance_not_enforceable));

    // The stored record still says it was not enforceable, so nothing was
    // silently upgraded to a subtraction.
    const ClearanceConstraint* stored =
        after->find_clearance(*ClearanceConstraintId::parse("clr-soft-1"));
    SC_CHECK(stored != nullptr);
    SC_CHECK(!stored->enforceable);
  }

  SC_CASE("a retired node contributes nothing usable and says so");
  {
    SetNodeLifecycleRequest request;
    request.node = id.hall;
    request.lifecycle = NodeLifecycle::retired;
    const Result<MutationOutcome> retired = registry.apply(request);
    SC_CHECK(retired.ok());

    const SnapshotPtr after = registry.snapshot();
    const AreaLedger area = after->capacity_of(id.hall, plain).area;

    // The declared envelope is still reported: retirement is an accounting
    // event, not a deletion.
    SC_CHECK_EQ(area.declared.value(), kHallDeclared);
    SC_CHECK_EQ(area.excluded.value(), kHallDeclared);
    SC_CHECK_EQ(area.usable.value(), 0);
    SC_CHECK_EQ(area.available.value(), 0);
    SC_CHECK_EQ(area.structural.value(), 0);
    SC_CHECK_EQ(area.claimed.value(), 0);
    SC_CHECK_EQ(area.earmarked.value(), 0);
    SC_CHECK(!area.over_committed);
    SC_CHECK(area.state == MeasureState::known);

    // The report of the retired node carries the explanation, and the state is
    // still readable rather than refused.
    const Result<CapacityReport> report = registry.report(id.hall, plain);
    SC_CHECK(report.ok());
    SC_CHECK(report.value().explanations.contains(ReasonCode::node_retired));
    SC_CHECK(report.value().lifecycle == NodeLifecycle::retired);
    SC_CHECK_EQ(report.value().area.available.value(), 0);

    // Nothing usable is left in the subtree, and the rollup says the subtree
    // contains an inactive node.
    const Result<CapacityRollup> rolled = registry.rollup(id.hall, plain);
    SC_CHECK(rolled.ok());
    SC_CHECK(rolled.value().contains_inactive_nodes);
    SC_CHECK_EQ(rolled.value().area.available.value(), 0);
  }

  return ::sc_test::summary("accounting");
}
