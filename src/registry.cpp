// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the registry.
//
// Concurrency and lock order
//
//   L1  commit_mutex_  serializes writers, held for a whole mutation
//   L2  state_mutex_   a shared_mutex over the published snapshot pointer and
//                      the retained history
//   L3  the operating-system file lock, taken inside a durable commit while L1
//       is held and L2 is NOT held
//
// The order is L1 then L2, and L1 then L3. It is never reversed: no path takes
// L2 and then L1, and no path takes L3 and then L1 or L2. Readers take L2
// shared, copy the shared_ptr and release immediately, so every report, diff
// and fit search runs on an immutable snapshot with no lock held. The library
// invokes no caller-supplied callback anywhere, so no callback can re-enter a
// held lock; the store's fault-injection hook is the one exception and is
// documented as test-only and non-re-entrant.

#include "dccp/space_capacity/registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "diff_internal.hpp"
#include "dccp/space_capacity/text.hpp"
#include "mutations.hpp"

namespace dccp::space_capacity {

SpaceCapacityRegistry::~SpaceCapacityRegistry() { (void)close(); }

SpaceCapacityRegistry::SpaceCapacityRegistry(SpaceCapacityRegistry&& other) noexcept
    : store_(std::move(other.store_)), snapshot_(std::move(other.snapshot_)),
      history_(std::move(other.history_)), store_id_(std::move(other.store_id_)),
      incarnation_(other.incarnation_), fresh_floor_(other.fresh_floor_), open_(other.open_),
      durable_(other.durable_) {
  other.open_ = false;
  other.durable_ = false;
}

SpaceCapacityRegistry& SpaceCapacityRegistry::operator=(SpaceCapacityRegistry&& other) noexcept {
  if (this != &other) {
    (void)close();
    store_ = std::move(other.store_);
    snapshot_ = std::move(other.snapshot_);
    history_ = std::move(other.history_);
    store_id_ = std::move(other.store_id_);
    incarnation_ = other.incarnation_;
    fresh_floor_ = other.fresh_floor_;
    open_ = other.open_;
    durable_ = other.durable_;
    other.open_ = false;
    other.durable_ = false;
  }
  return *this;
}

Result<SpaceCapacityRegistry> SpaceCapacityRegistry::open(const StoreOptions& options) {
  Result<Store> opened = Store::open(options);
  if (!opened) return opened.error();

  SpaceCapacityRegistry registry;
  registry.store_ = std::make_unique<Store>(std::move(opened).value());
  registry.snapshot_ = registry.store_->snapshot();
  registry.store_id_ = registry.store_->store_id();
  registry.incarnation_ = registry.store_->incarnation();
  // Every attempt at or below this sequence was made before this process
  // opened the store, so the evidence it produced is recovered, not fresh.
  registry.fresh_floor_ = registry.store_->attempt();
  registry.history_.push_back(registry.snapshot_);
  registry.open_ = true;
  registry.durable_ = true;
  return registry;
}

Result<SpaceCapacityRegistry> SpaceCapacityRegistry::create_in_memory(StoreId store) {
  if (store.empty()) {
    return Error::make(ErrorCode::malformed_identity,
                       "an in-memory registry requires a store identity");
  }
  SpaceCapacityRegistry registry;
  Snapshot::BuildInput input;
  input.store = store;
  input.incarnation = StoreIncarnation{1};
  input.revision = RegistryRevision{0};
  input.attempt = AttemptId{StoreIncarnation{1}, 0};
  input.created_at = system_utc_now();
  Result<SnapshotPtr> built = Snapshot::build(std::move(input));
  if (!built) return built.error();
  registry.snapshot_ = std::move(built).value();
  registry.store_id_ = store;
  registry.incarnation_ = StoreIncarnation{1};
  registry.fresh_floor_ = registry.snapshot_->attempt();
  registry.history_.push_back(registry.snapshot_);
  registry.open_ = true;
  registry.durable_ = false;
  return registry;
}

Status SpaceCapacityRegistry::close() {
  std::unique_lock commit_lock(commit_mutex_);
  if (store_) {
    (void)store_->close();
    store_.reset();
  }
  {
    std::unique_lock state_lock(state_mutex_);
    history_.clear();
  }
  open_ = false;
  durable_ = false;
  return Status::success();
}

SnapshotPtr SpaceCapacityRegistry::snapshot() const {
  std::shared_lock state_lock(state_mutex_);
  return snapshot_;
}

RegistryRevision SpaceCapacityRegistry::revision() const { return snapshot()->revision(); }

StoreIncarnation SpaceCapacityRegistry::incarnation() const { return incarnation_; }

AttemptId SpaceCapacityRegistry::attempt() const { return snapshot()->attempt(); }

const StoreId& SpaceCapacityRegistry::store_id() const { return store_id_; }

AttemptId SpaceCapacityRegistry::fresh_floor() const { return fresh_floor_; }

Freshness SpaceCapacityRegistry::evidence_freshness(const EvidenceRef& evidence) const {
  return classify_evidence(evidence, fresh_floor_);
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

Result<MutationOutcome> SpaceCapacityRegistry::publish(SnapshotPtr candidate, SpaceNodeId subject,
                                                       ExplanationSet explanations) {
  MutationOutcome outcome;
  outcome.subject = std::move(subject);
  outcome.explanations = std::move(explanations);
  outcome.applied = true;

  if (durable_) {
    Result<AttemptId> committed = store_->commit(candidate);
    if (!committed) return committed.error();
    outcome.attempt = committed.value();
  } else {
    outcome.attempt = candidate->attempt();
  }
  outcome.revision = candidate->revision();
  outcome.state_digest = candidate->digest();

  {
    std::unique_lock state_lock(state_mutex_);
    snapshot_ = candidate;
    history_.push_back(std::move(candidate));
    while (history_.size() > kRetainedRevisions) {
      history_.pop_front();
    }
  }
  return outcome;
}

SnapshotPtr SpaceCapacityRegistry::current_snapshot_locked() const {
  std::shared_lock state_lock(state_mutex_);
  return snapshot_;
}

Result<std::optional<MutationOutcome>> SpaceCapacityRegistry::lookup_replay(
    const RequestId& key, RegistryRevision base) const {
  if (key.empty()) return std::optional<MutationOutcome>{};
  for (const MutationRecord& entry : idempotency_) {
    if (entry.key != key) continue;
    if (entry.applied_at != base) {
      return Error::make(ErrorCode::operation_id_conflict,
                         "the request key was already used against a different revision");
    }
    MutationOutcome replay = entry.outcome;
    replay.applied = false;
    replay.explanations.add(ReasonCode::idempotent_replay, std::string{},
                            "the request key had already been applied at this revision, so the "
                            "recorded outcome was returned and nothing was applied twice");
    return std::optional<MutationOutcome>{replay};
  }
  return std::optional<MutationOutcome>{};
}

void SpaceCapacityRegistry::remember_replay(const RequestId& key,
                                            const MutationOutcome& outcome) {
  if (key.empty()) return;
  MutationRecord entry;
  entry.key = key;
  entry.applied_at = outcome.revision;
  // The whole recorded outcome, so a replay hands the caller the revision, the
  // attempt and the digest its request actually landed on rather than a
  // default-constructed outcome.
  entry.outcome = outcome;
  entry.outcome.applied = true;
  idempotency_.push_back(std::move(entry));
  while (idempotency_.size() > Limits::kMaxIdempotencyRecords) {
    idempotency_.pop_front();
  }
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

// The one place where a mutation is staged. Every overload below funnels
// through exactly this sequence:
//
//   1 take L1 so writers are serialized
//   2 take L2 shared only long enough to copy the published snapshot pointer
//   3 build a private working copy and apply the mutation to it
//   4 build the candidate snapshot, which runs the full audit
//   5 hand the candidate to the store, which publishes it and verifies the
//      bytes it actually wrote
//   6 take L2 exclusive only for the pointer swap and the history push
//
// A refusal at any step leaves the published snapshot untouched, so memory and
// disk never disagree.
#define SPACE_CAPACITY_APPLY(Type, Mutation)                                                \
  Result<MutationOutcome> SpaceCapacityRegistry::apply(const Type& request) {                \
    std::unique_lock commit_lock(commit_mutex_);                                             \
    if (!open_) {                                                                            \
      return Error::make(ErrorCode::not_open, "the registry is not open");                   \
    }                                                                                        \
    const SnapshotPtr base = current_snapshot_locked();                                      \
    const RequestId& key = internal::request_key(request);                                   \
    Result<std::optional<MutationOutcome>> replay = lookup_replay(key, base->revision());    \
    if (!replay) return replay.error();                                                      \
    if (replay.value().has_value()) return *replay.value();                                  \
    internal::WorkingModel model = internal::WorkingModel::from(*base);                      \
    Result<internal::MutationResult> applied = internal::apply_##Mutation(model, request);       \
    if (!applied) return applied.error();                                                    \
    Result<SnapshotPtr> candidate = internal::finalize(model);                               \
    if (!candidate) return candidate.error();                                                \
    Result<MutationOutcome> outcome =                                                        \
        publish(std::move(candidate).value(), applied.value().subject,                       \
                applied.value().explanations);                                               \
    if (!outcome) return outcome.error();                                                    \
    remember_replay(key, outcome.value());                                                   \
    return outcome;                                                                          \
  }

SPACE_CAPACITY_APPLY(CreateNodeRequest, create_node)
SPACE_CAPACITY_APPLY(SetNodeMetadataRequest, set_node_metadata)
SPACE_CAPACITY_APPLY(SetNodeEnvelopeRequest, set_node_envelope)
SPACE_CAPACITY_APPLY(SetNodePlacementRequest, set_node_placement)
SPACE_CAPACITY_APPLY(SetNodeLifecycleRequest, set_node_lifecycle)
SPACE_CAPACITY_APPLY(SetNodeReferencesRequest, set_node_references)
SPACE_CAPACITY_APPLY(ReparentNodeRequest, reparent_node)
SPACE_CAPACITY_APPLY(RetireNodeRequest, retire_node)
SPACE_CAPACITY_APPLY(CreateClaimRequest, create_claim)
SPACE_CAPACITY_APPLY(TransitionClaimRequest, transition_claim)
SPACE_CAPACITY_APPLY(SetClaimDetailsRequest, set_claim_details)
SPACE_CAPACITY_APPLY(CreateReservationRequest, create_reservation)
SPACE_CAPACITY_APPLY(TransitionReservationRequest, transition_reservation)
SPACE_CAPACITY_APPLY(CreateExclusionRequest, create_exclusion)
SPACE_CAPACITY_APPLY(TransitionExclusionRequest, transition_exclusion)
SPACE_CAPACITY_APPLY(CreateClearanceRequest, create_clearance)
SPACE_CAPACITY_APPLY(CreateExpansionZoneRequest, create_expansion_zone)
SPACE_CAPACITY_APPLY(TransitionExpansionZoneRequest, transition_expansion_zone)

#undef SPACE_CAPACITY_APPLY

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Result<CapacityReport> SpaceCapacityRegistry::report(const SpaceNodeId& node,
                                                     const FitContext& context) const {
  const SnapshotPtr current = snapshot();
  return current->report(node, context);
}

Result<CapacityRollup> SpaceCapacityRegistry::rollup(const SpaceNodeId& node,
                                                     const FitContext& context) const {
  const SnapshotPtr current = snapshot();
  return current->rollup(node, context);
}

CapacityRollup SpaceCapacityRegistry::model_rollup(const FitContext& context) const {
  const SnapshotPtr current = snapshot();
  return space_capacity::model_rollup(*current, context);
}

Result<FitAssessment> SpaceCapacityRegistry::assess(const RackUnitFitRequest& request) const {
  const SnapshotPtr current = snapshot();
  return current->assess(request);
}

Result<FitAssessment> SpaceCapacityRegistry::assess(const PlanarRectFitRequest& request) const {
  const SnapshotPtr current = snapshot();
  return current->assess(request);
}

Result<FitAssessment> SpaceCapacityRegistry::assess(const PlanarAreaFitRequest& request) const {
  const SnapshotPtr current = snapshot();
  return current->assess(request);
}

// ---------------------------------------------------------------------------
// Revalidation and diffs
// ---------------------------------------------------------------------------

Result<Revalidation> SpaceCapacityRegistry::revalidate(const ObservationToken& token) const {
  Revalidation result;
  const SnapshotPtr current = snapshot();
  result.expected_revision = token.revision;
  result.actual_revision = current->revision();
  result.revision_compared = true;

  const SpaceNode* subject = current->find_node(token.subject);
  if (subject == nullptr) {
    result.valid = false;
    result.freshness = Freshness::orphaned;
    result.reason = ErrorCode::not_found;
    return result;
  }
  result.expected_generation = token.subject_generation;
  result.actual_generation = subject->generation;
  result.generation_compared = true;

  if (subject->generation != token.subject_generation) {
    result.valid = false;
    result.freshness = Freshness::stale;
    result.reason = ErrorCode::stale_generation;
    return result;
  }
  if (current->revision() != token.revision) {
    result.valid = false;
    result.freshness = Freshness::stale;
    result.reason = ErrorCode::stale_revision;
    return result;
  }
  if (token.attempt.incarnation() != incarnation_) {
    result.valid = false;
    result.freshness = Freshness::stale;
    result.reason = ErrorCode::stale_incarnation;
    return result;
  }
  if (token.attempt <= fresh_floor_) {
    // The observation was taken at or before the attempt this session started
    // from, so it describes state that was recovered from disk rather than
    // state this session observed. It is recovered, not fresh.
    result.valid = false;
    result.freshness = Freshness::recovered;
    result.reason = ErrorCode::stale_authority;
    return result;
  }
  result.valid = true;
  result.freshness = Freshness::fresh;
  result.reason = ErrorCode::ok;
  return result;
}

CapacityDiff SpaceCapacityRegistry::diff(const Snapshot& before, const Snapshot& after) {
  return internal::compute_diff(before, after);
}

Result<CapacityDiff> SpaceCapacityRegistry::diff(RegistryRevision from, RegistryRevision to) const {
  SnapshotPtr before;
  SnapshotPtr after;
  {
    std::shared_lock state_lock(state_mutex_);
    for (const SnapshotPtr& candidate : history_) {
      if (candidate->revision() == from) before = candidate;
      if (candidate->revision() == to) after = candidate;
    }
    if (!after && snapshot_ != nullptr && snapshot_->revision() == to) after = snapshot_;
  }
  if (!before) {
    return Error::stale(ErrorCode::stale_revision,
                        "the earlier revision is no longer retained for diff", std::string{},
                        from.value(), revision().value());
  }
  if (!after) {
    return Error::stale(ErrorCode::stale_revision,
                        "the later revision is no longer retained for diff", std::string{},
                        to.value(), revision().value());
  }
  return internal::compute_diff(*before, *after);
}

// ---------------------------------------------------------------------------
// Integrity and writer authority
// ---------------------------------------------------------------------------

Status SpaceCapacityRegistry::audit() const {
  const SnapshotPtr current = snapshot();
  return current->audit();
}

Status SpaceCapacityRegistry::verify() const {
  if (!open_) {
    return Status::failure(ErrorCode::not_open, "the registry is not open");
  }
  if (!durable_) {
    return Status::failure(ErrorCode::unsupported,
                           "an in-memory registry has nothing durable to verify");
  }
  Status checked = store_->verify_on_disk();
  if (!checked) return checked;
  return audit();
}

Result<StoreStatus> SpaceCapacityRegistry::store_status() const {
  if (!durable_) {
    return Error::make(ErrorCode::unsupported,
                       "an in-memory registry has no durable status to report");
  }
  return store_->status();
}

Status SpaceCapacityRegistry::acquire_writer_lease() {
  if (!durable_) {
    return Status::failure(ErrorCode::unsupported,
                           "an in-memory registry has no writer authority to lease");
  }
  return store_->acquire_writer_lease();
}

Status SpaceCapacityRegistry::release_writer_lease() {
  if (!durable_) {
    return Status::failure(ErrorCode::unsupported,
                           "an in-memory registry has no writer authority to release");
  }
  return store_->release_writer_lease();
}

bool SpaceCapacityRegistry::holds_writer_lease() const {
  return durable_ && store_->holds_writer_lease();
}

}  // namespace dccp::space_capacity
