// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - claims, reservations, exclusions, clearances and expansion.
//
// What this file proves:
//   * the claim predicates partition the six claim states exactly: consuming,
//     pending and terminal are pairwise disjoint and cover every state;
//   * claim, reservation, exclusion and expansion transitions are exactly the
//     documented tables, with no self transitions and with the documented
//     terminal states;
//   * a reservation in force holds space, and evaluating it against a caller
//     supplied reading expires it at its declared end and never moves a
//     terminal state;
//   * only an active exclusion blocks, and every default occupant mask is legal
//     and exactly the documented bit pattern;
//   * a footprint scope validates only in the form its kind names, and measures
//     only in the units its kind names;
//   * a hand-built valid record of each family validates, and each single broken
//     field is refused with its documented error code;
//   * the five record equality operators are reflexive and sensitive to every
//     field.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/units.hpp"

#include "fixture.hpp"
#include "test_support.hpp"

using namespace dccp::space_capacity;

namespace {

void expect_status(const Status& status, ErrorCode expected) {
  SC_CHECK(!status.ok());
  SC_CHECK_EQ(status.code(), expected);
}

// ---------------------------------------------------------------------------
// Hand-built valid records, using the shared fixture's identities.
// ---------------------------------------------------------------------------

[[nodiscard]] OccupancyClaim good_claim() {
  const sc_fixture::Ids id = sc_fixture::ids();
  OccupancyClaim claim;
  claim.id = id.planar_claim;
  claim.generation = EntityGeneration{1};
  claim.node = id.hall;
  claim.state = ClaimState::committed;
  claim.occupant = OccupantKind::infrastructure;
  claim.label = *DisplayLabel::parse("patch panel");
  claim.scope.kind = FootprintScopeKind::planar;
  claim.scope.rects = *RectSet::build({PlanarRect::make(0, 2000, 1000, 1000)});
  return claim;
}

[[nodiscard]] FootprintReservation good_reservation() {
  const sc_fixture::Ids id = sc_fixture::ids();
  FootprintReservation reservation;
  reservation.id = id.reservation;
  reservation.generation = EntityGeneration{1};
  reservation.node = id.rack2;
  reservation.state = ReservationState::held;
  reservation.reservation = ReservationRef::make("res-0001", 4, UpstreamState::active).value();
  reservation.holder_label = *DisplayLabel::parse("tenant b");
  reservation.scope.kind = FootprintScopeKind::rack_units;
  reservation.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 8)});
  return reservation;
}

[[nodiscard]] ExclusionRegion good_exclusion() {
  const sc_fixture::Ids id = sc_fixture::ids();
  ExclusionRegion region;
  region.id = id.exclusion;
  region.generation = EntityGeneration{1};
  region.node = id.hall;
  region.reason = ExclusionReason::thermal;
  region.state = ExclusionState::active;
  region.blocks = default_mask_for_reason(ExclusionReason::thermal);
  region.scope.kind = FootprintScopeKind::planar;
  region.scope.rects = *RectSet::build({PlanarRect::make(20000, 10000, 5000, 5000)});
  return region;
}

[[nodiscard]] ClearanceConstraint good_clearance() {
  const sc_fixture::Ids id = sc_fixture::ids();
  ClearanceConstraint constraint;
  constraint.id = id.clearance;
  constraint.generation = EntityGeneration{1};
  constraint.node = id.rack3;
  constraint.kind = ClearanceKind::front_service;
  constraint.has_band = true;
  constraint.band = PlanarRect::make(4000, sc_fixture::kRackDepth, sc_fixture::kRackWidth, 900);
  constraint.enforceable = true;
  return constraint;
}

[[nodiscard]] ClearanceConstraint good_vertical_clearance() {
  const sc_fixture::Ids id = sc_fixture::ids();
  ClearanceConstraint constraint;
  constraint.id = id.clearance;
  constraint.generation = EntityGeneration{1};
  constraint.node = id.rack1;
  constraint.kind = ClearanceKind::rack_unit_access;
  constraint.has_units = true;
  constraint.required_free_units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
  constraint.enforceable = true;
  return constraint;
}

[[nodiscard]] ExpansionZone good_zone() {
  const sc_fixture::Ids id = sc_fixture::ids();
  ExpansionZone zone;
  zone.id = id.zone;
  zone.generation = EntityGeneration{1};
  zone.node = id.hall;
  zone.state = ExpansionState::funded;
  zone.scope.kind = FootprintScopeKind::planar;
  zone.scope.rects = *RectSet::build({PlanarRect::make(3000, 3000, 2000, 2000)});
  return zone;
}

// ---------------------------------------------------------------------------
// Claim lifecycle
// ---------------------------------------------------------------------------

void claim_state_predicates() {
  SC_CASE("claim state predicates partition the six states");
  struct ClaimCase final {
    ClaimState state;
    bool consumes;
    bool pending;
    bool terminal;
  };
  const ClaimCase table[] = {
      {ClaimState::planned, false, true, false},
      {ClaimState::submitted, false, true, false},
      {ClaimState::committed, true, false, false},
      {ClaimState::releasing, true, false, false},
      {ClaimState::released, false, false, true},
      {ClaimState::retired, false, false, true},
  };
  for (const ClaimCase& item : table) {
    SC_CHECK_EQ(claim_state_consumes(item.state), item.consumes);
    SC_CHECK_EQ(claim_state_is_pending(item.state), item.pending);
    SC_CHECK_EQ(claim_state_is_terminal(item.state), item.terminal);
    // The three predicates are pairwise disjoint and cover every state.
    const int true_count =
        (item.consumes ? 1 : 0) + (item.pending ? 1 : 0) + (item.terminal ? 1 : 0);
    SC_CHECK_EQ(true_count, 1);
  }
  SC_CHECK(OccupancyClaim{}.consumes() == false);
  OccupancyClaim committed;
  committed.state = ClaimState::committed;
  SC_CHECK(committed.consumes());
  SC_CHECK(!committed.is_terminal());
}

void claim_transition_table() {
  SC_CASE("claim_transition_allowed matches the documented table");
  const ClaimState order[] = {ClaimState::planned,    ClaimState::submitted,
                              ClaimState::committed,  ClaimState::releasing,
                              ClaimState::released,   ClaimState::retired};
  const bool expected[6][6] = {
      /* planned   */ {false, true, false, false, true, true},
      /* submitted */ {true, false, true, false, true, true},
      /* committed */ {false, false, false, true, true, false},
      /* releasing */ {false, false, true, false, true, false},
      /* released  */ {false, false, false, false, false, false},
      /* retired   */ {false, false, false, false, false, false},
  };
  for (std::size_t from = 0; from < 6; ++from) {
    for (std::size_t to = 0; to < 6; ++to) {
      SC_CHECK_EQ(claim_transition_allowed(order[from], order[to]), expected[from][to]);
    }
  }
  for (const ClaimState state : order) {
    SC_CHECK(!claim_transition_allowed(state, state));  // no self transitions
    SC_CHECK(!claim_transition_allowed(ClaimState::released, state));
    SC_CHECK(!claim_transition_allowed(ClaimState::retired, state));
  }
  // A backwards move out of a terminal state is refused as an illegal
  // transition, not silently accepted.
  const OccupancyClaim released_claim = [] {
    OccupancyClaim claim = good_claim();
    claim.state = ClaimState::released;
    return claim;
  }();
  expect_status(validate_claim_transition(released_claim, ClaimState::planned),
                ErrorCode::claim_transition_illegal);
  expect_status(validate_claim_transition(released_claim, ClaimState::released),
                ErrorCode::claim_transition_illegal);
  OccupancyClaim committed_claim = good_claim();
  SC_CHECK(validate_claim_transition(committed_claim, ClaimState::releasing).ok());
}

// ---------------------------------------------------------------------------
// Reservation lifecycle
// ---------------------------------------------------------------------------

void reservation_transitions() {
  SC_CASE("reservation transitions and evaluation");
  const ReservationState order[] = {ReservationState::held, ReservationState::partially_claimed,
                                    ReservationState::released, ReservationState::expired,
                                    ReservationState::revoked};
  struct HoldCase final {
    ReservationState state;
    bool holds;
  };
  const HoldCase holds[] = {
      {ReservationState::held, true},
      {ReservationState::partially_claimed, true},
      {ReservationState::released, false},
      {ReservationState::expired, false},
      {ReservationState::revoked, false},
  };
  for (const HoldCase& item : holds) {
    SC_CHECK_EQ(reservation_state_holds(item.state), item.holds);
  }
  SC_CHECK(reservation_state_is_terminal(ReservationState::released));
  SC_CHECK(reservation_state_is_terminal(ReservationState::expired));
  SC_CHECK(reservation_state_is_terminal(ReservationState::revoked));
  SC_CHECK(!reservation_state_is_terminal(ReservationState::held));
  SC_CHECK(!reservation_state_is_terminal(ReservationState::partially_claimed));

  const bool expected[5][5] = {
      /* held              */ {false, true, true, true, true},
      /* partially_claimed */ {true, false, true, true, true},
      /* released          */ {false, false, false, false, false},
      /* expired           */ {true, false, false, false, false},
      /* revoked           */ {false, false, false, false, false},
  };
  for (std::size_t from = 0; from < 5; ++from) {
    for (std::size_t to = 0; to < 5; ++to) {
      SC_CHECK_EQ(reservation_transition_allowed(order[from], order[to]), expected[from][to]);
    }
  }
  for (const ReservationState state : order) {
    SC_CHECK(!reservation_transition_allowed(state, state));
  }

  // Evaluation against a caller supplied reading.
  FootprintReservation reservation = good_reservation();
  reservation.not_after = Timestamp{5000};
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{0}), ReservationState::held);
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{4999}), ReservationState::held);
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{5000}), ReservationState::expired);
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{5001}), ReservationState::expired);
  reservation.state = ReservationState::partially_claimed;
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{4999}),
              ReservationState::partially_claimed);
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{5000}), ReservationState::expired);
  // A terminal state is never moved by a reading.
  reservation.state = ReservationState::released;
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{60000}), ReservationState::released);
  reservation.state = ReservationState::expired;
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{60000}), ReservationState::expired);
  reservation.state = ReservationState::revoked;
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{60000}), ReservationState::revoked);
  // No declared end: the stored state, whatever the reading.
  reservation.state = ReservationState::held;
  reservation.not_after.reset();
  SC_CHECK_EQ(reservation_state_at(reservation, Timestamp{60000}), ReservationState::held);
}

// ---------------------------------------------------------------------------
// Exclusions
// ---------------------------------------------------------------------------

void exclusion_states_and_masks() {
  SC_CASE("exclusion state, transitions and default masks");
  struct ExclusionCase final {
    ExclusionState state;
    bool blocks;
  };
  const ExclusionCase states[] = {
      {ExclusionState::draft, false},
      {ExclusionState::active, true},
      {ExclusionState::suspended, false},
      {ExclusionState::retired, false},
  };
  for (const ExclusionCase& item : states) {
    SC_CHECK_EQ(exclusion_state_blocks(item.state), item.blocks);
  }
  SC_CHECK(exclusion_state_blocks(ExclusionState::active));
  SC_CHECK(!exclusion_state_blocks(ExclusionState::draft));

  const ExclusionState order[] = {ExclusionState::draft, ExclusionState::active,
                                  ExclusionState::suspended, ExclusionState::retired};
  const bool expected[4][4] = {
      /* draft     */ {false, true, false, true},
      /* active    */ {false, false, true, true},
      /* suspended */ {false, true, false, true},
      /* retired   */ {false, false, false, false},
  };
  for (std::size_t from = 0; from < 4; ++from) {
    for (std::size_t to = 0; to < 4; ++to) {
      SC_CHECK_EQ(exclusion_transition_allowed(order[from], order[to]), expected[from][to]);
    }
  }
  for (const ExclusionState state : order) {
    SC_CHECK(!exclusion_transition_allowed(state, state));
    SC_CHECK(!exclusion_transition_allowed(ExclusionState::retired, state));
  }

  // The default mask of every reason, against an exact expected bit pattern.
  // Bit positions are OccupantKind values: unknown 0, rack 1, asset 2,
  // workload 3, infrastructure 4, service_access 5.
  struct ReasonCase final {
    ExclusionReason reason;
    std::uint32_t bits;
  };
  const ReasonCase reasons[] = {
      {ExclusionReason::unspecified, 0x00u},
      {ExclusionReason::structural, 0x3Fu},
      {ExclusionReason::thermal, 0x1Fu},
      {ExclusionReason::electrical, 0x1Fu},
      {ExclusionReason::regulatory, 0x3Fu},
      {ExclusionReason::service_clearance, 0x20u},
      {ExclusionReason::incompatible_equipment, 0x1Fu},
      {ExclusionReason::reserved_expansion, 0x1Eu},
      {ExclusionReason::decommissioned, 0x3Fu},
      {ExclusionReason::safety, 0x3Fu},
      {ExclusionReason::operational, 0x1Fu},
  };
  for (const ReasonCase& item : reasons) {
    const OccupantMask mask = default_mask_for_reason(item.reason);
    SC_CHECK_EQ(mask.bits(), item.bits);
    // Every mask the table returns is a mask this version accepts.
    SC_CHECK(OccupantMask::from_bits(mask.bits()).ok());
    SC_CHECK(OccupantMask::from_bits(item.bits).ok());
  }
  SC_CHECK(default_mask_for_reason(ExclusionReason::unspecified).empty());
  SC_CHECK_EQ(OccupantMask::all().bits(), 0x3Fu);
  SC_CHECK(OccupantMask::all() == default_mask_for_reason(ExclusionReason::structural));
  SC_CHECK(default_mask_for_reason(ExclusionReason::service_clearance)
               .contains(OccupantKind::service_access));
  SC_CHECK(!default_mask_for_reason(ExclusionReason::thermal)
                .contains(OccupantKind::service_access));
  SC_CHECK(default_mask_for_reason(ExclusionReason::thermal).contains(OccupantKind::asset));
  SC_CHECK(!default_mask_for_reason(ExclusionReason::reserved_expansion)
                .contains(OccupantKind::unknown));
  SC_CHECK(default_mask_for_reason(ExclusionReason::reserved_expansion)
               .contains(OccupantKind::rack));
  SC_CHECK_EQ(OccupantMask::of(OccupantKind::service_access).bits(), 0x20u);
  // A mask with a bit outside the occupant kinds is refused.
  const Result<OccupantMask> stray = OccupantMask::from_bits(0x40u);
  SC_CHECK(!stray.ok());
  if (!stray.ok()) SC_CHECK_EQ(stray.error().code, ErrorCode::unknown_enum_token);
  SC_CHECK(OccupantMask::from_bits(0u).ok());
}

// ---------------------------------------------------------------------------
// Expansion zones
// ---------------------------------------------------------------------------

void expansion_states() {
  SC_CASE("expansion state predicates and transitions");
  struct ExpansionCase final {
    ExpansionState state;
    bool earmarks;
    bool planned;
  };
  const ExpansionCase table[] = {
      {ExpansionState::identified, true, true},
      {ExpansionState::planned, true, true},
      {ExpansionState::funded, true, true},
      {ExpansionState::active, true, false},
      {ExpansionState::consumed, false, false},
      {ExpansionState::retired, false, false},
  };
  for (const ExpansionCase& item : table) {
    SC_CHECK_EQ(expansion_state_earmarks(item.state), item.earmarks);
    SC_CHECK_EQ(expansion_state_is_planned(item.state), item.planned);
  }
  // A default zone is `identified`, which is a zone that still earmarks.
  SC_CHECK_EQ(ExpansionZone{}.state, ExpansionState::identified);
  SC_CHECK(ExpansionZone{}.earmarks());

  const ExpansionState order[] = {
      ExpansionState::identified, ExpansionState::planned,  ExpansionState::funded,
      ExpansionState::active,     ExpansionState::consumed, ExpansionState::retired};
  const bool expected[6][6] = {
      /* identified */ {false, true, true, false, false, true},
      /* planned    */ {true, false, true, true, false, true},
      /* funded     */ {false, true, false, true, false, true},
      /* active     */ {false, false, true, false, true, true},
      /* consumed   */ {false, false, false, false, false, false},
      /* retired    */ {false, false, false, false, false, false},
  };
  for (std::size_t from = 0; from < 6; ++from) {
    for (std::size_t to = 0; to < 6; ++to) {
      SC_CHECK_EQ(expansion_transition_allowed(order[from], order[to]), expected[from][to]);
    }
  }
  for (const ExpansionState state : order) {
    SC_CHECK(!expansion_transition_allowed(state, state));
    SC_CHECK(!expansion_transition_allowed(ExpansionState::consumed, state));
    SC_CHECK(!expansion_transition_allowed(ExpansionState::retired, state));
  }
  SC_CHECK(expansion_transition_allowed(ExpansionState::active, ExpansionState::consumed));
  SC_CHECK(expansion_transition_allowed(ExpansionState::active, ExpansionState::retired));
}

// ---------------------------------------------------------------------------
// Footprint scope
// ---------------------------------------------------------------------------

void footprint_scopes() {
  SC_CASE("FootprintScope validation and measurement");
  FootprintScope whole;
  whole.kind = FootprintScopeKind::whole_node;
  SC_CHECK(whole.validate_shape().ok());
  SC_CHECK(whole.is_whole_node());
  SC_CHECK(!whole.planar_area().has_value());
  SC_CHECK(!whole.unit_count().has_value());

  FootprintScope wrong = whole;
  wrong.rects = *RectSet::build({PlanarRect::make(0, 0, 10, 10)});
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);
  wrong = whole;
  wrong.units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);

  FootprintScope planar;
  planar.kind = FootprintScopeKind::planar;
  planar.rects = *RectSet::build(
      {PlanarRect::make(0, 0, 1000, 1000), PlanarRect::make(2000, 0, 500, 500)});
  SC_CHECK(planar.validate_shape().ok());
  SC_CHECK(!planar.is_whole_node());
  const std::optional<SquareMillimeters> area = planar.planar_area();
  SC_CHECK(area.has_value());
  if (area.has_value()) SC_CHECK_EQ(*area, SquareMillimeters{1250000});
  SC_CHECK(!planar.unit_count().has_value());
  wrong = planar;
  wrong.units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);
  wrong = planar;
  wrong.rects = RectSet{};
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);

  FootprintScope units;
  units.kind = FootprintScopeKind::rack_units;
  units.units = *IntervalSet::build(
      {RackUnitInterval::of_count(1, 2), RackUnitInterval::of_count(5, 5)});
  SC_CHECK(units.validate_shape().ok());
  const std::optional<RackUnits> count = units.unit_count();
  SC_CHECK(count.has_value());
  if (count.has_value()) SC_CHECK_EQ(*count, RackUnits{7});
  SC_CHECK(!units.planar_area().has_value());
  wrong = units;
  wrong.rects = *RectSet::build({PlanarRect::make(0, 0, 10, 10)});
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);
  wrong = units;
  wrong.units = IntervalSet{};
  expect_status(wrong.validate_shape(), ErrorCode::invalid_argument);

  // An unknown kind is refused as an unknown token and measures nothing.
  FootprintScope unknown;
  unknown.kind = static_cast<FootprintScopeKind>(99);
  expect_status(unknown.validate_shape(), ErrorCode::unknown_enum_token);
  SC_CHECK(!unknown.planar_area().has_value());
  SC_CHECK(!unknown.unit_count().has_value());
}

// ---------------------------------------------------------------------------
// Record shape validation
// ---------------------------------------------------------------------------

void claim_shape() {
  SC_CASE("validate_claim_shape");
  const OccupancyClaim base = good_claim();
  SC_CHECK(validate_claim_shape(base).ok());
  SC_CHECK_EQ(validate_claim_shape(base).code(), ErrorCode::ok);

  OccupancyClaim broken = base;
  broken.id = OccupancyClaimId{};
  expect_status(validate_claim_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.node = SpaceNodeId{};
  expect_status(validate_claim_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.state = static_cast<ClaimState>(0);
  expect_status(validate_claim_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.state = static_cast<ClaimState>(99);
  expect_status(validate_claim_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.occupant = static_cast<OccupantKind>(99);
  expect_status(validate_claim_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.scope.rects = RectSet{};
  expect_status(validate_claim_shape(broken), ErrorCode::invalid_argument);
  broken = base;
  broken.rack = RackRef{};
  expect_status(validate_claim_shape(broken), ErrorCode::malformed_reference);

  // A whole-node claim and a rack unit claim are ordinary.
  OccupancyClaim whole = base;
  whole.scope = FootprintScope{};
  SC_CHECK(validate_claim_shape(whole).ok());
  OccupancyClaim unit_claim = base;
  unit_claim.node = sc_fixture::ids().rack1;
  unit_claim.scope.kind = FootprintScopeKind::rack_units;
  unit_claim.scope.rects = RectSet{};
  unit_claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(10, 6)});
  unit_claim.occupant = OccupantKind::asset;
  SC_CHECK(validate_claim_shape(unit_claim).ok());
}

void reservation_shape() {
  SC_CASE("validate_reservation_shape");
  const FootprintReservation base = good_reservation();
  SC_CHECK(validate_reservation_shape(base).ok());

  FootprintReservation broken = base;
  broken.id = OccupancyClaimId{};
  expect_status(validate_reservation_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.node = SpaceNodeId{};
  expect_status(validate_reservation_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.state = static_cast<ReservationState>(0);
  expect_status(validate_reservation_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.state = static_cast<ReservationState>(99);
  expect_status(validate_reservation_shape(broken), ErrorCode::unknown_enum_token);
  // A hold in force must name the authority that granted it.
  broken = base;
  broken.reservation = ReservationRef{};
  expect_status(validate_reservation_shape(broken), ErrorCode::reservation_authority_missing);
  broken = base;
  broken.not_after = Timestamp{0};
  expect_status(validate_reservation_shape(broken), ErrorCode::invalid_timestamp);
  broken = base;
  broken.scope.units = IntervalSet{};
  expect_status(validate_reservation_shape(broken), ErrorCode::invalid_argument);

  // A hold that is over may keep an empty authority: it is history then.
  FootprintReservation released = base;
  released.state = ReservationState::released;
  released.reservation = ReservationRef{};
  SC_CHECK(validate_reservation_shape(released).ok());
  // A real declared end is ordinary.
  FootprintReservation timed = base;
  timed.not_after = Timestamp{1'800'000'000'000};
  SC_CHECK(validate_reservation_shape(timed).ok());
  SC_CHECK_EQ(reservation_state_at(timed, Timestamp{1'800'000'000'000}),
              ReservationState::expired);
}

void exclusion_shape() {
  SC_CASE("validate_exclusion_shape");
  const ExclusionRegion base = good_exclusion();
  SC_CHECK(validate_exclusion_shape(base).ok());

  ExclusionRegion broken = base;
  broken.id = ExclusionRegionId{};
  expect_status(validate_exclusion_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.node = SpaceNodeId{};
  expect_status(validate_exclusion_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.reason = static_cast<ExclusionReason>(200);
  expect_status(validate_exclusion_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.state = static_cast<ExclusionState>(0);
  expect_status(validate_exclusion_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.state = static_cast<ExclusionState>(99);
  expect_status(validate_exclusion_shape(broken), ErrorCode::unknown_enum_token);
  // An exclusion that excludes nothing is a modelling error.
  broken = base;
  broken.blocks = OccupantMask{};
  expect_status(validate_exclusion_shape(broken), ErrorCode::invalid_argument);
  // A mask with a bit outside the occupant kinds is not a wider mask.
  broken = base;
  broken.blocks = OccupantMask::of(static_cast<OccupantKind>(20));
  SC_CHECK(!broken.blocks.empty());
  expect_status(validate_exclusion_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.scope.rects = RectSet{};
  expect_status(validate_exclusion_shape(broken), ErrorCode::invalid_argument);

  // A draft blocks nothing in the engine and is still a legal record.
  ExclusionRegion draft = base;
  draft.state = ExclusionState::draft;
  SC_CHECK(validate_exclusion_shape(draft).ok());
  // A vertical exclusion on a rack envelope is ordinary.
  ExclusionRegion vertical = base;
  vertical.node = sc_fixture::ids().rack1;
  vertical.reason = ExclusionReason::electrical;
  vertical.blocks = default_mask_for_reason(ExclusionReason::electrical);
  vertical.scope.kind = FootprintScopeKind::rack_units;
  vertical.scope.rects = RectSet{};
  vertical.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 4)});
  SC_CHECK(validate_exclusion_shape(vertical).ok());
}

void clearance_shape() {
  SC_CASE("validate_clearance_shape");
  const ClearanceConstraint planar = good_clearance();
  SC_CHECK(validate_clearance_shape(planar).ok());
  const ClearanceConstraint vertical = good_vertical_clearance();
  SC_CHECK(validate_clearance_shape(vertical).ok());
  SC_CHECK(clearance_kind_is_planar(ClearanceKind::front_service));
  SC_CHECK(clearance_kind_is_planar(ClearanceKind::rear_service));
  SC_CHECK(clearance_kind_is_planar(ClearanceKind::side_service));
  SC_CHECK(clearance_kind_is_planar(ClearanceKind::overhead_service));
  SC_CHECK(!clearance_kind_is_planar(ClearanceKind::rack_unit_access));
  SC_CHECK(clearance_kind_is_vertical(ClearanceKind::rack_unit_access));
  SC_CHECK(!clearance_kind_is_vertical(ClearanceKind::front_service));

  ClearanceConstraint broken = planar;
  broken.id = ClearanceConstraintId{};
  expect_status(validate_clearance_shape(broken), ErrorCode::empty_value);
  broken = planar;
  broken.node = SpaceNodeId{};
  expect_status(validate_clearance_shape(broken), ErrorCode::empty_value);
  broken = planar;
  broken.kind = static_cast<ClearanceKind>(0);
  expect_status(validate_clearance_shape(broken), ErrorCode::unknown_enum_token);
  broken = planar;
  broken.kind = static_cast<ClearanceKind>(99);
  expect_status(validate_clearance_shape(broken), ErrorCode::unknown_enum_token);

  // Both forms absent.
  broken = planar;
  broken.has_band = false;
  broken.has_units = false;
  expect_status(validate_clearance_shape(broken), ErrorCode::scope_kind_mismatch);
  // A planar kind described in rack units.
  broken = planar;
  broken.has_band = false;
  broken.has_units = true;
  broken.required_free_units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
  expect_status(validate_clearance_shape(broken), ErrorCode::scope_kind_mismatch);
  // Both forms present at once.
  broken = planar;
  broken.has_units = true;
  expect_status(validate_clearance_shape(broken), ErrorCode::scope_kind_mismatch);
  // A vertical kind described as a band.
  broken = vertical;
  broken.has_units = false;
  broken.has_band = true;
  expect_status(validate_clearance_shape(broken), ErrorCode::scope_kind_mismatch);
  // A band that encloses no area.
  broken = planar;
  broken.band = PlanarRect::make(0, 0, 0, 10);
  expect_status(validate_clearance_shape(broken), ErrorCode::invalid_extent);
  // A vertical clearance that keeps no unit free.
  broken = vertical;
  broken.required_free_units = IntervalSet{};
  expect_status(validate_clearance_shape(broken), ErrorCode::invalid_extent);

  // An unenforceable clearance is reported, not refused.
  ClearanceConstraint reported = planar;
  reported.enforceable = false;
  SC_CHECK(validate_clearance_shape(reported).ok());
}

void expansion_shape() {
  SC_CASE("validate_expansion_zone_shape");
  const ExpansionZone base = good_zone();
  SC_CHECK(validate_expansion_zone_shape(base).ok());
  SC_CHECK(base.earmarks());

  ExpansionZone broken = base;
  broken.id = ExpansionZoneId{};
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.node = SpaceNodeId{};
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::empty_value);
  broken = base;
  broken.state = static_cast<ExpansionState>(0);
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.state = static_cast<ExpansionState>(99);
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::unknown_enum_token);
  broken = base;
  broken.scope.rects = RectSet{};
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::invalid_argument);
  // The Unix epoch is an unset field, not a target date.
  broken = base;
  broken.target_ready_by = Timestamp{0};
  expect_status(validate_expansion_zone_shape(broken), ErrorCode::invalid_timestamp);

  // A real readiness target is ordinary.
  ExpansionZone targeted = base;
  targeted.target_ready_by = Timestamp{1'900'000'000'000};
  SC_CHECK(validate_expansion_zone_shape(targeted).ok());
  // A consumed zone is history and earmarks nothing.
  ExpansionZone consumed = base;
  consumed.state = ExpansionState::consumed;
  SC_CHECK(validate_expansion_zone_shape(consumed).ok());
  SC_CHECK(!consumed.earmarks());
}

// ---------------------------------------------------------------------------
// Record equality
// ---------------------------------------------------------------------------

void record_equality() {
  SC_CASE("the five record equality operators are field-sensitive");
  // OccupancyClaim.
  const OccupancyClaim claim = good_claim();
  SC_CHECK(claim == claim);
  SC_CHECK(claim == good_claim());
  SC_CHECK(!(claim != good_claim()));
  OccupancyClaim claim_probe = claim;
  claim_probe.id = *OccupancyClaimId::parse("claim-other");
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.generation = EntityGeneration{2};
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.node = sc_fixture::ids().rack1;
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.scope.kind = FootprintScopeKind::whole_node;
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.state = ClaimState::releasing;
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.occupant = OccupantKind::asset;
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.label = *DisplayLabel::parse("other");
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.note = *Note::parse("a note");
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.rack = RackRef::of("rack:a01").value();
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.assets = *AssetRefSet::build({AssetRef::of("asset-1").value()});
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-1").value()});
  SC_CHECK(claim_probe != claim);
  claim_probe = claim;
  claim_probe.from_reservation = ReservationRef::make("res-0001", 1, UpstreamState::active).value();
  SC_CHECK(claim_probe != claim);

  // FootprintReservation.
  const FootprintReservation reservation = good_reservation();
  SC_CHECK(reservation == reservation);
  SC_CHECK(reservation == good_reservation());
  FootprintReservation reservation_probe = reservation;
  reservation_probe.id = *OccupancyClaimId::parse("hold-2");
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.generation = EntityGeneration{2};
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.node = sc_fixture::ids().rack3;
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.scope.kind = FootprintScopeKind::whole_node;
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.state = ReservationState::partially_claimed;
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.reservation = ReservationRef::make("res-0002", 1, UpstreamState::active).value();
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.holder_label = *DisplayLabel::parse("tenant c");
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.holder_assets = *AssetRefSet::build({AssetRef::of("asset-2").value()});
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-2").value()});
  SC_CHECK(reservation_probe != reservation);
  reservation_probe = reservation;
  reservation_probe.not_after = Timestamp{1};
  SC_CHECK(reservation_probe != reservation);

  // ExclusionRegion.
  const ExclusionRegion region = good_exclusion();
  SC_CHECK(region == region);
  SC_CHECK(region == good_exclusion());
  ExclusionRegion region_probe = region;
  region_probe.id = *ExclusionRegionId::parse("excl-2");
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.generation = EntityGeneration{2};
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.node = sc_fixture::ids().rack1;
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.scope.kind = FootprintScopeKind::whole_node;
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.reason = ExclusionReason::safety;
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.state = ExclusionState::suspended;
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.blocks = OccupantMask::all();
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.label = *DisplayLabel::parse("other");
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.note = *Note::parse("a note");
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-3").value()});
  SC_CHECK(region_probe != region);
  region_probe = region;
  region_probe.subjects = *AssetRefSet::build({AssetRef::of("asset-3").value()});
  SC_CHECK(region_probe != region);

  // ClearanceConstraint.
  const ClearanceConstraint constraint = good_clearance();
  SC_CHECK(constraint == constraint);
  SC_CHECK(constraint == good_clearance());
  ClearanceConstraint clearance_probe = constraint;
  clearance_probe.id = *ClearanceConstraintId::parse("clr-2");
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.generation = EntityGeneration{2};
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.node = sc_fixture::ids().rack4;
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.kind = ClearanceKind::rear_service;
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.band = PlanarRect::make(0, 0, 10, 10);
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.has_band = false;
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.required_free_units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.has_units = true;
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.enforceable = false;
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.label = *DisplayLabel::parse("other");
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.note = *Note::parse("a note");
  SC_CHECK(clearance_probe != constraint);
  clearance_probe = constraint;
  clearance_probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-4").value()});
  SC_CHECK(clearance_probe != constraint);
  // The vertical form is a different value from the planar one.
  SC_CHECK(good_vertical_clearance() != constraint);

  // ExpansionZone.
  const ExpansionZone zone = good_zone();
  SC_CHECK(zone == zone);
  SC_CHECK(zone == good_zone());
  ExpansionZone zone_probe = zone;
  zone_probe.id = *ExpansionZoneId::parse("zone-2");
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.generation = EntityGeneration{2};
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.node = sc_fixture::ids().building;
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.scope.kind = FootprintScopeKind::whole_node;
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.state = ExpansionState::planned;
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.label = *DisplayLabel::parse("other");
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.note = *Note::parse("a note");
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-5").value()});
  SC_CHECK(zone_probe != zone);
  zone_probe = zone;
  zone_probe.target_ready_by = Timestamp{1};
  SC_CHECK(zone_probe != zone);
}

}  // namespace

int main() {
  claim_state_predicates();
  claim_transition_table();
  reservation_transitions();
  exclusion_states_and_masks();
  expansion_states();
  footprint_scopes();
  claim_shape();
  reservation_shape();
  exclusion_shape();
  clearance_shape();
  expansion_shape();
  record_equality();
  return ::sc_test::summary("claim_region_test");
}
