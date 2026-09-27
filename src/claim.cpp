// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - occupancy claims, reserved footprint and their lifecycles.
//
// The transition rules live here as one table per family rather than as
// scattered conditionals, because two callers ask the same question: one asks
// "may this move happen?" before it mutates anything, the other applies the
// move. Both must get the same answer, and an illegal move must be refused
// with the same explanation every time.

#include "dccp/space_capacity/claim.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/text.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {
namespace {

// Persisted raw bounds for the state enums. A one-based state never stores
// zero: zero is what an uninitialised or truncated field carries, and it is
// refused as an unknown token rather than read as the first state. The
// header's `kCount` constants sit one past the last state, so the last state
// is `kCount - 1` and every value at or above `kCount` is unknown.
constexpr std::uint8_t kClaimStateMin = static_cast<std::uint8_t>(ClaimState::planned);
constexpr std::uint8_t kClaimStateMax = static_cast<std::uint8_t>(kClaimStateCount - 1);
constexpr std::uint8_t kReservationStateMin = static_cast<std::uint8_t>(ReservationState::held);
constexpr std::uint8_t kReservationStateMax = static_cast<std::uint8_t>(kReservationStateCount - 1);

// Occupant kinds are zero-based, so the whole range is admissible: zero means
// `unknown`, which is a recorded value and not an unset field.
constexpr std::uint8_t kOccupantKindMax = static_cast<std::uint8_t>(kOccupantKindCount - 1);

// Free text carried by a record. Both bounds come from Limits, and the text
// must already be valid UTF-8: Space Capacity never re-encodes or repairs what
// it was given.
Status validate_record_text(std::string_view text, std::size_t limit, std::string_view what) {
  if (text.size() > limit) {
    return Status::failure(ErrorCode::text_too_long,
                           std::string(what) + " must not exceed " +
                               to_text(static_cast<std::uint64_t>(limit)) + " bytes");
  }
  if (!is_utf8(text)) {
    return Status::failure(ErrorCode::invalid_utf8, std::string(what) + " must be valid UTF-8");
  }
  return Status::success();
}

// A record may carry at most kMaxExtentsPerRecord disjoint extents. The sets
// that hold them enforce this at construction; a scope that arrived from a
// decoder is restated here so that the bound does not depend on which path
// built the record.
Status validate_extent_count(std::size_t count, std::string_view what) {
  if (count > Limits::kMaxExtentsPerRecord) {
    return Status::failure(ErrorCode::limit_exceeded,
                           std::string(what) + " may hold at most " +
                               to_text(static_cast<std::uint64_t>(Limits::kMaxExtentsPerRecord)) +
                               " extents");
  }
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

std::string_view footprint_scope_kind_name(FootprintScopeKind value) noexcept {
  switch (value) {
    case FootprintScopeKind::whole_node:
      return "whole_node";
    case FootprintScopeKind::planar:
      return "planar";
    case FootprintScopeKind::rack_units:
      return "rack_units";
  }
  return "unknown";
}

bool parse_footprint_scope_kind(std::string_view text, FootprintScopeKind& out) noexcept {
  if (text == "whole_node") {
    out = FootprintScopeKind::whole_node;
    return true;
  }
  if (text == "planar") {
    out = FootprintScopeKind::planar;
    return true;
  }
  if (text == "rack_units") {
    out = FootprintScopeKind::rack_units;
    return true;
  }
  return false;
}

std::optional<SquareMillimeters> FootprintScope::planar_area() const noexcept {
  if (kind != FootprintScopeKind::planar) return std::nullopt;
  const Checked<SquareMillimeters> total = rects.total_area();
  // An overflowed total is not a zero total: the caller is told that the area
  // is unknown rather than that the scope covers nothing.
  if (!total) return std::nullopt;
  return total.value;
}

std::optional<RackUnits> FootprintScope::unit_count() const noexcept {
  if (kind != FootprintScopeKind::rack_units) return std::nullopt;
  const Checked<RackUnits> total = units.total();
  if (!total) return std::nullopt;
  return total.value;
}

Status FootprintScope::validate_shape() const {
  switch (kind) {
    case FootprintScopeKind::whole_node:
      if (!rects.empty() || !units.empty()) {
        return Status::failure(
            ErrorCode::invalid_argument,
            "a whole-node scope covers the declared envelope and carries neither rectangles nor "
            "rack unit intervals");
      }
      return Status::success();
    case FootprintScopeKind::planar: {
      if (!units.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
                               "a planar scope carries no rack unit intervals");
      }
      if (rects.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
                               "a planar scope must cover at least one rectangle");
      }
      return validate_extent_count(rects.size(), "a planar scope");
    }
    case FootprintScopeKind::rack_units: {
      if (!rects.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
                               "a rack unit scope carries no planar rectangles");
      }
      if (units.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
                               "a rack unit scope must cover at least one interval");
      }
      return validate_extent_count(units.size(), "a rack unit scope");
    }
  }
  return Status::failure(ErrorCode::unknown_enum_token,
                         "footprint scope kind value " +
                             to_text(static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind))) +
                             " is not a known scope kind");
}

// ---------------------------------------------------------------------------
// Occupant kinds
// ---------------------------------------------------------------------------

std::string_view occupant_kind_name(OccupantKind value) noexcept {
  switch (value) {
    case OccupantKind::unknown:
      return "unknown";
    case OccupantKind::rack:
      return "rack";
    case OccupantKind::asset:
      return "asset";
    case OccupantKind::workload:
      return "workload";
    case OccupantKind::infrastructure:
      return "infrastructure";
    case OccupantKind::service_access:
      return "service_access";
  }
  return "unknown";
}

bool parse_occupant_kind(std::string_view text, OccupantKind& out) noexcept {
  if (text == "unknown") {
    out = OccupantKind::unknown;
    return true;
  }
  if (text == "rack") {
    out = OccupantKind::rack;
    return true;
  }
  if (text == "asset") {
    out = OccupantKind::asset;
    return true;
  }
  if (text == "workload") {
    out = OccupantKind::workload;
    return true;
  }
  if (text == "infrastructure") {
    out = OccupantKind::infrastructure;
    return true;
  }
  if (text == "service_access") {
    out = OccupantKind::service_access;
    return true;
  }
  return false;
}

OccupantKind occupant_kind_from_value(std::uint8_t value) noexcept {
  if (value > kOccupantKindMax) return OccupantKind::unknown;
  return static_cast<OccupantKind>(value);
}

// ---------------------------------------------------------------------------
// Claim lifecycle
// ---------------------------------------------------------------------------

std::string_view claim_state_name(ClaimState value) noexcept {
  switch (value) {
    case ClaimState::planned:
      return "planned";
    case ClaimState::submitted:
      return "submitted";
    case ClaimState::committed:
      return "committed";
    case ClaimState::releasing:
      return "releasing";
    case ClaimState::released:
      return "released";
    case ClaimState::retired:
      return "retired";
  }
  return "unknown";
}

bool parse_claim_state(std::string_view text, ClaimState& out) noexcept {
  if (text == "planned") {
    out = ClaimState::planned;
    return true;
  }
  if (text == "submitted") {
    out = ClaimState::submitted;
    return true;
  }
  if (text == "committed") {
    out = ClaimState::committed;
    return true;
  }
  if (text == "releasing") {
    out = ClaimState::releasing;
    return true;
  }
  if (text == "released") {
    out = ClaimState::released;
    return true;
  }
  if (text == "retired") {
    out = ClaimState::retired;
    return true;
  }
  return false;
}

ClaimState claim_state_from_value(std::uint8_t value) noexcept {
  if (value < kClaimStateMin || value > kClaimStateMax) return ClaimState::planned;
  return static_cast<ClaimState>(value);
}

bool claim_state_consumes(ClaimState value) noexcept {
  return value == ClaimState::committed || value == ClaimState::releasing;
}

bool claim_state_is_pending(ClaimState value) noexcept {
  return value == ClaimState::planned || value == ClaimState::submitted;
}

bool claim_state_is_terminal(ClaimState value) noexcept {
  return value == ClaimState::released || value == ClaimState::retired;
}

bool claim_transition_allowed(ClaimState from, ClaimState to) noexcept {
  // A self transition is never a transition: it changes nothing and would
  // otherwise let a caller claim it had moved the record.
  if (from == to) return false;
  switch (from) {
    case ClaimState::planned:
      return to == ClaimState::submitted || to == ClaimState::released ||
             to == ClaimState::retired;
    case ClaimState::submitted:
      return to == ClaimState::committed || to == ClaimState::planned ||
             to == ClaimState::released || to == ClaimState::retired;
    case ClaimState::committed:
      return to == ClaimState::releasing || to == ClaimState::released;
    case ClaimState::releasing:
      // A teardown may be abandoned before it completes. The space is still
      // consumed in either state, so returning to `committed` changes no
      // capacity number; it only says the teardown did not finish.
      return to == ClaimState::released || to == ClaimState::committed;
    case ClaimState::released:
    case ClaimState::retired:
      // Both are terminal: a released claim is never revived, and a retired
      // one is never returned to service.
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Claim records
// ---------------------------------------------------------------------------

bool operator==(const OccupancyClaim& a, const OccupancyClaim& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.node == b.node && a.scope == b.scope &&
         a.state == b.state && a.occupant == b.occupant && a.label == b.label && a.note == b.note &&
         a.rack == b.rack && a.assets == b.assets && a.policies == b.policies &&
         a.evidence == b.evidence && a.from_reservation == b.from_reservation;
}

Status validate_claim_shape(const OccupancyClaim& claim) {
  if (claim.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "a claim must carry an identity");
  }
  if (claim.node.empty()) {
    return Status::failure(ErrorCode::empty_value, "a claim must name the node it occupies");
  }

  const std::uint8_t state = static_cast<std::uint8_t>(claim.state);
  if (state < kClaimStateMin || state > kClaimStateMax) {
    return Status::failure(ErrorCode::unknown_enum_token,
                           "claim state value " + to_text(static_cast<std::uint64_t>(state)) +
                               " is not a known claim state");
  }
  const std::uint8_t occupant = static_cast<std::uint8_t>(claim.occupant);
  if (occupant > kOccupantKindMax) {
    return Status::failure(ErrorCode::unknown_enum_token,
                           "occupant kind value " + to_text(static_cast<std::uint64_t>(occupant)) +
                               " is not a known occupant kind");
  }

  const Status scope = claim.scope.validate_shape();
  if (!scope) return scope;

  const Status label =
      validate_record_text(claim.label.value(), Limits::kMaxLabelBytes, "a claim label");
  if (!label) return label;
  const Status note = validate_record_text(claim.note.value(), Limits::kMaxNoteBytes, "a claim note");
  if (!note) return note;

  // A consuming claim can never cover nothing: the two scalar scope kinds are
  // already forced to be non-empty by validate_shape, and a whole-node scope
  // covers the whole envelope by definition. Nothing further is refused here.
  if (claim.rack.has_value() && claim.rack->empty()) {
    return Status::failure(ErrorCode::malformed_reference,
                           "a claim rack reference must not be empty");
  }
  return Status::success();
}

Status validate_claim_transition(const OccupancyClaim& existing, ClaimState next) {
  if (claim_transition_allowed(existing.state, next)) return Status::success();
  return Status::failure(ErrorCode::claim_transition_illegal,
                         "claim transition " + std::string(claim_state_name(existing.state)) +
                             " to " + std::string(claim_state_name(next)) +
                             " is not permitted");
}

// ---------------------------------------------------------------------------
// Reservation lifecycle
// ---------------------------------------------------------------------------

std::string_view reservation_state_name(ReservationState value) noexcept {
  switch (value) {
    case ReservationState::held:
      return "held";
    case ReservationState::partially_claimed:
      return "partially_claimed";
    case ReservationState::released:
      return "released";
    case ReservationState::expired:
      return "expired";
    case ReservationState::revoked:
      return "revoked";
  }
  return "unknown";
}

bool parse_reservation_state(std::string_view text, ReservationState& out) noexcept {
  if (text == "held") {
    out = ReservationState::held;
    return true;
  }
  if (text == "partially_claimed") {
    out = ReservationState::partially_claimed;
    return true;
  }
  if (text == "released") {
    out = ReservationState::released;
    return true;
  }
  if (text == "expired") {
    out = ReservationState::expired;
    return true;
  }
  if (text == "revoked") {
    out = ReservationState::revoked;
    return true;
  }
  return false;
}

ReservationState reservation_state_from_value(std::uint8_t value) noexcept {
  if (value < kReservationStateMin || value > kReservationStateMax) {
    return ReservationState::held;
  }
  return static_cast<ReservationState>(value);
}

bool reservation_state_holds(ReservationState value) noexcept {
  return value == ReservationState::held || value == ReservationState::partially_claimed;
}

bool reservation_state_is_terminal(ReservationState value) noexcept {
  return value == ReservationState::released || value == ReservationState::expired ||
         value == ReservationState::revoked;
}

bool reservation_transition_allowed(ReservationState from, ReservationState to) noexcept {
  if (from == to) return false;
  switch (from) {
    case ReservationState::held:
      return to == ReservationState::partially_claimed || to == ReservationState::released ||
             to == ReservationState::expired || to == ReservationState::revoked;
    case ReservationState::partially_claimed:
      // Claims against the hold may be released, which returns the part that
      // was claimed to the hold without giving the hold itself up.
      return to == ReservationState::held || to == ReservationState::released ||
             to == ReservationState::expired || to == ReservationState::revoked;
    case ReservationState::expired:
      // An expired hold may be renewed, and only explicitly: evaluation
      // against a reading never moves a record, it only reports.
      return to == ReservationState::held;
    case ReservationState::released:
    case ReservationState::revoked:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Reservation records
// ---------------------------------------------------------------------------

bool operator==(const FootprintReservation& a, const FootprintReservation& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.node == b.node && a.scope == b.scope &&
         a.state == b.state && a.reservation == b.reservation && a.holder_label == b.holder_label &&
         a.holder_assets == b.holder_assets && a.policies == b.policies &&
         a.evidence == b.evidence && a.not_after == b.not_after;
}

Status validate_reservation_shape(const FootprintReservation& reservation) {
  if (reservation.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "a reservation must carry an identity");
  }
  if (reservation.node.empty()) {
    return Status::failure(ErrorCode::empty_value, "a reservation must name the node it holds");
  }

  const std::uint8_t state = static_cast<std::uint8_t>(reservation.state);
  if (state < kReservationStateMin || state > kReservationStateMax) {
    return Status::failure(ErrorCode::unknown_enum_token,
                           "reservation state value " + to_text(static_cast<std::uint64_t>(state)) +
                               " is not a known reservation state");
  }

  const Status scope = reservation.scope.validate_shape();
  if (!scope) return scope;

  const Status label = validate_record_text(reservation.holder_label.value(),
                                            Limits::kMaxLabelBytes, "a reservation holder label");
  if (!label) return label;

  // Space Capacity does not grant holds. A hold that is in force must name the
  // authority that granted it, or the space would be subtracted on nobody's
  // word. A hold that is already released, expired or revoked may keep an
  // empty authority: it is a historical record at that point.
  if (!reservation_state_is_terminal(reservation.state) && reservation.reservation.empty()) {
    return Status::failure(ErrorCode::reservation_authority_missing,
                           "a reservation in force must name the authority that granted it");
  }

  // A declared end of the Unix epoch is not a meaningful hold end; it is an
  // unset field that arrived as a number.
  if (reservation.not_after.has_value() && reservation.not_after->is_zero()) {
    return Status::failure(ErrorCode::invalid_timestamp,
                           "a declared hold end must not be the Unix epoch");
  }
  return Status::success();
}

ReservationState reservation_state_at(const FootprintReservation& reservation,
                                      Timestamp now) noexcept {
  // The reading is compared against the declared end only. A terminal state is
  // never moved by a reading: `released`, `expired` and `revoked` report
  // themselves whatever the reading says.
  if (reservation.not_after.has_value() && reservation_state_holds(reservation.state) &&
      !(now < *reservation.not_after)) {
    return ReservationState::expired;
  }
  return reservation.state;
}

}  // namespace dccp::space_capacity
