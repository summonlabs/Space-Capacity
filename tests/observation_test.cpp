// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - observations, freshness and authority.
//
// Proves the three things this library refuses to conflate: an observation is a
// fact about a moment, authority is carried by explicit preconditions, and
// freshness says whether an observation still describes what is committed now.
//
// Reopening a durable store is done here for real, in a temporary working
// directory: evidence observed before the reopen keeps its original attempt
// identity and is reported as recovered rather than fresh, and the registry's
// fresh floor moves to the attempt the store had reached. A stale generation or
// a stale revision is refused with the numbers that were compared, never merged
// and never partially applied, and an idempotent request key is meant to return
// the recorded outcome instead of applying a second time.

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::space_capacity;

SpaceNode make_node(const char* id_text, SpaceNodeKind kind, SpatialClass spatial_class,
                    const SpaceNodeId& parent, std::uint32_t depth, const char* label) {
  SpaceNode node;
  node.id = *SpaceNodeId::parse(id_text);
  node.generation = EntityGeneration{1};
  node.kind = kind;
  node.spatial_class = spatial_class;
  node.lifecycle = NodeLifecycle::available;
  node.label = *DisplayLabel::parse(label);
  node.parent = parent;
  node.depth = depth;
  return node;
}

Result<MutationOutcome> create(SpaceCapacityRegistry& registry, const SpaceNode& node) {
  CreateNodeRequest request;
  request.node = node;
  return registry.apply(request);
}

Result<MutationOutcome> set_rack_envelope(SpaceCapacityRegistry& registry, const SpaceNodeId& rack,
                                          std::int32_t height) {
  SetNodeEnvelopeRequest request;
  request.node = rack;
  RackEnvelope envelope;
  envelope.height = RackUnits{height};
  request.rack = envelope;
  return registry.apply(request);
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  SC_CASE("classify_evidence: above the floor is fresh, at or below it is recovered");
  {
    EvidenceRef observed;
    observed.source = RegistryKind::physical_location_registry;
    observed.id = *ExternalId::parse("loc-42");
    observed.generation = EntityGeneration{7};
    observed.observed_at = AttemptId{StoreIncarnation{1}, 5};

    // Observed above the floor: this session observed it.
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 4}) == Freshness::fresh);
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 0}) == Freshness::fresh);

    // At the floor and below it: read out of a store, not yet revalidated.
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 5}) ==
             Freshness::recovered);
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 6}) ==
             Freshness::recovered);

    // Evidence this store never observed cannot be fresh, whatever the floor.
    const EvidenceRef unobserved;
    SC_CHECK(unobserved.observed_at.is_zero());
    SC_CHECK(classify_evidence(unobserved, AttemptId{StoreIncarnation{1}, 0}) ==
             Freshness::recovered);
    SC_CHECK(classify_evidence(unobserved, AttemptId{StoreIncarnation{1}, 99}) ==
             Freshness::recovered);

    // Evidence from a different incarnation of the store is stale, never
    // recovered: it was not written by this store at all.
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{2}, 0}) == Freshness::stale);
    SC_CHECK(classify_evidence(observed, AttemptId{StoreIncarnation{9}, 100}) == Freshness::stale);

    // The classifier is a pure function of its two arguments.
    SC_CHECK_EQ(static_cast<int>(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 5})),
                static_cast<int>(classify_evidence(observed, AttemptId{StoreIncarnation{1}, 5})));
  }

  // -------------------------------------------------------------------------
  SC_CASE("a reopened store reports previously observed evidence as recovered");
  {
    StoreOptions options;
    options.path = "evidence-store.spcstate";
    options.store_identity = *StoreId::parse("evidence-store");

    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    SC_CHECK(opened.ok());
    if (!opened.ok()) return ::sc_test::summary("observation");
    SpaceCapacityRegistry registry = std::move(opened).value();
    SC_CHECK(registry.durable());
    SC_CHECK(registry.is_open());
    SC_CHECK_EQ(registry.revision().value(), 0ull);

    const AttemptId floor_at_first_open = registry.fresh_floor();
    SC_CHECK_EQ(floor_at_first_open.sequence(), 0ull);
    SC_CHECK_EQ(registry.store_id().str(), std::string("evidence-store"));

    // A first mutation, whose attempt identity this session observed.
    const Result<MutationOutcome> site = create(
        registry, make_node("obs-site", SpaceNodeKind::site, SpatialClass::outdoor,
                            SpaceNodeId{}, 0, "Observation site"));
    SC_CHECK(site.ok());
    const AttemptId observed_at = registry.attempt();
    SC_CHECK(observed_at > floor_at_first_open);

    EvidenceRef evidence;
    evidence.source = RegistryKind::physical_location_registry;
    evidence.id = *ExternalId::parse("loc-42");
    evidence.generation = EntityGeneration{7};
    evidence.observed_at = observed_at;
    SC_CHECK(evidence.observed());

    // A second mutation that carries the evidence on a real record.
    SpaceNode building = make_node("obs-bldg", SpaceNodeKind::building, SpatialClass::enclosed,
                                   *SpaceNodeId::parse("obs-site"), 1, "Observation building");
    building.evidence = *EvidenceSet::build({evidence});
    const Result<MutationOutcome> created = create(registry, building);
    SC_CHECK(created.ok());
    const AttemptId last_attempt = registry.attempt();
    const RegistryRevision last_revision = registry.revision();
    SC_CHECK(last_attempt > observed_at);

    SC_CHECK(registry.evidence_freshness(evidence) == Freshness::fresh);
    SC_CHECK(classify_evidence(evidence, registry.fresh_floor()) == Freshness::fresh);

    const Status closed = registry.close();
    SC_CHECK(closed.ok());

    // Reopen the same store: the state is loaded, not recreated.
    Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
    SC_CHECK(reopened.ok());
    if (!reopened.ok()) return ::sc_test::summary("observation");
    SpaceCapacityRegistry again = std::move(reopened).value();

    SC_CHECK_EQ(again.revision().value(), last_revision.value());
    SC_CHECK(again.attempt() == last_attempt);
    SC_CHECK_EQ(again.store_id().str(), std::string("evidence-store"));

    // The fresh floor advanced to the attempt the store had reached, and the
    // evidence that was fresh a moment ago is now recovered, not fresh.
    const AttemptId floor_after_reopen = again.fresh_floor();
    SC_CHECK(floor_after_reopen > floor_at_first_open);
    SC_CHECK(floor_after_reopen == last_attempt);
    SC_CHECK_EQ(floor_after_reopen.sequence(), last_attempt.sequence());
    SC_CHECK(again.evidence_freshness(evidence) == Freshness::recovered);
    SC_CHECK(again.evidence_freshness(evidence) != Freshness::fresh);

    // The evidence itself survived the store: same source, same identity, same
    // generation, same attempt.
    const SpaceNode* stored = again.snapshot()->find_node(*SpaceNodeId::parse("obs-bldg"));
    SC_CHECK(stored != nullptr);
    if (stored != nullptr) {
      SC_CHECK_EQ(stored->evidence.size(), static_cast<std::size_t>(1));
      SC_CHECK(stored->evidence.refs().at(0) == evidence);
      SC_CHECK(stored->evidence.all_observed());
    }

    // -----------------------------------------------------------------------
    SC_CASE("revalidate: fresh, stale generation, stale incarnation, orphaned, recovered");
    {
      ObservationToken token;
      token.subject = *SpaceNodeId::parse("obs-bldg");

      // A token taken from the state this session just loaded is recovered: its
      // attempt is AT the floor, and only an attempt above the floor is fresh.
      const SpaceNode* record = again.snapshot()->find_node(token.subject);
      SC_CHECK(record != nullptr);
      token.subject_generation = record->generation;
      token.revision = again.revision();
      token.attempt = again.fresh_floor();
      const Result<Revalidation> at_floor = again.revalidate(token);
      SC_CHECK(at_floor.ok());
      SC_CHECK(!at_floor.value().valid);
      SC_CHECK(at_floor.value().freshness == Freshness::recovered);
      SC_CHECK(at_floor.value().reason == ErrorCode::stale_authority);
      SC_CHECK(at_floor.value().generation_compared);
      SC_CHECK(at_floor.value().revision_compared);
      SC_CHECK_EQ(at_floor.value().expected_revision.value(), token.revision.value());
      SC_CHECK_EQ(at_floor.value().actual_revision.value(), again.revision().value());

      // One mutation after the reopen: the record's generation moves, and a
      // token taken now is above the floor and therefore fresh.
      SetNodeMetadataRequest rename;
      rename.node = token.subject;
      rename.label = *DisplayLabel::parse("Renamed after reopen");
      const Result<MutationOutcome> renamed = again.apply(rename);
      SC_CHECK(renamed.ok());

      const SpaceNode* moved_record = again.snapshot()->find_node(token.subject);
      SC_CHECK(moved_record != nullptr);
      SC_CHECK_EQ(moved_record->generation.value(), 2ull);

      ObservationToken fresh;
      fresh.subject = token.subject;
      fresh.subject_generation = moved_record->generation;
      fresh.revision = again.revision();
      fresh.attempt = again.attempt();
      SC_CHECK(fresh.attempt > again.fresh_floor());
      const Result<Revalidation> valid = again.revalidate(fresh);
      SC_CHECK(valid.ok());
      SC_CHECK(valid.value().valid);
      SC_CHECK(valid.value().ok());
      SC_CHECK(valid.value().freshness == Freshness::fresh);
      SC_CHECK(valid.value().reason == ErrorCode::ok);

      // The generation the observation was taken at is no longer the current
      // one: the token is refused, and both generations are reported.
      ObservationToken stale_generation = fresh;
      stale_generation.subject_generation = EntityGeneration{1};
      const Result<Revalidation> stale = again.revalidate(stale_generation);
      SC_CHECK(stale.ok());
      SC_CHECK(!stale.value().valid);
      SC_CHECK(stale.value().freshness == Freshness::stale);
      SC_CHECK(stale.value().reason == ErrorCode::stale_generation);
      SC_CHECK_EQ(stale.value().expected_generation.value(), 1ull);
      SC_CHECK_EQ(stale.value().actual_generation.value(), 2ull);
      SC_CHECK(stale.value().generation_compared);

      // A token from a previous incarnation of the store is stale too, but for
      // a different reason: the observation cannot be compared at all.
      ObservationToken other_incarnation = fresh;
      other_incarnation.attempt = AttemptId{StoreIncarnation{99}, 9};
      const Result<Revalidation> incarnation = again.revalidate(other_incarnation);
      SC_CHECK(incarnation.ok());
      SC_CHECK(!incarnation.value().valid);
      SC_CHECK(incarnation.value().freshness == Freshness::stale);
      SC_CHECK(incarnation.value().reason == ErrorCode::stale_incarnation);

      // A token older than the fresh floor is recovered evidence: it was
      // observed before this process opened the store.
      ObservationToken before_floor = fresh;
      before_floor.attempt = AttemptId{again.incarnation(), 1};
      SC_CHECK(before_floor.attempt < again.fresh_floor());
      const Result<Revalidation> recovered = again.revalidate(before_floor);
      SC_CHECK(recovered.ok());
      SC_CHECK(!recovered.value().valid);
      SC_CHECK(recovered.value().freshness == Freshness::recovered);
      SC_CHECK(recovered.value().reason == ErrorCode::stale_authority);

      // A token for a subject that is not in the model is orphaned.
      ObservationToken orphan = fresh;
      orphan.subject = *SpaceNodeId::parse("obs-ghost");
      const Result<Revalidation> gone = again.revalidate(orphan);
      SC_CHECK(gone.ok());
      SC_CHECK(!gone.value().valid);
      SC_CHECK(gone.value().freshness == Freshness::orphaned);
      SC_CHECK(gone.value().reason == ErrorCode::not_found);
      SC_CHECK(!gone.value().generation_compared);
    }

    // -----------------------------------------------------------------------
    SC_CASE("a stale generation precondition is refused and the revision does not move");
    {
      // A record that can be edited: a rack with a declared envelope.
      const SpaceNode rack = make_node("obs-rack", SpaceNodeKind::rack, SpatialClass::rack,
                                       *SpaceNodeId::parse("obs-site"), 1, "Observation rack");
      SC_CHECK(create(again, rack).ok());
      SC_CHECK(set_rack_envelope(again, rack.id, 42).ok());

      CreateClaimRequest create_claim;
      create_claim.claim.id = *OccupancyClaimId::parse("obs-claim-1");
      create_claim.claim.generation = EntityGeneration{1};
      create_claim.claim.node = rack.id;
      create_claim.claim.state = ClaimState::planned;
      create_claim.claim.occupant = OccupantKind::asset;
      create_claim.claim.scope.kind = FootprintScopeKind::rack_units;
      create_claim.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 2)});
      SC_CHECK(again.apply(create_claim).ok());

      const SnapshotPtr claim_snapshot = again.snapshot();
      const OccupancyClaim* claim = claim_snapshot->find_claim(*OccupancyClaimId::parse(
          "obs-claim-1"));
      SC_CHECK(claim != nullptr);
      SC_CHECK_EQ(claim->generation.value(), 1ull);

      // Two edits, both prepared against generation 1 of the same record. The
      // first one applies; the second must not.
      SetClaimDetailsRequest first;
      first.claim = *OccupancyClaimId::parse("obs-claim-1");
      first.precondition = Precondition::at_generation(EntityGeneration{1});
      first.label = *DisplayLabel::parse("first edit");
      const Result<MutationOutcome> applied = again.apply(first);
      SC_CHECK(applied.ok());
      SC_CHECK(applied.value().applied);
      const RegistryRevision after_first = again.revision();

      SetClaimDetailsRequest second;
      second.claim = *OccupancyClaimId::parse("obs-claim-1");
      second.precondition = Precondition::at_generation(EntityGeneration{1});
      second.label = *DisplayLabel::parse("second edit");
      const Result<MutationOutcome> refused = again.apply(second);
      SC_CHECK(!refused);
      if (!refused.ok()) {
        SC_CHECK(refused.error().code == ErrorCode::stale_generation);
        SC_CHECK(refused.error().has_numbers);
        SC_CHECK_EQ(refused.error().expected, 1ull);  // the generation the request named
        SC_CHECK_EQ(refused.error().actual, 2ull);    // the generation that is current
        SC_CHECK_EQ(refused.error().subject, std::string("obs-claim-1"));
      }

      // The refusal left the revision exactly where the first edit put it.
      SC_CHECK_EQ(again.revision().value(), after_first.value());
      const OccupancyClaim* stored_claim =
          again.snapshot()->find_claim(*OccupancyClaimId::parse("obs-claim-1"));
      SC_CHECK(stored_claim != nullptr);
      if (stored_claim != nullptr) {
        SC_CHECK_EQ(stored_claim->generation.value(), 2ull);
        SC_CHECK_EQ(stored_claim->label.str(), std::string("first edit"));
      }
    }

    // -----------------------------------------------------------------------
    SC_CASE("a stale revision precondition is refused and the model is unchanged");
    {
      const RegistryRevision current = again.revision();
      const Digest before = again.snapshot()->digest();

      SetNodeMetadataRequest request;
      request.node = *SpaceNodeId::parse("obs-rack");
      request.precondition = Precondition::at_revision(RegistryRevision{0});
      request.label = *DisplayLabel::parse("must not land");
      const Result<MutationOutcome> refused = again.apply(request);
      SC_CHECK(!refused);
      if (!refused.ok()) {
        SC_CHECK(refused.error().code == ErrorCode::stale_revision);
        SC_CHECK(refused.error().has_numbers);
        SC_CHECK_EQ(refused.error().expected, 0ull);
        SC_CHECK_EQ(refused.error().actual, current.value());
      }

      // The revision did not move and the canonical state is byte-identical.
      SC_CHECK_EQ(again.revision().value(), current.value());
      SC_CHECK(again.snapshot()->digest() == before);
      const SpaceNode* rack = again.snapshot()->find_node(*SpaceNodeId::parse("obs-rack"));
      SC_CHECK(rack != nullptr);
      if (rack != nullptr) {
        SC_CHECK(rack->label.str() != std::string("must not land"));
      }

      // The canonical bytes of the current revision hash to its own digest, so
      // "the model is unchanged" is a statement about the committed bytes.
      SC_CHECK(again.snapshot()->digest() ==
               digest_in_domain(kDomainStateBinary, again.snapshot()->canonical_bytes()));
    }

    // -----------------------------------------------------------------------
    SC_CASE("a mutation with no precondition is accepted and reports where it landed");
    {
      const RegistryRevision before = again.revision();
      const SpaceNode rack2 = make_node("obs-rack-2", SpaceNodeKind::rack, SpatialClass::rack,
                                        *SpaceNodeId::parse("obs-site"), 1, "Second rack");
      const Result<MutationOutcome> outcome = create(again, rack2);
      SC_CHECK(outcome.ok());
      if (outcome.ok()) {
        SC_CHECK(outcome.value().applied);
        SC_CHECK(!outcome.value().replayed());
        SC_CHECK_EQ(outcome.value().revision.value(), before.value() + 1);
        SC_CHECK_EQ(outcome.value().revision.value(), again.revision().value());
        SC_CHECK(outcome.value().subject == rack2.id);
        SC_CHECK(!outcome.value().state_digest.is_zero());
        SC_CHECK(outcome.value().state_digest == again.snapshot()->digest());
      }

      // A precondition that names the current revision is accepted and lands on
      // the next one.
      const RegistryRevision current = again.revision();
      SetNodeMetadataRequest request;
      request.node = rack2.id;
      request.precondition = Precondition::at_revision(current);
      request.label = *DisplayLabel::parse("fenced");
      const Result<MutationOutcome> fenced = again.apply(request);
      SC_CHECK(fenced.ok());
      if (fenced.ok()) {
        SC_CHECK(fenced.value().applied);
        SC_CHECK_EQ(fenced.value().revision.value(), current.value() + 1);
      }
    }

    // -----------------------------------------------------------------------
    SC_CASE("an idempotent request key replays instead of applying twice");
    {
      CreateNodeRequest keyed;
      keyed.node = make_node("obs-keyed-1", SpaceNodeKind::rack, SpatialClass::rack,
                             *SpaceNodeId::parse("obs-site"), 1, "Keyed rack");
      keyed.request_id = *RequestId::parse("obs-key-1");
      SC_CHECK(!keyed.request_id.empty());

      const Result<MutationOutcome> first = again.apply(keyed);
      SC_CHECK(first.ok());
      if (!first.ok()) return ::sc_test::summary("observation");
      SC_CHECK(first.value().applied);
      const RegistryRevision landed = again.revision();
      SC_CHECK_EQ(first.value().revision.value(), landed.value());

      // The same request object, the same key, the same base revision: the
      // recorded outcome is returned and nothing is applied a second time.
      //
      // The replay itself is asserted here as the documented contract of
      // RequestId and of MutationOutcome. Every one of these holds except the
      // recorded revision: the outcome handed back carries revision 0 and
      // attempt 0.0 rather than the revision and attempt the request actually
      // landed on, because the record keeps only its key and its base revision.
      const Result<MutationOutcome> replay = again.apply(keyed);
      SC_CHECK(replay.ok());
      if (replay.ok()) {
        SC_CHECK(!replay.value().applied);
        SC_CHECK(replay.value().replayed());
        SC_CHECK_EQ(replay.value().revision.value(), landed.value());
        SC_CHECK(replay.value().explanations.contains(ReasonCode::idempotent_replay));
      }
      SC_CHECK_EQ(again.revision().value(), landed.value());

      // The same key against a DIFFERENT base revision is a conflict, not a
      // replay and not a second application.
      const SpaceNode unrelated = make_node("obs-unrelated-1", SpaceNodeKind::rack,
                                            SpatialClass::rack,
                                            *SpaceNodeId::parse("obs-site"), 1, "Unrelated");
      SC_CHECK(create(again, unrelated).ok());
      SC_CHECK(again.revision().value() != landed.value());

      CreateNodeRequest reused;
      reused.node = make_node("obs-keyed-2", SpaceNodeKind::rack, SpatialClass::rack,
                              *SpaceNodeId::parse("obs-site"), 1, "Keyed rack two");
      reused.request_id = *RequestId::parse("obs-key-1");
      const Result<MutationOutcome> conflict = again.apply(reused);
      SC_CHECK(!conflict);
      if (!conflict.ok()) {
        SC_CHECK(conflict.error().code == ErrorCode::operation_id_conflict);
      }
      SC_CHECK(again.snapshot()->find_node(*SpaceNodeId::parse("obs-keyed-2")) == nullptr);
    }

    // -----------------------------------------------------------------------
    SC_CASE("a request with no key is not deduplicated");
    {
      CreateNodeRequest unkeyed;
      unkeyed.node = make_node("obs-unkeyed-1", SpaceNodeKind::rack, SpatialClass::rack,
                               *SpaceNodeId::parse("obs-site"), 1, "Unkeyed rack");
      SC_CHECK(unkeyed.request_id.empty());

      const Result<MutationOutcome> first = again.apply(unkeyed);
      SC_CHECK(first.ok());
      if (first.ok()) {
        SC_CHECK(first.value().applied);
      }

      // The same request again: with no key there is nothing to recognise it
      // by, so the identity collision is reported instead.
      const Result<MutationOutcome> second = again.apply(unkeyed);
      SC_CHECK(!second);
      if (!second.ok()) {
        SC_CHECK(second.error().code == ErrorCode::already_exists);
      }
    }

    SC_CHECK(again.close().ok());
  }

  return ::sc_test::summary("observation");
}
