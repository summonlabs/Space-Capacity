// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal diff construction. Not installed, not part of the public API.

#pragma once

#include "dccp/space_capacity/diff.hpp"
#include "dccp/space_capacity/snapshot.hpp"

namespace dccp::space_capacity::internal {

// Builds the complete diff between two snapshots. Records are compared by
// identity; a record present in both with any differing field becomes a
// `modified` change carrying the sorted names of the differing fields.
CapacityDiff compute_diff(const Snapshot& before, const Snapshot& after);

// Compares one pair of records of each family and reports the differing field
// names in canonical order. Exposed so the registry's explain command can name
// the fields without materialising a whole diff.
bool node_differs(const SpaceNode& a, const SpaceNode& b, std::vector<std::string>& fields);
bool claim_differs(const OccupancyClaim& a, const OccupancyClaim& b,
                   std::vector<std::string>& fields);
bool reservation_differs(const FootprintReservation& a, const FootprintReservation& b,
                         std::vector<std::string>& fields);
bool exclusion_differs(const ExclusionRegion& a, const ExclusionRegion& b,
                       std::vector<std::string>& fields);
bool clearance_differs(const ClearanceConstraint& a, const ClearanceConstraint& b,
                       std::vector<std::string>& fields);
bool expansion_zone_differs(const ExpansionZone& a, const ExpansionZone& b,
                            std::vector<std::string>& fields);

}  // namespace dccp::space_capacity::internal
