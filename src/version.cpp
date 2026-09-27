// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - version constants.

#include "dccp/space_capacity/version.hpp"

namespace dccp::space_capacity {

std::string_view version_string() noexcept { return kLibraryVersion; }

std::string_view version_banner() noexcept {
  return "Space Capacity 1.0.0 (DCCP Tranche 2, repository 11)";
}

}  // namespace dccp::space_capacity
