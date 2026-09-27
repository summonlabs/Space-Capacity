// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - immutable snapshots and the query engine over them.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A snapshot is a complete, immutable view of one committed revision. Readers
// take a snapshot and work from it without holding a lock, so a long report, a
// diff or a fit search never blocks a writer and never observes a
// half-applied change. Writers build the next snapshot privately and publish it
// with one pointer swap, which is the only point at which state changes.
//
// The snapshot digest is a domain-separated digest of the canonical
// serialization of exactly the bytes the store writes for that revision. Two
// snapshots that describe the same model have the same digest regardless of the
// order in which records were inserted, because the canonical form sorts every
// collection.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

class Snapshot;
using SnapshotPtr = std::shared_ptr<const Snapshot>;

class SC_API Snapshot final {
 public:
  Snapshot() = default;

  // An empty model with a store identity and an incarnation.
  [[nodiscard]] static SnapshotPtr make_empty(StoreId store, StoreIncarnation incarnation,
                                              RegistryRevision revision, AttemptId attempt);

  [[nodiscard]] const StoreId& store() const noexcept { return store_; }
  [[nodiscard]] StoreIncarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] RegistryRevision revision() const noexcept { return revision_; }
  [[nodiscard]] AttemptId attempt() const noexcept { return attempt_; }
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
  [[nodiscard]] Timestamp created_at() const noexcept { return created_at_; }

  // ------------------------------------------------------------- collection
  [[nodiscard]] const std::vector<SpaceNode>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] const std::vector<OccupancyClaim>& claims() const noexcept { return claims_; }
  [[nodiscard]] const std::vector<FootprintReservation>& reservations() const noexcept {
    return reservations_;
  }
  [[nodiscard]] const std::vector<ExclusionRegion>& exclusions() const noexcept {
    return exclusions_;
  }
  [[nodiscard]] const std::vector<ClearanceConstraint>& clearances() const noexcept {
    return clearances_;
  }
  [[nodiscard]] const std::vector<ExpansionZone>& expansion_zones() const noexcept {
    return expansion_zones_;
  }

  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t record_count() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

  // ------------------------------------------------------------------ lookup
  [[nodiscard]] const SpaceNode* find_node(const SpaceNodeId& id) const noexcept;
  [[nodiscard]] const OccupancyClaim* find_claim(const OccupancyClaimId& id) const noexcept;
  [[nodiscard]] const FootprintReservation* find_reservation(
      const OccupancyClaimId& id) const noexcept;
  [[nodiscard]] const ExclusionRegion* find_exclusion(const ExclusionRegionId& id) const noexcept;
  [[nodiscard]] const ClearanceConstraint* find_clearance(
      const ClearanceConstraintId& id) const noexcept;
  [[nodiscard]] const ExpansionZone* find_expansion_zone(const ExpansionZoneId& id) const noexcept;

  // Direct children of a node, in canonical order.
  [[nodiscard]] std::vector<SpaceNodeId> children_of(const SpaceNodeId& parent) const;
  // Every node in the subtree rooted at `root`, including the root, in
  // canonical order. Returns an error when the root is unknown or when the
  // subtree exceeds the walk budget.
  [[nodiscard]] Result<std::vector<SpaceNodeId>> subtree_of(const SpaceNodeId& root) const;
  // Ancestor chain from the direct parent up to the root, nearest first.
  [[nodiscard]] Result<std::vector<SpaceNodeId>> ancestors_of(const SpaceNodeId& node) const;

  // The nearest ancestor-or-self that declares a plane. Empty when there is
  // none: the node is a grouping node under a root that declares no plane.
  [[nodiscard]] SpaceNodeId plane_owner_of(const SpaceNodeId& node) const;
  // The nearest ancestor-or-self that declares a vertical rack envelope.
  [[nodiscard]] SpaceNodeId rack_owner_of(const SpaceNodeId& node) const;

  [[nodiscard]] bool is_strict_ancestor(const SpaceNodeId& ancestor,
                                        const SpaceNodeId& descendant) const;

  // Roots of the model, in canonical order.
  [[nodiscard]] std::vector<SpaceNodeId> roots() const;

  // ---------------------------------------------------------------- capacity
  [[nodiscard]] NodeCapacity capacity_of(const SpaceNodeId& node,
                                         const FitContext& context) const;
  [[nodiscard]] Result<CapacityReport> report(const SpaceNodeId& node,
                                              const FitContext& context) const;
  [[nodiscard]] Result<CapacityRollup> rollup(const SpaceNodeId& node,
                                              const FitContext& context) const;
  // Rollup over every root of the model.
  [[nodiscard]] CapacityRollup rollup_all(const FitContext& context) const;

  // ---------------------------------------------------------------- queries
  [[nodiscard]] Result<FitAssessment> assess(const RackUnitFitRequest& request) const;
  [[nodiscard]] Result<FitAssessment> assess(const PlanarRectFitRequest& request) const;
  [[nodiscard]] Result<FitAssessment> assess(const PlanarAreaFitRequest& request) const;

  // ---------------------------------------------------------------- integrity
  // Full model audit: identity uniqueness, containment, depth, kind rules,
  // envelope and placement validity, extent bounds, reference bounds,
  // claim and scope consistency, plane-subdivision consistency, and ledger
  // reconstruction with a re-check of the non-double-counting invariant. This
  // is the same pass the store runs on every load, and it is strictly stricter
  // than any read path.
  [[nodiscard]] Status audit() const;

  // Builds a snapshot from records. Every collection is sorted and every index
  // rebuilt. Returns an error when the input violates a structural rule.
  struct BuildInput final {
    StoreId store{};
    StoreIncarnation incarnation{};
    RegistryRevision revision{};
    AttemptId attempt{};
    Timestamp created_at{};
    std::vector<SpaceNode> nodes;
    std::vector<OccupancyClaim> claims;
    std::vector<FootprintReservation> reservations;
    std::vector<ExclusionRegion> exclusions;
    std::vector<ClearanceConstraint> clearances;
    std::vector<ExpansionZone> expansion_zones;
  };

  [[nodiscard]] static Result<SnapshotPtr> build(BuildInput input);

  // The canonical byte image of this snapshot. This is exactly the payload the
  // store writes, so `digest()` is the digest of these bytes.
  [[nodiscard]] std::string canonical_bytes() const;

  // Canonical text rendering used by the CLI's `show` command.
  [[nodiscard]] std::string canonical_text() const;

 private:
  struct Index;

  StoreId store_{};
  StoreIncarnation incarnation_{};
  RegistryRevision revision_{};
  AttemptId attempt_{};
  Timestamp created_at_{};
  Digest digest_{};

  std::vector<SpaceNode> nodes_;
  std::vector<OccupancyClaim> claims_;
  std::vector<FootprintReservation> reservations_;
  std::vector<ExclusionRegion> exclusions_;
  std::vector<ClearanceConstraint> clearances_;
  std::vector<ExpansionZone> expansion_zones_;

  std::shared_ptr<const Index> index_;
};

// Builds the whole-model ledger from a snapshot without any node filter. Used
// by the diff engine and by the CLI's summary command.
SC_API CapacityRollup model_rollup(const Snapshot& snapshot, const FitContext& context);

}  // namespace dccp::space_capacity
