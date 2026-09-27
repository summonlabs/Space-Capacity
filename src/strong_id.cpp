// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - the identity grammar, label and UTF-8 validation, attempt
// rendering, and the canonical counter parser.
//
// Every predicate in this file is byte-wise and locale independent. <cctype>
// is deliberately never used: its predicates are locale dependent and take an
// int that must be representable as unsigned char, so they cannot classify an
// arbitrary byte safely. One value has exactly one spelling, and this file is
// where that spelling is decided.

#include "dccp/space_capacity/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/limits.hpp"

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

// [0-9A-Za-z]: the set an identifier must both start and end with.
[[nodiscard]] constexpr bool is_identifier_edge(char c) noexcept {
  return is_decimal_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// [0-9A-Za-z._:-]: the set the body of an identifier draws from.
[[nodiscard]] constexpr bool is_identifier_body(char c) noexcept {
  return is_identifier_edge(c) || c == '.' || c == '_' || c == ':' || c == '-';
}

[[nodiscard]] constexpr bool is_continuation_byte(unsigned int byte) noexcept {
  return (byte & 0xC0u) == 0x80u;
}

}  // namespace

std::string_view identifier_syntax_help() noexcept {
  // A raw string literal, so the brackets stay the literal text a caller
  // prints rather than something this file has to escape.
  return R"(identifiers are 1..128 bytes, start and end with [0-9A-Za-z], and contain only [0-9A-Za-z._:-])";
}

bool is_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned int lead = byte_value(text[index]);
    if (lead < 0x80u) {
      ++index;
      continue;
    }

    std::size_t trailing = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2u && lead <= 0xDFu) {
      // Two bytes. 0xC0 and 0xC1 could only ever spell an overlong value.
      trailing = 1;
      code_point = lead & 0x1Fu;
      minimum = 0x80u;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
      trailing = 2;
      code_point = lead & 0x0Fu;
      minimum = 0x800u;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
      // 0xF5..0xFF would name a code point above U+10FFFF.
      trailing = 3;
      code_point = lead & 0x07u;
      minimum = 0x10000u;
    } else {
      // A stray continuation byte, or a lead that can only be overlong.
      return false;
    }

    // The sequence needs `trailing` bytes after the lead byte.
    if (text.size() - index <= trailing) {
      return false;
    }
    for (std::size_t offset = 1; offset <= trailing; ++offset) {
      const unsigned int next = byte_value(text[index + offset]);
      if (!is_continuation_byte(next)) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }

    if (code_point < minimum) {
      return false;  // overlong encoding of a shorter sequence
    }
    if (code_point > 0x10FFFFu) {
      return false;  // beyond the last Unicode code point
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;  // a surrogate half is not a scalar value
    }
    index += trailing + 1;
  }
  return true;
}

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > Limits::kMaxIdentifierBytes) {
    return false;
  }
  if (!is_identifier_edge(text.front()) || !is_identifier_edge(text.back())) {
    return false;
  }
  for (const char c : text) {
    if (!is_identifier_body(c)) {
      return false;
    }
  }
  return true;
}

bool is_valid_label(std::string_view text) noexcept {
  if (text.size() > Limits::kMaxLabelBytes) {
    return false;
  }
  if (!is_utf8(text)) {
    return false;
  }
  for (const char c : text) {
    const unsigned int byte = byte_value(c);
    if (byte < 0x20u || byte == 0x7Fu) {
      return false;
    }
  }
  return true;
}

std::string AttemptId::to_string() const {
  // "<incarnation>.<sequence>", each a decimal counter with no leading zero:
  // one attempt, one spelling.
  std::string text = std::to_string(incarnation_.value());
  text.push_back('.');
  text.append(std::to_string(sequence_));
  return text;
}

template <typename Tag>
Result<CounterValue<Tag>> CounterValue<Tag>::parse(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::invalid_range,
                       "counter " + std::string(Tag::kind_name) + " must not be empty");
  }
  for (const char c : text) {
    if (!is_decimal_digit(c)) {
      return Error::make(ErrorCode::invalid_range, "counter " + std::string(Tag::kind_name) +
                                                       " must be decimal digits only, with no sign "
                                                       "and no whitespace");
    }
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error::make(ErrorCode::invalid_range,
                       "counter " + std::string(Tag::kind_name) + " must not carry a leading zero");
  }

  constexpr std::uint64_t kLimit = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value = 0;
  for (const char c : text) {
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    // Checked accumulation: refuse before the multiply and add could wrap.
    if (value > (kLimit - digit) / 10u) {
      return Error::make(ErrorCode::arithmetic_overflow,
                         "counter " + std::string(Tag::kind_name) + " does not fit in 64 bits");
    }
    value = value * 10u + digit;
  }
  return CounterValue<Tag>(value);
}

// The counter parser lives here rather than in the header, so that every
// translation unit that names a persisted counter links against this one
// definition and the accepted spelling cannot drift between them.
template Result<CounterValue<EntityGenerationName>> CounterValue<EntityGenerationName>::parse(
    std::string_view);
template Result<CounterValue<RegistryRevisionName>> CounterValue<RegistryRevisionName>::parse(
    std::string_view);
template Result<CounterValue<StoreIncarnationName>> CounterValue<StoreIncarnationName>::parse(
    std::string_view);

}  // namespace dccp::space_capacity
