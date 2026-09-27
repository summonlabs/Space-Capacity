// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal geometry kernels.
//
// Two exact routines back the whole accounting layer:
//
//   * overlap detection, which decides whether two rectangles of one record
//     collide, and which the audit pass uses to find committed claims that
//     cover the same space;
//   * union measure, which is what makes the ledger incapable of double
//     counting. Two exclusions over the same area measure once; two claims
//     over the same area measure once and are separately refused.
//
// Union measure is implemented as a sweep over x with a segment tree over
// compressed y coordinates, which is exact for arbitrary overlapping input and
// costs O(n log n). It is checked against a brute-force unit-grid reference
// model by a property test.

#include "geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dccp::space_capacity::internal {
namespace {

struct SweepEntry final {
  std::int64_t x_begin = 0;
  std::int64_t x_end = 0;
  std::int64_t y_begin = 0;
  std::int64_t y_end = 0;
  std::size_t index = 0;
};

// Segment tree over compressed y coordinates that maintains the total covered
// length of the active set. `count` records how many active rectangles fully
// cover a node's span; `covered` records the measure that is covered by at
// least one rectangle.
class CoverageTree final {
 public:
  CoverageTree(const std::vector<std::int64_t>& coordinates, std::size_t segments)
      : coordinates_(coordinates),
        count_(4 * (segments == 0 ? 1 : segments), 0),
        covered_(4 * (segments == 0 ? 1 : segments), 0),
        segments_(segments) {}

  void update(std::size_t node, std::size_t left, std::size_t right, std::size_t query_left,
              std::size_t query_right, int delta) noexcept {
    if (query_right <= left || right <= query_left) return;
    if (query_left <= left && right <= query_right) {
      count_[node] += delta;
    } else {
      const std::size_t middle = left + (right - left) / 2;
      update(node * 2, left, middle, query_left, query_right, delta);
      update(node * 2 + 1, middle, right, query_left, query_right, delta);
    }
    if (count_[node] > 0) {
      covered_[node] = coordinates_[right] - coordinates_[left];
    } else if (right - left == 1) {
      covered_[node] = 0;
    } else {
      covered_[node] = covered_[node * 2] + covered_[node * 2 + 1];
    }
  }

  [[nodiscard]] std::int64_t total_covered() const noexcept {
    return segments_ == 0 ? 0 : covered_[1];
  }

 private:
  const std::vector<std::int64_t>& coordinates_;
  std::vector<int> count_;
  std::vector<std::int64_t> covered_;
  std::size_t segments_ = 0;
};

}  // namespace

bool find_overlapping_pair(const std::vector<PlanarRect>& rects, std::size_t& first,
                                std::size_t& second) noexcept {
  std::vector<SweepEntry> entries;
  entries.reserve(rects.size());
  for (std::size_t i = 0; i < rects.size(); ++i) {
    const PlanarRect& rect = rects[i];
    if (rect.is_degenerate()) continue;
    entries.push_back(SweepEntry{rect.x.value(), rect.right().value(), rect.y.value(),
                                 rect.bottom().value(), i});
  }
  std::sort(entries.begin(), entries.end(), [](const SweepEntry& a, const SweepEntry& b) {
    if (a.x_begin != b.x_begin) return a.x_begin < b.x_begin;
    return a.index < b.index;
  });

  std::vector<SweepEntry> active;
  for (const SweepEntry& current : entries) {
    // Drop entries whose x range ended at or before this one begins.
    std::size_t kept = 0;
    for (std::size_t j = 0; j < active.size(); ++j) {
      if (active[j].x_end > current.x_begin) {
        active[kept++] = active[j];
      }
    }
    active.resize(kept);
    for (const SweepEntry& other : active) {
      if (current.y_begin < other.y_end && other.y_begin < current.y_end) {
        first = other.index;
        second = current.index;
        if (first > second) std::swap(first, second);
        return true;
      }
    }
    active.push_back(current);
    std::sort(active.begin(), active.end(), [](const SweepEntry& a, const SweepEntry& b) {
      if (a.y_begin != b.y_begin) return a.y_begin < b.y_begin;
      if (a.y_end != b.y_end) return a.y_end < b.y_end;
      return a.index < b.index;
    });
  }
  return false;
}

Checked<SquareMillimeters> measure_union_area(const std::vector<PlanarRect>& rects) noexcept {
  Checked<SquareMillimeters> result;

  struct Event final {
    std::int64_t x = 0;
    std::int64_t y_begin = 0;
    std::int64_t y_end = 0;
    int delta = 0;
  };

  std::vector<Event> events;
  std::vector<std::int64_t> coordinates;
  events.reserve(rects.size() * 2);
  coordinates.reserve(rects.size() * 2);
  for (const PlanarRect& rect : rects) {
    if (rect.is_degenerate()) continue;
    if (!rect.is_valid()) {
      result.overflowed = true;
      return result;
    }
    events.push_back(Event{rect.x.value(), rect.y.value(), rect.bottom().value(), 1});
    events.push_back(Event{rect.right().value(), rect.y.value(), rect.bottom().value(), -1});
    coordinates.push_back(rect.y.value());
    coordinates.push_back(rect.bottom().value());
  }
  if (events.empty()) {
    result.value = SquareMillimeters{0};
    return result;
  }

  std::sort(coordinates.begin(), coordinates.end());
  coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
  const std::size_t segments = coordinates.size() - 1;

  std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    if (a.x != b.x) return a.x < b.x;
    // Closings before openings at the same x keeps the sweep from measuring a
    // zero-width slab; the group is applied together anyway, so the order only
    // affects the intermediate state and never the total.
    return a.delta < b.delta;
  });

  CoverageTree tree(coordinates, segments);
  std::int64_t total = 0;
  std::int64_t previous_x = events.front().x;
  std::size_t i = 0;
  while (i < events.size()) {
    const std::size_t group_start = i;
    const std::int64_t x = events[i].x;
    const std::int64_t width = x - previous_x;
    if (width > 0) {
      const std::int64_t covered = tree.total_covered();
      if (covered > 0) {
        const Checked<std::int64_t> slab = checked_mul_signed(width, covered);
        if (!slab) {
          result.overflowed = true;
          return result;
        }
        const Checked<std::int64_t> running = checked_add_signed(total, slab.value);
        if (!running || running.value > Limits::kMaxSquareMillimeters) {
          result.overflowed = true;
          return result;
        }
        total = running.value;
      }
    }
    while (i < events.size() && events[i].x == x) ++i;
    for (std::size_t j = group_start; j < i; ++j) {
      const Event& event = events[j];
      const auto begin_index =
          static_cast<std::size_t>(std::lower_bound(coordinates.begin(), coordinates.end(),
                                                    event.y_begin) -
                                   coordinates.begin());
      const auto end_index = static_cast<std::size_t>(
          std::lower_bound(coordinates.begin(), coordinates.end(), event.y_end) -
          coordinates.begin());
      if (end_index > begin_index) {
        tree.update(1, 0, segments, begin_index, end_index, event.delta);
      }
    }
    previous_x = x;
  }

  result.value = SquareMillimeters{total};
  return result;
}

}  // namespace dccp::space_capacity::internal

namespace dccp::space_capacity {

// The public wrappers. They exist so that the union kernel can be exercised and
// cited directly from a test or a tool without going through a whole snapshot.
Checked<SquareMillimeters> rect_union_area(const std::vector<PlanarRect>& rects) noexcept {
  return internal::measure_union_area(rects);
}

Checked<SquareMillimeters> rect_set_union_area(const RectSet& rects) noexcept {
  return internal::measure_union_area(rects.rects());
}

bool find_overlapping_rect_pair(const std::vector<PlanarRect>& rects, std::size_t& first,
                                std::size_t& second) noexcept {
  return internal::find_overlapping_pair(rects, first, second);
}

}  // namespace dccp::space_capacity