// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - immutable snapshots, the query engine over them, and the
// model audit.
//
// The audit is deliberately the strictest pass in the library. It re-derives
// every ledger identity, re-checks every containment rule, re-checks that no
// two authority-granting records cover the same space, and re-checks that
// every record's scope is inside the envelope it names. Opening a store runs
// exactly this pass, so an inspection tool can never accept state that a read
// path would refuse.

#include "dccp/space_capacity/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/text.hpp"
#include "engine.hpp"
#include "geometry.hpp"

namespace dccp::space_capacity {
namespace {

template <typename Record, typename Id>
bool sort_and_check_unique(std::vector<Record>& records, const Id&, std::string& duplicate) {
  std::sort(records.begin(), records.end());
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].id == records[i - 1].id) {
      duplicate = records[i].id.str();
      return false;
    }
  }
  return true;
}

template <typename Record, typename Id>
const Record* find_sorted(const std::vector<Record>& records, const Id& id) noexcept {
  const auto it = std::lower_bound(records.begin(), records.end(), id,
                                   [](const Record& record, const Id& key) {
                                     return record.id < key;
                                   });
  if (it == records.end() || it->id != id) return nullptr;
  return &*it;
}

}  // namespace

struct Snapshot::Index final {
  std::unordered_map<SpaceNodeId, std::size_t> node_slot;
  std::unordered_map<SpaceNodeId, std::vector<SpaceNodeId>> children;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SnapshotPtr Snapshot::make_empty(StoreId store, StoreIncarnation incarnation,
                                 RegistryRevision revision, AttemptId attempt) {
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->store_ = std::move(store);
  snapshot->incarnation_ = incarnation;
  snapshot->revision_ = revision;
  snapshot->attempt_ = attempt;
  snapshot->index_ = std::make_shared<Index>();
  // The digest of an empty model is the digest of the canonical encoding of an
  // empty model's records, so an empty snapshot's digest is the digest of its
  // own canonical bytes, exactly like every other snapshot's.
  Snapshot::BuildInput empty;
  snapshot->digest_ = digest_in_domain(kDomainStateBinary, internal::encode_payload(empty));
  return snapshot;
}

Result<SnapshotPtr> Snapshot::build(BuildInput input) {
  if (input.nodes.size() > Limits::kMaxNodes) {
    return Error::make(ErrorCode::limit_exceeded,
                       "a model may hold at most " + to_text(static_cast<std::uint64_t>(
                                                          Limits::kMaxNodes)) +
                           " nodes");
  }
  if (input.claims.size() > Limits::kMaxClaims) {
    return Error::make(ErrorCode::limit_exceeded, "too many claims");
  }
  if (input.reservations.size() > Limits::kMaxReservations) {
    return Error::make(ErrorCode::limit_exceeded, "too many reservations");
  }
  if (input.exclusions.size() > Limits::kMaxExclusions) {
    return Error::make(ErrorCode::limit_exceeded, "too many exclusion regions");
  }
  if (input.clearances.size() > Limits::kMaxClearances) {
    return Error::make(ErrorCode::limit_exceeded, "too many clearance constraints");
  }
  if (input.expansion_zones.size() > Limits::kMaxExpansionZones) {
    return Error::make(ErrorCode::limit_exceeded, "too many expansion zones");
  }

  std::string duplicate;
  if (!sort_and_check_unique(input.nodes, SpaceNodeId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity, "duplicate node identity " + duplicate);
  }
  if (!sort_and_check_unique(input.claims, OccupancyClaimId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity, "duplicate claim identity " + duplicate);
  }
  if (!sort_and_check_unique(input.reservations, OccupancyClaimId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity,
                       "duplicate reservation identity " + duplicate);
  }
  if (!sort_and_check_unique(input.exclusions, ExclusionRegionId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity, "duplicate exclusion identity " + duplicate);
  }
  if (!sort_and_check_unique(input.clearances, ClearanceConstraintId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity, "duplicate clearance identity " + duplicate);
  }
  if (!sort_and_check_unique(input.expansion_zones, ExpansionZoneId{}, duplicate)) {
    return Error::make(ErrorCode::duplicate_identity,
                       "duplicate expansion zone identity " + duplicate);
  }

  auto snapshot = std::make_shared<Snapshot>();
  snapshot->store_ = std::move(input.store);
  snapshot->incarnation_ = input.incarnation;
  snapshot->revision_ = input.revision;
  snapshot->attempt_ = input.attempt;
  snapshot->created_at_ = input.created_at;
  snapshot->nodes_ = std::move(input.nodes);
  snapshot->claims_ = std::move(input.claims);
  snapshot->reservations_ = std::move(input.reservations);
  snapshot->exclusions_ = std::move(input.exclusions);
  snapshot->clearances_ = std::move(input.clearances);
  snapshot->expansion_zones_ = std::move(input.expansion_zones);

  auto index = std::make_shared<Index>();
  index->node_slot.reserve(snapshot->nodes_.size());
  for (std::size_t i = 0; i < snapshot->nodes_.size(); ++i) {
    index->node_slot.emplace(snapshot->nodes_[i].id, i);
  }
  for (const SpaceNode& node : snapshot->nodes_) {
    if (!node.parent.empty()) {
      index->children[node.parent].push_back(node.id);
    }
  }
  for (auto& entry : index->children) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  snapshot->index_ = std::move(index);

  // The digest is taken over the canonical serialization of exactly the records
  // the store would write, so two models that describe the same space agree
  // byte for byte regardless of the order the records arrived in.
  Snapshot::BuildInput canonical;
  canonical.store = snapshot->store_;
  canonical.incarnation = snapshot->incarnation_;
  canonical.revision = snapshot->revision_;
  canonical.attempt = snapshot->attempt_;
  canonical.created_at = snapshot->created_at_;
  canonical.nodes = snapshot->nodes_;
  canonical.claims = snapshot->claims_;
  canonical.reservations = snapshot->reservations_;
  canonical.exclusions = snapshot->exclusions_;
  canonical.clearances = snapshot->clearances_;
  canonical.expansion_zones = snapshot->expansion_zones_;
  const std::string bytes = internal::encode_payload(canonical);
  snapshot->digest_ = digest_in_domain(kDomainStateBinary, bytes);

  const Status audited = snapshot->audit();
  if (!audited) return audited.error();

  return SnapshotPtr{std::move(snapshot)};
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::size_t Snapshot::record_count() const noexcept {
  return nodes_.size() + claims_.size() + reservations_.size() + exclusions_.size() +
         clearances_.size() + expansion_zones_.size();
}

const SpaceNode* Snapshot::find_node(const SpaceNodeId& id) const noexcept {
  return find_sorted(nodes_, id);
}

const OccupancyClaim* Snapshot::find_claim(const OccupancyClaimId& id) const noexcept {
  return find_sorted(claims_, id);
}

const FootprintReservation* Snapshot::find_reservation(const OccupancyClaimId& id) const noexcept {
  return find_sorted(reservations_, id);
}

const ExclusionRegion* Snapshot::find_exclusion(const ExclusionRegionId& id) const noexcept {
  return find_sorted(exclusions_, id);
}

const ClearanceConstraint* Snapshot::find_clearance(
    const ClearanceConstraintId& id) const noexcept {
  return find_sorted(clearances_, id);
}

const ExpansionZone* Snapshot::find_expansion_zone(const ExpansionZoneId& id) const noexcept {
  return find_sorted(expansion_zones_, id);
}

std::vector<SpaceNodeId> Snapshot::children_of(const SpaceNodeId& parent) const {
  if (!index_) return {};
  const auto it = index_->children.find(parent);
  if (it == index_->children.end()) return {};
  return it->second;
}

Result<std::vector<SpaceNodeId>> Snapshot::subtree_of(const SpaceNodeId& root) const {
  if (find_node(root) == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", root.str());
  }
  std::vector<SpaceNodeId> collected;
  collected.reserve(16);
  std::vector<SpaceNodeId> pending{root};
  while (!pending.empty()) {
    SpaceNodeId current = pending.back();
    pending.pop_back();
    collected.push_back(current);
    if (collected.size() > Limits::kMaxNodes) {
      return Error::make(ErrorCode::limit_exceeded, "the subtree exceeds the node bound");
    }
    for (const SpaceNodeId& child : children_of(current)) {
      pending.push_back(child);
    }
    if (collected.size() + pending.size() > Limits::kMaxNodes) {
      return Error::make(ErrorCode::limit_exceeded, "the subtree exceeds the node bound");
    }
  }
  std::sort(collected.begin(), collected.end());
  return collected;
}

Result<std::vector<SpaceNodeId>> Snapshot::ancestors_of(const SpaceNodeId& node) const {
  const SpaceNode* current = find_node(node);
  if (current == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", node.str());
  }
  std::vector<SpaceNodeId> chain;
  SpaceNodeId cursor = current->parent;
  std::uint32_t guard = 0;
  while (!cursor.empty()) {
    if (++guard > Limits::kMaxContainmentDepth + 1) {
      return Error::with_subject(ErrorCode::containment_cycle,
                                 "the parent chain does not terminate", node.str());
    }
    chain.push_back(cursor);
    const SpaceNode* parent = find_node(cursor);
    if (parent == nullptr) {
      return Error::with_subject(ErrorCode::lineage_broken,
                                 "the parent chain names a node that is not in the model",
                                 cursor.str());
    }
    cursor = parent->parent;
  }
  return chain;
}

SpaceNodeId Snapshot::plane_owner_of(const SpaceNodeId& node) const {
  const SpaceNode* current = find_node(node);
  std::uint32_t guard = 0;
  while (current != nullptr) {
    if (current->own_planar.is_declared()) return current->id;
    if (++guard > Limits::kMaxContainmentDepth + 1) return SpaceNodeId{};
    current = current->parent.empty() ? nullptr : find_node(current->parent);
  }
  return SpaceNodeId{};
}

SpaceNodeId Snapshot::rack_owner_of(const SpaceNodeId& node) const {
  const SpaceNode* current = find_node(node);
  std::uint32_t guard = 0;
  while (current != nullptr) {
    if (current->own_rack.is_declared() && kind_may_declare_rack_envelope(current->kind)) {
      return current->id;
    }
    if (++guard > Limits::kMaxContainmentDepth + 1) return SpaceNodeId{};
    current = current->parent.empty() ? nullptr : find_node(current->parent);
  }
  return SpaceNodeId{};
}

bool Snapshot::is_strict_ancestor(const SpaceNodeId& ancestor,
                                  const SpaceNodeId& descendant) const {
  if (ancestor.empty() || descendant.empty() || ancestor == descendant) return false;
  const SpaceNode* current = find_node(descendant);
  std::uint32_t guard = 0;
  while (current != nullptr && !current->parent.empty()) {
    if (current->parent == ancestor) return true;
    if (++guard > Limits::kMaxContainmentDepth + 1) return false;
    current = find_node(current->parent);
  }
  return false;
}

std::vector<SpaceNodeId> Snapshot::roots() const {
  std::vector<SpaceNodeId> result;
  for (const SpaceNode& node : nodes_) {
    if (node.parent.empty()) result.push_back(node.id);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

NodeCapacity Snapshot::capacity_of(const SpaceNodeId& node,
                                   const FitContext& context) const {
  NodeCapacity capacity;
  const SpaceNode* record = find_node(node);
  if (record == nullptr) return capacity;

  capacity.node = node;
  capacity.generation = record->generation;
  capacity.lifecycle = record->lifecycle;
  capacity.owns_plane = record->own_planar.is_declared();
  capacity.owns_rack_envelope = record->own_rack.is_declared() &&
                                kind_may_declare_rack_envelope(record->kind);
  capacity.attributed_plane = plane_owner_of(node);

  if (capacity.owns_plane) {
    capacity.area = internal::ledger_from(internal::measure_plane(*this, node, context));
  } else {
    // A grouping node declares no plane. Its area is genuinely unknown here,
    // and the enclosure it sits in is named instead of reporting a zero.
    capacity.area.state = MeasureState::unknown;
  }
  if (capacity.owns_rack_envelope) {
    capacity.units = internal::ledger_from(internal::measure_rack(*this, node, context));
  } else {
    capacity.units.state = MeasureState::unknown;
  }
  return capacity;
}

namespace {

// Sums signed-checked values into an accumulator, reporting overflow.
template <typename T>
bool accumulate(T& into, T value) {
  const Checked<T> sum = checked_add_signed(into, value);
  if (!sum) return false;
  into = sum.value;
  return true;
}

}  // namespace

Result<CapacityRollup> Snapshot::rollup(const SpaceNodeId& node,
                                        const FitContext& context) const {
  Result<std::vector<SpaceNodeId>> subtree = subtree_of(node);
  if (!subtree) return subtree.error();

  const internal::ModelMeasure model = internal::measure_model(*this, context);

  CapacityRollup result;
  result.root = node;
  const SpaceNode* root = find_node(node);
  result.root_generation = root != nullptr ? root->generation : EntityGeneration{};
  result.revision = revision_;
  result.node_count = subtree.value().size();

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

  std::int64_t unit_declared = 0;
  std::int64_t unit_excluded = 0;
  std::int64_t unit_usable = 0;
  std::int64_t unit_occupied = 0;
  std::int64_t unit_available = 0;
  std::int64_t unit_pending = 0;
  std::int64_t unit_planned = 0;

  for (const SpaceNodeId& id : subtree.value()) {
    const SpaceNode* record = find_node(id);
    if (record == nullptr) continue;
    if (!lifecycle_is_usable_now(record->lifecycle) && !lifecycle_is_planned(record->lifecycle)) {
      result.contains_inactive_nodes = true;
    }

    const auto plane_it = model.planes.find(id);
    if (plane_it != model.planes.end()) {
      const internal::PlaneMeasure& measure = plane_it->second;
      ++result.plane_owner_count;
      // Every plane contributes exactly once. A plane's declared area already
      // excludes the area of the sub-planes it contains, so a nested plane is
      // never counted twice: the two areas are disjoint by construction.
      if (!accumulate(declared, measure.declared.value())) {
        return Error::make(ErrorCode::arithmetic_overflow, "declared area rollup overflowed");
      }
      if (!accumulate(excluded, measure.excluded.value()) ||
          !accumulate(usable, measure.usable_area.value()) ||
          !accumulate(structural, measure.structural.value()) ||
          !accumulate(claimed, measure.claimed.value()) ||
          !accumulate(held, measure.held.value()) ||
          !accumulate(earmarked, measure.earmarked.value()) ||
          !accumulate(available, measure.available.value()) ||
          !accumulate(pending, measure.pending.value()) ||
          !accumulate(planned, measure.planned_area.value())) {
        return Error::make(ErrorCode::arithmetic_overflow, "area rollup overflowed");
      }
      result.area.over_committed = result.area.over_committed || measure.over_committed;
      if (measure.overflowed) result.area.state = MeasureState::unknown;
      if (!record->own_planar.is_declared()) result.contains_undeclared_envelopes = true;
    } else if (kind_may_declare_plane(record->kind)) {
      result.contains_undeclared_envelopes = true;
    }

    const auto rack_it = model.racks.find(id);
    if (rack_it != model.racks.end()) {
      const internal::RackMeasure& measure = rack_it->second;
      ++result.rack_count;
      if (!accumulate(unit_declared, static_cast<std::int64_t>(measure.declared.value())) ||
          !accumulate(unit_excluded, static_cast<std::int64_t>(measure.excluded.value())) ||
          !accumulate(unit_usable, static_cast<std::int64_t>(measure.usable_units.value())) ||
          !accumulate(unit_occupied, static_cast<std::int64_t>(measure.occupied.value())) ||
          !accumulate(unit_available, static_cast<std::int64_t>(measure.available.value())) ||
          !accumulate(unit_pending, static_cast<std::int64_t>(measure.pending.value())) ||
          !accumulate(unit_planned, static_cast<std::int64_t>(measure.planned_units.value()))) {
        return Error::make(ErrorCode::arithmetic_overflow, "rack unit rollup overflowed");
      }
      result.units.over_committed = result.units.over_committed || measure.over_committed;
      if (measure.overflowed) result.units.state = MeasureState::unknown;
      result.units.free_runs += measure.free_runs;
      if (measure.largest_free_run.value() > result.units.largest_free_run.value()) {
        result.units.largest_free_run = measure.largest_free_run;
      }
    }
  }

  // Claims, reservations and regions are counted once each, in the subtree
  // that owns them.
  for (const OccupancyClaim& claim : claims_) {
    if (std::binary_search(subtree.value().begin(), subtree.value().end(), claim.node)) {
      ++result.claim_count;
    }
  }
  for (const FootprintReservation& reservation : reservations_) {
    if (std::binary_search(subtree.value().begin(), subtree.value().end(), reservation.node)) {
      ++result.reservation_count;
    }
  }
  for (const ExclusionRegion& region : exclusions_) {
    if (std::binary_search(subtree.value().begin(), subtree.value().end(), region.node)) {
      ++result.exclusion_count;
    }
  }
  for (const ClearanceConstraint& clearance : clearances_) {
    if (std::binary_search(subtree.value().begin(), subtree.value().end(), clearance.node)) {
      ++result.clearance_count;
    }
  }
  for (const ExpansionZone& zone : expansion_zones_) {
    if (std::binary_search(subtree.value().begin(), subtree.value().end(), zone.node)) {
      ++result.expansion_zone_count;
    }
  }

  if (declared < 0 || excluded < 0 || usable < 0 || structural < 0 || claimed < 0 || held < 0 ||
      earmarked < 0 || available < 0 || pending < 0 || planned < 0) {
    return Error::make(ErrorCode::arithmetic_overflow, "area rollup left the signed range");
  }

  result.area.declared = SquareMillimeters{declared};
  result.area.excluded = SquareMillimeters{excluded};
  result.area.usable = SquareMillimeters{usable};
  result.area.structural = SquareMillimeters{structural};
  result.area.claimed = SquareMillimeters{claimed};
  result.area.held = SquareMillimeters{held};
  result.area.earmarked = SquareMillimeters{earmarked};
  result.area.available = SquareMillimeters{available};
  result.area.pending = SquareMillimeters{pending};
  result.area.planned = SquareMillimeters{planned};

  result.units.declared = RackUnits{static_cast<std::int32_t>(unit_declared)};
  result.units.excluded = RackUnits{static_cast<std::int32_t>(unit_excluded)};
  result.units.usable = RackUnits{static_cast<std::int32_t>(unit_usable)};
  result.units.occupied = RackUnits{static_cast<std::int32_t>(unit_occupied)};
  result.units.available = RackUnits{static_cast<std::int32_t>(unit_available)};
  result.units.pending = RackUnits{static_cast<std::int32_t>(unit_pending)};
  result.units.planned = RackUnits{static_cast<std::int32_t>(unit_planned)};
  if (unit_available > 0) {
    const std::int64_t wasted = unit_available - result.units.largest_free_run.value();
    result.units.fragmentation_ppm =
        static_cast<std::uint32_t>((wasted * 1'000'000) / unit_available);
  }
  result.over_committed = result.area.over_committed || result.units.over_committed;
  return result;
}

CapacityRollup model_rollup(const Snapshot& snapshot, const FitContext& context) {
  const internal::ModelMeasure model = internal::measure_model(snapshot, context);

  CapacityRollup result;
  result.revision = snapshot.revision();
  result.node_count = snapshot.node_count();

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
  std::int64_t unit_declared = 0;
  std::int64_t unit_excluded = 0;
  std::int64_t unit_usable = 0;
  std::int64_t unit_occupied = 0;
  std::int64_t unit_available = 0;
  std::int64_t unit_pending = 0;
  std::int64_t unit_planned = 0;

  for (const SpaceNode& node : snapshot.nodes()) {
    if (!lifecycle_is_usable_now(node.lifecycle) && !lifecycle_is_planned(node.lifecycle)) {
      result.contains_inactive_nodes = true;
    }
    const auto plane_it = model.planes.find(node.id);
    if (plane_it != model.planes.end()) {
      ++result.plane_owner_count;
      const internal::PlaneMeasure& measure = plane_it->second;
      // Every plane contributes its declared area exactly once, because that
      // area already excludes whatever sub-planes subdivide it.
      declared += measure.declared.value();
      excluded += measure.excluded.value();
      usable += measure.usable_area.value();
      structural += measure.structural.value();
      claimed += measure.claimed.value();
      held += measure.held.value();
      earmarked += measure.earmarked.value();
      available += measure.available.value();
      pending += measure.pending.value();
      planned += measure.planned_area.value();
      result.area.over_committed = result.area.over_committed || measure.over_committed;
      if (measure.overflowed) result.area.state = MeasureState::unknown;
    } else if (kind_may_declare_plane(node.kind)) {
      result.contains_undeclared_envelopes = true;
    }
    const auto rack_it = model.racks.find(node.id);
    if (rack_it != model.racks.end()) {
      ++result.rack_count;
      const internal::RackMeasure& measure = rack_it->second;
      unit_declared += measure.declared.value();
      unit_excluded += measure.excluded.value();
      unit_usable += measure.usable_units.value();
      unit_occupied += measure.occupied.value();
      unit_available += measure.available.value();
      unit_pending += measure.pending.value();
      unit_planned += measure.planned_units.value();
      result.units.over_committed = result.units.over_committed || measure.over_committed;
      if (measure.overflowed) result.units.state = MeasureState::unknown;
      result.units.free_runs += measure.free_runs;
      if (measure.largest_free_run.value() > result.units.largest_free_run.value()) {
        result.units.largest_free_run = measure.largest_free_run;
      }
    }
  }

  result.claim_count = snapshot.claims().size();
  result.reservation_count = snapshot.reservations().size();
  result.exclusion_count = snapshot.exclusions().size();
  result.clearance_count = snapshot.clearances().size();
  result.expansion_zone_count = snapshot.expansion_zones().size();

  result.area.declared = SquareMillimeters{declared};
  result.area.excluded = SquareMillimeters{excluded};
  result.area.usable = SquareMillimeters{usable};
  result.area.structural = SquareMillimeters{structural};
  result.area.claimed = SquareMillimeters{claimed};
  result.area.held = SquareMillimeters{held};
  result.area.earmarked = SquareMillimeters{earmarked};
  result.area.available = SquareMillimeters{available};
  result.area.pending = SquareMillimeters{pending};
  result.area.planned = SquareMillimeters{planned};

  result.units.declared = RackUnits{static_cast<std::int32_t>(unit_declared)};
  result.units.excluded = RackUnits{static_cast<std::int32_t>(unit_excluded)};
  result.units.usable = RackUnits{static_cast<std::int32_t>(unit_usable)};
  result.units.occupied = RackUnits{static_cast<std::int32_t>(unit_occupied)};
  result.units.available = RackUnits{static_cast<std::int32_t>(unit_available)};
  result.units.pending = RackUnits{static_cast<std::int32_t>(unit_pending)};
  result.units.planned = RackUnits{static_cast<std::int32_t>(unit_planned)};
  if (unit_available > 0) {
    const std::int64_t wasted = unit_available - result.units.largest_free_run.value();
    result.units.fragmentation_ppm =
        static_cast<std::uint32_t>((wasted * 1'000'000) / unit_available);
  }
  result.over_committed = result.area.over_committed || result.units.over_committed;
  return result;
}

Result<CapacityReport> Snapshot::report(const SpaceNodeId& node,
                                        const FitContext& context) const {
  const SpaceNode* record = find_node(node);
  if (record == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", node.str());
  }
  Result<std::vector<SpaceNodeId>> subtree = subtree_of(node);
  if (!subtree) return subtree.error();
  Result<CapacityRollup> rolled = rollup(node, context);
  if (!rolled) return rolled.error();

  CapacityReport report;
  report.subject = node;
  report.subject_generation = record->generation;
  report.attributed_plane = plane_owner_of(node);
  report.lifecycle = record->lifecycle;
  report.owns_plane = record->own_planar.is_declared();
  report.owns_rack_envelope = record->own_rack.is_declared() &&
                              kind_may_declare_rack_envelope(record->kind);
  report.area = rolled.value().area;
  report.units = rolled.value().units;
  report.revision = revision_;
  report.attempt = attempt_;

  for (const SpaceNodeId& id : subtree.value()) {
    const SpaceNode* member = find_node(id);
    if (member == nullptr) continue;
    if (!member->own_planar.is_declared() && !member->own_rack.is_declared()) continue;
    report.planes.push_back(capacity_of(id, context));
  }

  if (!report.owns_plane && kind_may_declare_plane(record->kind)) {
    report.explanations.add(ReasonCode::plane_not_declared, node.str(),
                            "the node declares no plane of its own; its area is reported "
                            "through the plane named by attributed_plane");
  }
  if (report.area.state == MeasureState::unknown) {
    report.explanations.add(ReasonCode::unknown_capacity, node.str(),
                            "checked arithmetic refused an area total; no number is claimed");
  }
  if (report.area.over_committed) {
    report.explanations.add(ReasonCode::over_committed, node.str(),
                            "the space used or set aside exceeds the space that exists");
  }
  if (rolled.value().contains_inactive_nodes) {
    report.explanations.add(ReasonCode::node_retired, node.str(),
                            "the subtree contains a node that is not in service; it "
                            "contributes no usable capacity");
  }
  report.explanations.add(ReasonCode::rolled_up, node.str(),
                          "each plane in the subtree is counted exactly once");
  return report;
}

std::string Snapshot::canonical_bytes() const {
  BuildInput input;
  input.store = store_;
  input.incarnation = incarnation_;
  input.revision = revision_;
  input.attempt = attempt_;
  input.created_at = created_at_;
  input.nodes = nodes_;
  input.claims = claims_;
  input.reservations = reservations_;
  input.exclusions = exclusions_;
  input.clearances = clearances_;
  input.expansion_zones = expansion_zones_;
  return internal::encode_payload(input);
}

std::string Snapshot::canonical_text() const { return internal::render_model_text(*this); }

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------

namespace {

// Collects the rectangles a record contributes to the space it consumes.
void append_consuming_rects(const FootprintScope& scope, std::vector<PlanarRect>& out) {
  if (scope.kind != FootprintScopeKind::planar) return;
  out.insert(out.end(), scope.rects.rects().begin(), scope.rects.rects().end());
}

}  // namespace

Status Snapshot::audit() const {
  // ---------------------------------------------------------------- identity
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].id.empty()) {
      return Status::failure(ErrorCode::empty_value, "a node has no identity");
    }
    if (i > 0 && nodes_[i].id == nodes_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate node identity " + nodes_[i].id.str());
    }
  }
  for (std::size_t i = 0; i < claims_.size(); ++i) {
    if (i > 0 && claims_[i].id == claims_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate claim identity " + claims_[i].id.str());
    }
  }
  for (std::size_t i = 0; i < reservations_.size(); ++i) {
    if (i > 0 && reservations_[i].id == reservations_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate reservation identity " + reservations_[i].id.str());
    }
  }
  for (std::size_t i = 0; i < exclusions_.size(); ++i) {
    if (i > 0 && exclusions_[i].id == exclusions_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate exclusion identity " + exclusions_[i].id.str());
    }
  }
  for (std::size_t i = 0; i < clearances_.size(); ++i) {
    if (i > 0 && clearances_[i].id == clearances_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate clearance identity " + clearances_[i].id.str());
    }
  }
  for (std::size_t i = 0; i < expansion_zones_.size(); ++i) {
    if (i > 0 && expansion_zones_[i].id == expansion_zones_[i - 1].id) {
      return Status::failure(ErrorCode::duplicate_identity,
                             "duplicate expansion zone identity " + expansion_zones_[i].id.str());
    }
  }

  // ------------------------------------------------------------ node shapes
  for (const SpaceNode& node : nodes_) {
    const Status shape = validate_node_shape(node);
    if (!shape) return shape;
  }

  // ------------------------------------------------------------ containment
  for (const SpaceNode& node : nodes_) {
    if (node.parent.empty()) {
      if (!kind_may_be_root(node.kind)) {
        return Status::with_subject(ErrorCode::kind_must_have_parent,
                                    "a non-root node has no parent", node.id.str());
      }
      continue;
    }
    const SpaceNode* parent = find_node(node.parent);
    if (parent == nullptr) {
      return Status::with_subject(ErrorCode::lineage_broken,
                                  "the parent named by this node is not in the model",
                                  node.id.str());
    }
    const Status containment = validate_containment(*parent, node);
    if (!containment) {
      return Status(Error::with_subject(containment.code(), containment.error().message,
                                        node.id.str()));
    }
  }

  // ------------------------------------------------- structural scope shapes
  for (const OccupancyClaim& claim : claims_) {
    const Status shape = validate_claim_shape(claim);
    if (!shape) {
      return Status(Error::with_subject(shape.code(), shape.error().message, claim.id.str()));
    }
    if (find_node(claim.node) == nullptr) {
      return Status::with_subject(ErrorCode::not_found, "the claim names a node that is not in "
                                                        "the model",
                                  claim.id.str());
    }
  }
  for (const FootprintReservation& reservation : reservations_) {
    const Status shape = validate_reservation_shape(reservation);
    if (!shape) {
      return Status(Error::with_subject(shape.code(), shape.error().message,
                                        reservation.id.str()));
    }
    if (find_node(reservation.node) == nullptr) {
      return Status::with_subject(ErrorCode::not_found,
                                  "the reservation names a node that is not in the model",
                                  reservation.id.str());
    }
  }
  for (const ExclusionRegion& region : exclusions_) {
    const Status shape = validate_exclusion_shape(region);
    if (!shape) {
      return Status(Error::with_subject(shape.code(), shape.error().message, region.id.str()));
    }
    if (find_node(region.node) == nullptr) {
      return Status::with_subject(ErrorCode::not_found,
                                  "the exclusion names a node that is not in the model",
                                  region.id.str());
    }
  }
  for (const ClearanceConstraint& clearance : clearances_) {
    const Status shape = validate_clearance_shape(clearance);
    if (!shape) {
      return Status(Error::with_subject(shape.code(), shape.error().message, clearance.id.str()));
    }
    if (find_node(clearance.node) == nullptr) {
      return Status::with_subject(ErrorCode::not_found,
                                  "the clearance names a node that is not in the model",
                                  clearance.id.str());
    }
  }
  for (const ExpansionZone& zone : expansion_zones_) {
    const Status shape = validate_expansion_zone_shape(zone);
    if (!shape) {
      return Status(Error::with_subject(shape.code(), shape.error().message, zone.id.str()));
    }
    if (find_node(zone.node) == nullptr) {
      return Status::with_subject(ErrorCode::not_found,
                                  "the expansion zone names a node that is not in the model",
                                  zone.id.str());
    }
  }

  // ------------------------------------------------------- plane subdivision
  for (const SpaceNode& node : nodes_) {
    if (!node.own_planar.is_declared()) continue;
    const SpaceNodeId owner = plane_owner_of(node.parent);
    if (owner.empty() || owner == node.id) continue;
    const SpaceNode* parent_plane = find_node(owner);
    if (parent_plane == nullptr) {
      return Status::with_subject(ErrorCode::plane_mismatch,
                                  "the enclosing plane is not in the model", node.id.str());
    }
    if (!node.placement.has_base_rect) {
      return Status::with_subject(
          ErrorCode::invalid_placement,
          "a node that declares a plane inside another plane must state where it sits",
          node.id.str());
    }
    const Checked<SquareMillimeters> placement_area = node.placement.base_rect.area();
    if (!placement_area) {
      return Status::with_subject(ErrorCode::arithmetic_overflow,
                                  "the placement area overflowed", node.id.str());
    }
    if (placement_area.value != node.own_planar.declared_area) {
      return Status::with_subject(
          ErrorCode::plane_mismatch,
          "a subdivided plane must declare exactly the area of its placement: placement is " +
              to_text(placement_area.value) + " and declared is " +
              to_text(node.own_planar.declared_area),
          node.id.str());
    }
    if (!parent_plane->own_planar.rects.empty()) {
      const Checked<SquareMillimeters> covered =
          parent_plane->own_planar.rects.intersection_area(node.placement.base_rect);
      if (!covered) {
        return Status::with_subject(ErrorCode::arithmetic_overflow,
                                    "the containment measure overflowed", node.id.str());
      }
      if (covered.value != placement_area.value) {
        return Status::with_subject(
            ErrorCode::extent_out_of_envelope,
            "the placement is not entirely inside the declared rectangles of its plane",
            node.id.str());
      }
    }
    if (placement_area.value.value() > parent_plane->own_planar.declared_area.value()) {
      return Status::with_subject(ErrorCode::extent_out_of_envelope,
                                  "the placement is larger than the plane that contains it",
                                  node.id.str());
    }
  }

  // ------------------------------------------------- placement inside planes
  for (const SpaceNode& node : nodes_) {
    if (!node.placement.has_base_rect) continue;
    const SpaceNodeId owner = plane_owner_of(node.id);
    if (owner.empty() || owner == node.id) continue;
    const SpaceNode* plane = find_node(owner);
    if (plane == nullptr) continue;
    if (plane->own_planar.rects.empty()) continue;
    const Checked<SquareMillimeters> measured =
        plane->own_planar.rects.intersection_area(node.placement.base_rect);
    if (!measured) {
      return Status::with_subject(ErrorCode::arithmetic_overflow,
                                  "the containment measure overflowed", node.id.str());
    }
    const Checked<SquareMillimeters> placement_area = node.placement.base_rect.area();
    if (!placement_area) {
      return Status::with_subject(ErrorCode::arithmetic_overflow,
                                  "the placement area overflowed", node.id.str());
    }
    if (measured.value != placement_area.value) {
      return Status::with_subject(
          ErrorCode::extent_out_of_envelope,
          "the placement is not entirely inside the declared rectangles of its plane",
          node.id.str());
    }
  }

  // -------------------------------------------- scope compatibility and fit
  for (const OccupancyClaim& claim : claims_) {
    const SpaceNodeId plane = plane_owner_of(claim.node);
    const SpaceNodeId rack = rack_owner_of(claim.node);
    switch (claim.scope.kind) {
      case FootprintScopeKind::whole_node: {
        const SpaceNode* node = find_node(claim.node);
        if (node == nullptr) break;
        if (!node->own_planar.is_declared() && !node->own_rack.is_declared()) {
          return Status::with_subject(
              ErrorCode::envelope_not_declared,
              "a whole-node claim requires the node to declare an envelope", claim.id.str());
        }
        break;
      }
      case FootprintScopeKind::planar: {
        if (plane.empty()) {
          return Status::with_subject(
              ErrorCode::plane_not_declared,
              "a planar claim requires an enclosing node that declares a plane", claim.id.str());
        }
        const SpaceNode* owner = find_node(plane);
        if (owner != nullptr && !owner->own_planar.rects.empty()) {
          // The plane's rectangles are pairwise disjoint, so the measure of the
          // claim that lies inside the plane is the sum of the intersections.
          // Equality with the claim's own area proves full containment.
          std::int64_t covered_total = 0;
          for (const PlanarRect& claim_rect : claim.scope.rects.rects()) {
            for (const PlanarRect& plane_rect : owner->own_planar.rects.rects()) {
              const Checked<SquareMillimeters> piece =
                  rect_intersection_area(plane_rect, claim_rect);
              if (!piece) {
                return Status::with_subject(ErrorCode::arithmetic_overflow,
                                            "the containment measure overflowed", claim.id.str());
              }
              covered_total += piece.value.value();
            }
          }
          const Checked<SquareMillimeters> claimed_area = claim.scope.rects.total_area();
          if (!claimed_area) {
            return Status::with_subject(ErrorCode::arithmetic_overflow,
                                        "the claim area overflowed", claim.id.str());
          }
          if (covered_total != claimed_area.value.value()) {
            return Status::with_subject(
                ErrorCode::extent_out_of_envelope,
                "the claim is not entirely inside the declared rectangles of its plane",
                claim.id.str());
          }
        }
        break;
      }
      case FootprintScopeKind::rack_units: {
        if (rack.empty()) {
          return Status::with_subject(
              ErrorCode::unit_envelope_exceeded,
              "a rack unit claim requires an enclosing node that declares a rack envelope",
              claim.id.str());
        }
        const SpaceNode* owner = find_node(rack);
        if (owner != nullptr) {
          const RackUnitInterval envelope = owner->own_rack.full_span();
          for (const RackUnitInterval& interval : claim.scope.units.intervals()) {
            if (!interval_contains(envelope, interval)) {
              return Status::with_subject(
                  ErrorCode::unit_envelope_exceeded,
                  "the claim covers " + to_text(interval) +
                      " which is outside the rack envelope " + to_text(envelope),
                  claim.id.str());
            }
          }
        }
        break;
      }
    }
  }

  // ------------------------------------- no two authority records overlap
  {
    std::map<SpaceNodeId, std::vector<PlanarRect>> plane_rects;
    std::map<SpaceNodeId, std::vector<RackUnitInterval>> rack_units;

    for (const SpaceNode& node : nodes_) {
      if (!node.placement.has_base_rect) continue;
      const SpaceNodeId owner = plane_owner_of(node.id);
      if (owner.empty() || owner == node.id) continue;
      plane_rects[owner].push_back(node.placement.base_rect);
    }
    for (const OccupancyClaim& claim : claims_) {
      if (!claim_state_consumes(claim.state)) continue;
      append_consuming_rects(claim.scope, plane_rects[plane_owner_of(claim.node)]);
      if (claim.scope.kind == FootprintScopeKind::rack_units) {
        const std::vector<RackUnitInterval>& source = claim.scope.units.intervals();
        rack_units[rack_owner_of(claim.node)].insert(rack_units[rack_owner_of(claim.node)].end(),
                                                     source.begin(), source.end());
      }
    }
    for (const FootprintReservation& reservation : reservations_) {
      if (!reservation_state_holds(reservation.state)) continue;
      append_consuming_rects(reservation.scope, plane_rects[plane_owner_of(reservation.node)]);
      if (reservation.scope.kind == FootprintScopeKind::rack_units) {
        const SpaceNodeId owner = rack_owner_of(reservation.node);
        const std::vector<RackUnitInterval>& source = reservation.scope.units.intervals();
        rack_units[owner].insert(rack_units[owner].end(), source.begin(), source.end());
      }
    }
    for (const ExpansionZone& zone : expansion_zones_) {
      if (!zone.earmarks()) continue;
      append_consuming_rects(zone.scope, plane_rects[plane_owner_of(zone.node)]);
    }

    for (const auto& entry : plane_rects) {
      if (entry.first.empty()) continue;
      std::size_t first = 0;
      std::size_t second = 0;
      if (internal::find_overlapping_pair(entry.second, first, second)) {
        return Status::with_subject(
            ErrorCode::overlap,
            "two consuming records on the plane of this node cover overlapping area (" +
                to_text(entry.second[first]) + " and " + to_text(entry.second[second]) + ")",
            entry.first.str());
      }
    }
    for (const auto& entry : rack_units) {
      if (entry.first.empty()) continue;
      Result<IntervalSet> merged = IntervalSet::build(entry.second);
      if (!merged) {
        return Status::with_subject(ErrorCode::invalid_extent,
                                    "a rack unit scope could not be normalized", entry.first.str());
      }
      const Checked<RackUnits> total = merged.value().total();
      if (!total) {
        return Status::with_subject(ErrorCode::arithmetic_overflow,
                                    "the rack unit total overflowed", entry.first.str());
      }
      std::int32_t raw = 0;
      for (const RackUnitInterval& interval : entry.second) {
        raw += interval.count();
      }
      if (raw != total.value.value()) {
        return Status::with_subject(
            ErrorCode::overlap,
            "two consuming records on this rack envelope cover overlapping rack units",
            entry.first.str());
      }
    }
  }

  // ---------------------------------------------------- whole-node uniqueness
  {
    std::map<SpaceNodeId, std::uint32_t> whole_node_counts;
    for (const OccupancyClaim& claim : claims_) {
      if (!claim_state_consumes(claim.state)) continue;
      if (!claim.scope.is_whole_node()) continue;
      const SpaceNodeId owner =
          plane_owner_of(claim.node).empty() ? rack_owner_of(claim.node) : plane_owner_of(claim.node);
      if (whole_node_counts[owner]++ > 0) {
        return Status::with_subject(ErrorCode::overlap,
                                    "more than one whole-node claim covers this envelope",
                                    claim.id.str());
      }
    }
  }

  // -------------------------------------------------------------- ledgers
  //
  // Every ledger identity is re-derived from the measures and compared, so a
  // state that reached the model by any path other than the mutation API is
  // still refused if its arithmetic does not hold together.
  const FitContext context{};
  const internal::ModelMeasure model = internal::measure_model(*this, context);
  for (const auto& entry : model.planes) {
    const internal::PlaneMeasure& measure = entry.second;
    if (measure.overflowed) continue;
    if (!measure.usable && !measure.planned) {
      if (!measure.available.is_zero() || !measure.usable_area.is_zero()) {
        return Status::with_subject(
            ErrorCode::invariant_violation,
            "an out-of-service plane reports usable or available space", entry.first.str());
      }
      continue;
    }
    if (measure.planned) {
      if (!measure.available.is_zero() || !measure.usable_area.is_zero()) {
        return Status::with_subject(
            ErrorCode::invariant_violation,
            "a planned plane reports usable or available space", entry.first.str());
      }
      continue;
    }
    const std::int64_t usable =
        saturating_sub(measure.declared.value(), measure.excluded.value());
    if (measure.usable_area.value() != usable) {
      return Status::with_subject(
          ErrorCode::invariant_violation,
          "the plane ledger does not reconstruct: usable is " + to_text(measure.usable_area) +
              " but declared less excluded is " + to_text(SquareMillimeters{usable}),
          entry.first.str());
    }
    if (measure.excluded.value() > measure.declared.value()) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "the excluded area exceeds the declared area",
                                  entry.first.str());
    }
    if (measure.available.value() > measure.usable_area.value()) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "the available area exceeds the usable area",
                                  entry.first.str());
    }
    if (measure.over_committed && !measure.available.is_zero()) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "an over-committed plane reports available space",
                                  entry.first.str());
    }
  }
  for (const auto& entry : model.racks) {
    const internal::RackMeasure& measure = entry.second;
    if (measure.overflowed) continue;
    if (!measure.usable && !measure.planned) {
      if (measure.available.value() != 0 || measure.usable_units.value() != 0) {
        return Status::with_subject(
            ErrorCode::invariant_violation,
            "an out-of-service rack envelope reports usable or available units",
            entry.first.str());
      }
      continue;
    }
    if (measure.planned) {
      if (measure.available.value() != 0 || measure.usable_units.value() != 0) {
        return Status::with_subject(ErrorCode::invariant_violation,
                                    "a planned rack envelope reports usable or available units",
                                    entry.first.str());
      }
      continue;
    }
    const std::int32_t usable =
        saturating_sub(measure.declared.value(), measure.excluded.value());
    if (measure.usable_units.value() != usable) {
      return Status::with_subject(
          ErrorCode::invariant_violation,
          "the rack ledger does not reconstruct: usable is " + to_text(measure.usable_units) +
              " but declared less excluded is " +
              to_text(RackUnits{static_cast<std::int32_t>(usable)}),
          entry.first.str());
    }
    if (measure.available.value() > measure.usable_units.value()) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "the available units exceed the usable units",
                                  entry.first.str());
    }
    if (measure.excluded.value() > measure.declared.value()) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "the excluded units exceed the declared envelope",
                                  entry.first.str());
    }
    if (measure.available.value() > 0 && measure.free_runs == 0) {
      return Status::with_subject(ErrorCode::invariant_violation,
                                  "a rack envelope reports free units but no free run",
                                  entry.first.str());
    }
  }

  // ------------------------------------------------------ explanation guard
  const CapacityRollup whole = model_rollup(*this, context);
  if (whole.area.available.value() > whole.area.declared.value()) {
    return Status::failure(ErrorCode::invariant_violation,
                           "the whole-model available area exceeds the declared area");
  }
  return Status::success();
}

}  // namespace dccp::space_capacity
