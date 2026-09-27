// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - exact unit and extent algebra for physical space.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Authoritative accounting is exact integer accounting. There is no floating
// point anywhere in a capacity number, a footprint, a clearance, a rollup or a
// fragmentation index; every derived quantity is an integer or an exact
// integer ratio reported in parts per million.
//
// Units
//   * planar distance      millimetres          (int64)
//   * planar area          square millimetres   (int64)
//   * vertical rack space  whole rack units     (int32), 1-based
//   * counts               unsigned 64-bit
//
// Rack unit convention
//   Space Capacity uses HALF-OPEN intervals: [first, last) contains `first`
//   and excludes `last`. Two intervals that merely touch, [1,3) and [3,5), do
//   not overlap. This matches Rack Registry's RackUnitRange and Asset
//   Registry's UnitSpan. Physical Location Registry instead stores an
//   inclusive envelope {first, height} whose last unit is first + height - 1.
//   The conversions are explicit and live here:
//
//     inclusive_to_half_open(first, height) -> [first, first + height)
//     half_open_to_inclusive(interval)      -> {first, last - first}
//
//   The sibling caps differ as well: Physical Location Registry and Asset
//   Registry allow unit numbers up to 512, Rack Registry up to 1024, and Space
//   Capacity's own hard bound is Limits::kMaxRackUnits = 2048. An envelope
//   wider than a sibling's cap is representable here and is not representable
//   there; that difference is the operator's to resolve, and Space Capacity
//   never narrows a value silently.
//
// Zero and unknown
//   "Zero" and "unknown" are different values and are not interchangeable. An
//   area of zero square millimetres is a measured zero; an absent measurement
//   is an absent optional or a declared MeasureState, never a zero.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/checked.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Scalar unit wrappers
// ---------------------------------------------------------------------------

class SC_API Millimeters final {
 public:
  constexpr Millimeters() noexcept = default;
  constexpr explicit Millimeters(std::int64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

  [[nodiscard]] friend constexpr bool operator==(Millimeters a, Millimeters b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(Millimeters a, Millimeters b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(Millimeters a, Millimeters b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<=(Millimeters a, Millimeters b) noexcept {
    return a.value_ <= b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>(Millimeters a, Millimeters b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend constexpr bool operator>=(Millimeters a, Millimeters b) noexcept {
    return b <= a;
  }

 private:
  std::int64_t value_ = 0;
};

class SC_API SquareMillimeters final {
 public:
  constexpr SquareMillimeters() noexcept = default;
  constexpr explicit SquareMillimeters(std::int64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

  [[nodiscard]] friend constexpr bool operator==(SquareMillimeters a, SquareMillimeters b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(SquareMillimeters a, SquareMillimeters b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(SquareMillimeters a, SquareMillimeters b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<=(SquareMillimeters a, SquareMillimeters b) noexcept {
    return a.value_ <= b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>(SquareMillimeters a, SquareMillimeters b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend constexpr bool operator>=(SquareMillimeters a, SquareMillimeters b) noexcept {
    return b <= a;
  }

 private:
  std::int64_t value_ = 0;
};

// A count of whole rack units. Space Capacity stores the count and never
// derives millimetres from it: rack pitch is a rack property owned by Rack
// Registry.
class SC_API RackUnits final {
 public:
  constexpr RackUnits() noexcept = default;
  constexpr explicit RackUnits(std::int32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::int32_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  [[nodiscard]] friend constexpr bool operator==(RackUnits a, RackUnits b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(RackUnits a, RackUnits b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend constexpr bool operator<(RackUnits a, RackUnits b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<=(RackUnits a, RackUnits b) noexcept {
    return a.value_ <= b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>(RackUnits a, RackUnits b) noexcept {
    return b < a;
  }
  [[nodiscard]] friend constexpr bool operator>=(RackUnits a, RackUnits b) noexcept {
    return b <= a;
  }

 private:
  std::int32_t value_ = 0;
};

// Which physical quantity a number is expressed in. Carried alongside generic
// counts so that a mismatch is reported as invalid_units rather than silently
// combined.
enum class UnitKind : std::uint8_t {
  none = 0,
  millimeters = 1,
  square_millimeters = 2,
  rack_units = 3,
  count = 4,
};

SC_API std::string_view unit_kind_name(UnitKind kind) noexcept;
SC_API bool parse_unit_kind(std::string_view text, UnitKind& out) noexcept;

// ---------------------------------------------------------------------------
// Checked arithmetic on units
// ---------------------------------------------------------------------------

SC_API Checked<Millimeters> add(Millimeters a, Millimeters b) noexcept;
SC_API Checked<Millimeters> sub(Millimeters a, Millimeters b) noexcept;

SC_API Checked<SquareMillimeters> add(SquareMillimeters a, SquareMillimeters b) noexcept;
SC_API Checked<SquareMillimeters> sub(SquareMillimeters a, SquareMillimeters b) noexcept;

SC_API Checked<RackUnits> add(RackUnits a, RackUnits b) noexcept;
SC_API Checked<RackUnits> sub(RackUnits a, RackUnits b) noexcept;

// ---------------------------------------------------------------------------
// Planar rectangles
// ---------------------------------------------------------------------------

// An axis-aligned rectangle in a coordinate system local to the containing
// node. Width runs along x, depth along y. Space Capacity makes no claim about
// which cardinal direction x or y means; that is a site-orientation property
// owned upstream.
struct SC_API PlanarRect final {
  Millimeters x{};
  Millimeters y{};
  Millimeters width{};
  Millimeters height{};

  // True when the rectangle encloses no area.
  [[nodiscard]] bool is_degenerate() const noexcept;
  // True when the rectangle is inside the declared millimetre bounds and its
  // far edges can be formed without overflow. Every rectangle that has passed
  // validation satisfies this, which is what makes right() and bottom() exact
  // rather than merely unchecked.
  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] Checked<SquareMillimeters> area() const noexcept;
  [[nodiscard]] Millimeters right() const noexcept {
    return Millimeters{x.value() + width.value()};
  }
  [[nodiscard]] Millimeters bottom() const noexcept {
    return Millimeters{y.value() + height.value()};
  }

  [[nodiscard]] static PlanarRect make(std::int64_t x, std::int64_t y, std::int64_t width,
                                       std::int64_t height) noexcept {
    PlanarRect rect;
    rect.x = Millimeters{x};
    rect.y = Millimeters{y};
    rect.width = Millimeters{width};
    rect.height = Millimeters{height};
    return rect;
  }

  [[nodiscard]] friend bool operator==(const PlanarRect& a, const PlanarRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }
  [[nodiscard]] friend bool operator!=(const PlanarRect& a, const PlanarRect& b) noexcept {
    return !(a == b);
  }
  // Canonical order: y, then x, then height, then width.
  [[nodiscard]] friend bool operator<(const PlanarRect& a, const PlanarRect& b) noexcept;
};

SC_API bool rect_is_within(const PlanarRect& inner, const PlanarRect& outer) noexcept;
SC_API bool rect_intersects(const PlanarRect& a, const PlanarRect& b) noexcept;
// Intersection area of two rectangles; zero when they do not overlap.
SC_API Checked<SquareMillimeters> rect_intersection_area(const PlanarRect& a,
                                                         const PlanarRect& b) noexcept;

// ---------------------------------------------------------------------------
// Half-open rack-unit intervals
// ---------------------------------------------------------------------------

// The half-open interval [first, last) in whole rack units. Positions are
// 1-based rack unit numbers, which is how rack front panels are labelled. The
// interval [1, 3) is two units: 1 and 2.
struct SC_API RackUnitInterval final {
  std::int32_t first = 0;  // first unit, 1-based
  std::int32_t last = 0;   // one past the final unit, exclusive

  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] std::int32_t count() const noexcept { return last - first; }
  [[nodiscard]] bool contains(std::int32_t unit) const noexcept {
    return unit >= first && unit < last;
  }

  [[nodiscard]] static RackUnitInterval make(std::int32_t first, std::int32_t last) noexcept {
    RackUnitInterval interval;
    interval.first = first;
    interval.last = last;
    return interval;
  }
  [[nodiscard]] static RackUnitInterval of_count(std::int32_t first, std::int32_t count) noexcept {
    return make(first, first + count);
  }

  // Physical Location Registry stores {first, height} with an inclusive last
  // unit. This is the explicit conversion into the half-open convention.
  [[nodiscard]] static RackUnitInterval from_inclusive(std::int32_t first,
                                                       std::int32_t height) noexcept {
    return make(first, first + height);
  }
  // The inverse. `height` is last - first.
  [[nodiscard]] std::int32_t inclusive_height() const noexcept { return count(); }

  [[nodiscard]] friend bool operator==(const RackUnitInterval& a,
                                       const RackUnitInterval& b) noexcept {
    return a.first == b.first && a.last == b.last;
  }
  [[nodiscard]] friend bool operator!=(const RackUnitInterval& a,
                                       const RackUnitInterval& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const RackUnitInterval& a,
                                      const RackUnitInterval& b) noexcept {
    if (a.first != b.first) return a.first < b.first;
    return a.last < b.last;
  }
};

SC_API bool intervals_overlap(const RackUnitInterval& a, const RackUnitInterval& b) noexcept;
SC_API bool interval_contains(const RackUnitInterval& outer, const RackUnitInterval& inner) noexcept;

// An ordered, normalized, pairwise-disjoint set of intervals. Normalization
// merges touching or overlapping intervals and sorts by first, so two sets
// that describe the same space are equal as values.
class SC_API IntervalSet final {
 public:
  IntervalSet() = default;
  explicit IntervalSet(std::vector<RackUnitInterval> intervals);

  // Builds a normalized set. Refuses when any interval is invalid, when the
  // count exceeds kMaxExtentsPerRecord, or when the total would overflow.
  [[nodiscard]] static Result<IntervalSet> build(std::vector<RackUnitInterval> intervals);

  [[nodiscard]] const std::vector<RackUnitInterval>& intervals() const noexcept {
    return intervals_;
  }
  [[nodiscard]] bool empty() const noexcept { return intervals_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return intervals_.size(); }

  // Total number of units covered. Exact; overflow is reported.
  [[nodiscard]] Checked<RackUnits> total() const noexcept;

  [[nodiscard]] bool contains_unit(std::int32_t unit) const noexcept;
  [[nodiscard]] bool covers(const RackUnitInterval& interval) const noexcept;
  [[nodiscard]] bool intersects(const RackUnitInterval& interval) const noexcept;

  // Complement of this set inside [1, envelope_units + 1). The result is
  // normalized. `envelope_units` must be positive and within bounds.
  [[nodiscard]] static Result<IntervalSet> complement(const IntervalSet& set,
                                                      std::int32_t envelope_units);

  struct FreeRunStats final {
    std::int32_t largest = 0;
    std::uint32_t run_count = 0;
    std::int32_t total_free = 0;
  };

  // Largest single free run in the complement, the number of runs, and the
  // total free units. All exact integers.
  [[nodiscard]] static Result<FreeRunStats> free_run_stats(const IntervalSet& occupied,
                                                           std::int32_t envelope_units);

  // First run of `needed` units whose start position is congruent to
  // `alignment` modulo (1 means any start), scanned upwards. Positions are
  // 1-based. Returns a zero interval when no such run exists.
  [[nodiscard]] static Result<RackUnitInterval> first_fit(const IntervalSet& occupied,
                                                          std::int32_t envelope_units,
                                                          std::int32_t needed,
                                                          std::int32_t alignment);

  // Fragmentation of the free space in parts per million:
  //   ppm = (total_free - largest_run) * 1'000'000 / total_free
  // Zero when the free space is one run or empty. Exact integer division; the
  // remainder is discarded, never rounded up.
  [[nodiscard]] static Result<std::uint32_t> fragmentation_ppm(const IntervalSet& occupied,
                                                               std::int32_t envelope_units);

  [[nodiscard]] friend bool operator==(const IntervalSet& a, const IntervalSet& b) noexcept {
    return a.intervals_ == b.intervals_;
  }
  [[nodiscard]] friend bool operator!=(const IntervalSet& a, const IntervalSet& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const IntervalSet& a, const IntervalSet& b) noexcept {
    return a.intervals_ < b.intervals_;
  }

 private:
  std::vector<RackUnitInterval> intervals_;
};

// ---------------------------------------------------------------------------
// Rectangular region sets
// ---------------------------------------------------------------------------

// An ordered, canonical set of pairwise non-overlapping planar rectangles.
// Canonical order is by (y, x, height, width). Normalization refuses overlap
// rather than merging, because two overlapping exclusion rectangles are a
// modelling error in the input to one record, not something to silently union.
// Distinct records may overlap; the capacity engine measures their union.
class SC_API RectSet final {
 public:
  RectSet() = default;
  explicit RectSet(std::vector<PlanarRect> rects);

  [[nodiscard]] static Result<RectSet> build(std::vector<PlanarRect> rects);

  [[nodiscard]] const std::vector<PlanarRect>& rects() const noexcept { return rects_; }
  [[nodiscard]] bool empty() const noexcept { return rects_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return rects_.size(); }

  [[nodiscard]] Checked<SquareMillimeters> total_area() const noexcept;

  [[nodiscard]] bool intersects(const PlanarRect& rect) const noexcept;
  [[nodiscard]] Checked<SquareMillimeters> intersection_area(const PlanarRect& rect) const noexcept;

  [[nodiscard]] friend bool operator==(const RectSet& a, const RectSet& b) noexcept {
    return a.rects_ == b.rects_;
  }
  [[nodiscard]] friend bool operator!=(const RectSet& a, const RectSet& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const RectSet& a, const RectSet& b) noexcept {
    return a.rects_ < b.rects_;
  }

 private:
  std::vector<PlanarRect> rects_;
};

// Deterministic text rendering used by CLI output, store diagnostics and
// explanations. Never locale dependent.
SC_API std::string to_text(Millimeters value);
SC_API std::string to_text(SquareMillimeters value);
SC_API std::string to_text(RackUnits value);
SC_API std::string to_text(const RackUnitInterval& value);
SC_API std::string to_text(const IntervalSet& value);
SC_API std::string to_text(const PlanarRect& value);
SC_API std::string to_text(const RectSet& value);

}  // namespace dccp::space_capacity
