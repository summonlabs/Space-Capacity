// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - identities, counters and canonical text.
//
// What this file proves:
//   * the DCCP identifier grammar accepts exactly what it documents, with the
//     documented error codes for each refusal;
//   * identity families are unrelated types, so a node identity can never stand
//     in for a claim or an exclusion identity;
//   * the one identity syntax sentence is byte-identical to the published one;
//   * UTF-8 validation accepts all four sequence widths and refuses overlong,
//     truncated, surrogate and out-of-range encodings;
//   * an attempt identity orders by incarnation and then by sequence;
//   * decimal counters have exactly one spelling and refuse to wrap;
//   * every text helper is deterministic: rendering, parsing, quoting and line
//     splitting round-trip or refuse with one stable code;
//   * SHA-256 matches the published FIPS 180-4 vectors and its digest
//     round-trips through text.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/text.hpp"

#include "test_support.hpp"

using namespace dccp::space_capacity;

namespace {

// Compile-time proof that the identity families do not convert into one
// another, in either direction. A node identity is not a claim identity is not
// an exclusion identity, and no implicit conversion exists between them.
static_assert(!std::is_convertible_v<SpaceNodeId, OccupancyClaimId>);
static_assert(!std::is_convertible_v<OccupancyClaimId, SpaceNodeId>);
static_assert(!std::is_convertible_v<SpaceNodeId, ExclusionRegionId>);
static_assert(!std::is_convertible_v<ExclusionRegionId, SpaceNodeId>);
static_assert(!std::is_convertible_v<OccupancyClaimId, ExclusionRegionId>);
static_assert(!std::is_convertible_v<ExclusionRegionId, OccupancyClaimId>);
static_assert(!std::is_same_v<SpaceNodeId, OccupancyClaimId>);
static_assert(std::is_same_v<SpaceNodeId::tag_type, SpaceNodeTag>);
static_assert(std::is_same_v<OccupancyClaimId::tag_type, OccupancyClaimTag>);
static_assert(std::is_same_v<ExclusionRegionId::tag_type, ExclusionRegionTag>);

[[nodiscard]] std::string repeated(char byte, std::size_t count) {
  return std::string(count, byte);
}

template <typename T>
void expect_error(const Result<T>& result, ErrorCode expected) {
  SC_CHECK(!result.ok());
  if (result.ok()) return;
  SC_CHECK_EQ(result.error().code, expected);
}

// ---------------------------------------------------------------------------
// Identity grammar
// ---------------------------------------------------------------------------

void identifier_grammar() {
  SC_CASE("is_valid_identifier accepts the documented grammar");
  SC_CHECK(is_valid_identifier("rack-01"));
  SC_CHECK(is_valid_identifier("fac-1"));
  SC_CHECK(is_valid_identifier("a"));
  SC_CHECK(is_valid_identifier("0"));
  SC_CHECK(is_valid_identifier("A0._:-z"));
  SC_CHECK(is_valid_identifier("a.b_c:d-e"));
  SC_CHECK(is_valid_identifier(repeated('a', 128)));
  SC_CHECK(is_valid_identifier(repeated('9', 128)));

  SC_CHECK(!is_valid_identifier(""));
  SC_CHECK(!is_valid_identifier("-abc"));  // leading non-alphanumeric
  SC_CHECK(!is_valid_identifier("_abc"));
  SC_CHECK(!is_valid_identifier("abc-"));  // trailing non-alphanumeric
  SC_CHECK(!is_valid_identifier("abc:"));
  SC_CHECK(!is_valid_identifier("."));
  SC_CHECK(!is_valid_identifier("ab cd"));  // interior space
  SC_CHECK(!is_valid_identifier("a/b"));    // slash
  SC_CHECK(!is_valid_identifier("a\tb"));
  SC_CHECK(!is_valid_identifier(std::string_view("a\0b", 3)));  // interior NUL
  SC_CHECK(!is_valid_identifier(repeated('a', 129)));

  // The grammar is one sentence, published once.
  SC_CHECK_EQ(identifier_syntax_help(),
              std::string_view("identifiers are 1..128 bytes, start and end with [0-9A-Za-z], and "
                               "contain only [0-9A-Za-z._:-]"));
}

void identity_parsing() {
  SC_CASE("StrongId::parse refuses with the documented codes");
  const Result<SpaceNodeId> good = SpaceNodeId::parse("rack-01");
  SC_CHECK(good.ok());
  if (good.ok()) {
    SC_CHECK_EQ(good.value().str(), std::string("rack-01"));
    SC_CHECK_EQ(good.value().value(), std::string_view("rack-01"));
    SC_CHECK(!good.value().empty());
  }

  expect_error(SpaceNodeId::parse(""), ErrorCode::empty_value);
  expect_error(SpaceNodeId::parse(repeated('a', 129)), ErrorCode::identity_too_long);
  expect_error(SpaceNodeId::parse("-abc"), ErrorCode::malformed_identity);
  expect_error(SpaceNodeId::parse("abc-"), ErrorCode::malformed_identity);
  expect_error(SpaceNodeId::parse("ab cd"), ErrorCode::malformed_identity);
  expect_error(SpaceNodeId::parse("a/b"), ErrorCode::malformed_identity);
  expect_error(SpaceNodeId::parse(std::string_view("a\0b", 3)), ErrorCode::malformed_identity);
  expect_error(OccupancyClaimId::parse(""), ErrorCode::empty_value);
  expect_error(ExclusionRegionId::parse("a b"), ErrorCode::malformed_identity);

  // The three families built from the same text hold the same bytes and are
  // still three unrelated types: the static_asserts at the top of this file are
  // the compile-time half of this proof, and the bytes are the runtime half.
  const Result<SpaceNodeId> node = SpaceNodeId::parse("shared-name");
  const Result<OccupancyClaimId> claim = OccupancyClaimId::parse("shared-name");
  const Result<ExclusionRegionId> region = ExclusionRegionId::parse("shared-name");
  SC_CHECK(node.ok());
  SC_CHECK(claim.ok());
  SC_CHECK(region.ok());
  if (node.ok() && claim.ok() && region.ok()) {
    SC_CHECK_EQ(node.value().value(), claim.value().value());
    SC_CHECK_EQ(claim.value().value(), region.value().value());
  }

  // A default-constructed identity is empty, and empty is not an identity.
  const SpaceNodeId nothing;
  SC_CHECK(nothing.empty());
  SC_CHECK(OccupancyClaimId{}.empty());
  SC_CHECK(ExclusionRegionId{}.empty());
  SC_CHECK(!(SpaceNodeId::parse("a").value() == nothing));

  // Ordering inside one family is byte-wise.
  const Result<SpaceNodeId> earlier = SpaceNodeId::parse("rack-01");
  const Result<SpaceNodeId> later = SpaceNodeId::parse("rack-02");
  SC_CHECK(earlier.ok());
  SC_CHECK(later.ok());
  if (earlier.ok() && later.ok()) {
    SC_CHECK(earlier.value() < later.value());
    SC_CHECK(earlier.value() != later.value());
    SC_CHECK(earlier.value() == SpaceNodeId::parse("rack-01").value());
  }
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

void utf8_validation() {
  SC_CASE("is_utf8 accepts the four widths and refuses the rest");
  SC_CHECK(is_utf8(""));
  SC_CHECK(is_utf8("ascii only"));
  SC_CHECK(is_utf8("\xC2\xA9"));          // 2-byte U+00A9
  SC_CHECK(is_utf8("\xE2\x82\xAC"));      // 3-byte U+20AC
  SC_CHECK(is_utf8("\xF0\x9F\x98\x80"));  // 4-byte U+1F600
  SC_CHECK(is_utf8("mixed \xE2\x82\xAC text"));
  SC_CHECK(is_utf8("\x7F"));  // DEL is ASCII, and is_utf8 is not a label check

  // Overlong encodings spell a shorter sequence a second time.
  SC_CHECK(!is_utf8("\xC0\xAF"));
  SC_CHECK(!is_utf8("\xC1\x81"));
  SC_CHECK(!is_utf8("\xE0\x80\xAF"));
  SC_CHECK(!is_utf8("\xF0\x80\x80\xAF"));
  // Truncated sequences.
  SC_CHECK(!is_utf8("\xC2"));
  SC_CHECK(!is_utf8("\xE2\x82"));
  SC_CHECK(!is_utf8("\xF0\x9F\x98"));
  // A stray continuation byte is not a sequence.
  SC_CHECK(!is_utf8("\x80"));
  SC_CHECK(!is_utf8("ascii\x80"));
  // Surrogate halves and code points past U+10FFFF.
  SC_CHECK(!is_utf8("\xED\xA0\x80"));
  SC_CHECK(!is_utf8("\xED\xBF\xBF"));
  SC_CHECK(!is_utf8("\xF4\x90\x80\x80"));
  SC_CHECK(!is_utf8("\xF5\x80\x80\x80"));

  // A label is a narrower thing than UTF-8: control bytes are valid UTF-8 and
  // are still not a label.
  SC_CHECK(is_utf8("\n"));
  SC_CHECK(!is_valid_label("\n"));
  SC_CHECK(!is_valid_label(std::string_view("a\0b", 3)));
  SC_CHECK(is_valid_label("plain label"));
  SC_CHECK(is_valid_label("\xE2\x82\xAC"));
  SC_CHECK(is_valid_label(repeated('a', Limits::kMaxLabelBytes)));
  SC_CHECK(!is_valid_label(repeated('a', Limits::kMaxLabelBytes + 1)));
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

void attempt_identity() {
  SC_CASE("AttemptId orders by incarnation then sequence");
  const AttemptId first{StoreIncarnation{std::uint64_t{1}}, 5};
  const AttemptId same{StoreIncarnation{std::uint64_t{1}}, 5};
  const AttemptId next_sequence{StoreIncarnation{std::uint64_t{1}}, 6};
  const AttemptId next_incarnation{StoreIncarnation{std::uint64_t{2}}, 1};

  SC_CHECK(first == same);
  SC_CHECK(!(first != same));
  SC_CHECK(first != next_sequence);
  SC_CHECK(first < next_sequence);
  // The incarnation dominates: a later incarnation with a smaller sequence
  // still sorts above every attempt of the earlier incarnation.
  SC_CHECK(next_sequence < next_incarnation);
  SC_CHECK(!(next_incarnation < first));
  SC_CHECK(first <= same);
  SC_CHECK(next_incarnation > next_sequence);
  SC_CHECK_EQ(first.incarnation().value(), std::uint64_t{1});
  SC_CHECK_EQ(first.sequence(), std::uint64_t{5});
  SC_CHECK(!first.is_zero());
  SC_CHECK(AttemptId{}.is_zero());

  // Rendering is "<incarnation>.<sequence>".
  SC_CHECK_EQ(first.to_string(), std::string("1.5"));
  SC_CHECK_EQ((AttemptId{StoreIncarnation{std::uint64_t{12}}, 340}).to_string(),
              std::string("12.340"));
  SC_CHECK_EQ(AttemptId{}.to_string(), std::string("0.0"));
  SC_CHECK_EQ((AttemptId{StoreIncarnation{}, (std::numeric_limits<std::uint64_t>::max)()})
                  .to_string(),
              std::string("0.18446744073709551615"));

  // Sorting a shuffled list reproduces the total order above.
  const AttemptId origin{StoreIncarnation{std::uint64_t{1}}, 0};
  std::vector<AttemptId> attempts{next_incarnation, next_sequence, first, origin};
  std::sort(attempts.begin(), attempts.end());
  SC_CHECK_EQ(attempts.front(), origin);
  SC_CHECK_EQ(attempts.back(), next_incarnation);
}

void counters() {
  SC_CASE("CounterValue parse and next");
  constexpr std::uint64_t kMaxCounter = (std::numeric_limits<std::uint64_t>::max)();

  const Result<EntityGeneration> zero = EntityGeneration::parse("0");
  SC_CHECK(zero.ok());
  if (zero.ok()) {
    SC_CHECK_EQ(zero.value().value(), std::uint64_t{0});
    SC_CHECK(zero.value().is_zero());
    SC_CHECK_EQ(zero.value().to_string(), std::string("0"));
  }

  const Result<EntityGeneration> one = EntityGeneration::parse("1");
  SC_CHECK(one.ok());
  if (one.ok()) {
    SC_CHECK_EQ(one.value().value(), std::uint64_t{1});
    SC_CHECK(!one.value().is_max());
  }

  const Result<EntityGeneration> maximum = EntityGeneration::parse("18446744073709551615");
  SC_CHECK(maximum.ok());
  if (maximum.ok()) {
    SC_CHECK(maximum.value().is_max());
    SC_CHECK_EQ(maximum.value().value(), kMaxCounter);
    SC_CHECK_EQ(maximum.value().to_string(), std::string("18446744073709551615"));
  }

  expect_error(EntityGeneration::parse(""), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("00"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("01"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("-1"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("+1"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse(" 1"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("1 "), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("1.0"), ErrorCode::invalid_range);
  expect_error(EntityGeneration::parse("18446744073709551616"), ErrorCode::arithmetic_overflow);

  // next() advances by one and refuses to wrap at the maximum.
  const Result<EntityGeneration> advanced = EntityGeneration{std::uint64_t{5}}.next();
  SC_CHECK(advanced.ok());
  if (advanced.ok()) SC_CHECK_EQ(advanced.value().value(), std::uint64_t{6});
  const Result<EntityGeneration> wrapped = EntityGeneration{kMaxCounter}.next();
  SC_CHECK(!wrapped.ok());
  if (!wrapped.ok()) SC_CHECK_EQ(wrapped.error().code, ErrorCode::arithmetic_overflow);

  // Revisions and incarnations share the one parser.
  const Result<RegistryRevision> revision = RegistryRevision::parse("42");
  const Result<StoreIncarnation> incarnation = StoreIncarnation::parse("7");
  SC_CHECK(revision.ok());
  SC_CHECK(incarnation.ok());
  if (revision.ok()) SC_CHECK_EQ(revision.value().to_string(), std::string("42"));
  if (incarnation.ok()) SC_CHECK_EQ(incarnation.value().to_string(), std::string("7"));
  expect_error(RegistryRevision::parse("042"), ErrorCode::invalid_range);
  expect_error(StoreIncarnation::parse("x"), ErrorCode::invalid_range);
}

// ---------------------------------------------------------------------------
// Canonical text
// ---------------------------------------------------------------------------

void decimal_text() {
  SC_CASE("to_text renders every bound exactly");
  SC_CHECK_EQ(to_text(std::uint64_t{0}), std::string("0"));
  SC_CHECK_EQ(to_text(std::uint64_t{1}), std::string("1"));
  SC_CHECK_EQ(to_text((std::numeric_limits<std::uint64_t>::max)()),
              std::string("18446744073709551615"));
  SC_CHECK_EQ(to_text(std::int64_t{0}), std::string("0"));
  SC_CHECK_EQ(to_text(std::int64_t{1}), std::string("1"));
  SC_CHECK_EQ(to_text(std::int64_t{-1}), std::string("-1"));
  SC_CHECK_EQ(to_text(std::int64_t{-1000000}), std::string("-1000000"));
  SC_CHECK_EQ(to_text((std::numeric_limits<std::int64_t>::min)()),
              std::string("-9223372036854775808"));
  SC_CHECK_EQ(to_text((std::numeric_limits<std::int64_t>::max)()),
              std::string("9223372036854775807"));
  SC_CHECK_EQ(to_text((std::numeric_limits<std::uint32_t>::max)()), std::string("4294967295"));
  SC_CHECK_EQ(to_text((std::numeric_limits<std::int32_t>::min)()), std::string("-2147483648"));
}

void integer_parsing() {
  SC_CASE("parse_u64 and parse_i64 accept exactly one spelling");
  struct UnsignedCase final {
    const char* text;
    bool accepted;
    ErrorCode failure;
    std::uint64_t value;
  };
  const UnsignedCase unsigned_cases[] = {
      {"0", true, ErrorCode::ok, 0},
      {"1", true, ErrorCode::ok, 1},
      {"18446744073709551615", true, ErrorCode::ok, 18446744073709551615ull},
      {"", false, ErrorCode::invalid_range, 0},
      {"00", false, ErrorCode::invalid_range, 0},
      {"01", false, ErrorCode::invalid_range, 0},
      {"-1", false, ErrorCode::invalid_range, 0},
      {"+1", false, ErrorCode::invalid_range, 0},
      {" 1", false, ErrorCode::invalid_range, 0},
      {"1 ", false, ErrorCode::invalid_range, 0},
      {"1.0", false, ErrorCode::invalid_range, 0},
      {"0x10", false, ErrorCode::invalid_range, 0},
      {"18446744073709551616", false, ErrorCode::arithmetic_overflow, 0},
  };
  for (const UnsignedCase& item : unsigned_cases) {
    const Result<std::uint64_t> parsed = parse_u64(item.text);
    SC_CHECK_EQ(parsed.ok(), item.accepted);
    if (parsed.ok()) {
      SC_CHECK_EQ(parsed.value(), item.value);
    } else {
      SC_CHECK_EQ(parsed.error().code, item.failure);
    }
  }

  struct SignedCase final {
    const char* text;
    bool accepted;
    ErrorCode failure;
    std::int64_t value;
  };
  const SignedCase signed_cases[] = {
      {"0", true, ErrorCode::ok, 0},
      {"1", true, ErrorCode::ok, 1},
      {"-1", true, ErrorCode::ok, -1},
      {"9223372036854775807", true, ErrorCode::ok, (std::numeric_limits<std::int64_t>::max)()},
      {"-9223372036854775808", true, ErrorCode::ok, (std::numeric_limits<std::int64_t>::min)()},
      {"", false, ErrorCode::invalid_range, 0},
      {"-", false, ErrorCode::invalid_range, 0},
      {"-0", false, ErrorCode::invalid_range, 0},
      {"00", false, ErrorCode::invalid_range, 0},
      {"+1", false, ErrorCode::invalid_range, 0},
      {" 1", false, ErrorCode::invalid_range, 0},
      {"1 ", false, ErrorCode::invalid_range, 0},
      {"1.0", false, ErrorCode::invalid_range, 0},
      {"9223372036854775808", false, ErrorCode::arithmetic_overflow, 0},
      {"-9223372036854775809", false, ErrorCode::arithmetic_overflow, 0},
  };
  for (const SignedCase& item : signed_cases) {
    const Result<std::int64_t> parsed = parse_i64(item.text);
    SC_CHECK_EQ(parsed.ok(), item.accepted);
    if (parsed.ok()) {
      SC_CHECK_EQ(parsed.value(), item.value);
    } else {
      SC_CHECK_EQ(parsed.error().code, item.failure);
    }
  }

  // Rendering and parsing are inverses at every bound.
  const std::int64_t signed_values[] = {0,
                                        1,
                                        -1,
                                        1234567,
                                        -1234567,
                                        (std::numeric_limits<std::int64_t>::min)(),
                                        (std::numeric_limits<std::int64_t>::max)()};
  for (const std::int64_t value : signed_values) {
    const Result<std::int64_t> round = parse_i64(to_text(value));
    SC_CHECK(round.ok());
    if (round.ok()) SC_CHECK_EQ(round.value(), value);
  }
  const std::uint64_t unsigned_values[] = {0, 1, 4096,
                                           (std::numeric_limits<std::uint64_t>::max)()};
  for (const std::uint64_t value : unsigned_values) {
    const Result<std::uint64_t> round = parse_u64(to_text(value));
    SC_CHECK(round.ok());
    if (round.ok()) SC_CHECK_EQ(round.value(), value);
  }

  const Result<bool> yes = parse_bool("true");
  const Result<bool> no = parse_bool("false");
  SC_CHECK(yes.ok());
  SC_CHECK(no.ok());
  if (yes.ok()) SC_CHECK(yes.value());
  if (no.ok()) SC_CHECK(!no.value());
  expect_error(parse_bool("True"), ErrorCode::unknown_enum_token);
  expect_error(parse_bool("FALSE"), ErrorCode::unknown_enum_token);
  expect_error(parse_bool("1"), ErrorCode::unknown_enum_token);
  expect_error(parse_bool(""), ErrorCode::unknown_enum_token);
}

void quoting() {
  SC_CASE("quote_text and unquote_text round-trip");
  const Result<std::string> plain = quote_text("abc");
  SC_CHECK(plain.ok());
  if (plain.ok()) SC_CHECK_EQ(plain.value(), std::string("\"abc\""));

  const Result<std::string> nothing = quote_text("");
  SC_CHECK(nothing.ok());
  if (nothing.ok()) SC_CHECK_EQ(nothing.value(), std::string("\"\""));

  const Result<std::string> quote_inside = quote_text("a\"b");
  SC_CHECK(quote_inside.ok());
  if (quote_inside.ok()) SC_CHECK_EQ(quote_inside.value(), std::string("\"a\\\"b\""));

  const Result<std::string> backslash_inside = quote_text("a\\b");
  SC_CHECK(backslash_inside.ok());
  if (backslash_inside.ok()) SC_CHECK_EQ(backslash_inside.value(), std::string("\"a\\\\b\""));

  const Result<std::string> unquoted_ok = unquote_text("\"abc\"");
  SC_CHECK(unquoted_ok.ok());
  if (unquoted_ok.ok()) SC_CHECK_EQ(unquoted_ok.value(), std::string("abc"));
  const Result<std::string> unquoted_empty = unquote_text("\"\"");
  SC_CHECK(unquoted_empty.ok());
  if (unquoted_empty.ok()) SC_CHECK_EQ(unquoted_empty.value(), std::string(""));

  // A canonical document never carries a control byte.
  expect_error(quote_text("\n"), ErrorCode::invalid_character);
  expect_error(quote_text(std::string_view("a\0b", 3)), ErrorCode::invalid_character);

  // Refusals of the inverse form.
  expect_error(unquote_text(""), ErrorCode::malformed_identity);
  expect_error(unquote_text("abc"), ErrorCode::malformed_identity);
  expect_error(unquote_text("\""), ErrorCode::malformed_identity);
  expect_error(unquote_text("\"abc"), ErrorCode::malformed_identity);
  expect_error(unquote_text("abc\""), ErrorCode::malformed_identity);
  expect_error(unquote_text("\"\\\""), ErrorCode::malformed_identity);   // dangling escape
  expect_error(unquote_text("\"a\\b\""), ErrorCode::malformed_identity);  // unknown escape
  expect_error(unquote_text(std::string("\"a\nb\"")), ErrorCode::malformed_identity);

  const std::string_view round_trip[] = {
      std::string_view(""),
      std::string_view("a"),
      std::string_view("plain text"),
      std::string_view("back\\slash"),
      std::string_view("quo\"te"),
      std::string_view("\xE2\x82\xAC unicode"),
      std::string_view("trailing backslash \\"),
  };
  for (const std::string_view original : round_trip) {
    const Result<std::string> quoted = quote_text(original);
    SC_CHECK(quoted.ok());
    if (!quoted.ok()) continue;
    const Result<std::string> restored = unquote_text(quoted.value());
    SC_CHECK(restored.ok());
    if (!restored.ok()) continue;
    SC_CHECK_EQ(restored.value(), std::string(original));
  }
}

void line_splitting() {
  SC_CASE("split_lines accepts LF and CRLF and refuses NUL");
  const Result<std::vector<std::string_view>> empty = split_lines("");
  SC_CHECK(empty.ok());
  if (empty.ok()) SC_CHECK_EQ(empty.value().size(), std::size_t{0});

  const Result<std::vector<std::string_view>> single = split_lines("a");
  SC_CHECK(single.ok());
  if (single.ok()) {
    SC_CHECK_EQ(single.value().size(), std::size_t{1});
    SC_CHECK_EQ(single.value().at(0), std::string_view("a"));
  }

  // A trailing separator ends the last line rather than opening an empty one.
  const Result<std::vector<std::string_view>> trailing = split_lines("a\n");
  SC_CHECK(trailing.ok());
  if (trailing.ok()) {
    SC_CHECK_EQ(trailing.value().size(), std::size_t{1});
    SC_CHECK_EQ(trailing.value().at(0), std::string_view("a"));
  }

  const Result<std::vector<std::string_view>> two = split_lines("a\nb");
  SC_CHECK(two.ok());
  if (two.ok()) {
    SC_CHECK_EQ(two.value().size(), std::size_t{2});
    SC_CHECK_EQ(two.value().at(0), std::string_view("a"));
    SC_CHECK_EQ(two.value().at(1), std::string_view("b"));
  }

  // A CRLF pair is one separator.
  const Result<std::vector<std::string_view>> crlf = split_lines("a\r\nb\r\n");
  SC_CHECK(crlf.ok());
  if (crlf.ok()) {
    SC_CHECK_EQ(crlf.value().size(), std::size_t{2});
    SC_CHECK_EQ(crlf.value().at(0), std::string_view("a"));
    SC_CHECK_EQ(crlf.value().at(1), std::string_view("b"));
  }

  // A lone CR is a separator on its own.
  const Result<std::vector<std::string_view>> lone_cr = split_lines("a\rb");
  SC_CHECK(lone_cr.ok());
  if (lone_cr.ok()) {
    SC_CHECK_EQ(lone_cr.value().size(), std::size_t{2});
    SC_CHECK_EQ(lone_cr.value().at(0), std::string_view("a"));
    SC_CHECK_EQ(lone_cr.value().at(1), std::string_view("b"));
  }

  // An interior empty line is a line.
  const Result<std::vector<std::string_view>> blank = split_lines("a\n\nb");
  SC_CHECK(blank.ok());
  if (blank.ok()) {
    SC_CHECK_EQ(blank.value().size(), std::size_t{3});
    SC_CHECK_EQ(blank.value().at(1), std::string_view(""));
  }

  // A NUL byte is never part of a canonical document.
  expect_error(split_lines(std::string_view("a\0b", 3)), ErrorCode::invalid_character);
  expect_error(split_lines(std::string_view("\0", 1)), ErrorCode::invalid_character);
}

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------

void digest_text_and_vectors() {
  SC_CASE("Digest round-trips and matches the FIPS 180-4 vectors");
  const Digest abc = sha256(std::string_view("abc"));
  SC_CHECK_EQ(abc.hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const Digest empty = sha256(std::string_view(""));
  SC_CHECK_EQ(empty.hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  SC_CHECK_EQ(abc.hex().size(), std::size_t{64});
  for (const char digit : abc.hex()) {
    SC_CHECK((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'));
  }
  SC_CHECK_EQ(abc.tagged_hex(), std::string("sha256:") + abc.hex());
  SC_CHECK_EQ(abc.tagged_hex().size(), std::size_t{71});

  // parse round-trips, and accepts upper case as the same value.
  const Result<Digest> parsed = Digest::parse(abc.hex());
  SC_CHECK(parsed.ok());
  if (parsed.ok()) SC_CHECK_EQ(parsed.value(), abc);
  std::string upper = abc.hex();
  for (char& digit : upper) {
    if (digit >= 'a' && digit <= 'f') digit = static_cast<char>((digit - 'a') + 'A');
  }
  const Result<Digest> parsed_upper = Digest::parse(upper);
  SC_CHECK(parsed_upper.ok());
  if (parsed_upper.ok()) SC_CHECK_EQ(parsed_upper.value(), abc);

  // A wrong length is a range failure, and a wrong character is a character
  // failure.
  expect_error(Digest::parse(abc.hex().substr(0, 63)), ErrorCode::invalid_range);
  expect_error(Digest::parse(abc.hex() + "0"), ErrorCode::invalid_range);
  expect_error(Digest::parse(""), ErrorCode::invalid_range);
  std::string non_hex = abc.hex();
  non_hex[10] = 'z';
  expect_error(Digest::parse(non_hex), ErrorCode::invalid_character);

  // The tagged form requires the tag.
  const Result<Digest> tagged = Digest::parse_tagged(abc.tagged_hex());
  SC_CHECK(tagged.ok());
  if (tagged.ok()) SC_CHECK_EQ(tagged.value(), abc);
  expect_error(Digest::parse_tagged(abc.hex()), ErrorCode::malformed_identity);
  expect_error(Digest::parse_tagged(std::string("md5:") + abc.hex()),
               ErrorCode::malformed_identity);
  expect_error(Digest::parse_tagged("sha256"), ErrorCode::malformed_identity);
  expect_error(Digest::parse_tagged("sha256:"), ErrorCode::invalid_range);
  expect_error(Digest::parse_tagged("sha256:" + abc.hex().substr(0, 63)),
               ErrorCode::invalid_range);

  // is_zero is exact: all 32 bytes must be zero.
  const Digest zero_digest;
  SC_CHECK(zero_digest.is_zero());
  SC_CHECK(!abc.is_zero());
  SC_CHECK(!empty.is_zero());
  const std::array<std::uint8_t, kDigestBytes> zeros{};
  SC_CHECK(Digest::from_bytes(zeros.data(), zeros.size()).is_zero());
  std::array<std::uint8_t, kDigestBytes> one_bit{};
  one_bit[kDigestBytes - 1] = 1;
  SC_CHECK(!Digest::from_bytes(one_bit.data(), one_bit.size()).is_zero());

  // Streaming equals one shot.
  Sha256 stream;
  stream.update(std::string_view("ab"));
  stream.update(std::string_view("c"));
  SC_CHECK_EQ(stream.finish(), abc);
  SC_CHECK_EQ(stream.finish(), abc);  // sampling does not disturb the hash
  stream.reset();
  SC_CHECK_EQ(stream.finish(), empty);
}

}  // namespace

int main() {
  identifier_grammar();
  identity_parsing();
  utf8_validation();
  attempt_identity();
  counters();
  decimal_text();
  integer_parsing();
  quoting();
  line_splitting();
  digest_text_and_vectors();
  return ::sc_test::summary("identity_test");
}
