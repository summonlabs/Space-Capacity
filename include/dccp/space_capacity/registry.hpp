// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the registry: the public entry point of the library.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A registry is either durable, in which case every accepted mutation has been
// published to a store before it becomes visible, or in memory, in which case
// nothing is written anywhere and the registry says so.
//
// ---------------------------------------------------------------------------
// Concurrency and lock order
// ---------------------------------------------------------------------------
//
// Two locks exist in process:
//
//   L1  commit_mutex_   serializes writers. Held for a whole mutation.
//   L2  state_mutex_    a shared_mutex over the published snapshot pointer and
//                       the retained history. Taken shared to read the
//                       pointer, exclusive only for the pointer swap.
//
// The lock order is L1 then L2, and it is never reversed. Readers take L2
// shared, copy the shared_ptr and release immediately, so a report, a diff or
// a fit search runs entirely on an immutable snapshot and blocks nobody.
//
// A durable commit additionally takes the operating-system file lock (L3)
// while holding L1 and NOT holding L2, so the order is L1 then L3, and L3 is
// only ever taken on that path. No lock is held while a callback runs, because
// the library invokes no caller-supplied code at all: the only hook is the
// store's fault-injection hook, which exists for tests, runs inside the
// commit path and must not re-enter the registry.
//
// ---------------------------------------------------------------------------
// Publication order
// ---------------------------------------------------------------------------
//
// A mutation builds a candidate snapshot, audits it, hands it to the store,
// and only publishes it in memory after the store has published it on disk and
// verified the visible bytes. A failed durable step therefore leaves memory
// unchanged as well as disk, so the two never disagree.

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/diff.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/observation.hpp"
#include "dccp/space_capacity/query.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/requests.hpp"
#include "dccp/space_capacity/snapshot.hpp"
#include "dccp/space_capacity/store.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

// How many recent revisions stay addressable for diff queries. Bounded, like
// every other retained collection.
inline constexpr std::uint32_t kRetainedRevisions = 8;

// How many recent request keys stay addressable for idempotent replay. Bounded
// and explicit: a request key older than this window is applied again, which
// the conflict checks then refuse rather than double-applying.
struct SC_API MutationRecord final {
  RequestId key{};
  // The revision the request produced. A retry replays when the registry stands
  // at that revision, and is a conflict when it stands anywhere else.
  RegistryRevision applied_at{};
  MutationOutcome outcome{};
};

class SC_API SpaceCapacityRegistry final {
 public:
  SpaceCapacityRegistry() = default;
  ~SpaceCapacityRegistry();

  SpaceCapacityRegistry(const SpaceCapacityRegistry&) = delete;
  SpaceCapacityRegistry& operator=(const SpaceCapacityRegistry&) = delete;
  SpaceCapacityRegistry(SpaceCapacityRegistry&&) noexcept;
  SpaceCapacityRegistry& operator=(SpaceCapacityRegistry&&) noexcept;

  // Opens a durable registry. The state is loaded, integrity-checked, decoded
  // and audited exactly as the store's own open path does it.
  [[nodiscard]] static Result<SpaceCapacityRegistry> open(const StoreOptions& options);

  // An in-memory registry. Nothing is written and nothing is recoverable; the
  // registry reports `durable() == false` so a caller can never mistake it for
  // a durable one.
  [[nodiscard]] static Result<SpaceCapacityRegistry> create_in_memory(StoreId store);

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] bool durable() const noexcept { return store_ != nullptr; }

  [[nodiscard]] Status close();

  // ------------------------------------------------------------- observation
  // The published snapshot. The shared_ptr keeps the state alive for as long
  // as the caller holds it, with no lock held.
  [[nodiscard]] SnapshotPtr snapshot() const;
  [[nodiscard]] RegistryRevision revision() const;
  [[nodiscard]] StoreIncarnation incarnation() const;
  [[nodiscard]] AttemptId attempt() const;
  [[nodiscard]] const StoreId& store_id() const;

  // The persisted attempt sequence the store had reached when this process
  // opened it. Evidence observed at or below this is recovered, not fresh.
  [[nodiscard]] AttemptId fresh_floor() const;
  [[nodiscard]] Freshness evidence_freshness(const EvidenceRef& evidence) const;

  // ---------------------------------------------------------------- mutations
  [[nodiscard]] Result<MutationOutcome> apply(const CreateNodeRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetNodeMetadataRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetNodeEnvelopeRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetNodePlacementRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetNodeLifecycleRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetNodeReferencesRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const ReparentNodeRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const RetireNodeRequest& request);

  [[nodiscard]] Result<MutationOutcome> apply(const CreateClaimRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const TransitionClaimRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const SetClaimDetailsRequest& request);

  [[nodiscard]] Result<MutationOutcome> apply(const CreateReservationRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const TransitionReservationRequest& request);

  [[nodiscard]] Result<MutationOutcome> apply(const CreateExclusionRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const TransitionExclusionRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const CreateClearanceRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const CreateExpansionZoneRequest& request);
  [[nodiscard]] Result<MutationOutcome> apply(const TransitionExpansionZoneRequest& request);

  // ------------------------------------------------------------------ queries
  [[nodiscard]] Result<CapacityReport> report(const SpaceNodeId& node,
                                              const FitContext& context) const;
  [[nodiscard]] Result<CapacityRollup> rollup(const SpaceNodeId& node,
                                              const FitContext& context) const;
  [[nodiscard]] CapacityRollup model_rollup(const FitContext& context) const;

  [[nodiscard]] Result<FitAssessment> assess(const RackUnitFitRequest& request) const;
  [[nodiscard]] Result<FitAssessment> assess(const PlanarRectFitRequest& request) const;
  [[nodiscard]] Result<FitAssessment> assess(const PlanarAreaFitRequest& request) const;

  // --------------------------------------------------------------- revalidate
  [[nodiscard]] Result<Revalidation> revalidate(const ObservationToken& token) const;

  // ------------------------------------------------------------------- diffs
  // Diffs the retained revision against the current one, or two retained
  // revisions. A revision that is no longer retained is refused with
  // stale_revision rather than approximated.
  [[nodiscard]] Result<CapacityDiff> diff(RegistryRevision from, RegistryRevision to) const;
  [[nodiscard]] static CapacityDiff diff(const Snapshot& before, const Snapshot& after);

  // ---------------------------------------------------------------- integrity
  [[nodiscard]] Status audit() const;
  [[nodiscard]] Status verify() const;
  [[nodiscard]] Result<StoreStatus> store_status() const;

  // ------------------------------------------------------- writer authority
  // These exist so that process-level writer authority can be exercised and
  // proved from an independent process. They are not needed for ordinary use:
  // every commit takes the lock itself.
  [[nodiscard]] Status acquire_writer_lease();
  [[nodiscard]] Status release_writer_lease();
  [[nodiscard]] bool holds_writer_lease() const;

 private:
  [[nodiscard]] Result<MutationOutcome> publish(SnapshotPtr candidate, SpaceNodeId subject,
                                                ExplanationSet explanations);
  [[nodiscard]] SnapshotPtr current_snapshot_locked() const;
  [[nodiscard]] Result<std::optional<MutationOutcome>> lookup_replay(const RequestId& key,
                                                                    RegistryRevision base) const;
  void remember_replay(const RequestId& key, const MutationOutcome& outcome);

  mutable std::shared_mutex state_mutex_{};
  std::mutex commit_mutex_{};
  std::unique_ptr<Store> store_{};
  SnapshotPtr snapshot_{};
  std::deque<SnapshotPtr> history_{};
  std::deque<MutationRecord> idempotency_{};
  StoreId store_id_{};
  StoreIncarnation incarnation_{};
  AttemptId fresh_floor_{};
  bool open_ = false;
  bool durable_ = false;
};

}  // namespace dccp::space_capacity
