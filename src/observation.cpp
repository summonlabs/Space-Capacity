// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - freshness classification.

#include "dccp/space_capacity/observation.hpp"

#include <string_view>

namespace dccp::space_capacity {

std::string_view freshness_name(Freshness value) noexcept {
  switch (value) {
    case Freshness::fresh:
      return "fresh";
    case Freshness::recovered:
      return "recovered";
    case Freshness::stale:
      return "stale";
    case Freshness::superseded:
      return "superseded";
    case Freshness::orphaned:
      return "orphaned";
  }
  return "unknown";
}

bool parse_freshness(std::string_view text, Freshness& out) noexcept {
  if (text == "fresh") {
    out = Freshness::fresh;
    return true;
  }
  if (text == "recovered") {
    out = Freshness::recovered;
    return true;
  }
  if (text == "stale") {
    out = Freshness::stale;
    return true;
  }
  if (text == "superseded") {
    out = Freshness::superseded;
    return true;
  }
  if (text == "orphaned") {
    out = Freshness::orphaned;
    return true;
  }
  return false;
}

Freshness classify_evidence(const EvidenceRef& evidence, AttemptId fresh_floor) noexcept {
  if (evidence.observed_at.is_zero()) {
    // Evidence that this store never observed cannot be fresh. It is reported
    // as recovered rather than as fresh, so it can never support a new
    // authoritative statement without an explicit revalidation.
    return Freshness::recovered;
  }
  if (evidence.observed_at.incarnation() != fresh_floor.incarnation()) {
    return Freshness::stale;
  }
  return evidence.observed_at <= fresh_floor ? Freshness::recovered : Freshness::fresh;
}

}  // namespace dccp::space_capacity
