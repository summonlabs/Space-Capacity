// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity example: a durable store across a real close and reopen.
//
// What this shows:
//   * the commit protocol from the outside: apply, verify, close, reopen;
//   * that evidence observed before a reopen is recovered rather than fresh;
//   * that a stale precondition is refused instead of merged.
//
// This example writes one state file in the current directory and removes it
// again when it finishes.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

constexpr const char* kState = "example-durable.spcstate";

void cleanup() {
  std::error_code error;
  std::filesystem::remove(kState, error);
  std::filesystem::remove(std::string(kState) + ".prev", error);
  std::filesystem::remove(std::string(kState) + ".identity", error);
  std::filesystem::remove(std::string(kState) + ".lock", error);
}

StoreOptions options() {
  StoreOptions store;
  store.path = kState;
  store.store_identity = *StoreId::parse("example-durable");
  store.actor = "example";
  store.source = "example_durable_store";
  return store;
}

int create_site(SpaceCapacityRegistry& registry) {
  CreateNodeRequest request;
  request.node.id = *SpaceNodeId::parse("site-x");
  request.node.generation = EntityGeneration{1};
  request.node.kind = SpaceNodeKind::site;
  request.node.spatial_class = SpatialClass::outdoor;
  request.node.lifecycle = NodeLifecycle::available;
  request.node.label = *DisplayLabel::parse("Site X");
  const Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "apply failed: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  cleanup();

  std::string first_digest;
  RegistryRevision first_revision{};
  {
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options());
    if (!opened) {
      std::fprintf(stderr, "open failed: %s\n", opened.error().to_string().c_str());
      return 1;
    }
    SpaceCapacityRegistry registry = std::move(opened).value();
    RecoveryAction action = RecoveryAction::created;
    const Result<StoreStatus> status = registry.store_status();
    if (!status) {
      std::fprintf(stderr, "status failed: %s\n", status.error().to_string().c_str());
      return 1;
    }
    if (status.value().revision.value() != 0) action = RecoveryAction::loaded_current;
    const std::string action_name(recovery_action_name(action));
    const std::string incarnation_text = registry.incarnation().to_string();
    const std::string revision_text = registry.revision().to_string();
    std::printf("opened: store=%s incarnation=%s revision=%s recovery=%s\n",
                registry.store_id().str().c_str(), incarnation_text.c_str(),
                revision_text.c_str(), action_name.c_str());
    // The revision a caller would have observed before deciding to mutate. A
    // request fenced on it must be refused once the model has moved on.
    const RegistryRevision prepared_against = registry.revision();
    if (create_site(registry) != 0) return 1;
    const Status verified = registry.verify();
    if (!verified) {
      std::fprintf(stderr, "verify failed: %s\n", verified.error().to_string().c_str());
      return 1;
    }
    first_digest = registry.snapshot()->digest().tagged_hex();
    first_revision = registry.revision();
    std::printf("committed revision %s digest %s\n", first_revision.to_string().c_str(),
                first_digest.c_str());

    // A request fenced on the revision it was prepared against. After the
    // first apply the model has moved, so this one is refused.
    CreateNodeRequest stale;
    stale.node.id = *SpaceNodeId::parse("site-y");
    stale.node.generation = EntityGeneration{1};
    stale.node.kind = SpaceNodeKind::site;
    stale.node.spatial_class = SpatialClass::outdoor;
    stale.node.lifecycle = NodeLifecycle::available;
    stale.precondition = Precondition::at_revision(prepared_against);
    const Result<MutationOutcome> refused = registry.apply(stale);
    const std::string refusal = refused
                                    ? std::string("nothing (wrong)")
                                    : std::string(error_code_name(refused.error().code));
    std::printf("a second site at the same revision is refused with %s\n", refusal.c_str());
  }

  {
    Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options());
    if (!reopened) {
      std::fprintf(stderr, "reopen failed: %s\n", reopened.error().to_string().c_str());
      return 1;
    }
    SpaceCapacityRegistry registry = std::move(reopened).value();
    std::printf("reopened: revision %s digest %s\n", registry.revision().to_string().c_str(),
                registry.snapshot()->digest().tagged_hex().c_str());
    if (registry.snapshot()->digest().tagged_hex() != first_digest) {
      std::fputs("the reopened state is not byte-identical to what was committed\n", stderr);
      cleanup();
      return 1;
    }
    std::printf("the reopened state is byte-identical, and %zu nodes were recovered\n",
                registry.snapshot()->node_count());

    // Evidence attached before the reopen can no longer be fresh: the attempt
    // it was observed at is at or below the floor this session started from.
    EvidenceRef evidence;
    evidence.source = RegistryKind::asset_registry;
    evidence.id = *ExternalId::parse("11111111-1111-4111-8111-111111111111");
    evidence.generation = EntityGeneration{7};
    evidence.observed_at = registry.fresh_floor();
    std::printf("evidence from the previous session is classified %s\n",
                std::string(freshness_name(registry.evidence_freshness(evidence))).c_str());

    ObservationToken token;
    token.subject = *SpaceNodeId::parse("site-x");
    token.subject_generation = EntityGeneration{1};
    token.revision = registry.revision();
    token.attempt = registry.snapshot()->attempt();
    const Result<Revalidation> revalidated = registry.revalidate(token);
    const std::string freshness =
        revalidated ? std::string(freshness_name(revalidated.value().freshness))
                    : std::string(error_code_name(revalidated.error().code));
    std::printf("revalidating an observation from before the reopen reports %s\n",
                freshness.c_str());
  }

  cleanup();
  std::puts("done");
  return 0;
}
