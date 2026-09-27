// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - SHA-256 (FIPS 180-4) and the digest value type.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The digest is used for three distinct purposes and the differences matter:
//
//   * store integrity - the digest of a serialized image, checked before the
//     image is parsed. A matching digest means "these bytes are the bytes that
//     were written". It does not mean "this state is correct": validity is
//     decided by the model audit that follows, never by the checksum alone.
//   * content addressing - deterministic identity of a canonical state image,
//     so two independently built states can be compared exactly.
//   * domain separation - every digest of derived material is taken over a
//     domain label, a 0x1F separator and a big-endian length, so a digest
//     computed for one purpose can never be mistaken for another.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/error.hpp"

namespace dccp::space_capacity {

inline constexpr std::size_t kDigestBytes = 32;
inline constexpr std::size_t kDigestHexChars = 64;
inline constexpr char kDigestDomainSeparator = '\x1f';

class SC_API Digest final {
 public:
  Digest() noexcept = default;
  explicit Digest(const std::array<std::uint8_t, kDigestBytes>& bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static Digest from_bytes(const std::uint8_t* data, std::size_t size) noexcept;

  // Parses exactly 64 hexadecimal characters, lower or upper case. Returns an
  // error for any other length or for a non-hexadecimal character.
  [[nodiscard]] static Result<Digest> parse(std::string_view text);
  // Parses the tagged form "sha256:<64 hex>". The tag is required.
  [[nodiscard]] static Result<Digest> parse_tagged(std::string_view text);

  [[nodiscard]] const std::array<std::uint8_t, kDigestBytes>& bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] std::string tagged_hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  [[nodiscard]] friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  [[nodiscard]] friend bool operator!=(const Digest& a, const Digest& b) noexcept {
    return !(a == b);
  }
  [[nodiscard]] friend bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

 private:
  std::array<std::uint8_t, kDigestBytes> bytes_{};
};

// Streaming SHA-256. `finish()` returns the digest of everything written so
// far and does not disturb the object, so a running hash can be sampled.
class SC_API Sha256 final {
 public:
  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  [[nodiscard]] Digest finish() const noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_ = 0;
};

SC_API Digest sha256(const void* data, std::size_t size) noexcept;
SC_API Digest sha256(std::string_view text) noexcept;

// Domain-separated digest of a payload: SHA-256 over
//   domain || 0x1F || big-endian-uint64(payload size) || payload
// A digest computed under one domain label can never equal a digest computed
// under another, even for identical payload bytes.
SC_API Digest digest_in_domain(std::string_view domain, const void* payload,
                               std::size_t size) noexcept;
SC_API Digest digest_in_domain(std::string_view domain, std::string_view payload) noexcept;

// Stable domain labels. These are part of the durable format and are never
// reused for a different purpose.
inline constexpr std::string_view kDomainStateDocument = "space-capacity.state-document.v1";
inline constexpr std::string_view kDomainStateBinary = "space-capacity.state-binary.v1";
inline constexpr std::string_view kDomainRequest = "space-capacity.request.v1";
inline constexpr std::string_view kDomainSnapshot = "space-capacity.snapshot.v1";
inline constexpr std::string_view kDomainStoreIdentity = "space-capacity.store-identity.v1";

}  // namespace dccp::space_capacity
