// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - umbrella header.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Space Capacity is DCCP Tranche 2, repository 11 of the canonical 72-runtime
// Data Center Control Plane. It owns physical-space capacity accounting across
// facility containment: containment hierarchy, spatial class, rack and
// rack-unit envelopes, occupancy claims, reserved footprint, expansion zones,
// incompatibility and exclusion regions, and service-clearance constraints,
// in exact integer units.
//
// It answers one question:
//
//   what physical space is genuinely available now, at which hierarchy level
//   and generation, after occupancy, incompatibilities, reserved footprint and
//   expansion constraints have been applied?
//
// It does not own, and does not act as, any of the following:
//
//   * Physical Location Registry  - location identity and addressing
//   * Facility Topology           - the facility containment graph
//   * Rack Registry               - rack composition and mounting occupancy
//   * Asset Registry              - asset identity and asset lifecycle
//   * Facility Capacity           - aggregate facility capacity
//   * Power Capacity, Cooling Capacity - power and thermal capacity
//   * Facility Capacity Reservation - future reservation authority
//   * Facility Placement Planner  - placement planning
//   * ASI                         - accelerator execution and workload scheduling
//   * DFI                         - network paths, transport and congestion
//
// It answers physical-fit and footprint-availability questions and never grants
// placement. Consumer runtimes retain the authority to decide where anything
// goes.

#pragma once

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/checked.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/diff.hpp"
#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/observation.hpp"
#include "dccp/space_capacity/query.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/registry.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/requests.hpp"
#include "dccp/space_capacity/snapshot.hpp"
#include "dccp/space_capacity/store.hpp"
#include "dccp/space_capacity/strong_id.hpp"
#include "dccp/space_capacity/text.hpp"
#include "dccp/space_capacity/units.hpp"
#include "dccp/space_capacity/version.hpp"
