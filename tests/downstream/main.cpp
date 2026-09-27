// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - independent downstream consumer.
//
// This program is built OUT OF TREE against an installed Space Capacity
// package, by a CMake project that is not part of the library's own build. It
// includes only the public umbrella header and links only the exported
// namespaced target, so it proves the installed package is complete: headers,
// library, and a versioned CMake configuration that resolves from any prefix.
//
// It runs a real minimal lifecycle and checks a small piece of accounting
// arithmetic end to end, then removes everything it wrote.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

void remove_state(const std::string& state) {
  std::error_code error;
  std::filesystem::remove(state, error);
  std::filesystem::remove(state + ".prev", error);
  std::filesystem::remove(state + ".identity", error);
  std::filesystem::remove(state + ".lock", error);
}

int fail(const char* what, const Error& error) {
  std::fprintf(stderr, "%s: %s\n", what, error.to_string().c_str());
  return 1;
}

}  // namespace

int main() {
  std::printf("downstream consumer built against Space Capacity %s\n",
              std::string(version_string()).c_str());

  const std::string state = "downstream-consumer.spcstate";
  remove_state(state);

  StoreOptions options;
  options.path = state;
  options.store_identity = *StoreId::parse("downstream-store");
  options.actor = "downstream";
  options.source = "downstream_consumer";

  int status = 0;
  std::string digest;
  {
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    if (!opened) return fail("open", opened.error());
    SpaceCapacityRegistry registry = std::move(opened).value();

    // A site root first: only a site may be a root, so the hall is contained.
    const SpaceNodeId root_id = *SpaceNodeId::parse("downstream-site");
    {
      CreateNodeRequest request;
      request.node.id = root_id;
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::site;
      request.node.spatial_class = SpatialClass::outdoor;
      request.node.lifecycle = NodeLifecycle::available;
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (!outcome) return fail("create site", outcome.error());
    }

    const SpaceNodeId hall = *SpaceNodeId::parse("downstream-hall");
    {
      CreateNodeRequest request;
      request.node.id = hall;
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::hall;
      request.node.spatial_class = SpatialClass::floor;
      request.node.lifecycle = NodeLifecycle::available;
      request.node.parent = root_id;
      request.node.depth = 1;
      request.node.own_planar.declared_area = SquareMillimeters{10000 * 10000};
      request.node.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, 10000, 10000)});
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (!outcome) return fail("create hall", outcome.error());
    }

    // 100 square metres of floor, of which nothing is used yet.
    const NodeCapacity capacity = registry.snapshot()->capacity_of(hall, FitContext{});
    if (capacity.area.declared.value() != 100'000'000ll) {
      std::fprintf(stderr, "declared area is %lld, expected 100000000\n",
                   static_cast<long long>(capacity.area.declared.value()));
      status = 1;
    }
    if (capacity.area.available.value() != 100'000'000ll) {
      std::fprintf(stderr, "available area is %lld, expected 100000000\n",
                   static_cast<long long>(capacity.area.available.value()));
      status = 1;
    }

    // Take 10 square metres with a committed claim.
    {
      CreateClaimRequest request;
      request.claim.id = *OccupancyClaimId::parse("downstream-claim");
      request.claim.generation = EntityGeneration{1};
      request.claim.node = hall;
      request.claim.state = ClaimState::committed;
      request.claim.occupant = OccupantKind::workload;
      request.claim.scope.kind = FootprintScopeKind::planar;
      request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 0, 2000, 5000)});
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (!outcome) return fail("create claim", outcome.error());
    }

    const NodeCapacity after = registry.snapshot()->capacity_of(hall, FitContext{});
    if (after.area.available.value() != 90'000'000ll) {
      std::fprintf(stderr, "available area after the claim is %lld, expected 90000000\n",
                   static_cast<long long>(after.area.available.value()));
      status = 1;
    }
    if (after.area.claimed.value() != 10'000'000ll) {
      std::fprintf(stderr, "claimed area is %lld, expected 10000000\n",
                   static_cast<long long>(after.area.claimed.value()));
      status = 1;
    }

    // A fit query observes and never grants.
    PlanarRectFitRequest fit;
    fit.node = hall;
    fit.width = Millimeters{2000};
    fit.depth = Millimeters{5000};
    const Result<FitAssessment> assessment = registry.assess(fit);
    if (!assessment) return fail("assess", assessment.error());
    if (assessment.value().grants_placement()) {
      std::fputs("the assessment claimed to grant placement\n", stderr);
      status = 1;
    }
    if (assessment.value().verdict != FitVerdict::fits) {
      std::fputs("the assessment did not find a placement on a mostly empty plane\n", stderr);
      status = 1;
    }

    if (!registry.verify()) {
      std::fputs("verify failed\n", stderr);
      status = 1;
    }
    digest = registry.snapshot()->digest().tagged_hex();
    (void)registry.close();
  }

  {
    StoreOptions read_only = options;
    read_only.mode = OpenMode::read_only;
    Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(read_only);
    if (!reopened) return fail("reopen", reopened.error());
    if (reopened.value().snapshot()->digest().tagged_hex() != digest) {
      std::fputs("the reopened state is not byte-identical\n", stderr);
      status = 1;
    }
    if (reopened.value().snapshot()->node_count() != 2) {
      std::fputs("the reopened state lost a record\n", stderr);
      status = 1;
    }
    (void)reopened.value().close();
  }

  remove_state(state);
  std::error_code error;
  if (std::filesystem::exists(state, error)) {
    std::fputs("the downstream consumer left residue behind\n", stderr);
    status = 1;
  }

  if (status == 0) std::puts("downstream consumer: PASS");
  return status;
}
