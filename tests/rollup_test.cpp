// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the subtree rollup cannot double count.
//
// Proves that a plane contributes its area exactly once however deep the
// containment goes, that the rollup over the whole model equals the rollup over
// the model's only root, that a rack declares no plane and reports a zero that
// is a measurement rather than an absence, and that the per-plane ledger the
// rollup sums is the same ledger an independent reconstruction of every family
// - exclusions, enforceable clearance bands, placements, committed claims, held
// reservations and earmarking zones - produces from the raw records.
//
// The reference model here walks the containment chain itself, resolves plane
// owners itself, and aggregates every field itself. It agrees with the
// production rollup for EVERY node in the model, which is what makes the
// no-double-count claim a check rather than an assertion.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "fixture.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;
using sc_fixture::Ids;

constexpr std::int64_t kBuildingDeclared = 2'400'000'000;  // 60000 x 40000
constexpr std::int64_t kHallDeclared = 600'000'000;        // 30000 x 20000
constexpr std::int64_t kRackFootprint = 720'000;           // 600 x 1200
constexpr std::int64_t kHallStructural = 4 * kRackFootprint;  // 2'880'000

// ---------------------------------------------------------------------------
// Independent containment helpers. None of these calls Snapshot::plane_owner_of
// or Snapshot::subtree_of: the reference walks the parent links itself, so it
// cannot inherit a mistake from the code it is checking.
// ---------------------------------------------------------------------------

SpaceNodeId parent_of(const Snapshot& snapshot, const SpaceNodeId& node) {
  const SpaceNode* record = snapshot.find_node(node);
  if (record == nullptr) return SpaceNodeId{};
  return record->parent;
}

// The nearest strictly-enclosing ancestor that declares a plane.
SpaceNodeId ancestor_plane(const Snapshot& snapshot, const SpaceNodeId& node) {
  SpaceNodeId cursor = parent_of(snapshot, node);
  std::uint32_t guard = 0;
  while (!cursor.empty()) {
    const SpaceNode* record = snapshot.find_node(cursor);
    if (record == nullptr) return SpaceNodeId{};
    if (record->own_planar.is_declared()) return record->id;
    if (++guard > 32u) return SpaceNodeId{};
    cursor = record->parent;
  }
  return SpaceNodeId{};
}

// The nearest ancestor-or-self that declares a plane, which is where a record's
// planar area is accounted.
SpaceNodeId plane_owner_by_walk(const Snapshot& snapshot, const SpaceNodeId& node) {
  const SpaceNode* record = snapshot.find_node(node);
  if (record == nullptr) return SpaceNodeId{};
  if (record->own_planar.is_declared()) return record->id;
  return ancestor_plane(snapshot, node);
}

SpaceNodeId rack_owner_by_walk(const Snapshot& snapshot, const SpaceNodeId& node) {
  const SpaceNode* record = snapshot.find_node(node);
  if (record == nullptr) return SpaceNodeId{};
  if (record->own_rack.is_declared() && kind_may_declare_rack_envelope(record->kind)) {
    return record->id;
  }
  SpaceNodeId cursor = record->parent;
  std::uint32_t guard = 0;
  while (!cursor.empty()) {
    const SpaceNode* ancestor = snapshot.find_node(cursor);
    if (ancestor == nullptr) return SpaceNodeId{};
    if (ancestor->own_rack.is_declared() && kind_may_declare_rack_envelope(ancestor->kind)) {
      return ancestor->id;
    }
    if (++guard > 32u) return SpaceNodeId{};
    cursor = ancestor->parent;
  }
  return SpaceNodeId{};
}

// Every node of the subtree rooted at `root`, including the root, collected by
// walking the child links.
std::vector<SpaceNodeId> subtree_by_walk(const Snapshot& snapshot, const SpaceNodeId& root) {
  std::vector<SpaceNodeId> members;
  std::vector<SpaceNodeId> pending{root};
  while (!pending.empty()) {
    const SpaceNodeId current = pending.back();
    pending.pop_back();
    if (snapshot.find_node(current) == nullptr) continue;
    members.push_back(current);
    for (const SpaceNode& candidate : snapshot.nodes()) {
      if (candidate.parent == current) pending.push_back(candidate.id);
    }
  }
  return members;
}

// True when no strict ancestor of `node` that is still inside the subtree rooted
// at `root` declares a plane: the node's declared area is then the topmost one
// in that subtree and is the area that physically exists there.
bool is_topmost_plane_owner(const Snapshot& snapshot, const SpaceNodeId& root,
                            const SpaceNodeId& node) {
  if (node == root) return true;  // nothing above the subtree root is in the subtree
  SpaceNodeId cursor = parent_of(snapshot, node);
  while (!cursor.empty()) {
    const SpaceNode* record = snapshot.find_node(cursor);
    if (record == nullptr) return true;
    if (record->own_planar.is_declared()) return false;
    if (cursor == root) return true;  // the subtree root declares no plane
    cursor = record->parent;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Independent per-plane ledger reconstruction from the raw record families.
// ---------------------------------------------------------------------------

struct PlaneLedger final {
  std::int64_t declared = 0;
  std::int64_t excluded = 0;
  std::int64_t usable = 0;
  std::int64_t structural = 0;
  std::int64_t claimed = 0;
  std::int64_t held = 0;
  std::int64_t earmarked = 0;
  std::int64_t blocked = 0;
  std::int64_t available = 0;
};

std::int64_t area_of(const std::vector<PlanarRect>& rects) {
  const Checked<SquareMillimeters> measured = rect_union_area(rects);
  return measured.ok() ? measured.value.value() : -1;
}

bool consumes(const OccupancyClaim& claim) { return claim_state_consumes(claim.state); }

// Rebuilds one plane's ledger from the records themselves.
PlaneLedger reconstruct_plane(const Snapshot& snapshot, const SpaceNodeId& plane) {
  const SpaceNode* owner = snapshot.find_node(plane);
  PlaneLedger ledger;
  if (owner == nullptr) return ledger;

  std::vector<PlanarRect> excluded;
  std::vector<PlanarRect> structural;
  std::vector<PlanarRect> subplanes;
  std::vector<PlanarRect> claimed;
  std::vector<PlanarRect> held;
  std::vector<PlanarRect> earmarked;

  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.placement.has_base_rect) continue;
    if (node.id == plane) continue;  // the plane's own placement sits in its parent
    if (plane_owner_by_walk(snapshot, node.id) != plane &&
        ancestor_plane(snapshot, node.id) != plane) {
      continue;
    }
    if (node.own_planar.is_declared()) {
      // A nested plane is a subdivision: its placement is removed from the
      // enclosing plane's declared area instead of being counted as consumed.
      if (ancestor_plane(snapshot, node.id) == plane) subplanes.push_back(node.placement.base_rect);
    } else if (plane_owner_by_walk(snapshot, node.id) == plane) {
      structural.push_back(node.placement.base_rect);
    }
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now() || region.blocks.empty()) continue;
    if (plane_owner_by_walk(snapshot, region.node) != plane) continue;
    if (region.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : region.scope.rects.rects()) excluded.push_back(rect);
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.has_band || !clearance.enforceable) continue;
    if (plane_owner_by_walk(snapshot, clearance.node) != plane) continue;
    excluded.push_back(clearance.band);
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    if (!consumes(claim)) continue;
    if (plane_owner_by_walk(snapshot, claim.node) != plane) continue;
    if (claim.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : claim.scope.rects.rects()) claimed.push_back(rect);
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (plane_owner_by_walk(snapshot, reservation.node) != plane) continue;
    if (reservation.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : reservation.scope.rects.rects()) held.push_back(rect);
  }
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    if (plane_owner_by_walk(snapshot, zone.node) != plane) continue;
    if (zone.scope.kind != FootprintScopeKind::planar) continue;
    for (const PlanarRect& rect : zone.scope.rects.rects()) earmarked.push_back(rect);
  }

  const std::int64_t own_declared = owner->own_planar.declared_area.value();
  const std::int64_t subplane_area = area_of(subplanes);
  ledger.declared = own_declared > subplane_area ? own_declared - subplane_area : 0;
  ledger.excluded = area_of(excluded);
  if (ledger.excluded > ledger.declared) ledger.excluded = ledger.declared;
  ledger.usable = ledger.declared - ledger.excluded;
  ledger.structural = area_of(structural);
  ledger.claimed = area_of(claimed);
  ledger.held = area_of(held);
  ledger.earmarked = area_of(earmarked);

  std::vector<PlanarRect> blockers = excluded;
  blockers.insert(blockers.end(), structural.begin(), structural.end());
  blockers.insert(blockers.end(), claimed.begin(), claimed.end());
  blockers.insert(blockers.end(), held.begin(), held.end());
  blockers.insert(blockers.end(), earmarked.begin(), earmarked.end());
  ledger.blocked = area_of(blockers);
  const std::int64_t clamped = ledger.blocked < ledger.declared ? ledger.blocked : ledger.declared;
  ledger.available = ledger.declared - clamped;
  return ledger;
}

struct RackLedger final {
  std::int64_t declared = 0;
  std::int64_t excluded = 0;
  std::int64_t usable = 0;
  std::int64_t occupied = 0;
  std::int64_t available = 0;
  std::uint32_t free_runs = 0;
  std::int64_t largest_free_run = 0;
  std::uint32_t fragmentation_ppm = 0;
};

RackLedger reconstruct_rack(const Snapshot& snapshot, const SpaceNodeId& rack) {
  RackLedger ledger;
  const SpaceNode* owner = snapshot.find_node(rack);
  if (owner == nullptr) return ledger;
  ledger.declared = owner->own_rack.height.value();

  std::vector<RackUnitInterval> excluded;
  std::vector<RackUnitInterval> occupied;
  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.placement.has_u_span) continue;
    if (node.id == rack) continue;
    if (rack_owner_by_walk(snapshot, node.id) != rack) continue;
    occupied.push_back(node.placement.u_span);
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    if (!consumes(claim)) continue;
    if (rack_owner_by_walk(snapshot, claim.node) != rack) continue;
    if (claim.scope.kind != FootprintScopeKind::rack_units) continue;
    for (const RackUnitInterval& span : claim.scope.units.intervals()) occupied.push_back(span);
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (!reservation_state_holds(reservation.state)) continue;
    if (rack_owner_by_walk(snapshot, reservation.node) != rack) continue;
    if (reservation.scope.kind != FootprintScopeKind::rack_units) continue;
    for (const RackUnitInterval& span : reservation.scope.units.intervals()) {
      occupied.push_back(span);
    }
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now() || region.blocks.empty()) continue;
    if (rack_owner_by_walk(snapshot, region.node) != rack) continue;
    if (region.scope.kind != FootprintScopeKind::rack_units) continue;
    for (const RackUnitInterval& span : region.scope.units.intervals()) excluded.push_back(span);
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.has_units || !clearance.enforceable) continue;
    if (rack_owner_by_walk(snapshot, clearance.node) != rack) continue;
    for (const RackUnitInterval& span : clearance.required_free_units.intervals()) {
      excluded.push_back(span);
    }
  }

  const IntervalSet excluded_set = IntervalSet::build(excluded).value();
  const IntervalSet occupied_set = IntervalSet::build(occupied).value();
  ledger.excluded = excluded_set.total().value.value();
  ledger.occupied = occupied_set.total().value.value();
  ledger.usable = ledger.declared - ledger.excluded;

  std::vector<RackUnitInterval> blocked;
  blocked.insert(blocked.end(), excluded.begin(), excluded.end());
  blocked.insert(blocked.end(), occupied.begin(), occupied.end());
  const IntervalSet blocked_set = IntervalSet::build(blocked).value();
  ledger.available = ledger.declared - blocked_set.total().value.value();
  const IntervalSet::FreeRunStats stats =
      IntervalSet::free_run_stats(blocked_set, static_cast<std::int32_t>(ledger.declared)).value();
  ledger.free_runs = stats.run_count;
  ledger.largest_free_run = stats.largest;
  if (ledger.available > 0) {
    ledger.fragmentation_ppm = static_cast<std::uint32_t>(
        (ledger.available - ledger.largest_free_run) * 1'000'000 / ledger.available);
  }
  return ledger;
}

// ---------------------------------------------------------------------------
// Independent subtree aggregation.
// ---------------------------------------------------------------------------

struct RollupReference final {
  std::uint64_t node_count = 0;
  std::uint64_t plane_owner_count = 0;
  std::uint64_t rack_count = 0;
  std::uint64_t claim_count = 0;
  std::uint64_t reservation_count = 0;
  std::uint64_t exclusion_count = 0;
  std::uint64_t clearance_count = 0;
  std::uint64_t expansion_zone_count = 0;

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

  std::int64_t topmost_declared = 0;

  std::int64_t unit_declared = 0;
  std::int64_t unit_excluded = 0;
  std::int64_t unit_usable = 0;
  std::int64_t unit_occupied = 0;
  std::int64_t unit_available = 0;
  std::int64_t unit_pending = 0;
  std::int64_t unit_planned = 0;
  std::uint32_t unit_free_runs = 0;
  std::int64_t unit_largest_free_run = 0;
  std::uint32_t unit_fragmentation_ppm = 0;

  bool contains_undeclared_envelopes = false;
  bool contains_inactive_nodes = false;
  bool over_committed = false;
};

RollupReference reference_rollup(const Snapshot& snapshot, const SpaceNodeId& root) {
  const FitContext context{};
  const std::vector<SpaceNodeId> members = subtree_by_walk(snapshot, root);
  RollupReference reference;
  reference.node_count = members.size();

  for (const SpaceNodeId& member : members) {
    const SpaceNode* record = snapshot.find_node(member);
    if (record == nullptr) continue;
    if (!lifecycle_is_usable_now(record->lifecycle) && !lifecycle_is_planned(record->lifecycle)) {
      reference.contains_inactive_nodes = true;
    }
    if (kind_may_declare_plane(record->kind) && !record->own_planar.is_declared()) {
      reference.contains_undeclared_envelopes = true;
    }

    if (record->own_planar.is_declared()) {
      ++reference.plane_owner_count;
      const AreaLedger area = snapshot.capacity_of(member, context).area;
      // The rollup sums EVERY plane owner's own-level ledger: a nested plane's
      // area has already been removed from the enclosing plane's declared area,
      // so this is a sum of disjoint areas and not a double count.
      reference.declared += area.declared.value();
      reference.excluded += area.excluded.value();
      reference.usable += area.usable.value();
      reference.structural += area.structural.value();
      reference.claimed += area.claimed.value();
      reference.held += area.held.value();
      reference.earmarked += area.earmarked.value();
      reference.available += area.available.value();
      reference.pending += area.pending.value();
      reference.planned += area.planned.value();
      reference.over_committed = reference.over_committed || area.over_committed;
      // The independent count: only the topmost plane owners of the subtree
      // declare area that physically exists, and their raw declared areas must
      // add up to exactly the same number.
      if (is_topmost_plane_owner(snapshot, root, member)) {
        reference.topmost_declared += record->own_planar.declared_area.value();
      }
    }
    if (record->own_rack.is_declared() && kind_may_declare_rack_envelope(record->kind)) {
      ++reference.rack_count;
      const UnitLedger units = snapshot.capacity_of(member, context).units;
      reference.unit_declared += units.declared.value();
      reference.unit_excluded += units.excluded.value();
      reference.unit_usable += units.usable.value();
      reference.unit_occupied += units.occupied.value();
      reference.unit_available += units.available.value();
      reference.unit_pending += units.pending.value();
      reference.unit_planned += units.planned.value();
      reference.unit_free_runs += units.free_runs;
      if (units.largest_free_run.value() > reference.unit_largest_free_run) {
        reference.unit_largest_free_run = units.largest_free_run.value();
      }
      reference.over_committed = reference.over_committed || units.over_committed;
    }
  }

  if (reference.unit_available > 0) {
    reference.unit_fragmentation_ppm = static_cast<std::uint32_t>(
        (reference.unit_available - reference.unit_largest_free_run) * 1'000'000 /
        reference.unit_available);
  }

  const auto in_subtree = [&members](const SpaceNodeId& node) {
    for (const SpaceNodeId& member : members) {
      if (member == node) return true;
    }
    return false;
  };
  for (const OccupancyClaim& claim : snapshot.claims()) {
    if (in_subtree(claim.node)) ++reference.claim_count;
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    if (in_subtree(reservation.node)) ++reference.reservation_count;
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    if (in_subtree(region.node)) ++reference.exclusion_count;
  }
  for (const ClearanceConstraint& clearance : snapshot.clearances()) {
    if (in_subtree(clearance.node)) ++reference.clearance_count;
  }
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    if (in_subtree(zone.node)) ++reference.expansion_zone_count;
  }
  return reference;
}

}  // namespace

int main() {
  SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
  const Ids id = sc_fixture::ids();
  const FitContext plain{};
  const SnapshotPtr snapshot = registry.snapshot();

  SC_CASE("a subtree with one plane reports that plane exactly once");
  {
    const Result<CapacityRollup> rolled = registry.rollup(id.hall, plain);
    SC_CHECK(rolled.ok());
    SC_CHECK_EQ(rolled.value().root.str(), std::string("hall-1"));
    SC_CHECK_EQ(rolled.value().revision.value(), registry.revision().value());
    SC_CHECK_EQ(rolled.value().area.declared.value(), kHallDeclared);
    SC_CHECK_EQ(rolled.value().area.declared.value(), 600'000'000);
    SC_CHECK_EQ(rolled.value().area.structural.value(), kHallStructural);
    SC_CHECK_EQ(rolled.value().plane_owner_count, 1ull);
    SC_CHECK_EQ(rolled.value().rack_count, 4ull);
  }

  SC_CASE("a nested plane is a subdivision and is not added a second time");
  {
    const Result<CapacityRollup> building = registry.rollup(id.building, plain);
    SC_CHECK(building.ok());

    // bldg-1 declares 2,400,000,000 and contains hall-1, which declares
    // 600,000,000 as a subdivision of it. The rollup sums the two own-level
    // ledgers: 1,800,000,000 + 600,000,000, which is the building's own plane
    // and not the 3,000,000,000 a naive sum of raw declared areas would give.
    SC_CHECK_EQ(building.value().area.declared.value(), kBuildingDeclared);
    SC_CHECK_EQ(building.value().area.declared.value(), 2'400'000'000);
    SC_CHECK(building.value().area.declared.value() != 3'000'000'000);

    // The enclosing plane removes the subdivision from its own declared area,
    // so its own ledger is the complementary part.
    const AreaLedger building_own = snapshot->capacity_of(id.building, plain).area;
    SC_CHECK_EQ(building_own.declared.value(), kBuildingDeclared - kHallDeclared);
    SC_CHECK_EQ(building_own.declared.value(), 1'800'000'000);
    SC_CHECK_EQ(building_own.structural.value(), 0);
    SC_CHECK_EQ(building_own.available.value(), 1'800'000'000);
    SC_CHECK_EQ(building.value().area.declared.value(),
                building_own.declared.value() +
                    snapshot->capacity_of(id.hall, plain).area.declared.value());

    SC_CHECK_EQ(building.value().plane_owner_count, 2ull);
    SC_CHECK_EQ(building.value().area.excluded.value(), 25'540'000);
    SC_CHECK_EQ(building.value().area.available.value(), 2'366'580'000);
  }

  SC_CASE("the whole model equals the rollup of its only root");
  {
    const Result<CapacityRollup> root = registry.rollup(id.site, plain);
    SC_CHECK(root.ok());
    SC_CHECK_EQ(root.value().area.declared.value(), kBuildingDeclared);
    SC_CHECK_EQ(root.value().plane_owner_count, 2ull);

    const CapacityRollup whole = registry.model_rollup(plain);
    // Field by field, apart from the two fields a whole-model rollup has no
    // value for: it has no root identity and therefore no root generation.
    SC_CHECK_EQ(whole.node_count, root.value().node_count);
    SC_CHECK_EQ(whole.plane_owner_count, root.value().plane_owner_count);
    SC_CHECK_EQ(whole.rack_count, root.value().rack_count);
    SC_CHECK_EQ(whole.claim_count, root.value().claim_count);
    SC_CHECK_EQ(whole.reservation_count, root.value().reservation_count);
    SC_CHECK_EQ(whole.exclusion_count, root.value().exclusion_count);
    SC_CHECK_EQ(whole.clearance_count, root.value().clearance_count);
    SC_CHECK_EQ(whole.expansion_zone_count, root.value().expansion_zone_count);
    SC_CHECK(whole.area == root.value().area);
    SC_CHECK(whole.units == root.value().units);
    SC_CHECK_EQ(whole.contains_undeclared_envelopes, root.value().contains_undeclared_envelopes);
    SC_CHECK_EQ(whole.contains_inactive_nodes, root.value().contains_inactive_nodes);
    SC_CHECK_EQ(whole.over_committed, root.value().over_committed);
    SC_CHECK_EQ(whole.revision.value(), root.value().revision.value());
    SC_CHECK(whole.root.empty());
    SC_CHECK(whole.root_generation.is_zero());
  }

  SC_CASE("a rack owns an envelope, no plane, and reports a measured zero area");
  {
    const Result<CapacityRollup> rolled = registry.rollup(id.rack1, plain);
    SC_CHECK(rolled.ok());
    SC_CHECK_EQ(rolled.value().rack_count, 1ull);
    SC_CHECK_EQ(rolled.value().plane_owner_count, 0ull);
    SC_CHECK_EQ(rolled.value().node_count, 2ull);  // rack-01 and band-a

    // No node in this subtree declares a plane, so the declared AREA is zero
    // and it is a measurement: the subtree really offers no floor.
    SC_CHECK_EQ(rolled.value().area.declared.value(), 0);
    SC_CHECK_EQ(rolled.value().area.available.value(), 0);
    SC_CHECK(rolled.value().area.state == MeasureState::known);
    SC_CHECK(!rolled.value().contains_undeclared_envelopes);

    SC_CHECK_EQ(rolled.value().units.declared.value(), 42);
    SC_CHECK_EQ(rolled.value().units.occupied.value(), 10);
    SC_CHECK_EQ(rolled.value().units.available.value(), 32);
    SC_CHECK_EQ(rolled.value().units.free_runs, 2u);
    SC_CHECK_EQ(rolled.value().units.largest_free_run.value(), 27);
  }

  SC_CASE("a row aggregates its racks and still owns no plane");
  {
    const Result<CapacityRollup> rolled = registry.rollup(id.row1, plain);
    SC_CHECK(rolled.ok());
    SC_CHECK_EQ(rolled.value().rack_count, 2ull);  // rack-01 and rack-02
    SC_CHECK_EQ(rolled.value().units.declared.value(), 84);
    SC_CHECK_EQ(rolled.value().units.occupied.value(), 18);  // 10 units plus an 8 unit hold
    SC_CHECK_EQ(rolled.value().units.available.value(), 66);
    SC_CHECK_EQ(rolled.value().area.declared.value(), 0);

    // A row MAY declare a plane and does not, so the zero above is flagged as
    // an absent envelope rather than being read as a measured zero area.
    SC_CHECK(rolled.value().contains_undeclared_envelopes);
    SC_CHECK_EQ(rolled.value().plane_owner_count, 0ull);
    SC_CHECK_EQ(rolled.value().node_count, 4ull);  // row-1, rack-01, rack-02, band-a
  }

  SC_CASE("the independent reference agrees with the rollup for every node");
  {
    for (const SpaceNode& node : snapshot->nodes()) {
      const Result<CapacityRollup> rolled = registry.rollup(node.id, plain);
      SC_CHECK(rolled.ok());
      if (!rolled.ok()) continue;
      const RollupReference reference = reference_rollup(*snapshot, node.id);
      const CapacityRollup& actual = rolled.value();

      SC_CHECK_EQ(actual.node_count, reference.node_count);
      SC_CHECK_EQ(actual.plane_owner_count, reference.plane_owner_count);
      SC_CHECK_EQ(actual.rack_count, reference.rack_count);
      SC_CHECK_EQ(actual.claim_count, reference.claim_count);
      SC_CHECK_EQ(actual.reservation_count, reference.reservation_count);
      SC_CHECK_EQ(actual.exclusion_count, reference.exclusion_count);
      SC_CHECK_EQ(actual.clearance_count, reference.clearance_count);
      SC_CHECK_EQ(actual.expansion_zone_count, reference.expansion_zone_count);

      SC_CHECK_EQ(actual.area.declared.value(), reference.declared);
      SC_CHECK_EQ(actual.area.excluded.value(), reference.excluded);
      SC_CHECK_EQ(actual.area.usable.value(), reference.usable);
      SC_CHECK_EQ(actual.area.structural.value(), reference.structural);
      SC_CHECK_EQ(actual.area.claimed.value(), reference.claimed);
      SC_CHECK_EQ(actual.area.held.value(), reference.held);
      SC_CHECK_EQ(actual.area.earmarked.value(), reference.earmarked);
      SC_CHECK_EQ(actual.area.available.value(), reference.available);
      SC_CHECK_EQ(actual.area.pending.value(), reference.pending);
      SC_CHECK_EQ(actual.area.planned.value(), reference.planned);

      SC_CHECK_EQ(actual.units.declared.value(), reference.unit_declared);
      SC_CHECK_EQ(actual.units.excluded.value(), reference.unit_excluded);
      SC_CHECK_EQ(actual.units.usable.value(), reference.unit_usable);
      SC_CHECK_EQ(actual.units.occupied.value(), reference.unit_occupied);
      SC_CHECK_EQ(actual.units.available.value(), reference.unit_available);
      SC_CHECK_EQ(actual.units.pending.value(), reference.unit_pending);
      SC_CHECK_EQ(actual.units.planned.value(), reference.unit_planned);
      SC_CHECK_EQ(actual.units.free_runs, reference.unit_free_runs);
      SC_CHECK_EQ(actual.units.largest_free_run.value(), reference.unit_largest_free_run);
      SC_CHECK_EQ(actual.units.fragmentation_ppm, reference.unit_fragmentation_ppm);

      SC_CHECK_EQ(actual.contains_undeclared_envelopes, reference.contains_undeclared_envelopes);
      SC_CHECK_EQ(actual.contains_inactive_nodes, reference.contains_inactive_nodes);
      SC_CHECK_EQ(actual.over_committed, reference.over_committed);

      // And the independent anti-double-count identity: summing the raw
      // declared areas of the TOPMOST plane owners, found by walking the parent
      // chain, gives exactly the number the rollup reports.
      SC_CHECK_EQ(actual.area.declared.value(), reference.topmost_declared);
    }
  }

  SC_CASE("every plane owner's ledger reconstructs from the raw records");
  {
    std::int64_t plane_declared_sum = 0;
    std::int64_t plane_available_sum = 0;
    for (const SpaceNode& node : snapshot->nodes()) {
      const NodeCapacity capacity = snapshot->capacity_of(node.id, plain);

      if (capacity.owns_plane) {
        const PlaneLedger reference = reconstruct_plane(*snapshot, node.id);
        SC_CHECK_EQ(capacity.area.declared.value(), reference.declared);
        SC_CHECK_EQ(capacity.area.excluded.value(), reference.excluded);
        SC_CHECK_EQ(capacity.area.usable.value(), reference.usable);
        SC_CHECK_EQ(capacity.area.structural.value(), reference.structural);
        SC_CHECK_EQ(capacity.area.claimed.value(), reference.claimed);
        SC_CHECK_EQ(capacity.area.held.value(), reference.held);
        SC_CHECK_EQ(capacity.area.earmarked.value(), reference.earmarked);
        SC_CHECK_EQ(capacity.area.available.value(), reference.available);

        // available <= usable <= declared, and the area that is left plus the
        // union of everything that blocks it is exactly what was declared.
        SC_CHECK(capacity.area.available.value() <= capacity.area.usable.value());
        SC_CHECK(capacity.area.usable.value() <= capacity.area.declared.value());
        SC_CHECK_EQ(capacity.area.available.value() + reference.blocked,
                    capacity.area.declared.value());
        SC_CHECK(!capacity.area.over_committed);

        plane_declared_sum += capacity.area.declared.value();
        plane_available_sum += capacity.area.available.value();
      }

      if (capacity.owns_rack_envelope) {
        const RackLedger reference = reconstruct_rack(*snapshot, node.id);
        SC_CHECK_EQ(capacity.units.declared.value(), reference.declared);
        SC_CHECK_EQ(capacity.units.excluded.value(), reference.excluded);
        SC_CHECK_EQ(capacity.units.usable.value(), reference.usable);
        SC_CHECK_EQ(capacity.units.occupied.value(), reference.occupied);
        SC_CHECK_EQ(capacity.units.available.value(), reference.available);
        SC_CHECK_EQ(capacity.units.free_runs, reference.free_runs);
        SC_CHECK_EQ(capacity.units.largest_free_run.value(), reference.largest_free_run);
        SC_CHECK_EQ(capacity.units.fragmentation_ppm, reference.fragmentation_ppm);

        SC_CHECK(capacity.units.available.value() <= capacity.units.usable.value());
        SC_CHECK(capacity.units.usable.value() <= capacity.units.declared.value());
        SC_CHECK_EQ(capacity.units.excluded.value() + capacity.units.occupied.value() +
                        capacity.units.available.value(),
                    capacity.units.declared.value());
      }
    }

    // Summing the plane owners' ledgers IS the whole-model rollup.
    const CapacityRollup whole = registry.model_rollup(plain);
    SC_CHECK_EQ(plane_declared_sum, whole.area.declared.value());
    SC_CHECK_EQ(plane_available_sum, whole.area.available.value());
    SC_CHECK_EQ(whole.area.declared.value(), kBuildingDeclared);
  }

  SC_CASE("report lists every plane owner of a subtree once, in canonical order");
  {
    const Result<CapacityReport> report = registry.report(id.hall, plain);
    SC_CHECK(report.ok());

    // hall-1 owns the plane; the four racks own the vertical envelopes that
    // stand on it. Each appears exactly once, and identities ascend.
    const std::vector<std::string> expected = {"hall-1", "rack-01", "rack-02", "rack-03",
                                               "rack-04"};
    SC_CHECK_EQ(report.value().planes.size(), expected.size());
    for (std::size_t i = 0; i < report.value().planes.size() && i < expected.size(); ++i) {
      SC_CHECK_EQ(report.value().planes.at(i).node.str(), expected.at(i));
    }
    for (std::size_t i = 1; i < report.value().planes.size(); ++i) {
      SC_CHECK(report.value().planes.at(i - 1).node < report.value().planes.at(i).node);
    }
    std::size_t seen = 0;
    for (const NodeCapacity& plane : report.value().planes) {
      for (const NodeCapacity& other : report.value().planes) {
        if (plane.node == other.node) ++seen;
      }
    }
    SC_CHECK_EQ(seen, report.value().planes.size());  // each identity once per pass
    SC_CHECK(report.value().planes.at(0).owns_plane);
    SC_CHECK(report.value().planes.at(0).attributed_plane == id.hall);
    SC_CHECK(report.value().planes.at(1).owns_rack_envelope);
    SC_CHECK(report.value().planes.at(1).attributed_plane == id.hall);

    // The report's own ledger is the subtree rollup it summarized.
    const Result<CapacityRollup> rolled = registry.rollup(id.hall, plain);
    SC_CHECK(report.value().area == rolled.value().area);
    SC_CHECK(report.value().units == rolled.value().units);
  }

  SC_CASE("the rollup counts every record family of a subtree exactly once");
  {
    const Result<CapacityRollup> rolled = registry.rollup(id.site, plain);
    SC_CHECK(rolled.ok());
    const CapacityRollup& value = rolled.value();

    // site-a, bldg-1, hall-1, row-1, row-2, rack-01..04, band-a.
    SC_CHECK_EQ(value.node_count, 10ull);
    SC_CHECK_EQ(value.claim_count, 2ull);
    SC_CHECK_EQ(value.reservation_count, 1ull);
    SC_CHECK_EQ(value.exclusion_count, 1ull);
    SC_CHECK_EQ(value.clearance_count, 1ull);
    SC_CHECK_EQ(value.expansion_zone_count, 1ull);
    SC_CHECK_EQ(value.rack_count, 4ull);
    SC_CHECK_EQ(value.plane_owner_count, 2ull);

    // The four kinds of record counted above are the four the fixture holds.
    SC_CHECK_EQ(snapshot->claims().size(), static_cast<std::size_t>(2));
    SC_CHECK_EQ(snapshot->reservations().size(), static_cast<std::size_t>(1));
    SC_CHECK_EQ(snapshot->exclusions().size(), static_cast<std::size_t>(1));
    SC_CHECK_EQ(snapshot->clearances().size(), static_cast<std::size_t>(1));
    SC_CHECK_EQ(snapshot->expansion_zones().size(), static_cast<std::size_t>(1));
    SC_CHECK_EQ(snapshot->nodes().size(), static_cast<std::size_t>(10));
    SC_CHECK_EQ(snapshot->record_count(), static_cast<std::size_t>(16));
  }

  return ::sc_test::summary("rollup");
}
