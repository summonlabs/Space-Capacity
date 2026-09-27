// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal geometry kernels: rectangle overlap detection and exact union
// measure. Not installed, not part of the public API.

#pragma once

#include <cstddef>
#include <vector>

#include "dccp/space_capacity/checked.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity::internal {

// Finds the first pair of overlapping rectangles in the input order, if any.
// Sweep over x with an active set keyed on y: two rectangles clash exactly
// when their x ranges overlap and their y intervals overlap. Cost is
// O(n log n) plus the cost of the overlaps that actually exist.
bool find_overlapping_pair(const std::vector<PlanarRect>& rects, std::size_t& first,
                                std::size_t& second) noexcept;

// Exact measure of the union of the rectangles. Overlaps are counted once.
// Sweep over x with a segment tree over compressed y coordinates, so the cost
// is O(n log n) and the result is exact for any input, overlapping or not.
Checked<SquareMillimeters> measure_union_area(const std::vector<PlanarRect>& rects) noexcept;

}  // namespace dccp::space_capacity::internal
