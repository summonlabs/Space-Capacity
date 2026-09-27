// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - hard bounds on every externally supplied or persisted size.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every quantity that can arrive from a file, a command line or a caller is
// bounded here before it is used to size an allocation, a loop or a container.
// Nothing in this library allocates from an unvalidated length.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dccp/space_capacity/export.hpp"

namespace dccp::space_capacity {

struct Limits final {
  // ---------------------------------------------------------------- identity
  // Space Capacity identity syntax is the DCCP identifier syntax: 1..128
  // bytes, first and last character alphanumeric, interior characters from
  // [0-9A-Za-z._:-]. Upstream registry references are opaque and wider,
  // because the sibling registries disagree about their own caps: Physical
  // Location Registry and Facility Topology allow 128 bytes, Asset Registry
  // allows 192. The wider bound is the one that accepts every sibling value.
  static constexpr std::size_t kMaxIdentifierBytes = 128;
  static constexpr std::size_t kMaxReferenceBytes = 192;
  static constexpr std::size_t kMaxLabelBytes = 128;
  static constexpr std::size_t kMaxNoteBytes = 512;
  static constexpr std::size_t kMaxActorBytes = 64;
  static constexpr std::size_t kMaxSourceBytes = 64;
  static constexpr std::size_t kMaxReasonBytes = 512;

  // ------------------------------------------------------------- containment
  // Containment depth is bounded so that a persisted parent chain cannot be
  // turned into unbounded work or unbounded recursion. The upstream depths
  // differ: Physical Location Registry allows up to 128 address components and
  // Facility Topology pins its structural depth at 5. Space Capacity models
  // five physical levels below the root plus rack unit bands, and bounds the
  // walk at eight so that a skipped level costs nothing.
  static constexpr std::uint32_t kMaxContainmentDepth = 8;
  static constexpr std::uint32_t kMaxNodes = 200000;
  static constexpr std::uint32_t kMaxChildrenPerNode = 20000;
  static constexpr std::uint32_t kMaxClaims = 500000;
  static constexpr std::uint32_t kMaxReservations = 100000;
  static constexpr std::uint32_t kMaxExclusions = 100000;
  static constexpr std::uint32_t kMaxClearances = 100000;
  static constexpr std::uint32_t kMaxExpansionZones = 50000;

  // A single record may carry at most this many disjoint extents. Fixed, so
  // that a persisted extent list has a hard allocation ceiling.
  static constexpr std::uint32_t kMaxExtentsPerRecord = 64;

  // ------------------------------------------------------------ refs/evidence
  static constexpr std::uint32_t kMaxReferencesPerRecord = 32;
  static constexpr std::uint32_t kMaxEvidencePerRecord = 32;
  static constexpr std::uint32_t kMaxExplanationsPerResult = 64;
  static constexpr std::uint32_t kMaxCandidatesPerReport = 64;

  // The planar fit search enumerates a candidate set derived from the plane
  // boundary and the obstacles. The product of the candidate x and y sets is
  // bounded here; a plane with more structure than this returns an
  // indeterminate verdict with a budget explanation rather than running for an
  // unbounded time, so a query can never be turned into a denial of service by
  // a large model.
  static constexpr std::uint64_t kMaxFitCandidatePairs = 1'000'000;

  // Bounded idempotency: the store retains this many recent request keys.
  static constexpr std::uint32_t kMaxIdempotencyRecords = 256;

  // ------------------------------------------------------------------ extents
  // Dimensions are exact integers. Planar distance is whole millimetres and
  // planar area is whole square millimetres. The bounds keep the area of the
  // largest representable rectangle and every intermediate product inside
  // int64 without overflow, and they are wide enough for a campus: 10 km on a
  // side, 100 km^2 of declared plane.
  //
  // Vertical space is whole rack units with a half-open interval convention:
  // the interval [first, last) contains first and excludes last. The hard
  // bound is a superset of every DCCP sibling cap, which are 512 in Physical
  // Location Registry and Asset Registry and 1024 in Rack Registry.
  static constexpr std::int64_t kMaxMillimeters = 10'000'000;
  static constexpr std::int64_t kMaxSquareMillimeters = 100'000'000'000'000;
  static constexpr std::int32_t kMaxRackUnits = 2048;
  static constexpr std::int32_t kMinRackUnits = 1;

  // ------------------------------------------------------------------- store
  // Whole-store envelope. A store larger than this is refused before any
  // buffer is allocated for it.
  static constexpr std::uint64_t kMaxStoreBytes = 1ull << 30;  // 1 GiB
  static constexpr std::uint64_t kMaxPayloadBytes = kMaxStoreBytes - (256ull << 10);
  static constexpr std::uint64_t kMaxDocumentBytes = kMaxPayloadBytes;
  static constexpr std::uint64_t kMaxStateFileBytes = kMaxStoreBytes;

  // Durable frame versions this build implements. A store written by a
  // different format version is refused before its payload is read.
  static constexpr std::uint32_t kMinFormatVersion = 1;
  static constexpr std::uint32_t kMaxFormatVersion = 1;

  // ------------------------------------------------------------------ counts
  static constexpr std::uint64_t kMaxCounter = 0xFFFF'FFFF'FFFF'FFFFull;
};

}  // namespace dccp::space_capacity
