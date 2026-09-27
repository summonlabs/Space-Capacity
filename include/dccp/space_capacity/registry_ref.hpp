// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - references to identities and hierarchy owned by other
// runtimes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Space Capacity owns no registry of physical things. Every facility,
// building, hall, row, rack, asset and policy it reasons about is named by an
// identity minted elsewhere, and Space Capacity stores that identity as an
// opaque, validated reference together with the generation and revision it
// observed.
//
// Four consequences are deliberate:
//
//   * a reference is not a copy. Space Capacity never restates an upstream
//     record's attributes as if it owned them. It records what it observed and
//     at which attempt, and it reports the observation as observed.
//   * a reference is not authority. Holding a reference to a rack does not
//     permit Space Capacity to declare the rack present, mounted or healthy.
//   * an identity is opaque and byte-preserved. The sibling runtimes disagree
//     about identity: Physical Location Registry and Facility Topology use
//     1..128-byte identifiers with a shared grammar, Rack Registry uses a
//     prefixed form such as "rack:a01", and Asset Registry uses a 36-character
//     UUID. Space Capacity accepts the widest of those (192 bytes) and never
//     reinterprets a value it was given.
//   * a lineage link is directional. When an upstream location is retired and
//     replaced, the reference records both the retirement and the successor so
//     that a capacity number computed over the old identity can be explained
//     rather than silently carried forward.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Source runtimes
// ---------------------------------------------------------------------------

// The runtimes whose identities Space Capacity consumes. The numeric values are
// persisted and are therefore stable.
enum class RegistryKind : std::uint8_t {
  none = 0,
  physical_location_registry = 1,
  facility_topology = 2,
  rack_registry = 3,
  asset_registry = 4,
  policy_registry = 5,
  facility_capacity = 6,
  power_capacity = 7,
  cooling_capacity = 8,
  facility_capacity_reservation = 9,
  facility_placement_planner = 10,
};

SC_API std::string_view registry_kind_name(RegistryKind kind) noexcept;
SC_API bool parse_registry_kind(std::string_view text, RegistryKind& out) noexcept;

// Lifecycle state of the referenced subject as observed at the source. Space
// Capacity stores the observation; it never infers the state, and it reports
// `unknown` rather than guessing `active`.
//
// The mapping from a sibling's own vocabulary is the caller's:
//   Physical Location Registry  Active/Retired/Replaced -> active/retired/replaced
//   Rack Registry               Defined..Active -> active, Retired/Removed -> retired
//   Asset Registry              Decommissioned/Disposed -> retired
//   Facility Topology           has no node lifecycle: a node exists or is gone
enum class UpstreamState : std::uint8_t {
  unknown = 0,
  active = 1,
  degraded = 2,
  retired = 3,
  replaced = 4,
};

SC_API std::string_view upstream_state_name(UpstreamState state) noexcept;
SC_API bool parse_upstream_state(std::string_view text, UpstreamState& out) noexcept;

// ---------------------------------------------------------------------------
// Opaque upstream identity
// ---------------------------------------------------------------------------

// An identity minted by another runtime, preserved byte for byte.
//
// The accepted character set is deliberately wider than Space Capacity's own
// identifier grammar because Asset Registry UUIDs contain hyphens, Rack
// Registry identities contain a colon, and Physical Location Registry paths
// contain slashes. Only printable ASCII without control characters is refused,
// plus the empty string and anything over the bound.
class SC_API ExternalId final {
 public:
  ExternalId() noexcept = default;

  [[nodiscard]] static Result<ExternalId> parse(std::string_view text);
  [[nodiscard]] static bool is_acceptable(std::string_view text) noexcept;

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string_view value() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const ExternalId& a, const ExternalId& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend bool operator!=(const ExternalId& a, const ExternalId& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const ExternalId& a, const ExternalId& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

// ---------------------------------------------------------------------------
// Tagged reference families
// ---------------------------------------------------------------------------

struct LocationRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::physical_location_registry;
  static constexpr std::string_view kKindName = "location";
};
struct FacilityNodeRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::facility_topology;
  static constexpr std::string_view kKindName = "facility-node";
};
struct RackRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::rack_registry;
  static constexpr std::string_view kKindName = "rack";
};
struct AssetRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::asset_registry;
  static constexpr std::string_view kKindName = "asset";
};
struct PolicyRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::policy_registry;
  static constexpr std::string_view kKindName = "policy";
};
struct ReservationRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::facility_capacity_reservation;
  static constexpr std::string_view kKindName = "reservation";
};
struct PlacementPlanRefTag final {
  static constexpr RegistryKind kRegistry = RegistryKind::facility_placement_planner;
  static constexpr std::string_view kKindName = "placement-plan";
};

// A reference to one record in one source runtime.
//
//   id              the identity minted by the source runtime
//   generation      the generation of that record as observed
//   source_revision the revision of the source runtime as observed
//   state           the lifecycle state of the subject as observed
//   successor       for a replaced subject, the identity that superseded it
//
// A reference is a value. Two references are equal only when all five fields
// are equal, so a reference observed at a later generation is a different
// value and cannot silently stand in for the earlier one.
template <typename Tag>
class StrongRef final {
 public:
  using tag_type = Tag;
  static constexpr RegistryKind registry_kind = Tag::kRegistry;

  StrongRef() noexcept = default;

  [[nodiscard]] static Result<StrongRef> of(std::string_view id_text) {
    Result<ExternalId> id = ExternalId::parse(id_text);
    if (!id) return id.error();
    StrongRef ref;
    ref.id_ = std::move(id).value();
    return ref;
  }

  [[nodiscard]] static Result<StrongRef> make(std::string_view id_text, std::uint64_t generation,
                                              UpstreamState state) {
    Result<StrongRef> ref = of(id_text);
    if (!ref) return ref.error();
    ref.value().set_generation(EntityGeneration{generation});
    ref.value().set_state(state);
    return ref;
  }

  [[nodiscard]] const ExternalId& id() const noexcept { return id_; }
  [[nodiscard]] EntityGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint64_t source_revision() const noexcept { return source_revision_; }
  [[nodiscard]] UpstreamState state() const noexcept { return state_; }
  [[nodiscard]] const ExternalId& successor() const noexcept { return successor_; }
  [[nodiscard]] bool has_successor() const noexcept { return !successor_.empty(); }
  [[nodiscard]] bool empty() const noexcept { return id_.empty(); }

  void set_generation(EntityGeneration value) noexcept { generation_ = value; }
  void set_source_revision(std::uint64_t value) noexcept { source_revision_ = value; }
  void set_state(UpstreamState value) noexcept { state_ = value; }
  void set_successor(ExternalId value) noexcept { successor_ = std::move(value); }

  // A reference is usable for accounting only when it names something and the
  // source did not report it retired or replaced. Retired subjects may still be
  // referenced for lineage and audit; where a live subject is required they are
  // refused with stale_authority, never silently accepted.
  [[nodiscard]] bool is_live() const noexcept {
    return !id_.empty() && (state_ == UpstreamState::active || state_ == UpstreamState::degraded ||
                            state_ == UpstreamState::unknown);
  }

  [[nodiscard]] std::string text() const { return id_.str(); }

  [[nodiscard]] friend bool operator==(const StrongRef& a, const StrongRef& b) noexcept {
    return a.id_ == b.id_ && a.generation_ == b.generation_ &&
           a.source_revision_ == b.source_revision_ && a.state_ == b.state_ &&
           a.successor_ == b.successor_;
  }
  [[nodiscard]] friend bool operator!=(const StrongRef& a, const StrongRef& b) noexcept {
    return !(a == b);
  }
  // Canonical order: identity, then generation, then source revision, then
  // state, then successor, so reference sets have a stable persisted order.
  [[nodiscard]] friend bool operator<(const StrongRef& a, const StrongRef& b) noexcept {
    if (a.id_ != b.id_) return a.id_ < b.id_;
    if (a.generation_ != b.generation_) return a.generation_ < b.generation_;
    if (a.source_revision_ != b.source_revision_) return a.source_revision_ < b.source_revision_;
    if (a.state_ != b.state_) return static_cast<int>(a.state_) < static_cast<int>(b.state_);
    return a.successor_ < b.successor_;
  }

 private:
  ExternalId id_{};
  EntityGeneration generation_{};
  std::uint64_t source_revision_ = 0;
  UpstreamState state_ = UpstreamState::unknown;
  ExternalId successor_{};
};

using LocationRef = StrongRef<LocationRefTag>;
using FacilityNodeRef = StrongRef<FacilityNodeRefTag>;
using RackRef = StrongRef<RackRefTag>;
using AssetRef = StrongRef<AssetRefTag>;
using PolicyRef = StrongRef<PolicyRefTag>;
using ReservationRef = StrongRef<ReservationRefTag>;
using PlacementPlanRef = StrongRef<PlacementPlanRefTag>;

// ---------------------------------------------------------------------------
// Bounded reference sets
// ---------------------------------------------------------------------------

// An ordered, deduplicated set of one reference family. The bound is enforced
// at construction, so a persisted record can never declare an unbounded list.
template <typename Ref>
class RefSet final {
 public:
  RefSet() = default;

  [[nodiscard]] static Result<RefSet> build(std::vector<Ref> refs) {
    if (refs.size() > Limits::kMaxReferencesPerRecord) {
      return Error::make(ErrorCode::limit_exceeded,
                         "reference set exceeds " +
                             std::to_string(Limits::kMaxReferencesPerRecord) + " entries");
    }
    for (const Ref& ref : refs) {
      if (ref.empty()) {
        return Error::make(ErrorCode::empty_value, "a reference set entry must not be empty");
      }
    }
    // Deterministic order with duplicate removal: insertion sort over the
    // canonical order, then unique.
    for (std::size_t i = 1; i < refs.size(); ++i) {
      Ref key = refs[i];
      std::size_t j = i;
      while (j > 0 && key < refs[j - 1]) {
        refs[j] = refs[j - 1];
        --j;
      }
      refs[j] = std::move(key);
    }
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    RefSet result;
    result.refs_ = std::move(refs);
    return result;
  }

  [[nodiscard]] const std::vector<Ref>& refs() const noexcept { return refs_; }
  [[nodiscard]] bool empty() const noexcept { return refs_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }

  [[nodiscard]] bool contains(const Ref& ref) const noexcept {
    for (const Ref& candidate : refs_) {
      if (candidate == ref) return true;
    }
    return false;
  }
  // True when the set holds any reference to `id`, whatever generation it was
  // observed at. Used for lineage questions, never for authority.
  [[nodiscard]] bool contains_identity(std::string_view id) const noexcept {
    for (const Ref& candidate : refs_) {
      if (candidate.id().value() == id) return true;
    }
    return false;
  }

  [[nodiscard]] friend bool operator==(const RefSet& a, const RefSet& b) noexcept {
    return a.refs_ == b.refs_;
  }
  [[nodiscard]] friend bool operator!=(const RefSet& a, const RefSet& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const RefSet& a, const RefSet& b) noexcept {
    return a.refs_ < b.refs_;
  }

 private:
  std::vector<Ref> refs_;
};

using LocationRefSet = RefSet<LocationRef>;
using FacilityNodeRefSet = RefSet<FacilityNodeRef>;
using RackRefSet = RefSet<RackRef>;
using AssetRefSet = RefSet<AssetRef>;
using PolicyRefSet = RefSet<PolicyRef>;

}  // namespace dccp::space_capacity
