// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - adversarial input handling for the durable state image.
//
// One healthy image is built through the public API, then damaged in every way
// an operator, a failing disk or a hostile writer could damage it. The file
// proves that the decoder:
//
//   * never accepts a truncated image, at any offset, and never crashes on one;
//   * never accepts a single-byte mutation as the image it is not: every
//     mutation is either refused or re-encodes to different bytes;
//   * names a foreign byte order, an unimplemented format or model version, a
//     foreign rack coordinate model, non-zero flags and an oversized or
//     mis-declared payload with their own codes, and does so only after the
//     header check value it recomputes here has been repaired, so the field
//     under test is the field that decides;
//   * refuses a damaged trailer, payload digest, image digest and identity
//     digest with the specific codes those checks own;
//   * refuses an empty, one-byte, header-sized and trailer-short file without
//     crashing;
//   * refuses a NUL written into a persisted identity, or else yields a model
//     the audit still accepts - never a silently altered one;
//   * refuses a hostile declared payload length before anything is allocated;
//   * refuses an extent outside the millimetre bounds, both at the mutation API
//     that owns the rule and in an image that carries the same value.
//
// The CRC-32 is implemented in this file; nothing private in the library is
// reached into, and no timeout, alarm or process limit exists anywhere.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "fixture.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;

// ---------------------------------------------------------------------------
// The durable frame, restated here
//
// These offsets are part of the format and are never renumbered. They are
// repeated in this file on purpose: a test that poked a byte through a library
// accessor would stop proving anything the moment the accessor moved. The
// payload is located from the image itself (the trailer is the last 40 bytes
// and the declared payload length is in the header) so the tests do not depend
// on how many bytes the frame spends before the payload.
// ---------------------------------------------------------------------------

constexpr std::size_t kOffsetFormat = 8;
constexpr std::size_t kOffsetModel = 12;
constexpr std::size_t kOffsetEndian = 16;
constexpr std::size_t kOffsetCoordinate = 20;
constexpr std::size_t kOffsetPayloadLength = 48;
constexpr std::size_t kOffsetFlags = 56;
constexpr std::size_t kOffsetHeaderCrc = 60;
constexpr std::size_t kOffsetPayloadDigest = 64;
constexpr std::size_t kOffsetIdentityDigest = 96;

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

bool read_bytes(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  out = std::move(bytes);
  return true;
}

void put_u32_be(std::string& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<char>((value >> 24) & 0xFFu);
  bytes[offset + 1] = static_cast<char>((value >> 16) & 0xFFu);
  bytes[offset + 2] = static_cast<char>((value >> 8) & 0xFFu);
  bytes[offset + 3] = static_cast<char>(value & 0xFFu);
}

void put_u64_be(std::string& bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<char>((value >> (56 - 8 * index)) & 0xFFu);
  }
}

std::uint64_t read_u64_be(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8) |
            static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]));
  }
  return value;
}

// The eight bytes a signed 64-bit field takes in the canonical encoding.
std::string encoded_i64(std::int64_t value) {
  std::string bytes(8, '\0');
  put_u64_be(bytes, 0, static_cast<std::uint64_t>(value));
  return bytes;
}

void flip_low_bit(std::string& image, std::size_t offset) {
  image[offset] = static_cast<char>(static_cast<unsigned char>(image[offset]) ^ 0x01u);
}

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, init and xorout
// 0xFFFFFFFF), implemented here so the header check value can be repaired after
// a header field is edited deliberately. Reaching the field under test must not
// depend on the library's own check value.
std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t size) {
  static const std::vector<std::uint32_t> table = [] {
    std::vector<std::uint32_t> values(256, 0);
    for (std::uint32_t index = 0; index < 256u; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      values[index] = value;
    }
    return values;
  }();
  std::uint32_t value = 0xFFFF'FFFFu;
  for (std::size_t index = 0; index < size; ++index) {
    value = table[(value ^ data[index]) & 0xFFu] ^ (value >> 8);
  }
  return value ^ 0xFFFF'FFFFu;
}

void patch_header_crc(std::string& image) {
  const std::uint32_t crc =
      crc32_ieee(reinterpret_cast<const std::uint8_t*>(image.data()), kOffsetHeaderCrc);
  put_u32_be(image, kOffsetHeaderCrc, crc);
}

// The payload digest field covers the payload alone.
void patch_payload_digest(std::string& image) {
  const std::uint64_t declared = read_u64_be(image, kOffsetPayloadLength);
  const std::size_t start =
      image.size() - kStoreTrailerBytes - static_cast<std::size_t>(declared);
  const std::string_view payload(image.data() + start, static_cast<std::size_t>(declared));
  const Digest digest = sha256(payload);
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    image[kOffsetPayloadDigest + index] = static_cast<char>(digest.bytes()[index]);
  }
}

// The trailer digest covers every byte before it, the header included, so a
// header field that is not covered by the header CRC is only reached after this
// is repaired too.
void patch_trailer_digest(std::string& image) {
  const std::string_view body(image.data(), image.size() - kStoreTrailerBytes);
  const Digest trailer = digest_in_domain(kDomainStateBinary, body);
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    image[image.size() - kStoreTrailerBytes + index] = static_cast<char>(trailer.bytes()[index]);
  }
}

void note(const char* what, const std::string& detail) {
  std::fprintf(stdout, "note: %s: %s\n", what, detail.c_str());
}

void note_error(const char* what, const Error& error) {
  std::fprintf(stdout, "note: %s: %s\n", what, error.to_string().c_str());
}

// ---------------------------------------------------------------------------
// The healthy image, and the geometry derived from it
// ---------------------------------------------------------------------------

struct Image final {
  std::string bytes{};
  std::size_t payload_start = 0;
  std::size_t payload_length = 0;
};

// Locates the payload from the image itself: the trailer is the last 40 bytes
// and the declared payload length sits at a fixed header offset, so the payload
// starts where the trailer begins minus that length. Everything else in the
// frame - the identity block, the creation stamp - may grow without breaking
// these tests.
bool locate(const std::string& bytes, Image& out) {
  if (bytes.size() < kStoreHeaderBytes + kStoreTrailerBytes) return false;
  const std::uint64_t declared = read_u64_be(bytes, kOffsetPayloadLength);
  if (declared > bytes.size()) return false;
  const std::size_t length = static_cast<std::size_t>(declared);
  if (bytes.size() < kStoreTrailerBytes + length) return false;
  const std::size_t start = bytes.size() - kStoreTrailerBytes - length;
  if (start < kStoreHeaderBytes) return false;
  out.bytes = bytes;
  out.payload_start = start;
  out.payload_length = length;
  return true;
}

// A mutated image is acceptable only when the decoder refuses it, or when what
// it decodes to re-encodes to bytes other than the mutated bytes. An image is
// never accepted silently as the image it is not.
bool mutation_is_refused_or_differs(const std::string& healthy, std::size_t offset) {
  std::string mutated = healthy;
  flip_low_bit(mutated, offset);
  if (mutated == healthy) return false;
  const Result<SnapshotPtr> decoded = Store::decode_image(mutated);
  if (!decoded.ok()) return true;
  return Store::frame(*decoded.value()) != mutated;
}

// Every header edit below is made in a copy, and the check value is repaired
// only where the case says so, so each case reads the code that its own field
// decides and nothing else.
void apply_and_expect(const Image& image, const char* what, ErrorCode expected) {
  const Result<Store::Decoded> inspected = Store::inspect_image(image.bytes);
  SC_CHECK(!inspected.ok());
  if (inspected.ok()) {
    note(what, "the image was accepted");
    return;
  }
  SC_CHECK_EQ(inspected.code(), expected);
  if (inspected.code() != expected) note_error(what, inspected.error());
  const Result<SnapshotPtr> decoded = Store::decode_image(image.bytes);
  SC_CHECK(!decoded.ok());
  if (!decoded.ok() && decoded.code() != expected) note_error(what, decoded.error());
}

// ---------------------------------------------------------------------------
// The model the image is built from
// ---------------------------------------------------------------------------

SpaceNodeId id_of(const char* text) {
  const Result<SpaceNodeId> parsed = SpaceNodeId::parse(text);
  if (!parsed.ok()) {
    std::fprintf(stderr, "note: the test asked for an invalid identity %s\n", text);
    std::abort();
  }
  return parsed.value();
}

StoreOptions options_for(const char* state, const char* identity) {
  StoreOptions options;
  options.path = std::filesystem::path(state);
  options.store_identity = *StoreId::parse(identity);
  return options;
}

CreateNodeRequest node_request(const SpaceNodeId& id, SpaceNodeKind kind,
                               SpatialClass spatial_class, const SpaceNodeId& parent,
                               std::uint32_t depth, const char* label) {
  CreateNodeRequest request;
  request.node = sc_fixture::make_node(id, kind, spatial_class, parent, depth, label);
  return request;
}

SetNodeEnvelopeRequest planar_envelope(const SpaceNodeId& node, std::int64_t width,
                                       std::int64_t depth) {
  SetNodeEnvelopeRequest request;
  request.node = node;
  PlanarEnvelope envelope;
  envelope.declared_area = SquareMillimeters{width * depth};
  envelope.rects = *RectSet::build({PlanarRect::make(0, 0, width, depth)});
  request.planar = envelope;
  return request;
}

SetNodeEnvelopeRequest rack_envelope(const SpaceNodeId& node, std::int32_t height) {
  SetNodeEnvelopeRequest request;
  request.node = node;
  RackEnvelope envelope;
  envelope.height = RackUnits{height};
  request.rack = envelope;
  return request;
}

SetNodePlacementRequest placement_request(const SpaceNodeId& node, PlanarRect rect) {
  SetNodePlacementRequest request;
  request.node = node;
  request.placement.base_rect = rect;
  request.placement.has_base_rect = true;
  return request;
}

// Applies one mutation of any request type and reports it. Returns false on
// refusal so the image is never built from a half-applied model.
template <typename Request>
bool apply_ok(SpaceCapacityRegistry& registry, const Request& request) {
  const Result<MutationOutcome> outcome = registry.apply(request);
  SC_CHECK(outcome.ok());
  if (!outcome.ok()) note_error("apply", outcome.error());
  return outcome.ok();
}

// The whole facility the shared fixture describes: the same identities, the
// same kinds and the same geometry, taken from tests/fixture.hpp. The records
// are applied here rather than through the fixture's own helpers so that a
// refusal is a failed check instead of an abort that would hide every case
// after it, and in the order the library accepts - a node that declares a plane
// inside another plane must already state where it sits, so hall-1 is placed
// before it declares its own plane.
bool build_facility_records(SpaceCapacityRegistry& registry) {
  const sc_fixture::Ids id = sc_fixture::ids();

  if (!apply_ok(registry, node_request(id.site, SpaceNodeKind::site, SpatialClass::outdoor,
                                       SpaceNodeId{}, 0, "Site A"))) {
    return false;
  }
  if (!apply_ok(registry, node_request(id.building, SpaceNodeKind::building,
                                       SpatialClass::enclosed, id.site, 1, "Building 1"))) {
    return false;
  }
  if (!apply_ok(registry, planar_envelope(id.building, sc_fixture::kBuildingWidth,
                                          sc_fixture::kBuildingDepth))) {
    return false;
  }
  if (!apply_ok(registry, node_request(id.hall, SpaceNodeKind::hall, SpatialClass::floor,
                                       id.building, 2, "Hall 1"))) {
    return false;
  }
  if (!apply_ok(registry, placement_request(
                              id.hall, PlanarRect::make(0, 0, sc_fixture::kHallWidth,
                                                        sc_fixture::kHallDepth)))) {
    return false;
  }
  if (!apply_ok(registry,
                planar_envelope(id.hall, sc_fixture::kHallWidth, sc_fixture::kHallDepth))) {
    return false;
  }
  if (!apply_ok(registry, node_request(id.row1, SpaceNodeKind::row, SpatialClass::aisle, id.hall,
                                       3, "Row 1"))) {
    return false;
  }
  if (!apply_ok(registry, node_request(id.row2, SpaceNodeKind::row, SpatialClass::aisle, id.hall,
                                       3, "Row 2"))) {
    return false;
  }

  const SpaceNodeId racks[4] = {id.rack1, id.rack2, id.rack3, id.rack4};
  const SpaceNodeId rows[4] = {id.row1, id.row1, id.row2, id.row2};
  for (int index = 0; index < 4; ++index) {
    if (!apply_ok(registry, node_request(racks[index], SpaceNodeKind::rack, SpatialClass::rack,
                                         rows[index], 4, "Rack"))) {
      return false;
    }
    if (!apply_ok(registry, rack_envelope(racks[index], sc_fixture::kRackHeight))) return false;
    if (!apply_ok(registry, placement_request(
                                racks[index], PlanarRect::make(static_cast<std::int64_t>(index) * 2000,
                                                               0, sc_fixture::kRackWidth,
                                                               sc_fixture::kRackDepth)))) {
      return false;
    }
  }
  {
    SpaceNode band = sc_fixture::make_node(id.band, SpaceNodeKind::rack_unit_band,
                                           SpatialClass::rack, id.rack1, 5, "band");
    band.placement.has_u_span = true;
    band.placement.u_span = RackUnitInterval::of_count(1, 4);
    CreateNodeRequest request;
    request.node = band;
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateClaimRequest request;
    request.claim.id = id.planar_claim;
    request.claim.generation = EntityGeneration{1};
    request.claim.node = id.hall;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::infrastructure;
    request.claim.label = *DisplayLabel::parse("patch panel");
    request.claim.scope.kind = FootprintScopeKind::planar;
    request.claim.scope.rects = *RectSet::build({PlanarRect::make(0, 2000, 1000, 1000)});
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateClaimRequest request;
    request.claim.id = id.unit_claim;
    request.claim.generation = EntityGeneration{1};
    request.claim.node = id.rack1;
    request.claim.state = ClaimState::committed;
    request.claim.occupant = OccupantKind::asset;
    request.claim.scope.kind = FootprintScopeKind::rack_units;
    request.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(10, 6)});
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateReservationRequest request;
    request.reservation.id = id.reservation;
    request.reservation.generation = EntityGeneration{1};
    request.reservation.node = id.rack2;
    request.reservation.state = ReservationState::held;
    const Result<ReservationRef> authority =
        ReservationRef::make("res-0001", 4, UpstreamState::active);
    if (!authority.ok()) return false;
    request.reservation.reservation = authority.value();
    request.reservation.holder_label = *DisplayLabel::parse("tenant b");
    request.reservation.scope.kind = FootprintScopeKind::rack_units;
    request.reservation.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 8)});
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateExclusionRequest request;
    request.region.id = id.exclusion;
    request.region.generation = EntityGeneration{1};
    request.region.node = id.hall;
    request.region.reason = ExclusionReason::thermal;
    request.region.state = ExclusionState::active;
    request.region.blocks = default_mask_for_reason(ExclusionReason::thermal);
    request.region.scope.kind = FootprintScopeKind::planar;
    request.region.scope.rects = *RectSet::build({PlanarRect::make(20000, 10000, 5000, 5000)});
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateClearanceRequest request;
    request.constraint.id = id.clearance;
    request.constraint.generation = EntityGeneration{1};
    request.constraint.node = id.rack3;
    request.constraint.kind = ClearanceKind::front_service;
    request.constraint.has_band = true;
    request.constraint.band = PlanarRect::make(4000, sc_fixture::kRackDepth, sc_fixture::kRackWidth,
                                               900);
    request.constraint.enforceable = true;
    if (!apply_ok(registry, request)) return false;
  }
  {
    CreateExpansionZoneRequest request;
    request.zone.id = id.zone;
    request.zone.generation = EntityGeneration{1};
    request.zone.node = id.hall;
    request.zone.state = ExpansionState::funded;
    request.zone.scope.kind = FootprintScopeKind::planar;
    request.zone.scope.rects = *RectSet::build({PlanarRect::make(3000, 3000, 2000, 2000)});
    if (!apply_ok(registry, request)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 1. Truncation at every offset
// ---------------------------------------------------------------------------

void case_01_truncation_at_every_offset(const Image& image) {
  SC_CASE("no truncation of the image is ever accepted");
  const std::string& healthy = image.bytes;
  // The sweep is the whole image, so it covers at least 200 offsets on any
  // image this fixture can produce; the bound is asserted so a future image
  // that shrank below it could never turn this case into a two-offset test.
  SC_CHECK(healthy.size() >= 200);

  std::size_t accepted = 0;
  std::size_t first_accepted = healthy.size();
  for (std::size_t length = 0; length < healthy.size(); ++length) {
    const std::string_view shorter(healthy.data(), length);
    if (Store::inspect_image(shorter).ok() || Store::decode_image(shorter).ok()) {
      if (accepted == 0) first_accepted = length;
      ++accepted;
    }
  }
  if (accepted != 0) {
    note("truncation", "the first truncation that was accepted was " +
                           std::to_string(first_accepted) + " bytes long");
  }
  SC_CHECK_EQ(accepted, std::size_t{0});

  // The offsets the case must name explicitly.
  const std::size_t named[8] = {0,          1,          2,          135,
                                136,        137,        healthy.size() - 2, healthy.size() - 1};
  for (std::size_t index = 0; index < 8; ++index) {
    const std::string_view shorter(healthy.data(), named[index]);
    SC_CHECK(!Store::inspect_image(shorter).ok());
    SC_CHECK(!Store::decode_image(shorter).ok());
  }

  // A buffer of exactly the frame minimum is still not a whole generation.
  const std::string_view frame_only(healthy.data(), kStoreHeaderBytes + kStoreTrailerBytes);
  const Result<Store::Decoded> inspected = Store::inspect_image(frame_only);
  SC_CHECK(!inspected.ok());
  if (!inspected.ok()) SC_CHECK_EQ(inspected.code(), ErrorCode::truncated_input);
}

// ---------------------------------------------------------------------------
// 2. Single-byte mutation
// ---------------------------------------------------------------------------

void case_02_single_byte_mutation(const Image& image) {
  SC_CASE("no single-byte mutation is accepted as the unchanged image");
  const std::string& healthy = image.bytes;

  std::size_t accepted = 0;
  for (std::size_t offset = 0; offset < 256 && offset < healthy.size(); ++offset) {
    if (!mutation_is_refused_or_differs(healthy, offset)) {
      if (accepted == 0) note("mutation", "the header mutation at offset " +
                                              std::to_string(offset) + " was accepted unchanged");
      ++accepted;
    }
  }
  SC_CHECK_EQ(accepted, std::size_t{0});

  // A fixed seed, printed by the generator, so the sample is replayable exactly.
  sc_test::Rng rng(0x5EED'1234ull);
  std::size_t payload_mutations = 0;
  std::size_t payload_accepted = 0;
  if (image.payload_length > 0) {
    for (std::size_t index = 0; index < 256; ++index) {
      const std::size_t offset =
          image.payload_start + static_cast<std::size_t>(rng.bounded(image.payload_length));
      ++payload_mutations;
      if (!mutation_is_refused_or_differs(healthy, offset)) {
        if (payload_accepted == 0) {
          note("mutation", "the payload mutation at offset " + std::to_string(offset) +
                               " was accepted unchanged");
        }
        ++payload_accepted;
      }
    }
  }
  SC_CHECK_EQ(payload_mutations, std::size_t{256});
  SC_CHECK_EQ(payload_accepted, std::size_t{0});
}

// ---------------------------------------------------------------------------
// 3. Wrong byte order
// ---------------------------------------------------------------------------

void case_03_wrong_endian(const Image& image) {
  SC_CASE("a foreign byte order is named, before and after the header CRC is repaired");
  // The endian tag is overwritten and NOTHING is recomputed. The header check
  // value therefore fails first, and the reader answers that failure by looking
  // at the endian field: a header written by a byte-swapped writer is worth
  // naming precisely, so the code that actually comes back is wrong_endian and
  // not the generic corruption. The assertion below records which one happens.
  Image swapped = image;
  put_u32_be(swapped.bytes, kOffsetEndian, 0x04030201u);
  const Result<Store::Decoded> unpatched = Store::inspect_image(swapped.bytes);
  SC_CHECK(!unpatched.ok());
  if (!unpatched.ok()) {
    SC_CHECK_EQ(unpatched.code(), ErrorCode::wrong_endian);
    note_error("inspect_image(endian swapped, CRC untouched)", unpatched.error());
  }

  // Now the check value is repaired, so the endian comparison itself decides.
  Image repaired = image;
  put_u32_be(repaired.bytes, kOffsetEndian, 0x04030201u);
  patch_header_crc(repaired.bytes);
  apply_and_expect(repaired, "inspect_image(endian swapped, CRC repaired)",
                   ErrorCode::wrong_endian);
}

// ---------------------------------------------------------------------------
// 4. Wrong format version
// ---------------------------------------------------------------------------

void case_04_wrong_format(const Image& image) {
  SC_CASE("an unimplemented format version is refused");
  const std::uint32_t versions[2] = {0u, 99u};
  for (std::size_t index = 0; index < 2; ++index) {
    Image edited = image;
    put_u32_be(edited.bytes, kOffsetFormat, versions[index]);
    patch_header_crc(edited.bytes);
    apply_and_expect(edited, "inspect_image(format version)", ErrorCode::incompatible_version);
  }
}

// ---------------------------------------------------------------------------
// 5. Wrong model revision
// ---------------------------------------------------------------------------

void case_05_wrong_model(const Image& image) {
  SC_CASE("an unimplemented model revision is refused");
  const std::uint32_t revisions[2] = {0u, 99u};
  for (std::size_t index = 0; index < 2; ++index) {
    Image edited = image;
    put_u32_be(edited.bytes, kOffsetModel, revisions[index]);
    patch_header_crc(edited.bytes);
    apply_and_expect(edited, "inspect_image(model revision)", ErrorCode::incompatible_version);
  }
}

// ---------------------------------------------------------------------------
// 6. Wrong coordinate model
// ---------------------------------------------------------------------------

void case_06_wrong_coordinate_model(const Image& image) {
  SC_CASE("a foreign rack coordinate model is refused");
  Image edited = image;
  put_u32_be(edited.bytes, kOffsetCoordinate, 4u);
  patch_header_crc(edited.bytes);
  apply_and_expect(edited, "inspect_image(coordinate model)",
                   ErrorCode::unsupported_coordinate_model);
}

// ---------------------------------------------------------------------------
// 7. Non-zero flags
// ---------------------------------------------------------------------------

void case_07_non_zero_flags(const Image& image) {
  SC_CASE("format flags this build does not implement are refused");
  Image edited = image;
  put_u32_be(edited.bytes, kOffsetFlags, 1u);
  patch_header_crc(edited.bytes);
  apply_and_expect(edited, "inspect_image(flags)", ErrorCode::incompatible_version);
}

// ---------------------------------------------------------------------------
// 8. Oversized declaration
// ---------------------------------------------------------------------------

void case_08_oversized_declaration(const Image& image) {
  SC_CASE("a declared payload above the store bound is refused as oversized");
  Image edited = image;
  put_u64_be(edited.bytes, kOffsetPayloadLength, Limits::kMaxPayloadBytes + 1);
  patch_header_crc(edited.bytes);
  apply_and_expect(edited, "inspect_image(payload length above the bound)", ErrorCode::oversized);
}

// ---------------------------------------------------------------------------
// 9. Length mismatch
// ---------------------------------------------------------------------------

void case_09_length_mismatch(const Image& image) {
  SC_CASE("a declared payload length that disagrees with the bytes is refused");
  Image edited = image;
  put_u64_be(edited.bytes, kOffsetPayloadLength,
             static_cast<std::uint64_t>(image.payload_length) + 1);
  patch_header_crc(edited.bytes);
  apply_and_expect(edited, "inspect_image(declared length one too large)",
                   ErrorCode::truncated_input);
}

// ---------------------------------------------------------------------------
// 10. Bad trailer magic
// ---------------------------------------------------------------------------

void case_10_bad_trailer_magic(const Image& image) {
  SC_CASE("a damaged trailer magic is refused as corruption");
  Image edited = image;
  edited.bytes[edited.bytes.size() - 1] =
      static_cast<char>(static_cast<unsigned char>(edited.bytes.back()) ^ 0x01u);
  apply_and_expect(edited, "inspect_image(last byte flipped)", ErrorCode::corruption);
}

// ---------------------------------------------------------------------------
// 11. Bad payload digest
// ---------------------------------------------------------------------------

void case_11_bad_payload_digest(const Image& image) {
  SC_CASE("a damaged payload digest is refused as an integrity failure");
  Image edited = image;
  flip_low_bit(edited.bytes, kOffsetPayloadDigest);
  patch_header_crc(edited.bytes);
  // The payload digest is compared before the trailer digest is, so the code
  // that actually comes back is integrity_failure rather than corruption: the
  // digest disagrees with the payload bytes it covers. The trailer digest would
  // report the same code for the same edit.
  apply_and_expect(edited, "inspect_image(payload digest byte flipped)",
                   ErrorCode::integrity_failure);
}

// ---------------------------------------------------------------------------
// 12. Bad image digest
// ---------------------------------------------------------------------------

void case_12_bad_image_digest(const Image& image) {
  SC_CASE("a damaged trailer digest is refused as an integrity failure");
  Image edited = image;
  flip_low_bit(edited.bytes, edited.bytes.size() - kStoreTrailerBytes);
  apply_and_expect(edited, "inspect_image(trailer digest byte flipped)",
                   ErrorCode::integrity_failure);
}

// ---------------------------------------------------------------------------
// 13. Bad identity digest
// ---------------------------------------------------------------------------

void case_13_bad_identity_digest(const Image& image) {
  SC_CASE("a damaged identity digest is refused as a foreign store");
  // The header CRC covers only the first 60 bytes, so repairing it is not
  // enough on its own: the identity digest sits at offset 96 and the trailer
  // digest covers the whole header. Both repairs are made here, so the identity
  // comparison is genuinely the check that decides.
  Image digest_only = image;
  flip_low_bit(digest_only.bytes, kOffsetIdentityDigest);
  patch_header_crc(digest_only.bytes);
  patch_trailer_digest(digest_only.bytes);
  apply_and_expect(digest_only, "inspect_image(identity digest byte flipped)",
                   ErrorCode::wrong_store);

  // Without the trailer repair the image digest refuses the edit first, which
  // is the stricter outcome and is recorded as such.
  Image unrepaired = image;
  flip_low_bit(unrepaired.bytes, kOffsetIdentityDigest);
  patch_header_crc(unrepaired.bytes);
  const Result<Store::Decoded> inspected = Store::inspect_image(unrepaired.bytes);
  SC_CHECK(!inspected.ok());
  if (!inspected.ok()) {
    SC_CHECK_EQ(inspected.code(), ErrorCode::integrity_failure);
    note_error("inspect_image(identity digest flipped, trailer untouched)", inspected.error());
  }
}

// ---------------------------------------------------------------------------
// 14. Degenerate files
// ---------------------------------------------------------------------------

void case_14_degenerate_files(const Image& image) {
  SC_CASE("empty, one-byte, header-sized and trailer-short files are refused");
  const std::size_t lengths[4] = {0, 1, kStoreHeaderBytes,
                                  kStoreHeaderBytes + kStoreTrailerBytes - 1};
  for (std::size_t index = 0; index < 4; ++index) {
    const std::string_view stub(image.bytes.data(), lengths[index]);
    const Result<Store::Decoded> inspected = Store::inspect_image(stub);
    SC_CHECK(!inspected.ok());
    if (!inspected.ok()) SC_CHECK_EQ(inspected.code(), ErrorCode::truncated_input);
    const Result<SnapshotPtr> decoded = Store::decode_image(stub);
    SC_CHECK(!decoded.ok());
  }
}

// ---------------------------------------------------------------------------
// 15. A NUL written into a persisted text field
// ---------------------------------------------------------------------------

void case_15_nul_in_text_field(const Image& image) {
  SC_CASE("a NUL written into a persisted identity is refused or stays auditable");
  const std::string_view payload(image.bytes.data() + image.payload_start, image.payload_length);
  const std::size_t found = payload.find("site-a");
  SC_CHECK(found != std::string_view::npos);
  if (found == std::string_view::npos) {
    note("nul mutation", "the fixture identity was not found in the payload");
    return;
  }
  const std::size_t absolute = image.payload_start + found;
  SC_CHECK(absolute >= image.payload_start);
  SC_CHECK(absolute < image.payload_start + image.payload_length);

  std::string mutated = image.bytes;
  mutated[absolute] = '\0';
  // The payload digest and the trailer digest are repaired, so the integrity
  // checks pass and the text decoder is the thing that decides. Without that
  // repair this case would only re-prove that a changed payload fails its
  // digest, which is already proved above.
  patch_payload_digest(mutated);
  patch_trailer_digest(mutated);

  const Result<SnapshotPtr> decoded = Store::decode_image(mutated);
  bool acceptable = false;
  if (!decoded.ok()) {
    acceptable = true;
    note("nul mutation", std::string("the decode refused it with ") +
                             std::string(error_code_name(decoded.code())));
    SC_CHECK_EQ(decoded.code(), ErrorCode::malformed_identity);
  } else {
    const Status audited = decoded.value()->audit();
    acceptable = audited.ok();
    note("nul mutation", std::string("the decode accepted it; the audit ") +
                             (audited.ok() ? "accepted it too" : "refused it"));
  }
  SC_CHECK(acceptable);
}

// ---------------------------------------------------------------------------
// 16. A hostile declared length
// ---------------------------------------------------------------------------

void case_16_hostile_declared_length(const Image& image) {
  SC_CASE("a hostile declared payload length is refused before anything is allocated");
  Image edited = image;
  // The bound check on the declared length runs immediately after the header
  // fields are read and before any buffer is sized from it, so the answer comes
  // back without the process ever trying to reach the declared size.
  put_u64_be(edited.bytes, kOffsetPayloadLength, 0xFFFF'FFFF'FFFF'FFFFull);
  patch_header_crc(edited.bytes);
  apply_and_expect(edited, "inspect_image(huge declared payload length)", ErrorCode::oversized);
}

// ---------------------------------------------------------------------------
// 17. Extents out of range
// ---------------------------------------------------------------------------

void case_17_extent_out_of_range(const Image& image) {
  SC_CASE("an extent outside the millimetre bounds is refused");

  // Through the public API: an out-of-range rectangle is refused by the shape
  // validation every mutation runs, with invalid_extent, before anything is
  // published. This is the authority for the rule; the image below only has to
  // agree with it.
  {
    Result<SpaceCapacityRegistry> created =
        SpaceCapacityRegistry::create_in_memory(*StoreId::parse("corrupt-extent"));
    SC_CHECK(created.ok());
    if (!created.ok()) {
      note_error("create_in_memory", created.error());
      return;
    }
    SpaceCapacityRegistry registry = std::move(created).value();
    const Result<MutationOutcome> site =
        registry.apply(node_request(id_of("u-site"), SpaceNodeKind::site, SpatialClass::outdoor,
                                    SpaceNodeId{}, 0, "Site"));
    SC_CHECK(site.ok());
    const Result<MutationOutcome> rack =
        registry.apply(node_request(id_of("u-rack"), SpaceNodeKind::rack, SpatialClass::rack,
                                    id_of("u-site"), 1, "Rack"));
    SC_CHECK(rack.ok());
    if (!site.ok() || !rack.ok()) return;

    SetNodePlacementRequest request;
    request.node = id_of("u-rack");
    request.placement.base_rect =
        PlanarRect::make(Limits::kMaxMillimeters + 1, 0, 600, 1200);
    request.placement.has_base_rect = true;
    const Result<MutationOutcome> refused = registry.apply(request);
    SC_CHECK(!refused.ok());
    if (!refused.ok()) {
      SC_CHECK_EQ(refused.code(), ErrorCode::invalid_extent);
      note_error("apply(placement outside the millimetre bounds)", refused.error());
    }
    SC_CHECK_EQ(registry.revision().value(), std::uint64_t{2});
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }

  // The same value reached through the file instead of the API: a rectangle
  // coordinate inside the payload is raised past the bound, and both digests are
  // repaired so the model audit is what refuses it.
  {
    const std::string needle = encoded_i64(60000);
    const std::size_t found = image.bytes.find(needle, image.payload_start);
    SC_CHECK(found != std::string::npos);
    if (found == std::string::npos) {
      note("extent in the payload", "the building plane rectangle was not found");
      return;
    }
    SC_CHECK(found + 8 <= image.payload_start + image.payload_length);
    std::string mutated = image.bytes;
    put_u64_be(mutated, found, static_cast<std::uint64_t>(Limits::kMaxMillimeters + 1));
    patch_payload_digest(mutated);
    patch_trailer_digest(mutated);

    const Result<Store::Decoded> inspected = Store::inspect_image(mutated);
    SC_CHECK(inspected.ok());  // the frame is intact; only the model is wrong
    const Result<SnapshotPtr> decoded = Store::decode_image(mutated);
    SC_CHECK(!decoded.ok());
    if (decoded.ok()) {
      note("extent in the payload", "the out-of-range rectangle was accepted");
    } else {
      SC_CHECK_EQ(decoded.code(), ErrorCode::invalid_extent);
      note_error("decode_image(rectangle past the millimetre bound)", decoded.error());
    }
  }
}

}  // namespace

int main() {
  std::error_code error;
  const std::filesystem::path here = std::filesystem::current_path(error);
  std::fprintf(stdout, "working directory: %s\n", here.string().c_str());

  const char* const state = "corrupt-fixture.spcstate";
  const StoreOptions options = options_for(state, "corrupt-store");

  Image image;
  {
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    SC_CHECK(opened.ok());
    if (!opened.ok()) {
      note_error("open (facility)", opened.error());
      return ::sc_test::summary("corruption");
    }
    SpaceCapacityRegistry registry = std::move(opened).value();
    if (!build_facility_records(registry)) {
      const Status closed = registry.close();
      SC_CHECK(closed.ok());
      return ::sc_test::summary("corruption");
    }
    const std::string framed = Store::frame(*registry.snapshot());
    SC_CHECK(locate(framed, image));
    std::string on_disk;
    const bool read = read_bytes(std::filesystem::path(state), on_disk);
    SC_CHECK(read);
    // The image the corruption cases damage is byte for byte the image the
    // store itself published, so no case tests anything the writer would not
    // have written.
    SC_CHECK(on_disk == framed);
    if (!locate(on_disk, image)) {
      note("image", "the committed state file could not be located");
      const Status closed = registry.close();
      SC_CHECK(closed.ok());
      return ::sc_test::summary("corruption");
    }
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }
  SC_CHECK(image.payload_length > 0);

  case_01_truncation_at_every_offset(image);
  case_02_single_byte_mutation(image);
  case_03_wrong_endian(image);
  case_04_wrong_format(image);
  case_05_wrong_model(image);
  case_06_wrong_coordinate_model(image);
  case_07_non_zero_flags(image);
  case_08_oversized_declaration(image);
  case_09_length_mismatch(image);
  case_10_bad_trailer_magic(image);
  case_11_bad_payload_digest(image);
  case_12_bad_image_digest(image);
  case_13_bad_identity_digest(image);
  case_14_degenerate_files(image);
  case_15_nul_in_text_field(image);
  case_16_hostile_declared_length(image);
  case_17_extent_out_of_range(image);

  return ::sc_test::summary("corruption");
}
