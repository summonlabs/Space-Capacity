// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - occupancy claims and reserved footprint.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two record families consume space:
//
//   * an OccupancyClaim states that a region of a node is taken. A claim that
//     is committed consumes space now. A claim that is planned or submitted
//     does not: it is reported separately as pending, so a number an operator
//     reads as "available now" never includes space something merely intends
//     to use. A claim that is releasing still consumes, so space is never
//     double-promised while a teardown is in flight.
//
//   * a FootprintReservation holds space for a future use that has already
//     been decided elsewhere. Space Capacity does not decide reservations and
//     does not grant them: recording one requires a reservation reference from
//     the runtime that owns that authority, and Space Capacity refuses a
//     reservation record that arrives without one.
//
// Neither family grants placement. A claim records that space is consumed; a
// query reports what a fit would be. Nothing here authorises a workload, an
// asset or a rack to be put anywhere, and nothing here schedules anything.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

enum class FootprintScopeKind : std::uint8_t {
  // The whole declared envelope of the node. A rack may hold at most one
  // committed whole-node claim, because a second one would be
  // indistinguishable from the first.
  whole_node = 1,
  // An explicit set of planar rectangles inside the node's plane.
  planar = 2,
  // An explicit set of rack-unit intervals inside the node's vertical
  // envelope. Only a node that declares a rack envelope has one to consume.
  rack_units = 3,
};

SC_API std::string_view footprint_scope_kind_name(FootprintScopeKind value) noexcept;
SC_API bool parse_footprint_scope_kind(std::string_view text, FootprintScopeKind& out) noexcept;

// What part of a node a claim, reservation, exclusion or zone covers.
struct SC_API FootprintScope final {
  FootprintScopeKind kind = FootprintScopeKind::whole_node;
  RectSet rects{};
  IntervalSet units{};

  [[nodiscard]] bool is_whole_node() const noexcept {
    return kind == FootprintScopeKind::whole_node;
  }

  // The exact planar area the scope covers, or nullopt when the scope is not
  // planar and therefore has no area of its own. A whole-node scope has no
  // area of its own: it covers whatever the node declares, which the capacity
  // engine resolves, not this type.
  [[nodiscard]] std::optional<SquareMillimeters> planar_area() const noexcept;

  // The exact number of rack units the scope covers, or nullopt when the scope
  // is not measured in rack units.
  [[nodiscard]] std::optional<RackUnits> unit_count() const noexcept;

  // Structural validation independent of any node: the kind must match the
  // populated field, extents must be bounded and non-degenerate, and unit
  // intervals must already be normalized and disjoint.
  [[nodiscard]] Status validate_shape() const;

  [[nodiscard]] friend bool operator==(const FootprintScope& a,
                                       const FootprintScope& b) noexcept {
    return a.kind == b.kind && a.rects == b.rects && a.units == b.units;
  }
  [[nodiscard]] friend bool operator!=(const FootprintScope& a,
                                       const FootprintScope& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const FootprintScope& a,
                                      const FootprintScope& b) noexcept {
    if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    if (a.rects != b.rects) return a.rects < b.rects;
    return a.units < b.units;
  }
};

// ---------------------------------------------------------------------------
// Occupancy claims
// ---------------------------------------------------------------------------

// What kind of thing occupies the space. Used to answer incompatibility and
// exclusion questions and to explain a refusal; never used as authority.
enum class OccupantKind : std::uint8_t {
  unknown = 0,
  rack = 1,
  asset = 2,
  workload = 3,
  infrastructure = 4,
  service_access = 5,
};

inline constexpr std::size_t kOccupantKindCount = 6;

SC_API std::string_view occupant_kind_name(OccupantKind value) noexcept;
SC_API bool parse_occupant_kind(std::string_view text, OccupantKind& out) noexcept;
SC_API OccupantKind occupant_kind_from_value(std::uint8_t value) noexcept;

// Claim lifecycle. `planned` and `submitted` are non-consuming; `committed`
// and `releasing` consume; `released` and `retired` consume nothing.
enum class ClaimState : std::uint8_t {
  planned = 1,
  submitted = 2,
  committed = 3,
  releasing = 4,
  released = 5,
  retired = 6,
};

inline constexpr std::size_t kClaimStateCount = 7;

SC_API std::string_view claim_state_name(ClaimState value) noexcept;
SC_API bool parse_claim_state(std::string_view text, ClaimState& out) noexcept;
SC_API ClaimState claim_state_from_value(std::uint8_t value) noexcept;

SC_API bool claim_state_consumes(ClaimState value) noexcept;
SC_API bool claim_state_is_pending(ClaimState value) noexcept;
SC_API bool claim_state_is_terminal(ClaimState value) noexcept;

// Legal transitions. A transition not allowed here is refused as a conflict,
// including every backwards move, so a released claim cannot be revived.
SC_API bool claim_transition_allowed(ClaimState from, ClaimState to) noexcept;

struct SC_API OccupancyClaim final {
  OccupancyClaimId id{};
  EntityGeneration generation{};

  // The node whose plane or envelope is consumed. Required.
  SpaceNodeId node{};

  FootprintScope scope{};
  ClaimState state = ClaimState::planned;
  OccupantKind occupant = OccupantKind::unknown;

  DisplayLabel label{};
  Note note{};

  // Who or what occupies. All optional: a claim may be recorded before the
  // occupant is correlated with a registry row, in which case the claim is
  // reported as uncorrelated.
  std::optional<RackRef> rack{};
  AssetRefSet assets{};
  PolicyRefSet policies{};
  EvidenceSet evidence{};

  // Reservation this claim realized, when the claim came from a hold. Kept so
  // that releasing the claim can be explained back to the hold.
  std::optional<ReservationRef> from_reservation{};

  [[nodiscard]] bool consumes() const noexcept { return claim_state_consumes(state); }
  [[nodiscard]] bool is_terminal() const noexcept { return claim_state_is_terminal(state); }

  [[nodiscard]] friend bool operator==(const OccupancyClaim& a,
                                       const OccupancyClaim& b) noexcept;
  [[nodiscard]] friend bool operator!=(const OccupancyClaim& a,
                                       const OccupancyClaim& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const OccupancyClaim& a,
                                      const OccupancyClaim& b) noexcept {
    return a.id < b.id;
  }
};

SC_API Status validate_claim_shape(const OccupancyClaim& claim);
SC_API Status validate_claim_transition(const OccupancyClaim& existing, ClaimState next);

// ---------------------------------------------------------------------------
// Reserved footprint
// ---------------------------------------------------------------------------

enum class ReservationState : std::uint8_t {
  // The hold is in force and subtracts from available space.
  held = 1,
  // The hold is in force for the part that has not yet been claimed.
  partially_claimed = 2,
  // The holder gave the space up; it subtracts nothing.
  released = 3,
  // The hold passed its declared expiry when evaluated against a caller
  // supplied reading; it subtracts nothing and is reported as expired.
  expired = 4,
  // The external authority withdrew the reservation; it subtracts nothing.
  revoked = 5,
};

inline constexpr std::size_t kReservationStateCount = 6;

SC_API std::string_view reservation_state_name(ReservationState value) noexcept;
SC_API bool parse_reservation_state(std::string_view text, ReservationState& out) noexcept;
SC_API ReservationState reservation_state_from_value(std::uint8_t value) noexcept;

SC_API bool reservation_state_holds(ReservationState value) noexcept;
SC_API bool reservation_state_is_terminal(ReservationState value) noexcept;
SC_API bool reservation_transition_allowed(ReservationState from, ReservationState to) noexcept;

struct SC_API FootprintReservation final {
  // Reservations use the claim identity family so that a store holds one
  // identity space, but they carry their own record type so a reservation
  // identity can never be passed where a claim identity is required.
  OccupancyClaimId id{};
  EntityGeneration generation{};

  SpaceNodeId node{};
  FootprintScope scope{};
  ReservationState state = ReservationState::held;

  // Authority for the hold. Required for every non-terminal reservation state:
  // Space Capacity refuses to hold space that no runtime with reservation
  // authority has reserved.
  ReservationRef reservation{};

  DisplayLabel holder_label{};
  AssetRefSet holder_assets{};
  PolicyRefSet policies{};
  EvidenceSet evidence{};

  // Declared end of the hold, if any. Space Capacity never reads a clock to
  // evaluate this; a caller passes the reading it wants the hold evaluated
  // against, so the same state gives the same answer at the same reading.
  std::optional<Timestamp> not_after{};

  [[nodiscard]] friend bool operator==(const FootprintReservation& a,
                                       const FootprintReservation& b) noexcept;
  [[nodiscard]] friend bool operator!=(const FootprintReservation& a,
                                       const FootprintReservation& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const FootprintReservation& a,
                                      const FootprintReservation& b) noexcept {
    return a.id < b.id;
  }
};

SC_API Status validate_reservation_shape(const FootprintReservation& reservation);

// The effective state at a caller-supplied reading. Returns the stored state
// when nothing changes, or `expired` when the hold declares a `not_after` and
// the reading is at or past it. Terminal states never become non-terminal.
SC_API ReservationState reservation_state_at(const FootprintReservation& reservation,
                                             Timestamp now) noexcept;

}  // namespace dccp::space_capacity
