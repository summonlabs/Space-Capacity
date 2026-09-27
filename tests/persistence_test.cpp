// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - persistence: what survives a real close and a real reopen.
//
// Serialization alone is not persistence, so every case below uses real files
// in the current working directory and a real close followed by a real open.
// The file proves that:
//
//   * creating a store writes <name>.spcstate and <name>.spcstate.identity,
//     and a fresh store reports RecoveryAction::created at revision 0;
//   * one commit advances the revision by exactly one, and after a close and a
//     fresh open the digest, the revision, the incarnation, the record counts,
//     the canonical text and the file bytes are all identical;
//   * a whole fixture survives the round trip record family by record family;
//   * exactly one whole generation is ever visible: after every mutation of a
//     sequence the on-disk image decodes and passes the same audit every read
//     path runs;
//   * a retained previous generation is kept, decodes, and is used as one whole
//     authoritative state when the current generation is damaged, is reported
//     as loaded_previous with previous_was_used, and is restored as current;
//   * a swapped state file, a state file with no identity anchor, and a store
//     that must exist but does not are each refused with their own code and are
//     never adopted;
//   * two handles on one path cannot both commit;
//   * verify(), inspect_image() and decode_image() are strict, and truncated,
//     extended, mis-declared or corrupted images are refused;
//   * staging files are retired by an open, a read-only store refuses to write,
//     and an in-memory registry writes nothing at all.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
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
// The offsets are part of the format and are never renumbered. They are
// repeated in this file on purpose: a test that poked a byte through a library
// accessor would stop proving anything the moment the accessor moved.
// ---------------------------------------------------------------------------

constexpr std::size_t kOffsetPayloadLength = 48;
constexpr std::size_t kOffsetHeaderCrc = 60;

// ---------------------------------------------------------------------------
// Small file helpers. Standard library only, no test framework.
// ---------------------------------------------------------------------------

// Removes the whole file set of one state path, so a case never depends on what
// an earlier run of the executable left behind.
void clean_state(const std::filesystem::path& state) {
  std::error_code error;
  const std::string base = state.string();
  std::filesystem::remove(base, error);
  std::filesystem::remove(base + std::string(kPreviousSuffix), error);
  std::filesystem::remove(base + std::string(kIdentitySuffix), error);
  std::filesystem::remove(base + std::string(kLockSuffix), error);
}

bool read_bytes(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  out = std::move(bytes);
  return true;
}

bool write_bytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) return false;
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  return !file.fail();
}

// Every entry of the current working directory, sorted, so two listings can be
// compared exactly.
std::vector<std::string> listing() {
  std::vector<std::string> names;
  std::error_code error;
  std::filesystem::directory_iterator iterator(std::filesystem::path("."), error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    names.push_back(iterator->path().filename().string());
    iterator.increment(error);
  }
  std::sort(names.begin(), names.end());
  return names;
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

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, init and xorout
// 0xFFFFFFFF), implemented here so the test never reaches into the library for
// the check value it is deliberately corrupting.
std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t size) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
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

// Recomputes the header check value over bytes [0, 60) after a header field has
// been edited, so the check the edit is meant to exercise is the one reached.
void patch_header_crc(std::string& image) {
  const std::uint32_t crc =
      crc32_ieee(reinterpret_cast<const std::uint8_t*>(image.data()), kOffsetHeaderCrc);
  put_u32_be(image, kOffsetHeaderCrc, crc);
}

// Flips the low bit of one byte in the middle of the payload, leaving the frame
// length and every header field alone. The payload is located from the image
// itself - the trailer is the last 40 bytes and the declared payload length is
// in the header - so the byte really is a payload byte.
bool flip_payload_byte(std::string& image) {
  if (image.size() < kStoreHeaderBytes + kStoreTrailerBytes) return false;
  const std::uint64_t declared = read_u64_be(image, kOffsetPayloadLength);
  if (declared == 0) return false;
  const std::size_t length = static_cast<std::size_t>(declared);
  if (image.size() < kStoreTrailerBytes + length) return false;
  const std::size_t start = image.size() - kStoreTrailerBytes - length;
  if (start < kStoreHeaderBytes) return false;
  const std::size_t middle = start + length / 2;
  image[middle] = static_cast<char>(static_cast<unsigned char>(image[middle]) ^ 0x01u);
  return true;
}

void note_error(const char* what, const Error& error) {
  std::fprintf(stderr, "note: %s -> %s\n", what, error.to_string().c_str());
}

// ---------------------------------------------------------------------------
// Small model helpers
// ---------------------------------------------------------------------------

SpaceNodeId id_of(const char* text) {
  const Result<SpaceNodeId> parsed = SpaceNodeId::parse(text);
  if (!parsed.ok()) {
    std::fprintf(stderr, "note: the test asked for an invalid identity %s\n", text);
    std::abort();
  }
  return parsed.value();
}

StoreId store_id_of(const char* text) {
  const Result<StoreId> parsed = StoreId::parse(text);
  if (!parsed.ok()) {
    std::fprintf(stderr, "note: the test asked for an invalid store identity %s\n", text);
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

// Creates one flat site, which is all the small cases need: it is the smallest
// mutation that changes the model, so a revision advance is never ambiguous.
CreateNodeRequest site_request(const char* id) {
  return node_request(id_of(id), SpaceNodeKind::site, SpatialClass::outdoor, SpaceNodeId{}, 0,
                      "Site");
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
// refusal so a case can stop instead of cascading failures out of a state that
// was never published.
template <typename Request>
bool apply_ok(SpaceCapacityRegistry& registry, const Request& request) {
  const Result<MutationOutcome> outcome = registry.apply(request);
  SC_CHECK(outcome.ok());
  if (!outcome.ok()) {
    note_error("apply", outcome.error());
    return false;
  }
  return true;
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

// A durable registry with one site already committed at revision 1.
bool open_with_one_commit(const StoreOptions& options, SpaceCapacityRegistry& registry,
                          const char* site) {
  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("open", opened.error());
    return false;
  }
  registry = std::move(opened).value();
  if (!apply_ok(registry, site_request(site))) return false;
  SC_CHECK_EQ(registry.revision().value(), std::uint64_t{1});
  return true;
}

// The one property every generation on disk must have: it decodes, it audits,
// and it is the revision the writer believes is current.
bool visible_generation_is_whole(SpaceCapacityRegistry& registry,
                                 const std::filesystem::path& state) {
  std::string bytes;
  const bool read = read_bytes(state, bytes);
  SC_CHECK(read);
  if (!read) return false;
  const Result<SnapshotPtr> decoded = Store::decode_image(bytes);
  SC_CHECK(decoded.ok());
  if (!decoded.ok()) {
    note_error("decode_image(state file)", decoded.error());
    return false;
  }
  const Status audited = decoded.value()->audit();
  SC_CHECK(audited.ok());
  if (!audited.ok()) note_error("audit(state file)", audited.error());
  SC_CHECK_EQ(decoded.value()->revision().value(), registry.revision().value());
  return decoded.value()->revision() == registry.revision();
}

// ---------------------------------------------------------------------------
// 1. Creation writes the file set and reports itself as a creation
// ---------------------------------------------------------------------------

void case_01_creation() {
  SC_CASE("creating a store writes the state file and its identity anchor");
  const std::filesystem::path state = "persist-01.spcstate";
  const std::filesystem::path identity =
      std::filesystem::path(state.string() + std::string(kIdentitySuffix));
  clean_state(state);

  Result<Store> opened = Store::open(options_for("persist-01.spcstate", "persist-one"));
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("Store::open (create)", opened.error());
    return;
  }
  Store& store = opened.value();

  SC_CHECK(store.is_open());
  SC_CHECK(std::filesystem::exists(state));
  SC_CHECK(std::filesystem::exists(identity));
  SC_CHECK_EQ(store.recovery().action, RecoveryAction::created);
  SC_CHECK_EQ(store.revision().value(), std::uint64_t{0});
  SC_CHECK_EQ(store.incarnation().value(), std::uint64_t{1});
  SC_CHECK(store.store_id() == store_id_of("persist-one"));

  // The created state is a whole generation already, not an empty file.
  std::string bytes;
  const bool read = read_bytes(state, bytes);
  SC_CHECK(read);
  if (read) {
    const Result<Store::Decoded> inspected = Store::inspect_image(bytes);
    SC_CHECK(inspected.ok());
    if (inspected.ok()) {
      SC_CHECK_EQ(inspected.value().revision.value(), std::uint64_t{0});
      SC_CHECK_EQ(inspected.value().format_version, kStoreFormatVersion);
    } else {
      note_error("inspect_image(created state)", inspected.error());
    }
  }
  const Status closed = store.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 2. One commit, one revision, and a byte-identical reopen
// ---------------------------------------------------------------------------

void case_02_commit_advances_by_one() {
  SC_CASE("a commit advances the revision by exactly one and reopens byte-identically");
  const std::filesystem::path state = "persist-02.spcstate";
  clean_state(state);

  std::string before_bytes;
  std::string before_text;
  Digest before_digest;
  std::uint64_t before_revision = 0;
  std::uint64_t before_incarnation = 0;
  std::size_t before_records = 0;

  {
    SpaceCapacityRegistry registry;
    if (!open_with_one_commit(options_for("persist-02.spcstate", "persist-two"), registry,
                              "persist-two-site")) {
      return;
    }
    const SnapshotPtr current = registry.snapshot();
    before_digest = current->digest();
    before_revision = current->revision().value();
    before_incarnation = current->incarnation().value();
    before_records = current->record_count();
    before_text = current->canonical_text();
    SC_CHECK_EQ(before_revision, std::uint64_t{1});
    SC_CHECK_EQ(before_records, std::size_t{1});

    const bool read = read_bytes(state, before_bytes);
    SC_CHECK(read);
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }

  Result<SpaceCapacityRegistry> reopened =
      SpaceCapacityRegistry::open(options_for("persist-02.spcstate", "persist-two"));
  SC_CHECK(reopened.ok());
  if (!reopened.ok()) {
    note_error("reopen", reopened.error());
    return;
  }
  SpaceCapacityRegistry registry = std::move(reopened).value();
  const SnapshotPtr after = registry.snapshot();

  SC_CHECK_EQ(after->digest().hex(), before_digest.hex());
  SC_CHECK_EQ(after->revision().value(), before_revision);
  SC_CHECK_EQ(after->incarnation().value(), before_incarnation);
  SC_CHECK_EQ(after->record_count(), before_records);
  SC_CHECK_EQ(after->canonical_text(), before_text);

  std::string after_bytes;
  const bool read = read_bytes(state, after_bytes);
  SC_CHECK(read);
  SC_CHECK(after_bytes == before_bytes);

  const Status verified = registry.verify();
  SC_CHECK(verified.ok());
  if (!verified.ok()) note_error("verify after reopen", verified.error());
  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 3. A whole fixture survives close and reopen, record for record
// ---------------------------------------------------------------------------

void case_03_fixture_round_trip() {
  SC_CASE("a whole fixture survives close and reopen record for record");
  const std::filesystem::path state = "persist-03.spcstate";
  clean_state(state);
  const StoreOptions options = options_for("persist-03.spcstate", "persist-three");

  // The shared fixture aborts the process when a mutation is refused, which is
  // right for a fixture and wrong for a report, so the same records are applied
  // here with every refusal recorded as a failed check.
  std::string before_text;
  std::size_t before_records = 0;
  std::uint64_t before_revision = 0;
  Digest before_digest;
  {
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    SC_CHECK(opened.ok());
    if (!opened.ok()) {
      note_error("open (facility)", opened.error());
      return;
    }
    SpaceCapacityRegistry registry = std::move(opened).value();
    if (!build_facility_records(registry)) return;
    const SnapshotPtr current = registry.snapshot();
    before_text = current->canonical_text();
    before_records = current->record_count();
    before_revision = current->revision().value();
    before_digest = current->digest();
    SC_CHECK_EQ(current->node_count(), std::size_t{10});
    SC_CHECK_EQ(current->claims().size(), std::size_t{2});
    SC_CHECK_EQ(current->reservations().size(), std::size_t{1});
    SC_CHECK_EQ(current->exclusions().size(), std::size_t{1});
    SC_CHECK_EQ(current->clearances().size(), std::size_t{1});
    SC_CHECK_EQ(current->expansion_zones().size(), std::size_t{1});
    SC_CHECK_EQ(before_records, std::size_t{16});
    SC_CHECK(visible_generation_is_whole(registry, state));
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }

  Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
  SC_CHECK(reopened.ok());
  if (!reopened.ok()) {
    note_error("reopen (fixture)", reopened.error());
    return;
  }
  SpaceCapacityRegistry registry = std::move(reopened).value();
  const SnapshotPtr after = registry.snapshot();

  // Every record family, re-read from the file, is exactly what was committed.
  SC_CHECK_EQ(after->canonical_text(), before_text);
  SC_CHECK_EQ(after->record_count(), before_records);
  SC_CHECK_EQ(after->revision().value(), before_revision);
  SC_CHECK_EQ(after->digest().hex(), before_digest.hex());
  SC_CHECK_EQ(after->node_count(), std::size_t{10});
  SC_CHECK_EQ(after->claims().size(), std::size_t{2});
  SC_CHECK_EQ(after->reservations().size(), std::size_t{1});
  SC_CHECK_EQ(after->exclusions().size(), std::size_t{1});
  SC_CHECK_EQ(after->clearances().size(), std::size_t{1});
  SC_CHECK_EQ(after->expansion_zones().size(), std::size_t{1});

  const sc_fixture::Ids id = sc_fixture::ids();
  SC_CHECK(after->find_node(id.rack1) != nullptr);
  SC_CHECK(after->find_claim(id.planar_claim) != nullptr);
  SC_CHECK(after->find_claim(id.unit_claim) != nullptr);
  SC_CHECK(after->find_reservation(id.reservation) != nullptr);
  SC_CHECK(after->find_exclusion(id.exclusion) != nullptr);
  SC_CHECK(after->find_clearance(id.clearance) != nullptr);
  SC_CHECK(after->find_expansion_zone(id.zone) != nullptr);
  const Status audited = registry.audit();
  SC_CHECK(audited.ok());
  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 4. Exactly one whole generation is ever visible
// ---------------------------------------------------------------------------

void case_04_one_whole_generation() {
  SC_CASE("after every mutation exactly one whole generation is visible on disk");
  const std::filesystem::path state = "persist-04.spcstate";
  clean_state(state);

  Result<SpaceCapacityRegistry> opened =
      SpaceCapacityRegistry::open(options_for("persist-04.spcstate", "persist-four"));
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("open", opened.error());
    return;
  }
  SpaceCapacityRegistry registry = std::move(opened).value();
  SC_CHECK(visible_generation_is_whole(registry, state));

  if (!apply_ok(registry, node_request(id_of("p4-site"), SpaceNodeKind::site,
                                       SpatialClass::outdoor, SpaceNodeId{}, 0, "Site"))) {
    return;
  }
  SC_CHECK(visible_generation_is_whole(registry, state));

  if (!apply_ok(registry, node_request(id_of("p4-building"), SpaceNodeKind::building,
                                       SpatialClass::enclosed, id_of("p4-site"), 1, "Building"))) {
    return;
  }
  SC_CHECK(visible_generation_is_whole(registry, state));

  if (!apply_ok(registry, planar_envelope(id_of("p4-building"), 60000, 40000))) return;
  SC_CHECK(visible_generation_is_whole(registry, state));

  if (!apply_ok(registry, node_request(id_of("p4-hall"), SpaceNodeKind::hall, SpatialClass::floor,
                                       id_of("p4-building"), 2, "Hall"))) {
    return;
  }
  SC_CHECK(visible_generation_is_whole(registry, state));

  if (!apply_ok(registry, placement_request(id_of("p4-hall"),
                                            PlanarRect::make(1000, 1000, 30000, 20000)))) {
    return;
  }
  SC_CHECK(visible_generation_is_whole(registry, state));
  SC_CHECK_EQ(registry.revision().value(), std::uint64_t{5});

  // The previous generation, if retained, is a whole generation too.
  const std::filesystem::path previous =
      std::filesystem::path(state.string() + std::string(kPreviousSuffix));
  if (std::filesystem::exists(previous)) {
    std::string bytes;
    const bool read = read_bytes(previous, bytes);
    SC_CHECK(read);
    if (read) {
      const Result<SnapshotPtr> decoded = Store::decode_image(bytes);
      SC_CHECK(decoded.ok());
      if (decoded.ok()) {
        SC_CHECK(decoded.value()->audit().ok());
        SC_CHECK_EQ(decoded.value()->revision().value(), std::uint64_t{4});
      } else {
        note_error("decode_image(previous)", decoded.error());
      }
    }
  } else {
    SC_CHECK(false && "the previous generation was not retained");
  }

  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 5. A retained previous generation is kept and decodes
// ---------------------------------------------------------------------------

void case_05_retained_previous() {
  SC_CASE("retain_previous keeps a previous generation that decodes");
  const std::filesystem::path state = "persist-05.spcstate";
  const std::filesystem::path previous =
      std::filesystem::path(state.string() + std::string(kPreviousSuffix));
  clean_state(state);

  StoreOptions options = options_for("persist-05.spcstate", "persist-five");
  options.retain_previous = true;

  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("open", opened.error());
    return;
  }
  SpaceCapacityRegistry registry = std::move(opened).value();

  if (!apply_ok(registry, site_request("p5-site-a"))) return;
  SC_CHECK(std::filesystem::exists(previous));
  if (!apply_ok(registry, site_request("p5-site-b"))) return;
  SC_CHECK_EQ(registry.revision().value(), std::uint64_t{2});

  SC_CHECK(std::filesystem::exists(previous));
  std::string bytes;
  const bool read = read_bytes(previous, bytes);
  SC_CHECK(read);
  if (read) {
    const Result<SnapshotPtr> decoded = Store::decode_image(bytes);
    SC_CHECK(decoded.ok());
    if (decoded.ok()) {
      SC_CHECK_EQ(decoded.value()->revision().value(), std::uint64_t{1});
      SC_CHECK_EQ(decoded.value()->incarnation().value(), registry.incarnation().value());
      SC_CHECK(decoded.value()->audit().ok());
      SC_CHECK_EQ(decoded.value()->node_count(), std::size_t{1});
    } else {
      note_error("decode_image(.prev)", decoded.error());
    }
  }
  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 6. Recovery from a corrupt current generation
// ---------------------------------------------------------------------------

void case_06_recovery_from_corrupt_current() {
  SC_CASE("a damaged current generation is recovered from the previous one and restored");
  const std::filesystem::path state = "persist-06.spcstate";
  clean_state(state);

  StoreOptions options = options_for("persist-06.spcstate", "persist-six");
  options.retain_previous = true;

  {
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    SC_CHECK(opened.ok());
    if (!opened.ok()) {
      note_error("open", opened.error());
      return;
    }
    SpaceCapacityRegistry registry = std::move(opened).value();
    if (!apply_ok(registry, site_request("p6-site-a"))) return;
    if (!apply_ok(registry, site_request("p6-site-b"))) return;
    SC_CHECK_EQ(registry.revision().value(), std::uint64_t{2});
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }

  std::string image;
  const bool read = read_bytes(state, image);
  SC_CHECK(read);
  if (!read) return;
  const bool flipped = flip_payload_byte(image);
  SC_CHECK(flipped);
  if (!flipped) return;
  const bool written = write_bytes(state, image);
  SC_CHECK(written);

  Result<Store> recovered = Store::open(options);
  SC_CHECK(recovered.ok());
  if (!recovered.ok()) {
    note_error("open (recovery)", recovered.error());
    return;
  }
  Store& store = recovered.value();
  SC_CHECK_EQ(store.recovery().action, RecoveryAction::loaded_previous);
  SC_CHECK(store.recovery().previous_was_used);
  SC_CHECK_EQ(store.revision().value(), std::uint64_t{1});
  SC_CHECK_EQ(store.store_id().str(), std::string("persist-six"));
  SC_CHECK(store.snapshot() != nullptr);
  if (store.snapshot() != nullptr) {
    const Status audited = store.snapshot()->audit();
    SC_CHECK(audited.ok());
    SC_CHECK_EQ(store.snapshot()->node_count(), std::size_t{1});
  }
  const Status closed = store.close();
  SC_CHECK(closed.ok());

  // The store restored the previous generation as the current one, so a second
  // open finds a healthy current file and says so.
  std::string restored;
  const bool reread = read_bytes(state, restored);
  SC_CHECK(reread);
  if (reread) {
    const Result<SnapshotPtr> decoded = Store::decode_image(restored);
    SC_CHECK(decoded.ok());
    if (decoded.ok()) {
      SC_CHECK_EQ(decoded.value()->revision().value(), std::uint64_t{1});
    } else {
      note_error("decode_image(restored state)", decoded.error());
    }
  }

  Result<Store> second = Store::open(options);
  SC_CHECK(second.ok());
  if (!second.ok()) {
    note_error("open (after restoration)", second.error());
    return;
  }
  Store& reopened = second.value();
  SC_CHECK_EQ(reopened.recovery().action, RecoveryAction::loaded_current);
  SC_CHECK(!reopened.recovery().previous_was_used);
  SC_CHECK_EQ(reopened.revision().value(), std::uint64_t{1});
  const Status closed_again = reopened.close();
  SC_CHECK(closed_again.ok());
}

// ---------------------------------------------------------------------------
// 7. A swapped store is refused, never adopted
// ---------------------------------------------------------------------------

void case_07_swapped_store_is_refused() {
  SC_CASE("a state file swapped for another store's is refused and never adopted");
  const std::filesystem::path state_a = "persist-07a.spcstate";
  const std::filesystem::path state_b = "persist-07b.spcstate";
  const std::filesystem::path anchor_a =
      std::filesystem::path(state_a.string() + std::string(kIdentitySuffix));
  clean_state(state_a);
  clean_state(state_b);

  {
    Result<Store> a = Store::open(options_for("persist-07a.spcstate", "persist-seven-a"));
    SC_CHECK(a.ok());
    if (!a.ok()) {
      note_error("open (store A)", a.error());
      return;
    }
    const Status closed = a.value().close();
    SC_CHECK(closed.ok());
  }
  {
    Result<Store> b = Store::open(options_for("persist-07b.spcstate", "persist-seven-b"));
    SC_CHECK(b.ok());
    if (!b.ok()) {
      note_error("open (store B)", b.error());
      return;
    }
    const Status closed = b.value().close();
    SC_CHECK(closed.ok());
  }

  std::string anchor_text;
  const bool anchor_read = read_bytes(anchor_a, anchor_text);
  SC_CHECK(anchor_read);
  SC_CHECK(anchor_text.find("store persist-seven-a") != std::string::npos);

  std::string b_bytes;
  const bool b_read = read_bytes(state_b, b_bytes);
  SC_CHECK(b_read);
  if (!b_read) return;
  const bool swapped = write_bytes(state_a, b_bytes);
  SC_CHECK(swapped);
  SC_CHECK(std::filesystem::exists(anchor_a));

  Result<SpaceCapacityRegistry> refused =
      SpaceCapacityRegistry::open(options_for("persist-07a.spcstate", "persist-seven-a"));
  SC_CHECK(!refused.ok());
  if (!refused.ok()) {
    SC_CHECK_EQ(refused.code(), ErrorCode::wrong_store);
    note_error("open (swapped store)", refused.error());
  }

  // B was not adopted: A's anchor still names A and a second open refuses the
  // same way, so nothing was rewritten into agreement with the swapped file.
  std::string anchor_after;
  const bool after_read = read_bytes(anchor_a, anchor_after);
  SC_CHECK(after_read);
  SC_CHECK(anchor_after == anchor_text);

  Result<SpaceCapacityRegistry> refused_again =
      SpaceCapacityRegistry::open(options_for("persist-07a.spcstate", "persist-seven-a"));
  SC_CHECK(!refused_again.ok());
  if (!refused_again.ok()) SC_CHECK_EQ(refused_again.code(), ErrorCode::wrong_store);
}

// ---------------------------------------------------------------------------
// 8. A state file with no identity anchor is refused
// ---------------------------------------------------------------------------

void case_08_missing_anchor_is_refused() {
  SC_CASE("a state file with no identity anchor is refused, not adopted");
  const std::filesystem::path state = "persist-08.spcstate";
  const std::filesystem::path anchor =
      std::filesystem::path(state.string() + std::string(kIdentitySuffix));
  clean_state(state);

  {
    Result<SpaceCapacityRegistry> opened =
        SpaceCapacityRegistry::open(options_for("persist-08.spcstate", "persist-eight"));
    SC_CHECK(opened.ok());
    if (!opened.ok()) {
      note_error("open", opened.error());
      return;
    }
    if (!apply_ok(opened.value(), site_request("p8-site"))) return;
    const Status closed = opened.value().close();
    SC_CHECK(closed.ok());
  }

  SC_CHECK(std::filesystem::exists(state));
  SC_CHECK(std::filesystem::exists(anchor));
  std::error_code error;
  const bool removed = std::filesystem::remove(anchor, error);
  SC_CHECK(removed);
  SC_CHECK(!std::filesystem::exists(anchor));

  Result<SpaceCapacityRegistry> refused =
      SpaceCapacityRegistry::open(options_for("persist-08.spcstate", "persist-eight"));
  SC_CHECK(!refused.ok());
  if (!refused.ok()) {
    SC_CHECK_EQ(refused.code(), ErrorCode::wrong_store);
    note_error("open (anchor removed)", refused.error());
  }
}

// ---------------------------------------------------------------------------
// 9. A required store that does not exist is refused
// ---------------------------------------------------------------------------

void case_09_must_exist_with_no_state() {
  SC_CASE("create = must_exist with no state and no anchor is refused");
  const std::filesystem::path state = "persist-09.spcstate";
  const std::filesystem::path anchor =
      std::filesystem::path(state.string() + std::string(kIdentitySuffix));
  clean_state(state);
  SC_CHECK(!std::filesystem::exists(state));
  SC_CHECK(!std::filesystem::exists(anchor));

  StoreOptions options = options_for("persist-09.spcstate", "persist-nine");
  options.create = CreateMode::must_exist;

  Result<SpaceCapacityRegistry> refused = SpaceCapacityRegistry::open(options);
  SC_CHECK(!refused.ok());
  if (!refused.ok()) {
    SC_CHECK_EQ(refused.code(), ErrorCode::no_authoritative_state);
    note_error("open (must_exist, nothing there)", refused.error());
  }
  SC_CHECK(!std::filesystem::exists(state));
  SC_CHECK(!std::filesystem::exists(anchor));
}

// ---------------------------------------------------------------------------
// 10. Two writers on one path: the second commit is stale
// ---------------------------------------------------------------------------

void case_10_two_handles_one_path() {
  SC_CASE("a second handle on the same path cannot commit over the first");
  const std::filesystem::path state = "persist-10.spcstate";
  clean_state(state);
  const StoreOptions options = options_for("persist-10.spcstate", "persist-ten");

  Result<SpaceCapacityRegistry> first = SpaceCapacityRegistry::open(options);
  SC_CHECK(first.ok());
  if (!first.ok()) {
    note_error("open (first handle)", first.error());
    return;
  }
  SpaceCapacityRegistry writer = std::move(first).value();

  Result<SpaceCapacityRegistry> second = SpaceCapacityRegistry::open(options);
  SC_CHECK(second.ok());
  if (!second.ok()) {
    note_error("open (second handle)", second.error());
    return;
  }
  SpaceCapacityRegistry stale = std::move(second).value();
  SC_CHECK_EQ(stale.revision().value(), std::uint64_t{0});

  if (!apply_ok(writer, site_request("p10-site-a"))) return;
  SC_CHECK_EQ(writer.revision().value(), std::uint64_t{1});

  // The first handle moved the file, so the second one's planned revision is
  // no longer the one on disk and the commit is refused rather than merged.
  const Result<MutationOutcome> refused = stale.apply(site_request("p10-site-b"));
  SC_CHECK(!refused.ok());
  if (!refused.ok()) {
    SC_CHECK_EQ(refused.code(), ErrorCode::stale_revision);
    note_error("apply (stale handle)", refused.error());
  }
  SC_CHECK_EQ(stale.revision().value(), std::uint64_t{0});
  SC_CHECK_EQ(writer.revision().value(), std::uint64_t{1});
  SC_CHECK_EQ(writer.snapshot()->node_count(), std::size_t{1});

  const Status closed_stale = stale.close();
  SC_CHECK(closed_stale.ok());
  const Status closed_writer = writer.close();
  SC_CHECK(closed_writer.ok());
}

// ---------------------------------------------------------------------------
// 11. verify() follows the bytes on disk
// ---------------------------------------------------------------------------

void case_11_verify_sees_damage() {
  SC_CASE("verify succeeds on a healthy store and fails after a payload flip");
  const std::filesystem::path state = "persist-11.spcstate";
  clean_state(state);

  SpaceCapacityRegistry registry;
  if (!open_with_one_commit(options_for("persist-11.spcstate", "persist-eleven"), registry,
                            "p11-site")) {
    return;
  }

  const Status healthy = registry.verify();
  SC_CHECK(healthy.ok());
  if (!healthy.ok()) note_error("verify (healthy)", healthy.error());

  std::string image;
  const bool read = read_bytes(state, image);
  SC_CHECK(read);
  if (!read) return;
  const bool flipped = flip_payload_byte(image);
  SC_CHECK(flipped);
  if (!flipped) return;
  const bool written = write_bytes(state, image);
  SC_CHECK(written);

  const Status damaged = registry.verify();
  SC_CHECK(!damaged.ok());
  if (!damaged.ok()) {
    SC_CHECK_EQ(damaged.code(), ErrorCode::integrity_failure);
    note_error("verify (flipped payload byte)", damaged.error());
  }
  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 12. inspect_image and decode_image are at least as strict as the open path
// ---------------------------------------------------------------------------

void case_12_image_checks_are_strict() {
  SC_CASE("inspect_image and decode_image are as strict as the open path");
  clean_state(std::filesystem::path("persist-12.spcstate"));

  SpaceCapacityRegistry registry;
  if (!open_with_one_commit(options_for("persist-12.spcstate", "persist-twelve"), registry,
                            "p12-site")) {
    return;
  }
  const SnapshotPtr current = registry.snapshot();
  const std::string healthy = Store::frame(*current);
  SC_CHECK(healthy.size() > kStoreHeaderBytes + kStoreTrailerBytes);

  const Result<Store::Decoded> inspected = Store::inspect_image(healthy);
  SC_CHECK(inspected.ok());
  if (inspected.ok()) {
    SC_CHECK_EQ(inspected.value().revision.value(), current->revision().value());
    SC_CHECK_EQ(inspected.value().store.str(), current->store().str());
  } else {
    note_error("inspect_image(healthy)", inspected.error());
  }
  const Result<SnapshotPtr> decoded = Store::decode_image(healthy);
  SC_CHECK(decoded.ok());
  if (decoded.ok()) {
    SC_CHECK_EQ(decoded.value()->digest().hex(), current->digest().hex());
    SC_CHECK(decoded.value()->audit().ok());
  } else {
    note_error("decode_image(healthy)", decoded.error());
  }

  std::string flipped = healthy;
  const bool did_flip = flip_payload_byte(flipped);
  SC_CHECK(did_flip);
  if (did_flip) {
    const Result<Store::Decoded> bad_inspect = Store::inspect_image(flipped);
    SC_CHECK(!bad_inspect.ok());
    if (!bad_inspect.ok()) SC_CHECK_EQ(bad_inspect.code(), ErrorCode::integrity_failure);
    const Result<SnapshotPtr> bad_decode = Store::decode_image(flipped);
    SC_CHECK(!bad_decode.ok());
    if (!bad_decode.ok()) SC_CHECK_EQ(bad_decode.code(), ErrorCode::integrity_failure);
  }

  std::string header_damage = healthy;
  header_damage[8] = static_cast<char>(static_cast<unsigned char>(header_damage[8]) ^ 0x40u);
  const Result<Store::Decoded> bad_header = Store::inspect_image(header_damage);
  SC_CHECK(!bad_header.ok());
  if (!bad_header.ok()) {
    SC_CHECK_NE(bad_header.code(), ErrorCode::ok);
    SC_CHECK_EQ(bad_header.code(), ErrorCode::corruption);  // the header CRC fires first
  }
  const Result<SnapshotPtr> bad_header_decode = Store::decode_image(header_damage);
  SC_CHECK(!bad_header_decode.ok());
  if (!bad_header_decode.ok()) SC_CHECK_NE(bad_header_decode.code(), ErrorCode::ok);

  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 13. Truncated, extended and mis-declared images
// ---------------------------------------------------------------------------

void case_13_length_disagreements() {
  SC_CASE("truncated, extended and mis-declared images are refused");
  clean_state(std::filesystem::path("persist-13.spcstate"));

  SpaceCapacityRegistry registry;
  if (!open_with_one_commit(options_for("persist-13.spcstate", "persist-thirteen"), registry,
                            "p13-site")) {
    return;
  }
  const std::string healthy = Store::frame(*registry.snapshot());
  const std::uint64_t payload_length = read_u64_be(healthy, kOffsetPayloadLength);
  SC_CHECK(payload_length > 0);

  const std::string shortened = healthy.substr(0, healthy.size() - 1);
  SC_CHECK_EQ(shortened.size(), healthy.size() - 1);
  const Result<Store::Decoded> short_inspect = Store::inspect_image(shortened);
  SC_CHECK(!short_inspect.ok());
  if (!short_inspect.ok()) SC_CHECK_EQ(short_inspect.code(), ErrorCode::truncated_input);
  const Result<SnapshotPtr> short_decode = Store::decode_image(shortened);
  SC_CHECK(!short_decode.ok());
  if (!short_decode.ok()) SC_CHECK_EQ(short_decode.code(), ErrorCode::truncated_input);

  std::string extended = healthy;
  extended.push_back('\0');
  const Result<Store::Decoded> long_inspect = Store::inspect_image(extended);
  SC_CHECK(!long_inspect.ok());
  if (!long_inspect.ok()) SC_CHECK_EQ(long_inspect.code(), ErrorCode::truncated_input);
  const Result<SnapshotPtr> long_decode = Store::decode_image(extended);
  SC_CHECK(!long_decode.ok());
  if (!long_decode.ok()) SC_CHECK_EQ(long_decode.code(), ErrorCode::truncated_input);

  std::string raised = healthy;
  put_u64_be(raised, kOffsetPayloadLength, payload_length + 1);
  patch_header_crc(raised);
  const Result<Store::Decoded> raised_inspect = Store::inspect_image(raised);
  SC_CHECK(!raised_inspect.ok());
  if (!raised_inspect.ok()) SC_CHECK_EQ(raised_inspect.code(), ErrorCode::truncated_input);
  const Result<SnapshotPtr> raised_decode = Store::decode_image(raised);
  SC_CHECK(!raised_decode.ok());
  if (!raised_decode.ok()) SC_CHECK_EQ(raised_decode.code(), ErrorCode::truncated_input);
  if (!raised_inspect.ok()) note_error("inspect_image(declared length + 1)", raised_inspect.error());

  const Status closed = registry.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 14. Staging files are retired by an open
// ---------------------------------------------------------------------------

void case_14_staging_is_retired() {
  SC_CASE("a staging file left in the directory is retired by an open");
  const std::filesystem::path state = "persist-14.spcstate";
  const std::filesystem::path staging =
      std::filesystem::path(state.string() + std::string(kStagingMarker) + "999-1");
  clean_state(state);
  std::error_code error;
  std::filesystem::remove(staging, error);

  const bool planted = write_bytes(staging, "a staging file that is never authoritative");
  SC_CHECK(planted);
  SC_CHECK(std::filesystem::exists(staging));

  Result<SpaceCapacityRegistry> opened =
      SpaceCapacityRegistry::open(options_for("persist-14.spcstate", "persist-fourteen"));
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("open", opened.error());
    return;
  }
  SC_CHECK(!std::filesystem::exists(staging));
  const Status closed = opened.value().close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 15. A read-only store refuses to commit
// ---------------------------------------------------------------------------

void case_15_read_only_refuses_commit() {
  SC_CASE("a read-only open refuses a commit");
  const std::filesystem::path state = "persist-15.spcstate";
  clean_state(state);

  {
    SpaceCapacityRegistry registry;
    if (!open_with_one_commit(options_for("persist-15.spcstate", "persist-fifteen"), registry,
                              "p15-site")) {
      return;
    }
    const Status closed = registry.close();
    SC_CHECK(closed.ok());
  }

  StoreOptions options = options_for("persist-15.spcstate", "persist-fifteen");
  options.mode = OpenMode::read_only;
  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  SC_CHECK(opened.ok());
  if (!opened.ok()) {
    note_error("open (read only)", opened.error());
    return;
  }
  SpaceCapacityRegistry read_only = std::move(opened).value();
  SC_CHECK_EQ(read_only.revision().value(), std::uint64_t{1});

  const Result<MutationOutcome> refused = read_only.apply(site_request("p15-site-b"));
  SC_CHECK(!refused.ok());
  if (!refused.ok()) {
    SC_CHECK_EQ(refused.code(), ErrorCode::read_only_store);
    note_error("apply (read only)", refused.error());
  }
  SC_CHECK_EQ(read_only.revision().value(), std::uint64_t{1});
  const Status closed = read_only.close();
  SC_CHECK(closed.ok());
}

// ---------------------------------------------------------------------------
// 16. An in-memory registry writes nothing at all
// ---------------------------------------------------------------------------

void case_16_in_memory_writes_nothing() {
  SC_CASE("an in-memory registry is not durable and writes no file");
  const std::vector<std::string> before = listing();

  Result<SpaceCapacityRegistry> created =
      SpaceCapacityRegistry::create_in_memory(*StoreId::parse("persist-memory"));
  SC_CHECK(created.ok());
  if (!created.ok()) {
    note_error("create_in_memory", created.error());
    return;
  }
  SpaceCapacityRegistry memory = std::move(created).value();

  SC_CHECK(!memory.durable());
  SC_CHECK(memory.is_open());
  const Result<StoreStatus> status = memory.store_status();
  SC_CHECK(!status.ok());
  if (!status.ok()) {
    SC_CHECK_EQ(status.code(), ErrorCode::unsupported);
    note_error("store_status (in memory)", status.error());
  }
  const Status verified = memory.verify();
  SC_CHECK(!verified.ok());
  if (!verified.ok()) {
    SC_CHECK_EQ(verified.code(), ErrorCode::unsupported);
    note_error("verify (in memory)", verified.error());
  }

  // It is still a working registry: mutating it must not reach the disk.
  if (!apply_ok(memory, site_request("p16-site"))) return;
  SC_CHECK_EQ(memory.revision().value(), std::uint64_t{1});

  const std::vector<std::string> after = listing();
  SC_CHECK_EQ(after.size(), before.size());
  SC_CHECK(after == before);

  const Status closed = memory.close();
  SC_CHECK(closed.ok());
  SC_CHECK(!memory.durable());
}

}  // namespace

int main() {
  std::error_code error;
  const std::filesystem::path here = std::filesystem::current_path(error);
  std::fprintf(stdout, "working directory: %s\n", here.string().c_str());

  case_01_creation();
  case_02_commit_advances_by_one();
  case_03_fixture_round_trip();
  case_04_one_whole_generation();
  case_05_retained_previous();
  case_06_recovery_from_corrupt_current();
  case_07_swapped_store_is_refused();
  case_08_missing_anchor_is_refused();
  case_09_must_exist_with_no_state();
  case_10_two_handles_one_path();
  case_11_verify_sees_damage();
  case_12_image_checks_are_strict();
  case_13_length_disagreements();
  case_14_staging_is_retired();
  case_15_read_only_refuses_commit();
  case_16_in_memory_writes_nothing();

  return ::sc_test::summary("persistence");
}
