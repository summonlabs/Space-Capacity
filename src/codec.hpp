// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal canonical codec. Not installed, not part of the public API.
//
// The payload is a byte-exact, tag-length-value document. It is deterministic:
// every collection is written in canonical order, every integer has exactly
// one encoding, and every optional field is written either as present-with-
// value or as an explicit absence marker. Decoding is strict: an unknown tag,
// a non-minimal integer, a length that exceeds its container, a repeated
// field, a missing required field and trailing bytes are all refused with a
// specific code, and never repaired.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/snapshot.hpp"

namespace dccp::space_capacity::internal {

// Serializes the records of a model into the payload of a state image.
std::string encode_payload(const Snapshot::BuildInput& input);

// Parses a payload. `input` is overwritten only on success, so a failed decode
// never yields a half-populated model.
Status decode_payload(std::string_view payload, Snapshot::BuildInput& input);

// Canonical text rendering of a whole snapshot, used by the CLI's `show`
// command and by tests that compare two states as text.
std::string render_model_text(const Snapshot& snapshot);

// Canonical text rendering of one record family, used by diff output.
std::string render_node(const SpaceNode& node);
std::string render_claim(const OccupancyClaim& claim);
std::string render_reservation(const FootprintReservation& reservation);
std::string render_exclusion(const ExclusionRegion& region);
std::string render_clearance(const ClearanceConstraint& constraint);
std::string render_expansion_zone(const ExpansionZone& zone);

}  // namespace dccp::space_capacity::internal
