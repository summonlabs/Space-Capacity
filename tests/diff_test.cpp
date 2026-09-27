// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - deterministic diffs between two revisions.
//
// Proves that a diff reports whole records, in canonical identity order, with
// the exact list of fields that moved; that the empty model against the fixture
// is exactly one `added` change per record and nothing else; that the same two
// revisions always produce an equal diff; that the model-wide ledger movement
// is the difference of the two whole-model rollups computed independently in
// the test; and that a revision which is no longer retained is refused rather
// than approximated.
//
// A metadata mutation advances the record's generation as well as the field it
// edits, so the field list of an edited record carries "generation" too. That
// is asserted exactly, because it is the field list a caller acts on.

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "fixture.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;
using sc_fixture::Ids;

bool has_field(const std::vector<std::string>& fields, const char* name) {
  for (const std::string& field : fields) {
    if (field == name) return true;
  }
  return false;
}

void check_ascending(const std::vector<SpaceNodeId>& ids) {
  for (std::size_t i = 1; i < ids.size(); ++i) {
    SC_CHECK(ids.at(i - 1) < ids.at(i));
  }
}

std::vector<SpaceNodeId> node_ids(const CapacityDiff& diff) {
  std::vector<SpaceNodeId> ids;
  for (const NodeChange& change : diff.nodes) ids.push_back(change.id);
  return ids;
}
std::vector<OccupancyClaimId> claim_ids(const CapacityDiff& diff) {
  std::vector<OccupancyClaimId> ids;
  for (const ClaimChange& change : diff.claims) ids.push_back(change.id);
  return ids;
}
std::vector<OccupancyClaimId> reservation_ids(const CapacityDiff& diff) {
  std::vector<OccupancyClaimId> ids;
  for (const ReservationChange& change : diff.reservations) ids.push_back(change.id);
  return ids;
}
std::vector<ExclusionRegionId> exclusion_ids(const CapacityDiff& diff) {
  std::vector<ExclusionRegionId> ids;
  for (const ExclusionChange& change : diff.exclusions) ids.push_back(change.id);
  return ids;
}
std::vector<ClearanceConstraintId> clearance_ids(const CapacityDiff& diff) {
  std::vector<ClearanceConstraintId> ids;
  for (const ClearanceChange& change : diff.clearances) ids.push_back(change.id);
  return ids;
}
std::vector<ExpansionZoneId> zone_ids(const CapacityDiff& diff) {
  std::vector<ExpansionZoneId> ids;
  for (const ExpansionZoneChange& change : diff.expansion_zones) ids.push_back(change.id);
  return ids;
}

// Every change vector of one diff, in the order the families are declared.
void check_canonical_order(const CapacityDiff& diff) {
  check_ascending(node_ids(diff));
  const std::vector<OccupancyClaimId> claims = claim_ids(diff);
  for (std::size_t i = 1; i < claims.size(); ++i) SC_CHECK(claims.at(i - 1) < claims.at(i));
  const std::vector<OccupancyClaimId> reservations = reservation_ids(diff);
  for (std::size_t i = 1; i < reservations.size(); ++i) {
    SC_CHECK(reservations.at(i - 1) < reservations.at(i));
  }
  const std::vector<ExclusionRegionId> exclusions = exclusion_ids(diff);
  for (std::size_t i = 1; i < exclusions.size(); ++i) {
    SC_CHECK(exclusions.at(i - 1) < exclusions.at(i));
  }
  const std::vector<ClearanceConstraintId> clearances = clearance_ids(diff);
  for (std::size_t i = 1; i < clearances.size(); ++i) {
    SC_CHECK(clearances.at(i - 1) < clearances.at(i));
  }
  const std::vector<ExpansionZoneId> zones = zone_ids(diff);
  for (std::size_t i = 1; i < zones.size(); ++i) SC_CHECK(zones.at(i - 1) < zones.at(i));
}

// The whole-model ledger movement, computed in the test from the two rollups.
void check_ledger_movement(const CapacityDiff& diff, const Snapshot& before,
                           const Snapshot& after) {
  const FitContext context{};
  const CapacityRollup before_rollup = model_rollup(before, context);
  const CapacityRollup after_rollup = model_rollup(after, context);
  SC_CHECK(diff.area_delta == subtract(before_rollup.area, after_rollup.area));
  SC_CHECK(diff.unit_delta == subtract(before_rollup.units, after_rollup.units));

  // Field by field, so a single field's sign cannot hide behind equality.
  const AreaLedgerDelta area = subtract(before_rollup.area, after_rollup.area);
  SC_CHECK_EQ(diff.area_delta.declared, area.declared);
  SC_CHECK_EQ(diff.area_delta.excluded, area.excluded);
  SC_CHECK_EQ(diff.area_delta.usable, area.usable);
  SC_CHECK_EQ(diff.area_delta.structural, area.structural);
  SC_CHECK_EQ(diff.area_delta.claimed, area.claimed);
  SC_CHECK_EQ(diff.area_delta.held, area.held);
  SC_CHECK_EQ(diff.area_delta.earmarked, area.earmarked);
  SC_CHECK_EQ(diff.area_delta.available, area.available);
  SC_CHECK_EQ(diff.area_delta.pending, area.pending);
  SC_CHECK_EQ(diff.area_delta.planned, area.planned);
  const UnitLedgerDelta units = subtract(before_rollup.units, after_rollup.units);
  SC_CHECK_EQ(diff.unit_delta.declared, units.declared);
  SC_CHECK_EQ(diff.unit_delta.excluded, units.excluded);
  SC_CHECK_EQ(diff.unit_delta.usable, units.usable);
  SC_CHECK_EQ(diff.unit_delta.occupied, units.occupied);
  SC_CHECK_EQ(diff.unit_delta.available, units.available);
  SC_CHECK_EQ(diff.unit_delta.pending, units.pending);
  SC_CHECK_EQ(diff.unit_delta.planned, units.planned);
}

}  // namespace

int main() {
  SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
  const Ids id = sc_fixture::ids();
  const FitContext plain{};

  SC_CASE("the empty model against the fixture is one added change per record");
  {
    SpaceCapacityRegistry empty =
        SpaceCapacityRegistry::create_in_memory(*StoreId::parse("diff-empty")).value();
    const SnapshotPtr before = empty.snapshot();
    const SnapshotPtr after = registry.snapshot();

    SC_CHECK(before->empty());
    SC_CHECK_EQ(before->record_count(), static_cast<std::size_t>(0));
    SC_CHECK_EQ(before->node_count(), static_cast<std::size_t>(0));
    // An empty model's digest is the digest of its own canonical bytes.
    SC_CHECK(before->digest() ==
             digest_in_domain(kDomainStateBinary, before->canonical_bytes()));

    const CapacityDiff diff = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK(!diff.empty());
    SC_CHECK_EQ(diff.change_count(), after->record_count());
    SC_CHECK_EQ(diff.change_count(), static_cast<std::size_t>(16));
    SC_CHECK_EQ(diff.from_revision.value(), before->revision().value());
    SC_CHECK_EQ(diff.to_revision.value(), after->revision().value());
    SC_CHECK(diff.from_digest == before->digest());
    SC_CHECK(diff.to_digest == after->digest());
    SC_CHECK(diff.from_incarnation == before->incarnation());
    SC_CHECK(diff.to_incarnation == after->incarnation());

    // One added change per record of every family, and nothing removed or
    // modified.
    SC_CHECK_EQ(diff.nodes.size(), after->nodes().size());
    SC_CHECK_EQ(diff.claims.size(), after->claims().size());
    SC_CHECK_EQ(diff.reservations.size(), after->reservations().size());
    SC_CHECK_EQ(diff.exclusions.size(), after->exclusions().size());
    SC_CHECK_EQ(diff.clearances.size(), after->clearances().size());
    SC_CHECK_EQ(diff.expansion_zones.size(), after->expansion_zones().size());

    for (const NodeChange& change : diff.nodes) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(!change.before.has_value());
      SC_CHECK(change.after.has_value());
      SC_CHECK(change.fields.empty());  // an addition has no field list
      SC_CHECK(after->find_node(change.id) != nullptr);
    }
    for (const ClaimChange& change : diff.claims) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(after->find_claim(change.id) != nullptr);
    }
    for (const ReservationChange& change : diff.reservations) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(after->find_reservation(change.id) != nullptr);
    }
    for (const ExclusionChange& change : diff.exclusions) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(after->find_exclusion(change.id) != nullptr);
    }
    for (const ClearanceChange& change : diff.clearances) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(after->find_clearance(change.id) != nullptr);
    }
    for (const ExpansionZoneChange& change : diff.expansion_zones) {
      SC_CHECK(change.kind == ChangeKind::added);
      SC_CHECK(after->find_expansion_zone(change.id) != nullptr);
    }

    check_canonical_order(diff);
    check_ledger_movement(diff, *before, *after);

    // Every addition moves the whole-model ledger up by exactly the area that
    // physically exists: the building's 2,400,000,000, of which the hall's
    // 600,000,000 is a subdivision counted inside it, and 4 x 42 rack units.
    SC_CHECK_EQ(diff.area_delta.declared, 2'400'000'000);
    SC_CHECK_EQ(diff.unit_delta.declared, 168);
    SC_CHECK_EQ(diff.area_delta.available, 2'366'580'000);
  }

  SC_CASE("a diff of a revision against itself is empty");
  {
    const SnapshotPtr current = registry.snapshot();
    const CapacityDiff static_diff = SpaceCapacityRegistry::diff(*current, *current);
    SC_CHECK(static_diff.empty());
    SC_CHECK_EQ(static_diff.change_count(), static_cast<std::size_t>(0));
    SC_CHECK(static_diff.nodes.empty());
    SC_CHECK(static_diff.claims.empty());
    SC_CHECK(static_diff.area_delta.is_zero());
    SC_CHECK(static_diff.unit_delta.is_zero());
    SC_CHECK(static_diff.explanations.empty());
    SC_CHECK_EQ(static_diff.from_revision.value(), static_diff.to_revision.value());
    SC_CHECK(static_diff.from_digest == static_diff.to_digest);

    const Result<CapacityDiff> retained =
        registry.diff(current->revision(), current->revision());
    SC_CHECK(retained.ok());
    SC_CHECK(retained.value().empty());
    SC_CHECK(static_diff == retained.value());
  }

  SC_CASE("a metadata change shows exactly the fields that moved");
  {
    const SnapshotPtr before = registry.snapshot();
    SetNodeMetadataRequest request;
    request.node = id.hall;
    request.label = *DisplayLabel::parse("Hall 1 renamed");
    const Result<MutationOutcome> outcome = registry.apply(request);
    SC_CHECK(outcome.ok());
    const SnapshotPtr after = registry.snapshot();

    const CapacityDiff diff = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK_EQ(diff.nodes.size(), static_cast<std::size_t>(1));
    SC_CHECK(diff.claims.empty());
    SC_CHECK(diff.reservations.empty());
    SC_CHECK(diff.exclusions.empty());
    SC_CHECK(diff.clearances.empty());
    SC_CHECK(diff.expansion_zones.empty());

    const NodeChange& change = diff.nodes.at(0);
    SC_CHECK(change.kind == ChangeKind::modified);
    SC_CHECK(change.id == id.hall);
    SC_CHECK(change.before.has_value());
    SC_CHECK(change.after.has_value());

    // The label moved, and so did the record's generation: every metadata
    // mutation advances it. Those are the only two fields that differ.
    SC_CHECK_EQ(change.fields.size(), static_cast<std::size_t>(2));
    SC_CHECK_EQ(change.fields.at(0), std::string("generation"));
    SC_CHECK_EQ(change.fields.at(1), std::string("label"));
    SC_CHECK(has_field(change.fields, "label"));
    SC_CHECK(!has_field(change.fields, "note"));
    SC_CHECK(!has_field(change.fields, "lifecycle"));
    SC_CHECK(!has_field(change.fields, "placement"));
    if (change.before.has_value() && change.after.has_value()) {
      SC_CHECK(change.before->label.str() != change.after->label.str());
      SC_CHECK_EQ(change.after->label.str(), std::string("Hall 1 renamed"));
      SC_CHECK_EQ(change.after->generation.value(), change.before->generation.value() + 1);
      SC_CHECK(change.before->placement == change.after->placement);
    }
    SC_CHECK(diff.explanations.empty());

    check_canonical_order(diff);
    check_ledger_movement(diff, *before, *after);
    // A label moves no capacity number.
    SC_CHECK(diff.area_delta.is_zero());
    SC_CHECK(diff.unit_delta.is_zero());
    SC_CHECK(diff.area_delta.available == 0);

    // The retained-revision path gives the same answer as the snapshot path.
    const Result<CapacityDiff> retained =
        registry.diff(before->revision(), after->revision());
    SC_CHECK(retained.ok());
    SC_CHECK(retained.value() == diff);
  }

  SC_CASE("retiring a node is explained, and the node is modified rather than removed");
  {
    const SnapshotPtr before = registry.snapshot();
    RetireNodeRequest request;
    request.node = id.rack4;
    const Result<MutationOutcome> outcome = registry.apply(request);
    SC_CHECK(outcome.ok());
    const SnapshotPtr after = registry.snapshot();

    const CapacityDiff diff = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK_EQ(diff.nodes.size(), static_cast<std::size_t>(1));
    const NodeChange& change = diff.nodes.at(0);
    SC_CHECK(change.kind == ChangeKind::modified);
    SC_CHECK(change.id == id.rack4);
    SC_CHECK(has_field(change.fields, "lifecycle"));
    SC_CHECK_EQ(change.fields.size(), static_cast<std::size_t>(2));
    SC_CHECK_EQ(change.fields.at(0), std::string("generation"));
    SC_CHECK_EQ(change.fields.at(1), std::string("lifecycle"));
    SC_CHECK(change.after.has_value());
    if (change.after.has_value()) {
      SC_CHECK(change.after->lifecycle == NodeLifecycle::retired);
      SC_CHECK(change.after->replaced_by.empty());
    }

    // The retirement is explained, and the record is still there: retirement is
    // a capacity event, not a deletion.
    SC_CHECK(diff.explanations.contains(ReasonCode::node_retired));
    SC_CHECK(after->find_node(id.rack4) != nullptr);
    SC_CHECK(after->find_clearance(id.clearance) != nullptr);

    check_canonical_order(diff);
    check_ledger_movement(diff, *before, *after);
    // The retired rack contributes nothing usable, so the unit rollup loses
    // every one of its 42 units.
    SC_CHECK_EQ(diff.unit_delta.available, -42);
    SC_CHECK_EQ(diff.unit_delta.usable, -42);
  }

  SC_CASE("replacing a node modifies both ends of the lineage and explains it");
  {
    const SnapshotPtr before = registry.snapshot();
    SetNodeLifecycleRequest request;
    request.node = id.rack2;
    request.lifecycle = NodeLifecycle::replaced;
    request.successor = id.rack1;
    const Result<MutationOutcome> outcome = registry.apply(request);
    SC_CHECK(outcome.ok());
    const SnapshotPtr after = registry.snapshot();

    const CapacityDiff diff = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK_EQ(diff.nodes.size(), static_cast<std::size_t>(2));
    SC_CHECK(diff.nodes.at(0).id == id.rack1);
    SC_CHECK(diff.nodes.at(1).id == id.rack2);
    for (const NodeChange& change : diff.nodes) {
      SC_CHECK(change.kind == ChangeKind::modified);
    }
    SC_CHECK(has_field(diff.nodes.at(0).fields, "replaces"));
    SC_CHECK(has_field(diff.nodes.at(1).fields, "lifecycle"));
    SC_CHECK(has_field(diff.nodes.at(1).fields, "replaced_by"));
    SC_CHECK(has_field(diff.nodes.at(1).fields, "generation"));

    SC_CHECK(diff.explanations.contains(ReasonCode::lineage_preserved));
    SC_CHECK(diff.explanations.contains(ReasonCode::node_replaced));
    const SpaceNode* successor = after->find_node(id.rack1);
    const SpaceNode* replaced = after->find_node(id.rack2);
    SC_CHECK(successor != nullptr);
    SC_CHECK(replaced != nullptr);
    if (successor != nullptr && replaced != nullptr) {
      SC_CHECK(successor->replaces == id.rack2);
      SC_CHECK(replaced->replaced_by == id.rack1);
      SC_CHECK(replaced->lifecycle == NodeLifecycle::replaced);
    }

    check_canonical_order(diff);
    check_ledger_movement(diff, *before, *after);
  }

  SC_CASE("a claim state change is a modification of the claim record");
  {
    const SnapshotPtr before = registry.snapshot();
    TransitionClaimRequest request;
    request.claim = id.planar_claim;
    request.next = ClaimState::releasing;
    const Result<MutationOutcome> outcome = registry.apply(request);
    SC_CHECK(outcome.ok());
    const SnapshotPtr after = registry.snapshot();

    const CapacityDiff diff = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK_EQ(diff.claims.size(), static_cast<std::size_t>(1));
    SC_CHECK(diff.nodes.empty());

    const ClaimChange& change = diff.claims.at(0);
    SC_CHECK(change.kind == ChangeKind::modified);
    SC_CHECK(change.id == id.planar_claim);
    // The state moved and, because a claim transition also advances the
    // record's generation, so did the generation. Nothing else did.
    SC_CHECK_EQ(change.fields.size(), static_cast<std::size_t>(2));
    SC_CHECK_EQ(change.fields.at(0), std::string("generation"));
    SC_CHECK_EQ(change.fields.at(1), std::string("state"));
    SC_CHECK(has_field(change.fields, "state"));
    SC_CHECK(!has_field(change.fields, "scope"));
    SC_CHECK(!has_field(change.fields, "node"));

    // Nothing was removed: the claim is still in the model, in its new state.
    SC_CHECK(after->find_claim(id.planar_claim) != nullptr);
    const OccupancyClaim* stored = after->find_claim(id.planar_claim);
    if (stored != nullptr) {
      SC_CHECK(stored->state == ClaimState::releasing);
      SC_CHECK(stored->consumes());  // a releasing claim still consumes
    }

    check_canonical_order(diff);
    check_ledger_movement(diff, *before, *after);
    // Releasing consumes exactly as committing did, so no capacity moved.
    SC_CHECK(diff.area_delta.is_zero());
  }

  SC_CASE("the same two revisions always produce the same diff");
  {
    const SnapshotPtr before = registry.snapshot();
    SetNodeMetadataRequest request;
    request.node = id.row1;
    request.label = *DisplayLabel::parse("Row 1 renamed");
    SC_CHECK(registry.apply(request).ok());
    const SnapshotPtr after = registry.snapshot();

    const CapacityDiff first = SpaceCapacityRegistry::diff(*before, *after);
    const CapacityDiff second = SpaceCapacityRegistry::diff(*before, *after);
    SC_CHECK(first == second);
    SC_CHECK(!(first != second));
    SC_CHECK_EQ(first.change_count(), second.change_count());

    SC_CHECK_EQ(first.nodes.size(), second.nodes.size());
    for (std::size_t i = 0; i < first.nodes.size() && i < second.nodes.size(); ++i) {
      SC_CHECK(first.nodes.at(i).fields == second.nodes.at(i).fields);
      SC_CHECK(first.nodes.at(i).id == second.nodes.at(i).id);
    }
    check_canonical_order(first);
    check_canonical_order(second);
    check_ledger_movement(first, *before, *after);

    // Through the retained-revision path as well.
    const Result<CapacityDiff> retained = registry.diff(before->revision(), after->revision());
    SC_CHECK(retained.ok());
    SC_CHECK(retained.value() == first);
    SC_CHECK(retained.value().nodes.at(0).fields == first.nodes.at(0).fields);
  }

  SC_CASE("a revision that is no longer retained is refused, a retained pair is not");
  {
    SpaceCapacityRegistry small =
        SpaceCapacityRegistry::create_in_memory(*StoreId::parse("diff-retained")).value();
    const RegistryRevision first_revision = small.revision();
    SC_CHECK_EQ(first_revision.value(), 0ull);

    // More mutations than the retained window holds.
    const std::uint32_t mutations = kRetainedRevisions + 1;
    for (std::uint32_t i = 0; i < mutations; ++i) {
      CreateNodeRequest request;
      request.node.id = *SpaceNodeId::parse("retained-" + std::to_string(i));
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::site;
      request.node.spatial_class = SpatialClass::outdoor;
      request.node.lifecycle = NodeLifecycle::available;
      request.node.label = *DisplayLabel::parse("Retained");
      SC_CHECK(small.apply(request).ok());
    }
    const RegistryRevision current = small.revision();
    SC_CHECK_EQ(current.value(), static_cast<std::uint64_t>(mutations));

    // The oldest revision fell out of the window: the diff is refused rather
    // than approximated from what is left.
    const Result<CapacityDiff> too_old = small.diff(first_revision, current);
    SC_CHECK(!too_old);
    if (!too_old.ok()) {
      SC_CHECK(too_old.error().code == ErrorCode::stale_revision);
      SC_CHECK(too_old.error().has_numbers);
      SC_CHECK_EQ(too_old.error().expected, first_revision.value());
      SC_CHECK_EQ(too_old.error().actual, current.value());
    }

    // Two revisions that are still retained diff normally.
    const RegistryRevision oldest_retained =
        RegistryRevision{current.value() - (kRetainedRevisions - 1)};
    const Result<CapacityDiff> retained = small.diff(oldest_retained, current);
    SC_CHECK(retained.ok());
    if (retained.ok()) {
      SC_CHECK_EQ(retained.value().from_revision.value(), oldest_retained.value());
      SC_CHECK_EQ(retained.value().to_revision.value(), current.value());
      // Every mutation above created one site, and the oldest retained revision
      // already held the first two of them.
      SC_CHECK_EQ(retained.value().change_count(), static_cast<std::size_t>(7));
      SC_CHECK_EQ(retained.value().nodes.size(), static_cast<std::size_t>(7));
      for (const NodeChange& change : retained.value().nodes) {
        SC_CHECK(change.kind == ChangeKind::added);
      }
      check_canonical_order(retained.value());
      // Every one of the seven added sites declares no plane and no rack
      // envelope, so the model-wide ledger does not move at all.
      SC_CHECK(retained.value().area_delta.is_zero());
      SC_CHECK(retained.value().unit_delta.is_zero());
    }

    // A later revision than the one committed is refused too.
    const Result<CapacityDiff> future =
        small.diff(current, RegistryRevision{current.value() + 1});
    SC_CHECK(!future);
    if (!future.ok()) {
      SC_CHECK(future.error().code == ErrorCode::stale_revision);
    }
  }

  SC_CASE("every field name a diff can report is stable and never empty");
  {
    // The tables are part of the CLI contract: dense from zero, and past the
    // end the accessor says "unknown" rather than returning nothing.
    SC_CHECK_EQ(node_field_name(0), std::string("generation"));
    SC_CHECK_EQ(node_field_name(4), std::string("label"));
    SC_CHECK_EQ(node_field_name(18), std::string("replaces"));
    SC_CHECK_EQ(node_field_name(19), std::string("unknown"));
    SC_CHECK_EQ(claim_field_name(3), std::string("state"));
    SC_CHECK_EQ(claim_field_name(11), std::string("from_reservation"));
    SC_CHECK_EQ(reservation_field_name(3), std::string("state"));
    SC_CHECK_EQ(reservation_field_name(9), std::string("not_after"));
    SC_CHECK_EQ(exclusion_field_name(4), std::string("state"));
    SC_CHECK_EQ(clearance_field_name(5), std::string("enforceable"));
    SC_CHECK_EQ(expansion_zone_field_name(3), std::string("state"));
    SC_CHECK_EQ(expansion_zone_field_name(64), std::string("unknown"));

    SC_CHECK_EQ(change_kind_name(ChangeKind::added), std::string("added"));
    SC_CHECK_EQ(change_kind_name(ChangeKind::removed), std::string("removed"));
    SC_CHECK_EQ(change_kind_name(ChangeKind::modified), std::string("modified"));
    ChangeKind parsed = ChangeKind::modified;
    SC_CHECK(parse_change_kind("added", parsed));
    SC_CHECK(parsed == ChangeKind::added);
    SC_CHECK(!parse_change_kind("added ", parsed));
  }

  return ::sc_test::summary("diff");
}
