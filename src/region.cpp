// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - exclusions, service clearance and expansion zones.
//
// Three families that reduce what a declared envelope may be used for. Each
// carries a lifecycle whose legal moves are stated here once, and each is
// validated structurally: a record that names no extent, or an exclusion that
// excludes nothing, is refused rather than silently measured as zero.

#include "dccp/space_capacity/region.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/text.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {
namespace {

// The low bits of an occupant mask are exactly the bits that name an occupant
// kind. Every other bit is refused: a persisted mask has one meaning, and a
// stray high bit is a corrupt field rather than a wider mask.
constexpr std::uint32_t kOccupantKindBits =
    (std::uint32_t{1} << static_cast<std::uint32_t>(kOccupantKindCount)) - 1u;

// Every kind, and every kind except service access. Stated as the union of the
// kinds themselves so that a mask is always inside the occupant-kind range.
constexpr OccupantMask every_kind_except_service_access() noexcept {
  return OccupantMask::of(OccupantKind::unknown) | OccupantMask::of(OccupantKind::rack) |
         OccupantMask::of(OccupantKind::asset) | OccupantMask::of(OccupantKind::workload) |
         OccupantMask::of(OccupantKind::infrastructure);
}

// Persisted raw bounds. A one-based state never stores zero: zero is what an
// uninitialised or truncated field carries, and it is refused as an unknown
// token rather than read as the first state.
constexpr std::uint8_t kExclusionReasonMax = static_cast<std::uint8_t>(ExclusionReason::operational);
constexpr std::uint8_t kExclusionStateMin = static_cast<std::uint8_t>(ExclusionState::draft);
constexpr std::uint8_t kExclusionStateMax = static_cast<std::uint8_t>(ExclusionState::retired);
constexpr std::uint8_t kClearanceKindMin = static_cast<std::uint8_t>(ClearanceKind::front_service);
constexpr std::uint8_t kClearanceKindMax = static_cast<std::uint8_t>(ClearanceKind::rack_unit_access);
constexpr std::uint8_t kExpansionStateMin = static_cast<std::uint8_t>(ExpansionState::identified);
constexpr std::uint8_t kExpansionStateMax = static_cast<std::uint8_t>(ExpansionState::retired);

// Renders a 32-bit mask as a fixed-width hexadecimal token, so that a refusal
// names the offending bits instead of an opaque decimal number. Never locale
// dependent.
std::string mask_text(std::uint32_t bits) {
  std::string text = "0x";
  for (int shift = 28; shift >= 0; shift -= 4) {
    const std::uint32_t nibble = (bits >> static_cast<unsigned int>(shift)) & 0x0Fu;
    const char digit = (nibble < 10u) ? static_cast<char>('0' + nibble)
                                      : static_cast<char>('a' + (nibble - 10u));
    text.push_back(digit);
  }
  return text;
}

// Free text carried by a record: bounded by kMaxLabelBytes or kMaxNoteBytes and
// already valid UTF-8.
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

Status unknown_value(std::string_view what, std::uint8_t value) {
  return Status::failure(ErrorCode::unknown_enum_token,
                         std::string(what) + " value " +
                             to_text(static_cast<std::uint64_t>(value)) +
                             " is not a known value");
}

}  // namespace

// ---------------------------------------------------------------------------
// Occupant masks
// ---------------------------------------------------------------------------

Result<OccupantMask> OccupantMask::from_bits(std::uint32_t bits) {
  const std::uint32_t offending = bits & ~kOccupantKindBits;
  if (offending != 0) {
    return Error::make(ErrorCode::unknown_enum_token,
                       "occupant mask " + mask_text(bits) + " sets " + mask_text(offending) +
                           ", which is outside the " +
                           to_text(static_cast<std::uint64_t>(kOccupantKindCount)) +
                           " occupant kinds this version understands");
  }
  OccupantMask mask;
  mask.bits_ = bits;
  return mask;
}

// ---------------------------------------------------------------------------
// Exclusion regions
// ---------------------------------------------------------------------------

std::string_view exclusion_reason_name(ExclusionReason value) noexcept {
  switch (value) {
    case ExclusionReason::unspecified:
      return "unspecified";
    case ExclusionReason::structural:
      return "structural";
    case ExclusionReason::thermal:
      return "thermal";
    case ExclusionReason::electrical:
      return "electrical";
    case ExclusionReason::regulatory:
      return "regulatory";
    case ExclusionReason::service_clearance:
      return "service_clearance";
    case ExclusionReason::incompatible_equipment:
      return "incompatible_equipment";
    case ExclusionReason::reserved_expansion:
      return "reserved_expansion";
    case ExclusionReason::decommissioned:
      return "decommissioned";
    case ExclusionReason::safety:
      return "safety";
    case ExclusionReason::operational:
      return "operational";
  }
  return "unknown";
}

bool parse_exclusion_reason(std::string_view text, ExclusionReason& out) noexcept {
  if (text == "unspecified") {
    out = ExclusionReason::unspecified;
    return true;
  }
  if (text == "structural") {
    out = ExclusionReason::structural;
    return true;
  }
  if (text == "thermal") {
    out = ExclusionReason::thermal;
    return true;
  }
  if (text == "electrical") {
    out = ExclusionReason::electrical;
    return true;
  }
  if (text == "regulatory") {
    out = ExclusionReason::regulatory;
    return true;
  }
  if (text == "service_clearance") {
    out = ExclusionReason::service_clearance;
    return true;
  }
  if (text == "incompatible_equipment") {
    out = ExclusionReason::incompatible_equipment;
    return true;
  }
  if (text == "reserved_expansion") {
    out = ExclusionReason::reserved_expansion;
    return true;
  }
  if (text == "decommissioned") {
    out = ExclusionReason::decommissioned;
    return true;
  }
  if (text == "safety") {
    out = ExclusionReason::safety;
    return true;
  }
  if (text == "operational") {
    out = ExclusionReason::operational;
    return true;
  }
  return false;
}

ExclusionReason exclusion_reason_from_value(std::uint8_t value) noexcept {
  if (value > kExclusionReasonMax) return ExclusionReason::unspecified;
  return static_cast<ExclusionReason>(value);
}

std::string_view exclusion_state_name(ExclusionState value) noexcept {
  switch (value) {
    case ExclusionState::draft:
      return "draft";
    case ExclusionState::active:
      return "active";
    case ExclusionState::suspended:
      return "suspended";
    case ExclusionState::retired:
      return "retired";
  }
  return "unknown";
}

bool parse_exclusion_state(std::string_view text, ExclusionState& out) noexcept {
  if (text == "draft") {
    out = ExclusionState::draft;
    return true;
  }
  if (text == "active") {
    out = ExclusionState::active;
    return true;
  }
  if (text == "suspended") {
    out = ExclusionState::suspended;
    return true;
  }
  if (text == "retired") {
    out = ExclusionState::retired;
    return true;
  }
  return false;
}

ExclusionState exclusion_state_from_value(std::uint8_t value) noexcept {
  if (value < kExclusionStateMin || value > kExclusionStateMax) return ExclusionState::draft;
  return static_cast<ExclusionState>(value);
}

bool exclusion_state_blocks(ExclusionState value) noexcept {
  // Only an active exclusion removes space. A draft has not been decided, a
  // suspended one is not in force, and a retired one is history.
  return value == ExclusionState::active;
}

bool exclusion_transition_allowed(ExclusionState from, ExclusionState to) noexcept {
  if (from == to) return false;
  switch (from) {
    case ExclusionState::draft:
      return to == ExclusionState::active || to == ExclusionState::retired;
    case ExclusionState::active:
      return to == ExclusionState::suspended || to == ExclusionState::retired;
    case ExclusionState::suspended:
      // A suspended exclusion may be put back in force, or abandoned.
      return to == ExclusionState::active || to == ExclusionState::retired;
    case ExclusionState::retired:
      return false;
  }
  return false;
}

OccupantMask default_mask_for_reason(ExclusionReason reason) noexcept {
  // The default mask table. A region may narrow or widen this explicitly; this
  // is what an operator gets when they state only a reason.
  //
  //   reason                  | blocked occupant kinds
  //   ------------------------|--------------------------------------------
  //   unspecified             | none
  //   structural              | every occupant kind
  //   thermal                 | every occupant kind except service_access
  //   electrical              | every occupant kind except service_access
  //   regulatory              | every occupant kind
  //   service_clearance       | service_access only
  //   incompatible_equipment  | every occupant kind except service_access
  //   reserved_expansion      | rack, asset, workload, infrastructure
  //   decommissioned          | every occupant kind
  //   safety                  | every occupant kind
  //   operational             | every occupant kind except service_access
  //
  // The rows follow from what each reason is about. A statement about the
  // space itself (structural, regulatory, decommissioned, safety) blocks
  // everything that could be put there, including the access needed to work on
  // it. A statement about what the space can host (thermal, electrical,
  // incompatible equipment, operational) blocks every occupant but leaves
  // service access open, because someone still has to reach the equipment. A
  // service clearance blocks only service access: it is the access itself that
  // is being reserved. A reserved expansion keeps the space for a build-out, so
  // it blocks everything that would occupy it, but not the access around it.
  switch (reason) {
    case ExclusionReason::unspecified:
      return OccupantMask{};
    case ExclusionReason::service_clearance:
      return OccupantMask::of(OccupantKind::service_access);
    case ExclusionReason::reserved_expansion:
      return OccupantMask::of(OccupantKind::rack) | OccupantMask::of(OccupantKind::asset) |
             OccupantMask::of(OccupantKind::workload) |
             OccupantMask::of(OccupantKind::infrastructure);
    case ExclusionReason::structural:
    case ExclusionReason::regulatory:
    case ExclusionReason::decommissioned:
    case ExclusionReason::safety:
      return OccupantMask::all();
    case ExclusionReason::thermal:
    case ExclusionReason::electrical:
    case ExclusionReason::incompatible_equipment:
    case ExclusionReason::operational:
      return every_kind_except_service_access();
  }
  // A reason this version does not know blocks nothing by default: an unknown
  // reason is never widened into a blanket exclusion.
  return OccupantMask{};
}

bool operator==(const ExclusionRegion& a, const ExclusionRegion& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.node == b.node && a.scope == b.scope &&
         a.reason == b.reason && a.state == b.state && a.blocks == b.blocks && a.label == b.label &&
         a.note == b.note && a.policies == b.policies && a.subjects == b.subjects &&
         a.evidence == b.evidence;
}

Status validate_exclusion_shape(const ExclusionRegion& region) {
  if (region.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "an exclusion must carry an identity");
  }
  if (region.node.empty()) {
    return Status::failure(ErrorCode::empty_value, "an exclusion must name the node it excludes");
  }

  const std::uint8_t reason = static_cast<std::uint8_t>(region.reason);
  if (reason > kExclusionReasonMax) {
    return unknown_value("exclusion reason", reason);
  }
  const std::uint8_t state = static_cast<std::uint8_t>(region.state);
  if (state < kExclusionStateMin || state > kExclusionStateMax) {
    return unknown_value("exclusion state", state);
  }

  const Status scope = region.scope.validate_shape();
  if (!scope) return scope;

  const Status label =
      validate_record_text(region.label.value(), Limits::kMaxLabelBytes, "an exclusion label");
  if (!label) return label;
  const Status note =
      validate_record_text(region.note.value(), Limits::kMaxNoteBytes, "an exclusion note");
  if (!note) return note;

  if (region.blocks.empty()) {
    return Status::failure(ErrorCode::invalid_argument,
                           "an exclusion that excludes nothing is a modelling error");
  }
  // A draft or retired exclusion blocks nothing in the capacity engine; that is
  // legal and is not a second empty-mask rule. The mask itself must still name
  // only occupant kinds this version understands.
  const Result<OccupantMask> mask = OccupantMask::from_bits(region.blocks.bits());
  if (!mask) return Status(mask.error());
  return Status::success();
}

// ---------------------------------------------------------------------------
// Service clearance
// ---------------------------------------------------------------------------

std::string_view clearance_kind_name(ClearanceKind value) noexcept {
  switch (value) {
    case ClearanceKind::front_service:
      return "front_service";
    case ClearanceKind::rear_service:
      return "rear_service";
    case ClearanceKind::side_service:
      return "side_service";
    case ClearanceKind::overhead_service:
      return "overhead_service";
    case ClearanceKind::rack_unit_access:
      return "rack_unit_access";
  }
  return "unknown";
}

bool parse_clearance_kind(std::string_view text, ClearanceKind& out) noexcept {
  if (text == "front_service") {
    out = ClearanceKind::front_service;
    return true;
  }
  if (text == "rear_service") {
    out = ClearanceKind::rear_service;
    return true;
  }
  if (text == "side_service") {
    out = ClearanceKind::side_service;
    return true;
  }
  if (text == "overhead_service") {
    out = ClearanceKind::overhead_service;
    return true;
  }
  if (text == "rack_unit_access") {
    out = ClearanceKind::rack_unit_access;
    return true;
  }
  return false;
}

ClearanceKind clearance_kind_from_value(std::uint8_t value) noexcept {
  if (value < kClearanceKindMin || value > kClearanceKindMax) return ClearanceKind::front_service;
  return static_cast<ClearanceKind>(value);
}

bool clearance_kind_is_planar(ClearanceKind value) noexcept {
  // The four service bands are measured in the plane of the nearest
  // plane-owning ancestor; only rack unit access is measured vertically.
  return value == ClearanceKind::front_service || value == ClearanceKind::rear_service ||
         value == ClearanceKind::side_service || value == ClearanceKind::overhead_service;
}

bool clearance_kind_is_vertical(ClearanceKind value) noexcept {
  return value == ClearanceKind::rack_unit_access;
}

bool operator==(const ClearanceConstraint& a, const ClearanceConstraint& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.node == b.node && a.kind == b.kind &&
         a.band == b.band && a.has_band == b.has_band &&
         a.required_free_units == b.required_free_units && a.has_units == b.has_units &&
         a.enforceable == b.enforceable && a.label == b.label && a.note == b.note &&
         a.policies == b.policies && a.evidence == b.evidence;
}

Status validate_clearance_shape(const ClearanceConstraint& constraint) {
  if (constraint.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "a clearance must carry an identity");
  }
  if (constraint.node.empty()) {
    return Status::failure(ErrorCode::empty_value, "a clearance must name the node it protects");
  }

  const std::uint8_t kind = static_cast<std::uint8_t>(constraint.kind);
  if (kind < kClearanceKindMin || kind > kClearanceKindMax) {
    return unknown_value("clearance kind", kind);
  }

  const Status label =
      validate_record_text(constraint.label.value(), Limits::kMaxLabelBytes, "a clearance label");
  if (!label) return label;
  const Status note =
      validate_record_text(constraint.note.value(), Limits::kMaxNoteBytes, "a clearance note");
  if (!note) return note;

  // Exactly one form is present, and it must be the form the kind is measured
  // in. A planar band described in rack units, or a vertical clearance with no
  // unit set, would otherwise be counted as a band of zero.
  if (clearance_kind_is_planar(constraint.kind)) {
    if (!constraint.has_band || constraint.has_units) {
      return Status::failure(
          ErrorCode::scope_kind_mismatch,
          "a " + std::string(clearance_kind_name(constraint.kind)) +
              " clearance is planar: it requires a band rectangle and no rack unit intervals");
    }
    if (!constraint.band.is_valid()) {
      return Status::failure(ErrorCode::invalid_extent,
                             "clearance band " + to_text(constraint.band) +
                                 " is degenerate or outside the millimetre bounds");
    }
    return Status::success();
  }
  if (clearance_kind_is_vertical(constraint.kind)) {
    if (!constraint.has_units || constraint.has_band) {
      return Status::failure(
          ErrorCode::scope_kind_mismatch,
          "a " + std::string(clearance_kind_name(constraint.kind)) +
              " clearance is vertical: it requires rack unit intervals and no band rectangle");
    }
    if (constraint.required_free_units.empty()) {
      return Status::failure(ErrorCode::invalid_extent,
                             "a " + std::string(clearance_kind_name(constraint.kind)) +
                                 " clearance must keep at least one rack unit free");
    }
    return Status::success();
  }
  return unknown_value("clearance kind", kind);
}

// ---------------------------------------------------------------------------
// Expansion zones
// ---------------------------------------------------------------------------

std::string_view expansion_state_name(ExpansionState value) noexcept {
  switch (value) {
    case ExpansionState::identified:
      return "identified";
    case ExpansionState::planned:
      return "planned";
    case ExpansionState::funded:
      return "funded";
    case ExpansionState::active:
      return "active";
    case ExpansionState::consumed:
      return "consumed";
    case ExpansionState::retired:
      return "retired";
  }
  return "unknown";
}

bool parse_expansion_state(std::string_view text, ExpansionState& out) noexcept {
  if (text == "identified") {
    out = ExpansionState::identified;
    return true;
  }
  if (text == "planned") {
    out = ExpansionState::planned;
    return true;
  }
  if (text == "funded") {
    out = ExpansionState::funded;
    return true;
  }
  if (text == "active") {
    out = ExpansionState::active;
    return true;
  }
  if (text == "consumed") {
    out = ExpansionState::consumed;
    return true;
  }
  if (text == "retired") {
    out = ExpansionState::retired;
    return true;
  }
  return false;
}

ExpansionState expansion_state_from_value(std::uint8_t value) noexcept {
  if (value < kExpansionStateMin || value > kExpansionStateMax) return ExpansionState::identified;
  return static_cast<ExpansionState>(value);
}

bool expansion_state_earmarks(ExpansionState value) noexcept {
  return value == ExpansionState::identified || value == ExpansionState::planned ||
         value == ExpansionState::funded || value == ExpansionState::active;
}

bool expansion_state_is_planned(ExpansionState value) noexcept {
  // Earmarking and planning are deliberately different questions. An `active`
  // build-out still earmarks the space: the area is a construction site and
  // ordinary availability must not include it. It is no longer merely planned,
  // though, so it is reported as committed future capacity rather than as a
  // plan. `consumed` and `retired` do neither: the space is ordinary again.
  return value == ExpansionState::identified || value == ExpansionState::planned ||
         value == ExpansionState::funded;
}

bool expansion_transition_allowed(ExpansionState from, ExpansionState to) noexcept {
  if (from == to) return false;
  switch (from) {
    case ExpansionState::identified:
      return to == ExpansionState::planned || to == ExpansionState::funded ||
             to == ExpansionState::retired;
    case ExpansionState::planned:
      return to == ExpansionState::identified || to == ExpansionState::funded ||
             to == ExpansionState::active || to == ExpansionState::retired;
    case ExpansionState::funded:
      // Funding may be withdrawn back to a plan, or the build-out may start.
      return to == ExpansionState::planned || to == ExpansionState::active ||
             to == ExpansionState::retired;
    case ExpansionState::active:
      // A build-out may be paused back to funded, completed into consumed, or
      // abandoned into retired.
      return to == ExpansionState::funded || to == ExpansionState::consumed ||
             to == ExpansionState::retired;
    case ExpansionState::consumed:
    case ExpansionState::retired:
      return false;
  }
  return false;
}

bool operator==(const ExpansionZone& a, const ExpansionZone& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.node == b.node && a.scope == b.scope &&
         a.state == b.state && a.label == b.label && a.note == b.note &&
         a.policies == b.policies && a.evidence == b.evidence &&
         a.target_ready_by == b.target_ready_by;
}

Status validate_expansion_zone_shape(const ExpansionZone& zone) {
  if (zone.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "an expansion zone must carry an identity");
  }
  if (zone.node.empty()) {
    return Status::failure(ErrorCode::empty_value, "an expansion zone must name the node it "
                                                   "earmarks");
  }

  const std::uint8_t state = static_cast<std::uint8_t>(zone.state);
  if (state < kExpansionStateMin || state > kExpansionStateMax) {
    return unknown_value("expansion state", state);
  }

  const Status scope = zone.scope.validate_shape();
  if (!scope) return scope;

  const Status label =
      validate_record_text(zone.label.value(), Limits::kMaxLabelBytes, "an expansion zone label");
  if (!label) return label;
  const Status note =
      validate_record_text(zone.note.value(), Limits::kMaxNoteBytes, "an expansion zone note");
  if (!note) return note;

  // A readiness target of the Unix epoch is an unset field that arrived as a
  // number, not a target date.
  if (zone.target_ready_by.has_value() && zone.target_ready_by->is_zero()) {
    return Status::failure(ErrorCode::invalid_timestamp,
                           "a target readiness instant must not be the Unix epoch");
  }
  return Status::success();
}

}  // namespace dccp::space_capacity
