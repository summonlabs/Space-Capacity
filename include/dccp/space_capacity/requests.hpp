// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - mutation requests and their preconditions.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every mutation that depends on current state carries an explicit
// precondition. A precondition that no longer holds is refused with
// stale_generation or stale_revision and the current values, never merged and
// never partially applied.
//
// A request never carries authority it does not have. Space Capacity owns
// physical-space accounting; it does not own placement, scheduling,
// reservation decisions or upstream registry content, and no request here can
// make it act as though it did.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/observation.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

// The fence a mutation is applied under.
//
//   * `revision` fences the whole model: the mutation is refused unless the
//     committed revision still equals it. Use it whenever the decision to
//     mutate was taken from an observation of the whole model.
//   * `generation` fences the one record the mutation changes: the mutation is
//     refused unless that record is still at that generation.
//
// Setting neither is legal and means "apply against whatever is committed".
// That is a deliberate, explicit choice, and the outcome records the revision
// it was applied at so a caller can tell what it actually got.
struct SC_API Precondition final {
  std::optional<RegistryRevision> revision{};
  std::optional<EntityGeneration> generation{};

  [[nodiscard]] bool empty() const noexcept { return !revision.has_value() && !generation.has_value(); }
  [[nodiscard]] static Precondition at_revision(RegistryRevision value) {
    Precondition p;
    p.revision = value;
    return p;
  }
  [[nodiscard]] static Precondition at_generation(EntityGeneration value) {
    Precondition p;
    p.generation = value;
    return p;
  }
  [[nodiscard]] static Precondition exact(RegistryRevision revision_value,
                                          EntityGeneration generation_value) {
    Precondition p;
    p.revision = revision_value;
    p.generation = generation_value;
    return p;
  }
  friend bool operator==(const Precondition&, const Precondition&) = default;
};

// ---------------------------------------------------------------------------
// Node requests
// ---------------------------------------------------------------------------

struct SC_API CreateNodeRequest final {
  SpaceNode node{};
  Precondition precondition{};
  RequestId request_id{};
};

struct SC_API SetNodeMetadataRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  // Every field is optional; a field that is not set is left alone, and a
  // field whose `clear_` flag is set is emptied. There is no third meaning.
  std::optional<DisplayLabel> label{};
  bool clear_label = false;
  std::optional<Note> note{};
  bool clear_note = false;
  std::optional<SpatialClass> spatial_class{};
};

struct SC_API SetNodeEnvelopeRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  std::optional<PlanarEnvelope> planar{};
  std::optional<RackEnvelope> rack{};
};

struct SC_API SetNodePlacementRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  NodePlacement placement{};
};

struct SC_API SetNodeLifecycleRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  NodeLifecycle lifecycle = NodeLifecycle::planned;
  // Required when moving to `replaced`, and refused when moving to any other
  // state, so a replacement link cannot be set without a retirement.
  SpaceNodeId successor{};
};

struct SC_API SetNodeReferencesRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  std::optional<LocationRef> location{};
  bool clear_location = false;
  std::optional<FacilityNodeRef> facility_node{};
  bool clear_facility_node = false;
  std::optional<RackRef> rack{};
  bool clear_rack = false;
  std::optional<AssetRefSet> assets{};
  std::optional<PolicyRefSet> policies{};
};

struct SC_API ReparentNodeRequest final {
  SpaceNodeId node{};
  SpaceNodeId new_parent{};
  Precondition precondition{};
  RequestId request_id{};

  // Where the node sits inside the new parent. Required when the new parent
  // chain reaches a plane or a rack envelope that the node consumes.
  NodePlacement placement{};
};

struct SC_API RetireNodeRequest final {
  SpaceNodeId node{};
  Precondition precondition{};
  RequestId request_id{};

  // When set, the node moves to `replaced` and records the successor. When
  // empty, the node moves to `retired`.
  SpaceNodeId successor{};
};

// ---------------------------------------------------------------------------
// Claim requests
// ---------------------------------------------------------------------------

struct SC_API CreateClaimRequest final {
  OccupancyClaim claim{};
  Precondition precondition{};
  RequestId request_id{};
  // When true, the claim is created in the state it carries even if that state
  // consumes space. When false, a consuming claim is refused at creation and
  // must be committed by an explicit transition, so space is taken in two
  // visible steps rather than one.
  bool allow_consuming_creation = true;
};

struct SC_API TransitionClaimRequest final {
  OccupancyClaimId claim{};
  Precondition precondition{};
  RequestId request_id{};
  ClaimState next = ClaimState::planned;
};

struct SC_API SetClaimDetailsRequest final {
  OccupancyClaimId claim{};
  Precondition precondition{};
  RequestId request_id{};

  std::optional<DisplayLabel> label{};
  bool clear_label = false;
  std::optional<Note> note{};
  bool clear_note = false;
  std::optional<OccupantKind> occupant{};
  std::optional<RackRef> rack{};
  bool clear_rack = false;
  std::optional<AssetRefSet> assets{};
  std::optional<PolicyRefSet> policies{};
  std::optional<EvidenceSet> evidence{};
};

// ---------------------------------------------------------------------------
// Reservation requests
// ---------------------------------------------------------------------------

struct SC_API CreateReservationRequest final {
  FootprintReservation reservation{};
  Precondition precondition{};
  RequestId request_id{};
};

struct SC_API TransitionReservationRequest final {
  OccupancyClaimId reservation{};
  Precondition precondition{};
  RequestId request_id{};
  ReservationState next = ReservationState::held;
  // Required when moving to `revoked`: the authority that withdrew the hold.
  // Space Capacity does not withdraw a hold on its own initiative.
  std::optional<ReservationRef> revocation_authority{};
};

// ---------------------------------------------------------------------------
// Exclusion, clearance and expansion requests
// ---------------------------------------------------------------------------

struct SC_API CreateExclusionRequest final {
  ExclusionRegion region{};
  Precondition precondition{};
  RequestId request_id{};
};

struct SC_API TransitionExclusionRequest final {
  ExclusionRegionId region{};
  Precondition precondition{};
  RequestId request_id{};
  ExclusionState next = ExclusionState::draft;
};

struct SC_API CreateClearanceRequest final {
  ClearanceConstraint constraint{};
  Precondition precondition{};
  RequestId request_id{};
};

struct SC_API CreateExpansionZoneRequest final {
  ExpansionZone zone{};
  Precondition precondition{};
  RequestId request_id{};
};

struct SC_API TransitionExpansionZoneRequest final {
  ExpansionZoneId zone{};
  Precondition precondition{};
  RequestId request_id{};
  ExpansionState next = ExpansionState::identified;
};

// ---------------------------------------------------------------------------
// Outcome
// ---------------------------------------------------------------------------

// What a mutation did. `applied` is false exactly when the request key had
// already been used at the same base revision and the recorded outcome was
// returned instead of applying the request a second time.
struct SC_API MutationOutcome final {
  RegistryRevision revision{};
  AttemptId attempt{};
  Digest state_digest{};
  bool applied = true;
  SpaceNodeId subject{};
  ExplanationSet explanations{};

  [[nodiscard]] bool replayed() const noexcept { return !applied; }
};

}  // namespace dccp::space_capacity
