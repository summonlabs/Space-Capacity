// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal mutation application. Not installed, not part of the public API.
//
// Every mutation is applied to a private copy of the current model, validated
// in the documented stage order, and only then handed to the caller to publish.
// A refused mutation leaves the working copy untouched and the caller
// untouched with it.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/requests.hpp"
#include "dccp/space_capacity/snapshot.hpp"

namespace dccp::space_capacity::internal {

// A mutable working copy of one committed revision.
struct WorkingModel final {
  StoreId store{};
  StoreIncarnation incarnation{};
  RegistryRevision base_revision{};
  AttemptId attempt{};
  Timestamp created_at{};

  std::vector<SpaceNode> nodes;
  std::vector<OccupancyClaim> claims;
  std::vector<FootprintReservation> reservations;
  std::vector<ExclusionRegion> exclusions;
  std::vector<ClearanceConstraint> clearances;
  std::vector<ExpansionZone> zones;

  [[nodiscard]] static WorkingModel from(const Snapshot& snapshot);

  [[nodiscard]] SpaceNode* node(const SpaceNodeId& id);
  [[nodiscard]] const SpaceNode* node(const SpaceNodeId& id) const;
  [[nodiscard]] OccupancyClaim* claim(const OccupancyClaimId& id);
  [[nodiscard]] const OccupancyClaim* claim(const OccupancyClaimId& id) const;
  [[nodiscard]] FootprintReservation* reservation(const OccupancyClaimId& id);
  [[nodiscard]] const FootprintReservation* reservation(const OccupancyClaimId& id) const;
  [[nodiscard]] ExclusionRegion* exclusion(const ExclusionRegionId& id);
  [[nodiscard]] const ExclusionRegion* exclusion(const ExclusionRegionId& id) const;
  [[nodiscard]] ClearanceConstraint* clearance(const ClearanceConstraintId& id);
  [[nodiscard]] const ClearanceConstraint* clearance(const ClearanceConstraintId& id) const;
  [[nodiscard]] ExpansionZone* zone(const ExpansionZoneId& id);
  [[nodiscard]] const ExpansionZone* zone(const ExpansionZoneId& id) const;

  // The nearest ancestor-or-self that declares a plane, and the same for a
  // rack envelope, computed over the working copy.
  [[nodiscard]] SpaceNodeId plane_owner_of(const SpaceNodeId& id) const;
  [[nodiscard]] SpaceNodeId rack_owner_of(const SpaceNodeId& id) const;
  [[nodiscard]] bool is_ancestor(const SpaceNodeId& ancestor, const SpaceNodeId& descendant) const;
  [[nodiscard]] std::uint32_t subtree_height(const SpaceNodeId& id) const;
};

// The result of applying one mutation to a working copy.
struct MutationResult final {
  SpaceNodeId subject{};
  ExplanationSet explanations{};
};

// Stage 1: the request's own shape, independent of any model state.
Status check_argument_shape(const SpaceNode& node);
Status check_argument_shape(const OccupancyClaim& claim);
Status check_argument_shape(const FootprintReservation& reservation);
Status check_argument_shape(const ExclusionRegion& region);
Status check_argument_shape(const ClearanceConstraint& constraint);
Status check_argument_shape(const ExpansionZone& zone);

// Stage 2: the authority fences carried by the request. `current_revision` is
// the revision the working copy was taken from; `current_generation` is the
// generation of the record the request mutates, or nullopt for a creation.
Status check_preconditions(const Precondition& precondition, RegistryRevision current_revision,
                           std::optional<EntityGeneration> current_generation,
                           const std::string& subject);

// Stage 5: refuses a consuming record that overlaps another consuming record on
// the same envelope. This is the check that keeps the ledger's union measure
// equal to its sum over families.
Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node, const OccupancyClaimId& ignore_claim,
                               const SpaceNodeId& ignore_node);
Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node, const OccupancyClaimId& ignore_claim);
Status check_consuming_overlap(const WorkingModel& model, const FootprintScope& scope,
                               const SpaceNodeId& node);

// Stage 5: refuses a new consuming record that takes space a committed
// exclusion or an enforceable clearance already removes from use.
Status check_blocked_by_exclusion(const WorkingModel& model, const FootprintScope& scope,
                                  const SpaceNodeId& node, Timestamp now);

// The complete mutation set. Each returns the subject identity and the
// explanations the outcome carries.
Result<MutationResult> apply_create_node(WorkingModel& model, const CreateNodeRequest& request);
Result<MutationResult> apply_set_node_metadata(WorkingModel& model,
                                               const SetNodeMetadataRequest& request);
Result<MutationResult> apply_set_node_envelope(WorkingModel& model,
                                               const SetNodeEnvelopeRequest& request);
Result<MutationResult> apply_set_node_placement(WorkingModel& model,
                                                const SetNodePlacementRequest& request);
Result<MutationResult> apply_set_node_lifecycle(WorkingModel& model,
                                                const SetNodeLifecycleRequest& request);
Result<MutationResult> apply_set_node_references(WorkingModel& model,
                                                 const SetNodeReferencesRequest& request);
Result<MutationResult> apply_reparent_node(WorkingModel& model, const ReparentNodeRequest& request);
Result<MutationResult> apply_retire_node(WorkingModel& model, const RetireNodeRequest& request);

Result<MutationResult> apply_create_claim(WorkingModel& model, const CreateClaimRequest& request);
Result<MutationResult> apply_transition_claim(WorkingModel& model,
                                              const TransitionClaimRequest& request);
Result<MutationResult> apply_set_claim_details(WorkingModel& model,
                                               const SetClaimDetailsRequest& request);

Result<MutationResult> apply_create_reservation(WorkingModel& model,
                                                const CreateReservationRequest& request);
Result<MutationResult> apply_transition_reservation(WorkingModel& model,
                                                    const TransitionReservationRequest& request);

Result<MutationResult> apply_create_exclusion(WorkingModel& model,
                                              const CreateExclusionRequest& request);
Result<MutationResult> apply_transition_exclusion(WorkingModel& model,
                                                  const TransitionExclusionRequest& request);
Result<MutationResult> apply_create_clearance(WorkingModel& model,
                                              const CreateClearanceRequest& request);
Result<MutationResult> apply_create_expansion_zone(WorkingModel& model,
                                                   const CreateExpansionZoneRequest& request);
Result<MutationResult> apply_transition_expansion_zone(
    WorkingModel& model, const TransitionExpansionZoneRequest& request);

// Turns a working copy into a snapshot one revision ahead of its base.
Result<SnapshotPtr> finalize(WorkingModel& model);

// The request key of each request type, for idempotency.
const RequestId& request_key(const CreateNodeRequest& request);
const RequestId& request_key(const SetNodeMetadataRequest& request);
const RequestId& request_key(const SetNodeEnvelopeRequest& request);
const RequestId& request_key(const SetNodePlacementRequest& request);
const RequestId& request_key(const SetNodeLifecycleRequest& request);
const RequestId& request_key(const SetNodeReferencesRequest& request);
const RequestId& request_key(const ReparentNodeRequest& request);
const RequestId& request_key(const RetireNodeRequest& request);
const RequestId& request_key(const CreateClaimRequest& request);
const RequestId& request_key(const TransitionClaimRequest& request);
const RequestId& request_key(const SetClaimDetailsRequest& request);
const RequestId& request_key(const CreateReservationRequest& request);
const RequestId& request_key(const TransitionReservationRequest& request);
const RequestId& request_key(const CreateExclusionRequest& request);
const RequestId& request_key(const TransitionExclusionRequest& request);
const RequestId& request_key(const CreateClearanceRequest& request);
const RequestId& request_key(const CreateExpansionZoneRequest& request);
const RequestId& request_key(const TransitionExpansionZoneRequest& request);

}  // namespace dccp::space_capacity::internal
