// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Canonical binary codec and canonical text rendering for a state image.
//
// ---------------------------------------------------------------------------
// Document shape
// ---------------------------------------------------------------------------
//
// A payload is one flat sequence of fields:
//
//     varint(tag) varint(payload length) <payload bytes>
//
// The document is consumed completely: a byte left over is refused, never
// ignored. The store frames the payload, so no leading length is written here.
//
// A varint is unsigned LEB128 with a maximum of ten bytes. Only the minimal
// encoding of a value is accepted, so a trailing 0x80 group -- a group that
// contributes nothing -- is refused and every value has exactly one spelling.
// Integers are fixed width and big-endian: one byte for a boolean or an enum,
// four for a u32 or an i32, eight for a u64 or an i64. No integer is ever
// written as a varint, so the width of a field is part of its identity.
//
// Text is varint(length) followed by the bytes. A collection is varint(count)
// followed by the elements. A rectangle (four i64) and a rack unit interval
// (two i32) are fixed width, so a count alone delimits them; every other
// element carries its own varint(length) prefix and is itself a field
// document, which is what makes each nested record bounded by its container.
//
// ---------------------------------------------------------------------------
// Tag table (stable; never renumber a tag)
// ---------------------------------------------------------------------------
//
//   top-level families
//     0x01 nodes          0x02 claims          0x03 reservations
//     0x04 exclusions     0x05 clearances      0x06 expansion zones
//
//   record fields (base 0x10)
//     0x10 id                 0x11 generation        0x12 kind
//     0x13 spatial class      0x14 lifecycle         0x15 label
//     0x16 note               0x17 parent            0x18 depth
//     0x19 has base rect      0x1A base rect         0x1B has u span
//     0x1C u span             0x1D planar area       0x1E planar rects
//     0x1F rack height        0x20 location          0x21 facility node
//     0x22 rack               0x23 assets            0x24 policies
//     0x25 evidence           0x26 replaced by       0x27 replaces
//     0x28 node               0x29 scope             0x2A state
//     0x2B occupant           0x2C from reservation  0x2D reservation
//     0x2E holder label       0x2F holder assets     0x30 not after
//     0x31 reason             0x32 blocks            0x33 subjects
//     0x34 has band           0x35 band              0x36 has units
//     0x37 required units     0x38 enforceable       0x39 target ready by
//
//   reference fields (base 0x40): 0x40 registry 0x41 id 0x42 generation
//     0x43 source revision 0x44 state 0x45 successor
//   evidence fields (base 0x50): 0x50 source 0x51 id 0x52 generation
//     0x53 observed incarnation 0x54 observed sequence
//   scope fields (base 0x60): 0x60 kind 0x61 rects 0x62 units
//
// A tag names one field in every family that uses it: 0x2A is one byte of
// state in a claim, a reservation, an exclusion and an expansion zone, and no
// family gives one tag two meanings. Every field of every family is written,
// including an optional field, which carries an explicit presence byte, so a
// record has one encoding and a missing field is always a malformed document.
//
// ---------------------------------------------------------------------------
// Canonical order
// ---------------------------------------------------------------------------
//
// Fields are written in the order the record families declare them. The
// decoder accepts the fields of one record in any order but refuses an unknown
// tag, a repeated tag and a missing tag, so a byte string that is not what the
// encoder writes is refused rather than repaired. Records are written in the
// order the caller supplies them: canonical record order, and the canonical
// order inside every set, is established by Snapshot::build and not here.
//
// ---------------------------------------------------------------------------
// Error mapping
// ---------------------------------------------------------------------------
//
//   non-minimal varint, trailing bytes, wrong field width, unknown tag,
//   missing tag, an unreadable tail                -> corruption
//   a varint longer than ten bytes, a declared length or element count that
//   exceeds the remaining input, a text value over its own bound
//                                                  -> oversized
//   an element count over its Limits::kMax* bound  -> limit_exceeded
//   a repeated tag                                 -> duplicate_identity
//   an identity that is not one                    -> malformed_identity
//   free text that is not UTF-8                    -> invalid_utf8
//   an enum value outside its known range          -> unknown_enum_token
//
// Decoding writes into local temporaries and moves them into the caller's
// model only after the whole payload has been accepted, so a failed decode
// never leaves a half-populated model behind.

#include "codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/text.hpp"
#include "dccp/space_capacity/units.hpp"

namespace dccp::space_capacity::internal {
namespace {

// ---------------------------------------------------------------------------
// Format constants
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxVarintBytes = 10;
constexpr std::size_t kRectBytes = 32;      // four i64
constexpr std::size_t kIntervalBytes = 8;   // two i32

// Top-level family tags.
constexpr std::uint64_t kTagNodes = 0x01;
constexpr std::uint64_t kTagClaims = 0x02;
constexpr std::uint64_t kTagReservations = 0x03;
constexpr std::uint64_t kTagExclusions = 0x04;
constexpr std::uint64_t kTagClearances = 0x05;
constexpr std::uint64_t kTagExpansionZones = 0x06;

// Record field tags.
constexpr std::uint64_t kRecordTagBase = 0x10;
constexpr std::uint64_t kTagId = 0x10;
constexpr std::uint64_t kTagGeneration = 0x11;
constexpr std::uint64_t kTagKind = 0x12;
constexpr std::uint64_t kTagSpatialClass = 0x13;
constexpr std::uint64_t kTagLifecycle = 0x14;
constexpr std::uint64_t kTagLabel = 0x15;
constexpr std::uint64_t kTagNote = 0x16;
constexpr std::uint64_t kTagParent = 0x17;
constexpr std::uint64_t kTagDepth = 0x18;
constexpr std::uint64_t kTagHasBaseRect = 0x19;
constexpr std::uint64_t kTagBaseRect = 0x1A;
constexpr std::uint64_t kTagHasUSpan = 0x1B;
constexpr std::uint64_t kTagUSpan = 0x1C;
constexpr std::uint64_t kTagPlanarArea = 0x1D;
constexpr std::uint64_t kTagPlanarRects = 0x1E;
constexpr std::uint64_t kTagRackHeight = 0x1F;
constexpr std::uint64_t kTagLocation = 0x20;
constexpr std::uint64_t kTagFacilityNode = 0x21;
constexpr std::uint64_t kTagRack = 0x22;
constexpr std::uint64_t kTagAssets = 0x23;
constexpr std::uint64_t kTagPolicies = 0x24;
constexpr std::uint64_t kTagEvidence = 0x25;
constexpr std::uint64_t kTagReplacedBy = 0x26;
constexpr std::uint64_t kTagReplaces = 0x27;
constexpr std::uint64_t kTagNode = 0x28;
constexpr std::uint64_t kTagScope = 0x29;
constexpr std::uint64_t kTagState = 0x2A;
constexpr std::uint64_t kTagOccupant = 0x2B;
constexpr std::uint64_t kTagFromReservation = 0x2C;
constexpr std::uint64_t kTagReservation = 0x2D;
constexpr std::uint64_t kTagHolderLabel = 0x2E;
constexpr std::uint64_t kTagHolderAssets = 0x2F;
constexpr std::uint64_t kTagNotAfter = 0x30;
constexpr std::uint64_t kTagReason = 0x31;
constexpr std::uint64_t kTagBlocks = 0x32;
constexpr std::uint64_t kTagSubjects = 0x33;
constexpr std::uint64_t kTagHasBand = 0x34;
constexpr std::uint64_t kTagBand = 0x35;
constexpr std::uint64_t kTagHasUnits = 0x36;
constexpr std::uint64_t kTagRequiredUnits = 0x37;
constexpr std::uint64_t kTagEnforceable = 0x38;
constexpr std::uint64_t kTagTargetReadyBy = 0x39;

// Reference field tags.
constexpr std::uint64_t kRefTagBase = 0x40;
constexpr std::uint64_t kTagRefRegistry = 0x40;
constexpr std::uint64_t kTagRefId = 0x41;
constexpr std::uint64_t kTagRefGeneration = 0x42;
constexpr std::uint64_t kTagRefSourceRevision = 0x43;
constexpr std::uint64_t kTagRefState = 0x44;
constexpr std::uint64_t kTagRefSuccessor = 0x45;

// Evidence reference field tags.
constexpr std::uint64_t kEvidenceTagBase = 0x50;
constexpr std::uint64_t kTagEvidenceSource = 0x50;
constexpr std::uint64_t kTagEvidenceId = 0x51;
constexpr std::uint64_t kTagEvidenceGeneration = 0x52;
constexpr std::uint64_t kTagEvidenceIncarnation = 0x53;
constexpr std::uint64_t kTagEvidenceSequence = 0x54;

// Footprint scope field tags.
constexpr std::uint64_t kScopeTagBase = 0x60;
constexpr std::uint64_t kTagScopeKind = 0x60;
constexpr std::uint64_t kTagScopeRects = 0x61;
constexpr std::uint64_t kTagScopeUnits = 0x62;

// One bit per field of one family, used to detect a repeated tag and a missing
// tag. A family never uses more than 64 tags.
constexpr std::uint64_t field_bit(std::uint64_t tag, std::uint64_t base) noexcept {
  return std::uint64_t{1} << (tag - base);
}

constexpr std::uint64_t kNodeFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagKind, kRecordTagBase) | field_bit(kTagSpatialClass, kRecordTagBase) |
    field_bit(kTagLifecycle, kRecordTagBase) | field_bit(kTagLabel, kRecordTagBase) |
    field_bit(kTagNote, kRecordTagBase) | field_bit(kTagParent, kRecordTagBase) |
    field_bit(kTagDepth, kRecordTagBase) | field_bit(kTagHasBaseRect, kRecordTagBase) |
    field_bit(kTagBaseRect, kRecordTagBase) | field_bit(kTagHasUSpan, kRecordTagBase) |
    field_bit(kTagUSpan, kRecordTagBase) | field_bit(kTagPlanarArea, kRecordTagBase) |
    field_bit(kTagPlanarRects, kRecordTagBase) | field_bit(kTagRackHeight, kRecordTagBase) |
    field_bit(kTagLocation, kRecordTagBase) | field_bit(kTagFacilityNode, kRecordTagBase) |
    field_bit(kTagRack, kRecordTagBase) | field_bit(kTagAssets, kRecordTagBase) |
    field_bit(kTagPolicies, kRecordTagBase) | field_bit(kTagEvidence, kRecordTagBase) |
    field_bit(kTagReplacedBy, kRecordTagBase) | field_bit(kTagReplaces, kRecordTagBase);

constexpr std::uint64_t kClaimFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagNode, kRecordTagBase) | field_bit(kTagScope, kRecordTagBase) |
    field_bit(kTagState, kRecordTagBase) | field_bit(kTagOccupant, kRecordTagBase) |
    field_bit(kTagLabel, kRecordTagBase) | field_bit(kTagNote, kRecordTagBase) |
    field_bit(kTagRack, kRecordTagBase) | field_bit(kTagAssets, kRecordTagBase) |
    field_bit(kTagPolicies, kRecordTagBase) | field_bit(kTagEvidence, kRecordTagBase) |
    field_bit(kTagFromReservation, kRecordTagBase);

constexpr std::uint64_t kReservationFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagNode, kRecordTagBase) | field_bit(kTagScope, kRecordTagBase) |
    field_bit(kTagState, kRecordTagBase) | field_bit(kTagReservation, kRecordTagBase) |
    field_bit(kTagHolderLabel, kRecordTagBase) | field_bit(kTagHolderAssets, kRecordTagBase) |
    field_bit(kTagPolicies, kRecordTagBase) | field_bit(kTagEvidence, kRecordTagBase) |
    field_bit(kTagNotAfter, kRecordTagBase);

constexpr std::uint64_t kExclusionFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagNode, kRecordTagBase) | field_bit(kTagScope, kRecordTagBase) |
    field_bit(kTagReason, kRecordTagBase) | field_bit(kTagState, kRecordTagBase) |
    field_bit(kTagBlocks, kRecordTagBase) | field_bit(kTagLabel, kRecordTagBase) |
    field_bit(kTagNote, kRecordTagBase) | field_bit(kTagPolicies, kRecordTagBase) |
    field_bit(kTagSubjects, kRecordTagBase) | field_bit(kTagEvidence, kRecordTagBase);

constexpr std::uint64_t kClearanceFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagNode, kRecordTagBase) | field_bit(kTagKind, kRecordTagBase) |
    field_bit(kTagHasBand, kRecordTagBase) | field_bit(kTagBand, kRecordTagBase) |
    field_bit(kTagHasUnits, kRecordTagBase) | field_bit(kTagRequiredUnits, kRecordTagBase) |
    field_bit(kTagEnforceable, kRecordTagBase) | field_bit(kTagLabel, kRecordTagBase) |
    field_bit(kTagNote, kRecordTagBase) | field_bit(kTagPolicies, kRecordTagBase) |
    field_bit(kTagEvidence, kRecordTagBase);

constexpr std::uint64_t kExpansionZoneFields =
    field_bit(kTagId, kRecordTagBase) | field_bit(kTagGeneration, kRecordTagBase) |
    field_bit(kTagNode, kRecordTagBase) | field_bit(kTagScope, kRecordTagBase) |
    field_bit(kTagState, kRecordTagBase) | field_bit(kTagLabel, kRecordTagBase) |
    field_bit(kTagNote, kRecordTagBase) | field_bit(kTagPolicies, kRecordTagBase) |
    field_bit(kTagEvidence, kRecordTagBase) | field_bit(kTagTargetReadyBy, kRecordTagBase);

constexpr std::uint64_t kRefFields =
    field_bit(kTagRefRegistry, kRefTagBase) | field_bit(kTagRefId, kRefTagBase) |
    field_bit(kTagRefGeneration, kRefTagBase) | field_bit(kTagRefSourceRevision, kRefTagBase) |
    field_bit(kTagRefState, kRefTagBase) | field_bit(kTagRefSuccessor, kRefTagBase);

constexpr std::uint64_t kEvidenceFields =
    field_bit(kTagEvidenceSource, kEvidenceTagBase) | field_bit(kTagEvidenceId, kEvidenceTagBase) |
    field_bit(kTagEvidenceGeneration, kEvidenceTagBase) |
    field_bit(kTagEvidenceIncarnation, kEvidenceTagBase) |
    field_bit(kTagEvidenceSequence, kEvidenceTagBase);

constexpr std::uint64_t kScopeFields =
    field_bit(kTagScopeKind, kScopeTagBase) | field_bit(kTagScopeRects, kScopeTagBase) |
    field_bit(kTagScopeUnits, kScopeTagBase);

// Known enum ranges. A value outside its range is refused; a value of zero is
// accepted wherever the type has a legal "none" or "unspecified" member.
constexpr std::uint8_t kMaxRegistryKind = 10;
constexpr std::uint8_t kMaxUpstreamState = 4;
constexpr std::uint8_t kMaxSpaceNodeKind = static_cast<std::uint8_t>(kSpaceNodeKindCount - 1);
constexpr std::uint8_t kMaxSpatialClass = 8;
constexpr std::uint8_t kMaxNodeLifecycle = static_cast<std::uint8_t>(kNodeLifecycleCount - 1);
constexpr std::uint8_t kMaxOccupantKind = static_cast<std::uint8_t>(kOccupantKindCount - 1);
constexpr std::uint8_t kMaxClaimState = static_cast<std::uint8_t>(kClaimStateCount - 1);
constexpr std::uint8_t kMaxReservationState =
    static_cast<std::uint8_t>(kReservationStateCount - 1);
constexpr std::uint8_t kMaxExclusionReason = 10;
constexpr std::uint8_t kMaxExclusionState = 4;
constexpr std::uint8_t kMaxClearanceKind = 5;
constexpr std::uint8_t kMaxExpansionState = 6;
constexpr std::uint8_t kMaxScopeKind = 3;

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void put_varint(std::string& out, std::uint64_t value) {
  while (value >= 0x80u) {
    const std::uint8_t group = static_cast<std::uint8_t>(value & 0x7Fu);
    out.push_back(static_cast<char>(static_cast<unsigned int>(group) | 0x80u));
    value >>= 7u;
  }
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(value & 0x7Fu)));
}

// Big-endian, fixed width. `width` is exactly the natural width of the field.
void append_unsigned(std::string& out, std::uint64_t value, std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    const std::size_t shift = 8u * (width - 1u - index);
    out.push_back(static_cast<char>(static_cast<std::uint8_t>((value >> shift) & 0xFFu)));
  }
}

// A signed value is written as the low `width` bytes of its two's complement
// rendering, which is exact for a negative value at either width.
void append_signed(std::string& out, std::int64_t value, std::size_t width) {
  append_unsigned(out, static_cast<std::uint64_t>(value), width);
}

void append_text(std::string& out, std::string_view text) {
  put_varint(out, static_cast<std::uint64_t>(text.size()));
  out.append(text);
}

void put_field(std::string& out, std::uint64_t tag, std::string_view payload) {
  put_varint(out, tag);
  put_varint(out, static_cast<std::uint64_t>(payload.size()));
  out.append(payload);
}

void put_uint_field(std::string& out, std::uint64_t tag, std::uint64_t value, std::size_t width) {
  std::string payload;
  append_unsigned(payload, value, width);
  put_field(out, tag, payload);
}

void put_int_field(std::string& out, std::uint64_t tag, std::int64_t value, std::size_t width) {
  std::string payload;
  append_signed(payload, value, width);
  put_field(out, tag, payload);
}

void put_bool_field(std::string& out, std::uint64_t tag, bool value) {
  std::string payload(1, static_cast<char>(value ? 1 : 0));
  put_field(out, tag, payload);
}

void put_text_field(std::string& out, std::uint64_t tag, std::string_view text) {
  std::string payload;
  append_text(payload, text);
  put_field(out, tag, payload);
}

void append_rect(std::string& out, const PlanarRect& rect) {
  append_signed(out, rect.x.value(), 8);
  append_signed(out, rect.y.value(), 8);
  append_signed(out, rect.width.value(), 8);
  append_signed(out, rect.height.value(), 8);
}

void append_interval(std::string& out, const RackUnitInterval& interval) {
  append_signed(out, static_cast<std::int64_t>(interval.first), 4);
  append_signed(out, static_cast<std::int64_t>(interval.last), 4);
}

// A set is a count followed by fixed-width elements, so the count alone
// delimits it and a truncated set is detected before any element is read.
void append_rect_set(std::string& out, const RectSet& set) {
  put_varint(out, static_cast<std::uint64_t>(set.size()));
  for (const PlanarRect& rect : set.rects()) {
    append_rect(out, rect);
  }
}

void append_interval_set(std::string& out, const IntervalSet& set) {
  put_varint(out, static_cast<std::uint64_t>(set.size()));
  for (const RackUnitInterval& interval : set.intervals()) {
    append_interval(out, interval);
  }
}

void put_rect_field(std::string& out, std::uint64_t tag, const PlanarRect& rect) {
  std::string payload;
  append_rect(payload, rect);
  put_field(out, tag, payload);
}

void put_interval_field(std::string& out, std::uint64_t tag, const RackUnitInterval& interval) {
  std::string payload;
  append_interval(payload, interval);
  put_field(out, tag, payload);
}

void put_rect_set_field(std::string& out, std::uint64_t tag, const RectSet& set) {
  std::string payload;
  append_rect_set(payload, set);
  put_field(out, tag, payload);
}

void put_interval_set_field(std::string& out, std::uint64_t tag, const IntervalSet& set) {
  std::string payload;
  append_interval_set(payload, set);
  put_field(out, tag, payload);
}

// A reference is a field document of its own, so it carries its own tags and
// is bounded by the length prefix of whatever holds it.
template <typename Ref>
void append_ref(std::string& out, const Ref& ref) {
  put_uint_field(out, kTagRefRegistry, static_cast<std::uint8_t>(Ref::registry_kind), 1);
  put_text_field(out, kTagRefId, ref.id().value());
  put_uint_field(out, kTagRefGeneration, ref.generation().value(), 8);
  put_uint_field(out, kTagRefSourceRevision, ref.source_revision(), 8);
  put_uint_field(out, kTagRefState, static_cast<std::uint8_t>(ref.state()), 1);
  put_text_field(out, kTagRefSuccessor, ref.successor().value());
}

template <typename Ref>
void put_optional_ref_field(std::string& out, std::uint64_t tag,
                            const std::optional<Ref>& value) {
  std::string payload;
  if (value.has_value()) {
    payload.push_back(1);
    append_ref(payload, *value);
  } else {
    payload.push_back(0);
  }
  put_field(out, tag, payload);
}

template <typename Ref>
void put_ref_set_field(std::string& out, std::uint64_t tag, const RefSet<Ref>& set) {
  std::string payload;
  put_varint(payload, static_cast<std::uint64_t>(set.size()));
  std::string element;
  for (const Ref& ref : set.refs()) {
    element.clear();
    append_ref(element, ref);
    put_varint(payload, static_cast<std::uint64_t>(element.size()));
    payload.append(element);
  }
  put_field(out, tag, payload);
}

void put_evidence_set_field(std::string& out, std::uint64_t tag, const EvidenceSet& set) {
  std::string payload;
  put_varint(payload, static_cast<std::uint64_t>(set.size()));
  std::string element;
  for (const EvidenceRef& ref : set.refs()) {
    element.clear();
    put_uint_field(element, kTagEvidenceSource, static_cast<std::uint8_t>(ref.source), 1);
    put_text_field(element, kTagEvidenceId, ref.id.value());
    put_uint_field(element, kTagEvidenceGeneration, ref.generation.value(), 8);
    put_uint_field(element, kTagEvidenceIncarnation, ref.observed_at.incarnation().value(), 8);
    put_uint_field(element, kTagEvidenceSequence, ref.observed_at.sequence(), 8);
    put_varint(payload, static_cast<std::uint64_t>(element.size()));
    payload.append(element);
  }
  put_field(out, tag, payload);
}

void put_scope_field(std::string& out, std::uint64_t tag, const FootprintScope& scope) {
  std::string payload;
  put_uint_field(payload, kTagScopeKind, static_cast<std::uint8_t>(scope.kind), 1);
  put_rect_set_field(payload, kTagScopeRects, scope.rects);
  put_interval_set_field(payload, kTagScopeUnits, scope.units);
  put_field(out, tag, payload);
}

void put_optional_timestamp_field(std::string& out, std::uint64_t tag,
                                  const std::optional<Timestamp>& value) {
  std::string payload;
  if (value.has_value()) {
    payload.push_back(1);
    append_signed(payload, value->unix_millis, 8);
  } else {
    payload.push_back(0);
  }
  put_field(out, tag, payload);
}

template <typename Item, typename EncodeItem>
void encode_family(std::string& out, std::uint64_t tag, const std::vector<Item>& items,
                   EncodeItem encode_item) {
  std::string body;
  put_varint(body, static_cast<std::uint64_t>(items.size()));
  std::string element;
  for (const Item& item : items) {
    element.clear();
    encode_item(element, item);
    put_varint(body, static_cast<std::uint64_t>(element.size()));
    body.append(element);
  }
  put_field(out, tag, body);
}

// ---------------------------------------------------------------------------
// Record encoders. Fields are written in the order the family declares them.
// ---------------------------------------------------------------------------

void encode_node(std::string& out, const SpaceNode& node) {
  put_text_field(out, kTagId, node.id.value());
  put_uint_field(out, kTagGeneration, node.generation.value(), 8);
  put_uint_field(out, kTagKind, static_cast<std::uint8_t>(node.kind), 1);
  put_uint_field(out, kTagSpatialClass, static_cast<std::uint8_t>(node.spatial_class), 1);
  put_uint_field(out, kTagLifecycle, static_cast<std::uint8_t>(node.lifecycle), 1);
  put_text_field(out, kTagLabel, node.label.value());
  put_text_field(out, kTagNote, node.note.value());
  put_text_field(out, kTagParent, node.parent.value());
  put_uint_field(out, kTagDepth, node.depth, 4);
  put_bool_field(out, kTagHasBaseRect, node.placement.has_base_rect);
  put_rect_field(out, kTagBaseRect, node.placement.base_rect);
  put_bool_field(out, kTagHasUSpan, node.placement.has_u_span);
  put_interval_field(out, kTagUSpan, node.placement.u_span);
  put_int_field(out, kTagPlanarArea, node.own_planar.declared_area.value(), 8);
  put_rect_set_field(out, kTagPlanarRects, node.own_planar.rects);
  put_int_field(out, kTagRackHeight, static_cast<std::int64_t>(node.own_rack.height.value()), 4);
  put_optional_ref_field(out, kTagLocation, node.location);
  put_optional_ref_field(out, kTagFacilityNode, node.facility_node);
  put_optional_ref_field(out, kTagRack, node.rack);
  put_ref_set_field(out, kTagAssets, node.assets);
  put_ref_set_field(out, kTagPolicies, node.policies);
  put_evidence_set_field(out, kTagEvidence, node.evidence);
  put_text_field(out, kTagReplacedBy, node.replaced_by.value());
  put_text_field(out, kTagReplaces, node.replaces.value());
}

void encode_claim(std::string& out, const OccupancyClaim& claim) {
  put_text_field(out, kTagId, claim.id.value());
  put_uint_field(out, kTagGeneration, claim.generation.value(), 8);
  put_text_field(out, kTagNode, claim.node.value());
  put_scope_field(out, kTagScope, claim.scope);
  put_uint_field(out, kTagState, static_cast<std::uint8_t>(claim.state), 1);
  put_uint_field(out, kTagOccupant, static_cast<std::uint8_t>(claim.occupant), 1);
  put_text_field(out, kTagLabel, claim.label.value());
  put_text_field(out, kTagNote, claim.note.value());
  put_optional_ref_field(out, kTagRack, claim.rack);
  put_ref_set_field(out, kTagAssets, claim.assets);
  put_ref_set_field(out, kTagPolicies, claim.policies);
  put_evidence_set_field(out, kTagEvidence, claim.evidence);
  put_optional_ref_field(out, kTagFromReservation, claim.from_reservation);
}

void encode_reservation(std::string& out, const FootprintReservation& reservation) {
  put_text_field(out, kTagId, reservation.id.value());
  put_uint_field(out, kTagGeneration, reservation.generation.value(), 8);
  put_text_field(out, kTagNode, reservation.node.value());
  put_scope_field(out, kTagScope, reservation.scope);
  put_uint_field(out, kTagState, static_cast<std::uint8_t>(reservation.state), 1);
  std::string payload;
  append_ref(payload, reservation.reservation);
  put_field(out, kTagReservation, payload);
  put_text_field(out, kTagHolderLabel, reservation.holder_label.value());
  put_ref_set_field(out, kTagHolderAssets, reservation.holder_assets);
  put_ref_set_field(out, kTagPolicies, reservation.policies);
  put_evidence_set_field(out, kTagEvidence, reservation.evidence);
  put_optional_timestamp_field(out, kTagNotAfter, reservation.not_after);
}

void encode_exclusion(std::string& out, const ExclusionRegion& region) {
  put_text_field(out, kTagId, region.id.value());
  put_uint_field(out, kTagGeneration, region.generation.value(), 8);
  put_text_field(out, kTagNode, region.node.value());
  put_scope_field(out, kTagScope, region.scope);
  put_uint_field(out, kTagReason, static_cast<std::uint8_t>(region.reason), 1);
  put_uint_field(out, kTagState, static_cast<std::uint8_t>(region.state), 1);
  put_uint_field(out, kTagBlocks, region.blocks.bits(), 4);
  put_text_field(out, kTagLabel, region.label.value());
  put_text_field(out, kTagNote, region.note.value());
  put_ref_set_field(out, kTagPolicies, region.policies);
  put_ref_set_field(out, kTagSubjects, region.subjects);
  put_evidence_set_field(out, kTagEvidence, region.evidence);
}

void encode_clearance(std::string& out, const ClearanceConstraint& constraint) {
  put_text_field(out, kTagId, constraint.id.value());
  put_uint_field(out, kTagGeneration, constraint.generation.value(), 8);
  put_text_field(out, kTagNode, constraint.node.value());
  put_uint_field(out, kTagKind, static_cast<std::uint8_t>(constraint.kind), 1);
  put_bool_field(out, kTagHasBand, constraint.has_band);
  put_rect_field(out, kTagBand, constraint.band);
  put_bool_field(out, kTagHasUnits, constraint.has_units);
  put_interval_set_field(out, kTagRequiredUnits, constraint.required_free_units);
  put_bool_field(out, kTagEnforceable, constraint.enforceable);
  put_text_field(out, kTagLabel, constraint.label.value());
  put_text_field(out, kTagNote, constraint.note.value());
  put_ref_set_field(out, kTagPolicies, constraint.policies);
  put_evidence_set_field(out, kTagEvidence, constraint.evidence);
}

void encode_expansion_zone(std::string& out, const ExpansionZone& zone) {
  put_text_field(out, kTagId, zone.id.value());
  put_uint_field(out, kTagGeneration, zone.generation.value(), 8);
  put_text_field(out, kTagNode, zone.node.value());
  put_scope_field(out, kTagScope, zone.scope);
  put_uint_field(out, kTagState, static_cast<std::uint8_t>(zone.state), 1);
  put_text_field(out, kTagLabel, zone.label.value());
  put_text_field(out, kTagNote, zone.note.value());
  put_ref_set_field(out, kTagPolicies, zone.policies);
  put_evidence_set_field(out, kTagEvidence, zone.evidence);
  put_optional_timestamp_field(out, kTagTargetReadyBy, zone.target_ready_by);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

Status fail(ErrorCode code, std::string_view what, std::string_view problem) {
  std::string message(what);
  message.append(problem);
  return Status::failure(code, std::move(message));
}

// A window over exactly the bytes of one field, one record or one document.
// Every read checks the remaining length before it reads, so no read can pass
// the end of the window and no length is used to size an allocation before it
// has been checked against the bytes that are actually there.
class Decoder final {
 public:
  Decoder() = default;
  explicit Decoder(std::string_view bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] bool at_end() const noexcept { return pos_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - pos_; }

  Status read_u8(std::uint8_t& out) {
    if (remaining() < 1) return short_read();
    out = static_cast<std::uint8_t>(static_cast<unsigned char>(bytes_[pos_]));
    ++pos_;
    return Status::success();
  }

  Status read_unsigned(std::size_t width, std::uint64_t& out) {
    if (remaining() < width) return short_read();
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value = (value << 8u) |
              static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[pos_ + index]));
    }
    pos_ += width;
    out = value;
    return Status::success();
  }

  // Widths four and eight are the only signed widths the format uses.
  Status read_signed(std::size_t width, std::int64_t& out) {
    std::uint64_t raw = 0;
    const Status status = read_unsigned(width, raw);
    if (!status) return status;
    if (width >= 8) {
      out = static_cast<std::int64_t>(raw);
      return Status::success();
    }
    out = static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
    return Status::success();
  }

  Status read_bool(bool& out) {
    std::uint8_t byte = 0;
    const Status status = read_u8(byte);
    if (!status) return status;
    if (byte > 1u) {
      return Status::failure(ErrorCode::corruption, "a boolean is written as exactly 0 or 1");
    }
    out = byte != 0u;
    return Status::success();
  }

  // Unsigned LEB128, at most ten bytes, minimal form only.
  Status read_varint(std::uint64_t& out) {
    std::uint64_t value = 0;
    std::size_t shift = 0;
    for (std::size_t index = 0; index < kMaxVarintBytes; ++index) {
      std::uint8_t byte = 0;
      const Status status = read_u8(byte);
      if (!status) return status;
      const std::uint64_t group = static_cast<std::uint64_t>(byte & 0x7Fu);
      if (shift == 63 && group > 1u) {
        return Status::failure(ErrorCode::corruption, "a varint value does not fit in 64 bits");
      }
      value |= group << shift;
      if ((byte & 0x80u) == 0u) {
        // A minimal encoding never ends in a group that contributes nothing.
        if (index != 0 && group == 0u) {
          return Status::failure(ErrorCode::corruption,
                                 "a varint is not encoded in its minimal form");
        }
        out = value;
        return Status::success();
      }
      shift += 7u;
    }
    return Status::failure(ErrorCode::oversized, "a varint is longer than ten bytes");
  }

  // Text is bounded before it is allocated: by the bytes that are actually
  // left in this window and by the field's own limit.
  Status read_text(std::size_t limit, std::string& out) {
    std::uint64_t length = 0;
    const Status status = read_varint(length);
    if (!status) return status;
    if (length > static_cast<std::uint64_t>(remaining())) {
      return Status::failure(ErrorCode::oversized,
                             "a text length exceeds the bytes that remain");
    }
    if (length > static_cast<std::uint64_t>(limit)) {
      return Status::failure(ErrorCode::oversized, "a text value exceeds its bound");
    }
    out.assign(bytes_.data() + pos_, static_cast<std::size_t>(length));
    pos_ += static_cast<std::size_t>(length);
    return Status::success();
  }

  // Splits the next `length` bytes off this window as a window of their own.
  Status take(std::size_t length, Decoder& out) {
    if (length > remaining()) {
      return Status::failure(ErrorCode::oversized,
                             "a declared length exceeds the bytes that remain");
    }
    out = Decoder(bytes_.substr(pos_, length));
    pos_ += length;
    return Status::success();
  }

 private:
  Status short_read() const {
    return Status::failure(ErrorCode::corruption, "the input ended before a value was complete");
  }

  std::string_view bytes_{};
  std::size_t pos_ = 0;
};

Status unknown_tag(std::string_view what) { return fail(ErrorCode::corruption, what, " has an unknown tag"); }
Status repeated_tag(std::string_view what) { return fail(ErrorCode::duplicate_identity, what, " repeats a tag"); }
Status missing_tag(std::string_view what) { return fail(ErrorCode::corruption, what, " is missing a field"); }
Status width_mismatch() {
  return Status::failure(ErrorCode::corruption, "a field payload does not match its width");
}

// Reads one field header out of `body` and leaves its payload in `field`. A
// tag that this family does not use is refused before its length is trusted.
Status next_field(Decoder& body, std::uint64_t base, std::uint64_t required, std::uint64_t& seen,
                  std::string_view what, std::uint64_t& tag, Decoder& field) {
  Status status = body.read_varint(tag);
  if (!status) return status;
  if (tag < base || tag - base >= 64u) return unknown_tag(what);
  const std::uint64_t bit = std::uint64_t{1} << (tag - base);
  if ((required & bit) == 0u) return unknown_tag(what);
  if ((seen & bit) != 0u) return repeated_tag(what);
  seen |= bit;
  std::uint64_t length = 0;
  status = body.read_varint(length);
  if (!status) return status;
  return body.take(static_cast<std::size_t>(length), field);
}

Status expect_end(const Decoder& field) {
  if (!field.at_end()) return width_mismatch();
  return Status::success();
}

// ---------------------------------------------------------------------------
// Field readers. Each one consumes exactly the payload of one field.
// ---------------------------------------------------------------------------

Status read_bool_field(Decoder& field, bool& out) {
  const Status status = field.read_bool(out);
  if (!status) return status;
  return expect_end(field);
}

Status read_u8_field(Decoder& field, std::uint8_t& out) {
  const Status status = field.read_u8(out);
  if (!status) return status;
  return expect_end(field);
}

Status read_u32_field(Decoder& field, std::uint32_t& out) {
  std::uint64_t raw = 0;
  const Status status = field.read_unsigned(4, raw);
  if (!status) return status;
  out = static_cast<std::uint32_t>(raw);
  return expect_end(field);
}

Status read_u64_field(Decoder& field, std::uint64_t& out) {
  const Status status = field.read_unsigned(8, out);
  if (!status) return status;
  return expect_end(field);
}

Status read_i32_field(Decoder& field, std::int32_t& out) {
  std::int64_t raw = 0;
  const Status status = field.read_signed(4, raw);
  if (!status) return status;
  out = static_cast<std::int32_t>(raw);
  return expect_end(field);
}

Status read_i64_field(Decoder& field, std::int64_t& out) {
  const Status status = field.read_signed(8, out);
  if (!status) return status;
  return expect_end(field);
}

Status read_text_field(Decoder& field, std::size_t limit, std::string& out) {
  const Status status = field.read_text(limit, out);
  if (!status) return status;
  return expect_end(field);
}

Status read_enum_field(Decoder& field, std::uint8_t max_value, std::string_view what,
                       std::uint8_t& out) {
  const Status status = read_u8_field(field, out);
  if (!status) return status;
  if (out > max_value) return fail(ErrorCode::unknown_enum_token, what, " has an unknown value");
  return Status::success();
}

template <typename Id>
Status read_identity_field(Decoder& field, bool allow_empty, Id& out) {
  std::string text;
  const Status status = read_text_field(field, Limits::kMaxIdentifierBytes, text);
  if (!status) return status;
  if (text.empty()) {
    if (!allow_empty) {
      return Status::failure(ErrorCode::malformed_identity, "an identity must not be empty");
    }
    out = Id{};
    return Status::success();
  }
  if (!is_valid_identifier(text)) {
    return Status::failure(ErrorCode::malformed_identity,
                           "an identity must satisfy: " + std::string(identifier_syntax_help()));
  }
  Result<Id> parsed = Id::parse(text);
  if (!parsed) return parsed.error();
  out = std::move(parsed).value();
  return Status::success();
}

Status read_label_field(Decoder& field, DisplayLabel& out) {
  std::string text;
  const Status status = read_text_field(field, Limits::kMaxLabelBytes, text);
  if (!status) return status;
  if (!is_utf8(text)) {
    return Status::failure(ErrorCode::invalid_utf8, "a label must be valid UTF-8");
  }
  Result<DisplayLabel> parsed = DisplayLabel::parse(text);
  if (!parsed) return parsed.error();
  out = std::move(parsed).value();
  return Status::success();
}

Status read_note_field(Decoder& field, Note& out) {
  std::string text;
  const Status status = read_text_field(field, Limits::kMaxNoteBytes, text);
  if (!status) return status;
  if (!is_utf8(text)) {
    return Status::failure(ErrorCode::invalid_utf8, "a note must be valid UTF-8");
  }
  Result<Note> parsed = Note::parse(text);
  if (!parsed) return parsed.error();
  out = std::move(parsed).value();
  return Status::success();
}

Status read_rect_field(Decoder& field, PlanarRect& out) {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t width = 0;
  std::int64_t height = 0;
  Status status = field.read_signed(8, x);
  if (!status) return status;
  status = field.read_signed(8, y);
  if (!status) return status;
  status = field.read_signed(8, width);
  if (!status) return status;
  status = field.read_signed(8, height);
  if (!status) return status;
  out = PlanarRect::make(x, y, width, height);
  return expect_end(field);
}

Status read_interval_field(Decoder& field, RackUnitInterval& out) {
  std::int64_t first = 0;
  std::int64_t last = 0;
  Status status = field.read_signed(4, first);
  if (!status) return status;
  status = field.read_signed(4, last);
  if (!status) return status;
  out = RackUnitInterval::make(static_cast<std::int32_t>(first), static_cast<std::int32_t>(last));
  return expect_end(field);
}

// A rectangle set is a count followed by fixed-width rectangles. The count is
// bounded twice before anything is allocated: by the bytes that remain, so an
// absurd count near the end of the input costs nothing, and by the per-record
// bound.
Status read_rect_set_field(Decoder& field, RectSet& out) {
  std::uint64_t count = 0;
  Status status = field.read_varint(count);
  if (!status) return status;
  if (count > static_cast<std::uint64_t>(field.remaining() / kRectBytes)) {
    return Status::failure(ErrorCode::oversized,
                           "a rectangle count exceeds the bytes that remain");
  }
  if (count > static_cast<std::uint64_t>(Limits::kMaxExtentsPerRecord)) {
    return Status::failure(ErrorCode::limit_exceeded, "a rectangle set exceeds its bound");
  }
  std::vector<PlanarRect> rects;
  rects.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    PlanarRect rect;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    status = field.read_signed(8, x);
    if (!status) return status;
    status = field.read_signed(8, y);
    if (!status) return status;
    status = field.read_signed(8, width);
    if (!status) return status;
    status = field.read_signed(8, height);
    if (!status) return status;
    rect = PlanarRect::make(x, y, width, height);
    rects.push_back(rect);
  }
  Result<RectSet> built = RectSet::build(std::move(rects));
  if (!built) return built.error();
  out = std::move(built).value();
  return expect_end(field);
}

Status read_interval_set_field(Decoder& field, IntervalSet& out) {
  std::uint64_t count = 0;
  Status status = field.read_varint(count);
  if (!status) return status;
  if (count > static_cast<std::uint64_t>(field.remaining() / kIntervalBytes)) {
    return Status::failure(ErrorCode::oversized,
                           "an interval count exceeds the bytes that remain");
  }
  if (count > static_cast<std::uint64_t>(Limits::kMaxExtentsPerRecord)) {
    return Status::failure(ErrorCode::limit_exceeded, "an interval set exceeds its bound");
  }
  std::vector<RackUnitInterval> intervals;
  intervals.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    std::int64_t first = 0;
    std::int64_t last = 0;
    status = field.read_signed(4, first);
    if (!status) return status;
    status = field.read_signed(4, last);
    if (!status) return status;
    intervals.push_back(RackUnitInterval::make(static_cast<std::int32_t>(first),
                                               static_cast<std::int32_t>(last)));
  }
  Result<IntervalSet> built = IntervalSet::build(std::move(intervals));
  if (!built) return built.error();
  out = std::move(built).value();
  return expect_end(field);
}

Status read_scope_field(Decoder& field, FootprintScope& out) {
  std::uint64_t seen = 0;
  while (!field.at_end()) {
    std::uint64_t tag = 0;
    Decoder body;
    Status status = next_field(field, kScopeTagBase, kScopeFields, seen, "a footprint scope", tag, body);
    if (!status) return status;
    switch (tag) {
      case kTagScopeKind: {
        std::uint8_t value = 0;
        status = read_enum_field(body, kMaxScopeKind, "a footprint scope kind", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "a footprint scope kind has an unknown value");
        }
        out.kind = static_cast<FootprintScopeKind>(value);
        break;
      }
      case kTagScopeRects:
        status = read_rect_set_field(body, out.rects);
        if (!status) return status;
        break;
      case kTagScopeUnits:
        status = read_interval_set_field(body, out.units);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a footprint scope");
    }
  }
  if (seen != kScopeFields) return missing_tag("a footprint scope");
  return Status::success();
}

// An external identity is opaque but never empty, never over its bound and
// never a control byte: it is checked with the same rule the reference types
// enforce, before any value of that type is built.
Status read_external_id(Decoder& field, bool allow_empty, ExternalId& out) {
  std::string text;
  const Status status = read_text_field(field, Limits::kMaxReferenceBytes, text);
  if (!status) return status;
  if (text.empty()) {
    if (!allow_empty) {
      return Status::failure(ErrorCode::malformed_identity,
                             "a reference identity must not be empty");
    }
    out = ExternalId{};
    return Status::success();
  }
  if (!ExternalId::is_acceptable(text)) {
    return Status::failure(ErrorCode::malformed_identity,
                           "a reference identity is not an acceptable external identity");
  }
  Result<ExternalId> parsed = ExternalId::parse(text);
  if (!parsed) return parsed.error();
  out = std::move(parsed).value();
  return Status::success();
}

// A strong reference is a field document. Its registry tag is part of the
// value: a reference of one family that carries the tag of another is not
// representable as that family's type and is refused as corrupt.
template <typename Ref>
Status read_ref(Decoder& field, Ref& out) {
  std::uint64_t seen = 0;
  std::uint8_t registry = 0;
  ExternalId id;
  std::uint64_t generation = 0;
  std::uint64_t source_revision = 0;
  std::uint8_t state = 0;
  ExternalId successor;
  while (!field.at_end()) {
    std::uint64_t tag = 0;
    Decoder body;
    Status status = next_field(field, kRefTagBase, kRefFields, seen, "a reference", tag, body);
    if (!status) return status;
    switch (tag) {
      case kTagRefRegistry:
        status = read_enum_field(body, kMaxRegistryKind, "a reference registry kind", registry);
        if (!status) return status;
        break;
      case kTagRefId:
        status = read_external_id(body, false, id);
        if (!status) return status;
        break;
      case kTagRefGeneration:
        status = read_u64_field(body, generation);
        if (!status) return status;
        break;
      case kTagRefSourceRevision:
        status = read_u64_field(body, source_revision);
        if (!status) return status;
        break;
      case kTagRefState:
        status = read_enum_field(body, kMaxUpstreamState, "an upstream state", state);
        if (!status) return status;
        break;
      case kTagRefSuccessor:
        status = read_external_id(body, true, successor);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a reference");
    }
  }
  if (seen != kRefFields) return missing_tag("a reference");
  if (registry != static_cast<std::uint8_t>(Ref::registry_kind)) {
    return Status::failure(ErrorCode::corruption,
                           "a reference carries the registry kind of another family");
  }
  Result<Ref> made = Ref::of(id.value());
  if (!made) return made.error();
  Ref ref = std::move(made).value();
  ref.set_generation(EntityGeneration{generation});
  ref.set_source_revision(source_revision);
  ref.set_state(static_cast<UpstreamState>(state));
  if (!successor.empty()) ref.set_successor(std::move(successor));
  out = std::move(ref);
  return Status::success();
}

// An optional reference is always written: one presence byte, and the
// reference itself only when it is present.
template <typename Ref>
Status read_optional_ref_field(Decoder& field, std::optional<Ref>& out) {
  bool present = false;
  Status status = field.read_bool(present);
  if (!status) return status;
  if (!present) {
    out.reset();
    return expect_end(field);
  }
  Ref ref;
  status = read_ref(field, ref);
  if (!status) return status;
  out = std::move(ref);
  return Status::success();
}

template <typename Ref>
Status read_ref_set_field(Decoder& field, RefSet<Ref>& out) {
  std::uint64_t count = 0;
  Status status = field.read_varint(count);
  if (!status) return status;
  if (count > static_cast<std::uint64_t>(field.remaining())) {
    return Status::failure(ErrorCode::oversized,
                           "a reference count exceeds the bytes that remain");
  }
  if (count > static_cast<std::uint64_t>(Limits::kMaxReferencesPerRecord)) {
    return Status::failure(ErrorCode::limit_exceeded, "a reference set exceeds its bound");
  }
  std::vector<Ref> refs;
  refs.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    std::uint64_t length = 0;
    status = field.read_varint(length);
    if (!status) return status;
    Decoder element;
    status = field.take(static_cast<std::size_t>(length), element);
    if (!status) return status;
    Ref ref;
    status = read_ref(element, ref);
    if (!status) return status;
    refs.push_back(std::move(ref));
  }
  Result<RefSet<Ref>> built = RefSet<Ref>::build(std::move(refs));
  if (!built) return built.error();
  out = std::move(built).value();
  return expect_end(field);
}

Status read_evidence_set_field(Decoder& field, EvidenceSet& out) {
  std::uint64_t count = 0;
  Status status = field.read_varint(count);
  if (!status) return status;
  if (count > static_cast<std::uint64_t>(field.remaining())) {
    return Status::failure(ErrorCode::oversized,
                           "an evidence count exceeds the bytes that remain");
  }
  if (count > static_cast<std::uint64_t>(Limits::kMaxEvidencePerRecord)) {
    return Status::failure(ErrorCode::limit_exceeded, "an evidence set exceeds its bound");
  }
  std::vector<EvidenceRef> refs;
  refs.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    std::uint64_t length = 0;
    status = field.read_varint(length);
    if (!status) return status;
    Decoder element;
    status = field.take(static_cast<std::size_t>(length), element);
    if (!status) return status;
    EvidenceRef ref;
    std::uint64_t seen = 0;
    while (!element.at_end()) {
      std::uint64_t tag = 0;
      Decoder body;
      status = next_field(element, kEvidenceTagBase, kEvidenceFields, seen, "an evidence reference",
                          tag, body);
      if (!status) return status;
      switch (tag) {
        case kTagEvidenceSource: {
          std::uint8_t value = 0;
          status = read_enum_field(body, kMaxRegistryKind, "an evidence source", value);
          if (!status) return status;
          ref.source = static_cast<RegistryKind>(value);
          break;
        }
        case kTagEvidenceId:
          status = read_external_id(body, false, ref.id);
          if (!status) return status;
          break;
        case kTagEvidenceGeneration: {
          std::uint64_t value = 0;
          status = read_u64_field(body, value);
          if (!status) return status;
          ref.generation = EntityGeneration{value};
          break;
        }
        case kTagEvidenceIncarnation: {
          std::uint64_t value = 0;
          status = read_u64_field(body, value);
          if (!status) return status;
          ref.observed_at = AttemptId{StoreIncarnation{value}, ref.observed_at.sequence()};
          break;
        }
        case kTagEvidenceSequence: {
          std::uint64_t value = 0;
          status = read_u64_field(body, value);
          if (!status) return status;
          ref.observed_at = AttemptId{ref.observed_at.incarnation(), value};
          break;
        }
        default:
          return unknown_tag("an evidence reference");
      }
    }
    if (seen != kEvidenceFields) return missing_tag("an evidence reference");
    refs.push_back(std::move(ref));
  }
  Result<EvidenceSet> built = EvidenceSet::build(std::move(refs));
  if (!built) return built.error();
  out = std::move(built).value();
  return expect_end(field);
}

Status read_optional_timestamp_field(Decoder& field, std::optional<Timestamp>& out) {
  bool present = false;
  Status status = field.read_bool(present);
  if (!status) return status;
  if (!present) {
    out.reset();
    return expect_end(field);
  }
  std::int64_t millis = 0;
  status = field.read_signed(8, millis);
  if (!status) return status;
  out = Timestamp{millis};
  return expect_end(field);
}

// ---------------------------------------------------------------------------
// Record decoders
// ---------------------------------------------------------------------------

Status decode_node(Decoder& body, SpaceNode& node) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status = next_field(body, kRecordTagBase, kNodeFields, seen, "a node record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, node.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        node.generation = EntityGeneration{value};
        break;
      }
      case kTagKind: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxSpaceNodeKind, "a node kind", value);
        if (!status) return status;
        // `none` is a legal zero and is decoded as it stands: whether a node
        // may be persisted without a kind is the audit's question, not this
        // reader's.
        node.kind = space_node_kind_from_value(value);
        break;
      }
      case kTagSpatialClass: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxSpatialClass, "a spatial class", value);
        if (!status) return status;
        node.spatial_class = static_cast<SpatialClass>(value);
        break;
      }
      case kTagLifecycle: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxNodeLifecycle, "a node lifecycle", value);
        if (!status) return status;
        // NodeLifecycle has no zero member. A zero is decoded as it stands and
        // left for the model audit to refuse.
        node.lifecycle = value == 0u ? static_cast<NodeLifecycle>(value)
                                     : node_lifecycle_from_value(value);
        break;
      }
      case kTagLabel:
        status = read_label_field(field, node.label);
        if (!status) return status;
        break;
      case kTagNote:
        status = read_note_field(field, node.note);
        if (!status) return status;
        break;
      case kTagParent:
        status = read_identity_field(field, true, node.parent);
        if (!status) return status;
        break;
      case kTagDepth:
        status = read_u32_field(field, node.depth);
        if (!status) return status;
        break;
      case kTagHasBaseRect:
        status = read_bool_field(field, node.placement.has_base_rect);
        if (!status) return status;
        break;
      case kTagBaseRect:
        status = read_rect_field(field, node.placement.base_rect);
        if (!status) return status;
        break;
      case kTagHasUSpan:
        status = read_bool_field(field, node.placement.has_u_span);
        if (!status) return status;
        break;
      case kTagUSpan:
        status = read_interval_field(field, node.placement.u_span);
        if (!status) return status;
        break;
      case kTagPlanarArea: {
        std::int64_t value = 0;
        status = read_i64_field(field, value);
        if (!status) return status;
        node.own_planar.declared_area = SquareMillimeters{value};
        break;
      }
      case kTagPlanarRects:
        status = read_rect_set_field(field, node.own_planar.rects);
        if (!status) return status;
        break;
      case kTagRackHeight: {
        std::int32_t value = 0;
        status = read_i32_field(field, value);
        if (!status) return status;
        node.own_rack.height = RackUnits{value};
        break;
      }
      case kTagLocation:
        status = read_optional_ref_field(field, node.location);
        if (!status) return status;
        break;
      case kTagFacilityNode:
        status = read_optional_ref_field(field, node.facility_node);
        if (!status) return status;
        break;
      case kTagRack:
        status = read_optional_ref_field(field, node.rack);
        if (!status) return status;
        break;
      case kTagAssets:
        status = read_ref_set_field(field, node.assets);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, node.policies);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, node.evidence);
        if (!status) return status;
        break;
      case kTagReplacedBy:
        status = read_identity_field(field, true, node.replaced_by);
        if (!status) return status;
        break;
      case kTagReplaces:
        status = read_identity_field(field, true, node.replaces);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a node record");
    }
  }
  if (seen != kNodeFields) return missing_tag("a node record");
  return Status::success();
}

Status decode_claim(Decoder& body, OccupancyClaim& claim) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status =
        next_field(body, kRecordTagBase, kClaimFields, seen, "a claim record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, claim.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        claim.generation = EntityGeneration{value};
        break;
      }
      case kTagNode:
        status = read_identity_field(field, false, claim.node);
        if (!status) return status;
        break;
      case kTagScope:
        status = read_scope_field(field, claim.scope);
        if (!status) return status;
        break;
      case kTagState: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxClaimState, "a claim state", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "a claim state has an unknown value");
        }
        claim.state = claim_state_from_value(value);
        break;
      }
      case kTagOccupant: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxOccupantKind, "an occupant kind", value);
        if (!status) return status;
        claim.occupant = occupant_kind_from_value(value);
        break;
      }
      case kTagLabel:
        status = read_label_field(field, claim.label);
        if (!status) return status;
        break;
      case kTagNote:
        status = read_note_field(field, claim.note);
        if (!status) return status;
        break;
      case kTagRack:
        status = read_optional_ref_field(field, claim.rack);
        if (!status) return status;
        break;
      case kTagAssets:
        status = read_ref_set_field(field, claim.assets);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, claim.policies);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, claim.evidence);
        if (!status) return status;
        break;
      case kTagFromReservation:
        status = read_optional_ref_field(field, claim.from_reservation);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a claim record");
    }
  }
  if (seen != kClaimFields) return missing_tag("a claim record");
  return Status::success();
}

Status decode_reservation(Decoder& body, FootprintReservation& reservation) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status =
        next_field(body, kRecordTagBase, kReservationFields, seen, "a reservation record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, reservation.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        reservation.generation = EntityGeneration{value};
        break;
      }
      case kTagNode:
        status = read_identity_field(field, false, reservation.node);
        if (!status) return status;
        break;
      case kTagScope:
        status = read_scope_field(field, reservation.scope);
        if (!status) return status;
        break;
      case kTagState: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxReservationState, "a reservation state", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "a reservation state has an unknown value");
        }
        reservation.state = reservation_state_from_value(value);
        break;
      }
      case kTagReservation:
        status = read_ref(field, reservation.reservation);
        if (!status) return status;
        break;
      case kTagHolderLabel:
        status = read_label_field(field, reservation.holder_label);
        if (!status) return status;
        break;
      case kTagHolderAssets:
        status = read_ref_set_field(field, reservation.holder_assets);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, reservation.policies);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, reservation.evidence);
        if (!status) return status;
        break;
      case kTagNotAfter:
        status = read_optional_timestamp_field(field, reservation.not_after);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a reservation record");
    }
  }
  if (seen != kReservationFields) return missing_tag("a reservation record");
  return Status::success();
}

Status decode_exclusion(Decoder& body, ExclusionRegion& region) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status =
        next_field(body, kRecordTagBase, kExclusionFields, seen, "an exclusion record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, region.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        region.generation = EntityGeneration{value};
        break;
      }
      case kTagNode:
        status = read_identity_field(field, false, region.node);
        if (!status) return status;
        break;
      case kTagScope:
        status = read_scope_field(field, region.scope);
        if (!status) return status;
        break;
      case kTagReason: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxExclusionReason, "an exclusion reason", value);
        if (!status) return status;
        region.reason = exclusion_reason_from_value(value);
        break;
      }
      case kTagState: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxExclusionState, "an exclusion state", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "an exclusion state has an unknown value");
        }
        region.state = exclusion_state_from_value(value);
        break;
      }
      case kTagBlocks: {
        std::uint32_t bits = 0;
        status = read_u32_field(field, bits);
        if (!status) return status;
        Result<OccupantMask> mask = OccupantMask::from_bits(bits);
        if (!mask) return mask.error();
        region.blocks = mask.value();
        break;
      }
      case kTagLabel:
        status = read_label_field(field, region.label);
        if (!status) return status;
        break;
      case kTagNote:
        status = read_note_field(field, region.note);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, region.policies);
        if (!status) return status;
        break;
      case kTagSubjects:
        status = read_ref_set_field(field, region.subjects);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, region.evidence);
        if (!status) return status;
        break;
      default:
        return unknown_tag("an exclusion record");
    }
  }
  if (seen != kExclusionFields) return missing_tag("an exclusion record");
  return Status::success();
}

Status decode_clearance(Decoder& body, ClearanceConstraint& constraint) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status =
        next_field(body, kRecordTagBase, kClearanceFields, seen, "a clearance record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, constraint.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        constraint.generation = EntityGeneration{value};
        break;
      }
      case kTagNode:
        status = read_identity_field(field, false, constraint.node);
        if (!status) return status;
        break;
      case kTagKind: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxClearanceKind, "a clearance kind", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "a clearance kind has an unknown value");
        }
        constraint.kind = clearance_kind_from_value(value);
        break;
      }
      case kTagHasBand:
        status = read_bool_field(field, constraint.has_band);
        if (!status) return status;
        break;
      case kTagBand:
        status = read_rect_field(field, constraint.band);
        if (!status) return status;
        break;
      case kTagHasUnits:
        status = read_bool_field(field, constraint.has_units);
        if (!status) return status;
        break;
      case kTagRequiredUnits:
        status = read_interval_set_field(field, constraint.required_free_units);
        if (!status) return status;
        break;
      case kTagEnforceable:
        status = read_bool_field(field, constraint.enforceable);
        if (!status) return status;
        break;
      case kTagLabel:
        status = read_label_field(field, constraint.label);
        if (!status) return status;
        break;
      case kTagNote:
        status = read_note_field(field, constraint.note);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, constraint.policies);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, constraint.evidence);
        if (!status) return status;
        break;
      default:
        return unknown_tag("a clearance record");
    }
  }
  if (seen != kClearanceFields) return missing_tag("a clearance record");
  return Status::success();
}

Status decode_expansion_zone(Decoder& body, ExpansionZone& zone) {
  std::uint64_t seen = 0;
  while (!body.at_end()) {
    std::uint64_t tag = 0;
    Decoder field;
    Status status =
        next_field(body, kRecordTagBase, kExpansionZoneFields, seen, "an expansion zone record", tag, field);
    if (!status) return status;
    switch (tag) {
      case kTagId:
        status = read_identity_field(field, false, zone.id);
        if (!status) return status;
        break;
      case kTagGeneration: {
        std::uint64_t value = 0;
        status = read_u64_field(field, value);
        if (!status) return status;
        zone.generation = EntityGeneration{value};
        break;
      }
      case kTagNode:
        status = read_identity_field(field, false, zone.node);
        if (!status) return status;
        break;
      case kTagScope:
        status = read_scope_field(field, zone.scope);
        if (!status) return status;
        break;
      case kTagState: {
        std::uint8_t value = 0;
        status = read_enum_field(field, kMaxExpansionState, "an expansion state", value);
        if (!status) return status;
        if (value < 1u) {
          return Status::failure(ErrorCode::unknown_enum_token,
                                 "an expansion state has an unknown value");
        }
        zone.state = expansion_state_from_value(value);
        break;
      }
      case kTagLabel:
        status = read_label_field(field, zone.label);
        if (!status) return status;
        break;
      case kTagNote:
        status = read_note_field(field, zone.note);
        if (!status) return status;
        break;
      case kTagPolicies:
        status = read_ref_set_field(field, zone.policies);
        if (!status) return status;
        break;
      case kTagEvidence:
        status = read_evidence_set_field(field, zone.evidence);
        if (!status) return status;
        break;
      case kTagTargetReadyBy:
        status = read_optional_timestamp_field(field, zone.target_ready_by);
        if (!status) return status;
        break;
      default:
        return unknown_tag("an expansion zone record");
    }
  }
  if (seen != kExpansionZoneFields) return missing_tag("an expansion zone record");
  return Status::success();
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

struct DecodedModel final {
  std::vector<SpaceNode> nodes;
  std::vector<OccupancyClaim> claims;
  std::vector<FootprintReservation> reservations;
  std::vector<ExclusionRegion> exclusions;
  std::vector<ClearanceConstraint> clearances;
  std::vector<ExpansionZone> expansion_zones;
};

// One family is one field: a count, then that many length-prefixed records.
// The count is bounded by the bytes that remain before it is used to reserve.
template <typename Item, typename DecodeItem>
Status decode_family(Decoder& document, std::uint64_t tag, std::uint64_t limit,
                     std::string_view what, DecodeItem decode_item, std::vector<Item>& out) {
  std::uint64_t actual_tag = 0;
  Status status = document.read_varint(actual_tag);
  if (!status) return status;
  if (actual_tag != tag) {
    return Status::failure(ErrorCode::corruption, "the document is missing a record family");
  }
  std::uint64_t length = 0;
  status = document.read_varint(length);
  if (!status) return status;
  Decoder family;
  status = document.take(static_cast<std::size_t>(length), family);
  if (!status) return status;
  std::uint64_t count = 0;
  status = family.read_varint(count);
  if (!status) return status;
  if (count > static_cast<std::uint64_t>(family.remaining())) {
    return fail(ErrorCode::oversized, what, " has a count that exceeds the bytes that remain");
  }
  if (count > limit) return fail(ErrorCode::limit_exceeded, what, " exceeds its record bound");
  std::vector<Item> items;
  items.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    std::uint64_t item_length = 0;
    status = family.read_varint(item_length);
    if (!status) return status;
    Decoder item_body;
    status = family.take(static_cast<std::size_t>(item_length), item_body);
    if (!status) return status;
    Item item{};
    status = decode_item(item_body, item);
    if (!status) return status;
    items.push_back(std::move(item));
  }
  if (!family.at_end()) {
    return fail(ErrorCode::corruption, what, " has trailing bytes");
  }
  out = std::move(items);
  return Status::success();
}

Status decode_document(std::string_view payload, DecodedModel& model) {
  Decoder document(payload);
  Status status = decode_family(document, kTagNodes, Limits::kMaxNodes, "a node family",
                                decode_node, model.nodes);
  if (!status) return status;
  status = decode_family(document, kTagClaims, Limits::kMaxClaims, "a claim family", decode_claim,
                         model.claims);
  if (!status) return status;
  status = decode_family(document, kTagReservations, Limits::kMaxReservations,
                         "a reservation family", decode_reservation, model.reservations);
  if (!status) return status;
  status = decode_family(document, kTagExclusions, Limits::kMaxExclusions, "an exclusion family",
                         decode_exclusion, model.exclusions);
  if (!status) return status;
  status = decode_family(document, kTagClearances, Limits::kMaxClearances, "a clearance family",
                         decode_clearance, model.clearances);
  if (!status) return status;
  status = decode_family(document, kTagExpansionZones, Limits::kMaxExpansionZones,
                         "an expansion zone family", decode_expansion_zone, model.expansion_zones);
  if (!status) return status;
  if (!document.at_end()) {
    return Status::failure(ErrorCode::corruption, "the document carries trailing bytes");
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Text rendering
// ---------------------------------------------------------------------------

// A quoted value, or "-" for an absent one. Free text that reached a record
// through its parser cannot contain a control byte, so quoting cannot fail
// here; the fallback keeps the renderer total rather than throwing.
std::string quoted_or_dash(std::string_view text) {
  if (text.empty()) return "-";
  Result<std::string> quoted = quote_text(text);
  if (!quoted) return "-";
  return std::move(quoted).value();
}

std::string render_bool(bool value) { return value ? std::string("true") : std::string("false"); }

template <typename Ref>
std::string render_ref(const Ref& ref) {
  std::string out(registry_kind_name(Ref::registry_kind));
  out.push_back(':');
  out.append(quoted_or_dash(ref.id().value()));
  out.push_back('@');
  out.append(to_text(ref.generation().value()));
  out.push_back('#');
  out.append(to_text(ref.source_revision()));
  out.push_back('!');
  out.append(upstream_state_name(ref.state()));
  if (ref.has_successor()) {
    out.append(">");
    out.append(quoted_or_dash(ref.successor().value()));
  }
  return out;
}

template <typename Ref>
std::string render_optional_ref(const std::optional<Ref>& ref) {
  if (!ref.has_value()) return "-";
  return render_ref(*ref);
}

template <typename Ref>
std::string render_ref_set(const RefSet<Ref>& set) {
  if (set.empty()) return "-";
  std::string out;
  for (const Ref& ref : set.refs()) {
    if (!out.empty()) out.push_back(',');
    out.append(render_ref(ref));
  }
  return out;
}

std::string render_evidence_set(const EvidenceSet& set) {
  if (set.empty()) return "-";
  std::string out;
  for (const EvidenceRef& ref : set.refs()) {
    if (!out.empty()) out.push_back(',');
    out.append(registry_kind_name(ref.source));
    out.push_back(':');
    out.append(quoted_or_dash(ref.id.value()));
    out.push_back('@');
    out.append(to_text(ref.generation.value()));
    out.push_back('!');
    out.append(to_text(ref.observed_at.incarnation().value()));
    out.push_back('/');
    out.append(to_text(ref.observed_at.sequence()));
  }
  return out;
}

std::string render_scope(const FootprintScope& scope) {
  std::string out(footprint_scope_kind_name(scope.kind));
  if (!scope.rects.empty()) {
    out.push_back(':');
    out.append(to_text(scope.rects));
  }
  if (!scope.units.empty()) {
    out.push_back(':');
    out.append(to_text(scope.units));
  }
  return out;
}

std::string render_mask(OccupantMask mask) {
  if (mask.empty()) return "none";
  std::string out;
  for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(kOccupantKindCount); ++index) {
    const auto kind = static_cast<OccupantKind>(index);
    if (!mask.contains(kind)) continue;
    if (!out.empty()) out.push_back('+');
    out.append(occupant_kind_name(kind));
  }
  // A mask is a portable 32-bit set, so a bit above the known kinds is
  // reported rather than dropped.
  for (std::uint32_t bit = static_cast<std::uint32_t>(kOccupantKindCount); bit < 32u; ++bit) {
    if ((mask.bits() & (std::uint32_t{1} << bit)) == 0u) continue;
    if (!out.empty()) out.push_back('+');
    out.append("bit");
    out.append(to_text(static_cast<std::uint64_t>(bit)));
  }
  return out;
}

std::string render_optional_timestamp(const std::optional<Timestamp>& value) {
  return render_optional(value, [](Timestamp stamp) { return to_text(stamp); });
}

void append_field(std::string& line, std::string_view key, std::string_view value) {
  line.push_back(' ');
  line.append(key);
  line.push_back('=');
  line.append(value);
}

std::string render_identity_or_dash(std::string_view identity) {
  if (identity.empty()) return "-";
  return std::string(identity);
}

std::string render_rect_or_dash(bool present, const PlanarRect& rect) {
  if (!present) return "-";
  return to_text(rect);
}

std::string render_interval_or_dash(bool present, const RackUnitInterval& interval) {
  if (!present) return "-";
  return to_text(interval);
}

std::string render_interval_set_or_dash(bool present, const IntervalSet& set) {
  if (!present) return "-";
  return to_text(set);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

std::string encode_payload(const Snapshot::BuildInput& input) {
  std::string out;
  encode_family(out, kTagNodes, input.nodes, encode_node);
  encode_family(out, kTagClaims, input.claims, encode_claim);
  encode_family(out, kTagReservations, input.reservations, encode_reservation);
  encode_family(out, kTagExclusions, input.exclusions, encode_exclusion);
  encode_family(out, kTagClearances, input.clearances, encode_clearance);
  encode_family(out, kTagExpansionZones, input.expansion_zones, encode_expansion_zone);
  return out;
}

Status decode_payload(std::string_view payload, Snapshot::BuildInput& input) {
  DecodedModel model;
  const Status status = decode_document(payload, model);
  if (!status) return status;
  // The payload carries records, not the store framing around them, so the
  // store identity, incarnation, revision, attempt and creation stamp of the
  // caller's model are left exactly as they were.
  input.nodes = std::move(model.nodes);
  input.claims = std::move(model.claims);
  input.reservations = std::move(model.reservations);
  input.exclusions = std::move(model.exclusions);
  input.clearances = std::move(model.clearances);
  input.expansion_zones = std::move(model.expansion_zones);
  return Status::success();
}

std::string render_model_text(const Snapshot& snapshot) {
  std::string out;
  out.append("spacecap/1\n");
  for (const SpaceNode& node : snapshot.nodes()) {
    out.append(render_node(node));
    out.push_back('\n');
  }
  for (const OccupancyClaim& claim : snapshot.claims()) {
    out.append(render_claim(claim));
    out.push_back('\n');
  }
  for (const FootprintReservation& reservation : snapshot.reservations()) {
    out.append(render_reservation(reservation));
    out.push_back('\n');
  }
  for (const ExclusionRegion& region : snapshot.exclusions()) {
    out.append(render_exclusion(region));
    out.push_back('\n');
  }
  for (const ClearanceConstraint& constraint : snapshot.clearances()) {
    out.append(render_clearance(constraint));
    out.push_back('\n');
  }
  for (const ExpansionZone& zone : snapshot.expansion_zones()) {
    out.append(render_expansion_zone(zone));
    out.push_back('\n');
  }
  out.append("digest sha256:");
  out.append(snapshot.digest().hex());
  out.push_back('\n');
  return out;
}

std::string render_node(const SpaceNode& node) {
  std::string line = "node ";
  line.append(node.id.value());
  append_field(line, "gen", to_text(node.generation.value()));
  append_field(line, "kind", space_node_kind_name(node.kind));
  append_field(line, "class", spatial_class_name(node.spatial_class));
  append_field(line, "lifecycle", node_lifecycle_name(node.lifecycle));
  append_field(line, "depth", to_text(static_cast<std::uint64_t>(node.depth)));
  append_field(line, "parent", render_identity_or_dash(node.parent.value()));
  append_field(line, "label", quoted_or_dash(node.label.value()));
  append_field(line, "note", quoted_or_dash(node.note.value()));
  append_field(line, "base-rect",
               render_rect_or_dash(node.placement.has_base_rect, node.placement.base_rect));
  append_field(line, "u-span",
               render_interval_or_dash(node.placement.has_u_span, node.placement.u_span));
  append_field(line, "area", to_text(node.own_planar.declared_area));
  append_field(line, "rects", to_text(node.own_planar.rects));
  append_field(line, "rack-height", to_text(node.own_rack.height));
  append_field(line, "location", render_optional_ref(node.location));
  append_field(line, "facility-node", render_optional_ref(node.facility_node));
  append_field(line, "rack", render_optional_ref(node.rack));
  append_field(line, "assets", render_ref_set(node.assets));
  append_field(line, "policies", render_ref_set(node.policies));
  append_field(line, "evidence", render_evidence_set(node.evidence));
  append_field(line, "replaced-by", render_identity_or_dash(node.replaced_by.value()));
  append_field(line, "replaces", render_identity_or_dash(node.replaces.value()));
  return line;
}

std::string render_claim(const OccupancyClaim& claim) {
  std::string line = "claim ";
  line.append(claim.id.value());
  append_field(line, "gen", to_text(claim.generation.value()));
  append_field(line, "node", render_identity_or_dash(claim.node.value()));
  append_field(line, "scope", render_scope(claim.scope));
  append_field(line, "state", claim_state_name(claim.state));
  append_field(line, "occupant", occupant_kind_name(claim.occupant));
  append_field(line, "label", quoted_or_dash(claim.label.value()));
  append_field(line, "note", quoted_or_dash(claim.note.value()));
  append_field(line, "rack", render_optional_ref(claim.rack));
  append_field(line, "assets", render_ref_set(claim.assets));
  append_field(line, "policies", render_ref_set(claim.policies));
  append_field(line, "evidence", render_evidence_set(claim.evidence));
  append_field(line, "from-reservation", render_optional_ref(claim.from_reservation));
  return line;
}

std::string render_reservation(const FootprintReservation& reservation) {
  std::string line = "reservation ";
  line.append(reservation.id.value());
  append_field(line, "gen", to_text(reservation.generation.value()));
  append_field(line, "node", render_identity_or_dash(reservation.node.value()));
  append_field(line, "scope", render_scope(reservation.scope));
  append_field(line, "state", reservation_state_name(reservation.state));
  append_field(line, "reservation", render_ref(reservation.reservation));
  append_field(line, "holder-label", quoted_or_dash(reservation.holder_label.value()));
  append_field(line, "holder-assets", render_ref_set(reservation.holder_assets));
  append_field(line, "policies", render_ref_set(reservation.policies));
  append_field(line, "evidence", render_evidence_set(reservation.evidence));
  append_field(line, "not-after", render_optional_timestamp(reservation.not_after));
  return line;
}

std::string render_exclusion(const ExclusionRegion& region) {
  std::string line = "exclusion ";
  line.append(region.id.value());
  append_field(line, "gen", to_text(region.generation.value()));
  append_field(line, "node", render_identity_or_dash(region.node.value()));
  append_field(line, "scope", render_scope(region.scope));
  append_field(line, "reason", exclusion_reason_name(region.reason));
  append_field(line, "state", exclusion_state_name(region.state));
  append_field(line, "blocks", render_mask(region.blocks));
  append_field(line, "label", quoted_or_dash(region.label.value()));
  append_field(line, "note", quoted_or_dash(region.note.value()));
  append_field(line, "policies", render_ref_set(region.policies));
  append_field(line, "subjects", render_ref_set(region.subjects));
  append_field(line, "evidence", render_evidence_set(region.evidence));
  return line;
}

std::string render_clearance(const ClearanceConstraint& constraint) {
  std::string line = "clearance ";
  line.append(constraint.id.value());
  append_field(line, "gen", to_text(constraint.generation.value()));
  append_field(line, "node", render_identity_or_dash(constraint.node.value()));
  append_field(line, "kind", clearance_kind_name(constraint.kind));
  append_field(line, "band", render_rect_or_dash(constraint.has_band, constraint.band));
  append_field(line, "units",
               render_interval_set_or_dash(constraint.has_units, constraint.required_free_units));
  append_field(line, "enforceable", render_bool(constraint.enforceable));
  append_field(line, "label", quoted_or_dash(constraint.label.value()));
  append_field(line, "note", quoted_or_dash(constraint.note.value()));
  append_field(line, "policies", render_ref_set(constraint.policies));
  append_field(line, "evidence", render_evidence_set(constraint.evidence));
  return line;
}

std::string render_expansion_zone(const ExpansionZone& zone) {
  std::string line = "expansion-zone ";
  line.append(zone.id.value());
  append_field(line, "gen", to_text(zone.generation.value()));
  append_field(line, "node", render_identity_or_dash(zone.node.value()));
  append_field(line, "scope", render_scope(zone.scope));
  append_field(line, "state", expansion_state_name(zone.state));
  append_field(line, "label", quoted_or_dash(zone.label.value()));
  append_field(line, "note", quoted_or_dash(zone.note.value()));
  append_field(line, "policies", render_ref_set(zone.policies));
  append_field(line, "evidence", render_evidence_set(zone.evidence));
  append_field(line, "target-ready-by", render_optional_timestamp(zone.target_ready_by));
  return line;
}

}  // namespace dccp::space_capacity::internal
