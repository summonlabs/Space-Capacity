// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - exclusions, service clearance and expansion zones.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Three record families reduce what a declared envelope can be used for:
//
//   * an ExclusionRegion says that part of a plane, or part of a rack's
//     vertical envelope, must not be used, and optionally says for which kinds
//     of occupant. An exclusion is not a conflict: it is a standing statement
//     about the space, and it subtracts from usable capacity whether or not
//     anything is there now. Two exclusions that overlap are measured once,
//     because exclusion is measured by union.
//
//   * a ClearanceConstraint says how much space must stay clear for service
//     access. When a constraint is enforceable the capacity engine subtracts
//     its band and refuses occupancy inside the band. When it is not
//     enforceable the band is reported and subtracted from nothing, and every
//     report says which of the two it was. Nothing is ever silently ignored.
//
//   * an ExpansionZone says that space which is physically free has already
//     been earmarked by a future build-out programme. Space in an earmarking
//     zone subtracts from ordinary availability and is reported separately, so
//     that "free floor" never silently includes area a construction programme
//     has already claimed. A zone that has been consumed or retired is
//     ordinary space again.
//
// None of the three families grants permission to build anything.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Occupant masks
// ---------------------------------------------------------------------------

// A set of OccupantKind values, stored as a portable 32-bit mask so a
// persisted mask has exactly one meaning on every platform.
class SC_API OccupantMask final {
 public:
  constexpr OccupantMask() noexcept = default;

  [[nodiscard]] static constexpr OccupantMask of(OccupantKind kind) noexcept {
    OccupantMask mask;
    mask.bits_ = static_cast<std::uint32_t>(1) << static_cast<std::uint32_t>(kind);
    return mask;
  }
  [[nodiscard]] static constexpr OccupantMask all() noexcept {
    // Every occupant kind, and no bit outside the kind range, so that a mask
    // produced by all() is always accepted by from_bits().
    OccupantMask mask;
    mask.bits_ = (static_cast<std::uint32_t>(1) << kOccupantKindCount) - 1u;
    return mask;
  }
  [[nodiscard]] static Result<OccupantMask> from_bits(std::uint32_t bits);

  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr bool contains(OccupantKind kind) const noexcept {
    return (bits_ & (static_cast<std::uint32_t>(1) << static_cast<std::uint32_t>(kind))) != 0;
  }

  [[nodiscard]] constexpr OccupantMask operator|(OccupantMask other) const noexcept {
    OccupantMask mask;
    mask.bits_ = bits_ | other.bits_;
    return mask;
  }
  [[nodiscard]] friend constexpr bool operator==(OccupantMask a, OccupantMask b) noexcept {
    return a.bits_ == b.bits_;
  }
  [[nodiscard]] friend constexpr bool operator!=(OccupantMask a, OccupantMask b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(OccupantMask a, OccupantMask b) noexcept {
    return a.bits_ < b.bits_;
  }

 private:
  std::uint32_t bits_ = 0;
};

// ---------------------------------------------------------------------------
// Exclusion regions
// ---------------------------------------------------------------------------

enum class ExclusionReason : std::uint8_t {
  unspecified = 0,
  structural = 1,
  thermal = 2,
  electrical = 3,
  regulatory = 4,
  service_clearance = 5,
  incompatible_equipment = 6,
  reserved_expansion = 7,
  decommissioned = 8,
  safety = 9,
  operational = 10,
};

SC_API std::string_view exclusion_reason_name(ExclusionReason value) noexcept;
SC_API bool parse_exclusion_reason(std::string_view text, ExclusionReason& out) noexcept;
SC_API ExclusionReason exclusion_reason_from_value(std::uint8_t value) noexcept;

enum class ExclusionState : std::uint8_t {
  draft = 1,
  active = 2,
  suspended = 3,
  retired = 4,
};

SC_API std::string_view exclusion_state_name(ExclusionState value) noexcept;
SC_API bool parse_exclusion_state(std::string_view text, ExclusionState& out) noexcept;
SC_API ExclusionState exclusion_state_from_value(std::uint8_t value) noexcept;

// True when the state removes space from usable capacity.
SC_API bool exclusion_state_blocks(ExclusionState value) noexcept;
SC_API bool exclusion_transition_allowed(ExclusionState from, ExclusionState to) noexcept;

// The mask of occupant kinds a reason blocks by default. A region may narrow
// or widen this explicitly; the default is what an operator gets when they
// state only a reason.
SC_API OccupantMask default_mask_for_reason(ExclusionReason reason) noexcept;

struct SC_API ExclusionRegion final {
  ExclusionRegionId id{};
  EntityGeneration generation{};

  SpaceNodeId node{};
  FootprintScope scope{};
  ExclusionReason reason = ExclusionReason::unspecified;
  ExclusionState state = ExclusionState::draft;

  // Which occupant kinds this region blocks. Empty blocks nothing and is
  // refused: an exclusion that excludes nothing is a modelling error.
  OccupantMask blocks{};

  DisplayLabel label{};
  Note note{};

  PolicyRefSet policies{};
  AssetRefSet subjects{};
  EvidenceSet evidence{};

  [[nodiscard]] bool blocks_now() const noexcept { return exclusion_state_blocks(state); }

  [[nodiscard]] friend bool operator==(const ExclusionRegion& a,
                                       const ExclusionRegion& b) noexcept;
  [[nodiscard]] friend bool operator!=(const ExclusionRegion& a,
                                       const ExclusionRegion& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const ExclusionRegion& a,
                                      const ExclusionRegion& b) noexcept {
    return a.id < b.id;
  }
};

SC_API Status validate_exclusion_shape(const ExclusionRegion& region);

// ---------------------------------------------------------------------------
// Service clearance
// ---------------------------------------------------------------------------

enum class ClearanceKind : std::uint8_t {
  front_service = 1,     // free planar band in front of the protected node
  rear_service = 2,      // free planar band behind it
  side_service = 3,      // free planar band to one side
  overhead_service = 4,  // free planar band above it
  rack_unit_access = 5,  // rack units that must stay free for access
};

SC_API std::string_view clearance_kind_name(ClearanceKind value) noexcept;
SC_API bool parse_clearance_kind(std::string_view text, ClearanceKind& out) noexcept;
SC_API ClearanceKind clearance_kind_from_value(std::uint8_t value) noexcept;

SC_API bool clearance_kind_is_planar(ClearanceKind value) noexcept;
SC_API bool clearance_kind_is_vertical(ClearanceKind value) noexcept;

struct SC_API ClearanceConstraint final {
  ClearanceConstraintId id{};
  EntityGeneration generation{};

  // The node whose service access this constraint protects. The band is
  // expressed in the plane of the nearest plane-owning ancestor, so a
  // clearance around a rack reduces the floor the rack stands on.
  SpaceNodeId node{};
  ClearanceKind kind = ClearanceKind::front_service;

  // Planar form, an absolute rectangle in the plane owner's coordinates.
  PlanarRect band{};
  bool has_band = false;

  // Vertical form.
  IntervalSet required_free_units{};
  bool has_units = false;

  // When true the capacity engine subtracts the band and refuses occupancy
  // inside it. When false the band is reported and subtracted from nothing.
  bool enforceable = true;

  DisplayLabel label{};
  Note note{};

  PolicyRefSet policies{};
  EvidenceSet evidence{};

  [[nodiscard]] friend bool operator==(const ClearanceConstraint& a,
                                       const ClearanceConstraint& b) noexcept;
  [[nodiscard]] friend bool operator!=(const ClearanceConstraint& a,
                                       const ClearanceConstraint& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const ClearanceConstraint& a,
                                      const ClearanceConstraint& b) noexcept {
    return a.id < b.id;
  }
};

SC_API Status validate_clearance_shape(const ClearanceConstraint& constraint);

// ---------------------------------------------------------------------------
// Expansion zones
// ---------------------------------------------------------------------------

enum class ExpansionState : std::uint8_t {
  identified = 1,  // the space has been identified as an expansion candidate
  planned = 2,     // the expansion is planned
  funded = 3,      // the expansion is funded
  active = 4,      // build-out is under way
  consumed = 5,    // the expansion was realized; the space is ordinary again
  retired = 6,     // the expansion was abandoned; the space is ordinary again
};

SC_API std::string_view expansion_state_name(ExpansionState value) noexcept;
SC_API bool parse_expansion_state(std::string_view text, ExpansionState& out) noexcept;
SC_API ExpansionState expansion_state_from_value(std::uint8_t value) noexcept;

// True when the zone removes space from ordinary availability.
SC_API bool expansion_state_earmarks(ExpansionState value) noexcept;
// True when the zone counts as planned future capacity.
SC_API bool expansion_state_is_planned(ExpansionState value) noexcept;
SC_API bool expansion_transition_allowed(ExpansionState from, ExpansionState to) noexcept;

struct SC_API ExpansionZone final {
  ExpansionZoneId id{};
  EntityGeneration generation{};

  SpaceNodeId node{};
  FootprintScope scope{};
  ExpansionState state = ExpansionState::identified;

  DisplayLabel label{};
  Note note{};

  PolicyRefSet policies{};
  EvidenceSet evidence{};

  // Declared readiness target. Stored and compared against a caller supplied
  // reading; never against an internally read clock.
  std::optional<Timestamp> target_ready_by{};

  [[nodiscard]] bool earmarks() const noexcept { return expansion_state_earmarks(state); }

  [[nodiscard]] friend bool operator==(const ExpansionZone& a,
                                       const ExpansionZone& b) noexcept;
  [[nodiscard]] friend bool operator!=(const ExpansionZone& a,
                                       const ExpansionZone& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const ExpansionZone& a,
                                      const ExpansionZone& b) noexcept {
    return a.id < b.id;
  }
};

SC_API Status validate_expansion_zone_shape(const ExpansionZone& zone);

}  // namespace dccp::space_capacity
