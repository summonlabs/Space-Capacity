// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - canonical text helpers: decimal rendering, the three
// scalar parsers, quoting, and line splitting.
//
// Numbers are rendered by hand rather than through std::to_string or the
// printf family, so that no locale, no grouping and no library detail can
// reach the document format. Every parser accepts exactly one spelling, and
// every predicate is a plain byte comparison: <cctype> is locale dependent and
// is not used anywhere in this file.

#include "dccp/space_capacity/text.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"

namespace dccp::space_capacity {

namespace {

// The value of one byte without the sign extension a plain `char` would bring
// on a target where char is signed.
[[nodiscard]] constexpr unsigned int byte_value(char c) noexcept {
  return static_cast<unsigned int>(static_cast<unsigned char>(c));
}

[[nodiscard]] constexpr bool is_decimal_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

constexpr std::uint64_t kMaxUnsigned = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxSignedMagnitude =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
// |INT64_MIN|, which is one larger than INT64_MAX and is only representable as
// an unsigned value.
constexpr std::uint64_t kMinSignedMagnitude = kMaxSignedMagnitude + 1u;

// Appends the decimal digits of `magnitude`, most significant first, with no
// leading zeros and no grouping.
void append_decimal(std::string& out, std::uint64_t magnitude) {
  char digits[20];  // the widest 64-bit value has 20 decimal digits
  std::size_t count = 0;
  do {
    digits[count] = static_cast<char>('0' + static_cast<int>(magnitude % 10u));
    ++count;
    magnitude /= 10u;
  } while (magnitude != 0u);
  while (count != 0) {
    --count;
    out.push_back(digits[count]);
  }
}

}  // namespace

std::string to_text(std::uint64_t value) {
  std::string text;
  text.reserve(20);
  append_decimal(text, value);
  return text;
}

std::string to_text(std::int64_t value) {
  std::string text;
  text.reserve(21);
  if (value < 0) {
    text.push_back('-');
    // Negate in unsigned space: |INT64_MIN| is not representable in int64_t,
    // and converting a negative value to unsigned is well defined modulo 2^64.
    append_decimal(text, static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value));
  } else {
    append_decimal(text, static_cast<std::uint64_t>(value));
  }
  return text;
}

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::invalid_range, "an unsigned integer must not be empty");
  }
  for (const char c : text) {
    if (!is_decimal_digit(c)) {
      return Error::make(ErrorCode::invalid_range,
                         "an unsigned integer must be decimal digits only, with no sign and no "
                         "whitespace");
    }
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error::make(ErrorCode::invalid_range,
                       "an unsigned integer must not carry a leading zero");
  }

  std::uint64_t value = 0;
  for (const char c : text) {
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    // Checked accumulation: refuse before the multiply and add could wrap.
    if (value > (kMaxUnsigned - digit) / 10u) {
      return Error::make(ErrorCode::arithmetic_overflow,
                         "an unsigned integer does not fit in 64 bits");
    }
    value = value * 10u + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::invalid_range, "a signed integer must not be empty");
  }

  const bool negative = text.front() == '-';
  const std::string_view digits = negative ? text.substr(1) : text;
  if (digits.empty()) {
    return Error::make(ErrorCode::invalid_range,
                       "a signed integer must carry at least one digit after its sign");
  }
  for (const char c : digits) {
    if (!is_decimal_digit(c)) {
      return Error::make(ErrorCode::invalid_range,
                         "a signed integer must be an optional minus sign followed by decimal "
                         "digits only");
    }
  }
  if (digits.front() == '0' && (negative || digits.size() > 1)) {
    // "-0" would be a second spelling of zero, and no integer carries a
    // leading zero.
    return Error::make(ErrorCode::invalid_range, "a signed integer must not carry a leading zero");
  }

  // The bound is the magnitude, so INT64_MIN is accepted and one past it is
  // not.
  const std::uint64_t limit = negative ? kMinSignedMagnitude : kMaxSignedMagnitude;
  std::uint64_t magnitude = 0;
  for (const char c : digits) {
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (limit - digit) / 10u) {
      return Error::make(ErrorCode::arithmetic_overflow, "a signed integer does not fit in 64 bits");
    }
    magnitude = magnitude * 10u + digit;
  }

  if (!negative) {
    return static_cast<std::int64_t>(magnitude);
  }
  if (magnitude == kMinSignedMagnitude) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>(magnitude);
}

Result<bool> parse_bool(std::string_view text) {
  if (text == "true") {
    return true;
  }
  if (text == "false") {
    return false;
  }
  return Error::make(ErrorCode::unknown_enum_token,
                     "a boolean is spelled exactly \"true\" or exactly \"false\"");
}

Result<std::string> quote_text(std::string_view text) {
  for (const char c : text) {
    if (byte_value(c) < 0x20u) {
      return Error::make(ErrorCode::invalid_character,
                         "a quoted value must not contain a control character below 0x20");
    }
  }

  std::string quoted;
  quoted.reserve(text.size() + 2);
  quoted.push_back('"');
  for (const char c : text) {
    if (c == '\\' || c == '"') {
      quoted.push_back('\\');
    }
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
}

Result<std::string> unquote_text(std::string_view text) {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return Error::make(ErrorCode::malformed_identity,
                       "a quoted value starts and ends with a double quote and is at least two "
                       "bytes long");
  }

  std::string unquoted;
  unquoted.reserve(text.size() - 2);
  // The content is everything strictly between the two quotes.
  for (std::size_t index = 1; index + 1 < text.size(); ++index) {
    const char current = text[index];
    if (current == '\\') {
      ++index;
      if (index + 1 >= text.size()) {
        // The escape consumed the closing quote: nothing follows the
        // backslash.
        return Error::make(ErrorCode::malformed_identity,
                           "a quoted value must not end with a dangling escape");
      }
      const char escaped = text[index];
      if (escaped != '\\' && escaped != '"') {
        return Error::make(ErrorCode::malformed_identity,
                           "a quoted value must escape only a backslash or a double quote");
      }
      unquoted.push_back(escaped);
      continue;
    }
    if (byte_value(current) < 0x20u) {
      return Error::make(ErrorCode::malformed_identity,
                         "a quoted value must not contain a control character below 0x20");
    }
    unquoted.push_back(current);
  }
  return unquoted;
}

Result<std::vector<std::string_view>> split_lines(std::string_view document) {
  for (const char c : document) {
    if (c == '\0') {
      return Error::make(ErrorCode::invalid_character, "a canonical document must not contain NUL");
    }
  }

  std::vector<std::string_view> lines;
  if (document.empty()) {
    return lines;
  }

  std::size_t start = 0;
  std::size_t index = 0;
  while (index < document.size()) {
    const char current = document[index];
    if (current == '\r' || current == '\n') {
      lines.push_back(document.substr(start, index - start));
      // A CRLF pair is one separator; a lone CR is a separator on its own.
      if (current == '\r' && index + 1 < document.size() && document[index + 1] == '\n') {
        index += 2;
      } else {
        ++index;
      }
      start = index;
      continue;
    }
    ++index;
  }
  // A trailing separator ends the last line rather than opening an empty one.
  if (start < document.size()) {
    lines.push_back(document.substr(start));
  }
  return lines;
}

bool is_ascii_printable(std::string_view text) noexcept {
  for (const char c : text) {
    const unsigned int byte = byte_value(c);
    if (byte < 0x20u || byte > 0x7Eu) {
      return false;
    }
  }
  return true;
}

}  // namespace dccp::space_capacity

namespace dccp::space_capacity {

std::string to_text(std::uint32_t value) { return to_text(static_cast<std::uint64_t>(value)); }

std::string to_text(std::int32_t value) { return to_text(static_cast<std::int64_t>(value)); }

}  // namespace dccp::space_capacity