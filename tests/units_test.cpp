// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - exact unit and extent algebra.
//
// What this file proves:
//   * rack unit intervals are half-open, so touching intervals never overlap and
//     [1,3) covers exactly the units 1 and 2;
//   * the inclusive {first, height} convention of the Physical Location Registry
//     round-trips with the half-open form;
//   * interval and rectangle sets normalize, sort, merge or refuse exactly as
//     documented, with the documented error codes;
//   * complement, free-run statistics, first-fit and fragmentation are exact
//     integer arithmetic, including the discarded remainder of the index;
//   * the union-area kernel counts overlapping geometry exactly once, and agrees
//     with an independent brute-force count over a coarse integer grid;
//   * text rendering of every unit and extent type is deterministic;
//   * the checked arithmetic helpers refuse to wrap rather than saturating.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/checked.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/units.hpp"

#include "test_support.hpp"

using namespace dccp::space_capacity;

namespace {

// A normalized set from the given intervals. The inputs below are all legal, so
// a refusal here is a defect and is reported as one by the caller's checks.
[[nodiscard]] IntervalSet make_set(std::vector<RackUnitInterval> intervals) {
  const Result<IntervalSet> built = IntervalSet::build(std::move(intervals));
  if (!built.ok()) return IntervalSet{};
  return built.value();
}

[[nodiscard]] RectSet make_rects(std::vector<PlanarRect> rects) {
  const Result<RectSet> built = RectSet::build(std::move(rects));
  if (!built.ok()) return RectSet{};
  return built.value();
}

template <typename T>
void expect_error(const Result<T>& result, ErrorCode expected) {
  SC_CHECK(!result.ok());
  if (result.ok()) return;
  SC_CHECK_EQ(result.error().code, expected);
}

// ---------------------------------------------------------------------------
// Rack unit intervals
// ---------------------------------------------------------------------------

void rack_unit_intervals_are_half_open() {
  SC_CASE("RackUnitInterval [1,3) covers 1 and 2 and not 3");
  const RackUnitInterval span = RackUnitInterval::make(1, 3);
  SC_CHECK(span.is_valid());
  SC_CHECK_EQ(span.count(), 2);
  SC_CHECK(span.contains(1));
  SC_CHECK(span.contains(2));
  SC_CHECK(!span.contains(3));
  SC_CHECK(!span.contains(0));

  // first < 1 is not a rack unit number.
  SC_CHECK(!RackUnitInterval::make(0, 3).is_valid());
  SC_CHECK(!RackUnitInterval::make(-1, 3).is_valid());
  // last <= first encloses nothing.
  SC_CHECK(!RackUnitInterval::make(1, 1).is_valid());
  SC_CHECK(!RackUnitInterval::make(3, 1).is_valid());
  // last > kMaxRackUnits + 1 is past the hard bound, and the bound itself is
  // accepted.
  SC_CHECK(!RackUnitInterval::make(1, Limits::kMaxRackUnits + 2).is_valid());
  const RackUnitInterval whole = RackUnitInterval::make(1, Limits::kMaxRackUnits + 1);
  SC_CHECK(whole.is_valid());
  SC_CHECK_EQ(whole.count(), Limits::kMaxRackUnits);
}

void interval_overlap_is_half_open() {
  SC_CASE("intervals_overlap is false for touching intervals");
  SC_CHECK(!intervals_overlap(RackUnitInterval::make(1, 3), RackUnitInterval::make(3, 5)));
  SC_CHECK(!intervals_overlap(RackUnitInterval::make(3, 5), RackUnitInterval::make(1, 3)));
  SC_CHECK(intervals_overlap(RackUnitInterval::make(1, 4), RackUnitInterval::make(3, 5)));
  SC_CHECK(intervals_overlap(RackUnitInterval::make(3, 5), RackUnitInterval::make(1, 4)));
  SC_CHECK(intervals_overlap(RackUnitInterval::make(1, 5), RackUnitInterval::make(2, 3)));
  SC_CHECK(!intervals_overlap(RackUnitInterval::make(5, 6), RackUnitInterval::make(1, 5)));
  // interval_contains is inclusive at both ends of the half-open form.
  SC_CHECK(interval_contains(RackUnitInterval::make(1, 5), RackUnitInterval::make(1, 5)));
  SC_CHECK(interval_contains(RackUnitInterval::make(1, 5), RackUnitInterval::make(3, 5)));
  SC_CHECK(!interval_contains(RackUnitInterval::make(3, 5), RackUnitInterval::make(3, 6)));
}

void inclusive_conversion() {
  SC_CASE("from_inclusive round-trips with inclusive_height");
  // Physical Location Registry stores {first, height} and means the units
  // first .. first + height - 1, so the half-open interval ends at first +
  // height: last = first + height.
  const RackUnitInterval converted = RackUnitInterval::from_inclusive(5, 3);
  SC_CHECK_EQ(converted.first, 5);
  SC_CHECK_EQ(converted.last, 5 + 3);
  SC_CHECK_EQ(converted.inclusive_height(), 3);
  SC_CHECK(converted.contains(5));
  SC_CHECK(converted.contains(7));
  SC_CHECK(!converted.contains(8));
  SC_CHECK_EQ(converted, RackUnitInterval::of_count(5, 3));

  for (std::int32_t first = 1; first <= 8; ++first) {
    for (std::int32_t height = 1; height <= 4; ++height) {
      const RackUnitInterval round = RackUnitInterval::from_inclusive(first, height);
      SC_CHECK(round.is_valid());
      SC_CHECK_EQ(round.first, first);
      SC_CHECK_EQ(round.last, first + height);
      SC_CHECK_EQ(round.inclusive_height(), height);
      SC_CHECK_EQ(round.count(), height);
    }
  }
}

// ---------------------------------------------------------------------------
// Interval sets
// ---------------------------------------------------------------------------

void interval_set_build() {
  SC_CASE("IntervalSet::build sorts and merges");
  // Touching intervals describe one run: [1,3) and [3,5) merge into [1,5).
  const IntervalSet touching =
      make_set({RackUnitInterval::make(1, 3), RackUnitInterval::make(3, 5)});
  SC_CHECK_EQ(touching.size(), std::size_t{1});
  SC_CHECK_EQ(touching.intervals().front(), RackUnitInterval::make(1, 5));
  SC_CHECK(touching.total().ok());
  SC_CHECK_EQ(touching.total().value, RackUnits{4});

  // Overlapping intervals merge as well.
  const IntervalSet overlapping =
      make_set({RackUnitInterval::make(1, 4), RackUnitInterval::make(3, 5)});
  SC_CHECK_EQ(overlapping.size(), std::size_t{1});
  SC_CHECK_EQ(overlapping.intervals().front(), RackUnitInterval::make(1, 5));

  // Disjoint intervals are sorted, and two input orders give one value.
  const IntervalSet first_order =
      make_set({RackUnitInterval::make(5, 6), RackUnitInterval::make(1, 3)});
  const IntervalSet second_order =
      make_set({RackUnitInterval::make(1, 3), RackUnitInterval::make(5, 6)});
  SC_CHECK_EQ(first_order, second_order);
  SC_CHECK_EQ(first_order.size(), std::size_t{2});
  SC_CHECK_EQ(first_order.intervals().front(), RackUnitInterval::make(1, 3));
  SC_CHECK_EQ(first_order.intervals().back(), RackUnitInterval::make(5, 6));
  SC_CHECK(first_order.contains_unit(1));
  SC_CHECK(first_order.contains_unit(5));
  SC_CHECK(!first_order.contains_unit(4));
  SC_CHECK(first_order.covers(RackUnitInterval::make(1, 3)));
  SC_CHECK(!first_order.covers(RackUnitInterval::make(1, 5)));
  SC_CHECK(first_order.intersects(RackUnitInterval::make(2, 7)));
  SC_CHECK(!first_order.intersects(RackUnitInterval::make(3, 5)));

  // An invalid interval is refused with invalid_extent.
  expect_error(IntervalSet::build({RackUnitInterval::make(0, 3)}), ErrorCode::invalid_extent);
  expect_error(IntervalSet::build({RackUnitInterval::make(1, 1)}), ErrorCode::invalid_extent);
  expect_error(IntervalSet::build({RackUnitInterval::make(1, Limits::kMaxRackUnits + 2)}),
               ErrorCode::invalid_extent);

  // One interval past the per-record bound is refused with limit_exceeded, and
  // exactly the bound is accepted.
  const auto bound = static_cast<std::int32_t>(Limits::kMaxExtentsPerRecord);
  std::vector<RackUnitInterval> too_many;
  std::vector<RackUnitInterval> at_bound;
  for (std::int32_t index = 0; index <= bound; ++index) {
    const RackUnitInterval interval =
        RackUnitInterval::make(1 + (2 * index), 2 + (2 * index));
    too_many.push_back(interval);
    if (index < bound) at_bound.push_back(interval);
  }
  expect_error(IntervalSet::build(too_many), ErrorCode::limit_exceeded);
  const Result<IntervalSet> accepted = IntervalSet::build(at_bound);
  SC_CHECK(accepted.ok());
  if (accepted.ok()) {
    SC_CHECK_EQ(accepted.value().size(), Limits::kMaxExtentsPerRecord);
    SC_CHECK_EQ(accepted.value().total().value, RackUnits{bound});
  }

  // An empty set is legal.
  const IntervalSet none;
  SC_CHECK(none.empty());
  SC_CHECK_EQ(none.size(), std::size_t{0});
  SC_CHECK_EQ(none.total().value, RackUnits{0});
}

void complement_and_free_runs() {
  SC_CASE("complement and free_run_stats are exact");
  const IntervalSet single = make_set({RackUnitInterval::make(2, 3)});
  const Result<IntervalSet> gap = IntervalSet::complement(single, 5);
  SC_CHECK(gap.ok());
  if (gap.ok()) {
    SC_CHECK_EQ(gap.value(), make_set({RackUnitInterval::make(1, 2), RackUnitInterval::make(3, 6)}));
    SC_CHECK_EQ(gap.value().size(), std::size_t{2});
    SC_CHECK_EQ(to_text(gap.value()), std::string("[1,2),[3,6)"));
  }

  // The complement of an empty set inside N units is [1, N + 1).
  const Result<IntervalSet> all_free = IntervalSet::complement(IntervalSet{}, 10);
  SC_CHECK(all_free.ok());
  if (all_free.ok()) {
    SC_CHECK_EQ(all_free.value(), make_set({RackUnitInterval::make(1, 11)}));
  }

  // The complement of a full envelope is empty.
  const Result<IntervalSet> no_free =
      IntervalSet::complement(make_set({RackUnitInterval::make(1, 11)}), 10);
  SC_CHECK(no_free.ok());
  if (no_free.ok()) SC_CHECK(no_free.value().empty());

  // An envelope of zero units and one past the bound are refused.
  expect_error(IntervalSet::complement(single, 0), ErrorCode::invalid_range);
  expect_error(IntervalSet::complement(single, Limits::kMaxRackUnits + 1), ErrorCode::invalid_range);

  // A hole in the middle leaves two free runs: [1,3) and [5,11).
  const Result<IntervalSet::FreeRunStats> stats =
      IntervalSet::free_run_stats(make_set({RackUnitInterval::make(3, 5)}), 10);
  SC_CHECK(stats.ok());
  if (stats.ok()) {
    SC_CHECK_EQ(stats.value().largest, 6);
    SC_CHECK_EQ(stats.value().run_count, std::uint32_t{2});
    SC_CHECK_EQ(stats.value().total_free, 8);
  }

  // No hole at all: one run covering the whole envelope.
  const Result<IntervalSet::FreeRunStats> whole =
      IntervalSet::free_run_stats(IntervalSet{}, 7);
  SC_CHECK(whole.ok());
  if (whole.ok()) {
    SC_CHECK_EQ(whole.value().largest, 7);
    SC_CHECK_EQ(whole.value().run_count, std::uint32_t{1});
    SC_CHECK_EQ(whole.value().total_free, 7);
  }
}

// ---------------------------------------------------------------------------
// First fit
// ---------------------------------------------------------------------------

void first_fit_alignment() {
  SC_CASE("first_fit returns the lowest aligned start");
  // A start position p is aligned when (p - 1) % alignment == 0, so alignment 4
  // accepts the positions 1, 5, 9, 13, ...
  //
  // Free space [5,13): the run starts at 5, and 5 is itself aligned, so the fit
  // begins there. (The brief that motivated this test said 9 is the first
  // aligned position at or after 5; (5 - 1) % 4 == 0, so 5 is. 9 is the answer
  // for a run that starts at 6, which is asserted below.)
  const IntervalSet free_from_5 =
      make_set({RackUnitInterval::make(1, 5), RackUnitInterval::make(13, 14)});
  const Result<RackUnitInterval> aligned_at_5 = IntervalSet::first_fit(free_from_5, 13, 3, 4);
  SC_CHECK(aligned_at_5.ok());
  if (aligned_at_5.ok()) {
    SC_CHECK_EQ(aligned_at_5.value(), RackUnitInterval::make(5, 8));
    SC_CHECK_EQ(aligned_at_5.value().count(), 3);
  }

  // Free space [6,13): the first aligned position at or after 6 is 9, and
  // [9,12) fits, so the fit is [9,12).
  const IntervalSet free_from_6 =
      make_set({RackUnitInterval::make(1, 6), RackUnitInterval::make(13, 14)});
  const Result<RackUnitInterval> aligned_at_9 = IntervalSet::first_fit(free_from_6, 13, 3, 4);
  SC_CHECK(aligned_at_9.ok());
  if (aligned_at_9.ok()) {
    SC_CHECK_EQ(aligned_at_9.value(), RackUnitInterval::make(9, 12));
  }

  // Alignment 1 accepts any position.
  const Result<RackUnitInterval> any_1 = IntervalSet::first_fit(make_set({RackUnitInterval::make(1, 5)}), 13, 3, 1);
  SC_CHECK(any_1.ok());
  if (any_1.ok()) SC_CHECK_EQ(any_1.value(), RackUnitInterval::make(5, 8));

  // A run too small for the request is skipped in favour of the next one.
  const IntervalSet two_runs = make_set({RackUnitInterval::make(1, 2), RackUnitInterval::make(4, 5)});
  const Result<RackUnitInterval> any_2 = IntervalSet::first_fit(two_runs, 13, 3, 1);
  SC_CHECK(any_2.ok());
  if (any_2.ok()) SC_CHECK_EQ(any_2.value(), RackUnitInterval::make(5, 8));

  // A run long enough for the request but with no aligned start inside it is
  // skipped: [4,6) holds 2 units, alignment 4 shifts the start to 5, and 5 + 2
  // leaves the run.
  const IntervalSet too_short = make_set({RackUnitInterval::make(1, 4), RackUnitInterval::make(6, 14)});
  const Result<RackUnitInterval> skipped = IntervalSet::first_fit(too_short, 13, 2, 4);
  SC_CHECK(skipped.ok());
  if (skipped.ok()) {
    SC_CHECK(!skipped.value().is_valid());
    SC_CHECK_EQ(skipped.value(), RackUnitInterval{});
  }

  // A later run that does satisfy the alignment is used.
  const IntervalSet later = make_set(
      {RackUnitInterval::make(1, 4), RackUnitInterval::make(6, 9), RackUnitInterval::make(13, 14)});
  const Result<RackUnitInterval> second_run = IntervalSet::first_fit(later, 13, 2, 4);
  SC_CHECK(second_run.ok());
  if (second_run.ok()) SC_CHECK_EQ(second_run.value(), RackUnitInterval::make(9, 11));

  // No run is large enough: the result is a default-constructed, invalid
  // interval rather than an error.
  const IntervalSet crumbs = make_set(
      {RackUnitInterval::make(1, 4), RackUnitInterval::make(5, 7), RackUnitInterval::make(8, 12)});
  const Result<RackUnitInterval> none = IntervalSet::first_fit(crumbs, 12, 5, 1);
  SC_CHECK(none.ok());
  if (none.ok()) {
    SC_CHECK(!none.value().is_valid());
    SC_CHECK_EQ(none.value(), RackUnitInterval{});
  }

  // Out-of-range requests are refused.
  expect_error(IntervalSet::first_fit(two_runs, 12, 0, 1), ErrorCode::invalid_range);
  expect_error(IntervalSet::first_fit(two_runs, 12, Limits::kMaxRackUnits + 1, 1),
               ErrorCode::invalid_range);
  expect_error(IntervalSet::first_fit(two_runs, 12, 1, 0), ErrorCode::invalid_range);
}

void first_fit_invariants() {
  SC_CASE("first_fit result never leaves the free space");
  sc_test::Rng rng(0x5CA1Eull);
  int fits_found = 0;
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const auto envelope = static_cast<std::int32_t>(rng.between(1, 64));
    const auto runs = static_cast<int>(rng.bounded(7));
    std::vector<RackUnitInterval> occupied;
    occupied.reserve(static_cast<std::size_t>(runs));
    for (int index = 0; index < runs; ++index) {
      const auto first = static_cast<std::int32_t>(rng.between(1, envelope));
      const auto last = static_cast<std::int32_t>(rng.between(first + 1, envelope + 1));
      occupied.push_back(RackUnitInterval::make(first, last));
    }
    const Result<IntervalSet> occupied_set = IntervalSet::build(occupied);
    SC_CHECK(occupied_set.ok());
    if (!occupied_set.ok()) continue;

    const auto needed = static_cast<std::int32_t>(rng.between(1, envelope + 3));
    const auto alignment = static_cast<std::int32_t>(rng.between(1, 8));
    const Result<RackUnitInterval> fit =
        IntervalSet::first_fit(occupied_set.value(), envelope, needed, alignment);
    SC_CHECK(fit.ok());
    if (!fit.ok()) continue;

    const Result<IntervalSet> free_space = IntervalSet::complement(occupied_set.value(), envelope);
    SC_CHECK(free_space.ok());
    if (!free_space.ok()) continue;

    const RackUnitInterval found = fit.value();
    if (!found.is_valid()) continue;  // no run satisfies the request
    ++fits_found;
    SC_CHECK_EQ(found.count(), needed);
    SC_CHECK_EQ((found.first - 1) % alignment, 0);
    SC_CHECK(found.first >= 1);
    SC_CHECK(found.last <= envelope + 1);
    // The fit lies inside the free space and touches no occupied unit.
    SC_CHECK(free_space.value().covers(found));
    for (std::int32_t unit = found.first; unit < found.last; ++unit) {
      SC_CHECK(!occupied_set.value().contains_unit(unit));
    }
  }
  SC_CHECK(fits_found > 0);
}

void fragmentation() {
  SC_CASE("fragmentation_ppm is integer division");
  // One free run: the whole free space is contiguous, so the index is zero.
  const Result<std::uint32_t> one_run =
      IntervalSet::fragmentation_ppm(make_set({RackUnitInterval::make(1, 4)}), 10);
  SC_CHECK(one_run.ok());
  if (one_run.ok()) SC_CHECK_EQ(one_run.value(), std::uint32_t{0});

  // No free space at all: zero, not a division by zero.
  const Result<std::uint32_t> full =
      IntervalSet::fragmentation_ppm(make_set({RackUnitInterval::make(1, 11)}), 10);
  SC_CHECK(full.ok());
  if (full.ok()) SC_CHECK_EQ(full.value(), std::uint32_t{0});

  // Occupied [3,5) in 10 units: free is [1,3) and [5,11), so total_free = 8 and
  // the largest run is 6. (8 - 6) * 1000000 / 8 = 250000 exactly.
  const Result<std::uint32_t> hole =
      IntervalSet::fragmentation_ppm(make_set({RackUnitInterval::make(3, 5)}), 10);
  SC_CHECK(hole.ok());
  if (hole.ok()) SC_CHECK_EQ(hole.value(), std::uint32_t{250000});

  // Occupied [4,5) and [8,9) in 9 units: free is [1,4), [5,8) and [9,10), so
  // total_free = 7 and the largest run is 3. The exact quotient is
  // (7 - 3) * 1000000 / 7 = 571428.571..., and integer division discards the
  // remainder, so the result is 571428 and not the rounded 571429.
  const Result<std::uint32_t> truncated = IntervalSet::fragmentation_ppm(
      make_set({RackUnitInterval::make(4, 5), RackUnitInterval::make(8, 9)}), 9);
  SC_CHECK(truncated.ok());
  if (truncated.ok()) SC_CHECK_EQ(truncated.value(), std::uint32_t{571428});
}

// ---------------------------------------------------------------------------
// Planar rectangles
// ---------------------------------------------------------------------------

void planar_rects() {
  SC_CASE("PlanarRect area, validity and degeneracy");
  const PlanarRect rect = PlanarRect::make(3, 4, 5, 7);
  SC_CHECK(rect.is_valid());
  SC_CHECK(!rect.is_degenerate());
  SC_CHECK_EQ(rect.right(), Millimeters{8});
  SC_CHECK_EQ(rect.bottom(), Millimeters{11});
  const Checked<SquareMillimeters> measured = rect.area();
  SC_CHECK(measured.ok());
  SC_CHECK_EQ(measured.value, SquareMillimeters{35});

  // Past the millimetre bound on either edge or either extent.
  SC_CHECK(!PlanarRect::make(0, 0, Limits::kMaxMillimeters + 1, 1).is_valid());
  SC_CHECK(!PlanarRect::make(0, 0, 1, Limits::kMaxMillimeters + 1).is_valid());
  SC_CHECK(!PlanarRect::make(Limits::kMaxMillimeters, 0, 2, 1).is_valid());
  SC_CHECK(!PlanarRect::make(0, Limits::kMaxMillimeters, 1, 2).is_valid());
  SC_CHECK(!PlanarRect::make(-1, 0, 10, 10).is_valid());
  SC_CHECK(!PlanarRect::make(0, -1, 10, 10).is_valid());
  // The bound itself is accepted, and its area is the declared area bound.
  SC_CHECK(PlanarRect::make(0, 0, Limits::kMaxMillimeters, Limits::kMaxMillimeters).is_valid());

  // A negative or zero extent encloses no area.
  SC_CHECK(PlanarRect::make(0, 0, -5, 5).is_degenerate());
  SC_CHECK(PlanarRect::make(0, 0, 5, -5).is_degenerate());
  SC_CHECK(PlanarRect::make(0, 0, 0, 5).is_degenerate());
  SC_CHECK(PlanarRect::make(0, 0, 5, 0).is_degenerate());
  SC_CHECK(!PlanarRect::make(0, 0, 1, 1).is_degenerate());
  SC_CHECK(!PlanarRect::make(0, 0, -5, 5).is_valid());
  SC_CHECK(!PlanarRect::make(0, 0, 0, 5).is_valid());
  const Checked<SquareMillimeters> zero = PlanarRect::make(0, 0, 0, 5).area();
  SC_CHECK(zero.ok());
  SC_CHECK_EQ(zero.value, SquareMillimeters{0});

  // Canonical order is by y, then x, then height, then width.
  SC_CHECK(PlanarRect::make(5, 0, 1, 1) < PlanarRect::make(0, 1, 1, 1));
  SC_CHECK(PlanarRect::make(0, 0, 1, 1) < PlanarRect::make(1, 0, 1, 1));
  SC_CHECK(PlanarRect::make(0, 0, 1, 1) < PlanarRect::make(0, 0, 1, 2));
  SC_CHECK(PlanarRect::make(0, 0, 1, 1) < PlanarRect::make(0, 0, 2, 1));
  SC_CHECK(PlanarRect::make(0, 0, 1, 1) == PlanarRect::make(0, 0, 1, 1));
  SC_CHECK(PlanarRect::make(0, 0, 1, 1) != PlanarRect::make(0, 0, 1, 2));

  // Containment uses the half-open far edges.
  SC_CHECK(rect_is_within(PlanarRect::make(1, 1, 1, 1), PlanarRect::make(0, 0, 3, 3)));
  SC_CHECK(rect_is_within(PlanarRect::make(0, 0, 3, 3), PlanarRect::make(0, 0, 3, 3)));
  SC_CHECK(!rect_is_within(PlanarRect::make(0, 0, 3, 3), PlanarRect::make(1, 1, 1, 1)));
  SC_CHECK(!rect_is_within(PlanarRect::make(2, 2, 2, 2), PlanarRect::make(0, 0, 3, 3)));
}

void rect_intersections() {
  SC_CASE("rect_intersects and rect_intersection_area");
  const PlanarRect left = PlanarRect::make(0, 0, 10, 10);
  const PlanarRect right = PlanarRect::make(10, 0, 10, 10);
  const PlanarRect above = PlanarRect::make(0, 10, 10, 10);
  const PlanarRect overlap = PlanarRect::make(5, 5, 10, 10);
  const PlanarRect inside = PlanarRect::make(2, 2, 3, 3);
  const PlanarRect far_away = PlanarRect::make(100, 100, 10, 10);

  // Touching on an edge is not an overlap.
  SC_CHECK(!rect_intersects(left, right));
  SC_CHECK(!rect_intersects(left, above));
  SC_CHECK(!rect_intersects(right, left));
  SC_CHECK(!rect_intersects(left, far_away));
  // A degenerate rectangle intersects nothing.
  SC_CHECK(!rect_intersects(left, PlanarRect::make(5, 5, 0, 5)));
  SC_CHECK(rect_intersects(left, overlap));
  SC_CHECK(rect_intersects(left, inside));
  SC_CHECK(rect_intersects(inside, left));

  const Checked<SquareMillimeters> overlap_area = rect_intersection_area(left, overlap);
  SC_CHECK(overlap_area.ok());
  SC_CHECK_EQ(overlap_area.value, SquareMillimeters{25});
  SC_CHECK_EQ(rect_intersection_area(left, right).value, SquareMillimeters{0});
  SC_CHECK_EQ(rect_intersection_area(left, far_away).value, SquareMillimeters{0});
  SC_CHECK_EQ(rect_intersection_area(left, above).value, SquareMillimeters{0});
  // Containment: the intersection is the contained rectangle.
  SC_CHECK_EQ(rect_intersection_area(left, inside).value, SquareMillimeters{9});
  SC_CHECK_EQ(rect_intersection_area(inside, left).value, SquareMillimeters{9});
  SC_CHECK_EQ(rect_intersection_area(left, left).value, SquareMillimeters{100});
}

void rect_set_build() {
  SC_CASE("RectSet::build refuses overlap and sorts canonically");
  // Two rectangles of one record that overlap are a modelling error.
  expect_error(
      RectSet::build({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(5, 5, 10, 10)}),
      ErrorCode::overlap);
  // An invalid rectangle is refused before any overlap question is asked.
  expect_error(RectSet::build({PlanarRect::make(0, 0, 0, 10)}), ErrorCode::invalid_extent);

  // Touching rectangles do not overlap and are accepted.
  const RectSet touching =
      make_rects({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(10, 0, 10, 10)});
  SC_CHECK_EQ(touching.size(), std::size_t{2});
  SC_CHECK(touching.total_area().ok());
  SC_CHECK_EQ(touching.total_area().value, SquareMillimeters{200});
  SC_CHECK(touching.intersects(PlanarRect::make(5, 0, 10, 10)));
  SC_CHECK(!touching.intersects(PlanarRect::make(20, 0, 10, 10)));
  // The members are disjoint, so the intersection with a query rectangle is
  // the sum over the members it meets: (5,0,10,10) spans x 5..15 and meets both
  // members, 50 square millimetres each, while (5,0,4,10) meets only the first.
  SC_CHECK_EQ(touching.intersection_area(PlanarRect::make(5, 0, 10, 10)).value,
              SquareMillimeters{100});
  SC_CHECK_EQ(touching.intersection_area(PlanarRect::make(5, 0, 4, 10)).value,
              SquareMillimeters{40});
  SC_CHECK_EQ(touching.intersection_area(PlanarRect::make(50, 0, 4, 10)).value,
              SquareMillimeters{0});

  // The canonical order is by (y, x, height, width), whatever the input order.
  const RectSet sorted =
      make_rects({PlanarRect::make(20, 0, 10, 10), PlanarRect::make(0, 0, 10, 10)});
  SC_CHECK_EQ(sorted.size(), std::size_t{2});
  SC_CHECK_EQ(sorted.rects().front(), PlanarRect::make(0, 0, 10, 10));
  SC_CHECK_EQ(sorted.rects().back(), PlanarRect::make(20, 0, 10, 10));
  SC_CHECK_EQ(sorted,
              make_rects({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(20, 0, 10, 10)}));
  SC_CHECK_EQ(sorted.total_area().value, SquareMillimeters{200});

  const RectSet empty;
  SC_CHECK(empty.empty());
  SC_CHECK_EQ(empty.total_area().value, SquareMillimeters{0});
}

// ---------------------------------------------------------------------------
// Union measure
// ---------------------------------------------------------------------------

void union_area() {
  SC_CASE("rect_union_area counts overlapping geometry once");
  // Two identical rectangles: the union is one of them, not twice one of them.
  const Checked<SquareMillimeters> identical =
      rect_union_area({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(0, 0, 10, 10)});
  SC_CHECK(identical.ok());
  SC_CHECK_EQ(identical.value, SquareMillimeters{100});

  // Containment: the union is the outer rectangle.
  SC_CHECK_EQ(rect_union_area({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(2, 2, 3, 3)}).value,
              SquareMillimeters{100});
  SC_CHECK_EQ(rect_union_area({PlanarRect::make(2, 2, 3, 3), PlanarRect::make(0, 0, 10, 10)}).value,
              SquareMillimeters{100});

  // Three rectangles that overlap so that the union is exactly known:
  //   A (0,0,4,4), B (2,0,4,4), C (0,2,4,4)
  // The areas sum to 48, the union is 32:
  //   rows y 0..1: A + B cover x 0..6            -> 6 * 2 = 12
  //   rows y 2..3: A + B + C cover x 0..6        -> 6 * 2 = 12
  //   rows y 4..5: C alone covers x 0..4         -> 4 * 2 =  8
  const std::vector<PlanarRect> three{PlanarRect::make(0, 0, 4, 4), PlanarRect::make(2, 0, 4, 4),
                                      PlanarRect::make(0, 2, 4, 4)};
  const Checked<SquareMillimeters> union_three = rect_union_area(three);
  SC_CHECK(union_three.ok());
  SC_CHECK_EQ(union_three.value, SquareMillimeters{32});

  // Disjoint rectangles sum exactly, and touching ones are not counted twice.
  SC_CHECK_EQ(rect_union_area({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(10, 0, 10, 10)}).value,
              SquareMillimeters{200});
  // Nothing at all, and nothing that encloses area.
  SC_CHECK_EQ(rect_union_area({}).value, SquareMillimeters{0});
  SC_CHECK_EQ(
      rect_union_area({PlanarRect::make(0, 0, 0, 10), PlanarRect::make(0, 0, 10, 0)}).value,
      SquareMillimeters{0});

  // The RectSet form measures the same union.
  const RectSet pair =
      make_rects({PlanarRect::make(0, 0, 10, 10), PlanarRect::make(10, 0, 10, 10)});
  const Checked<SquareMillimeters> set_union = rect_set_union_area(pair);
  SC_CHECK(set_union.ok());
  SC_CHECK_EQ(set_union.value, SquareMillimeters{200});
}

void union_area_against_brute_force() {
  SC_CASE("rect_union_area equals a brute-force grid count");
  // Forty pseudo-random rectangles, every coordinate a multiple of 10 mm, so a
  // 10 mm grid cell is covered exactly when its lower-left corner is inside some
  // rectangle. The union then equals (number of covered cells) * (cell area).
  sc_test::Rng rng(0x5EEDC0DEull);
  constexpr std::int64_t kCell = 10;
  constexpr std::int64_t kCells = 19;  // 19 x 19 cells cover 0 .. 189 mm
  constexpr std::int64_t kCellArea = kCell * kCell;

  std::vector<PlanarRect> generated;
  generated.reserve(40);
  for (int index = 0; index < 40; ++index) {
    const std::int64_t x = kCell * rng.between(0, 11);
    const std::int64_t y = kCell * rng.between(0, 11);
    const std::int64_t width = kCell * rng.between(1, 8);
    const std::int64_t height = kCell * rng.between(1, 8);
    const PlanarRect candidate = PlanarRect::make(x, y, width, height);
    SC_CHECK(candidate.is_valid());
    generated.push_back(candidate);
  }

  std::vector<char> covered(static_cast<std::size_t>(kCells * kCells), 0);
  for (const PlanarRect& rect : generated) {
    const std::int64_t first_x = rect.x.value() / kCell;
    const std::int64_t last_x = rect.right().value() / kCell;
    const std::int64_t first_y = rect.y.value() / kCell;
    const std::int64_t last_y = rect.bottom().value() / kCell;
    for (std::int64_t cell_x = first_x; cell_x < last_x; ++cell_x) {
      for (std::int64_t cell_y = first_y; cell_y < last_y; ++cell_y) {
        covered[static_cast<std::size_t>((cell_y * kCells) + cell_x)] = 1;
      }
    }
  }
  std::int64_t marked = 0;
  for (const char cell : covered) {
    if (cell != 0) ++marked;
  }
  SC_CHECK(marked > 0);

  const Checked<SquareMillimeters> measured = rect_union_area(generated);
  SC_CHECK(measured.ok());
  SC_CHECK_EQ(measured.value.value(), marked * kCellArea);

  // Sanity: a union is never larger than the sum of its parts, and never
  // smaller than any one part.
  std::int64_t summed = 0;
  std::int64_t largest = 0;
  for (const PlanarRect& rect : generated) {
    const Checked<SquareMillimeters> area = rect.area();
    SC_CHECK(area.ok());
    if (!area.ok()) continue;
    summed += area.value.value();
    if (area.value.value() > largest) largest = area.value.value();
  }
  SC_CHECK(measured.value.value() <= summed);
  SC_CHECK(measured.value.value() >= largest);
}

void overlapping_pairs() {
  SC_CASE("find_overlapping_rect_pair reports the first overlapping pair");
  const std::vector<PlanarRect> disjoint{PlanarRect::make(0, 0, 10, 10),
                                         PlanarRect::make(20, 0, 10, 10),
                                         PlanarRect::make(40, 0, 10, 10)};
  std::size_t first = 99;
  std::size_t second = 99;
  SC_CHECK(!find_overlapping_rect_pair(disjoint, first, second));
  SC_CHECK_EQ(first, std::size_t{99});
  SC_CHECK_EQ(second, std::size_t{99});

  const std::vector<PlanarRect> overlapping{PlanarRect::make(0, 0, 10, 10),
                                            PlanarRect::make(20, 0, 5, 5),
                                            PlanarRect::make(5, 5, 10, 10)};
  SC_CHECK(find_overlapping_rect_pair(overlapping, first, second));
  SC_CHECK_EQ(first, std::size_t{0});
  SC_CHECK_EQ(second, std::size_t{2});

  const std::vector<PlanarRect> adjacent{PlanarRect::make(0, 0, 10, 10),
                                         PlanarRect::make(5, 0, 10, 10),
                                         PlanarRect::make(30, 30, 10, 10)};
  SC_CHECK(find_overlapping_rect_pair(adjacent, first, second));
  SC_CHECK_EQ(first, std::size_t{0});
  SC_CHECK_EQ(second, std::size_t{1});
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void text_rendering() {
  SC_CASE("to_text renders units and extents deterministically");
  SC_CHECK_EQ(to_text(Millimeters{0}), std::string("0mm"));
  SC_CHECK_EQ(to_text(Millimeters{1234}), std::string("1234mm"));
  SC_CHECK_EQ(to_text(Millimeters{-42}), std::string("-42mm"));
  SC_CHECK_EQ(to_text(Millimeters{Limits::kMaxMillimeters}), std::string("10000000mm"));
  SC_CHECK_EQ(to_text(SquareMillimeters{0}), std::string("0mm2"));
  SC_CHECK_EQ(to_text(SquareMillimeters{35}), std::string("35mm2"));
  SC_CHECK_EQ(to_text(SquareMillimeters{-1}), std::string("-1mm2"));
  SC_CHECK_EQ(to_text(RackUnits{0}), std::string("0U"));
  SC_CHECK_EQ(to_text(RackUnits{42}), std::string("42U"));
  // A one-unit interval names one unit; a wider one names both end units, and
  // both spell the half-open form.
  SC_CHECK_EQ(to_text(RackUnitInterval::make(5, 6)), std::string("U5 [5,6)"));
  SC_CHECK_EQ(to_text(RackUnitInterval::make(1, 3)), std::string("U1-U2 [1,3)"));
  SC_CHECK_EQ(to_text(RackUnitInterval{}), std::string("U0 [0,0)"));
  SC_CHECK_EQ(to_text(IntervalSet{}), std::string("{}"));
  SC_CHECK_EQ(to_text(make_set({RackUnitInterval::make(1, 3), RackUnitInterval::make(5, 6)})),
              std::string("[1,3),[5,6)"));
  SC_CHECK_EQ(to_text(PlanarRect::make(0, 0, 10, 10)), std::string("(0,0) 10x10mm"));
  SC_CHECK_EQ(to_text(RectSet{}), std::string("{}"));
  SC_CHECK_EQ(to_text(make_rects({PlanarRect::make(0, 0, 10, 10)})), std::string("(0,0) 10x10mm"));
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

void checked_arithmetic() {
  SC_CASE("checked arithmetic refuses to wrap");
  constexpr std::uint64_t kMaxU64 = (std::numeric_limits<std::uint64_t>::max)();
  constexpr std::int64_t kMaxI64 = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t kMinI64 = (std::numeric_limits<std::int64_t>::min)();

  const Checked<std::uint64_t> sum = checked_add<std::uint64_t>(std::uint64_t{1}, std::uint64_t{2});
  SC_CHECK(sum.ok());
  SC_CHECK_EQ(sum.value, std::uint64_t{3});

  const Checked<std::uint64_t> wrapped = checked_add<std::uint64_t>(kMaxU64, std::uint64_t{1});
  SC_CHECK(!wrapped.ok());
  SC_CHECK_EQ(wrapped.value, std::uint64_t{0});

  // 2^32 * 2^32 is 2^64, which does not fit.
  const Checked<std::uint64_t> product =
      checked_mul<std::uint64_t>(std::uint64_t{1} << 32, std::uint64_t{1} << 32);
  SC_CHECK(!product.ok());
  SC_CHECK_EQ(product.value, std::uint64_t{0});

  const Checked<std::uint64_t> scaled =
      checked_mul<std::uint64_t>(std::uint64_t{1000}, std::uint64_t{1000});
  SC_CHECK(scaled.ok());
  SC_CHECK_EQ(scaled.value, std::uint64_t{1000000});
  const Checked<std::uint64_t> zero_product = checked_mul<std::uint64_t>(std::uint64_t{0}, kMaxU64);
  SC_CHECK(zero_product.ok());
  SC_CHECK_EQ(zero_product.value, std::uint64_t{0});

  const Checked<std::uint64_t> borrowed =
      checked_sub<std::uint64_t>(std::uint64_t{0}, std::uint64_t{1});
  SC_CHECK(!borrowed.ok());
  const Checked<std::uint64_t> difference =
      checked_sub<std::uint64_t>(std::uint64_t{5}, std::uint64_t{3});
  SC_CHECK(difference.ok());
  SC_CHECK_EQ(difference.value, std::uint64_t{2});
  const Checked<std::uint64_t> incremented = checked_increment<std::uint64_t>(kMaxU64);
  SC_CHECK(!incremented.ok());

  // Saturating subtraction clamps at zero instead of wrapping.
  SC_CHECK_EQ(saturating_sub<std::int64_t>(std::int64_t{5}, std::int64_t{3}), std::int64_t{2});
  SC_CHECK_EQ(saturating_sub<std::int64_t>(std::int64_t{3}, std::int64_t{5}), std::int64_t{0});
  SC_CHECK_EQ(saturating_sub<std::int64_t>(std::int64_t{-5}, std::int64_t{-3}), std::int64_t{0});
  SC_CHECK_EQ(saturating_sub<std::int64_t>(kMinI64, std::int64_t{1}), std::int64_t{0});
  SC_CHECK_EQ(saturating_sub<std::int64_t>(kMaxI64, std::int64_t{-1}), std::int64_t{0});

  // Signed multiplication refuses INT64_MIN * -1 in both argument orders, and
  // refuses to leave the range in either direction.
  const Checked<std::int64_t> min_times_negative = checked_mul_signed<std::int64_t>(kMinI64, std::int64_t{-1});
  SC_CHECK(!min_times_negative.ok());
  SC_CHECK_EQ(min_times_negative.value, std::int64_t{0});
  const Checked<std::int64_t> negative_times_min = checked_mul_signed<std::int64_t>(std::int64_t{-1}, kMinI64);
  SC_CHECK(!negative_times_min.ok());
  const Checked<std::int64_t> signed_product = checked_mul_signed<std::int64_t>(std::int64_t{-3}, std::int64_t{4});
  SC_CHECK(signed_product.ok());
  SC_CHECK_EQ(signed_product.value, std::int64_t{-12});
  const Checked<std::int64_t> signed_wrap = checked_mul_signed<std::int64_t>(kMaxI64, std::int64_t{2});
  SC_CHECK(!signed_wrap.ok());
  const Checked<std::int64_t> signed_min = checked_mul_signed<std::int64_t>(kMinI64, std::int64_t{1});
  SC_CHECK(signed_min.ok());
  SC_CHECK_EQ(signed_min.value, kMinI64);
}

}  // namespace

int main() {
  rack_unit_intervals_are_half_open();
  interval_overlap_is_half_open();
  inclusive_conversion();
  interval_set_build();
  complement_and_free_runs();
  first_fit_alignment();
  first_fit_invariants();
  fragmentation();
  planar_rects();
  rect_intersections();
  rect_set_build();
  union_area();
  union_area_against_brute_force();
  overlapping_pairs();
  text_rendering();
  checked_arithmetic();
  return ::sc_test::summary("units_test");
}
