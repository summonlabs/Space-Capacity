// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - stable typed error model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every fallible entry point returns Result<T>. The code is the stable,
// machine-readable outcome; the message is a stable human explanation. Codes
// are grouped by the stage of validation that owns them and are never reused
// for a different meaning. Distinct conditions keep distinct codes:
//
//   * "unknown" is not "zero"                    (unknown_state vs a value)
//   * "unsupported" is not "unavailable"         (unsupported vs unavailable)
//   * "stale generation" is not "conflict"       (300 vs 403)
//   * "not found" is not "already exists"        (201 vs 202)
//   * capacity is not authority to consume it    (500 vs 302)

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "dccp/space_capacity/export.hpp"

namespace dccp::space_capacity {

enum class ErrorCode : std::uint16_t {
  ok = 0,

  // --- 1xx: the caller supplied something structurally unacceptable --------
  invalid_argument = 100,
  malformed_identity = 101,
  identity_too_long = 102,
  invalid_character = 103,
  unknown_enum_token = 104,
  invalid_range = 105,
  empty_value = 106,
  invalid_utf8 = 107,
  invalid_timestamp = 108,
  invalid_units = 109,
  invalid_extent = 110,
  unsupported = 111,
  text_too_long = 112,
  malformed_reference = 113,
  count_mismatch = 114,

  // --- 2xx: identity, containment and structure ----------------------------
  duplicate_identity = 200,
  not_found = 201,
  already_exists = 202,
  identity_conflict = 203,
  invalid_kind_for_parent = 204,
  kind_must_not_have_parent = 205,
  kind_must_have_parent = 206,
  containment_cycle = 207,
  depth_exceeded = 208,
  node_has_children = 209,
  invalid_spatial_class = 210,
  invalid_placement = 211,
  plane_not_declared = 212,
  envelope_not_declared = 213,
  lineage_broken = 214,
  replacement_cycle = 215,
  self_reference = 216,

  // --- 3xx: authority ------------------------------------------------------
  stale_generation = 300,
  stale_revision = 301,
  stale_authority = 302,
  stale_incarnation = 303,
  fencing_rejected = 304,
  authority_missing = 305,
  operation_id_conflict = 306,
  writer_lock_held = 307,
  writer_lock_invalid = 308,
  read_only_store = 309,
  stale_sequence = 310,

  // --- 4xx: lifecycle and state conflict -----------------------------------
  lifecycle_transition_illegal = 400,
  lifecycle_mutation_forbidden = 401,
  claim_transition_illegal = 402,
  conflict = 403,
  overlap = 404,
  incompatible_kind = 405,
  clearance_violation = 406,
  exclusion_violation = 407,
  reservation_not_held = 408,
  node_not_available = 409,
  hierarchy_incompatible = 410,
  occupant_incompatible = 411,
  scope_kind_mismatch = 412,
  reservation_authority_missing = 413,
  record_retired = 414,

  // --- 5xx: capacity -------------------------------------------------------
  capacity_exceeded = 500,
  extent_out_of_envelope = 501,
  unit_envelope_exceeded = 502,
  extent_overlap = 503,
  insufficient_free_space = 504,
  fit_search_budget_exceeded = 505,
  arithmetic_overflow = 506,
  plane_mismatch = 507,

  // --- 6xx: limits ---------------------------------------------------------
  limit_exceeded = 600,
  too_many_records = 601,
  state_too_large = 602,

  // --- 7xx: persistence ----------------------------------------------------
  io_failure = 700,
  permission_denied = 701,
  corruption = 702,
  integrity_failure = 703,
  truncated_input = 704,
  oversized = 705,
  incompatible_version = 706,
  wrong_endian = 707,
  wrong_store = 708,
  path_rejected = 709,
  not_open = 710,
  already_open = 711,
  recovery_required = 712,
  recovery_unavailable = 713,
  lock_conflict = 714,
  no_authoritative_state = 715,
  unsupported_coordinate_model = 716,
  layout_invalid = 717,

  // --- 8xx: outcomes that are neither success nor a plain failure ----------
  indeterminate = 800,
  unavailable = 801,
  unknown_state = 802,
  cancelled = 803,

  // --- 9xx: internal -------------------------------------------------------
  invariant_violation = 900,
  internal_error = 901,
  no_op_mutation = 902,
};

// Coarse grouping of a code, for callers that act on classes of failure.
enum class ErrorCategory : std::uint8_t {
  ok = 0,
  argument = 1,
  structure = 2,
  authority = 3,
  lifecycle = 4,
  capacity = 5,
  limit = 6,
  persistence = 7,
  uncertainty = 8,
  internal = 9,
};

SC_API std::string_view error_code_name(ErrorCode code) noexcept;
SC_API bool parse_error_code(std::string_view name, ErrorCode& out) noexcept;
SC_API ErrorCategory error_category(ErrorCode code) noexcept;
SC_API std::string_view error_category_name(ErrorCategory category) noexcept;
SC_API std::uint16_t error_code_value(ErrorCode code) noexcept;

// Whether the condition could plausibly succeed on a retry after the caller
// revalidates against current state. A hint about the caller's next step, not
// a promise.
SC_API bool error_code_is_retryable(ErrorCode code) noexcept;

// The stage of validation a code belongs to. Stages are applied in ascending
// order and the first failing stage decides the reported code.
enum class ValidationStage : std::uint32_t {
  argument_shape = 0,           // malformed input: identities, units, sizes
  precondition_authority = 1,   // generations, revisions, epochs, tokens
  referential = 2,              // the subjects the request names must exist
  compatibility = 3,            // kind, hierarchy, lifecycle and conflicts
  capacity = 4,                 // bounds, occupancy and extent arithmetic
  persistence = 5,              // the durable step
  count = 6,
};

SC_API std::string_view validation_stage_name(ValidationStage stage) noexcept;
SC_API ValidationStage validation_stage_of(ErrorCode code) noexcept;

// One refusal or decision.
struct SC_API Error final {
  ErrorCode code = ErrorCode::ok;
  std::string message;
  // Optional structured context. Never populated with unvalidated external
  // text beyond what the caller itself supplied.
  std::string subject;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  bool has_numbers = false;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

  [[nodiscard]] static Error make(ErrorCode c, std::string msg) { return Error(c, std::move(msg)); }

  [[nodiscard]] static Error with_subject(ErrorCode c, std::string msg, std::string subject_) {
    Error e(c, std::move(msg));
    e.subject = std::move(subject_);
    return e;
  }

  [[nodiscard]] static Error stale(ErrorCode c, std::string msg, std::string subject_,
                                   std::uint64_t expected_, std::uint64_t actual_) {
    Error e(c, std::move(msg));
    e.subject = std::move(subject_);
    e.expected = expected_;
    e.actual = actual_;
    e.has_numbers = true;
    return e;
  }

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::ok; }
  [[nodiscard]] ErrorCategory category() const noexcept { return error_category(code); }

  // Deterministic one-line rendering:
  //   "code: message [subject=S] [expected=E actual=A]"
  // with each bracket group present only when its fields are set. Never locale
  // dependent.
  [[nodiscard]] std::string to_string() const;
};

// Result<T>: either a value or an error. There is no third state and no
// partially populated value: a failed Result holds an Error and no value.
template <typename T>
class Result final {
 public:
  Result(T value) : storage_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] bool has_value() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
  [[nodiscard]] T value_or(T fallback) const { return ok() ? std::get<0>(storage_) : std::move(fallback); }

  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }
  // Never throws, even on a successful Result: a caller that asks only for the
  // code of a success gets `ok` rather than an exception crossing a noexcept
  // boundary.
  [[nodiscard]] ErrorCode code() const noexcept {
    return ok() ? ErrorCode::ok : std::get<1>(storage_).code;
  }

  [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }
  [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }
  [[nodiscard]] const T& operator*() const& { return std::get<0>(storage_); }
  [[nodiscard]] T& operator*() & { return std::get<0>(storage_); }

 private:
  std::variant<T, Error> storage_;
};

// Result<void> equivalent.
class SC_API Status final {
 public:
  Status() = default;                                // ok
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Status success() { return Status(); }
  [[nodiscard]] static Status failure(ErrorCode code, std::string message) {
    return Status(Error(code, std::move(message)));
  }
  // A failure that names the record it is about, so an operator can tell which
  // node, claim or region was refused without parsing the message.
  [[nodiscard]] static Status with_subject(ErrorCode code, std::string message,
                                           std::string subject) {
    return Status(Error::with_subject(code, std::move(message), std::move(subject)));
  }

  [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }
  [[nodiscard]] std::string to_string() const { return error_.to_string(); }

 private:
  Error error_{};
};

// Interned explanation of a refusal or of a decision. Explanation codes are
// stable and machine-readable, and the text is derived only from the code and
// the structured arguments, so the same input yields byte-identical output.
enum class ReasonCode : std::uint16_t {
  none = 0,

  // fit outcomes
  fits_contiguous = 1,
  no_contiguous_run = 2,
  insufficient_free_area = 3,
  excluded_by_region = 4,
  blocked_by_clearance = 5,
  blocked_by_incompatibility = 6,
  earmarked_by_expansion = 7,
  envelope_too_small = 8,
  node_not_available = 9,
  node_retired = 10,
  node_replaced = 11,
  unknown_capacity = 12,
  unavailable_evidence = 13,
  candidate_search_exhausted = 14,
  plane_not_declared = 15,
  rack_envelope_not_declared = 16,
  counted_pending = 17,

  // accounting outcomes
  rolled_up = 30,
  reserved_footprint_subtracted = 31,
  committed_occupancy_subtracted = 32,
  planned_footprint_excluded = 33,
  structural_placement_subtracted = 34,
  clearance_not_enforceable = 35,
  over_committed = 36,
  fragmented_free_space = 37,

  // authority outcomes
  stale_generation_refused = 50,
  stale_revision_refused = 51,
  stale_observation_refused = 52,
  recovered_evidence_not_fresh = 53,
  fenced_by_other_writer = 54,
  reservation_without_authority = 55,
  idempotent_replay = 56,

  // persistence outcomes
  store_published = 70,
  store_recovered = 71,
  store_created = 72,
  store_rejected_corrupt = 73,
  store_rejected_version = 74,
  store_rejected_identity = 75,
  store_rejected_endian = 76,
  store_rejected_oversized = 77,
  store_previous_retained = 78,

  // model outcomes
  lineage_preserved = 90,
  lineage_broken = 91,
  double_count_prevented = 92,
  containment_refused = 93,
  plane_subdivision = 94,
  unenforced_clearance_reported = 95,
};

SC_API std::string_view reason_code_name(ReasonCode code) noexcept;
SC_API bool parse_reason_code(std::string_view name, ReasonCode& out) noexcept;

struct SC_API Explanation final {
  ReasonCode code = ReasonCode::none;
  std::string subject;  // node/claim/region identity text, empty when global
  std::string detail;   // stable, code-derived text

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] friend bool operator==(const Explanation& a, const Explanation& b) noexcept {
    return a.code == b.code && a.subject == b.subject && a.detail == b.detail;
  }
  [[nodiscard]] friend bool operator!=(const Explanation& a, const Explanation& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const Explanation& a, const Explanation& b) noexcept {
    if (a.code != b.code) return a.code < b.code;
    if (a.subject != b.subject) return a.subject < b.subject;
    return a.detail < b.detail;
  }
};

// A deterministic, deduplicated, ordered set of explanations. Ordering is by
// (code, subject, detail), so two runs over equal state produce equal lists.
// The set is bounded by kMaxExplanationsPerResult: it retains the smallest
// elements under that ordering and reports `truncated()` when something it was
// asked to record is not present, rather than growing without bound.
class SC_API ExplanationSet final {
 public:
  void add(ReasonCode code, std::string subject, std::string detail);
  void add(const Explanation& explanation);

  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] const std::vector<Explanation>& items() const noexcept { return items_; }
  [[nodiscard]] bool contains(ReasonCode code) const noexcept;
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  void clear() noexcept;

 private:
  std::vector<Explanation> items_;
  bool truncated_ = false;
};

}  // namespace dccp::space_capacity
