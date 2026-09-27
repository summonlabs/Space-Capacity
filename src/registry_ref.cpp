// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - upstream registry references.
//
// The identity of another runtime is preserved byte for byte. The accepted
// character set is wider than Space Capacity's own identifier grammar because
// the DCCP siblings disagree about their own forms: Physical Location Registry
// and Facility Topology use 1..128-byte identifiers, Rack Registry uses a
// prefixed form such as "rack:a01", Asset Registry uses a 36-character UUID,
// and Asset Registry's own location and rack references allow 192 bytes.
//
// A value is accepted when it is 1..192 bytes of printable ASCII with no
// control byte. Nothing is normalised, trimmed, case-folded or re-encoded, and
// the empty string is not a reference.

#include "dccp/space_capacity/registry_ref.hpp"

#include <cstddef>
#include <string_view>

namespace dccp::space_capacity {

std::string_view registry_kind_name(RegistryKind kind) noexcept {
  switch (kind) {
    case RegistryKind::none:
      return "none";
    case RegistryKind::physical_location_registry:
      return "physical-location-registry";
    case RegistryKind::facility_topology:
      return "facility-topology";
    case RegistryKind::rack_registry:
      return "rack-registry";
    case RegistryKind::asset_registry:
      return "asset-registry";
    case RegistryKind::policy_registry:
      return "policy-registry";
    case RegistryKind::facility_capacity:
      return "facility-capacity";
    case RegistryKind::power_capacity:
      return "power-capacity";
    case RegistryKind::cooling_capacity:
      return "cooling-capacity";
    case RegistryKind::facility_capacity_reservation:
      return "facility-capacity-reservation";
    case RegistryKind::facility_placement_planner:
      return "facility-placement-planner";
  }
  return "unknown";
}

bool parse_registry_kind(std::string_view text, RegistryKind& out) noexcept {
  if (text == "none") {
    out = RegistryKind::none;
    return true;
  }
  if (text == "physical-location-registry") {
    out = RegistryKind::physical_location_registry;
    return true;
  }
  if (text == "facility-topology") {
    out = RegistryKind::facility_topology;
    return true;
  }
  if (text == "rack-registry") {
    out = RegistryKind::rack_registry;
    return true;
  }
  if (text == "asset-registry") {
    out = RegistryKind::asset_registry;
    return true;
  }
  if (text == "policy-registry") {
    out = RegistryKind::policy_registry;
    return true;
  }
  if (text == "facility-capacity") {
    out = RegistryKind::facility_capacity;
    return true;
  }
  if (text == "power-capacity") {
    out = RegistryKind::power_capacity;
    return true;
  }
  if (text == "cooling-capacity") {
    out = RegistryKind::cooling_capacity;
    return true;
  }
  if (text == "facility-capacity-reservation") {
    out = RegistryKind::facility_capacity_reservation;
    return true;
  }
  if (text == "facility-placement-planner") {
    out = RegistryKind::facility_placement_planner;
    return true;
  }
  return false;
}

std::string_view upstream_state_name(UpstreamState state) noexcept {
  switch (state) {
    case UpstreamState::unknown:
      return "unknown";
    case UpstreamState::active:
      return "active";
    case UpstreamState::degraded:
      return "degraded";
    case UpstreamState::retired:
      return "retired";
    case UpstreamState::replaced:
      return "replaced";
  }
  return "unknown";
}

bool parse_upstream_state(std::string_view text, UpstreamState& out) noexcept {
  if (text == "unknown") {
    out = UpstreamState::unknown;
    return true;
  }
  if (text == "active") {
    out = UpstreamState::active;
    return true;
  }
  if (text == "degraded") {
    out = UpstreamState::degraded;
    return true;
  }
  if (text == "retired") {
    out = UpstreamState::retired;
    return true;
  }
  if (text == "replaced") {
    out = UpstreamState::replaced;
    return true;
  }
  return false;
}

bool ExternalId::is_acceptable(std::string_view text) noexcept {
  if (text.empty()) return false;
  if (text.size() > Limits::kMaxReferenceBytes) return false;
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20 || byte > 0x7E) return false;
  }
  return true;
}

Result<ExternalId> ExternalId::parse(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::empty_value, "an upstream reference must not be empty");
  }
  if (text.size() > Limits::kMaxReferenceBytes) {
    return Error::make(ErrorCode::text_too_long,
                       "an upstream reference exceeds " +
                           std::to_string(Limits::kMaxReferenceBytes) + " bytes");
  }
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20 || byte > 0x7E) {
      return Error::make(ErrorCode::invalid_character,
                         "an upstream reference must be printable ASCII with no control byte");
    }
  }
  ExternalId id;
  id.value_.assign(text.data(), text.size());
  return id;
}

}  // namespace dccp::space_capacity
