// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - deterministic diffs between two revisions of a model.
//
// A diff is defined over whole records and over signed ledger deltas. Two
// records of one family are compared field by field and never by whole-record
// equality, so a caller is told exactly which fields moved instead of only that
// something did. Each family is merged over the two snapshots' canonical
// identity order with a two-pointer walk, which yields the change vector in
// canonical order without a second sort.
//
// Determinism: the result is built only from the two snapshots. No pointer
// value, no clock reading, no locale, no hash order and no iteration over an
// unordered container reaches it, so the same two revisions always produce
// byte-identical output.
//
// The ledger movement is the whole-model rollup of the later revision less the
// whole-model rollup of the earlier one. Every delta field is `to - from`,
// computed with the checked signed helpers and clamped at the ends of int64
// rather than allowed to wrap.

#include "diff_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/space_capacity/checked.hpp"
#include "dccp/space_capacity/query.hpp"

namespace dccp::space_capacity {
namespace {

// ---------------------------------------------------------------------------
// Field-name tables
// ---------------------------------------------------------------------------
//
// The indices are the persisted CLI contract: a name is never renumbered and
// never removed, and the tables are dense from zero so a caller can walk them
// until the returned name is "unknown". The identity of a record is absent from
// every table on purpose: an identity is what a merge keys on, so it cannot
// itself change between two revisions of the same record.

constexpr std::string_view kNodeFields[] = {
    "generation",     // 0
    "kind",           // 1
    "spatial_class",  // 2
    "lifecycle",      // 3
    "label",          // 4
    "note",           // 5
    "parent",         // 6
    "depth",          // 7
    "placement",      // 8
    "own_planar",     // 9
    "own_rack",       // 10
    "location",       // 11
    "facility_node",  // 12
    "rack",           // 13
    "assets",         // 14
    "policies",       // 15
    "evidence",       // 16
    "replaced_by",    // 17
    "replaces",       // 18
};

constexpr std::string_view kClaimFields[] = {
    "generation",        // 0
    "node",              // 1
    "scope",             // 2
    "state",             // 3
    "occupant",          // 4
    "label",             // 5
    "note",              // 6
    "rack",              // 7
    "assets",            // 8
    "policies",          // 9
    "evidence",          // 10
    "from_reservation",  // 11
};

constexpr std::string_view kReservationFields[] = {
    "generation",     // 0
    "node",           // 1
    "scope",          // 2
    "state",          // 3
    "reservation",    // 4
    "holder_label",   // 5
    "holder_assets",  // 6
    "policies",       // 7
    "evidence",       // 8
    "not_after",      // 9
};

constexpr std::string_view kExclusionFields[] = {
    "generation",  // 0
    "node",        // 1
    "scope",       // 2
    "reason",      // 3
    "state",       // 4
    "blocks",      // 5
    "label",       // 6
    "note",        // 7
    "policies",    // 8
    "subjects",    // 9
    "evidence",    // 10
};

constexpr std::string_view kClearanceFields[] = {
    "generation",   // 0
    "node",         // 1
    "kind",         // 2
    "band",         // 3
    "units",        // 4
    "enforceable",  // 5
    "label",        // 6
    "note",         // 7
    "policies",     // 8
    "evidence",     // 9
};

constexpr std::string_view kExpansionZoneFields[] = {
    "generation",       // 0
    "node",             // 1
    "scope",            // 2
    "state",            // 3
    "label",            // 4
    "note",             // 5
    "policies",         // 6
    "evidence",         // 7
    "target_ready_by",  // 8
};

// The name at one index of one table, or "unknown" when the index is past the
// end. Dense tables plus this bound check are what make the six accessors total
// functions with no gaps.
template <std::size_t N>
[[nodiscard]] std::string_view name_at(const std::string_view (&names)[N],
                                       std::uint32_t index) noexcept {
  if (index >= N) {
    return "unknown";
  }
  return names[static_cast<std::size_t>(index)];
}

// Appends the stable name of a differing field. The call sites below run in
// ascending index order, so the appended list is in the canonical order of the
// table and no name can appear twice.
void note_if(bool differs, std::string_view (*name_of)(std::uint32_t) noexcept,
             std::uint32_t index, std::vector<std::string>& fields) {
  if (differs) {
    fields.emplace_back(name_of(index));
  }
}

// Two optionals differ when their presence differs, or when both are present
// and their values differ. Comparing presence first is what keeps "absent" and
// "present and empty" apart.
template <typename T>
[[nodiscard]] bool optional_differs(const std::optional<T>& a,
                                    const std::optional<T>& b) noexcept {
  if (a.has_value() != b.has_value()) {
    return true;
  }
  if (!a.has_value()) {
    return false;
  }
  return !(*a == *b);
}

// ---------------------------------------------------------------------------
// Signed ledger deltas
// ---------------------------------------------------------------------------

// `to - from` for one ledger field, clamped instead of wrapped.
//
// `checked_sub_signed` decides whether the exact difference is representable
// before it forms it, so no overflowing subtraction is ever evaluated. When the
// difference does not fit, the result saturates at the matching end of int64:
// int64 minus int64 can only leave the range when the operands have opposite
// signs, and the sign of the true difference is then the sign of `to`, so the
// saturated end is known without computing the overflowing value.
[[nodiscard]] std::int64_t clamped_delta(std::int64_t to, std::int64_t from) noexcept {
  const Checked<std::int64_t> difference = checked_sub_signed<std::int64_t>(to, from);
  if (!difference.overflowed) {
    return difference.value;
  }
  return to < 0 ? std::numeric_limits<std::int64_t>::min()
                : std::numeric_limits<std::int64_t>::max();
}

// One area field. Square millimetres are already int64, so the clamp is
// reachable and is the documented outcome rather than undefined behaviour.
[[nodiscard]] std::int64_t delta_of(SquareMillimeters to, SquareMillimeters from) noexcept {
  return clamped_delta(to.value(), from.value());
}

// One rack-unit field. Rack units are int32, so widening both sides to int64
// makes the difference exact and the clamp unreachable; the same checked path
// is used anyway so that every ledger field is computed one way.
[[nodiscard]] std::int64_t delta_of(RackUnits to, RackUnits from) noexcept {
  return clamped_delta(static_cast<std::int64_t>(to.value()),
                       static_cast<std::int64_t>(from.value()));
}

// ---------------------------------------------------------------------------
// Identity merge
// ---------------------------------------------------------------------------

// Merges one family of the two snapshots. Both vectors are in canonical
// identity order, so one pass produces the canonical change list: an identity
// only in `before` is a removal, one only in `after` is an addition, and one in
// both is a modification when any comparable field moved. A record present in
// both with nothing comparable changed is not a change and is not emitted.
template <typename Record, typename Id, typename Differs>
void merge_family(const std::vector<Record>& before, const std::vector<Record>& after,
                  Differs differs, std::vector<RecordChange<Record, Id>>& out) {
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < before.size() && right < after.size()) {
    const Record& a = before[left];
    const Record& b = after[right];
    if (a.id < b.id) {
      RecordChange<Record, Id> change;
      change.kind = ChangeKind::removed;
      change.id = a.id;
      change.before = a;
      out.push_back(std::move(change));
      ++left;
    } else if (b.id < a.id) {
      RecordChange<Record, Id> change;
      change.kind = ChangeKind::added;
      change.id = b.id;
      change.after = b;
      out.push_back(std::move(change));
      ++right;
    } else {
      RecordChange<Record, Id> change;
      change.kind = ChangeKind::modified;
      change.id = a.id;
      change.before = a;
      change.after = b;
      if (differs(a, b, change.fields)) {
        out.push_back(std::move(change));
      }
      ++left;
      ++right;
    }
  }
  for (; left < before.size(); ++left) {
    RecordChange<Record, Id> change;
    change.kind = ChangeKind::removed;
    change.id = before[left].id;
    change.before = before[left];
    out.push_back(std::move(change));
  }
  for (; right < after.size(); ++right) {
    RecordChange<Record, Id> change;
    change.kind = ChangeKind::added;
    change.id = after[right].id;
    change.after = after[right];
    out.push_back(std::move(change));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Change kinds
// ---------------------------------------------------------------------------

std::string_view change_kind_name(ChangeKind value) noexcept {
  switch (value) {
    case ChangeKind::added:
      return "added";
    case ChangeKind::removed:
      return "removed";
    case ChangeKind::modified:
      return "modified";
  }
  return "unknown";
}

bool parse_change_kind(std::string_view text, ChangeKind& out) noexcept {
  if (text == "added") {
    out = ChangeKind::added;
    return true;
  }
  if (text == "removed") {
    out = ChangeKind::removed;
    return true;
  }
  if (text == "modified") {
    out = ChangeKind::modified;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Field names
// ---------------------------------------------------------------------------

std::string_view node_field_name(std::uint32_t index) noexcept {
  return name_at(kNodeFields, index);
}

std::string_view claim_field_name(std::uint32_t index) noexcept {
  return name_at(kClaimFields, index);
}

std::string_view reservation_field_name(std::uint32_t index) noexcept {
  return name_at(kReservationFields, index);
}

std::string_view exclusion_field_name(std::uint32_t index) noexcept {
  return name_at(kExclusionFields, index);
}

std::string_view clearance_field_name(std::uint32_t index) noexcept {
  return name_at(kClearanceFields, index);
}

std::string_view expansion_zone_field_name(std::uint32_t index) noexcept {
  return name_at(kExpansionZoneFields, index);
}

// ---------------------------------------------------------------------------
// Ledger deltas
// ---------------------------------------------------------------------------

bool AreaLedgerDelta::is_zero() const noexcept {
  return declared == 0 && excluded == 0 && usable == 0 && structural == 0 && claimed == 0 &&
         held == 0 && earmarked == 0 && available == 0 && pending == 0 && planned == 0;
}

bool operator==(const AreaLedgerDelta& a, const AreaLedgerDelta& b) noexcept {
  return a.declared == b.declared && a.excluded == b.excluded && a.usable == b.usable &&
         a.structural == b.structural && a.claimed == b.claimed && a.held == b.held &&
         a.earmarked == b.earmarked && a.available == b.available && a.pending == b.pending &&
         a.planned == b.planned;
}

bool operator!=(const AreaLedgerDelta& a, const AreaLedgerDelta& b) noexcept {
  return !(a == b);
}

bool UnitLedgerDelta::is_zero() const noexcept {
  return declared == 0 && excluded == 0 && usable == 0 && occupied == 0 && available == 0 &&
         pending == 0 && planned == 0;
}

bool operator==(const UnitLedgerDelta& a, const UnitLedgerDelta& b) noexcept {
  return a.declared == b.declared && a.excluded == b.excluded && a.usable == b.usable &&
         a.occupied == b.occupied && a.available == b.available && a.pending == b.pending &&
         a.planned == b.planned;
}

bool operator!=(const UnitLedgerDelta& a, const UnitLedgerDelta& b) noexcept {
  return !(a == b);
}

AreaLedgerDelta subtract(const AreaLedger& from, const AreaLedger& to) noexcept {
  AreaLedgerDelta delta;
  delta.declared = delta_of(to.declared, from.declared);
  delta.excluded = delta_of(to.excluded, from.excluded);
  delta.usable = delta_of(to.usable, from.usable);
  delta.structural = delta_of(to.structural, from.structural);
  delta.claimed = delta_of(to.claimed, from.claimed);
  delta.held = delta_of(to.held, from.held);
  delta.earmarked = delta_of(to.earmarked, from.earmarked);
  delta.available = delta_of(to.available, from.available);
  delta.pending = delta_of(to.pending, from.pending);
  delta.planned = delta_of(to.planned, from.planned);
  return delta;
}

UnitLedgerDelta subtract(const UnitLedger& from, const UnitLedger& to) noexcept {
  UnitLedgerDelta delta;
  delta.declared = delta_of(to.declared, from.declared);
  delta.excluded = delta_of(to.excluded, from.excluded);
  delta.usable = delta_of(to.usable, from.usable);
  delta.occupied = delta_of(to.occupied, from.occupied);
  delta.available = delta_of(to.available, from.available);
  delta.pending = delta_of(to.pending, from.pending);
  delta.planned = delta_of(to.planned, from.planned);
  return delta;
}

// ---------------------------------------------------------------------------
// CapacityDiff
// ---------------------------------------------------------------------------

bool CapacityDiff::empty() const noexcept {
  return nodes.empty() && claims.empty() && reservations.empty() && exclusions.empty() &&
         clearances.empty() && expansion_zones.empty() && area_delta.is_zero() &&
         unit_delta.is_zero();
}

std::size_t CapacityDiff::change_count() const noexcept {
  return nodes.size() + claims.size() + reservations.size() + exclusions.size() +
         clearances.size() + expansion_zones.size();
}

bool operator==(const CapacityDiff& a, const CapacityDiff& b) noexcept {
  // ExplanationSet exposes no operator== of its own, so its two members are
  // compared through their accessors: the ordered item list and the truncation
  // marker together are the whole value.
  return a.from_revision == b.from_revision && a.to_revision == b.to_revision &&
         a.from_incarnation == b.from_incarnation && a.to_incarnation == b.to_incarnation &&
         a.from_digest == b.from_digest && a.to_digest == b.to_digest && a.nodes == b.nodes &&
         a.claims == b.claims && a.reservations == b.reservations &&
         a.exclusions == b.exclusions && a.clearances == b.clearances &&
         a.expansion_zones == b.expansion_zones && a.area_delta == b.area_delta &&
         a.unit_delta == b.unit_delta &&
         a.explanations.items() == b.explanations.items() &&
         a.explanations.truncated() == b.explanations.truncated();
}

bool operator!=(const CapacityDiff& a, const CapacityDiff& b) noexcept { return !(a == b); }

namespace internal {

// ---------------------------------------------------------------------------
// Per-family field comparison
// ---------------------------------------------------------------------------
//
// The call sites run in ascending field index, which is the canonical order of
// the matching table; each carries the name it emits, so the numbering in
// diff.hpp and the numbering here are checked against each other by reading.
// `fields` is always cleared first, so a caller may reuse one vector across
// families.

bool node_differs(const SpaceNode& a, const SpaceNode& b, std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, node_field_name, 0, fields);              // generation
  note_if(a.kind != b.kind, node_field_name, 1, fields);                          // kind
  note_if(a.spatial_class != b.spatial_class, node_field_name, 2, fields);        // spatial_class
  note_if(a.lifecycle != b.lifecycle, node_field_name, 3, fields);                // lifecycle
  note_if(a.label != b.label, node_field_name, 4, fields);                        // label
  note_if(a.note != b.note, node_field_name, 5, fields);                          // note
  note_if(a.parent != b.parent, node_field_name, 6, fields);                      // parent
  note_if(a.depth != b.depth, node_field_name, 7, fields);                        // depth
  note_if(a.placement != b.placement, node_field_name, 8, fields);                // placement
  note_if(a.own_planar != b.own_planar, node_field_name, 9, fields);              // own_planar
  note_if(a.own_rack != b.own_rack, node_field_name, 10, fields);                 // own_rack
  note_if(optional_differs(a.location, b.location), node_field_name, 11, fields);  // location
  note_if(optional_differs(a.facility_node, b.facility_node), node_field_name, 12,
          fields);                                                                // facility_node
  note_if(optional_differs(a.rack, b.rack), node_field_name, 13, fields);         // rack
  note_if(a.assets != b.assets, node_field_name, 14, fields);                     // assets
  note_if(a.policies != b.policies, node_field_name, 15, fields);                 // policies
  note_if(a.evidence != b.evidence, node_field_name, 16, fields);                 // evidence
  note_if(a.replaced_by != b.replaced_by, node_field_name, 17, fields);           // replaced_by
  note_if(a.replaces != b.replaces, node_field_name, 18, fields);                 // replaces
  return !fields.empty();
}

bool claim_differs(const OccupancyClaim& a, const OccupancyClaim& b,
                   std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, claim_field_name, 0, fields);              // generation
  note_if(a.node != b.node, claim_field_name, 1, fields);                          // node
  note_if(a.scope != b.scope, claim_field_name, 2, fields);                        // scope
  note_if(a.state != b.state, claim_field_name, 3, fields);                        // state
  note_if(a.occupant != b.occupant, claim_field_name, 4, fields);                  // occupant
  note_if(a.label != b.label, claim_field_name, 5, fields);                        // label
  note_if(a.note != b.note, claim_field_name, 6, fields);                          // note
  note_if(optional_differs(a.rack, b.rack), claim_field_name, 7, fields);          // rack
  note_if(a.assets != b.assets, claim_field_name, 8, fields);                      // assets
  note_if(a.policies != b.policies, claim_field_name, 9, fields);                  // policies
  note_if(a.evidence != b.evidence, claim_field_name, 10, fields);                 // evidence
  note_if(optional_differs(a.from_reservation, b.from_reservation), claim_field_name, 11,
          fields);                                                                // from_reservation
  return !fields.empty();
}

bool reservation_differs(const FootprintReservation& a, const FootprintReservation& b,
                         std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, reservation_field_name, 0, fields);  // generation
  note_if(a.node != b.node, reservation_field_name, 1, fields);              // node
  note_if(a.scope != b.scope, reservation_field_name, 2, fields);            // scope
  note_if(a.state != b.state, reservation_field_name, 3, fields);            // state
  note_if(a.reservation != b.reservation, reservation_field_name, 4, fields);  // reservation
  note_if(a.holder_label != b.holder_label, reservation_field_name, 5, fields);  // holder_label
  note_if(a.holder_assets != b.holder_assets, reservation_field_name, 6,
          fields);                                                             // holder_assets
  note_if(a.policies != b.policies, reservation_field_name, 7, fields);        // policies
  note_if(a.evidence != b.evidence, reservation_field_name, 8, fields);        // evidence
  note_if(optional_differs(a.not_after, b.not_after), reservation_field_name, 9,
          fields);                                                             // not_after
  return !fields.empty();
}

bool exclusion_differs(const ExclusionRegion& a, const ExclusionRegion& b,
                       std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, exclusion_field_name, 0, fields);  // generation
  note_if(a.node != b.node, exclusion_field_name, 1, fields);              // node
  note_if(a.scope != b.scope, exclusion_field_name, 2, fields);            // scope
  note_if(a.reason != b.reason, exclusion_field_name, 3, fields);          // reason
  note_if(a.state != b.state, exclusion_field_name, 4, fields);            // state
  note_if(a.blocks != b.blocks, exclusion_field_name, 5, fields);          // blocks
  note_if(a.label != b.label, exclusion_field_name, 6, fields);            // label
  note_if(a.note != b.note, exclusion_field_name, 7, fields);              // note
  note_if(a.policies != b.policies, exclusion_field_name, 8, fields);      // policies
  note_if(a.subjects != b.subjects, exclusion_field_name, 9, fields);      // subjects
  note_if(a.evidence != b.evidence, exclusion_field_name, 10, fields);     // evidence
  return !fields.empty();
}

bool clearance_differs(const ClearanceConstraint& a, const ClearanceConstraint& b,
                       std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, clearance_field_name, 0, fields);  // generation
  note_if(a.node != b.node, clearance_field_name, 1, fields);              // node
  note_if(a.kind != b.kind, clearance_field_name, 2, fields);              // kind
  // The planar band and the flag that says whether it is declared are one
  // comparable field: an undeclared band carries a default rectangle that must
  // not be read as a band, so the flag alone is a difference.
  note_if(a.band != b.band || a.has_band != b.has_band, clearance_field_name, 3,
          fields);                                                         // band
  // Likewise the vertical form: the interval set and its declared flag.
  note_if(a.required_free_units != b.required_free_units || a.has_units != b.has_units,
          clearance_field_name, 4, fields);                                // units
  note_if(a.enforceable != b.enforceable, clearance_field_name, 5, fields);  // enforceable
  note_if(a.label != b.label, clearance_field_name, 6, fields);              // label
  note_if(a.note != b.note, clearance_field_name, 7, fields);                // note
  note_if(a.policies != b.policies, clearance_field_name, 8, fields);        // policies
  note_if(a.evidence != b.evidence, clearance_field_name, 9, fields);        // evidence
  return !fields.empty();
}

bool expansion_zone_differs(const ExpansionZone& a, const ExpansionZone& b,
                            std::vector<std::string>& fields) {
  fields.clear();
  note_if(a.generation != b.generation, expansion_zone_field_name, 0, fields);  // generation
  note_if(a.node != b.node, expansion_zone_field_name, 1, fields);              // node
  note_if(a.scope != b.scope, expansion_zone_field_name, 2, fields);            // scope
  note_if(a.state != b.state, expansion_zone_field_name, 3, fields);            // state
  note_if(a.label != b.label, expansion_zone_field_name, 4, fields);            // label
  note_if(a.note != b.note, expansion_zone_field_name, 5, fields);              // note
  note_if(a.policies != b.policies, expansion_zone_field_name, 6, fields);      // policies
  note_if(a.evidence != b.evidence, expansion_zone_field_name, 7, fields);      // evidence
  note_if(optional_differs(a.target_ready_by, b.target_ready_by), expansion_zone_field_name, 8,
          fields);  // target_ready_by
  return !fields.empty();
}

// ---------------------------------------------------------------------------
// Whole diff
// ---------------------------------------------------------------------------

// The explanation pass. It reads the merged node changes, so it runs in
// canonical node order and its output does not depend on how the two snapshots
// were built. ExplanationSet sorts and deduplicates on insertion, so the final
// list depends only on the set of explanations offered.
void add_transition_explanations(CapacityDiff& diff) {
  for (const NodeChange& change : diff.nodes) {
    if (change.kind == ChangeKind::removed || !change.after.has_value()) {
      continue;
    }
    const SpaceNode& node = *change.after;
    const bool had_before = change.before.has_value();
    const std::string subject = node.id.str();

    // "Became retired" means the later revision has the node retired and the
    // earlier one did not: a node that arrives already retired has become
    // retired as far as the model is concerned, and a node that was already
    // retired has not changed.
    const bool was_retired = had_before && change.before->lifecycle == NodeLifecycle::retired;
    if (node.lifecycle == NodeLifecycle::retired && !was_retired) {
      diff.explanations.add(ReasonCode::node_retired, subject,
                            "node is retired and contributes no usable capacity");
    }

    const bool was_replaced = had_before && change.before->lifecycle == NodeLifecycle::replaced;
    if (node.lifecycle == NodeLifecycle::replaced && !was_replaced) {
      diff.explanations.add(ReasonCode::node_replaced, subject,
                            "node was replaced and contributes no usable capacity");
    }

    // Lineage is preserved when a successor is recorded now and was not
    // recorded before. Clearing a successor is a broken link, not a preserved
    // one, so it is not reported as preserved here.
    const bool successor_is_new =
        !node.replaced_by.empty() &&
        (!had_before || change.before->replaced_by != node.replaced_by);
    if (successor_is_new) {
      diff.explanations.add(ReasonCode::lineage_preserved, subject,
                            "replaced_by " + std::string(node.replaced_by.value()));
    }
  }

  // A model whose available area moved backwards is over-committed: the later
  // revision consumes more of the model than the earlier one left free. This is
  // a model-wide statement, so its subject is empty.
  if (diff.area_delta.available < 0) {
    diff.explanations.add(ReasonCode::over_committed, std::string(),
                          "model-wide available area decreased");
  }
}

CapacityDiff compute_diff(const Snapshot& before, const Snapshot& after) {
  CapacityDiff diff;

  diff.from_revision = before.revision();
  diff.to_revision = after.revision();
  diff.from_incarnation = before.incarnation();
  diff.to_incarnation = after.incarnation();
  diff.from_digest = before.digest();
  diff.to_digest = after.digest();

  // Every family is sorted by identity in a snapshot, so one two-pointer pass
  // per family is both correct and canonical.
  merge_family<SpaceNode, SpaceNodeId>(before.nodes(), after.nodes(), &node_differs, diff.nodes);
  merge_family<OccupancyClaim, OccupancyClaimId>(before.claims(), after.claims(), &claim_differs,
                                                 diff.claims);
  merge_family<FootprintReservation, OccupancyClaimId>(before.reservations(),
                                                       after.reservations(),
                                                       &reservation_differs, diff.reservations);
  merge_family<ExclusionRegion, ExclusionRegionId>(before.exclusions(), after.exclusions(),
                                                   &exclusion_differs, diff.exclusions);
  merge_family<ClearanceConstraint, ClearanceConstraintId>(
      before.clearances(), after.clearances(), &clearance_differs, diff.clearances);
  merge_family<ExpansionZone, ExpansionZoneId>(before.expansion_zones(), after.expansion_zones(),
                                               &expansion_zone_differs, diff.expansion_zones);

  // Model-wide ledger movement: the whole-model root rollup of the later
  // revision less the whole-model root rollup of the earlier one. The context
  // is the default reading, so the answer never depends on when it was asked.
  const CapacityRollup before_rollup = model_rollup(before, FitContext{});
  const CapacityRollup after_rollup = model_rollup(after, FitContext{});
  diff.area_delta = subtract(before_rollup.area, after_rollup.area);
  diff.unit_delta = subtract(before_rollup.units, after_rollup.units);

  add_transition_explanations(diff);
  return diff;
}

}  // namespace internal
}  // namespace dccp::space_capacity
