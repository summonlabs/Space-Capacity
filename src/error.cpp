// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Stable typed error model: name tables, category and validation-stage maps,
// retryability, deterministic rendering and the bounded explanation set. Every
// mapping in this file is total and locale independent: no floating point, no
// std::locale, and no formatting that depends on the ambient C locale.

#include "dccp/space_capacity/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/space_capacity/limits.hpp"

namespace dccp::space_capacity {
namespace {

// ---------------------------------------------------------------------------
// Name tables.
//
// Each enumerator appears exactly once, spelled exactly like the enumerator.
// Both directions - lookup by code and parse by name - read the same table, so
// the two can never disagree and a name can never be produced twice.
// ---------------------------------------------------------------------------

struct ErrorCodeName final {
  ErrorCode code;
  std::string_view name;
};

constexpr ErrorCodeName kErrorCodeNames[] = {
    {ErrorCode::ok, "ok"},

    // 1xx: structurally unacceptable argument.
    {ErrorCode::invalid_argument, "invalid_argument"},
    {ErrorCode::malformed_identity, "malformed_identity"},
    {ErrorCode::identity_too_long, "identity_too_long"},
    {ErrorCode::invalid_character, "invalid_character"},
    {ErrorCode::unknown_enum_token, "unknown_enum_token"},
    {ErrorCode::invalid_range, "invalid_range"},
    {ErrorCode::empty_value, "empty_value"},
    {ErrorCode::invalid_utf8, "invalid_utf8"},
    {ErrorCode::invalid_timestamp, "invalid_timestamp"},
    {ErrorCode::invalid_units, "invalid_units"},
    {ErrorCode::invalid_extent, "invalid_extent"},
    {ErrorCode::unsupported, "unsupported"},
    {ErrorCode::text_too_long, "text_too_long"},
    {ErrorCode::malformed_reference, "malformed_reference"},
    {ErrorCode::count_mismatch, "count_mismatch"},

    // 2xx: identity, containment and structure.
    {ErrorCode::duplicate_identity, "duplicate_identity"},
    {ErrorCode::not_found, "not_found"},
    {ErrorCode::already_exists, "already_exists"},
    {ErrorCode::identity_conflict, "identity_conflict"},
    {ErrorCode::invalid_kind_for_parent, "invalid_kind_for_parent"},
    {ErrorCode::kind_must_not_have_parent, "kind_must_not_have_parent"},
    {ErrorCode::kind_must_have_parent, "kind_must_have_parent"},
    {ErrorCode::containment_cycle, "containment_cycle"},
    {ErrorCode::depth_exceeded, "depth_exceeded"},
    {ErrorCode::node_has_children, "node_has_children"},
    {ErrorCode::invalid_spatial_class, "invalid_spatial_class"},
    {ErrorCode::invalid_placement, "invalid_placement"},
    {ErrorCode::plane_not_declared, "plane_not_declared"},
    {ErrorCode::envelope_not_declared, "envelope_not_declared"},
    {ErrorCode::lineage_broken, "lineage_broken"},
    {ErrorCode::replacement_cycle, "replacement_cycle"},
    {ErrorCode::self_reference, "self_reference"},

    // 3xx: authority.
    {ErrorCode::stale_generation, "stale_generation"},
    {ErrorCode::stale_revision, "stale_revision"},
    {ErrorCode::stale_authority, "stale_authority"},
    {ErrorCode::stale_incarnation, "stale_incarnation"},
    {ErrorCode::fencing_rejected, "fencing_rejected"},
    {ErrorCode::authority_missing, "authority_missing"},
    {ErrorCode::operation_id_conflict, "operation_id_conflict"},
    {ErrorCode::writer_lock_held, "writer_lock_held"},
    {ErrorCode::writer_lock_invalid, "writer_lock_invalid"},
    {ErrorCode::read_only_store, "read_only_store"},
    {ErrorCode::stale_sequence, "stale_sequence"},

    // 4xx: lifecycle and state conflict.
    {ErrorCode::lifecycle_transition_illegal, "lifecycle_transition_illegal"},
    {ErrorCode::lifecycle_mutation_forbidden, "lifecycle_mutation_forbidden"},
    {ErrorCode::claim_transition_illegal, "claim_transition_illegal"},
    {ErrorCode::conflict, "conflict"},
    {ErrorCode::overlap, "overlap"},
    {ErrorCode::incompatible_kind, "incompatible_kind"},
    {ErrorCode::clearance_violation, "clearance_violation"},
    {ErrorCode::exclusion_violation, "exclusion_violation"},
    {ErrorCode::reservation_not_held, "reservation_not_held"},
    {ErrorCode::node_not_available, "node_not_available"},
    {ErrorCode::hierarchy_incompatible, "hierarchy_incompatible"},
    {ErrorCode::occupant_incompatible, "occupant_incompatible"},
    {ErrorCode::scope_kind_mismatch, "scope_kind_mismatch"},
    {ErrorCode::reservation_authority_missing, "reservation_authority_missing"},
    {ErrorCode::record_retired, "record_retired"},

    // 5xx: capacity and extent arithmetic.
    {ErrorCode::capacity_exceeded, "capacity_exceeded"},
    {ErrorCode::extent_out_of_envelope, "extent_out_of_envelope"},
    {ErrorCode::unit_envelope_exceeded, "unit_envelope_exceeded"},
    {ErrorCode::extent_overlap, "extent_overlap"},
    {ErrorCode::insufficient_free_space, "insufficient_free_space"},
    {ErrorCode::fit_search_budget_exceeded, "fit_search_budget_exceeded"},
    {ErrorCode::arithmetic_overflow, "arithmetic_overflow"},
    {ErrorCode::plane_mismatch, "plane_mismatch"},

    // 6xx: hard limits.
    {ErrorCode::limit_exceeded, "limit_exceeded"},
    {ErrorCode::too_many_records, "too_many_records"},
    {ErrorCode::state_too_large, "state_too_large"},

    // 7xx: persistence.
    {ErrorCode::io_failure, "io_failure"},
    {ErrorCode::permission_denied, "permission_denied"},
    {ErrorCode::corruption, "corruption"},
    {ErrorCode::integrity_failure, "integrity_failure"},
    {ErrorCode::truncated_input, "truncated_input"},
    {ErrorCode::oversized, "oversized"},
    {ErrorCode::incompatible_version, "incompatible_version"},
    {ErrorCode::wrong_endian, "wrong_endian"},
    {ErrorCode::wrong_store, "wrong_store"},
    {ErrorCode::path_rejected, "path_rejected"},
    {ErrorCode::not_open, "not_open"},
    {ErrorCode::already_open, "already_open"},
    {ErrorCode::recovery_required, "recovery_required"},
    {ErrorCode::recovery_unavailable, "recovery_unavailable"},
    {ErrorCode::lock_conflict, "lock_conflict"},
    {ErrorCode::no_authoritative_state, "no_authoritative_state"},
    {ErrorCode::unsupported_coordinate_model, "unsupported_coordinate_model"},
    {ErrorCode::layout_invalid, "layout_invalid"},

    // 8xx: neither success nor a plain failure.
    {ErrorCode::indeterminate, "indeterminate"},
    {ErrorCode::unavailable, "unavailable"},
    {ErrorCode::unknown_state, "unknown_state"},
    {ErrorCode::cancelled, "cancelled"},

    // 9xx: internal.
    {ErrorCode::invariant_violation, "invariant_violation"},
    {ErrorCode::internal_error, "internal_error"},
    {ErrorCode::no_op_mutation, "no_op_mutation"},
};

struct ReasonCodeName final {
  ReasonCode code;
  std::string_view name;
};

constexpr ReasonCodeName kReasonCodeNames[] = {
    {ReasonCode::none, "none"},

    // Fit outcomes.
    {ReasonCode::fits_contiguous, "fits_contiguous"},
    {ReasonCode::no_contiguous_run, "no_contiguous_run"},
    {ReasonCode::insufficient_free_area, "insufficient_free_area"},
    {ReasonCode::excluded_by_region, "excluded_by_region"},
    {ReasonCode::blocked_by_clearance, "blocked_by_clearance"},
    {ReasonCode::blocked_by_incompatibility, "blocked_by_incompatibility"},
    {ReasonCode::earmarked_by_expansion, "earmarked_by_expansion"},
    {ReasonCode::envelope_too_small, "envelope_too_small"},
    {ReasonCode::node_not_available, "node_not_available"},
    {ReasonCode::node_retired, "node_retired"},
    {ReasonCode::node_replaced, "node_replaced"},
    {ReasonCode::unknown_capacity, "unknown_capacity"},
    {ReasonCode::unavailable_evidence, "unavailable_evidence"},
    {ReasonCode::candidate_search_exhausted, "candidate_search_exhausted"},
    {ReasonCode::plane_not_declared, "plane_not_declared"},
    {ReasonCode::rack_envelope_not_declared, "rack_envelope_not_declared"},
    {ReasonCode::counted_pending, "counted_pending"},

    // Accounting outcomes.
    {ReasonCode::rolled_up, "rolled_up"},
    {ReasonCode::reserved_footprint_subtracted, "reserved_footprint_subtracted"},
    {ReasonCode::committed_occupancy_subtracted, "committed_occupancy_subtracted"},
    {ReasonCode::planned_footprint_excluded, "planned_footprint_excluded"},
    {ReasonCode::structural_placement_subtracted, "structural_placement_subtracted"},
    {ReasonCode::clearance_not_enforceable, "clearance_not_enforceable"},
    {ReasonCode::over_committed, "over_committed"},
    {ReasonCode::fragmented_free_space, "fragmented_free_space"},

    // Authority outcomes.
    {ReasonCode::stale_generation_refused, "stale_generation_refused"},
    {ReasonCode::stale_revision_refused, "stale_revision_refused"},
    {ReasonCode::stale_observation_refused, "stale_observation_refused"},
    {ReasonCode::recovered_evidence_not_fresh, "recovered_evidence_not_fresh"},
    {ReasonCode::fenced_by_other_writer, "fenced_by_other_writer"},
    {ReasonCode::reservation_without_authority, "reservation_without_authority"},
    {ReasonCode::idempotent_replay, "idempotent_replay"},

    // Persistence outcomes.
    {ReasonCode::store_published, "store_published"},
    {ReasonCode::store_recovered, "store_recovered"},
    {ReasonCode::store_created, "store_created"},
    {ReasonCode::store_rejected_corrupt, "store_rejected_corrupt"},
    {ReasonCode::store_rejected_version, "store_rejected_version"},
    {ReasonCode::store_rejected_identity, "store_rejected_identity"},
    {ReasonCode::store_rejected_endian, "store_rejected_endian"},
    {ReasonCode::store_rejected_oversized, "store_rejected_oversized"},
    {ReasonCode::store_previous_retained, "store_previous_retained"},

    // Model outcomes.
    {ReasonCode::lineage_preserved, "lineage_preserved"},
    {ReasonCode::lineage_broken, "lineage_broken"},
    {ReasonCode::double_count_prevented, "double_count_prevented"},
    {ReasonCode::containment_refused, "containment_refused"},
    {ReasonCode::plane_subdivision, "plane_subdivision"},
    {ReasonCode::unenforced_clearance_reported, "unenforced_clearance_reported"},
};

// The category names are indexed by the enumerator value, which the header
// declares contiguously from ok = 0 to internal = 9.
constexpr std::string_view kErrorCategoryNames[] = {
    "ok",       "argument", "structure",   "authority", "lifecycle",
    "capacity", "limit",    "persistence", "uncertainty", "internal",
};

static_assert(static_cast<std::uint8_t>(ErrorCategory::ok) == 0,
              "kErrorCategoryNames assumes ErrorCategory::ok is the first enumerator");
static_assert(static_cast<std::uint8_t>(ErrorCategory::internal) == 9,
              "kErrorCategoryNames assumes ErrorCategory::internal is the tenth enumerator");
static_assert(std::size(kErrorCategoryNames) == 10,
              "kErrorCategoryNames must name every ErrorCategory enumerator exactly once");

}  // namespace

// ---------------------------------------------------------------------------
// ErrorCode names.
// ---------------------------------------------------------------------------

std::string_view error_code_name(ErrorCode code) noexcept {
  for (const ErrorCodeName& entry : kErrorCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  // Not a declared enumerator: an unnamed code is reported as unknown rather
  // than as a plausible-looking neighbour.
  return "unknown";
}

bool parse_error_code(std::string_view name, ErrorCode& out) noexcept {
  for (const ErrorCodeName& entry : kErrorCodeNames) {
    if (entry.name == name) {
      out = entry.code;
      return true;
    }
  }
  // `out` is left untouched: a failed parse never half-updates the caller.
  return false;
}

// ---------------------------------------------------------------------------
// ErrorCategory.
// ---------------------------------------------------------------------------

ErrorCategory error_category(ErrorCode code) noexcept {
  const std::uint32_t value = static_cast<std::uint16_t>(code);
  if (value == 0u) {
    return ErrorCategory::ok;
  }
  // Grouping is by hundreds: the leading digit of the code is the class of
  // failure, so a new code inherits its category from the block it is added to.
  switch (value / 100u) {
    case 1u:
      return ErrorCategory::argument;
    case 2u:
      return ErrorCategory::structure;
    case 3u:
      return ErrorCategory::authority;
    case 4u:
      return ErrorCategory::lifecycle;
    case 5u:
      return ErrorCategory::capacity;
    case 6u:
      return ErrorCategory::limit;
    case 7u:
      return ErrorCategory::persistence;
    case 8u:
      return ErrorCategory::uncertainty;
    case 9u:
      return ErrorCategory::internal;
    default:
      // 1..99 and anything above 999 are not declared codes; internal is the
      // category that claims no external meaning.
      return ErrorCategory::internal;
  }
}

std::string_view error_category_name(ErrorCategory category) noexcept {
  const std::size_t index = static_cast<std::uint8_t>(category);
  if (index < std::size(kErrorCategoryNames)) {
    return kErrorCategoryNames[index];
  }
  return "unknown";
}

std::uint16_t error_code_value(ErrorCode code) noexcept {
  return static_cast<std::uint16_t>(code);
}

// ---------------------------------------------------------------------------
// Retryability.
//
// A code is retryable when re-reading current state could plausibly change the
// answer: the caller lost a race, the store was busy or not yet ready, or the
// request was already applied. A code that describes the request itself - a
// malformed identity, a missing record, a version the store cannot read - will
// fail identically on every retry and is not retryable.
// ---------------------------------------------------------------------------

bool error_code_is_retryable(ErrorCode code) noexcept {
  switch (code) {
    // Authority that moved under the caller: a fresh read may succeed.
    case ErrorCode::stale_generation:
    case ErrorCode::stale_revision:
    case ErrorCode::stale_authority:
    case ErrorCode::stale_incarnation:
    case ErrorCode::fencing_rejected:
    case ErrorCode::stale_sequence:

    // Contention: another writer holds the store or the record.
    case ErrorCode::writer_lock_held:
    case ErrorCode::lock_conflict:

    // A conflicting or overlapping occupant may be gone on the next attempt.
    case ErrorCode::conflict:
    case ErrorCode::overlap:

    // Occupancy and free space are observations, not permanent properties.
    case ErrorCode::capacity_exceeded:
    case ErrorCode::insufficient_free_space:

    // The outcome is not yet known, or the durable step is temporarily unable.
    case ErrorCode::indeterminate:
    case ErrorCode::unavailable:
    case ErrorCode::recovery_required:

    // Idempotent replay: the caller's own work is already durable.
    case ErrorCode::no_op_mutation:
      return true;

    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Validation stages.
// ---------------------------------------------------------------------------

std::string_view validation_stage_name(ValidationStage stage) noexcept {
  switch (stage) {
    case ValidationStage::argument_shape:
      return "argument_shape";
    case ValidationStage::precondition_authority:
      return "precondition_authority";
    case ValidationStage::referential:
      return "referential";
    case ValidationStage::compatibility:
      return "compatibility";
    case ValidationStage::capacity:
      return "capacity";
    case ValidationStage::persistence:
      return "persistence";
    case ValidationStage::count:
      return "count";
    default:
      return "unknown";
  }
}

ValidationStage validation_stage_of(ErrorCode code) noexcept {
  // Exceptions to the numeric-group rule. Each one is a code whose number sits
  // in one block but whose failure is detected at another stage.
  switch (code) {
    // Success has no failing stage; it is reported with the first stage so that
    // "no refusal" and "input was checked" are not two different answers.
    case ErrorCode::ok:
      return ValidationStage::argument_shape;

    // Existence questions: the subject must be there (or must not be there).
    case ErrorCode::duplicate_identity:
    case ErrorCode::not_found:
    case ErrorCode::already_exists:
    case ErrorCode::identity_conflict:
      return ValidationStage::referential;

    // Authority-shaped codes that can only fail at the durable step: the file
    // lock, the caller's write token and the read-only medium are properties of
    // the store, not of the request.
    case ErrorCode::writer_lock_held:
    case ErrorCode::writer_lock_invalid:
    case ErrorCode::read_only_store:
      return ValidationStage::persistence;

    default:
      break;
  }

  // Primary rule: the stage is the hundreds group of the code.
  const std::uint32_t value = static_cast<std::uint16_t>(code);
  if (value < 200u) {  // 0..199, including the unassigned 1..99
    return ValidationStage::argument_shape;
  }
  if (value < 300u) {  // 200..299 minus the referential codes handled above
    return ValidationStage::compatibility;
  }
  if (value < 400u) {  // 300..399 minus the persistence codes handled above
    return ValidationStage::precondition_authority;
  }
  if (value < 500u) {  // 400..499
    return ValidationStage::compatibility;
  }
  if (value < 700u) {  // 500..599 and 600..699: bounds and hard limits
    return ValidationStage::capacity;
  }
  if (value < 800u) {  // 700..799
    return ValidationStage::persistence;
  }
  // 800..899 and 900..999, plus values that are not declared codes at all: the
  // capacity stage is the one that owns outcomes the caller cannot act on. This
  // makes the function total for every std::uint16_t bit pattern.
  return ValidationStage::capacity;
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------

std::string Error::to_string() const {
  const std::string_view name = error_code_name(code);

  std::string out;
  out.append(name.data(), name.size());
  out.append(": ");
  if (message.empty()) {
    // An unnamed refusal still says what failed, so the line is never blank.
    out.append(name.data(), name.size());
  } else {
    out.append(message);
  }
  if (!subject.empty()) {
    out.append(" [subject=");
    out.append(subject);
    out.push_back(']');
  }
  if (has_numbers) {
    out.append(" [expected=");
    out.append(std::to_string(expected));
    out.append(" actual=");
    out.append(std::to_string(actual));
    out.push_back(']');
  }
  return out;
}

std::string Explanation::to_string() const {
  const std::string_view name = reason_code_name(code);

  std::string out;
  out.append(name.data(), name.size());
  if (!detail.empty()) {
    out.append(": ");
    out.append(detail);
  }
  if (!subject.empty()) {
    out.append(" [subject=");
    out.append(subject);
    out.push_back(']');
  }
  return out;
}

// ---------------------------------------------------------------------------
// ReasonCode names.
// ---------------------------------------------------------------------------

std::string_view reason_code_name(ReasonCode code) noexcept {
  for (const ReasonCodeName& entry : kReasonCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown";
}

bool parse_reason_code(std::string_view name, ReasonCode& out) noexcept {
  for (const ReasonCodeName& entry : kReasonCodeNames) {
    if (entry.name == name) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// ExplanationSet.
// ---------------------------------------------------------------------------

void ExplanationSet::add(ReasonCode code, std::string subject, std::string detail) {
  // "none" is the absence of an explanation and is never stored, so it cannot
  // consume one of the bounded slots.
  if (code == ReasonCode::none) {
    return;
  }

  Explanation incoming;
  incoming.code = code;
  incoming.subject = std::move(subject);
  incoming.detail = std::move(detail);

  const std::size_t cap = static_cast<std::size_t>(Limits::kMaxExplanationsPerResult);

  // Deduplicate before anything else: an explanation that is already present
  // changes nothing, so it must not consume capacity or displace another entry.
  const auto position = std::lower_bound(items_.begin(), items_.end(), incoming);
  if (position != items_.end() && *position == incoming) {
    return;
  }

  if (items_.size() < cap) {
    items_.insert(position, std::move(incoming));
    return;
  }

  // At capacity. Policy, chosen so that the final contents depend only on the
  // set of explanations offered and never on the order they arrived in:
  //
  //   * the set keeps the `cap` smallest explanations under operator<;
  //   * an incoming explanation that sorts at or after the current largest is
  //     dropped, and the set is marked truncated;
  //   * an incoming explanation that sorts before the current largest takes the
  //     largest one's place, is inserted at its sorted position so the vector
  //     stays sorted, and the set is marked truncated because the largest
  //     explanation it was asked to hold is gone.
  //
  // `truncated_` therefore means: something this set was asked to record is not
  // present, and the caller must treat the list as incomplete.
  truncated_ = true;
  if (items_.empty() || !(incoming < items_.back())) {
    return;
  }
  items_.pop_back();
  items_.insert(std::lower_bound(items_.begin(), items_.end(), incoming), std::move(incoming));
}

void ExplanationSet::add(const Explanation& explanation) {
  add(explanation.code, explanation.subject, explanation.detail);
}

bool ExplanationSet::contains(ReasonCode code) const noexcept {
  for (const Explanation& item : items_) {
    if (item.code == code) {
      return true;
    }
  }
  return false;
}

void ExplanationSet::clear() noexcept {
  items_.clear();
  truncated_ = false;
}

}  // namespace dccp::space_capacity
