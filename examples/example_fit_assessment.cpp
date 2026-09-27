// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity example: a fit assessment is an observation, not a grant.
//
// What this shows:
//   * asking where a rack unit run or a floor rectangle would fit;
//   * that the assessment carries the revision it was taken at;
//   * that taking the space is a separate mutation which is refused when the
//     space is not free, so a query can never hand out authority.
//
// This example writes nothing.

#include <cstdint>
#include <cstdio>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

}  // namespace

int main() {
  Result<SpaceCapacityRegistry> created =
      SpaceCapacityRegistry::create_in_memory(*StoreId::parse("example-fit"));
  if (!created) {
    std::fprintf(stderr, "cannot start: %s\n", created.error().to_string().c_str());
    return 1;
  }
  SpaceCapacityRegistry registry = std::move(created).value();

  const SpaceNodeId site = *SpaceNodeId::parse("site-1");
  const SpaceNodeId hall = *SpaceNodeId::parse("hall-1");
  const SpaceNodeId rack = *SpaceNodeId::parse("rack-1");

  {
    SpaceNode node;
    node.id = site;
    node.generation = EntityGeneration{1};
    node.kind = SpaceNodeKind::site;
    node.spatial_class = SpatialClass::outdoor;
    node.lifecycle = NodeLifecycle::available;
    CreateNodeRequest request;
    request.node = node;
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "site refused: %s\n", outcome.error().to_string().c_str());
      return 1;
    }
  }
  {
    SpaceNode node;
    node.id = hall;
    node.generation = EntityGeneration{1};
    node.kind = SpaceNodeKind::hall;
    node.spatial_class = SpatialClass::floor;
    node.lifecycle = NodeLifecycle::available;
    node.parent = site;
    node.depth = 1;
    node.own_planar.declared_area = SquareMillimeters{10000 * 10000};
    node.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, 10000, 10000)});
    node.placement.has_base_rect = true;
    node.placement.base_rect = PlanarRect::make(0, 0, 10000, 10000);
    CreateNodeRequest request;
    request.node = node;
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "hall refused: %s\n", outcome.error().to_string().c_str());
      return 1;
    }
  }
  {
    SpaceNode node;
    node.id = rack;
    node.generation = EntityGeneration{1};
    node.kind = SpaceNodeKind::rack;
    node.spatial_class = SpatialClass::rack;
    node.lifecycle = NodeLifecycle::available;
    node.parent = hall;
    node.depth = 2;
    node.own_rack.height = RackUnits{42};
    node.placement.has_base_rect = true;
    node.placement.base_rect = PlanarRect::make(1000, 1000, 600, 1200);
    CreateNodeRequest request;
    request.node = node;
    const Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "rack refused: %s\n", outcome.error().to_string().c_str());
      return 1;
    }
  }

  // Three committed claims split the rack's vertical space into fragments.
  const std::int32_t firsts[3] = {1, 10, 30};
  for (int i = 0; i < 3; ++i) {
    CreateClaimRequest request;
    request.claim.id = *OccupancyClaimId::parse("claim-" + std::to_string(i + 1));
    request.claim.generation = EntityGeneration{1};
    request.claim.node = rack;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::asset;
    request.claim.scope.kind = FootprintScopeKind::rack_units;
    request.claim.scope.units =
        *IntervalSet::build({RackUnitInterval::of_count(firsts[i], 4)});
    if (!registry.apply(request)) return 1;
  }

  std::puts("-- asking where 6 contiguous rack units would fit --");
  {
    RackUnitFitRequest request;
    request.rack = rack;
    request.needed = RackUnits{6};
    request.alignment = 1;
    request.max_candidates = 3;
    const Result<FitAssessment> assessment = registry.assess(request);
    if (!assessment) {
      std::fprintf(stderr, "assessment failed: %s\n", assessment.error().to_string().c_str());
      return 1;
    }
    std::printf("verdict=%s at revision %s\n",
                std::string(fit_verdict_name(assessment.value().verdict)).c_str(),
                assessment.value().revision.to_string().c_str());
    std::printf("declared=%s occupied=%s available=%s free-runs=%u largest-run=%s ppm=%u\n",
                to_text(assessment.value().units.declared).c_str(),
                to_text(assessment.value().units.occupied).c_str(),
                to_text(assessment.value().units.available).c_str(),
                assessment.value().units.free_runs,
                to_text(assessment.value().units.largest_free_run).c_str(),
                assessment.value().units.fragmentation_ppm);
    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      std::printf("  candidate run %s\n", to_text(candidate.units).c_str());
    }
    std::printf("the assessment grants placement: %s\n",
                assessment.value().grants_placement() ? "true" : "false");
  }

  std::puts("");
  std::puts("-- asking where a 2 m x 2 m floor rectangle would fit --");
  {
    PlanarRectFitRequest request;
    request.node = hall;
    request.width = Millimeters{2000};
    request.depth = Millimeters{2000};
    request.alignment = Millimeters{100};
    request.max_candidates = 3;
    const Result<FitAssessment> assessment = registry.assess(request);
    if (!assessment) {
      std::fprintf(stderr, "assessment failed: %s\n", assessment.error().to_string().c_str());
      return 1;
    }
    std::printf("verdict=%s on plane %s with %s free\n",
                std::string(fit_verdict_name(assessment.value().verdict)).c_str(),
                assessment.value().attributed_plane.str().c_str(),
                to_text(assessment.value().area.available).c_str());
    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      std::printf("  candidate rectangle %s\n", to_text(candidate.rect).c_str());
    }
  }

  std::puts("");
  std::puts("-- taking the space is a separate, preconditioned mutation --");
  {
    CreateClaimRequest overlapping;
    overlapping.claim.id = *OccupancyClaimId::parse("claim-overlap");
    overlapping.claim.generation = EntityGeneration{1};
    overlapping.claim.node = rack;
    overlapping.claim.state = ClaimState::committed;
    overlapping.claim.occupant = OccupantKind::asset;
    overlapping.claim.scope.kind = FootprintScopeKind::rack_units;
    overlapping.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(2, 3)});
    const Result<MutationOutcome> refused = registry.apply(overlapping);
    if (refused) {
      std::fputs("the overlapping claim was accepted, which is a defect\n", stderr);
      return 1;
    }
    std::printf("an overlapping claim is refused: %s\n",
                refused.error().to_string().c_str());
  }

  // The same query now reflects the state that was actually committed, and the
  // revision it reports is the one a caller must fence on before acting.
  {
    RackUnitFitRequest request;
    request.rack = rack;
    request.needed = RackUnits{6};
    request.max_candidates = 1;
    const Result<FitAssessment> assessment = registry.assess(request);
    if (!assessment) return 1;
    std::printf("re-asked at revision %s: verdict=%s\n",
                assessment.value().revision.to_string().c_str(),
                std::string(fit_verdict_name(assessment.value().verdict)).c_str());
  }
  return 0;
}
