// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - evidence sets, caller-supplied instants and free text.
//
// system_utc_now() is the only place in this library that reads a clock. It
// converts the operating system's reading into whole milliseconds since the
// Unix epoch UTC through an explicit constant offset, never through the
// unspecified epoch of std::chrono::system_clock, so no result anywhere in the
// library depends on when it happened to run.
//
// Instant text is rendered and parsed by hand with Howard Hinnant's civil-days
// algorithms, so no locale, <ctime> formatter or time zone database takes part
// and one instant always produces the same byte string. Rendering and parsing
// are inverses, and every negative instant is handled by floor division rather
// than by truncation toward zero.
//
// Evidence sets and the free-text wrappers are pure functions of their input:
// they bound, validate, order and never consult anything outside the argument.

#include "dccp/space_capacity/evidence.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
// The build defines these, but the header is only safe with them in any build.
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace dccp::space_capacity {
namespace {

// ---------------------------------------------------------------------------
// Calendar arithmetic
// ---------------------------------------------------------------------------

constexpr std::int64_t kMillisPerSecond = 1'000;
constexpr std::int64_t kMillisPerMinute = 60 * kMillisPerSecond;
constexpr std::int64_t kMillisPerHour = 60 * kMillisPerMinute;
constexpr std::int64_t kMillisPerDay = 24 * kMillisPerHour;

// 1601-01-01T00:00:00Z and 1970-01-01T00:00:00Z, in 100-nanosecond ticks, and
// the number of ticks in one millisecond.
constexpr std::int64_t kFiletimeEpochTicks = 116'444'736'000'000'000;
constexpr std::int64_t kTicksPerMilli = 10'000;

// A year magnitude this parser accepts before the day arithmetic runs. The
// largest year an int64 millisecond count can name is about 292,277,024,628;
// the bound keeps every intermediate product inside int64 while still leaving
// the final range check to reject what milliseconds cannot hold.
constexpr std::int64_t kMaxYearMagnitude = 300'000'000'000;

// The whole-day counts around the epoch whose millisecond extent still leaves
// room for a partial day in int64. Days outside this window cannot be named by
// any int64 millisecond count, so parsing refuses them.
constexpr std::int64_t kMaxDays = std::numeric_limits<std::int64_t>::max() / kMillisPerDay;
constexpr std::int64_t kMinDays = -(kMaxDays + 1);
// |INT64_MIN|, the widest magnitude a negative instant can carry.
constexpr std::uint64_t kMinInstantMagnitude = 9'223'372'036'854'775'808ull;

struct CivilDate final {
  std::int64_t year = 0;
  std::uint32_t month = 1;
  std::uint32_t day = 1;
};

// Howard Hinnant's civil_from_days: the proleptic Gregorian date of a day
// count measured from 1970-01-01. Valid for every int64 day count, including
// dates before the epoch.
[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::uint32_t doe = static_cast<std::uint32_t>(days - era * 146097);
  const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint32_t mp = (5 * doy + 2) / 153;
  const std::uint32_t day = doy - (153 * mp + 2) / 5 + 1;
  const std::uint32_t month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
  return CivilDate{year, month, day};
}

// The inverse: the day count of a proleptic Gregorian date. The caller keeps
// `year` inside kMaxYearMagnitude so every product here stays in int64.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, std::uint32_t month,
                                                     std::uint32_t day) noexcept {
  const std::int64_t y = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::uint32_t yoe = static_cast<std::uint32_t>(y - era * 400);
  const std::uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const std::uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] constexpr bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr std::uint32_t days_in_month(std::int64_t year,
                                                    std::uint32_t month) noexcept {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return is_leap_year(year) ? 29u : 28u;
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Decimal rendering
// ---------------------------------------------------------------------------

// Appends `value` in decimal, zero padded to `min_width` digits. Digits are
// produced by hand so no locale, stream or printf takes part.
void append_decimal(std::string& out, std::uint64_t value, std::size_t min_width) {
  char buffer[32];
  std::size_t used = 0;
  do {
    buffer[used] = static_cast<char>('0' + static_cast<int>(value % 10u));
    ++used;
    value /= 10u;
  } while (value != 0 && used < sizeof(buffer));
  while (used < min_width && used < sizeof(buffer)) {
    buffer[used] = '0';
    ++used;
  }
  while (used > 0) {
    --used;
    out.push_back(buffer[used]);
  }
}

// ---------------------------------------------------------------------------
// Fixed-width digit reads
// ---------------------------------------------------------------------------

// Reads exactly `count` ASCII digits at `position`, advancing it on success.
[[nodiscard]] bool read_digits(std::string_view text, std::size_t& position, std::size_t count,
                               std::uint32_t& out) noexcept {
  if (count == 0 || count > 9) return false;
  if (text.size() - position < count) return false;
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[position + i];
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<std::uint32_t>(c - '0');
  }
  position += count;
  out = value;
  return true;
}

[[nodiscard]] bool consume(std::string_view text, std::size_t& position, char expected) noexcept {
  if (position >= text.size() || text[position] != expected) return false;
  ++position;
  return true;
}

// ---------------------------------------------------------------------------
// Free text
// ---------------------------------------------------------------------------

// True when the text contains a byte that is neither printable nor part of a
// UTF-8 sequence: everything below 0x20 and DEL.
[[nodiscard]] bool has_control_byte(std::string_view text) noexcept {
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte < 0x20u || byte == 0x7Fu) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Instants
// ---------------------------------------------------------------------------

Timestamp system_utc_now() noexcept {
#if defined(_WIN32)
  FILETIME file_time{};
  GetSystemTimePreciseAsFileTime(&file_time);
  const std::uint64_t ticks = (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32) |
                              static_cast<std::uint64_t>(file_time.dwLowDateTime);
  const std::int64_t since_epoch = static_cast<std::int64_t>(ticks) - kFiletimeEpochTicks;
  std::int64_t millis = since_epoch / kTicksPerMilli;
  if (since_epoch < 0 && since_epoch % kTicksPerMilli != 0) --millis;  // floor, not truncate
  return Timestamp{millis};
#else
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return Timestamp{0};
  }
  const std::int64_t seconds = static_cast<std::int64_t>(now.tv_sec);
  const std::int64_t nanos = static_cast<std::int64_t>(now.tv_nsec);
  return Timestamp{seconds * kMillisPerSecond + nanos / 1'000'000};
#endif
}

std::string to_text(Timestamp value) {
  // Floor division: the day count steps down for instants before 1970, and the
  // millisecond-of-day is always in [0, 86399999). No intermediate product
  // leaves int64, not even for the extreme millisecond counts.
  std::int64_t days = value.unix_millis / kMillisPerDay;
  std::int64_t millis_of_day = value.unix_millis % kMillisPerDay;
  if (millis_of_day < 0) {
    millis_of_day += kMillisPerDay;
    --days;
  }

  const CivilDate date = civil_from_days(days);
  const std::uint64_t hour = static_cast<std::uint64_t>(millis_of_day / kMillisPerHour);
  const std::uint64_t minute = static_cast<std::uint64_t>(millis_of_day / kMillisPerMinute) % 60u;
  const std::uint64_t second = static_cast<std::uint64_t>(millis_of_day / kMillisPerSecond) % 60u;
  const std::uint64_t milli = static_cast<std::uint64_t>(millis_of_day % kMillisPerSecond);

  std::string out;
  out.reserve(24);
  // Years are four digits in 0000..9999 and take as many digits as they need
  // outside that range; a year before 0000 carries a leading minus sign.
  if (date.year < 0) {
    out.push_back('-');
    append_decimal(out, static_cast<std::uint64_t>(-date.year), 4);
  } else {
    append_decimal(out, static_cast<std::uint64_t>(date.year), 4);
  }
  out.push_back('-');
  append_decimal(out, date.month, 2);
  out.push_back('-');
  append_decimal(out, date.day, 2);
  out.push_back('T');
  append_decimal(out, hour, 2);
  out.push_back(':');
  append_decimal(out, minute, 2);
  out.push_back(':');
  append_decimal(out, second, 2);
  out.push_back('.');
  append_decimal(out, milli, 3);
  out.push_back('Z');
  return out;
}

bool parse_timestamp(std::string_view text, Timestamp& out) noexcept {
  // [YYYY...]-MM-DDTHH:MM:SS[.mmm]Z, nothing before it and nothing after it.
  std::size_t position = 0;
  bool negative_year = false;
  if (position < text.size() && text[position] == '-') {
    negative_year = true;
    ++position;
  }

  const std::size_t year_start = position;
  std::int64_t year = 0;
  while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
    if (year > kMaxYearMagnitude / 10) return false;  // longer than any millisecond count
    year = year * 10 + (text[position] - '0');
    ++position;
  }
  const std::size_t year_digits = position - year_start;
  if (year_digits < 4) return false;
  if (year_digits > 4 && text[year_start] == '0') return false;  // no padded extra digits
  if (negative_year) year = -year;

  std::uint32_t month = 0;
  std::uint32_t day = 0;
  std::uint32_t hour = 0;
  std::uint32_t minute = 0;
  std::uint32_t second = 0;
  std::uint32_t milli = 0;

  if (!consume(text, position, '-')) return false;
  if (!read_digits(text, position, 2, month)) return false;
  if (!consume(text, position, '-')) return false;
  if (!read_digits(text, position, 2, day)) return false;
  if (!consume(text, position, 'T')) return false;
  if (!read_digits(text, position, 2, hour)) return false;
  if (!consume(text, position, ':')) return false;
  if (!read_digits(text, position, 2, minute)) return false;
  if (!consume(text, position, ':')) return false;
  if (!read_digits(text, position, 2, second)) return false;
  // The millisecond field is three digits when it is present at all.
  if (position < text.size() && text[position] == '.') {
    ++position;
    if (!read_digits(text, position, 3, milli)) return false;
  }
  if (!consume(text, position, 'Z')) return false;
  if (position != text.size()) return false;

  if (month < 1 || month > 12) return false;
  if (day < 1 || day > days_in_month(year, month)) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  const std::int64_t days = days_from_civil(year, month, day);
  if (days > kMaxDays || days < kMinDays) return false;
  const std::int64_t millis_of_day = static_cast<std::int64_t>(hour) * kMillisPerHour +
                                     static_cast<std::int64_t>(minute) * kMillisPerMinute +
                                     static_cast<std::int64_t>(second) * kMillisPerSecond +
                                     static_cast<std::int64_t>(milli);
  if (days >= 0) {
    const std::int64_t base = days * kMillisPerDay;
    if (millis_of_day > std::numeric_limits<std::int64_t>::max() - base) return false;
    out = Timestamp{base + millis_of_day};
    return true;
  }
  // Before the epoch the magnitude is accumulated in unsigned arithmetic, so
  // the earliest instant an int64 millisecond count can name is reconstructed
  // exactly instead of being refused as an overflow.
  const std::uint64_t magnitude = static_cast<std::uint64_t>(-(days + 1)) + 1u;
  const std::uint64_t total_magnitude = magnitude * static_cast<std::uint64_t>(kMillisPerDay) -
                                        static_cast<std::uint64_t>(millis_of_day);
  if (total_magnitude > kMinInstantMagnitude) return false;
  if (total_magnitude == kMinInstantMagnitude) {
    out = Timestamp{std::numeric_limits<std::int64_t>::min()};
    return true;
  }
  out = Timestamp{-static_cast<std::int64_t>(total_magnitude)};
  return true;
}

// ---------------------------------------------------------------------------
// Evidence sets
// ---------------------------------------------------------------------------

Result<EvidenceSet> EvidenceSet::build(std::vector<EvidenceRef> refs) {
  if (refs.size() > static_cast<std::size_t>(Limits::kMaxEvidencePerRecord)) {
    return Error::make(ErrorCode::limit_exceeded,
                       "an evidence set holds at most " +
                           std::to_string(Limits::kMaxEvidencePerRecord) + " references");
  }
  for (const EvidenceRef& ref : refs) {
    if (ref.empty()) {
      return Error::make(ErrorCode::empty_value, "an evidence reference must name a record");
    }
  }
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
  EvidenceSet set;
  set.refs_ = std::move(refs);
  return set;
}

bool EvidenceSet::all_observed() const noexcept {
  for (const EvidenceRef& ref : refs_) {
    if (!ref.observed()) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Operator-supplied metadata
// ---------------------------------------------------------------------------

Result<DisplayLabel> DisplayLabel::parse(std::string_view text) {
  if (text.size() > Limits::kMaxLabelBytes) {
    return Error::make(ErrorCode::text_too_long, "a display label must not exceed " +
                                                     std::to_string(Limits::kMaxLabelBytes) +
                                                     " bytes");
  }
  if (!is_utf8(text)) {
    return Error::make(ErrorCode::invalid_utf8, "a display label must be valid UTF-8");
  }
  if (has_control_byte(text)) {
    return Error::make(ErrorCode::invalid_character,
                       "a display label must not contain a control byte");
  }
  DisplayLabel label;
  label.value_.assign(text.data(), text.size());
  return label;
}

Result<Note> Note::parse(std::string_view text) {
  if (text.size() > Limits::kMaxNoteBytes) {
    return Error::make(ErrorCode::text_too_long, "a note must not exceed " +
                                                     std::to_string(Limits::kMaxNoteBytes) +
                                                     " bytes");
  }
  if (!is_utf8(text)) {
    return Error::make(ErrorCode::invalid_utf8, "a note must be valid UTF-8");
  }
  if (has_control_byte(text)) {
    return Error::make(ErrorCode::invalid_character, "a note must not contain a control byte");
  }
  Note note;
  note.value_.assign(text.data(), text.size());
  return note;
}

}  // namespace dccp::space_capacity
