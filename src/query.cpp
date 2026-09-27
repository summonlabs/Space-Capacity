// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the fit engine.
//
// Two searches live here:
//
//   * a rack-unit search, which works on the exact complement of the blocked
//     intervals inside a rack's declared envelope. It is complete by
//     construction: the complement is a list of maximal free runs and every
//     aligned start inside every run is enumerated.
//
//   * a planar search, which enumerates candidate corners. The candidate set
//     is the left edge of every declared plane rectangle, the aligned right
//     edge of every obstacle, and the origin, on each axis. That set is
//     complete: any feasible axis-aligned placement can be slid up and left
//     inside its own free interval until it rests against the plane boundary
//     or against an obstacle, and the first aligned position at or after that
//     resting point is in the candidate set. A property test checks the claim
//     against a brute-force search over small integer grids.
//
// Neither search grants anything. A verdict describes the space; taking it
// requires a separate, preconditioned mutation.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/query.hpp"
#include "dccp/space_capacity/snapshot.hpp"
#include "dccp/space_capacity/text.hpp"
#include "engine.hpp"

namespace dccp::space_capacity {

std::string_view fit_verdict_name(FitVerdict value) noexcept {
  switch (value) {
    case FitVerdict::fits:
      return "fits";
    case FitVerdict::does_not_fit:
      return "does-not-fit";
    case FitVerdict::indeterminate:
      return "indeterminate";
  }
  return "unknown";
}

bool parse_fit_verdict(std::string_view text, FitVerdict& out) noexcept {
  if (text == "fits") {
    out = FitVerdict::fits;
    return true;
  }
  if (text == "does-not-fit") {
    out = FitVerdict::does_not_fit;
    return true;
  }
  if (text == "indeterminate") {
    out = FitVerdict::indeterminate;
    return true;
  }
  return false;
}

namespace {

std::int64_t align_up(std::int64_t value, std::int64_t alignment) {
  if (alignment <= 1 || value <= 0) return value < 0 ? 0 : value;
  const std::int64_t remainder = value % alignment;
  if (remainder == 0) return value;
  return value + (alignment - remainder);
}

std::uint32_t clamp_candidates(std::uint32_t requested) {
  if (requested == 0) return 1;
  if (requested > Limits::kMaxCandidatesPerReport) return Limits::kMaxCandidatesPerReport;
  return requested;
}

FitAssessment base_assessment(const Snapshot& snapshot, const SpaceNodeId& subject,
                              const SpaceNode& record, const SpaceNodeId& plane,
                              const FitContext& context) {
  FitAssessment assessment;
  assessment.subject = subject;
  assessment.subject_generation = record.generation;
  assessment.attributed_plane = plane;
  assessment.area = internal::ledger_from(internal::measure_plane(snapshot, plane, context));
  assessment.units =
      internal::ledger_from(internal::measure_rack(snapshot, snapshot.rack_owner_of(subject),
                                                   context));
  assessment.revision = snapshot.revision();
  assessment.attempt = snapshot.attempt();
  return assessment;
}

}  // namespace

Result<FitAssessment> Snapshot::assess(const RackUnitFitRequest& request) const {
  const SpaceNode* rack = find_node(request.rack);
  if (rack == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.rack.str());
  }
  if (!rack->own_rack.is_declared() || !kind_may_declare_rack_envelope(rack->kind)) {
    return Error::with_subject(ErrorCode::envelope_not_declared,
                               "the node does not declare a rack envelope", request.rack.str());
  }
  if (request.needed.value() < Limits::kMinRackUnits ||
      request.needed.value() > Limits::kMaxRackUnits) {
    return Error::with_subject(ErrorCode::invalid_range,
                               "the requested unit count is outside the representable range",
                               request.rack.str());
  }
  if (request.alignment < 1 || request.alignment > Limits::kMaxRackUnits) {
    return Error::with_subject(ErrorCode::invalid_range,
                               "the requested alignment is outside the representable range",
                               request.rack.str());
  }

  const internal::RackMeasure measure = internal::measure_rack(*this, request.rack, request.context);

  FitAssessment assessment;
  assessment.subject = request.rack;
  assessment.subject_generation = rack->generation;
  assessment.attributed_plane = plane_owner_of(request.rack);
  assessment.area = internal::ledger_from(
      internal::measure_plane(*this, plane_owner_of(request.rack), request.context));
  assessment.units = internal::ledger_from(measure);
  assessment.revision = revision_;
  assessment.attempt = attempt_;

  if (measure.overflowed) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(ReasonCode::unknown_capacity, request.rack.str(),
                                "checked arithmetic refused the rack ledger");
    return assessment;
  }
  if (measure.planned) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::node_not_available, request.rack.str(),
                                "the rack envelope is planned, so it offers no space now");
    return assessment;
  }
  if (!measure.usable) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::node_not_available, request.rack.str(),
                                "the rack envelope is not in service");
    return assessment;
  }
  if (request.needed.value() > measure.declared.value()) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(
        ReasonCode::envelope_too_small, request.rack.str(),
        "the request needs " + to_text(request.needed) + " and the envelope declares " +
            to_text(measure.declared));
    return assessment;
  }

  Result<IntervalSet> free_space = IntervalSet::complement(measure.blocked, measure.declared.value());
  if (!free_space) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(ReasonCode::unknown_capacity, request.rack.str(),
                                "the free space could not be resolved");
    return assessment;
  }

  const std::uint32_t limit = clamp_candidates(request.max_candidates);
  // The largest free run is reported even when it is too small for the
  // request, so an operator can see how close the fit was.
  RackUnitInterval largest{};
  for (const RackUnitInterval& run : free_space.value().intervals()) {
    if (run.count() > largest.count()) largest = run;
  }
  if (largest.is_valid()) assessment.largest_free_run = largest;

  for (const RackUnitInterval& run : free_space.value().intervals()) {
    if (run.count() < request.needed.value()) continue;
    const std::int32_t offset = (run.first - 1) % request.alignment;
    const std::int32_t shift = (offset == 0) ? 0 : (request.alignment - offset);
    for (std::int32_t start = run.first + shift;
         start + request.needed.value() <= run.last; start += request.alignment) {
      PlacementCandidate candidate;
      candidate.node = request.rack;
      candidate.units = RackUnitInterval::of_count(start, request.needed.value());
      candidate.has_units = true;
      assessment.candidates.push_back(candidate);
      if (assessment.candidates.size() >= limit) break;
    }
    if (assessment.candidates.size() >= limit) break;
  }

  if (!assessment.candidates.empty()) {
    assessment.verdict = FitVerdict::fits;
    assessment.explanations.add(ReasonCode::fits_contiguous, request.rack.str(),
                                "a contiguous run of " + to_text(request.needed) +
                                    " rack units is free");
  } else {
    assessment.verdict = FitVerdict::does_not_fit;
    if (measure.available.is_zero()) {
      assessment.explanations.add(ReasonCode::no_contiguous_run, request.rack.str(),
                                  "the usable envelope holds no free rack unit");
    } else {
      assessment.explanations.add(
          ReasonCode::fragmented_free_space, request.rack.str(),
          "the envelope holds " + to_text(measure.available) +
              " free units but the largest run is " + to_text(measure.largest_free_run));
    }
  }
  if (!measure.blocked.empty()) {
    assessment.explanations.add(ReasonCode::committed_occupancy_subtracted, request.rack.str(),
                                "blocked units measured by union: " +
                                    to_text(measure.blocked));
  }
  return assessment;
}

Result<FitAssessment> Snapshot::assess(const PlanarRectFitRequest& request) const {
  const SpaceNode* node = find_node(request.node);
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  const SpaceNodeId plane = plane_owner_of(request.node);
  if (plane.empty()) {
    return Error::with_subject(ErrorCode::plane_not_declared,
                               "no node in the containment chain declares a plane",
                               request.node.str());
  }
  const SpaceNode* owner = find_node(plane);
  if (owner == nullptr) {
    return Error::with_subject(ErrorCode::plane_mismatch, "the enclosing plane is not in the model",
                               request.node.str());
  }
  if (request.width.value() <= 0 || request.depth.value() <= 0) {
    return Error::with_subject(ErrorCode::invalid_range,
                               "the requested rectangle must have a positive width and depth",
                               request.node.str());
  }
  if (request.width.value() > Limits::kMaxMillimeters ||
      request.depth.value() > Limits::kMaxMillimeters) {
    return Error::with_subject(ErrorCode::invalid_range,
                               "the requested rectangle exceeds the millimetre bounds",
                               request.node.str());
  }

  const internal::PlaneMeasure measure = internal::measure_plane(*this, plane, request.context);
  FitAssessment assessment = base_assessment(*this, request.node, *node, plane, request.context);
  assessment.units.state = MeasureState::unknown;

  if (measure.overflowed) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(ReasonCode::unknown_capacity, request.node.str(),
                                "checked arithmetic refused the plane ledger");
    return assessment;
  }
  if (measure.planned) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::node_not_available, request.node.str(),
                                "the plane is planned, so it offers no space now");
    return assessment;
  }
  if (!measure.usable) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::node_not_available, request.node.str(),
                                "the plane is not in service");
    return assessment;
  }
  if (owner->own_planar.rects.empty()) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(
        ReasonCode::plane_not_declared, request.node.str(),
        "the plane declares an area but no internal structure, so no placement can be shown");
    return assessment;
  }

  const std::vector<PlanarRect>& declared = owner->own_planar.rects.rects();
  std::int64_t alignment = request.alignment.value();
  if (alignment <= 1) alignment = 1;

  std::vector<std::int64_t> xs;
  std::vector<std::int64_t> ys;
  xs.push_back(0);
  ys.push_back(0);
  for (const PlanarRect& rect : declared) {
    xs.push_back(align_up(rect.x.value(), alignment));
    ys.push_back(align_up(rect.y.value(), alignment));
    xs.push_back(align_up(rect.right().value(), alignment));
    ys.push_back(align_up(rect.bottom().value(), alignment));
  }
  for (const PlanarRect& obstacle : measure.obstacles) {
    xs.push_back(align_up(obstacle.right().value(), alignment));
    ys.push_back(align_up(obstacle.bottom().value(), alignment));
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

  const std::uint64_t pairs =
      static_cast<std::uint64_t>(xs.size()) * static_cast<std::uint64_t>(ys.size());
  if (pairs > Limits::kMaxFitCandidatePairs) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(ReasonCode::candidate_search_exhausted, request.node.str(),
                                "the candidate set exceeds the search budget of " +
                                    to_text(Limits::kMaxFitCandidatePairs) + " positions");
    return assessment;
  }

  const std::uint32_t limit = clamp_candidates(request.max_candidates);
  std::vector<PlacementCandidate> found;
  for (const std::int64_t y : ys) {
    for (const std::int64_t x : xs) {
      const PlanarRect candidate = PlanarRect::make(x, y, request.width.value(),
                                                     request.depth.value());
      if (!candidate.is_valid()) continue;
      const Checked<SquareMillimeters> candidate_area = candidate.area();
      if (!candidate_area) continue;

      // Inside the plane: the plane's rectangles are pairwise disjoint, so the
      // covered measure must equal the placement's own measure.
      std::int64_t covered = 0;
      bool overflowed = false;
      for (const PlanarRect& rect : declared) {
        const Checked<SquareMillimeters> piece = rect_intersection_area(rect, candidate);
        if (!piece) {
          overflowed = true;
          break;
        }
        covered += piece.value.value();
      }
      if (overflowed || covered != candidate_area.value.value()) continue;

      bool blocked = false;
      for (const PlanarRect& obstacle : measure.obstacles) {
        if (rect_intersects(obstacle, candidate)) {
          blocked = true;
          break;
        }
      }
      if (blocked) continue;

      PlacementCandidate placement;
      placement.node = plane;
      placement.rect = candidate;
      placement.has_rect = true;
      found.push_back(placement);
    }
  }

  std::sort(found.begin(), found.end(), [](const PlacementCandidate& a,
                                           const PlacementCandidate& b) {
    if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
    return a.rect.x < b.rect.x;
  });
  if (found.size() > limit) found.resize(limit);

  assessment.candidates = std::move(found);
  if (!assessment.candidates.empty()) {
    assessment.verdict = FitVerdict::fits;
    assessment.explanations.add(ReasonCode::fits_contiguous, request.node.str(),
                                "at least one placement of " + to_text(request.width) + " by " +
                                    to_text(request.depth) + " is free on this plane");
  } else {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(
        ReasonCode::no_contiguous_run, request.node.str(),
        "no placement of " + to_text(request.width) + " by " + to_text(request.depth) +
            " fits inside the declared rectangles without meeting an obstacle");
  }
  if (!measure.reported_clearances.empty()) {
    assessment.explanations.add(ReasonCode::clearance_not_enforceable, request.node.str(),
                                "the plane carries a clearance that is reported but not "
                                "subtracted from capacity");
  }
  return assessment;
}

Result<FitAssessment> Snapshot::assess(const PlanarAreaFitRequest& request) const {
  const SpaceNode* node = find_node(request.node);
  if (node == nullptr) {
    return Error::with_subject(ErrorCode::not_found, "no such node", request.node.str());
  }
  if (request.needed.is_negative()) {
    return Error::with_subject(ErrorCode::invalid_range, "the requested area is negative",
                               request.node.str());
  }
  const SpaceNodeId plane = plane_owner_of(request.node);
  if (plane.empty()) {
    return Error::with_subject(ErrorCode::plane_not_declared,
                               "no node in the containment chain declares a plane",
                               request.node.str());
  }
  const internal::PlaneMeasure measure = internal::measure_plane(*this, plane, request.context);

  FitAssessment assessment = base_assessment(*this, request.node, *node, plane, request.context);
  assessment.units.state = MeasureState::unknown;

  if (measure.overflowed) {
    assessment.verdict = FitVerdict::indeterminate;
    assessment.explanations.add(ReasonCode::unknown_capacity, request.node.str(),
                                "checked arithmetic refused the plane ledger");
    return assessment;
  }
  if (measure.planned || !measure.usable) {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::node_not_available, request.node.str(),
                                "the plane offers no usable space now");
    return assessment;
  }
  if (measure.available.value() >= request.needed.value()) {
    assessment.verdict = FitVerdict::fits;
    assessment.explanations.add(
        ReasonCode::fits_contiguous, request.node.str(),
        "the plane holds " + to_text(measure.available) + " of free area, which is at least " +
            to_text(request.needed) +
            "; this verdict is an area comparison and claims no contiguous placement");
  } else {
    assessment.verdict = FitVerdict::does_not_fit;
    assessment.explanations.add(ReasonCode::insufficient_free_area, request.node.str(),
                                "the plane holds " + to_text(measure.available) +
                                    " of free area against a request for " +
                                    to_text(request.needed));
  }
  return assessment;
}

}  // namespace dccp::space_capacity
