// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - canonical text helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Everything here is deterministic and locale independent. Numbers are
// rendered in the C locale with no grouping and no thousands separators, text
// is quoted with a single escaping rule, and every parser accepts exactly one
// spelling.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/export.hpp"

namespace dccp::space_capacity {

// Renders a bounded integer in decimal. The 32-bit overloads exist so that a
// call with a plain `std::int32_t` or `std::uint32_t` is not ambiguous between
// the 64-bit overloads.
SC_API std::string to_text(std::uint64_t value);
SC_API std::string to_text(std::int64_t value);
SC_API std::string to_text(std::uint32_t value);
SC_API std::string to_text(std::int32_t value);

// Parses a decimal unsigned integer. Rejects signs, whitespace, an empty
// string, leading zeros, and anything that overflows.
SC_API Result<std::uint64_t> parse_u64(std::string_view text);
SC_API Result<std::int64_t> parse_i64(std::string_view text);

// Parses a boolean. Accepts exactly "true" and "false".
SC_API Result<bool> parse_bool(std::string_view text);

// Quotes a string for canonical output: wraps in double quotes and escapes
// backslash and double quote with a backslash. Any byte below 0x20 is refused,
// because a canonical document never contains one.
SC_API Result<std::string> quote_text(std::string_view text);

// The inverse. Rejects an unquoted value, a trailing backslash and any
// embedded control byte.
SC_API Result<std::string> unquote_text(std::string_view text);

// Renders an absent option as "-" and a present one through `render`.
template <typename T, typename Render>
[[nodiscard]] std::string render_optional(const std::optional<T>& value, Render render) {
  if (!value.has_value()) return "-";
  return render(*value);
}

// Splits a canonical document into lines. Accepts both LF and CRLF on input,
// so a file that travelled through a Windows checkout still parses, and
// rejects a NUL byte anywhere.
SC_API Result<std::vector<std::string_view>> split_lines(std::string_view document);

// True when the text is printable ASCII (0x20..0x7E) only. The canonical
// document format uses this for every token; free text uses is_valid_label.
SC_API bool is_ascii_printable(std::string_view text) noexcept;

}  // namespace dccp::space_capacity
