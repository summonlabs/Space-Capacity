// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - public surface and lifecycle proof.
//
// This file includes ONLY the installed public header, the umbrella
// `dccp/space_capacity/space_capacity.hpp`. It never includes anything from
// `src/`, so it fails to compile the moment a public declaration depends on an
// internal one. It then runs a complete minimal lifecycle through the public
// API: open, mutate, observe, query, diff, persist, reopen, verify, close.

#include <cstdio>
#include <filesystem>
#include <string>

#include "dccp/space_capacity/space_capacity.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;

void remove_state(const std::string& state) {
  std::error_code error;
  std::filesystem::remove(state, error);
  std::filesystem::remove(state + ".prev", error);
  std::filesystem::remove(state + ".identity", error);
  std::filesystem::remove(state + ".lock", error);
}

}  // namespace

int main() {
  SC_CASE("version and boundary constants");
  SC_CHECK_EQ(kVersionMajor, 1);
  SC_CHECK_EQ(kVersionMinor, 0);
  SC_CHECK_EQ(kVersionPatch, 0);
  SC_CHECK_EQ(version_string(), std::string_view{"1.0.0"});
  SC_CHECK(version_banner().find("Tranche 2") != std::string_view::npos);
  SC_CHECK_EQ(kDccpRepositoryIndex, 11);
  SC_CHECK_EQ(kStoreFormatVersion, 1u);
  SC_CHECK_EQ(kModelRevision, 1u);
  SC_CHECK_EQ(std::string(identifier_syntax_help()),
              std::string("identifiers are 1..128 bytes, start and end with [0-9A-Za-z], and "
                          "contain only [0-9A-Za-z._:-]"));

  SC_CASE("every public enum name round-trips");
  {
    const SpaceNodeKind kinds[] = {SpaceNodeKind::site,    SpaceNodeKind::building,
                                   SpaceNodeKind::hall,    SpaceNodeKind::row,
                                   SpaceNodeKind::rack,    SpaceNodeKind::rack_unit_band,
                                   SpaceNodeKind::none};
    for (const SpaceNodeKind kind : kinds) {
      SpaceNodeKind parsed = SpaceNodeKind::none;
      SC_CHECK(parse_space_node_kind(space_node_kind_name(kind), parsed));
      SC_CHECK_EQ(static_cast<int>(parsed), static_cast<int>(kind));
    }
    const NodeLifecycle lifecycles[] = {
        NodeLifecycle::planned,       NodeLifecycle::provisioning, NodeLifecycle::available,
        NodeLifecycle::restricted,    NodeLifecycle::decommissioning,
        NodeLifecycle::retired,       NodeLifecycle::replaced};
    for (const NodeLifecycle value : lifecycles) {
      NodeLifecycle parsed = NodeLifecycle::planned;
      SC_CHECK(parse_node_lifecycle(node_lifecycle_name(value), parsed));
      SC_CHECK_EQ(static_cast<int>(parsed), static_cast<int>(value));
    }
    const ErrorCode codes[] = {ErrorCode::ok,
                               ErrorCode::invalid_argument,
                               ErrorCode::stale_generation,
                               ErrorCode::stale_revision,
                               ErrorCode::conflict,
                               ErrorCode::overlap,
                               ErrorCode::not_found,
                               ErrorCode::already_exists,
                               ErrorCode::incompatible_version,
                               ErrorCode::corruption,
                               ErrorCode::integrity_failure,
                               ErrorCode::limit_exceeded,
                               ErrorCode::unsupported,
                               ErrorCode::unavailable,
                               ErrorCode::unknown_state,
                               ErrorCode::permission_denied,
                               ErrorCode::io_failure,
                               ErrorCode::lock_conflict,
                               ErrorCode::invariant_violation};
    for (const ErrorCode code : codes) {
      ErrorCode parsed = ErrorCode::internal_error;
      SC_CHECK(parse_error_code(error_code_name(code), parsed));
      SC_CHECK_EQ(static_cast<int>(parsed), static_cast<int>(code));
    }
  }

  SC_CASE("minimal durable lifecycle through the public API");
  {
    const std::string state = "installed-api.spcstate";
    remove_state(state);

    StoreOptions options;
    options.path = state;
    options.store_identity = *StoreId::parse("installed-api-store");
    options.actor = "installed-api-test";
    options.source = "installed_api_test";

    std::string digest_before;
    {
      Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
      SC_CHECK(opened.ok());
      if (!opened) return ::sc_test::summary("installed_api_test");
      SpaceCapacityRegistry registry = std::move(opened).value();
      SC_CHECK(registry.durable());
      SC_CHECK_EQ(registry.revision().value(), 0ull);

      // A site root first: only a site may be a root, so a hall must be
      // contained. This mirrors the DCCP containment rules exactly.
      const SpaceNodeId root_id = *SpaceNodeId::parse("site-installed");
      {
        CreateNodeRequest root;
        root.node.id = root_id;
        root.node.generation = EntityGeneration{1};
        root.node.kind = SpaceNodeKind::site;
        root.node.spatial_class = SpatialClass::outdoor;
        root.node.lifecycle = NodeLifecycle::available;
        Result<MutationOutcome> created = registry.apply(root);
        SC_CHECK(created.ok());
        if (!created) return ::sc_test::summary("installed_api_test");
        SC_CHECK_EQ(created.value().revision.value(), 1ull);
      }

      CreateNodeRequest request;
      request.node.id = *SpaceNodeId::parse("hall-installed");
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::hall;
      request.node.spatial_class = SpatialClass::floor;
      request.node.lifecycle = NodeLifecycle::available;
      request.node.parent = root_id;
      request.node.depth = 1;
      request.node.own_planar.declared_area = SquareMillimeters{100000ll * 100000ll};
      request.node.own_planar.rects =
          *RectSet::build({PlanarRect::make(0, 0, 100000, 100000)});
      Result<MutationOutcome> outcome = registry.apply(request);
      SC_CHECK(outcome.ok());
      if (!outcome) return ::sc_test::summary("installed_api_test");
      SC_CHECK_EQ(outcome.value().revision.value(), 2ull);
      SC_CHECK(outcome.value().applied);

      const SpaceNodeId hall = request.node.id;
      NodeCapacity capacity = registry.snapshot()->capacity_of(hall, FitContext{});
      SC_CHECK_EQ(capacity.area.declared.value(), 10'000'000'000ll);
      SC_CHECK_EQ(capacity.area.available.value(), 10'000'000'000ll);

      // A fit query on an empty plane reports a placement and grants nothing.
      PlanarRectFitRequest fit;
      fit.node = hall;
      fit.width = Millimeters{1000};
      fit.depth = Millimeters{1000};
      Result<FitAssessment> assessment = registry.assess(fit);
      SC_CHECK(assessment.ok());
      if (assessment) {
        SC_CHECK_EQ(static_cast<int>(assessment.value().verdict),
                    static_cast<int>(FitVerdict::fits));
        SC_CHECK(!assessment.value().grants_placement());
        SC_CHECK(!assessment.value().candidates.empty());
      }

      // A token taken now is fresh, and the same token is stale after a change.
      ObservationToken token;
      token.subject = hall;
      token.subject_generation = EntityGeneration{1};
      token.revision = registry.revision();
      token.attempt = registry.snapshot()->attempt();
      Result<Revalidation> fresh = registry.revalidate(token);
      SC_CHECK(fresh.ok());
      if (fresh) SC_CHECK_EQ(static_cast<int>(fresh.value().freshness),
                             static_cast<int>(Freshness::fresh));

      SetNodeMetadataRequest rename;
      rename.node = hall;
      rename.label = *DisplayLabel::parse("renamed");
      SC_CHECK(registry.apply(rename).ok());

      Result<Revalidation> stale = registry.revalidate(token);
      SC_CHECK(stale.ok());
      if (stale) {
        SC_CHECK(!stale.value().valid);
        SC_CHECK_EQ(static_cast<int>(stale.value().freshness),
                    static_cast<int>(Freshness::stale));
      }

      digest_before = registry.snapshot()->digest().tagged_hex();
      SC_CHECK(registry.verify().ok());
      SC_CHECK(!digest_before.empty());
      (void)registry.close();
    }

    {
      Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
      SC_CHECK(reopened.ok());
      if (!reopened) return ::sc_test::summary("installed_api_test");
      SpaceCapacityRegistry registry = std::move(reopened).value();
      SC_CHECK_EQ(registry.snapshot()->digest().tagged_hex(), digest_before);
      SC_CHECK_EQ(registry.revision().value(), 3ull);
      SC_CHECK(registry.audit().ok());
      SC_CHECK_EQ(registry.snapshot()->node_count(), std::size_t{2});

      // Revision history is retained in process, not in the store, so after a
      // reopen only the current revision is addressable. A diff of a revision
      // against itself is empty; an earlier revision is refused rather than
      // approximated.
      Result<CapacityDiff> same = registry.diff(RegistryRevision{3}, RegistryRevision{3});
      SC_CHECK(same.ok());
      if (same) SC_CHECK_EQ(static_cast<int>(same.value().change_count()), 0);
      const Result<CapacityDiff> earlier =
          registry.diff(RegistryRevision{2}, RegistryRevision{3});
      SC_CHECK(!earlier.ok());
      SC_CHECK_EQ(static_cast<int>(earlier.error().code),
                  static_cast<int>(ErrorCode::stale_revision));

      const Result<StoreStatus> status = registry.store_status();
      SC_CHECK(status.ok());
      if (status) {
        SC_CHECK_EQ(status.value().revision.value(), 3ull);
        SC_CHECK(status.value().state_exists);
        SC_CHECK(status.value().identity_exists);
      }
      (void)registry.close();
    }
    remove_state(state);

    std::error_code error;
    SC_CHECK(!std::filesystem::exists(state, error));
    SC_CHECK(!std::filesystem::exists(state + ".identity", error));
  }

  SC_CASE("the in-memory registry is honest about not being durable");
  {
    Result<SpaceCapacityRegistry> created =
        SpaceCapacityRegistry::create_in_memory(*StoreId::parse("ephemeral"));
    SC_CHECK(created.ok());
    if (created) {
      SC_CHECK(!created.value().durable());
      SC_CHECK_EQ(static_cast<int>(created.value().verify().code()),
                  static_cast<int>(ErrorCode::unsupported));
      SC_CHECK_EQ(static_cast<int>(created.value().acquire_writer_lease().code()),
                  static_cast<int>(ErrorCode::unsupported));
    }
  }

  SC_CASE("a closed registry refuses work instead of pretending");
  {
    Result<SpaceCapacityRegistry> created =
        SpaceCapacityRegistry::create_in_memory(*StoreId::parse("closed"));
    SC_CHECK(created.ok());
    if (created) {
      SpaceCapacityRegistry registry = std::move(created).value();
      SC_CHECK(registry.close().ok());
      CreateNodeRequest request;
      request.node.id = *SpaceNodeId::parse("late");
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::site;
      request.node.spatial_class = SpatialClass::outdoor;
      request.node.lifecycle = NodeLifecycle::available;
      const Result<MutationOutcome> outcome = registry.apply(request);
      SC_CHECK(!outcome.ok());
      SC_CHECK_EQ(static_cast<int>(outcome.error().code),
                  static_cast<int>(ErrorCode::not_open));
    }
  }

  return ::sc_test::summary("installed_api_test");
}
