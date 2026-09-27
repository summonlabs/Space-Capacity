// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - deterministic diffs between two revisions of a model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A diff reports, in canonical order, what changed between two revisions and
// what the change did to the capacity ledger. It is defined over whole records
// and over signed ledger deltas. Because every record family has a canonical
// order and a unique identity, the same two revisions always produce the same
// diff.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

enum class ChangeKind : std::uint8_t {
  added = 1,
  removed = 2,
  modified = 3,
};

SC_API std::string_view change_kind_name(ChangeKind value) noexcept;
SC_API bool parse_change_kind(std::string_view text, ChangeKind& out) noexcept;

// Stable names of the fields that can differ, so "what changed" is machine
// readable rather than prose. The names are part of the CLI contract and are
// never renumbered.
SC_API std::string_view node_field_name(std::uint32_t index) noexcept;
SC_API std::string_view claim_field_name(std::uint32_t index) noexcept;
SC_API std::string_view reservation_field_name(std::uint32_t index) noexcept;
SC_API std::string_view exclusion_field_name(std::uint32_t index) noexcept;
SC_API std::string_view clearance_field_name(std::uint32_t index) noexcept;
SC_API std::string_view expansion_zone_field_name(std::uint32_t index) noexcept;

template <typename Record, typename Id>
struct RecordChange final {
  ChangeKind kind = ChangeKind::modified;
  Id id{};
  std::optional<Record> before{};
  std::optional<Record> after{};
  // Sorted, deduplicated field names; empty for added and removed records.
  std::vector<std::string> fields{};

  [[nodiscard]] friend bool operator==(const RecordChange& a,
                                       const RecordChange& b) noexcept {
    return a.kind == b.kind && a.id == b.id && a.before == b.before && a.after == b.after &&
           a.fields == b.fields;
  }
  [[nodiscard]] friend bool operator!=(const RecordChange& a,
                                       const RecordChange& b) noexcept {
    return !(a == b);
  }
};

using NodeChange = RecordChange<SpaceNode, SpaceNodeId>;
using ClaimChange = RecordChange<OccupancyClaim, OccupancyClaimId>;
using ReservationChange = RecordChange<FootprintReservation, OccupancyClaimId>;
using ExclusionChange = RecordChange<ExclusionRegion, ExclusionRegionId>;
using ClearanceChange = RecordChange<ClearanceConstraint, ClearanceConstraintId>;
using ExpansionZoneChange = RecordChange<ExpansionZone, ExpansionZoneId>;

// Signed difference of two ledgers. Every field is `to - from`, so a negative
// number means the measure shrank. Exact integers; no floating point.
struct SC_API AreaLedgerDelta final {
  std::int64_t declared = 0;
  std::int64_t excluded = 0;
  std::int64_t usable = 0;
  std::int64_t structural = 0;
  std::int64_t claimed = 0;
  std::int64_t held = 0;
  std::int64_t earmarked = 0;
  std::int64_t available = 0;
  std::int64_t pending = 0;
  std::int64_t planned = 0;

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] friend bool operator==(const AreaLedgerDelta& a,
                                       const AreaLedgerDelta& b) noexcept;
  [[nodiscard]] friend bool operator!=(const AreaLedgerDelta& a,
                                       const AreaLedgerDelta& b) noexcept;
};

struct SC_API UnitLedgerDelta final {
  std::int64_t declared = 0;
  std::int64_t excluded = 0;
  std::int64_t usable = 0;
  std::int64_t occupied = 0;
  std::int64_t available = 0;
  std::int64_t pending = 0;
  std::int64_t planned = 0;

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] friend bool operator==(const UnitLedgerDelta& a,
                                       const UnitLedgerDelta& b) noexcept;
  [[nodiscard]] friend bool operator!=(const UnitLedgerDelta& a,
                                       const UnitLedgerDelta& b) noexcept;
};

SC_API AreaLedgerDelta subtract(const AreaLedger& from, const AreaLedger& to) noexcept;
SC_API UnitLedgerDelta subtract(const UnitLedger& from, const UnitLedger& to) noexcept;

struct SC_API CapacityDiff final {
  RegistryRevision from_revision{};
  RegistryRevision to_revision{};
  StoreIncarnation from_incarnation{};
  StoreIncarnation to_incarnation{};
  Digest from_digest{};
  Digest to_digest{};

  std::vector<NodeChange> nodes;
  std::vector<ClaimChange> claims;
  std::vector<ReservationChange> reservations;
  std::vector<ExclusionChange> exclusions;
  std::vector<ClearanceChange> clearances;
  std::vector<ExpansionZoneChange> expansion_zones;

  // Model-wide ledger movement, from the rollup at the whole-model root before
  // the change to the rollup at the whole-model root after it.
  AreaLedgerDelta area_delta{};
  UnitLedgerDelta unit_delta{};

  ExplanationSet explanations{};

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t change_count() const noexcept;

  [[nodiscard]] friend bool operator==(const CapacityDiff& a,
                                       const CapacityDiff& b) noexcept;
  [[nodiscard]] friend bool operator!=(const CapacityDiff& a,
                                       const CapacityDiff& b) noexcept;
};

}  // namespace dccp::space_capacity
