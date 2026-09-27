// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the containment model of physical space.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A SpaceNode is one named region of physical space with a stable identity, a
// generation, a position in a containment tree, a declared envelope and a
// lifecycle. Space Capacity owns the node identity and the envelope
// arithmetic; it does not own the physical thing the node stands for.
//
// ---------------------------------------------------------------------------
// Levels may be skipped
// ---------------------------------------------------------------------------
//
// The DCCP sibling hierarchies both allow a level to be skipped: Facility
// Topology lets a facility contain a rack directly, and Physical Location
// Registry's containment table does the same. Space Capacity follows that
// rule. Canonical depth is a property of the kind; the stored depth is the
// distance from the root, which may be smaller. Nothing walks a fixed chain.
//
// ---------------------------------------------------------------------------
// Planes
// ---------------------------------------------------------------------------
//
// A node either declares a plane of its own or is a grouping node:
//
//   * a plane-declaring node states the planar area it offers (own_planar),
//     the vertical envelope it offers (own_rack), or both;
//   * a grouping node states neither. A row of racks that has no floor of its
//     own is a grouping node.
//
// A node's placement is where the node sits inside its parent's envelope. The
// area of a placement is consumed from the nearest ancestor that declares a
// plane; that ancestor is the node's plane owner. Every placement and every
// planar record therefore resolves to exactly one plane.
//
// When a plane-declaring node sits below another plane-declaring node, its
// plane is a SUBDIVISION of the plane above: its placement must be inside the
// parent plane, and its declared area must equal the area of that placement.
// The parent then already counts the whole subdivided area as consumed, which
// is what keeps declared area from being counted twice in a subtree rollup.
//
// ---------------------------------------------------------------------------
// Identity, generation and lineage
// ---------------------------------------------------------------------------
//
// A node identity is stable for the life of the physical region it names. The
// generation advances whenever mutable metadata changes. A retired node stays
// in the model with its history; when an upstream location is replaced, the
// node records the successor identity so that a capacity query over the old
// identity is answered with "replaced by X" rather than a stale number.
//
// Facility Topology has no per-node lifecycle at all: a node exists or it is
// removed. Space Capacity therefore models retirement as a capacity event on
// its own record and never asks an upstream registry whether a node is
// retiring.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

// Containment kinds, ordered from the root downwards. The numeric values are
// persisted and are therefore stable.
enum class SpaceNodeKind : std::uint8_t {
  none = 0,
  site = 1,
  building = 2,
  hall = 3,
  row = 4,
  rack = 5,
  rack_unit_band = 6,
};

inline constexpr std::size_t kSpaceNodeKindCount = 7;

// Canonical depth of each kind. A node's stored depth is its distance from the
// root and may be smaller than this, because levels may be skipped.
SC_API std::uint32_t canonical_depth(SpaceNodeKind kind) noexcept;
SC_API std::string_view space_node_kind_name(SpaceNodeKind kind) noexcept;
SC_API bool parse_space_node_kind(std::string_view text, SpaceNodeKind& out) noexcept;
SC_API SpaceNodeKind space_node_kind_from_value(std::uint8_t value) noexcept;

// Whether `parent` may contain `child`.
SC_API bool kind_may_contain(SpaceNodeKind parent, SpaceNodeKind child) noexcept;

// Whether a node of this kind may be a root. Only a site may.
SC_API bool kind_may_be_root(SpaceNodeKind kind) noexcept;
// Whether a node of this kind may declare a planar envelope.
SC_API bool kind_may_declare_plane(SpaceNodeKind kind) noexcept;
// Whether a node of this kind may declare a vertical rack envelope.
SC_API bool kind_may_declare_rack_envelope(SpaceNodeKind kind) noexcept;
// Whether a node of this kind may declare a placement with a base rectangle.
SC_API bool kind_may_have_base_rect(SpaceNodeKind kind) noexcept;
// Whether a node of this kind may declare a placement with a rack unit span.
SC_API bool kind_may_have_u_span(SpaceNodeKind kind) noexcept;

// The spatial class of a node. Declared, never inferred. `unspecified` is
// refused for a persisted node: an unknown class is not a class.
enum class SpatialClass : std::uint8_t {
  unspecified = 0,
  floor = 1,     // open floor area inside a building envelope
  rack = 2,      // inside a rack envelope
  aisle = 3,     // aisle floor between rows
  overhead = 4,  // overhead volume above the floor
  outdoor = 5,   // outside any building envelope
  service = 6,   // service corridor or clearance lane
  enclosed = 7,  // enclosed room inside a hall
  cage = 8,      // caged area inside a hall
};

SC_API std::string_view spatial_class_name(SpatialClass value) noexcept;
SC_API bool parse_spatial_class(std::string_view text, SpatialClass& out) noexcept;

// Whether a spatial class is admissible for a node kind.
SC_API bool spatial_class_allowed(SpaceNodeKind kind, SpatialClass value) noexcept;

// Node lifecycle. Only `available` and `restricted` contribute to capacity
// that exists now; `planned` and `provisioning` contribute to planned
// capacity; `decommissioning` contributes nothing usable and says so;
// `retired` and `replaced` contribute zero and must be explained.
enum class NodeLifecycle : std::uint8_t {
  planned = 1,
  provisioning = 2,
  available = 3,
  restricted = 4,
  decommissioning = 5,
  retired = 6,
  replaced = 7,
};

inline constexpr std::size_t kNodeLifecycleCount = 8;

SC_API std::string_view node_lifecycle_name(NodeLifecycle value) noexcept;
SC_API bool parse_node_lifecycle(std::string_view text, NodeLifecycle& out) noexcept;
SC_API NodeLifecycle node_lifecycle_from_value(std::uint8_t value) noexcept;

// True when the node's own envelope is usable space now.
SC_API bool lifecycle_is_usable_now(NodeLifecycle value) noexcept;
// True when the node's own envelope is counted as future capacity.
SC_API bool lifecycle_is_planned(NodeLifecycle value) noexcept;
// True when the node can no longer receive new occupancy of any kind.
SC_API bool lifecycle_is_terminal(NodeLifecycle value) noexcept;
// Legal lifecycle transitions. Self transitions are never legal.
SC_API bool lifecycle_transition_allowed(NodeLifecycle from, NodeLifecycle to) noexcept;
// Why a transition was refused, for a stable explanation.
SC_API std::string_view lifecycle_transition_refusal(NodeLifecycle from, NodeLifecycle to) noexcept;

// ---------------------------------------------------------------------------
// Envelopes
// ---------------------------------------------------------------------------

// The planar envelope a node declares for its own plane.
struct SC_API PlanarEnvelope final {
  // Total declared area. Must be consistent with the rectangle set: when
  // rectangles are declared, the declared area must be at least the sum of
  // their areas, and the difference is area declared but not subdivided.
  SquareMillimeters declared_area{};
  // Explicit sub-rectangles of the plane, in the node's local coordinates,
  // pairwise disjoint.
  RectSet rects{};

  [[nodiscard]] bool is_declared() const noexcept {
    return !declared_area.is_zero() || !rects.empty();
  }
  [[nodiscard]] friend bool operator==(const PlanarEnvelope& a,
                                       const PlanarEnvelope& b) noexcept {
    return a.declared_area == b.declared_area && a.rects == b.rects;
  }
  [[nodiscard]] friend bool operator!=(const PlanarEnvelope& a,
                                       const PlanarEnvelope& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const PlanarEnvelope& a,
                                      const PlanarEnvelope& b) noexcept {
    if (a.declared_area != b.declared_area) return a.declared_area < b.declared_area;
    return a.rects < b.rects;
  }
};

// The vertical envelope a rack declares, in whole rack units numbered from 1.
// The envelope is the half-open interval [1, height + 1).
struct SC_API RackEnvelope final {
  RackUnits height{};

  [[nodiscard]] bool is_declared() const noexcept { return !height.is_zero(); }
  [[nodiscard]] RackUnitInterval full_span() const noexcept {
    return RackUnitInterval::of_count(1, height.value());
  }
  [[nodiscard]] friend bool operator==(const RackEnvelope& a, const RackEnvelope& b) noexcept {
    return a.height == b.height;
  }
  [[nodiscard]] friend bool operator!=(const RackEnvelope& a, const RackEnvelope& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const RackEnvelope& a, const RackEnvelope& b) noexcept {
    return a.height < b.height;
  }
};

// Where a node sits inside its parent.
struct SC_API NodePlacement final {
  // Consumed from the parent chain's plane as floor area.
  PlanarRect base_rect{};
  bool has_base_rect = false;

  // Consumed from the parent rack's vertical envelope.
  RackUnitInterval u_span{};
  bool has_u_span = false;

  [[nodiscard]] bool empty() const noexcept { return !has_base_rect && !has_u_span; }

  [[nodiscard]] friend bool operator==(const NodePlacement& a,
                                       const NodePlacement& b) noexcept {
    return a.base_rect == b.base_rect && a.has_base_rect == b.has_base_rect &&
           a.u_span == b.u_span && a.has_u_span == b.has_u_span;
  }
  [[nodiscard]] friend bool operator!=(const NodePlacement& a,
                                       const NodePlacement& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const NodePlacement& a,
                                      const NodePlacement& b) noexcept {
    if (a.has_base_rect != b.has_base_rect) return a.has_base_rect < b.has_base_rect;
    if (a.base_rect != b.base_rect) return a.base_rect < b.base_rect;
    if (a.has_u_span != b.has_u_span) return a.has_u_span < b.has_u_span;
    return a.u_span < b.u_span;
  }
};

// ---------------------------------------------------------------------------
// Node record
// ---------------------------------------------------------------------------

struct SC_API SpaceNode final {
  SpaceNodeId id{};
  EntityGeneration generation{};

  SpaceNodeKind kind = SpaceNodeKind::none;
  SpatialClass spatial_class = SpatialClass::unspecified;
  NodeLifecycle lifecycle = NodeLifecycle::planned;

  DisplayLabel label{};
  Note note{};

  // Containment. A site has no parent.
  SpaceNodeId parent{};
  std::uint32_t depth = 0;  // distance from the root; parent depth + 1

  NodePlacement placement{};

  PlanarEnvelope own_planar{};
  RackEnvelope own_rack{};

  // Upstream identity this node stands for. All optional: Space Capacity
  // permits a node that is not yet correlated with a registry row and reports
  // its capacity as known-but-uncorrelated rather than inventing a reference.
  std::optional<LocationRef> location{};
  std::optional<FacilityNodeRef> facility_node{};
  std::optional<RackRef> rack{};
  AssetRefSet assets{};
  PolicyRefSet policies{};
  EvidenceSet evidence{};

  // Lineage. Set when this node was retired and replaced by another node.
  SpaceNodeId replaced_by{};
  // Set when this node replaced an earlier node.
  SpaceNodeId replaces{};

  [[nodiscard]] bool is_root() const noexcept { return parent.empty(); }
  [[nodiscard]] bool has_lineage() const noexcept {
    return !replaced_by.empty() || !replaces.empty();
  }
  // True when `this` contributes usable space now.
  [[nodiscard]] bool usable_now() const noexcept { return lifecycle_is_usable_now(lifecycle); }

  [[nodiscard]] friend bool operator==(const SpaceNode& a, const SpaceNode& b) noexcept;
  [[nodiscard]] friend bool operator!=(const SpaceNode& a, const SpaceNode& b) noexcept {
    return !(a == b);
  }
  // Canonical order is by identity, which is unique in a model.
  [[nodiscard]] friend bool operator<(const SpaceNode& a, const SpaceNode& b) noexcept {
    return a.id < b.id;
  }
};

// Structural rules a node must satisfy on its own, independent of any other
// record: identity present, kind set, class admissible, lifecycle consistent
// with lineage, envelope within bounds, placement well formed.
SC_API Status validate_node_shape(const SpaceNode& node);

// Containment rules that need both records: parent kind, depth, and the
// placement compatibility that a plane subdivision requires.
SC_API Status validate_containment(const SpaceNode& parent, const SpaceNode& child);

}  // namespace dccp::space_capacity
