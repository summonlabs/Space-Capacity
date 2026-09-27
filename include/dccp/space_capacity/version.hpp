// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - product, format and model version constants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "dccp/space_capacity/export.hpp"

namespace dccp::space_capacity {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kLibraryVersion = "1.0.0";
inline constexpr std::string_view kLibraryName = "space_capacity";
inline constexpr std::string_view kProductName = "Space Capacity";

// DCCP placement. Reported by the CLI and stored so an operator can tell which
// runtime wrote a store.
inline constexpr std::string_view kDccpTrancheName = "Tranche 2 - Facility Capacity and Placement";
inline constexpr int kDccpRepositoryIndex = 11;

// Durable store identification. The banner is the first line of the canonical
// document the store writes, and the format version is the second field of the
// binary frame. A reader refuses a banner or a version it does not implement.
inline constexpr std::string_view kStoreMagic = "SPCSTAT";
inline constexpr std::string_view kDocumentBanner = "spacecap/1";
inline constexpr std::string_view kHeadBanner = "spacecap-head/1";
inline constexpr std::string_view kIdentityBanner = "spacecap-identity/1";
inline constexpr std::uint32_t kStoreFormatVersion = 1;

// Semantic model revision. Bumped whenever the meaning of a persisted field
// changes in a way a reader of the same format version could misread. Readers
// refuse a store whose model revision they do not implement.
inline constexpr std::uint32_t kModelRevision = 1;

// Rack units per mount slot, matching the DCCP rack coordinate model. Space
// Capacity stores whole rack units and never derives them from slots; the
// constant is recorded in the store so a reader can prove the coordinate model
// it was written under.
inline constexpr std::uint32_t kMountSlotsPerRackUnit = 2;

// Human-readable version string, e.g. "1.0.0".
SC_API std::string_view version_string() noexcept;

// Long form, e.g. "Space Capacity 1.0.0 (DCCP Tranche 2, repository 11)".
SC_API std::string_view version_banner() noexcept;

}  // namespace dccp::space_capacity
