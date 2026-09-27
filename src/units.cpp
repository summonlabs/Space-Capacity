// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - exact unit and extent algebra.
//
// Every routine here is exact integer arithmetic. Nothing rounds, nothing
// saturates silently, and nothing consults a locale.

#include "dccp/space_capacity/units.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "geometry.hpp"

namespace dccp::space_capacity {
namespace {

constexpr char kDigitChars[] = "0123456789";

// Appends a signed decimal rendering of a magnitude with no locale, no
// grouping, and correct handling of the most negative value.
void append_decimal(std::string& out, bool negative, std::uint64_t magnitude) {
  char buffer[24];
  std::size_t used = 0;
  do {
    buffer[used++] = kDigitChars[magnitude % 10u];
    magnitude /= 10u;
  } while (magnitude != 0);
  if (negative) out.push_back('-');
  while (used > 0) {
    --used;
    out.push_back(buffer[used]);
  }
}

std::string signed_text(std::int64_t value) {
  const bool negative = value < 0;
  const std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(value) + 1ull) : static_cast<std::uint64_t>(value);
  std::string out;
  append_decimal(out, negative, magnitude);
  return out;
}

std::string unsigned_text(std::uint64_t value) {
  std::string out;
  append_decimal(out, false, value);
  return out;
}

// Normalizes a list of half-open intervals: sorts by first, merges touching
// and overlapping runs. Returns false when any interval is structurally
// invalid or when the result would exceed the per-record bound.
bool normalize_intervals(std::vector<RackUnitInterval>& intervals) {
  if (intervals.size() > Limits::kMaxExtentsPerRecord) return false;
  for (const RackUnitInterval& interval : intervals) {
    if (!interval.is_valid()) return false;
  }
  std::sort(intervals.begin(), intervals.end());
  std::vector<RackUnitInterval> merged;
  merged.reserve(intervals.size());
  for (const RackUnitInterval& interval : intervals) {
    if (!merged.empty() && interval.first <= merged.back().last) {
      if (interval.last > merged.back().last) merged.back().last = interval.last;
    } else {
      merged.push_back(interval);
    }
  }
  intervals = std::move(merged);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Unit kind
// ---------------------------------------------------------------------------

std::string_view unit_kind_name(UnitKind kind) noexcept {
  switch (kind) {
    case UnitKind::none:
      return "none";
    case UnitKind::millimeters:
      return "millimeters";
    case UnitKind::square_millimeters:
      return "square_millimeters";
    case UnitKind::rack_units:
      return "rack_units";
    case UnitKind::count:
      return "count";
  }
  return "unknown";
}

bool parse_unit_kind(std::string_view text, UnitKind& out) noexcept {
  if (text == "none") {
    out = UnitKind::none;
    return true;
  }
  if (text == "millimeters") {
    out = UnitKind::millimeters;
    return true;
  }
  if (text == "square_millimeters") {
    out = UnitKind::square_millimeters;
    return true;
  }
  if (text == "rack_units") {
    out = UnitKind::rack_units;
    return true;
  }
  if (text == "count") {
    out = UnitKind::count;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Checked unit arithmetic
// ---------------------------------------------------------------------------

Checked<Millimeters> add(Millimeters a, Millimeters b) noexcept {
  Checked<Millimeters> result;
  const Checked<std::int64_t> sum = checked_add_signed(a.value(), b.value());
  if (!sum) {
    result.overflowed = true;
    return result;
  }
  result.value = Millimeters{sum.value};
  return result;
}

Checked<Millimeters> sub(Millimeters a, Millimeters b) noexcept {
  Checked<Millimeters> result;
  const Checked<std::int64_t> difference = checked_sub_signed(a.value(), b.value());
  if (!difference) {
    result.overflowed = true;
    return result;
  }
  result.value = Millimeters{difference.value};
  return result;
}

Checked<SquareMillimeters> add(SquareMillimeters a, SquareMillimeters b) noexcept {
  Checked<SquareMillimeters> result;
  const Checked<std::int64_t> sum = checked_add_signed(a.value(), b.value());
  if (!sum) {
    result.overflowed = true;
    return result;
  }
  result.value = SquareMillimeters{sum.value};
  return result;
}

Checked<SquareMillimeters> sub(SquareMillimeters a, SquareMillimeters b) noexcept {
  Checked<SquareMillimeters> result;
  const Checked<std::int64_t> difference = checked_sub_signed(a.value(), b.value());
  if (!difference) {
    result.overflowed = true;
    return result;
  }
  result.value = SquareMillimeters{difference.value};
  return result;
}

Checked<RackUnits> add(RackUnits a, RackUnits b) noexcept {
  Checked<RackUnits> result;
  const Checked<std::int32_t> sum = checked_add_signed(a.value(), b.value());
  if (!sum) {
    result.overflowed = true;
    return result;
  }
  result.value = RackUnits{sum.value};
  return result;
}

Checked<RackUnits> sub(RackUnits a, RackUnits b) noexcept {
  Checked<RackUnits> result;
  const Checked<std::int32_t> difference = checked_sub_signed(a.value(), b.value());
  if (!difference) {
    result.overflowed = true;
    return result;
  }
  result.value = RackUnits{difference.value};
  return result;
}

// ---------------------------------------------------------------------------
// PlanarRect
// ---------------------------------------------------------------------------

bool PlanarRect::is_degenerate() const noexcept {
  return width.value() <= 0 || height.value() <= 0;
}

bool PlanarRect::is_valid() const noexcept {
  const std::int64_t xv = x.value();
  const std::int64_t yv = y.value();
  const std::int64_t wv = width.value();
  const std::int64_t hv = height.value();
  if (wv <= 0 || hv <= 0) return false;
  if (xv < 0 || yv < 0) return false;
  if (xv > Limits::kMaxMillimeters || yv > Limits::kMaxMillimeters) return false;
  if (wv > Limits::kMaxMillimeters || hv > Limits::kMaxMillimeters) return false;
  const Checked<std::int64_t> right = checked_add_signed(xv, wv);
  if (!right) return false;
  const Checked<std::int64_t> bottom = checked_add_signed(yv, hv);
  if (!bottom) return false;
  if (right.value > Limits::kMaxMillimeters || bottom.value > Limits::kMaxMillimeters) return false;
  const Checked<std::int64_t> measured = checked_mul_signed(wv, hv);
  if (!measured) return false;
  return measured.value <= Limits::kMaxSquareMillimeters;
}

Checked<SquareMillimeters> PlanarRect::area() const noexcept {
  Checked<SquareMillimeters> result;
  if (is_degenerate()) {
    result.value = SquareMillimeters{0};
    return result;
  }
  const Checked<std::int64_t> measured = checked_mul_signed(width.value(), height.value());
  if (!measured) {
    result.overflowed = true;
    return result;
  }
  result.value = SquareMillimeters{measured.value};
  return result;
}

bool operator<(const PlanarRect& a, const PlanarRect& b) noexcept {
  if (a.y != b.y) return a.y < b.y;
  if (a.x != b.x) return a.x < b.x;
  if (a.height != b.height) return a.height < b.height;
  return a.width < b.width;
}

bool rect_is_within(const PlanarRect& inner, const PlanarRect& outer) noexcept {
  if (inner.is_degenerate()) return false;
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
         inner.bottom() <= outer.bottom();
}

bool rect_intersects(const PlanarRect& a, const PlanarRect& b) noexcept {
  if (a.is_degenerate() || b.is_degenerate()) return false;
  // Half-open on both axes: touching edges do not overlap.
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

Checked<SquareMillimeters> rect_intersection_area(const PlanarRect& a,
                                                  const PlanarRect& b) noexcept {
  Checked<SquareMillimeters> result;
  if (!rect_intersects(a, b)) {
    result.value = SquareMillimeters{0};
    return result;
  }
  const std::int64_t left = (a.x > b.x) ? a.x.value() : b.x.value();
  const std::int64_t top = (a.y > b.y) ? a.y.value() : b.y.value();
  const std::int64_t right = (a.right() < b.right()) ? a.right().value() : b.right().value();
  const std::int64_t bottom = (a.bottom() < b.bottom()) ? a.bottom().value() : b.bottom().value();
  const Checked<std::int64_t> measured = checked_mul_signed(right - left, bottom - top);
  if (!measured) {
    result.overflowed = true;
    return result;
  }
  result.value = SquareMillimeters{measured.value};
  return result;
}

// ---------------------------------------------------------------------------
// Rack unit intervals
// ---------------------------------------------------------------------------

bool RackUnitInterval::is_valid() const noexcept {
  if (first < 1) return false;
  if (last <= first) return false;
  if (last > Limits::kMaxRackUnits + 1) return false;
  return true;
}

bool intervals_overlap(const RackUnitInterval& a, const RackUnitInterval& b) noexcept {
  return a.first < b.last && b.first < a.last;
}

bool interval_contains(const RackUnitInterval& outer, const RackUnitInterval& inner) noexcept {
  return inner.first >= outer.first && inner.last <= outer.last;
}

IntervalSet::IntervalSet(std::vector<RackUnitInterval> intervals) {
  std::vector<RackUnitInterval> copy = std::move(intervals);
  if (normalize_intervals(copy)) {
    intervals_ = std::move(copy);
  }
}

Result<IntervalSet> IntervalSet::build(std::vector<RackUnitInterval> intervals) {
  if (intervals.size() > Limits::kMaxExtentsPerRecord) {
    return Error::make(ErrorCode::limit_exceeded,
                       "an interval set may hold at most " +
                           unsigned_text(Limits::kMaxExtentsPerRecord) + " intervals");
  }
  for (const RackUnitInterval& interval : intervals) {
    if (!interval.is_valid()) {
      return Error::make(ErrorCode::invalid_extent,
                         "rack unit interval [" + signed_text(interval.first) + "," +
                             signed_text(interval.last) + ") is not inside [1," +
                             signed_text(Limits::kMaxRackUnits + 1) + ")");
    }
  }
  if (!normalize_intervals(intervals)) {
    return Error::make(ErrorCode::invalid_extent, "interval set could not be normalized");
  }
  return IntervalSet(std::move(intervals));
}

Checked<RackUnits> IntervalSet::total() const noexcept {
  Checked<RackUnits> result;
  std::int64_t sum = 0;
  for (const RackUnitInterval& interval : intervals_) {
    sum += interval.count();
    if (sum > static_cast<std::int64_t>(Limits::kMaxRackUnits)) {
      result.overflowed = true;
      return result;
    }
  }
  result.value = RackUnits{static_cast<std::int32_t>(sum)};
  return result;
}

bool IntervalSet::contains_unit(std::int32_t unit) const noexcept {
  for (const RackUnitInterval& interval : intervals_) {
    if (interval.contains(unit)) return true;
  }
  return false;
}

bool IntervalSet::covers(const RackUnitInterval& interval) const noexcept {
  for (const RackUnitInterval& candidate : intervals_) {
    if (interval_contains(candidate, interval)) return true;
  }
  return false;
}

bool IntervalSet::intersects(const RackUnitInterval& interval) const noexcept {
  for (const RackUnitInterval& candidate : intervals_) {
    if (intervals_overlap(candidate, interval)) return true;
  }
  return false;
}

Result<IntervalSet> IntervalSet::complement(const IntervalSet& set, std::int32_t envelope_units) {
  if (envelope_units < Limits::kMinRackUnits || envelope_units > Limits::kMaxRackUnits) {
    return Error::make(ErrorCode::invalid_range,
                       "rack envelope height " + signed_text(envelope_units) +
                           " is outside [1," + signed_text(Limits::kMaxRackUnits) + "]");
  }
  std::vector<RackUnitInterval> gaps;
  std::int32_t cursor = 1;
  const std::int32_t limit = envelope_units + 1;
  for (const RackUnitInterval& interval : set.intervals()) {
    // Every emitted gap is clamped to the envelope. An occupied interval that
    // starts beyond the envelope end contributes nothing, and the loop stops as
    // soon as the envelope is covered, so the complement is always a subset of
    // [1, envelope_units + 1) whatever the occupied set contains.
    if (cursor >= limit) break;
    if (interval.first > cursor) {
      gaps.push_back(RackUnitInterval::make(cursor, std::min(interval.first, limit)));
    }
    if (interval.last > cursor) cursor = interval.last;
  }
  if (cursor < limit) {
    gaps.push_back(RackUnitInterval::make(cursor, limit));
  }
  if (gaps.size() > Limits::kMaxExtentsPerRecord) {
    return Error::make(ErrorCode::limit_exceeded,
                       "the free space has more than " +
                           unsigned_text(Limits::kMaxExtentsPerRecord) + " runs");
  }
  IntervalSet result;
  result.intervals_ = std::move(gaps);
  return result;
}

Result<IntervalSet::FreeRunStats> IntervalSet::free_run_stats(const IntervalSet& occupied,
                                                              std::int32_t envelope_units) {
  Result<IntervalSet> free_space = complement(occupied, envelope_units);
  if (!free_space) return free_space.error();
  FreeRunStats stats;
  stats.run_count = static_cast<std::uint32_t>(free_space.value().size());
  for (const RackUnitInterval& run : free_space.value().intervals()) {
    if (run.count() > stats.largest) stats.largest = run.count();
    stats.total_free += run.count();
  }
  return stats;
}

Result<RackUnitInterval> IntervalSet::first_fit(const IntervalSet& occupied,
                                                std::int32_t envelope_units, std::int32_t needed,
                                                std::int32_t alignment) {
  if (needed < 1 || needed > Limits::kMaxRackUnits) {
    return Error::make(ErrorCode::invalid_range,
                       "requested unit count " + signed_text(needed) + " is outside [1," +
                           signed_text(Limits::kMaxRackUnits) + "]");
  }
  if (alignment < 1 || alignment > Limits::kMaxRackUnits) {
    return Error::make(ErrorCode::invalid_range,
                       "alignment " + signed_text(alignment) + " is outside [1," +
                           signed_text(Limits::kMaxRackUnits) + "]");
  }
  Result<IntervalSet> free_space = complement(occupied, envelope_units);
  if (!free_space) return free_space.error();
  for (const RackUnitInterval& run : free_space.value().intervals()) {
    if (run.count() < needed) continue;
    // Positions are 1-based and must satisfy (position - 1) % alignment == 0.
    const std::int32_t offset = (run.first - 1) % alignment;
    const std::int32_t shift = (offset == 0) ? 0 : (alignment - offset);
    const std::int32_t start = run.first + shift;
    if (start + needed <= run.last) {
      return RackUnitInterval::of_count(start, needed);
    }
  }
  return RackUnitInterval{};
}

Result<std::uint32_t> IntervalSet::fragmentation_ppm(const IntervalSet& occupied,
                                                     std::int32_t envelope_units) {
  Result<FreeRunStats> stats = free_run_stats(occupied, envelope_units);
  if (!stats) return stats.error();
  const FreeRunStats& value = stats.value();
  if (value.total_free <= 0) return std::uint32_t{0};
  const std::int64_t wasted = static_cast<std::int64_t>(value.total_free - value.largest);
  const std::int64_t ppm = (wasted * 1'000'000) / value.total_free;
  return static_cast<std::uint32_t>(ppm);
}

// ---------------------------------------------------------------------------
// RectSet
// ---------------------------------------------------------------------------

namespace {

// Detects a pair of overlapping rectangles. The kernel lives in geometry.cpp
// so that the overlap rule has exactly one implementation.

}  // namespace

RectSet::RectSet(std::vector<PlanarRect> rects) {
  Result<RectSet> built = RectSet::build(std::move(rects));
  if (built) {
    rects_ = std::move(built).value().rects_;
  }
}

Result<RectSet> RectSet::build(std::vector<PlanarRect> rects) {
  if (rects.size() > Limits::kMaxExtentsPerRecord) {
    return Error::make(ErrorCode::limit_exceeded,
                       "a rectangle set may hold at most " +
                           unsigned_text(Limits::kMaxExtentsPerRecord) + " rectangles");
  }
  for (const PlanarRect& rect : rects) {
    if (!rect.is_valid()) {
      return Error::make(ErrorCode::invalid_extent,
                         "rectangle (" + signed_text(rect.x.value()) + "," +
                             signed_text(rect.y.value()) + ") " +
                             signed_text(rect.width.value()) + "x" +
                             signed_text(rect.height.value()) +
                             " is degenerate or outside the millimetre bounds");
    }
  }
  std::sort(rects.begin(), rects.end());
  std::size_t first = 0;
  std::size_t second = 0;
  if (internal::find_overlapping_pair(rects, first, second)) {
    return Error::make(ErrorCode::overlap,
                       "rectangles at index " + unsigned_text(first) + " and " +
                           unsigned_text(second) + " of one record overlap");
  }
  RectSet result;
  result.rects_ = std::move(rects);
  return result;
}

Checked<SquareMillimeters> RectSet::total_area() const noexcept {
  Checked<SquareMillimeters> result;
  std::int64_t sum = 0;
  for (const PlanarRect& rect : rects_) {
    const Checked<SquareMillimeters> measured = rect.area();
    if (!measured || measured.value.value() > Limits::kMaxSquareMillimeters - sum) {
      result.overflowed = true;
      return result;
    }
    sum += measured.value.value();
  }
  result.value = SquareMillimeters{sum};
  return result;
}

bool RectSet::intersects(const PlanarRect& rect) const noexcept {
  for (const PlanarRect& candidate : rects_) {
    if (rect_intersects(candidate, rect)) return true;
  }
  return false;
}

Checked<SquareMillimeters> RectSet::intersection_area(const PlanarRect& rect) const noexcept {
  Checked<SquareMillimeters> result;
  std::int64_t sum = 0;
  for (const PlanarRect& candidate : rects_) {
    const Checked<SquareMillimeters> measured = rect_intersection_area(candidate, rect);
    if (!measured) {
      result.overflowed = true;
      return result;
    }
    const Checked<std::int64_t> total = checked_add_signed(sum, measured.value.value());
    if (!total) {
      result.overflowed = true;
      return result;
    }
    sum = total.value;
  }
  result.value = SquareMillimeters{sum};
  return result;
}

// ---------------------------------------------------------------------------
// Text rendering
// ---------------------------------------------------------------------------

std::string to_text(Millimeters value) {
  std::string out = signed_text(value.value());
  out += "mm";
  return out;
}

std::string to_text(SquareMillimeters value) {
  std::string out = signed_text(value.value());
  out += "mm2";
  return out;
}

std::string to_text(RackUnits value) {
  std::string out = signed_text(value.value());
  out += "U";
  return out;
}

std::string to_text(const RackUnitInterval& value) {
  // Half-open on the inside, inclusive on the outside, because that is how
  // rack units are spoken about on a data centre floor and it matches Rack
  // Registry's rendering.
  std::string out = "U";
  out += signed_text(value.first);
  if (value.count() > 1) {
    out += "-U";
    out += signed_text(value.last - 1);
  }
  out += " [";
  out += signed_text(value.first);
  out += ",";
  out += signed_text(value.last);
  out += ")";
  return out;
}

std::string to_text(const IntervalSet& value) {
  if (value.empty()) return "{}";
  std::string out;
  for (const RackUnitInterval& interval : value.intervals()) {
    if (!out.empty()) out += ",";
    out += "[";
    out += signed_text(interval.first);
    out += ",";
    out += signed_text(interval.last);
    out += ")";
  }
  return out;
}

std::string to_text(const PlanarRect& value) {
  std::string out = "(";
  out += signed_text(value.x.value());
  out += ",";
  out += signed_text(value.y.value());
  out += ") ";
  out += signed_text(value.width.value());
  out += "x";
  out += signed_text(value.height.value());
  out += "mm";
  return out;
}

std::string to_text(const RectSet& value) {
  if (value.empty()) return "{}";
  std::string out;
  for (const PlanarRect& rect : value.rects()) {
    if (!out.empty()) out += ",";
    out += to_text(rect);
  }
  return out;
}

}  // namespace dccp::space_capacity
