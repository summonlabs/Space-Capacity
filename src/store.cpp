// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the durable store.
//
// The commit protocol is the whole point of this file:
//
//   plan -> validate -> reserve the next revision and attempt -> write staging
//   -> flush -> read back and verify -> retain the previous generation ->
//   atomic publish (the commit point) -> flush the directory -> release the
//   writer lock -> re-read and verify what is actually on disk
//
// Nothing is authoritative before the commit point, exactly one whole
// generation is visible after it, and the in-memory state is published only
// after the visible bytes have been read back and verified. Acknowledgement
// and verified effect are separate steps, and the second one decides success.
//
// The writer lock is an operating-system lock, never the contents of a file. A
// lock file left behind by a dead process is a stale file: the lock is
// genuinely held the moment it is acquired, and the file is rewritten then.

#include "dccp/space_capacity/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "dccp/space_capacity/text.hpp"
#include "dccp/space_capacity/version.hpp"
#include "platform.hpp"

namespace dccp::space_capacity {
namespace {

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320)
//
// A cheap early rejection of a corrupted header, checked before the header is
// interpreted at all. It is a corruption detector, not an authenticity
// mechanism; the SHA-256 trailer is what decides integrity.
// ---------------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  static const auto table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      values[i] = value;
    }
    return values;
  }();
  std::uint32_t value = 0xFFFF'FFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    value = table[(value ^ data[i]) & 0xFFu] ^ (value >> 8);
  }
  return value ^ 0xFFFF'FFFFu;
}

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>(value & 0xFFu));
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void put_i64(std::string& out, std::int64_t value) {
  put_u64(out, static_cast<std::uint64_t>(value));
}

bool read_u32(std::string_view bytes, std::size_t offset, std::uint32_t& out) noexcept {
  if (offset + 4 > bytes.size()) return false;
  out = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) << 24) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3]));
  return true;
}

bool read_u64(std::string_view bytes, std::size_t offset, std::uint64_t& out) noexcept {
  if (offset + 8 > bytes.size()) return false;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint8_t>(bytes[offset + i]);
  }
  out = value;
  return true;
}

bool read_i64(std::string_view bytes, std::size_t offset, std::int64_t& out) noexcept {
  std::uint64_t raw = 0;
  if (!read_u64(bytes, offset, raw)) return false;
  out = static_cast<std::int64_t>(raw);
  return true;
}

// Header field offsets. Part of the durable format; never renumbered.
constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetFormat = 8;
constexpr std::size_t kOffsetModel = 12;
constexpr std::size_t kOffsetEndian = 16;
constexpr std::size_t kOffsetCoordinate = 20;
constexpr std::size_t kOffsetRevision = 24;
constexpr std::size_t kOffsetIncarnation = 32;
constexpr std::size_t kOffsetAttempt = 40;
constexpr std::size_t kOffsetPayloadLength = 48;
constexpr std::size_t kOffsetFlags = 56;
constexpr std::size_t kOffsetHeaderCrc = 60;
constexpr std::size_t kOffsetPayloadDigest = 64;
constexpr std::size_t kOffsetIdentityDigest = 96;
constexpr std::size_t kOffsetIdentityLength = 128;
constexpr std::size_t kOffsetReserved = 132;

// Layout of the variable-length region that follows the fixed header:
//   [kStoreHeaderBytes, + identity_length)      the store identity as text
//   [.., + 8)                                   the creation instant, i64 BE
//   [.., + payload_length)                      the payload
//   [.., + kStoreTrailerBytes)                  the trailer
std::size_t identity_offset() { return kStoreHeaderBytes; }
std::size_t created_at_offset(std::size_t identity_length) {
  return kStoreHeaderBytes + identity_length;
}
std::size_t payload_offset(std::size_t identity_length) { return created_at_offset(identity_length) + 8; }

std::string magic_bytes() {
  std::string magic(kStoreMagic);
  magic.resize(8, '\0');
  return magic;
}

std::string trailer_magic_bytes() {
  std::string magic(kStoreTrailerMagic);
  magic.resize(8, '\0');
  return magic;
}

std::string identity_anchor_text(const StoreId& store, StoreIncarnation incarnation,
                                 Timestamp created_at) {
  std::string body;
  body += "store ";
  body += store.str();
  body += "\nincarnation ";
  body += incarnation.to_string();
  body += "\ncreated ";
  body += to_text(created_at);
  body += "\n";

  // The anchor digest covers exactly the body that precedes it, so an anchor
  // that was edited by hand, truncated or swapped is refused rather than read.
  std::string out(kIdentityBanner);
  out += "\n";
  out += body;
  out += "digest ";
  out += sha256(body).tagged_hex();
  out += "\n";
  return out;
}

struct Anchor final {
  StoreId store{};
  StoreIncarnation incarnation{};
  Timestamp created_at{};
};

Result<Anchor> parse_identity_anchor(std::string_view text) {
  Result<std::vector<std::string_view>> lines = split_lines(text);
  if (!lines) return lines.error();
  const std::vector<std::string_view>& all = lines.value();
  if (all.size() != 5) {
    return Error::make(ErrorCode::corruption,
                       "the identity anchor must hold exactly a banner and four fields");
  }
  if (all[0] != kIdentityBanner) {
    return Error::make(ErrorCode::wrong_store, "the identity anchor banner is not recognised");
  }
  Anchor anchor;
  for (std::size_t i = 1; i < 4; ++i) {
    const std::string_view line = all[i];
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
      return Error::make(ErrorCode::corruption, "a malformed identity anchor field");
    }
    const std::string_view key = line.substr(0, space);
    const std::string_view value = line.substr(space + 1);
    if (key == "store") {
      Result<StoreId> parsed = StoreId::parse(value);
      if (!parsed) return parsed.error();
      anchor.store = std::move(parsed).value();
    } else if (key == "incarnation") {
      Result<StoreIncarnation> parsed = StoreIncarnation::parse(value);
      if (!parsed) return parsed.error();
      anchor.incarnation = std::move(parsed).value();
    } else if (key == "created") {
      Timestamp stamp;
      if (!parse_timestamp(value, stamp)) {
        return Error::make(ErrorCode::corruption, "the identity anchor timestamp is malformed");
      }
      anchor.created_at = stamp;
    } else {
      return Error::make(ErrorCode::corruption, "an unknown identity anchor field");
    }
  }
  const std::string_view digest_line = all[4];
  if (digest_line.size() < 7 || digest_line.substr(0, 7) != "digest ") {
    return Error::make(ErrorCode::corruption, "the identity anchor has no digest line");
  }
  Result<Digest> stated = Digest::parse_tagged(digest_line.substr(7));
  if (!stated) return stated.error();
  // The anchor digest covers every byte between the banner and the digest line.
  const std::size_t body_begin = text.find('\n');
  const std::size_t body_end = text.rfind("digest ");
  if (body_begin == std::string_view::npos || body_end == std::string_view::npos ||
      body_end <= body_begin) {
    return Error::make(ErrorCode::corruption, "the identity anchor body cannot be located");
  }
  const Digest computed = sha256(text.substr(body_begin + 1, body_end - body_begin - 1));
  if (computed != stated.value()) {
    return Error::make(ErrorCode::integrity_failure,
                       "the identity anchor digest does not match its contents");
  }
  return anchor;
}

std::string staging_name(const std::filesystem::path& state_path, std::uint64_t counter) {
  std::string name = state_path.filename().string();
  name += kStagingMarker;
  name += std::to_string(internal::process_id());
  name += "-";
  name += std::to_string(counter);
  return (state_path.parent_path() / name).string();
}

}  // namespace

std::string_view write_stage_name(WriteStage stage) noexcept {
  switch (stage) {
    case WriteStage::none:
      return "none";
    case WriteStage::lock_acquired:
      return "lock-acquired";
    case WriteStage::staging_planned:
      return "staging-planned";
    case WriteStage::staging_written:
      return "staging-written";
    case WriteStage::staging_flushed:
      return "staging-flushed";
    case WriteStage::staging_verified:
      return "staging-verified";
    case WriteStage::previous_retained:
      return "previous-retained";
    case WriteStage::before_publish:
      return "before-publish";
    case WriteStage::after_publish:
      return "after-publish";
    case WriteStage::directory_flushed:
      return "directory-flushed";
    case WriteStage::lock_released:
      return "lock-released";
    case WriteStage::published_verified:
      return "published-verified";
  }
  return "unknown";
}

std::string_view recovery_action_name(RecoveryAction action) noexcept {
  switch (action) {
    case RecoveryAction::none:
      return "none";
    case RecoveryAction::created:
      return "created";
    case RecoveryAction::loaded_current:
      return "loaded-current";
    case RecoveryAction::loaded_previous:
      return "loaded-previous";
    case RecoveryAction::no_state_accepted:
      return "no-state-accepted";
    case RecoveryAction::retired_staging:
      return "retired-staging";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Image framing
// ---------------------------------------------------------------------------

std::string Store::frame(const Snapshot& snapshot) {
  const std::string payload = snapshot.canonical_bytes();
  const std::string identity = snapshot.store().str();
  const Digest payload_digest = sha256(payload);
  const Digest identity_digest = digest_in_domain(kDomainStoreIdentity, identity);

  std::string header;
  header.reserve(kStoreHeaderBytes);
  header += magic_bytes();
  put_u32(header, kStoreFormatVersion);
  put_u32(header, kModelRevision);
  put_u32(header, kEndianTag);
  put_u32(header, kMountSlotsPerRackUnit);
  put_u64(header, snapshot.revision().value());
  put_u64(header, snapshot.incarnation().value());
  put_u64(header, snapshot.attempt().sequence());
  put_u64(header, static_cast<std::uint64_t>(payload.size()));
  put_u32(header, 0);  // flags
  put_u32(header, crc32(reinterpret_cast<const std::uint8_t*>(header.data()), kOffsetHeaderCrc));
  header.append(reinterpret_cast<const char*>(payload_digest.bytes().data()), kDigestBytes);
  header.append(reinterpret_cast<const char*>(identity_digest.bytes().data()), kDigestBytes);
  put_u32(header, static_cast<std::uint32_t>(identity.size()));
  put_u32(header, 0);  // reserved, must be zero

  std::string image;
  image.reserve(kStoreHeaderBytes + identity.size() + 8 + payload.size() + kStoreTrailerBytes);
  image += header;
  image += identity;
  put_i64(image, snapshot.created_at().unix_millis);
  image += payload;

  const Digest trailer = digest_in_domain(kDomainStateBinary, image);
  image.append(reinterpret_cast<const char*>(trailer.bytes().data()), kDigestBytes);
  image += trailer_magic_bytes();
  return image;
}

Result<Store::Decoded> Store::inspect_image(std::string_view bytes) {
  if (bytes.size() < kStoreHeaderBytes + kStoreTrailerBytes) {
    return Error::make(ErrorCode::truncated_input, "the state image is shorter than its frame");
  }
  if (bytes.size() > Limits::kMaxStateFileBytes) {
    return Error::make(ErrorCode::oversized, "the state image exceeds the store bound");
  }
  const std::string magic = magic_bytes();
  if (bytes.substr(kOffsetMagic, 8) != std::string_view(magic)) {
    return Error::make(ErrorCode::corruption, "the state image magic is not recognised");
  }

  std::uint32_t format = 0;
  std::uint32_t model = 0;
  std::uint32_t endian = 0;
  std::uint32_t coordinate = 0;
  std::uint32_t flags = 0;
  std::uint32_t header_crc = 0;
  std::uint32_t identity_length = 0;
  std::uint32_t reserved = 0;
  std::uint64_t revision = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t attempt = 0;
  std::uint64_t payload_length = 0;
  if (!read_u32(bytes, kOffsetFormat, format) || !read_u32(bytes, kOffsetModel, model) ||
      !read_u32(bytes, kOffsetEndian, endian) ||
      !read_u32(bytes, kOffsetCoordinate, coordinate) ||
      !read_u32(bytes, kOffsetFlags, flags) || !read_u32(bytes, kOffsetHeaderCrc, header_crc) ||
      !read_u32(bytes, kOffsetIdentityLength, identity_length) ||
      !read_u32(bytes, kOffsetReserved, reserved) ||
      !read_u64(bytes, kOffsetRevision, revision) ||
      !read_u64(bytes, kOffsetIncarnation, incarnation) ||
      !read_u64(bytes, kOffsetAttempt, attempt) ||
      !read_u64(bytes, kOffsetPayloadLength, payload_length)) {
    return Error::make(ErrorCode::truncated_input, "the state image header is incomplete");
  }

  const std::uint32_t computed_crc =
      crc32(reinterpret_cast<const std::uint8_t*>(bytes.data()), kOffsetHeaderCrc);
  if (computed_crc != header_crc) {
    // A byte-swapped writer is the one corruption worth naming precisely,
    // because it points at a real interoperability fault rather than damage.
    if (endian == 0x04030201u) {
      return Error::make(ErrorCode::wrong_endian,
                         "the state image was written by a different byte order");
    }
    return Error::make(ErrorCode::corruption, "the state image header check value does not match");
  }
  if (endian != kEndianTag) {
    return Error::make(ErrorCode::wrong_endian,
                       "the state image was written by a different byte order");
  }
  if (format < Limits::kMinFormatVersion || format > Limits::kMaxFormatVersion) {
    return Error::make(ErrorCode::incompatible_version,
                       "state format version " + std::to_string(format) +
                           " is not implemented by this build");
  }
  if (model != kModelRevision) {
    return Error::make(ErrorCode::incompatible_version,
                       "state model revision " + std::to_string(model) +
                           " is not implemented by this build");
  }
  if (coordinate != kMountSlotsPerRackUnit) {
    return Error::make(ErrorCode::unsupported_coordinate_model,
                       "the state image was written under a different rack coordinate model");
  }
  if (flags != 0) {
    return Error::make(ErrorCode::incompatible_version,
                       "the state image declares format flags this build does not implement");
  }
  if (reserved != 0) {
    return Error::make(ErrorCode::incompatible_version,
                       "the state image declares header bits this build does not implement");
  }
  // Bound the declared identity before it is used to locate anything, so a
  // hostile length cannot move an offset out of the buffer.
  if (identity_length == 0 || identity_length > kMaxIdentityBlockBytes) {
    return Error::make(ErrorCode::corruption,
                       "the declared store identity length is outside the representable range");
  }
  if (payload_length > Limits::kMaxPayloadBytes) {
    return Error::make(ErrorCode::oversized, "the declared payload exceeds the store bound");
  }
  const std::size_t identity_at = identity_offset();
  const std::size_t created_at_at = created_at_offset(identity_length);
  const std::size_t payload_at = payload_offset(identity_length);
  const std::uint64_t expected =
      static_cast<std::uint64_t>(payload_at) + payload_length + kStoreTrailerBytes;
  if (static_cast<std::uint64_t>(bytes.size()) != expected) {
    return Error::make(ErrorCode::truncated_input,
                       "the state image length does not match its declared payload");
  }
  const std::string_view identity_text =
      bytes.substr(identity_at, static_cast<std::size_t>(identity_length));
  if (!is_valid_identifier(identity_text)) {
    return Error::make(ErrorCode::malformed_identity,
                       "the state image carries a store identity that is not a valid identifier");
  }
  std::int64_t created_at = 0;
  if (!read_i64(bytes, created_at_at, created_at)) {
    return Error::make(ErrorCode::truncated_input, "the creation instant is incomplete");
  }
  const std::string_view payload = bytes.substr(payload_at, static_cast<std::size_t>(payload_length));
  const std::string_view trailer = bytes.substr(bytes.size() - kStoreTrailerBytes);
  if (trailer.substr(kDigestBytes, 8) != std::string_view(trailer_magic_bytes())) {
    return Error::make(ErrorCode::corruption, "the state image trailer magic is not recognised");
  }
  std::array<std::uint8_t, kDigestBytes> trailer_bytes{};
  std::memcpy(trailer_bytes.data(), trailer.data(), kDigestBytes);
  const Digest stated{trailer_bytes};

  std::array<std::uint8_t, kDigestBytes> payload_stated{};
  std::memcpy(payload_stated.data(), bytes.data() + kOffsetPayloadDigest, kDigestBytes);
  if (Digest{payload_stated} != sha256(payload)) {
    return Error::make(ErrorCode::integrity_failure,
                       "the payload digest does not match the payload bytes");
  }
  if (digest_in_domain(kDomainStateBinary, bytes.substr(0, bytes.size() - kStoreTrailerBytes)) !=
      stated) {
    return Error::make(ErrorCode::integrity_failure,
                       "the image digest does not match the image bytes");
  }

  Decoded decoded;
  decoded.incarnation = StoreIncarnation{incarnation};
  decoded.revision = RegistryRevision{revision};
  decoded.attempt = AttemptId{decoded.incarnation, attempt};
  decoded.created_at = Timestamp{created_at};
  decoded.digest = stated;
  decoded.content_digest = digest_in_domain(kDomainStateBinary, payload);
  decoded.format_version = format;
  decoded.model_revision = model;
  {
    Result<StoreId> parsed = StoreId::parse(identity_text);
    if (!parsed) return parsed.error();
    decoded.store = std::move(parsed).value();
  }

  std::array<std::uint8_t, kDigestBytes> identity_stated{};
  std::memcpy(identity_stated.data(), bytes.data() + kOffsetIdentityDigest, kDigestBytes);
  if (Digest{identity_stated} != digest_in_domain(kDomainStoreIdentity, decoded.store.str())) {
    return Error::make(ErrorCode::wrong_store,
                       "the store identity in the image does not match its identity digest");
  }
  return decoded;
}

Result<SnapshotPtr> Store::decode_image(std::string_view bytes) {
  Result<Decoded> decoded = inspect_image(bytes);
  if (!decoded) return decoded.error();

  // Decode the payload a second time, through the same strict reader, to build
  // the model. The audit inside Snapshot::build is the same pass every read
  // path runs, so an image that only a lenient reader would accept is refused.
  const std::uint64_t payload_length = [&] {
    std::uint64_t value = 0;
    (void)read_u64(bytes, kOffsetPayloadLength, value);
    return value;
  }();
  const std::uint32_t identity_length = [&bytes] {
    std::uint32_t value = 0;
    (void)read_u32(bytes, kOffsetIdentityLength, value);
    return value;
  }();
  const std::string_view payload =
      bytes.substr(payload_offset(identity_length), static_cast<std::size_t>(payload_length));

  Snapshot::BuildInput input;
  const Status parsed = internal::decode_payload(payload, input);
  if (!parsed) return parsed.error();
  input.store = decoded.value().store;
  input.incarnation = decoded.value().incarnation;
  input.revision = decoded.value().revision;
  input.attempt = decoded.value().attempt;
  input.created_at = decoded.value().created_at;
  return Snapshot::build(std::move(input));
}

Result<std::string> Store::encode_image(const Snapshot& snapshot) { return frame(snapshot); }

// ---------------------------------------------------------------------------
// Open, commit and close
// ---------------------------------------------------------------------------

struct Store::LockState final {
  internal::FileLock lock{};
  bool leased = false;
};

namespace {

std::filesystem::path previous_path_for(const std::filesystem::path& state) {
  return std::filesystem::path(state.string() + std::string(kPreviousSuffix));
}

std::filesystem::path identity_path_for(const std::filesystem::path& state) {
  return std::filesystem::path(state.string() + std::string(kIdentitySuffix));
}

std::filesystem::path lock_path_for(const std::filesystem::path& state) {
  return std::filesystem::path(state.string() + std::string(kLockSuffix));
}

StoreId derive_store_identity(const std::filesystem::path& state) {
  const std::string stem = state.stem().string();
  Result<StoreId> parsed = StoreId::parse(stem);
  if (parsed) return std::move(parsed).value();
  return StoreId{};
}

void run_fault_hook(const StoreOptions& options, WriteStage stage) {
  if (!options.fault_hook) return;
  options.fault_hook(stage);
}

}  // namespace

Store::~Store() { (void)close(); }

Store::Store(Store&& other) noexcept
    : options_(std::move(other.options_)), open_(other.open_), store_(std::move(other.store_)),
      incarnation_(other.incarnation_), revision_(other.revision_), attempt_(other.attempt_),
      digest_(other.digest_), created_at_(other.created_at_),
      snapshot_(std::move(other.snapshot_)), recovery_(std::move(other.recovery_)),
      lock_(std::move(other.lock_)), staging_counter_(other.staging_counter_) {
  other.open_ = false;
  other.staging_counter_ = 0;
}

Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    (void)close();
    options_ = std::move(other.options_);
    open_ = other.open_;
    store_ = std::move(other.store_);
    incarnation_ = other.incarnation_;
    revision_ = other.revision_;
    attempt_ = other.attempt_;
    digest_ = other.digest_;
    created_at_ = other.created_at_;
    snapshot_ = std::move(other.snapshot_);
    recovery_ = std::move(other.recovery_);
    lock_ = std::move(other.lock_);
    staging_counter_ = other.staging_counter_;
    other.open_ = false;
    other.staging_counter_ = 0;
  }
  return *this;
}

Result<Store> Store::open(const StoreOptions& options) {
  Store store;
  store.options_ = options;

  if (options.path.empty()) {
    return Error::make(ErrorCode::path_rejected, "the state path is empty");
  }
  if (options.actor.size() > Limits::kMaxActorBytes) {
    return Error::make(ErrorCode::text_too_long, "the actor name exceeds its bound");
  }
  if (options.source.size() > Limits::kMaxSourceBytes) {
    return Error::make(ErrorCode::text_too_long, "the source name exceeds its bound");
  }

  const std::filesystem::path state_path = options.path;
  const std::filesystem::path previous_path = previous_path_for(state_path);
  const std::filesystem::path identity_path = identity_path_for(state_path);
  const std::filesystem::path lock_path = lock_path_for(state_path);

  Status parent = internal::ensure_parent_directory(state_path);
  if (!parent) return parent.error();
  Status state_ok = internal::validate_state_path(state_path, true);
  if (!state_ok) return state_ok.error();
  Status previous_ok = internal::validate_state_path(previous_path, true);
  if (!previous_ok) return previous_ok.error();
  Status identity_ok = internal::validate_state_path(identity_path, true);
  if (!identity_ok) return identity_ok.error();

  const bool read_write = options.mode == OpenMode::read_write;

  // The writer lock is taken for the whole of open so that two processes cannot
  // race a creation or a recovery. A process that cannot take it may still open
  // the store and read it: it simply holds no authority, so it repairs nothing,
  // creates nothing, and every commit it attempts is refused until the holder
  // goes away. Refusing the open outright would make a store unreadable for as
  // long as any writer happened to be alive, which is not what an operator
  // asking for a report wants.
  Result<internal::FileLock> lock = internal::FileLock::acquire(lock_path, read_write);
  const bool holds_lock = lock.ok();
  if (!holds_lock && lock.code() != ErrorCode::lock_conflict) {
    return lock.error();
  }

  // Retire any staging file this or a dead process left behind. A staging file
  // is never authoritative, so removing one is always safe; it is done only
  // under authority, because it writes to the store directory.
  const std::filesystem::path directory =
      state_path.parent_path().empty() ? std::filesystem::path(".") : state_path.parent_path();
  if (holds_lock) {
    Result<std::vector<std::string>> names = internal::list_directory(directory, 100000);
    if (!names) return names.error();
    const std::string staging_prefix =
        state_path.filename().string() + std::string(kStagingMarker);
    for (const std::string& name : names.value()) {
      if (name.rfind(staging_prefix, 0) == 0) {
        (void)internal::remove_file(directory / name);
        ++store.recovery_.retired_staging_files;
      }
    }
  }

  // ------------------------------------------------------------------ anchor
  std::optional<Anchor> anchor;
  if (internal::file_exists(identity_path)) {
    Result<std::string> text =
        internal::read_file_bounded(identity_path, 4096);
    if (!text) return text.error();
    Result<Anchor> parsed = parse_identity_anchor(text.value());
    if (!parsed) return parsed.error();
    anchor = std::move(parsed).value();
  }

  const bool state_exists = internal::file_exists(state_path);
  const bool previous_exists = internal::file_exists(previous_path);

  if (!anchor.has_value() && state_exists) {
    return Error::make(ErrorCode::wrong_store,
                       "a state file exists but its identity anchor does not; the file is "
                       "refused rather than adopted");
  }

  std::optional<std::string> loaded_bytes;
  if (state_exists) {
    Result<std::string> bytes = internal::read_file_bounded(state_path, Limits::kMaxStateFileBytes);
    if (bytes) {
      loaded_bytes = std::move(bytes).value();
    } else if (!previous_exists) {
      return bytes.error();
    }
  }

  if (loaded_bytes.has_value()) {
    Result<SnapshotPtr> loaded = decode_image(*loaded_bytes);
    if (!loaded) {
      if (!previous_exists) return loaded.error();
      loaded_bytes.reset();
    } else {
      const StoreId& decoded_store = loaded.value()->store();
      if (anchor.has_value() && anchor->store != decoded_store) {
        return Error::make(ErrorCode::wrong_store,
                           "the state file carries a different store identity than its anchor");
      }
      if (anchor.has_value() && anchor->incarnation != loaded.value()->incarnation()) {
        return Error::make(ErrorCode::stale_incarnation,
                           "the state file incarnation does not match its anchor");
      }
      store.store_ = decoded_store;
      store.incarnation_ = loaded.value()->incarnation();
      store.revision_ = loaded.value()->revision();
      store.attempt_ = loaded.value()->attempt();
      store.digest_ = loaded.value()->digest();
      store.created_at_ = loaded.value()->created_at();
      store.snapshot_ = std::move(loaded).value();
      store.recovery_.action = RecoveryAction::loaded_current;
    }
  }

  if (!store.snapshot_ && previous_exists) {
    Result<std::string> bytes = internal::read_file_bounded(previous_path, Limits::kMaxStateFileBytes);
    if (!bytes) return bytes.error();
    Result<SnapshotPtr> loaded = decode_image(bytes.value());
    if (!loaded) return loaded.error();
    const StoreId& decoded_store = loaded.value()->store();
    if (anchor.has_value() && anchor->store != decoded_store) {
      return Error::make(ErrorCode::wrong_store,
                         "the previous state file carries a different store identity");
    }
    store.store_ = decoded_store;
    store.incarnation_ = loaded.value()->incarnation();
    store.revision_ = loaded.value()->revision();
    store.attempt_ = loaded.value()->attempt();
    store.digest_ = loaded.value()->digest();
    store.created_at_ = loaded.value()->created_at();
    store.snapshot_ = std::move(loaded).value();
    store.recovery_.action = RecoveryAction::loaded_previous;
    store.recovery_.previous_was_used = true;
    store.recovery_.explanations.add(ReasonCode::store_recovered, std::string{},
                                     "the current state was refused, so the previous generation "
                                     "was loaded as one whole authoritative state");
    if (read_write && holds_lock) {
      // Restore the previous generation as the current one. This writes the
      // same revision, so it is a restoration, not a new revision.
      const std::string image = frame(*store.snapshot_);
      const std::string staging = staging_name(state_path, ++store.staging_counter_);
      Status written = internal::write_file_flushed(staging, image);
      if (!written) return written.error();
      Status published = internal::atomic_replace(staging, state_path);
      if (!published) {
        (void)internal::remove_file(staging);
        return published.error();
      }
    }
  }

  if (!store.snapshot_) {
    if (!holds_lock) {
      // Creating a store requires authority. Without it the open is refused
      // with the conflict that caused it, rather than pretending that the
      // store could be created.
      return lock.error();
    }
    if (options.create != CreateMode::create_if_missing) {
      return Error::make(ErrorCode::no_authoritative_state,
                         "the store holds no state that this build will accept");
    }
    if (!read_write) {
      return Error::make(ErrorCode::no_authoritative_state,
                         "no state file exists at this path and a read-only open cannot create "
                         "one");
    }
    StoreId identity;
    if (options.store_identity.has_value()) {
      identity = *options.store_identity;
    } else {
      identity = derive_store_identity(state_path);
      if (identity.empty()) {
        return Error::make(
            ErrorCode::malformed_identity,
            "the state file name is not a valid store identity; supply one explicitly");
      }
    }
    if (anchor.has_value() && anchor->store != identity) {
      return Error::make(ErrorCode::wrong_store,
                         "the requested store identity differs from the existing anchor");
    }
    const StoreIncarnation incarnation =
        anchor.has_value() ? StoreIncarnation{anchor->incarnation.value() + 1}
                           : StoreIncarnation{1};
    Snapshot::BuildInput input;
    input.store = identity;
    input.incarnation = incarnation;
    input.revision = RegistryRevision{0};
    input.attempt = AttemptId{incarnation, 0};
    input.created_at = system_utc_now();
    Result<SnapshotPtr> built = Snapshot::build(std::move(input));
    if (!built) return built.error();

    store.store_ = identity;
    store.incarnation_ = incarnation;
    store.revision_ = RegistryRevision{0};
    store.attempt_ = AttemptId{incarnation, 0};
    store.digest_ = built.value()->digest();
    store.created_at_ = built.value()->created_at();
    store.snapshot_ = std::move(built).value();
    store.recovery_.action = RecoveryAction::created;

    const std::string anchor_text =
        identity_anchor_text(store.store_, store.incarnation_, store.created_at_);
    Status written = internal::write_file_flushed(identity_path, anchor_text);
    if (!written) return written.error();
    Result<std::string> reread = internal::read_file_bounded(identity_path, 4096);
    if (!reread) return reread.error();
    Result<Anchor> verified = parse_identity_anchor(reread.value());
    if (!verified) return verified.error();
    if (verified.value().store != store.store_ ||
        verified.value().incarnation != store.incarnation_) {
      return Error::make(ErrorCode::integrity_failure,
                         "the identity anchor did not read back as it was written");
    }

    const std::string staging = staging_name(state_path, ++store.staging_counter_);
    Status wrote = internal::write_file_flushed(staging, frame(*store.snapshot_));
    if (!wrote) return wrote.error();
    Status published = internal::atomic_replace(staging, state_path);
    if (!published) {
      (void)internal::remove_file(staging);
      return published.error();
    }
  }

  if (!holds_lock) {
    // The open succeeded without authority. Every commit will be refused until
    // the lock can be taken, and the store reports that it holds no lease.
    store.open_ = true;
    return store;
  }  if (!read_write) {
    (void)lock.value().release();
  } else {
    store.lock_ = std::make_shared<LockState>();
    store.lock_->lock = std::move(lock).value();
    const std::string holder = std::string(kHeadBanner) + "\nholder pid " +
                               std::to_string(internal::process_id()) + "\nactor " +
                               options.actor + "\nsource " + options.source + "\n";
    (void)store.lock_->lock.write_holder(holder);
    // Authority is released again: an open is not a lease. A caller that wants
    // to hold authority asks for it explicitly with acquire_writer_lease().
    (void)store.lock_->lock.release();
  }

  store.open_ = true;
  return store;
}

Result<AttemptId> Store::commit(const SnapshotPtr& next) {
  if (!open_) {
    return Error::make(ErrorCode::not_open, "the store is not open");
  }
  if (options_.mode != OpenMode::read_write) {
    return Error::make(ErrorCode::read_only_store, "the store was opened read-only");
  }
  if (!next) {
    return Error::make(ErrorCode::invalid_argument, "the next state is null");
  }
  if (next->store() != store_) {
    return Error::make(ErrorCode::wrong_store,
                       "the next state carries a different store identity");
  }
  if (next->incarnation() != incarnation_) {
    return Error::make(ErrorCode::stale_incarnation,
                       "the next state carries a different store incarnation");
  }
  const Checked<std::uint64_t> expected = checked_increment(revision_.value());
  if (!expected || next->revision().value() != expected.value) {
    return Error::make(ErrorCode::stale_revision,
                       "the next state must be exactly one revision ahead of the committed one");
  }
  if (next->digest().is_zero()) {
    return Error::make(ErrorCode::invariant_violation, "the next state carries no digest");
  }

  const std::filesystem::path state_path = options_.path;
  const std::filesystem::path previous_path = previous_path_for(state_path);

  // The lock is held by the lease when there is one, and taken here otherwise.
  std::optional<internal::FileLock> local;
  if (!(lock_ && lock_->lock.held())) {
    Result<internal::FileLock> acquired =
        internal::FileLock::acquire(lock_path_for(state_path), true);
    if (!acquired) return acquired.error();
    local = std::move(acquired).value();
  }
  run_fault_hook(options_, WriteStage::lock_acquired);

  // Fence: the committed revision on disk must still be the one this commit was
  // planned against. A different revision means another writer moved the store
  // and this commit is stale, so it is refused rather than merged.
  if (internal::file_exists(state_path)) {
    Result<std::string> current = internal::read_file_bounded(state_path, Limits::kMaxStateFileBytes);
    if (!current) return current.error();
    Result<Decoded> decoded = inspect_image(current.value());
    if (!decoded) return decoded.error();
    if (decoded.value().revision != revision_) {
      return Error::stale(ErrorCode::stale_revision,
                          "the committed revision moved while this commit was being prepared",
                          store_.str(), revision_.value(), decoded.value().revision.value());
    }
    if (decoded.value().incarnation != incarnation_) {
      return Error::stale(ErrorCode::stale_incarnation,
                          "the store was recreated while this commit was being prepared",
                          store_.str(), incarnation_.value(),
                          decoded.value().incarnation.value());
    }
  }

  const std::string image = frame(*next);
  const std::string staging = staging_name(state_path, ++staging_counter_);
  run_fault_hook(options_, WriteStage::staging_planned);

  Status wrote = internal::write_file_flushed(staging, image);
  if (!wrote) return wrote.error();
  run_fault_hook(options_, WriteStage::staging_written);
  Status flushed = internal::flush_file(staging);
  if (!flushed) return flushed.error();
  run_fault_hook(options_, WriteStage::staging_flushed);

  {
    Result<std::string> back = internal::read_file_bounded(staging, Limits::kMaxStateFileBytes);
    if (!back) return back.error();
    if (back.value() != image) {
      return Error::make(ErrorCode::integrity_failure,
                         "the staging file did not read back as it was written");
    }
    Result<SnapshotPtr> reread = decode_image(back.value());
    if (!reread) return reread.error();
    if (reread.value()->digest() != next->digest()) {
      return Error::make(ErrorCode::integrity_failure,
                         "the staging file decodes to a different state than was planned");
    }
  }
  run_fault_hook(options_, WriteStage::staging_verified);

  if (options_.retain_previous && internal::file_exists(state_path)) {
    Result<std::string> current = internal::read_file_bounded(state_path, Limits::kMaxStateFileBytes);
    if (!current) return current.error();
    Status kept = internal::write_file_flushed(previous_path, current.value());
    if (!kept) return kept.error();
  }
  run_fault_hook(options_, WriteStage::previous_retained);

  run_fault_hook(options_, WriteStage::before_publish);
  Status published = internal::atomic_replace(staging, state_path);
  if (!published) {
    (void)internal::remove_file(staging);
    return published.error();
  }
  run_fault_hook(options_, WriteStage::after_publish);

  Status synced = internal::sync_directory(state_path.parent_path());
  if (!synced) return synced.error();
  run_fault_hook(options_, WriteStage::directory_flushed);

  if (local.has_value()) {
    (void)local->release();
  }
  run_fault_hook(options_, WriteStage::lock_released);

  // Verified effect: read back what is actually on disk. Only now does the
  // store accept that the commit happened.
  Status verified = verify_published(*next);
  if (!verified) return verified.error();
  run_fault_hook(options_, WriteStage::published_verified);

  revision_ = next->revision();
  attempt_ = next->attempt();
  digest_ = next->digest();
  created_at_ = next->created_at();
  snapshot_ = next;

  if (options_.retain_previous) {
    const std::filesystem::path directory = state_path.parent_path().empty()
                                                ? std::filesystem::path(".")
                                                : state_path.parent_path();
    Result<std::vector<std::string>> names = internal::list_directory(directory, 100000);
    if (names) {
      const std::string prefix = state_path.filename().string() + std::string(kStagingMarker);
      for (const std::string& name : names.value()) {
        if (name.rfind(prefix, 0) == 0) {
          (void)internal::remove_file(directory / name);
        }
      }
    }
  }
  return attempt_;
}

Status Store::verify_published(const Snapshot& expected) const {
  Result<std::string> bytes =
      internal::read_file_bounded(options_.path, Limits::kMaxStateFileBytes);
  if (!bytes) return bytes.error();
  Result<Decoded> decoded = inspect_image(bytes.value());
  if (!decoded) return decoded.error();
  if (decoded.value().store != expected.store()) {
    return Status::failure(ErrorCode::wrong_store,
                           "the published state carries a different store identity");
  }
  if (decoded.value().revision != expected.revision()) {
    return Status::failure(ErrorCode::stale_revision,
                           "the published state is not the revision that was committed");
  }
  // The strongest available check: the bytes on disk are byte for byte the
  // frame this snapshot encodes to, so nothing about the visible generation is
  // left unverified.
  if (bytes.value() != frame(expected)) {
    return Status::failure(ErrorCode::integrity_failure,
                           "the published bytes are not the frame the committed state encodes to");
  }
  return Status::success();
}

Status Store::verify_on_disk() const {
  if (!open_) {
    return Status::failure(ErrorCode::not_open, "the store is not open");
  }
  Result<std::string> bytes =
      internal::read_file_bounded(options_.path, Limits::kMaxStateFileBytes);
  if (!bytes) return bytes.error();
  Result<Decoded> decoded = inspect_image(bytes.value());
  if (!decoded) return decoded.error();
  if (decoded.value().store != store_) {
    return Status::failure(ErrorCode::wrong_store,
                           "the published state carries a different store identity");
  }
  if (decoded.value().revision != revision_) {
    return Status::failure(ErrorCode::stale_revision,
                           "the published state is not the revision that was committed");
  }
  if (snapshot_ == nullptr) {
    return Status::failure(ErrorCode::no_authoritative_state,
                           "the store holds no authoritative state to verify against");
  }
  if (bytes.value() != frame(*snapshot_)) {
    return Status::failure(ErrorCode::integrity_failure,
                           "the state file is not the frame this store holds");
  }
  return Status::success();
}

Result<StoreStatus> Store::status() const {
  StoreStatus result;
  result.path = options_.path;
  result.identity_path = identity_path_for(options_.path);
  result.state_exists = internal::file_exists(options_.path);
  result.previous_exists = internal::file_exists(previous_path_for(options_.path));
  result.identity_exists = internal::file_exists(identity_path_for(options_.path));
  result.lock_held_by_this_process = holds_writer_lease();
  if (result.state_exists) {
    Result<std::uint64_t> bytes = internal::path_file_size(options_.path);
    if (bytes) result.state_bytes = bytes.value();
  }
  if (result.previous_exists) {
    Result<std::uint64_t> bytes = internal::path_file_size(previous_path_for(options_.path));
    if (bytes) result.previous_bytes = bytes.value();
  }
  if (!result.state_exists) {
    return Error::make(ErrorCode::no_authoritative_state, "the store holds no state file");
  }
  Result<std::string> bytes =
      internal::read_file_bounded(options_.path, Limits::kMaxStateFileBytes);
  if (!bytes) return bytes.error();
  Result<Decoded> decoded = inspect_image(bytes.value());
  if (!decoded) return decoded.error();
  result.store = decoded.value().store;
  result.incarnation = decoded.value().incarnation;
  result.revision = decoded.value().revision;
  result.attempt = decoded.value().attempt;
  result.digest = decoded.value().content_digest;
  return result;
}

Status Store::acquire_writer_lease() {
  if (!open_) return Status::failure(ErrorCode::not_open, "the store is not open");
  if (options_.mode != OpenMode::read_write) {
    return Status::failure(ErrorCode::read_only_store, "the store was opened read-only");
  }
  if (lock_ && lock_->lock.held() && lock_->leased) return Status::success();
  if (!lock_) lock_ = std::make_shared<LockState>();
  Result<internal::FileLock> acquired =
      internal::FileLock::acquire(lock_path_for(options_.path), true);
  if (!acquired) return acquired.error();
  lock_->lock = std::move(acquired).value();
  lock_->leased = true;
  const std::string holder = std::string(kHeadBanner) + "\nholder pid " +
                             std::to_string(internal::process_id()) + "\nactor " +
                             options_.actor + "\nsource " + options_.source + "\nlease held\n";
  (void)lock_->lock.write_holder(holder);
  return Status::success();
}

Status Store::release_writer_lease() {
  if (!lock_ || !lock_->leased) return Status::success();
  Status released = lock_->lock.release();
  lock_->leased = false;
  return released;
}

bool Store::holds_writer_lease() const noexcept {
  return lock_ && lock_->leased && lock_->lock.held();
}

Status Store::close() {
  if (lock_) {
    if (lock_->lock.held()) {
      const std::string holder = std::string(kHeadBanner) + "\nholder -\nactor -\nsource -\n";
      (void)lock_->lock.write_holder(holder);
    }
    (void)lock_->lock.release();
    lock_.reset();
  }
  open_ = false;
  return Status::success();
}

}  // namespace dccp::space_capacity
