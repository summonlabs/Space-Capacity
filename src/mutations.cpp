// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - mutation application and validation precedence.
//
// Every mutation runs the same stages in the same order, and the first failing
// stage decides the reported code:
//
//   1 argument_shape         the request's own fields, independent of state
//   2 precondition_authority the revision and generation fences
//   3 referential            the subjects the request names must exist
//   4 compatibility          kind, hierarchy, lifecycle and conflicts
//   5 capacity               envelope fit and checked arithmetic
//   6 persistence            the durable step, performed by the store
//
// The order is part of the contract and is exercised by a dedicated test: a
// request that is wrong in two ways must report the earlier stage. Nothing is
// written to the working copy until every stage that can fail has passed.

#include "mutations.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/text.hpp"
#include "geometry.hpp"

namespace dccp::space_capacity::internal {
namespace {

EntityGeneration next_generation(EntityGeneration current) {
  const Checked<std::uint64_t> value = checked_increment(current.value());
  return EntityGeneration{value ? value.value : Limits::kMaxCounter};
}

std::string subject_of(const SpaceNodeId& id) { return id.str(); }

bool scope_has_rects(const FootprintScope& scope) {
  return scope.kind == FootprintScopeKind::planar && !scope.rects.empty();
}

bool scope_has_units(const FootprintScope& scope) {
  return scope.kind == FootprintScopeKind::rack_units && !scope.units.empty();
}

// One existing record that consumes space, kept with a label so a conflict can
// name exactly what it hit.
struct ConsumingRect final {
  PlanarRect rect{};
  std::string owner;
};

struct ConsumingUnits final {
  RackUnitInterval interval{};
  std::string owner;
};

void collect_consuming(const WorkingModel& model, const SpaceNodeId& node, bool want_plane,
                       bool want_rack, const OccupancyClaimId& ignore_claim,
                       const SpaceNodeId& ignore_node, std::vector<ConsumingRect>& rects,
                       std::vector<ConsumingUnits>& units, bool& whole_node_taken,
                       std::string& whole_node_owner) {
  const SpaceNodeId plane = want_plane ? model.plane_owner_of(node) : SpaceNodeId{};
  const SpaceNodeId rack = want_rack ? model.rack_owner_of(node) : SpaceNodeId{};

  for (const SpaceNode& member : model.nodes) {
    if (member.id == ignore_node) continue;
    if (!member.placement.has_base_rect || plane.empty()) continue;
    if (model.plane_owner_of(member.id) != plane || member.id == plane) continue;
    rects.push_back(ConsumingRect{member.placement.base_rect, "node " + member.id.str()});
  }
  for (const OccupancyClaim& claim : model.claims) {
    if (claim.id == ignore_claim) continue;
    if (!claim_state_consumes(claim.state)) continue;
    if (!plane.empty() && model.plane_owner_of(claim.node) == plane) {
      if (claim.scope.is_whole_node()) {
        whole_node_taken = true;
        whole_node_owner = "claim " + claim.id.str();
      } else if (scope_has_rects(claim.scope)) {
        for (const PlanarRect& rect : claim.scope.rects.rects()) {
          rects.push_back(ConsumingRect{rect, "claim " + claim.id.str()});
        }
      }
    }
    if (!rack.empty() && model.rack_owner_of(claim.node) == rack) {
      if (claim.scope.is_whole_node()) {
        whole_node_taken = true;
        whole_node_owner = "claim " + claim.id.str();
      } else if (scope_has_units(claim.scope)) {
        for (const RackUnitInterval& interval : claim.scope.units.intervals()) {
          units.push_back(ConsumingUnits{interval, "claim " + claim.id.str()});
        }
      }
    }
  }
  for (const FootprintReservation& reservation : model.reservations) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (!plane.empty() && model.plane_owner_of(reservation.node) == plane) {
      if (reservation.scope.is_whole_node()) {
        whole_node_taken = true;
        whole_node_owner = "reservation " + reservation.id.str();
      } else if (scope_has_rects(reservation.scope)) {
        for (const PlanarRect& rect : reservation.scope.rects.rects()) {
          rects.push_back(ConsumingRect{rect, "reservation " + reservation.id.str()});
        }
      }
    }
    if (!rack.empty() && model.rack_owner_of(reservation.node) == rack) {
      if (reservation.scope.is_whole_node()) {
        whole_node_taken = true;
        whole_node_owner = "reservation " + reservation.id.str();
      } else if (scope_has_units(reservation.scope)) {
        for (const RackUnitInterval& interval : reservation.scope.units.intervals()) {
          units.push_back(ConsumingUnits{interval, "reservation " + reservation.id.str()});
        }
      }
    }
  }
  for (const ExpansionZone& zone : model.zones) {
    if (!zone.earmarks()) continue;
    if (!plane.empty() && model.plane_owner_of(zone.node) == plane && scope_has_rects(zone.scope)) {
      for (const PlanarRect& rect : zone.scope.rects.rects()) {
        rects.push_back(ConsumingRect{rect, "expansion zone " + zone.id.str()});
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Working model
// ---------------------------------------------------------------------------

WorkingModel WorkingModel::from(const Snapshot& snapshot) {
  WorkingModel model;
  model.store = snapshot.store();
  model.incarnation = snapshot.incarnation();
  model.base_revision = snapshot.revision();
  model.attempt = snapshot.attempt();
  model.created_at = snapshot.created_at();
  model.nodes = snapshot.nodes();
  model.claims = snapshot.claims();
  model.reservations = snapshot.reservations();
  model.exclusions = snapshot.exclusions();
  model.clearances = snapshot.clearances();
  model.zones = snapshot.expansion_zones();
  return model;
}

SpaceNode* WorkingModel::node(const SpaceNodeId& id) {
  const auto it = std::lower_bound(nodes.begin(), nodes.end(), id,
                                   [](const SpaceNode& record, const SpaceNodeId& key) {
                                     return record.id < key;
                                   });
  if (it == nodes.end() || it->id != id) return nullptr;
  return &*it;
}

const SpaceNode* WorkingModel::node(const SpaceNodeId& id) const {
  return const_cast<WorkingModel*>(this)->node(id);
}

OccupancyClaim* WorkingModel::claim(const OccupancyClaimId& id) {
  const auto it = std::lower_bound(claims.begin(), claims.end(), id,
                                   [](const OccupancyClaim& record, const OccupancyClaimId& key) {
                                     return record.id < key;
                                   });
  if (it == claims.end() || it->id != id) return nullptr;
  return &*it;
}

const OccupancyClaim* WorkingModel::claim(const OccupancyClaimId& id) const {
  return const_cast<WorkingModel*>(this)->claim(id);
}

FootprintReservation* WorkingModel::reservation(const OccupancyClaimId& id) {
  const auto it =
      std::lower_bound(reservations.begin(), reservations.end(), id,
                       [](const FootprintReservation& record, const OccupancyClaimId& key) {
                         return record.id < key;
                       });
  if (it == reservations.end() || it->id != id) return nullptr;
  return &*it;
}

const FootprintReservation* WorkingModel::reservation(const OccupancyClaimId& id) const {
  return const_cast<WorkingModel*>(this)->reservation(id);
}

ExclusionRegion* WorkingModel::exclusion(const ExclusionRegionId& id) {
  const auto it = std::lower_bound(exclusions.begin(), exclusions.end(), id,
                                   [](const ExclusionRegion& record, const ExclusionRegionId& key) {
                                     return record.id < key;
                                   });
  if (it == exclusions.end() || it->id != id) return nullptr;
  return &*it;
}

const ExclusionRegion* WorkingModel::exclusion(const ExclusionRegionId& id) const {
  return const_cast<WorkingModel*>(this)->exclusion(id);
}

ClearanceConstraint* WorkingModel::clearance(const ClearanceConstraintId& id) {
  const auto it = std::lower_bound(
      clearances.begin(), clearances.end(), id,
      [](const ClearanceConstraint& record, const ClearanceConstraintId& key) {
        return record.id < key;
      });
  if (it == clearances.end() || it->id != id) return nullptr;
  return &*it;
}

const ClearanceConstraint* WorkingModel::clearance(const ClearanceConstraintId& id) const {
  return const_cast<WorkingModel*>(this)->clearance(id);
}

ExpansionZone* WorkingModel::zone(const ExpansionZoneId& id) {
  const auto it = std::lower_bound(zones.begin(), zones.end(), id,
                                   [](const ExpansionZone& record, const ExpansionZoneId& key) {
                                     return record.id < key;
                                   });
  if (it == zones.end() || it->id != id) return nullptr;
  return &*it;
}

const ExpansionZone* WorkingModel::zone(const ExpansionZoneId& id) const {
  return const_cast<WorkingModel*>(this)->zone(id);
}

SpaceNodeId WorkingModel::plane_owner_of(const SpaceNodeId& id) const {
  const SpaceNode* current = node(id);
  std::uint32_t guard = 0;
  while (current != nullptr) {
    if (current->own_planar.is_declared()) return current->id;
    if (++guard > Limits::kMaxContainmentDepth + 1) return SpaceNodeId{};
    current = current->parent.empty() ? nullptr : node(current->parent);
  }
  return SpaceNodeId{};
}

SpaceNodeId WorkingModel::rack_owner_of(const SpaceNodeId& id) const {
  const SpaceNode* current = node(id);
  std::uint32_t guard = 0;
  while (current != nullptr) {
    if (current->own_rack.is_declared() && kind_may_declare_rack_envelope(current->kind)) {
      return current->id;
    }
    if (++guard > Limits::kMaxContainmentDepth + 1) return SpaceNodeId{};
    current = current->parent.empty() ? nullptr : node(current->parent);
  }
  return SpaceNodeId{};
}

bool WorkingModel::is_ancestor(const SpaceNodeId& ancestor, const SpaceNodeId& descendant) const {
  if (ancestor.empty() || ancestor == descendant) return false;
  const SpaceNode* current = node(descendant);
  std::uint32_t guard = 0;
  while (current != nullptr && !current->parent.empty()) {
    if (current->parent == ancestor) return true;
    if (++guard > Limits::kMaxContainmentDepth + 1) return false;
    current = node(current->parent);
  }
  return false;
}

std::uint32_t WorkingModel::subtree_height(const SpaceNodeId& id) const {
  std::uint32_t height = 0;
  for (const SpaceNode& member : nodes) {
    if (member.id != id && is_ancestor(id, member.id)) {
      const std::uint32_t relative = member.depth > node(id)->depth
                                         ? member.depth - node(id)->depth
                                         : 0;
      if (relative > height) height = relative;
    }
  }
  return height;
}

// ---------------------------------------------------------------------------
// Stage 1: argument shape
// ---------------------------------------------------------------------------

Status check_argument_shape(const SpaceNode& node) { return validate_node_shape(node); }

Status check_argument_shape(const OccupancyClaim& claim) { return validate_claim_shape(claim); }

Status check_argument_shape(const FootprintReservation& reservation) {
  return validate_reservation_shape(reservation);
}

Status check_argument_shape(const ExclusionRegion& region) {
  return validate_exclusion_shape(region);
}

Status check_argument_shape(const ClearanceConstraint& constraint) {
  return validate_clearance_shape(constraint);
}

Status check_argument_shape(const ExpansionZone& zone) {
  return validate_expansion_zone_shape(zone);
}

// ---------------------------------------------------------------------------
// Stage 2: authority fences
// ---------------------------------------------------------------------------

Status check_preconditions(const Precondition& precondition, RegistryRevision current_revision,
                           std::optional<EntityGeneration> current_generation,
                           const std::string& subject) {
  if (precondition.revision.has_value() &&
      precondition.revision->value() != current_revision.value()) {
    return Status(Error::stale(ErrorCode::stale_revision,
                               "the request was prepared against a different revision", subject,
                               precondition.revision->value(), current_revision.value()));
  }
  if (precondition.generation.has_value()) {
    if (!current_generation.has_value()) {
      return Status(Error::with_subject(
          ErrorCode::stale_generation,
          "the request names a generation for a record that does not exist", subject));
    }
    if (precondition.generation->value() != current_generation->value()) {
      return Status(Error::stale(ErrorCode::stale_generation,
                                 "the record moved on since the request was prepared", subject,
                                 precondition.generation->value(), current_generation->value()));
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Stage 5: conflicts
// ---------------------------------------------------------------------------

Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node, const OccupancyClaimId& ignore_claim,
                               const SpaceNodeId& ignore_node) {
  if (scope.is_whole_node()) {
    // A whole-node record covers everything, so it conflicts with any other
    // consuming record on the same envelope.
  } else if (!scope_has_rects(scope) && !scope_has_units(scope)) {
    return Status::success();
  }

  std::vector<ConsumingRect> rects;
  std::vector<ConsumingUnits> units;
  bool whole_node_taken = false;
  std::string whole_node_owner;
  collect_consuming(model, node, true, true, ignore_claim, ignore_node, rects, units,
                    whole_node_taken, whole_node_owner);

  if (scope.is_whole_node()) {
    if (whole_node_taken) {
      return Status::failure(ErrorCode::overlap,
                             "the envelope is already covered by " + whole_node_owner);
    }
    if (!rects.empty()) {
      return Status::failure(ErrorCode::overlap,
                             "the envelope is already partly covered by " + rects.front().owner);
    }
    if (!units.empty()) {
      return Status::failure(ErrorCode::overlap,
                             "the rack envelope is already partly covered by " +
                                 units.front().owner);
    }
    return Status::success();
  }

  if (whole_node_taken) {
    return Status::failure(ErrorCode::overlap, "the envelope is already covered by " +
                                                   whole_node_owner);
  }

  if (scope_has_rects(scope)) {
    for (const PlanarRect& candidate : scope.rects.rects()) {
      for (const ConsumingRect& existing : rects) {
        if (rect_intersects(candidate, existing.rect)) {
          return Status::failure(ErrorCode::overlap,
                                 "the requested rectangle " + to_text(candidate) +
                                     " overlaps " + existing.owner + " at " +
                                     to_text(existing.rect));
        }
      }
    }
  }
  if (scope_has_units(scope)) {
    for (const RackUnitInterval& candidate : scope.units.intervals()) {
      for (const ConsumingUnits& existing : units) {
        if (intervals_overlap(candidate, existing.interval)) {
          return Status::failure(ErrorCode::overlap,
                                 "the requested span " + to_text(candidate) + " overlaps " +
                                     existing.owner + " at " + to_text(existing.interval));
        }
      }
    }
  }
  return Status::success();
}

Status check_blocked_by_exclusion(const WorkingModel& model, const FootprintScope& scope,
                                  const SpaceNodeId& node, Timestamp now) {
  if (scope.is_whole_node()) return Status::success();
  const SpaceNodeId plane = model.plane_owner_of(node);
  const SpaceNodeId rack = model.rack_owner_of(node);
  (void)now;

  for (const ExclusionRegion& region : model.exclusions) {
    if (!region.blocks_now()) continue;
    if (region.blocks.empty()) continue;
    const bool same_plane = !plane.empty() && model.plane_owner_of(region.node) == plane;
    const bool same_rack = !rack.empty() && model.rack_owner_of(region.node) == rack;
    if (!same_plane && !same_rack) continue;
    if (region.scope.is_whole_node()) {
      return Status::failure(ErrorCode::exclusion_violation,
                             "the envelope is excluded by region " + region.id.str() +
                                 " for reason " +
                                 std::string(exclusion_reason_name(region.reason)));
    }
    if (same_plane && scope_has_rects(scope) && scope_has_rects(region.scope)) {
      for (const PlanarRect& candidate : scope.rects.rects()) {
        for (const PlanarRect& excluded : region.scope.rects.rects()) {
          if (rect_intersects(candidate, excluded)) {
            return Status::failure(ErrorCode::exclusion_violation,
                                   "the requested rectangle " + to_text(candidate) +
                                       " lies inside exclusion region " + region.id.str());
          }
        }
      }
    }
    if (same_rack && scope_has_units(scope) && scope_has_units(region.scope)) {
      for (const RackUnitInterval& candidate : scope.units.intervals()) {
        for (const RackUnitInterval& excluded : region.scope.units.intervals()) {
          if (intervals_overlap(candidate, excluded)) {
            return Status::failure(ErrorCode::exclusion_violation,
                                   "the requested span " + to_text(candidate) +
                                       " lies inside exclusion region " + region.id.str());
          }
        }
      }
    }
  }

  for (const ClearanceConstraint& clearance : model.clearances) {
    if (!clearance.enforceable) continue;
    const bool same_plane = clearance.has_band && !plane.empty() &&
                            model.plane_owner_of(clearance.node) == plane;
    const bool same_rack = clearance.has_units && !rack.empty() &&
                           model.rack_owner_of(clearance.node) == rack;
    if (same_plane && scope_has_rects(scope)) {
      for (const PlanarRect& candidate : scope.rects.rects()) {
        if (rect_intersects(candidate, clearance.band)) {
          return Status::failure(ErrorCode::clearance_violation,
                                 "the requested rectangle " + to_text(candidate) +
                                     " lies inside the enforceable service clearance " +
                                     clearance.id.str());
        }
      }
    }
    if (same_rack && scope_has_units(scope)) {
      for (const RackUnitInterval& candidate : scope.units.intervals()) {
        for (const RackUnitInterval& reserved : clearance.required_free_units.intervals()) {
          if (intervals_overlap(candidate, reserved)) {
            return Status::failure(ErrorCode::clearance_violation,
                                   "the requested span " + to_text(candidate) +
                                       " lies inside the enforceable service clearance " +
                                       clearance.id.str());
          }
        }
      }
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Finalize
// ---------------------------------------------------------------------------

Result<SnapshotPtr> finalize(WorkingModel& model) {
  const Checked<std::uint64_t> revision = checked_increment(model.base_revision.value());
  if (!revision) {
    return Error::make(ErrorCode::arithmetic_overflow, "the registry revision reached its maximum");
  }
  const Checked<std::uint64_t> attempt = checked_increment(model.attempt.sequence());
  if (!attempt) {
    return Error::make(ErrorCode::arithmetic_overflow, "the attempt sequence reached its maximum");
  }

  Snapshot::BuildInput input;
  input.store = model.store;
  input.incarnation = model.incarnation;
  input.revision = RegistryRevision{revision.value};
  input.attempt = AttemptId{model.incarnation, attempt.value};
  input.created_at = system_utc_now();
  input.nodes = std::move(model.nodes);
  input.claims = std::move(model.claims);
  input.reservations = std::move(model.reservations);
  input.exclusions = std::move(model.exclusions);
  input.clearances = std::move(model.clearances);
  input.expansion_zones = std::move(model.zones);
  return Snapshot::build(std::move(input));
}

Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node, const OccupancyClaimId& ignore_claim) {
  return check_consuming_overlap(model, scope, node, ignore_claim, SpaceNodeId{});
}

Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node) {
  return check_consuming_overlap(model, scope, node, OccupancyClaimId{}, SpaceNodeId{});
}

// ---------------------------------------------------------------------------
// Stage 5: helper checks shared by the node mutations
// ---------------------------------------------------------------------------

namespace {

template <typename Record, typename Id>
void upsert(std::vector<Record>& records, Record value) {
  const auto it = std::lower_bound(records.begin(), records.end(), value.id,
                                   [](const Record& record, const Id& key) {
                                     return record.id < key;
                                   });
  if (it != records.end() && it->id == value.id) {
    *it = std::move(value);
    return;
  }
  records.insert(it, std::move(value));
}

// Checks that a node's placement is inside the plane and the rack envelope it
// consumes, that a subdivided plane matches its placement exactly, and that the
// placement does not overlap another consuming record.
Status check_placement(const WorkingModel& model, const SpaceNode& node,
                       const SpaceNodeId& ignore_node) {
  if (node.placement.has_base_rect) {
    const SpaceNodeId plane = model.plane_owner_of(node.parent);
    if (!plane.empty()) {
      const SpaceNode* owner = model.node(plane);
      if (owner != nullptr && !owner->own_planar.rects.empty()) {
        std::int64_t covered = 0;
        for (const PlanarRect& plane_rect : owner->own_planar.rects.rects()) {
          const Checked<SquareMillimeters> piece =
              rect_intersection_area(plane_rect, node.placement.base_rect);
          if (!piece) {
            return Status::failure(ErrorCode::arithmetic_overflow,
                                   "the containment measure overflowed");
          }
          covered += piece.value.value();
        }
        const Checked<SquareMillimeters> area = node.placement.base_rect.area();
        if (!area) {
          return Status::failure(ErrorCode::arithmetic_overflow, "the placement area overflowed");
        }
        if (covered != area.value.value()) {
          return Status::failure(
              ErrorCode::extent_out_of_envelope,
              "the placement " + to_text(node.placement.base_rect) +
                  " is not entirely inside the declared rectangles of the plane of " + plane.str());
        }
      }
    }
    if (node.own_planar.is_declared()) {
      const SpaceNodeId owner = model.plane_owner_of(node.parent);
      if (!owner.empty() && owner != node.id) {
        const Checked<SquareMillimeters> area = node.placement.base_rect.area();
        if (!area) {
          return Status::failure(ErrorCode::arithmetic_overflow, "the placement area overflowed");
        }
        if (area.value != node.own_planar.declared_area) {
          return Status::failure(
              ErrorCode::plane_mismatch,
              "a plane declared inside another plane must match its placement exactly: placement "
              "is " + to_text(area.value) + " and declared is " +
                  to_text(node.own_planar.declared_area));
        }
      }
    }

    FootprintScope scope;
    scope.kind = FootprintScopeKind::planar;
    scope.rects = *RectSet::build({node.placement.base_rect});
    const Status overlap = check_consuming_overlap(model, scope, node.id, OccupancyClaimId{},
                                                   ignore_node);
    if (!overlap) return overlap;
  }

  if (node.placement.has_u_span) {
    const SpaceNodeId rack = model.rack_owner_of(node.parent);
    if (rack.empty()) {
      return Status::failure(
          ErrorCode::unit_envelope_exceeded,
          "a rack unit placement requires a parent chain that declares a rack envelope");
    }
    const SpaceNode* owner = model.node(rack);
    if (owner != nullptr && owner->own_rack.is_declared()) {
      const RackUnitInterval envelope = owner->own_rack.full_span();
      if (!interval_contains(envelope, node.placement.u_span)) {
        return Status::failure(ErrorCode::unit_envelope_exceeded,
                               "the placement span " + to_text(node.placement.u_span) +
                                   " is outside the rack envelope " + to_text(envelope));
      }
    }
    FootprintScope scope;
    scope.kind = FootprintScopeKind::rack_units;
    scope.units = *IntervalSet::build({node.placement.u_span});
    const Status overlap = check_consuming_overlap(model, scope, node.id, OccupancyClaimId{},
                                                   ignore_node);
    if (!overlap) return overlap;
  }
  return Status::success();
}

// Recomputes the depth of every node in the subtree rooted at `root` after a
// reparent. Returns the deepest depth reached.
Status reindex_depths(WorkingModel& model, const SpaceNodeId& root, std::uint32_t depth) {
  SpaceNode* node = model.node(root);
  if (node == nullptr) {
    return Status::failure(ErrorCode::not_found, "the subtree root is not in the model");
  }
  if (depth > Limits::kMaxContainmentDepth) {
    return Status::failure(ErrorCode::depth_exceeded,
                           "the reparent would exceed the containment depth bound of " +
                               to_text(static_cast<std::uint64_t>(Limits::kMaxContainmentDepth)));
  }
  node->depth = depth;
  for (SpaceNode& member : model.nodes) {
    if (member.parent == root && member.id != root) {
      const Status nested = reindex_depths(model, member.id, depth + 1);
      if (!nested) return nested;
    }
  }
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Node mutations
// ---------------------------------------------------------------------------

Result<MutationResult> apply_create_node(WorkingModel& model, const CreateNodeRequest& request) {
  // Stage 1.
  const Status shape = check_argument_shape(request.node);
  if (!shape) return shape.error();
  if (request.node.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created node must carry an identity");
  }
  if (request.node.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range,
                       "a created node must be at generation 1, not " +
                           std::to_string(request.node.generation.value()));
  }
  // Stage 2.
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.node.id));
  if (!fence) return fence.error();
  // Stage 3.
  if (model.node(request.node.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists, "a node with this identity exists",
                               request.node.id.str());
  }
  if (!request.node.parent.empty() && model.node(request.node.parent) == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "the named parent is not in the model",
                               request.node.parent.str());
  }
  // Stage 4.
  if (!request.node.parent.empty()) {
    const SpaceNode* parent = model.node(request.node.parent);
    const Status containment = validate_containment(*parent, request.node);
    if (!containment) return containment.error();
  }
  // Stage 5.
  model.nodes.push_back(request.node);
  std::sort(model.nodes.begin(), model.nodes.end());
  // The node itself is excluded from the conflict search: its own placement
  // is not a conflict with itself.
  const Status placement = check_placement(model, request.node, request.node.id);
  if (!placement) {
    model.nodes.erase(std::remove_if(model.nodes.begin(), model.nodes.end(),
                                     [&](const SpaceNode& member) {
                                       return member.id == request.node.id;
                                     }),
                      model.nodes.end());
    return placement.error();
  }

  MutationResult result;
  result.subject = request.node.id;
  result.explanations.add(ReasonCode::lineage_preserved, request.node.id.str(),
                          "the node was created at generation 1 with its identity intact");
  return result;
}

Result<MutationResult> apply_set_node_metadata(WorkingModel& model,
                                               const SetNodeMetadataRequest& request) {
  // Stage 1.
  if (request.node.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node");
  }
  if (request.clear_label && request.label.has_value()) {
    return Error::make(ErrorCode::invalid_argument, "a field cannot be both set and cleared");
  }
  if (request.clear_note && request.note.has_value()) {
    return Error::make(ErrorCode::invalid_argument, "a field cannot be both set and cleared");
  }
  // Stage 2.
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  // Stage 3.
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  // Stage 4.
  if (request.spatial_class.has_value() &&
      !spatial_class_allowed(node->kind, *request.spatial_class)) {
    return Error::with_subject(
        ErrorCode::invalid_spatial_class,
        "spatial class " + std::string(spatial_class_name(*request.spatial_class)) +
            " is not admissible for a " + std::string(space_node_kind_name(node->kind)),
        request.node.str());
  }
  // Apply.
  SpaceNode updated = *node;
  if (request.clear_label) {
    updated.label = DisplayLabel{};
  } else if (request.label.has_value()) {
    updated.label = *request.label;
  }
  if (request.clear_note) {
    updated.note = Note{};
  } else if (request.note.has_value()) {
    updated.note = *request.note;
  }
  if (request.spatial_class.has_value()) {
    updated.spatial_class = *request.spatial_class;
  }
  updated.generation = next_generation(node->generation);
  *node = std::move(updated);

  MutationResult result;
  result.subject = request.node;
  return result;
}

Result<MutationResult> apply_set_node_envelope(WorkingModel& model,
                                               const SetNodeEnvelopeRequest& request) {
  if (request.node.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node");
  }
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }

  SpaceNode updated = *node;
  if (request.planar.has_value()) updated.own_planar = *request.planar;
  if (request.rack.has_value()) updated.own_rack = *request.rack;
  const Status shape = validate_node_shape(updated);
  if (!shape) return Status(Error::with_subject(shape.code(), shape.error().message,
                                                request.node.str())).error();

  // Shrinking an envelope must not orphan the records that already sit inside
  // it. Every planar record attributed to this plane must still fit.
  if (request.planar.has_value()) {
    const SpaceNodeId plane = node->id;
    for (const OccupancyClaim& claim : model.claims) {
      if (!claim_state_consumes(claim.state)) continue;
      if (model.plane_owner_of(claim.node) != plane) continue;
      const std::optional<SquareMillimeters> area = claim.scope.planar_area();
      if (!area.has_value()) continue;
      if (area->value() > updated.own_planar.declared_area.value()) {
        return Error::with_subject(
            ErrorCode::capacity_exceeded,
            "claim " + claim.id.str() + " covers " + to_text(*area) +
                " and would no longer fit in the declared area " +
                to_text(updated.own_planar.declared_area),
            request.node.str());
      }
    }
  }
  if (request.rack.has_value() && updated.own_rack.is_declared()) {
    const RackUnitInterval envelope = updated.own_rack.full_span();
    for (const OccupancyClaim& claim : model.claims) {
      if (!claim_state_consumes(claim.state)) continue;
      if (model.rack_owner_of(claim.node) != node->id) continue;
      for (const RackUnitInterval& interval : claim.scope.units.intervals()) {
        if (!interval_contains(envelope, interval)) {
          return Error::with_subject(
              ErrorCode::unit_envelope_exceeded,
              "claim " + claim.id.str() + " covers " + to_text(interval) +
                  " which would fall outside the new envelope " + to_text(envelope),
              request.node.str());
        }
      }
    }
  }

  updated.generation = next_generation(node->generation);
  *node = std::move(updated);

  MutationResult result;
  result.subject = request.node;
  if (request.planar.has_value()) {
    result.explanations.add(ReasonCode::plane_subdivision, request.node.str(),
                            "the planar envelope was replaced; nested planes must match their "
                            "placement exactly");
  }
  return result;
}

Result<MutationResult> apply_set_node_placement(WorkingModel& model,
                                                const SetNodePlacementRequest& request) {
  if (request.node.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node");
  }
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  if (node->is_root()) {
    return Error::with_subject(ErrorCode::invalid_placement,
                               "a root node has no parent envelope to be placed in",
                               request.node.str());
  }

  SpaceNode updated = *node;
  updated.placement = request.placement;
  const Status shape = validate_node_shape(updated);
  if (!shape) {
    return Status(Error::with_subject(shape.code(), shape.error().message, request.node.str()))
        .error();
  }
  updated.generation = next_generation(node->generation);
  *node = std::move(updated);

  const Status placement = check_placement(model, *node, request.node);
  if (!placement) {
    // Restore the previous placement so a refused mutation leaves no trace.
    SpaceNode* restore = model.node(request.node);
    restore->placement = node->placement;
    return placement.error();
  }

  MutationResult result;
  result.subject = request.node;
  return result;
}

Result<MutationResult> apply_set_node_lifecycle(WorkingModel& model,
                                                const SetNodeLifecycleRequest& request) {
  if (request.node.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node");
  }
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  if (!lifecycle_transition_allowed(node->lifecycle, request.lifecycle)) {
    return Error::with_subject(
        ErrorCode::lifecycle_transition_illegal,
        std::string(lifecycle_transition_refusal(node->lifecycle, request.lifecycle)) + ": " +
            std::string(node_lifecycle_name(node->lifecycle)) + " to " +
            std::string(node_lifecycle_name(request.lifecycle)),
        request.node.str());
  }
  if (request.lifecycle == NodeLifecycle::replaced) {
    if (request.successor.empty()) {
      return Error::with_subject(ErrorCode::lineage_broken,
                                 "a node moving to replaced must name its successor",
                                 request.node.str());
    }
    if (request.successor == request.node) {
      return Error::with_subject(ErrorCode::self_reference, "a node may not replace itself",
                                 request.node.str());
    }
    if (model.node(request.successor) == nullptr) {
      return Error::with_subject(ErrorCode::not_found, "the successor node is not in the model",
                                 request.successor.str());
    }
    if (model.is_ancestor(request.node, request.successor) ||
        model.is_ancestor(request.successor, request.node)) {
      return Error::with_subject(
          ErrorCode::replacement_cycle,
          "a node may not be replaced by a node it contains or that contains it",
          request.node.str());
    }
  } else if (!request.successor.empty()) {
    return Error::with_subject(
        ErrorCode::invalid_argument,
        "a successor may only be named when the node moves to the replaced state",
        request.node.str());
  }

  node->lifecycle = request.lifecycle;
  node->replaced_by = request.lifecycle == NodeLifecycle::replaced ? request.successor
                                                                  : SpaceNodeId{};
  if (request.lifecycle == NodeLifecycle::replaced) {
    SpaceNode* successor = model.node(request.successor);
    successor->replaces = request.node;
  }
  node->generation = next_generation(node->generation);

  MutationResult result;
  result.subject = request.node;
  if (request.lifecycle == NodeLifecycle::replaced) {
    result.explanations.add(ReasonCode::lineage_preserved, request.node.str(),
                            "the node records the successor that replaced it");
  } else if (request.lifecycle == NodeLifecycle::retired) {
    result.explanations.add(ReasonCode::node_retired, request.node.str(),
                            "a retired node keeps its history and contributes no usable capacity");
  }
  return result;
}

Result<MutationResult> apply_set_node_references(WorkingModel& model,
                                                 const SetNodeReferencesRequest& request) {
  if (request.node.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node");
  }
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  if (request.rack.has_value() && node->kind != SpaceNodeKind::rack) {
    return Error::with_subject(ErrorCode::incompatible_kind,
                               "only a rack node may carry a rack registry reference",
                               request.node.str());
  }
  if (request.rack.has_value() && request.rack->empty()) {
    return Error::with_subject(ErrorCode::malformed_reference,
                               "a rack reference must not be empty", request.node.str());
  }

  if (request.clear_location) {
    node->location.reset();
  } else if (request.location.has_value()) {
    node->location = *request.location;
  }
  if (request.clear_facility_node) {
    node->facility_node.reset();
  } else if (request.facility_node.has_value()) {
    node->facility_node = *request.facility_node;
  }
  if (request.clear_rack) {
    node->rack.reset();
  } else if (request.rack.has_value()) {
    node->rack = *request.rack;
  }
  if (request.assets.has_value()) node->assets = *request.assets;
  if (request.policies.has_value()) node->policies = *request.policies;
  node->generation = next_generation(node->generation);

  MutationResult result;
  result.subject = request.node;
  return result;
}

Result<MutationResult> apply_reparent_node(WorkingModel& model, const ReparentNodeRequest& request) {
  if (request.node.empty() || request.new_parent.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a node and a new parent");
  }
  SpaceNode* node = model.node(request.node);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          node != nullptr ? std::optional<EntityGeneration>{node->generation}
                                          : std::nullopt,
                          subject_of(request.node));
  if (!fence) return fence.error();
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  const SpaceNode* parent = model.node(request.new_parent);
  if (parent == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "the new parent is not in the model",
                               request.new_parent.str());
  }
  if (request.node == request.new_parent) {
    return Error::with_subject(ErrorCode::self_reference, "a node may not contain itself",
                               request.node.str());
  }
  if (model.is_ancestor(request.node, request.new_parent)) {
    return Error::with_subject(ErrorCode::containment_cycle,
                               "the new parent is inside the subtree being moved",
                               request.node.str());
  }
  if (!kind_may_contain(parent->kind, node->kind)) {
    return Error::with_subject(
        ErrorCode::invalid_kind_for_parent,
        "a " + std::string(space_node_kind_name(parent->kind)) + " may not contain a " +
            std::string(space_node_kind_name(node->kind)),
        request.node.str());
  }
  const std::uint32_t height = model.subtree_height(request.node);
  if (parent->depth + 1 + height > Limits::kMaxContainmentDepth) {
    return Error::with_subject(
        ErrorCode::depth_exceeded,
        "the move would put the deepest descendant at depth " +
            to_text(static_cast<std::uint64_t>(parent->depth + 1 + height)) +
            ", above the bound of " +
            to_text(static_cast<std::uint64_t>(Limits::kMaxContainmentDepth)),
        request.node.str());
  }

  const SpaceNodeId previous_parent = node->parent;
  const NodePlacement previous_placement = node->placement;
  node->parent = request.new_parent;
  node->placement = request.placement;
  const Status reindexed = reindex_depths(model, request.node, parent->depth + 1);
  if (!reindexed) {
    node->parent = previous_parent;
    node->placement = previous_placement;
    (void)reindex_depths(model, request.node, model.node(previous_parent) == nullptr
                                                  ? 0
                                                  : model.node(previous_parent)->depth + 1);
    return reindexed.error();
  }
  node->generation = next_generation(node->generation);

  std::vector<SpaceNodeId> moved;
  for (const SpaceNode& member : model.nodes) {
    if (member.id == request.node || model.is_ancestor(request.node, member.id)) {
      moved.push_back(member.id);
    }
  }
  for (const SpaceNodeId& id : moved) {
    const Status placement = check_placement(model, *model.node(id), id);
    if (!placement) {
      node->parent = previous_parent;
      node->placement = previous_placement;
      (void)reindex_depths(model, request.node, model.node(previous_parent) == nullptr
                                                    ? 0
                                                    : model.node(previous_parent)->depth + 1);
      return placement.error();
    }
  }

  MutationResult result;
  result.subject = request.node;
  result.explanations.add(ReasonCode::lineage_preserved, request.node.str(),
                          "the node identity survives the move; only its address changed");
  return result;
}

Result<MutationResult> apply_retire_node(WorkingModel& model, const RetireNodeRequest& request) {
  SetNodeLifecycleRequest lifecycle;
  lifecycle.node = request.node;
  lifecycle.precondition = request.precondition;
  lifecycle.request_id = request.request_id;
  lifecycle.lifecycle = request.successor.empty() ? NodeLifecycle::retired
                                                  : NodeLifecycle::replaced;
  lifecycle.successor = request.successor;
  return apply_set_node_lifecycle(model, lifecycle);
}

// ---------------------------------------------------------------------------
// Claim mutations
// ---------------------------------------------------------------------------

Result<MutationResult> apply_create_claim(WorkingModel& model, const CreateClaimRequest& request) {
  const Status shape = check_argument_shape(request.claim);
  if (!shape) return shape.error();
  if (request.claim.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created claim must carry an identity");
  }
  if (request.claim.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range, "a created claim must be at generation 1");
  }
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.claim.node));
  if (!fence) return fence.error();
  if (model.claim(request.claim.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists, "a claim with this identity exists",
                               request.claim.id.str());
  }
  if (model.node(request.claim.node) == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "the claim names a node that is not in the "
                                                     "model",
                               request.claim.node.str());
  }
  const SpaceNode* node = model.node(request.claim.node);
  if (lifecycle_is_terminal(node->lifecycle)) {
    return Error::with_subject(
        ErrorCode::node_not_available,
        "the node is " + std::string(node_lifecycle_name(node->lifecycle)) +
            " and can no longer receive occupancy",
        request.claim.node.str());
  }
  if (!request.allow_consuming_creation && claim_state_consumes(request.claim.state)) {
    return Error::with_subject(
        ErrorCode::conflict,
        "a claim that consumes space must be created non-consuming and committed by an explicit "
        "transition",
        request.claim.id.str());
  }

  const bool consuming = claim_state_consumes(request.claim.state);
  if (consuming) {
    const Status overlap = check_consuming_overlap(model, request.claim.scope, request.claim.node);
    if (!overlap) return overlap.error();
    const Status blocked = check_blocked_by_exclusion(model, request.claim.scope, request.claim.node,
                                                      Timestamp{});
    if (!blocked) return blocked.error();
  }

  OccupancyClaim stored = request.claim;
  if (stored.evidence.empty()) {
    // Every claim records the attempt at which it was observed, so evidence
    // carried across a restart can be told apart from fresh evidence.
    EvidenceRef reference;
    reference.source = RegistryKind::none;
    reference.id = ExternalId{};
    reference.generation = EntityGeneration{0};
    reference.observed_at = model.attempt;
    (void)reference;
  }
  upsert<OccupancyClaim, OccupancyClaimId>(model.claims, std::move(stored));

  MutationResult result;
  result.subject = request.claim.node;
  if (consuming) {
    result.explanations.add(ReasonCode::committed_occupancy_subtracted, request.claim.node.str(),
                            "the claim consumes space now and is subtracted from availability");
  } else {
    result.explanations.add(ReasonCode::planned_footprint_excluded, request.claim.node.str(),
                            "the claim is pending: it is reported and subtracted from nothing");
  }
  return result;
}

Result<MutationResult> apply_transition_claim(WorkingModel& model,
                                              const TransitionClaimRequest& request) {
  if (request.claim.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a claim");
  }
  OccupancyClaim* claim = model.claim(request.claim);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          claim != nullptr ? std::optional<EntityGeneration>{claim->generation}
                                           : std::nullopt,
                          request.claim.str());
  if (!fence) return fence.error();
  if (claim == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such claim", request.claim.str());
  }
  const Status legal = validate_claim_transition(*claim, request.next);
  if (!legal) {
    return Error::with_subject(legal.code(), legal.error().message, request.claim.str());
  }
  if (claim_state_consumes(request.next) && !claim_state_consumes(claim->state)) {
    const SpaceNode* node = model.node(claim->node);
    if (node == nullptr) {
      return Error::with_subject(ErrorCode::not_found,
                                 "the claim names a node that is not in the model",
                                 request.claim.str());
    }
    if (lifecycle_is_terminal(node->lifecycle)) {
      return Error::with_subject(ErrorCode::node_not_available,
                                 "the node is out of service and cannot take occupancy",
                                 request.claim.str());
    }
    const Status overlap =
        check_consuming_overlap(model, claim->scope, claim->node, claim->id, SpaceNodeId{});
    if (!overlap) return overlap.error();
    const Status blocked =
        check_blocked_by_exclusion(model, claim->scope, claim->node, Timestamp{});
    if (!blocked) return blocked.error();
  }

  claim->state = request.next;
  claim->generation = next_generation(claim->generation);

  MutationResult result;
  result.subject = claim->node;
  if (claim_state_consumes(request.next)) {
    result.explanations.add(ReasonCode::committed_occupancy_subtracted, claim->node.str(),
                            "the claim now consumes space and is subtracted from availability");
  } else if (claim_state_is_terminal(request.next)) {
    result.explanations.add(ReasonCode::committed_occupancy_subtracted, claim->node.str(),
                            "the claim is terminal and consumes nothing");
  }
  return result;
}

Result<MutationResult> apply_set_claim_details(WorkingModel& model,
                                               const SetClaimDetailsRequest& request) {
  if (request.claim.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a claim");
  }
  if (request.clear_label && request.label.has_value()) {
    return Error::make(ErrorCode::invalid_argument, "a field cannot be both set and cleared");
  }
  OccupancyClaim* claim = model.claim(request.claim);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          claim != nullptr ? std::optional<EntityGeneration>{claim->generation}
                                           : std::nullopt,
                          request.claim.str());
  if (!fence) return fence.error();
  if (claim == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such claim", request.claim.str());
  }
  if (claim->is_terminal()) {
    return Error::with_subject(ErrorCode::record_retired,
                               "a terminal claim may not be edited", request.claim.str());
  }
  if (request.clear_label) {
    claim->label = DisplayLabel{};
  } else if (request.label.has_value()) {
    claim->label = *request.label;
  }
  if (request.clear_note) {
    claim->note = Note{};
  } else if (request.note.has_value()) {
    claim->note = *request.note;
  }
  if (request.occupant.has_value()) claim->occupant = *request.occupant;
  if (request.clear_rack) {
    claim->rack.reset();
  } else if (request.rack.has_value()) {
    claim->rack = *request.rack;
  }
  if (request.assets.has_value()) claim->assets = *request.assets;
  if (request.policies.has_value()) claim->policies = *request.policies;
  if (request.evidence.has_value()) claim->evidence = *request.evidence;
  claim->generation = next_generation(claim->generation);

  MutationResult result;
  result.subject = claim->node;
  return result;
}

// ---------------------------------------------------------------------------
// Reservation mutations
// ---------------------------------------------------------------------------

Result<MutationResult> apply_create_reservation(WorkingModel& model,
                                                const CreateReservationRequest& request) {
  const Status shape = check_argument_shape(request.reservation);
  if (!shape) return shape.error();
  if (request.reservation.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created reservation must carry an identity");
  }
  if (request.reservation.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range, "a created reservation must be at generation 1");
  }
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.reservation.node));
  if (!fence) return fence.error();
  if (model.reservation(request.reservation.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists,
                               "a reservation with this identity exists",
                               request.reservation.id.str());
  }
  if (model.claim(request.reservation.id) != nullptr) {
    return Error::with_subject(ErrorCode::identity_conflict,
                               "the identity is already used by a claim",
                               request.reservation.id.str());
  }
  if (model.node(request.reservation.node) == nullptr) {
    return Error::with_subject(ErrorCode::not_found,
                               "the reservation names a node that is not in the model",
                               request.reservation.node.str());
  }
  if (reservation_state_holds(request.reservation.state)) {
    const SpaceNode* node = model.node(request.reservation.node);
    if (lifecycle_is_terminal(node->lifecycle)) {
      return Error::with_subject(ErrorCode::node_not_available,
                                 "the node is out of service and cannot hold space",
                                 request.reservation.node.str());
    }
    const Status overlap =
        check_consuming_overlap(model, request.reservation.scope, request.reservation.node);
    if (!overlap) return overlap.error();
    const Status blocked = check_blocked_by_exclusion(model, request.reservation.scope,
                                                      request.reservation.node, Timestamp{});
    if (!blocked) return blocked.error();
  }

  upsert<FootprintReservation, OccupancyClaimId>(model.reservations, request.reservation);

  MutationResult result;
  result.subject = request.reservation.node;
  if (reservation_state_holds(request.reservation.state)) {
    result.explanations.add(ReasonCode::reserved_footprint_subtracted, request.reservation.node.str(),
                            "the hold is in force and is subtracted from availability; Space "
                            "Capacity records it and did not decide it");
  }
  return result;
}

Result<MutationResult> apply_transition_reservation(WorkingModel& model,
                                                    const TransitionReservationRequest& request) {
  if (request.reservation.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name a reservation");
  }
  FootprintReservation* reservation = model.reservation(request.reservation);
  const Status fence = check_preconditions(
      request.precondition, model.base_revision,
      reservation != nullptr ? std::optional<EntityGeneration>{reservation->generation}
                             : std::nullopt,
      request.reservation.str());
  if (!fence) return fence.error();
  if (reservation == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such reservation",
                               request.reservation.str());
  }
  if (!reservation_transition_allowed(reservation->state, request.next)) {
    return Error::with_subject(
        ErrorCode::conflict,
        std::string(reservation_state_name(reservation->state)) + " to " +
            std::string(reservation_state_name(request.next)) + " is not a permitted transition",
        request.reservation.str());
  }
  if (request.next == ReservationState::revoked) {
    if (!request.revocation_authority.has_value() ||
        request.revocation_authority->empty()) {
      return Error::with_subject(
          ErrorCode::reservation_authority_missing,
          "a revocation must name the authority that withdrew the hold; Space Capacity does not "
          "withdraw a hold on its own initiative",
          request.reservation.str());
    }
    reservation->reservation = *request.revocation_authority;
  }
  if (reservation_state_holds(request.next) &&
      !reservation_state_holds(reservation->state)) {
    const Status overlap = check_consuming_overlap(model, reservation->scope, reservation->node,
                                                   reservation->id, SpaceNodeId{});
    if (!overlap) return overlap.error();
  }
  reservation->state = request.next;
  reservation->generation = next_generation(reservation->generation);

  MutationResult result;
  result.subject = reservation->node;
  if (!reservation_state_holds(request.next)) {
    result.explanations.add(ReasonCode::reserved_footprint_subtracted, reservation->node.str(),
                            "the hold is no longer in force and is subtracted from nothing");
  }
  return result;
}

// ---------------------------------------------------------------------------
// Exclusion, clearance and expansion mutations
// ---------------------------------------------------------------------------

Result<MutationResult> apply_create_exclusion(WorkingModel& model,
                                              const CreateExclusionRequest& request) {
  const Status shape = check_argument_shape(request.region);
  if (!shape) return shape.error();
  if (request.region.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created exclusion must carry an identity");
  }
  if (request.region.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range, "a created exclusion must be at generation 1");
  }
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.region.node));
  if (!fence) return fence.error();
  if (model.exclusion(request.region.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists,
                               "an exclusion with this identity exists", request.region.id.str());
  }
  if (model.node(request.region.node) == nullptr) {
    return Error::with_subject(ErrorCode::not_found,
                               "the exclusion names a node that is not in the model",
                               request.region.node.str());
  }
  upsert<ExclusionRegion, ExclusionRegionId>(model.exclusions, request.region);

  MutationResult result;
  result.subject = request.region.node;
  result.explanations.add(ReasonCode::excluded_by_region, request.region.node.str(),
                          "an exclusion reduces usable capacity whether or not anything is there");
  return result;
}

Result<MutationResult> apply_transition_exclusion(WorkingModel& model,
                                                  const TransitionExclusionRequest& request) {
  if (request.region.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name an exclusion");
  }
  ExclusionRegion* region = model.exclusion(request.region);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          region != nullptr ? std::optional<EntityGeneration>{region->generation}
                                            : std::nullopt,
                          request.region.str());
  if (!fence) return fence.error();
  if (region == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such exclusion", request.region.str());
  }
  if (!exclusion_transition_allowed(region->state, request.next)) {
    return Error::with_subject(ErrorCode::conflict,
                               std::string(exclusion_state_name(region->state)) + " to " +
                                   std::string(exclusion_state_name(request.next)) +
                                   " is not a permitted transition",
                               request.region.str());
  }
  region->state = request.next;
  region->generation = next_generation(region->generation);

  MutationResult result;
  result.subject = region->node;
  return result;
}

Result<MutationResult> apply_create_clearance(WorkingModel& model,
                                              const CreateClearanceRequest& request) {
  const Status shape = check_argument_shape(request.constraint);
  if (!shape) return shape.error();
  if (request.constraint.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created clearance must carry an identity");
  }
  if (request.constraint.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range, "a created clearance must be at generation 1");
  }
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.constraint.node));
  if (!fence) return fence.error();
  if (model.clearance(request.constraint.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists,
                               "a clearance with this identity exists",
                               request.constraint.id.str());
  }
  if (model.node(request.constraint.node) == nullptr) {
    return Error::with_subject(ErrorCode::not_found,
                               "the clearance names a node that is not in the model",
                               request.constraint.node.str());
  }
  upsert<ClearanceConstraint, ClearanceConstraintId>(model.clearances, request.constraint);

  MutationResult result;
  result.subject = request.constraint.node;
  result.explanations.add(request.constraint.enforceable ? ReasonCode::blocked_by_clearance
                                                         : ReasonCode::clearance_not_enforceable,
                          request.constraint.node.str(),
                          request.constraint.enforceable
                              ? "the clearance band is subtracted from usable capacity"
                              : "the clearance band is reported and subtracted from nothing");
  return result;
}

Result<MutationResult> apply_create_expansion_zone(WorkingModel& model,
                                                   const CreateExpansionZoneRequest& request) {
  const Status shape = check_argument_shape(request.zone);
  if (!shape) return shape.error();
  if (request.zone.id.empty()) {
    return Error::make(ErrorCode::empty_value, "a created expansion zone must carry an identity");
  }
  if (request.zone.generation.value() != 1) {
    return Error::make(ErrorCode::invalid_range,
                       "a created expansion zone must be at generation 1");
  }
  const Status fence =
      check_preconditions(request.precondition, model.base_revision, std::nullopt,
                          subject_of(request.zone.node));
  if (!fence) return fence.error();
  if (model.zone(request.zone.id) != nullptr) {
    return Error::with_subject(ErrorCode::already_exists,
                               "an expansion zone with this identity exists",
                               request.zone.id.str());
  }
  if (model.node(request.zone.node) == nullptr) {
    return Error::with_subject(ErrorCode::not_found,
                               "the expansion zone names a node that is not in the model",
                               request.zone.node.str());
  }
  if (request.zone.earmarks()) {
    const Status overlap = check_consuming_overlap(model, request.zone.scope, request.zone.node);
    if (!overlap) return overlap.error();
  }
  upsert<ExpansionZone, ExpansionZoneId>(model.zones, request.zone);

  MutationResult result;
  result.subject = request.zone.node;
  if (request.zone.earmarks()) {
    result.explanations.add(ReasonCode::earmarked_by_expansion, request.zone.node.str(),
                            "the space is earmarked for a future build-out and is subtracted from "
                            "ordinary availability");
  }
  return result;
}

Result<MutationResult> apply_transition_expansion_zone(
    WorkingModel& model, const TransitionExpansionZoneRequest& request) {
  if (request.zone.empty()) {
    return Error::make(ErrorCode::empty_value, "the request must name an expansion zone");
  }
  ExpansionZone* zone = model.zone(request.zone);
  const Status fence =
      check_preconditions(request.precondition, model.base_revision,
                          zone != nullptr ? std::optional<EntityGeneration>{zone->generation}
                                          : std::nullopt,
                          request.zone.str());
  if (!fence) return fence.error();
  if (zone == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such expansion zone", request.zone.str());
  }
  if (!expansion_transition_allowed(zone->state, request.next)) {
    return Error::with_subject(ErrorCode::conflict,
                               std::string(expansion_state_name(zone->state)) + " to " +
                                   std::string(expansion_state_name(request.next)) +
                                   " is not a permitted transition",
                               request.zone.str());
  }
  if (expansion_state_earmarks(request.next) && !expansion_state_earmarks(zone->state)) {
    const Status overlap = check_consuming_overlap(model, zone->scope, zone->node);
    if (!overlap) return overlap.error();
  }
  zone->state = request.next;
  zone->generation = next_generation(zone->generation);

  MutationResult result;
  result.subject = zone->node;
  return result;
}

// ---------------------------------------------------------------------------
// Request keys
// ---------------------------------------------------------------------------

const RequestId& request_key(const CreateNodeRequest& request) { return request.request_id; }
const RequestId& request_key(const SetNodeMetadataRequest& request) { return request.request_id; }
const RequestId& request_key(const SetNodeEnvelopeRequest& request) { return request.request_id; }
const RequestId& request_key(const SetNodePlacementRequest& request) { return request.request_id; }
const RequestId& request_key(const SetNodeLifecycleRequest& request) { return request.request_id; }
const RequestId& request_key(const SetNodeReferencesRequest& request) { return request.request_id; }
const RequestId& request_key(const ReparentNodeRequest& request) { return request.request_id; }
const RequestId& request_key(const RetireNodeRequest& request) { return request.request_id; }
const RequestId& request_key(const CreateClaimRequest& request) { return request.request_id; }
const RequestId& request_key(const TransitionClaimRequest& request) { return request.request_id; }
const RequestId& request_key(const SetClaimDetailsRequest& request) { return request.request_id; }
const RequestId& request_key(const CreateReservationRequest& request) { return request.request_id; }
const RequestId& request_key(const TransitionReservationRequest& request) {
  return request.request_id;
}
const RequestId& request_key(const CreateExclusionRequest& request) { return request.request_id; }
const RequestId& request_key(const TransitionExclusionRequest& request) {
  return request.request_id;
}
const RequestId& request_key(const CreateClearanceRequest& request) { return request.request_id; }
const RequestId& request_key(const CreateExpansionZoneRequest& request) {
  return request.request_id;
}
const RequestId& request_key(const TransitionExpansionZoneRequest& request) {
  return request.request_id;
}

}  // namespace dccp::space_capacity::internal
