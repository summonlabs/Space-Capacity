// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - node kinds, containment rules, lifecycles and shape
// validation.

#include "dccp/space_capacity/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include "dccp/space_capacity/text.hpp"
#include "geometry.hpp"

namespace dccp::space_capacity {
namespace {

// Containment table. Rows are the parent kind, columns the child kind.
// Levels may be skipped, so every legal ancestor for a child is marked, not
// merely its immediate canonical parent. This mirrors the DCCP sibling
// registries, both of which allow a facility to contain a rack directly.
constexpr bool kContains[kSpaceNodeKindCount][kSpaceNodeKindCount] = {
    /* none           */ {false, false, false, false, false, false, false},
    /* site           */ {false, false, true, true, true, true, false},
    /* building       */ {false, false, false, true, true, true, false},
    /* hall           */ {false, false, false, false, true, true, false},
    /* row            */ {false, false, false, false, false, true, false},
    /* rack           */ {false, false, false, false, false, false, true},
    /* rack_unit_band */ {false, false, false, false, false, false, false},
};

constexpr std::size_t kind_index(SpaceNodeKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

// Spatial classes admissible for each kind. `unspecified` is admissible for no
// persisted node.
constexpr bool kClassAllowed[kSpaceNodeKindCount][9] = {
    /* none           */ {false, false, false, false, false, false, false, false, false},
    /* site           */ {false, true, false, false, false, true, false, false, false},
    /* building       */ {false, true, false, false, false, false, false, true, false},
    /* hall           */ {false, true, false, true, false, false, false, true, true},
    /* row            */ {false, true, false, true, false, false, false, false, false},
    /* rack           */ {false, false, true, false, false, false, false, false, false},
    /* rack_unit_band */ {false, false, true, false, false, false, false, false, false},
};

constexpr std::size_t class_index(SpatialClass value) noexcept {
  return static_cast<std::size_t>(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

std::uint32_t canonical_depth(SpaceNodeKind kind) noexcept {
  switch (kind) {
    case SpaceNodeKind::site:
      return 0;
    case SpaceNodeKind::building:
      return 1;
    case SpaceNodeKind::hall:
      return 2;
    case SpaceNodeKind::row:
      return 3;
    case SpaceNodeKind::rack:
      return 4;
    case SpaceNodeKind::rack_unit_band:
      return 5;
    case SpaceNodeKind::none:
      break;
  }
  return 0;
}

std::string_view space_node_kind_name(SpaceNodeKind kind) noexcept {
  switch (kind) {
    case SpaceNodeKind::site:
      return "site";
    case SpaceNodeKind::building:
      return "building";
    case SpaceNodeKind::hall:
      return "hall";
    case SpaceNodeKind::row:
      return "row";
    case SpaceNodeKind::rack:
      return "rack";
    case SpaceNodeKind::rack_unit_band:
      return "rack-unit-band";
    case SpaceNodeKind::none:
      break;
  }
  return "none";
}

bool parse_space_node_kind(std::string_view text, SpaceNodeKind& out) noexcept {
  if (text == "site") {
    out = SpaceNodeKind::site;
    return true;
  }
  if (text == "building") {
    out = SpaceNodeKind::building;
    return true;
  }
  if (text == "hall") {
    out = SpaceNodeKind::hall;
    return true;
  }
  if (text == "row") {
    out = SpaceNodeKind::row;
    return true;
  }
  if (text == "rack") {
    out = SpaceNodeKind::rack;
    return true;
  }
  if (text == "rack-unit-band") {
    out = SpaceNodeKind::rack_unit_band;
    return true;
  }
  if (text == "none") {
    out = SpaceNodeKind::none;
    return true;
  }
  return false;
}

SpaceNodeKind space_node_kind_from_value(std::uint8_t value) noexcept {
  const auto kind = static_cast<SpaceNodeKind>(value);
  if (value >= kSpaceNodeKindCount) return SpaceNodeKind::none;
  return kind;
}

bool kind_may_contain(SpaceNodeKind parent, SpaceNodeKind child) noexcept {
  if (parent == SpaceNodeKind::none || child == SpaceNodeKind::none) return false;
  return kContains[kind_index(parent)][kind_index(child)];
}

bool kind_may_be_root(SpaceNodeKind kind) noexcept { return kind == SpaceNodeKind::site; }

bool kind_may_declare_plane(SpaceNodeKind kind) noexcept {
  switch (kind) {
    case SpaceNodeKind::site:
    case SpaceNodeKind::building:
    case SpaceNodeKind::hall:
    case SpaceNodeKind::row:
      return true;
    default:
      break;
  }
  return false;
}

bool kind_may_declare_rack_envelope(SpaceNodeKind kind) noexcept {
  return kind == SpaceNodeKind::rack;
}

bool kind_may_have_base_rect(SpaceNodeKind kind) noexcept {
  switch (kind) {
    case SpaceNodeKind::building:
    case SpaceNodeKind::hall:
    case SpaceNodeKind::row:
    case SpaceNodeKind::rack:
      return true;
    default:
      break;
  }
  return false;
}

bool kind_may_have_u_span(SpaceNodeKind kind) noexcept {
  return kind == SpaceNodeKind::rack_unit_band;
}

// ---------------------------------------------------------------------------
// Spatial class
// ---------------------------------------------------------------------------

std::string_view spatial_class_name(SpatialClass value) noexcept {
  switch (value) {
    case SpatialClass::unspecified:
      return "unspecified";
    case SpatialClass::floor:
      return "floor";
    case SpatialClass::rack:
      return "rack";
    case SpatialClass::aisle:
      return "aisle";
    case SpatialClass::overhead:
      return "overhead";
    case SpatialClass::outdoor:
      return "outdoor";
    case SpatialClass::service:
      return "service";
    case SpatialClass::enclosed:
      return "enclosed";
    case SpatialClass::cage:
      return "cage";
  }
  return "unknown";
}

bool parse_spatial_class(std::string_view text, SpatialClass& out) noexcept {
  if (text == "unspecified") {
    out = SpatialClass::unspecified;
    return true;
  }
  if (text == "floor") {
    out = SpatialClass::floor;
    return true;
  }
  if (text == "rack") {
    out = SpatialClass::rack;
    return true;
  }
  if (text == "aisle") {
    out = SpatialClass::aisle;
    return true;
  }
  if (text == "overhead") {
    out = SpatialClass::overhead;
    return true;
  }
  if (text == "outdoor") {
    out = SpatialClass::outdoor;
    return true;
  }
  if (text == "service") {
    out = SpatialClass::service;
    return true;
  }
  if (text == "enclosed") {
    out = SpatialClass::enclosed;
    return true;
  }
  if (text == "cage") {
    out = SpatialClass::cage;
    return true;
  }
  return false;
}

bool spatial_class_allowed(SpaceNodeKind kind, SpatialClass value) noexcept {
  if (kind == SpaceNodeKind::none) return false;
  if (value == SpatialClass::unspecified) return false;
  if (class_index(value) >= 9) return false;
  return kClassAllowed[kind_index(kind)][class_index(value)];
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string_view node_lifecycle_name(NodeLifecycle value) noexcept {
  switch (value) {
    case NodeLifecycle::planned:
      return "planned";
    case NodeLifecycle::provisioning:
      return "provisioning";
    case NodeLifecycle::available:
      return "available";
    case NodeLifecycle::restricted:
      return "restricted";
    case NodeLifecycle::decommissioning:
      return "decommissioning";
    case NodeLifecycle::retired:
      return "retired";
    case NodeLifecycle::replaced:
      return "replaced";
  }
  return "unknown";
}

bool parse_node_lifecycle(std::string_view text, NodeLifecycle& out) noexcept {
  if (text == "planned") {
    out = NodeLifecycle::planned;
    return true;
  }
  if (text == "provisioning") {
    out = NodeLifecycle::provisioning;
    return true;
  }
  if (text == "available") {
    out = NodeLifecycle::available;
    return true;
  }
  if (text == "restricted") {
    out = NodeLifecycle::restricted;
    return true;
  }
  if (text == "decommissioning") {
    out = NodeLifecycle::decommissioning;
    return true;
  }
  if (text == "retired") {
    out = NodeLifecycle::retired;
    return true;
  }
  if (text == "replaced") {
    out = NodeLifecycle::replaced;
    return true;
  }
  return false;
}

NodeLifecycle node_lifecycle_from_value(std::uint8_t value) noexcept {
  // The last enumerator is kNodeLifecycleCount - 1, so the bound excludes the
  // count itself: a value one past the last state is not a state.
  if (value < 1 || value >= kNodeLifecycleCount) return NodeLifecycle::planned;
  return static_cast<NodeLifecycle>(value);
}

bool lifecycle_is_usable_now(NodeLifecycle value) noexcept {
  return value == NodeLifecycle::available || value == NodeLifecycle::restricted;
}

bool lifecycle_is_planned(NodeLifecycle value) noexcept {
  return value == NodeLifecycle::planned || value == NodeLifecycle::provisioning;
}

bool lifecycle_is_terminal(NodeLifecycle value) noexcept {
  return value == NodeLifecycle::retired || value == NodeLifecycle::replaced;
}

bool lifecycle_transition_allowed(NodeLifecycle from, NodeLifecycle to) noexcept {
  if (from == to) return false;
  switch (from) {
    case NodeLifecycle::planned:
      return to == NodeLifecycle::provisioning || to == NodeLifecycle::available ||
             to == NodeLifecycle::retired;
    case NodeLifecycle::provisioning:
      return to == NodeLifecycle::available || to == NodeLifecycle::planned ||
             to == NodeLifecycle::retired;
    case NodeLifecycle::available:
      return to == NodeLifecycle::restricted || to == NodeLifecycle::decommissioning ||
             to == NodeLifecycle::retired || to == NodeLifecycle::replaced;
    case NodeLifecycle::restricted:
      return to == NodeLifecycle::available || to == NodeLifecycle::decommissioning ||
             to == NodeLifecycle::retired || to == NodeLifecycle::replaced;
    case NodeLifecycle::decommissioning:
      return to == NodeLifecycle::available || to == NodeLifecycle::retired ||
             to == NodeLifecycle::replaced;
    case NodeLifecycle::retired:
      // A retired region may be returned to service only by an explicit
      // provisioning step, so that "retired" is never silently undone.
      return to == NodeLifecycle::provisioning;
    case NodeLifecycle::replaced:
      return false;
  }
  return false;
}

std::string_view lifecycle_transition_refusal(NodeLifecycle from, NodeLifecycle to) noexcept {
  if (from == to) return "the node is already in that lifecycle state";
  if (from == NodeLifecycle::replaced) return "a replaced node is terminal";
  return "the lifecycle transition is not permitted";
}

// ---------------------------------------------------------------------------
// Record equality
// ---------------------------------------------------------------------------

// Defined out of line so that a whole-record comparison is one symbol that a
// diff, a test and a downstream consumer all share. The identity is included:
// two nodes are equal only when they are the same node and every field agrees.
bool operator==(const SpaceNode& a, const SpaceNode& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.kind == b.kind &&
         a.spatial_class == b.spatial_class && a.lifecycle == b.lifecycle && a.label == b.label &&
         a.note == b.note && a.parent == b.parent && a.depth == b.depth &&
         a.placement == b.placement && a.own_planar == b.own_planar &&
         a.own_rack == b.own_rack && a.location == b.location &&
         a.facility_node == b.facility_node && a.rack == b.rack && a.assets == b.assets &&
         a.policies == b.policies && a.evidence == b.evidence &&
         a.replaced_by == b.replaced_by && a.replaces == b.replaces;
}
// ---------------------------------------------------------------------------
// Shape validation
// ---------------------------------------------------------------------------

Status validate_node_shape(const SpaceNode& node) {
  if (node.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "a node must carry an identity");
  }
  if (node.kind == SpaceNodeKind::none) {
    return Status::failure(ErrorCode::invalid_kind_for_parent, "a node must declare a kind");
  }
  if (!spatial_class_allowed(node.kind, node.spatial_class)) {
    return Status::failure(
        ErrorCode::invalid_spatial_class,
        "spatial class " + std::string(spatial_class_name(node.spatial_class)) +
            " is not admissible for a " + std::string(space_node_kind_name(node.kind)));
  }
  if (node.depth > Limits::kMaxContainmentDepth) {
    return Status::failure(ErrorCode::depth_exceeded,
                           "containment depth " + to_text(static_cast<std::uint64_t>(node.depth)) +
                               " exceeds the bound " +
                               to_text(static_cast<std::uint64_t>(Limits::kMaxContainmentDepth)));
  }
  if (node.is_root()) {
    if (!kind_may_be_root(node.kind)) {
      return Status::failure(ErrorCode::kind_must_have_parent,
                             "a " + std::string(space_node_kind_name(node.kind)) +
                                 " must be contained; only a site may be a root");
    }
    if (node.depth != 0) {
      return Status::failure(ErrorCode::invalid_placement,
                             "a root node must have depth zero");
    }
  } else if (node.depth == 0) {
    return Status::failure(ErrorCode::invalid_placement,
                           "a contained node must have a depth of at least one");
  }

  if (node.own_planar.is_declared() && !kind_may_declare_plane(node.kind)) {
    return Status::failure(ErrorCode::plane_not_declared,
                           "a " + std::string(space_node_kind_name(node.kind)) +
                               " may not declare a planar envelope");
  }
  if (node.own_rack.is_declared() && !kind_may_declare_rack_envelope(node.kind)) {
    return Status::failure(ErrorCode::envelope_not_declared,
                           "a " + std::string(space_node_kind_name(node.kind)) +
                               " may not declare a rack envelope");
  }

  if (node.own_planar.declared_area.is_negative()) {
    return Status::failure(ErrorCode::invalid_extent, "a declared area must not be negative");
  }
  if (node.own_planar.declared_area.value() > Limits::kMaxSquareMillimeters) {
    return Status::failure(ErrorCode::extent_out_of_envelope,
                           "declared area exceeds the bound " +
                               to_text(static_cast<std::uint64_t>(Limits::kMaxSquareMillimeters)) +
                               " square millimetres");
  }
  if (!node.own_planar.rects.empty()) {
    const Checked<SquareMillimeters> rect_total = node.own_planar.rects.total_area();
    if (!rect_total) {
      return Status::failure(ErrorCode::arithmetic_overflow,
                             "the declared rectangles overflow the area total");
    }
    if (rect_total.value > node.own_planar.declared_area) {
      return Status::failure(
          ErrorCode::extent_out_of_envelope,
          "the declared rectangles cover " + to_text(rect_total.value) +
              " which exceeds the declared area " + to_text(node.own_planar.declared_area));
    }
  }

  if (node.own_rack.is_declared()) {
    if (node.own_rack.height.value() < Limits::kMinRackUnits ||
        node.own_rack.height.value() > Limits::kMaxRackUnits) {
      return Status::failure(ErrorCode::unit_envelope_exceeded,
                             "a rack envelope height of " +
                                 std::string(to_text(node.own_rack.height)) +
                                 " units is outside [1," +
                                 to_text(static_cast<std::uint64_t>(Limits::kMaxRackUnits)) + "]");
    }
  }

  if (node.placement.has_base_rect) {
    if (!kind_may_have_base_rect(node.kind)) {
      return Status::failure(ErrorCode::invalid_placement,
                             "a " + std::string(space_node_kind_name(node.kind)) +
                                 " may not declare a planar placement");
    }
    if (!node.placement.base_rect.is_valid()) {
      return Status::failure(ErrorCode::invalid_extent,
                             "placement rectangle " + to_text(node.placement.base_rect) +
                                 " is degenerate or outside the millimetre bounds");
    }
  }
  if (node.placement.has_u_span) {
    if (!kind_may_have_u_span(node.kind)) {
      return Status::failure(ErrorCode::invalid_placement,
                             "a " + std::string(space_node_kind_name(node.kind)) +
                                 " may not declare a rack unit span");
    }
    if (!node.placement.u_span.is_valid()) {
      return Status::failure(ErrorCode::invalid_extent,
                             "placement span " + to_text(node.placement.u_span) +
                                 " is not a valid rack unit interval");
    }
  }
  if (node.is_root() && !node.placement.empty()) {
    return Status::failure(ErrorCode::invalid_placement,
                           "a root node has no parent envelope to be placed in");
  }

  if (node.lifecycle == NodeLifecycle::replaced && node.replaced_by.empty()) {
    return Status::failure(ErrorCode::lineage_broken,
                           "a node in the replaced state must name its successor");
  }
  if (node.lifecycle != NodeLifecycle::replaced && !node.replaced_by.empty()) {
    return Status::failure(ErrorCode::lineage_broken,
                           "only a node in the replaced state may name a successor");
  }
  if (!node.replaced_by.empty() && node.replaced_by == node.id) {
    return Status::failure(ErrorCode::self_reference, "a node may not replace itself");
  }
  if (!node.replaces.empty() && node.replaces == node.id) {
    return Status::failure(ErrorCode::self_reference, "a node may not replace itself");
  }
  if (!node.replaced_by.empty() && !node.replaces.empty() && node.replaced_by == node.replaces) {
    return Status::failure(ErrorCode::replacement_cycle,
                           "a node may not both replace and be replaced by the same node");
  }
  if (!node.location.has_value() || node.location->empty()) {
    if (node.location.has_value()) {
      return Status::failure(ErrorCode::malformed_reference,
                             "a location reference must not be empty");
    }
  }
  if (node.facility_node.has_value() && node.facility_node->empty()) {
    return Status::failure(ErrorCode::malformed_reference,
                           "a facility node reference must not be empty");
  }
  if (node.rack.has_value() && node.rack->empty()) {
    return Status::failure(ErrorCode::malformed_reference, "a rack reference must not be empty");
  }
  if (node.rack.has_value() && node.kind != SpaceNodeKind::rack) {
    return Status::failure(ErrorCode::incompatible_kind,
                           "only a rack node may carry a rack registry reference");
  }
  return Status::success();
}

Status validate_containment(const SpaceNode& parent, const SpaceNode& child) {
  if (parent.id.empty() || child.id.empty()) {
    return Status::failure(ErrorCode::empty_value, "both nodes must carry an identity");
  }
  if (parent.id == child.id) {
    return Status::failure(ErrorCode::self_reference, "a node may not contain itself");
  }
  if (!kind_may_contain(parent.kind, child.kind)) {
    return Status::failure(
        ErrorCode::invalid_kind_for_parent,
        "a " + std::string(space_node_kind_name(parent.kind)) + " may not contain a " +
            std::string(space_node_kind_name(child.kind)));
  }
  if (parent.depth + 1 != child.depth) {
    return Status::failure(ErrorCode::invalid_placement,
                           "a child of a depth-" + to_text(parent.depth) +
                               " node must have depth " + to_text(parent.depth + 1ull));
  }
  if (child.depth > Limits::kMaxContainmentDepth) {
    return Status::failure(ErrorCode::depth_exceeded,
                           "containment depth " + to_text(child.depth) + " exceeds the bound " +
                               to_text(static_cast<std::uint64_t>(Limits::kMaxContainmentDepth)));
  }
  if (!lifecycle_is_terminal(parent.lifecycle)) {
    // A parent that can still receive occupancy is fine; nothing to check.
  }
  return Status::success();
}

}  // namespace dccp::space_capacity
