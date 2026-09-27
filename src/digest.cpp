// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - SHA-256 (FIPS 180-4) and the digest value type.
//
// The hash is implemented here so that the library carries no third-party
// dependency and so that padding, byte order and failure behaviour are fixed by
// this file instead of by a platform. The round function allocates nothing: the
// message schedule is a fixed stack array.
//
// Three choices are deliberate and are relied upon elsewhere:
//
//   * finish() runs on a copy of the running state. It is const, it can be
//     called repeatedly, and a hash can be sampled while it keeps streaming.
//   * the 64-bit bit-length field saturates at 0xFFFF'FFFF'FFFF'FFFF instead of
//     wrapping once the message reaches 2^61 bytes. A length that large cannot
//     be described by the field, and wrapping would silently alias two
//     different messages onto one digest, so the field is pinned. The byte
//     counter saturates with it, so the value stays a monotone function of the
//     message.
//   * a null pointer with a non-zero size contributes nothing rather than
//     faulting, because update() is noexcept and has no way to report. Callers
//     that pass a pointer must pass a valid one; size == 0 is always safe.
//
// Parsing never echoes caller text into an error message, so every diagnostic
// this file produces is a fixed string with no address or locale content.

#include "dccp/space_capacity/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace dccp::space_capacity {
namespace {

constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kLengthFieldBytes = 8;
// One 0x80 byte, up to 63 zero bytes and the length field.
constexpr std::size_t kMaxTailBytes = 1 + 63 + kLengthFieldBytes;

// The required prefix of the printable tagged form.
constexpr std::string_view kSha256Tag = "sha256:";

constexpr char kHexDigits[] = "0123456789abcdef";

// Saturating value for both the byte counter and the bit-length field.
constexpr std::uint64_t kSaturatedLength = 0xFFFF'FFFF'FFFF'FFFFull;

// Rotate right. The shift amount is masked, so a rotation by zero rotates by
// zero rather than shifting by the width, which would be undefined.
constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) noexcept {
  return (value >> amount) | (value << ((32u - amount) & 31u));
}

// Value of a single hexadecimal digit, or -1 when the character is not one.
constexpr std::int32_t hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<std::int32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<std::int32_t>(c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return static_cast<std::int32_t>(c - 'A') + 10;
  return -1;
}

// FIPS 180-4 section 4.2.2.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

}  // namespace

// ---------------------------------------------------------------- Digest value

Digest Digest::from_bytes(const std::uint8_t* data, std::size_t size) noexcept {
  std::array<std::uint8_t, kDigestBytes> bytes{};
  if (data == nullptr) {
    // No data at all: the zero digest. There is nothing to read and nothing to
    // guess at.
    return Digest(bytes);
  }
  const std::size_t copied = (size < kDigestBytes) ? size : kDigestBytes;
  if (copied != 0) {
    std::memcpy(bytes.data(), data, copied);
  }
  // A short source leaves the remaining bytes zero, as constructed above.
  return Digest(bytes);
}

Result<Digest> Digest::parse(std::string_view text) {
  if (text.size() != kDigestHexChars) {
    return Error(ErrorCode::invalid_range,
                 "digest text must be exactly 64 hexadecimal characters");
  }
  std::array<std::uint8_t, kDigestBytes> bytes{};
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    const std::int32_t high = hex_value(text[i * 2]);
    const std::int32_t low = hex_value(text[(i * 2) + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::invalid_character,
                   "digest text contains a character that is not a hexadecimal digit");
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Digest(bytes);
}

Result<Digest> Digest::parse_tagged(std::string_view text) {
  if (text.size() < kSha256Tag.size() || text.substr(0, kSha256Tag.size()) != kSha256Tag) {
    return Error(ErrorCode::malformed_identity, "digest text must be tagged with the prefix sha256:");
  }
  // The tag is settled; the remainder is an untagged digest, so a wrong length
  // or a non-hexadecimal character is reported with the same codes parse() uses.
  return parse(text.substr(kSha256Tag.size()));
}

std::string Digest::hex() const {
  std::string out(kDigestHexChars, '0');
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    const std::uint8_t byte = bytes_[i];
    out[i * 2] = kHexDigits[static_cast<std::size_t>(byte >> 4)];
    out[(i * 2) + 1] = kHexDigits[static_cast<std::size_t>(byte & 0x0Fu)];
  }
  return out;
}

std::string Digest::tagged_hex() const {
  std::string out;
  out.reserve(kSha256Tag.size() + kDigestHexChars);
  out.append(kSha256Tag);
  out.append(hex());
  return out;
}

bool Digest::is_zero() const noexcept {
  std::uint8_t accumulator = 0;
  for (const std::uint8_t byte : bytes_) {
    accumulator = static_cast<std::uint8_t>(accumulator | byte);
  }
  return accumulator == 0;
}

// ------------------------------------------------------------------- SHA-256

void Sha256::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  total_bytes_ = 0;
  buffered_ = 0;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }

  // Saturating byte counter. See the file comment: the bit-length field is a
  // 64-bit quantity and cannot describe 2^61 bytes or more.
  if (total_bytes_ > kSaturatedLength - static_cast<std::uint64_t>(size)) {
    total_bytes_ = kSaturatedLength;
  } else {
    total_bytes_ += static_cast<std::uint64_t>(size);
  }

  const auto* next = static_cast<const std::uint8_t*>(data);

  // Top up a partial block first, so that every later compression reads whole
  // blocks straight out of the caller's buffer.
  if (buffered_ != 0) {
    const std::size_t wanted = kBlockBytes - buffered_;
    const std::size_t taken = (size < wanted) ? size : wanted;
    std::memcpy(buffer_.data() + buffered_, next, taken);
    buffered_ += taken;
    next += taken;
    size -= taken;
    if (buffered_ != kBlockBytes) {
      return;
    }
    compress(buffer_.data());
    buffered_ = 0;
  }

  while (size >= kBlockBytes) {
    compress(next);
    next += kBlockBytes;
    size -= kBlockBytes;
  }

  if (size != 0) {
    std::memcpy(buffer_.data(), next, size);
    buffered_ = size;
  }
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    const std::size_t at = i * 4;
    schedule[i] = (static_cast<std::uint32_t>(block[at]) << 24) |
                  (static_cast<std::uint32_t>(block[at + 1]) << 16) |
                  (static_cast<std::uint32_t>(block[at + 2]) << 8) |
                  static_cast<std::uint32_t>(block[at + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t previous = schedule[i - 15];
    const std::uint32_t recent = schedule[i - 2];
    const std::uint32_t s0 = rotr(previous, 7) ^ rotr(previous, 18) ^ (previous >> 3);
    const std::uint32_t s1 = rotr(recent, 17) ^ rotr(recent, 19) ^ (recent >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + big_s1 + choose + kRoundConstants[i] + schedule[i];
    const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Digest Sha256::finish() const noexcept {
  // Everything below happens on a copy, so *this is left exactly as it was and
  // a running hash can be sampled as often as the caller likes.
  Sha256 final_state = *this;

  // The bit length is captured before padding, and saturates rather than wraps.
  const std::uint64_t bits = (final_state.total_bytes_ > (kSaturatedLength >> 3))
                                 ? kSaturatedLength
                                 : (final_state.total_bytes_ << 3);

  std::array<std::uint8_t, kMaxTailBytes> tail{};
  tail[0] = static_cast<std::uint8_t>(0x80);
  const std::size_t remainder = final_state.buffered_;
  // Zero bytes between the 0x80 byte and the length field, chosen so that the
  // padded message ends on a block boundary. remainder <= 55 stays in this
  // block; anything larger needs a whole extra block.
  const std::size_t zeros = (remainder <= 55) ? (55 - remainder) : (119 - remainder);
  const std::size_t tail_size = zeros + 1 + kLengthFieldBytes;
  for (std::size_t i = 0; i < kLengthFieldBytes; ++i) {
    tail[tail_size - kLengthFieldBytes + i] =
        static_cast<std::uint8_t>((bits >> (56u - (8u * static_cast<unsigned>(i)))) & 0xFFu);
  }
  final_state.update(tail.data(), tail_size);

  // The state is eight big-endian words, high byte first.
  std::array<std::uint8_t, kDigestBytes> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    const std::uint32_t word = final_state.state_[i];
    bytes[(i * 4)] = static_cast<std::uint8_t>((word >> 24) & 0xFFu);
    bytes[(i * 4) + 1] = static_cast<std::uint8_t>((word >> 16) & 0xFFu);
    bytes[(i * 4) + 2] = static_cast<std::uint8_t>((word >> 8) & 0xFFu);
    bytes[(i * 4) + 3] = static_cast<std::uint8_t>(word & 0xFFu);
  }
  return Digest(bytes);
}

// ------------------------------------------------------------- entry points

Digest sha256(const void* data, std::size_t size) noexcept {
  Sha256 hash;
  hash.update(data, size);
  return hash.finish();
}

Digest sha256(std::string_view text) noexcept { return sha256(text.data(), text.size()); }

Digest digest_in_domain(std::string_view domain, const void* payload, std::size_t size) noexcept {
  Sha256 hash;
  hash.update(domain.data(), domain.size());

  const std::uint8_t separator = static_cast<std::uint8_t>(kDigestDomainSeparator);
  hash.update(&separator, 1);

  // Big-endian length of the payload, so that the boundary between the domain
  // and the payload is unambiguous for every pair of inputs.
  const std::uint64_t length = static_cast<std::uint64_t>(size);
  std::array<std::uint8_t, kLengthFieldBytes> encoded_length{};
  for (std::size_t i = 0; i < kLengthFieldBytes; ++i) {
    encoded_length[i] =
        static_cast<std::uint8_t>((length >> (56u - (8u * static_cast<unsigned>(i)))) & 0xFFu);
  }
  hash.update(encoded_length.data(), encoded_length.size());

  hash.update(payload, size);
  return hash.finish();
}

Digest digest_in_domain(std::string_view domain, std::string_view payload) noexcept {
  return digest_in_domain(domain, payload.data(), payload.size());
}

}  // namespace dccp::space_capacity
