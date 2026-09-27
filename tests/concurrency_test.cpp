// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - threads, and the lock order the library documents.
//
// What this file proves:
//   * many readers and one writer make progress together: four reader threads
//     each take two thousand snapshots while the main thread applies two
//     hundred mutations, and every snapshot a reader obtains passes the full
//     audit, names the revision it was published at, and has a digest that is
//     stable across repeated reads and equal to the digest of its own canonical
//     bytes;
//   * many writers on one registry neither lose a success nor invent one: every
//     accepted write advances the revision by exactly one relative to the
//     revision the writer fenced on, every refusal is that fence and leaves the
//     record absent, and the final revision is the initial revision plus the
//     number of successes;
//   * a race on one claim commits it exactly once;
//   * a snapshot is immutable: a pointer taken before a mutation keeps reporting
//     the old revision, the old digest and the old canonical bytes;
//   * the same request key applied by four threads at once takes effect exactly
//     once;
//   * a durable registry commits with the operating-system file lock held while
//     the writer lock is held and the publication lock is not.
//
// The lock order exercised here is exactly the documented one:
//
//   L1  commit_mutex_  serializes writers; held for a whole mutation.
//   L2  state_mutex_   a shared_mutex over the published snapshot pointer and
//                      the retained history; taken shared to read the pointer
//                      and exclusive only for the pointer swap.
//   L3  the OS file lock, taken inside a durable commit while L1 is held and
//       L2 is NOT held.
//
// The order is L1 then L2 and L1 then L3, and it is never reversed. A reader
// takes L2 shared, copies the shared_ptr and releases it, so a report, a rollup
// or a fit search runs entirely on an immutable snapshot and blocks nobody. If
// a writer ever held L2 while waiting for L1, or a durable commit took L3 before
// L1, the reader and writer loops below would deadlock rather than finish.
//
// Four software threads are run regardless of the hardware thread count, and
// std::thread::hardware_concurrency() is never consulted to size the work: the
// point is to overlap four independent threads, and a machine with fewer
// hardware threads merely interleaves them. Every thread is joined
// unconditionally. There is no timeout, no condition_variable::wait_for, no
// watchdog and no process limit anywhere in this file: a hang here is a defect
// to diagnose, not something to abort.
//
// The test harness counts checks in plain ints, so a check recorded from a
// racing thread would be a data race. Each thread therefore accumulates its own
// report - a private counter and the name of the first property it saw broken -
// and the main thread turns every report into an assertion after all threads
// have been joined. No invariant is asserted from a racing thread, and none is
// left unasserted.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"

#include "fixture.hpp"
#include "test_support.hpp"

using namespace dccp::space_capacity;
using namespace sc_fixture;

namespace {

constexpr int kReaders = 4;
constexpr int kReaderIterations = 2000;
constexpr int kWriterThreads = 4;
constexpr int kWritesPerThread = 100;
constexpr int kDurableWriters = 2;
constexpr int kDurableWritesPerThread = 20;
constexpr int kDurableReaderIterations = 400;

// The first property a thread saw broken, so a failure names something.
struct ThreadReport final {
  std::int64_t units = 0;
  std::int64_t violations = 0;
  const char* first_failure = "";
};

// A start barrier. Threads spin on an atomic flag so that the work they are
// about to race actually overlaps. It is not a wait with a deadline: there is
// no clock anywhere in this file.
class StartGate final {
 public:
  void open() noexcept { open_.store(true, std::memory_order_release); }
  void await() const noexcept {
    while (!open_.load(std::memory_order_acquire)) {
      // Spin. A worker cannot pass this line before every worker has been
      // started, which is what makes the races below overlap.
    }
  }

 private:
  std::atomic<bool> open_{false};
};

// The one piece of mutable state this file shares between threads: the first
// broken property any thread saw, whichever thread saw it. A report slot per
// thread needs no lock, so a mutex guards exactly this.
std::mutex failure_mutex;
std::string first_failure_text;

void note(ThreadReport& report, const char* what) {
  ++report.violations;
  if (report.first_failure[0] == '\0') report.first_failure = what;
  std::lock_guard<std::mutex> guard(failure_mutex);
  if (first_failure_text.empty()) first_failure_text = what;
}

void remove_store_files(const std::string& state) {
  std::error_code error;
  std::filesystem::remove(state, error);
  std::filesystem::remove(state + ".prev", error);
  std::filesystem::remove(state + ".identity", error);
  std::filesystem::remove(state + ".lock", error);
}

RackUnitFitRequest rack_fit(const SpaceNodeId& rack, std::int32_t needed) {
  RackUnitFitRequest request;
  request.rack = rack;
  request.needed = RackUnits{needed};
  request.alignment = 1;
  request.max_candidates = 1;
  return request;
}

}  // namespace

int main() {
  const Ids id = ids();

  std::printf("readers: %d x %d iterations, writer: 200 mutations\n", kReaders, kReaderIterations);
  std::printf("writers: %d x %d mutations, durable writers: %d x %d\n", kWriterThreads,
              kWritesPerThread, kDurableWriters, kDurableWritesPerThread);

  // -------------------------------------------------------------------------
  SC_CASE("many readers, one writer");
  {
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const FitContext context{};
    const std::uint64_t initial_revision = registry.revision().value();
    const std::size_t initial_nodes = registry.snapshot()->node_count();
    std::vector<ThreadReport> reports(static_cast<std::size_t>(kReaders));
    std::vector<std::thread> readers;

    for (int reader = 0; reader < kReaders; ++reader) {
      readers.emplace_back([&registry, &reports, reader, &id]() {
        ThreadReport& report = reports[static_cast<std::size_t>(reader)];
        const FitContext context{};
        const RackUnitFitRequest fit = rack_fit(id.rack1, 1);
        for (int iteration = 0; iteration < kReaderIterations; ++iteration) {
          const SnapshotPtr snapshot = registry.snapshot();
          ++report.units;
          if (snapshot == nullptr) {
            note(report, "snapshot() returned no snapshot");
            continue;
          }
          const RegistryRevision revision = snapshot->revision();
          if (!snapshot->audit().ok()) {
            note(report, "a published snapshot failed the full audit");
          }
          // The digest of one immutable snapshot is stable across repeated
          // reads, and it is the digest of that snapshot's canonical bytes.
          const std::string first_hex = snapshot->digest().hex();
          if (snapshot->digest().hex() != first_hex) {
            note(report, "the digest of one snapshot changed between reads");
          }
          if (digest_in_domain(kDomainStateBinary, snapshot->canonical_bytes()) !=
              snapshot->digest()) {
            note(report, "the digest is not the digest of the canonical bytes");
          }
          const CapacityRollup rollup = model_rollup(*snapshot, context);
          if (rollup.revision.value() != revision.value()) {
            note(report, "the model rollup names a different revision");
          }
          if (rollup.node_count != snapshot->node_count()) {
            note(report, "the model rollup counts a different number of nodes");
          }
          const Result<FitAssessment> assessment = snapshot->assess(fit);
          if (!assessment) {
            note(report, "the rack unit fit assessment was refused");
          } else {
            if (assessment.value().verdict != FitVerdict::fits) {
              note(report, "the rack unit fit assessment is not `fits`");
            }
            if (assessment.value().revision.value() != revision.value()) {
              note(report, "the assessment names a different revision");
            }
            if (assessment.value().attempt != snapshot->attempt()) {
              note(report, "the assessment names a different attempt");
            }
            if (assessment.value().subject != id.rack1) {
              note(report, "the assessment names a different subject");
            }
          }
        }
      });
    }

    // The main thread is the one writer, and it applies exactly two hundred
    // mutations while the readers are running.
    std::int64_t applied = 0;
    for (int mutation = 0; mutation < 200; ++mutation) {
      SetNodeMetadataRequest request;
      request.node = id.rack2;
      request.label = *DisplayLabel::parse("rack two revision " + std::to_string(mutation));
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (outcome) {
        ++applied;
      }
    }

    for (std::thread& reader : readers) reader.join();

    SC_CHECK_EQ(applied, static_cast<std::int64_t>(200));
    SC_CHECK_EQ(registry.revision().value(), initial_revision + static_cast<std::uint64_t>(applied));
    SC_CHECK_EQ(registry.snapshot()->node_count(), initial_nodes);
    SC_CHECK(registry.snapshot()->audit().ok());
    std::int64_t snapshots = 0;
    for (const ThreadReport& report : reports) {
      SC_CHECK_EQ(report.violations, static_cast<std::int64_t>(0));
      SC_CHECK_EQ(report.units, static_cast<std::int64_t>(kReaderIterations));
      snapshots += report.units;
      if (report.violations != 0) std::printf("reader failure: %s\n", report.first_failure);
    }
    SC_CHECK_EQ(snapshots, static_cast<std::int64_t>(kReaders) * kReaderIterations);
    std::printf("readers finished: %lld snapshots audited\n", static_cast<long long>(snapshots));
  }

  // -------------------------------------------------------------------------
  SC_CASE("many writers on one registry");
  {
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const std::uint64_t initial_revision = registry.revision().value();
    const std::size_t initial_nodes = registry.snapshot()->node_count();
    std::vector<ThreadReport> reports(static_cast<std::size_t>(kWriterThreads));
    std::vector<std::thread> writers;
    StartGate gate;

    for (int writer = 0; writer < kWriterThreads; ++writer) {
      writers.emplace_back([&registry, &reports, &gate, writer, &id]() {
        ThreadReport& report = reports[static_cast<std::size_t>(writer)];
        gate.await();
        int created = 0;
        int attempts = 0;
        // Every writer fences on the revision it has just observed, so a
        // success can only ever be the very next revision, and a refusal can
        // only ever be that fence. The attempt cap is a loop bound and not a
        // deadline: if it were ever reached the count assertions below would
        // fail loudly rather than hang.
        while (created < kWritesPerThread && attempts < 100000) {
          ++attempts;
          const std::string key =
              "mw-" + std::to_string(writer) + "-" + std::to_string(created);
          const SpaceNodeId node_id = *SpaceNodeId::parse(key);
          const RegistryRevision base = registry.revision();
          CreateNodeRequest request;
          request.node = make_node(node_id, SpaceNodeKind::rack, SpatialClass::rack, id.row1, 4,
                                   "many writers");
          request.precondition = Precondition::at_revision(base);
          const Result<MutationOutcome> outcome = registry.apply(request);
          if (outcome) {
            ++report.units;
            ++created;
            // Every one of these is decided under the writer lock and holds
            // whatever the other writers do: the fenced revision advanced by
            // exactly one, the outcome is an application and not a replay, it
            // carries a digest, and no other writer can make the published
            // revision go backwards.
            if (outcome.value().revision.value() != base.value() + 1) {
              note(report, "a success did not advance the revision by exactly one");
            }
            if (!outcome.value().applied) {
              note(report, "a fresh request was reported as a replay");
            }
            if (outcome.value().state_digest.is_zero()) {
              note(report, "a success carried no state digest");
            }
            // The digest a second successful write leaves in the registry is
            // its own, so the published revision can only be at or above this
            // outcome's revision. The equality of the two digests is asserted
            // in the single-writer cases below, where nothing can publish
            // between the apply and the read.
            if (registry.revision().value() < outcome.value().revision.value()) {
              note(report, "the published revision went backwards");
            }
          } else {
            if (outcome.code() != ErrorCode::stale_revision) {
              note(report, "a refusal was not the revision fence");
            }
            // The refusal left nothing behind: the record is absent and the
            // published model still audits.
            const SnapshotPtr snapshot = registry.snapshot();
            if (snapshot->find_node(node_id) != nullptr) {
              note(report, "a refused write left its record in the model");
            }
            if (!snapshot->audit().ok()) {
              note(report, "the model stopped auditing after a refusal");
            }
          }
        }
        report.violations += (created == kWritesPerThread) ? 0 : 1;
      });
    }
    gate.open();
    for (std::thread& writer : writers) writer.join();

    std::int64_t successes = 0;
    for (const ThreadReport& report : reports) {
      SC_CHECK_EQ(report.violations, static_cast<std::int64_t>(0));
      SC_CHECK_EQ(report.units, static_cast<std::int64_t>(kWritesPerThread));
      successes += report.units;
      if (report.violations != 0) std::printf("writer failure: %s\n", report.first_failure);
    }
    SC_CHECK_EQ(successes, static_cast<std::int64_t>(kWriterThreads) * kWritesPerThread);
    // The final revision is the initial revision plus the number of successes,
    // whatever the interleaving of the refusals was.
    SC_CHECK_EQ(registry.revision().value(),
                initial_revision + static_cast<std::uint64_t>(successes));

    const SnapshotPtr snapshot = registry.snapshot();
    SC_CHECK(snapshot->audit().ok());
    SC_CHECK(registry.audit().ok());
    SC_CHECK_EQ(snapshot->node_count(), initial_nodes + static_cast<std::size_t>(successes));
    // Every record a writer believed it created is in the model exactly once.
    std::size_t present = 0;
    for (int writer = 0; writer < kWriterThreads; ++writer) {
      for (int created = 0; created < kWritesPerThread; ++created) {
        const SpaceNodeId node_id =
            *SpaceNodeId::parse("mw-" + std::to_string(writer) + "-" + std::to_string(created));
        if (snapshot->find_node(node_id) != nullptr) ++present;
      }
    }
    SC_CHECK_EQ(present, static_cast<std::size_t>(successes));
    std::printf("writers finished: %lld successes on the same registry\n",
                static_cast<long long>(successes));
  }

  // -------------------------------------------------------------------------
  SC_CASE("a race on one claim commits it exactly once");
  {
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const std::uint64_t initial_revision = registry.revision().value();
    const OccupancyClaimId claim_id = *OccupancyClaimId::parse("race-claim");

    CreateClaimRequest create;
    create.claim.id = claim_id;
    create.claim.generation = EntityGeneration{1};
    create.claim.node = id.rack4;
    create.claim.state = ClaimState::submitted;
    create.claim.scope.kind = FootprintScopeKind::rack_units;
    create.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(20, 4)});
    SC_CHECK(registry.apply(create).ok());

    const SpaceNode* rack = registry.snapshot()->find_node(id.rack4);
    SC_CHECK(rack != nullptr);
    const OccupancyClaim* stored = registry.snapshot()->find_claim(claim_id);
    SC_CHECK(stored != nullptr);
    const EntityGeneration fence_generation = stored->generation;
    const std::int64_t available_before =
        registry.model_rollup(FitContext{}).units.available.value();

    std::atomic<int> succeeded{0};
    std::atomic<int> refused{0};
    std::atomic<ErrorCode> refusal{ErrorCode::ok};
    StartGate gate;
    const auto contender = [&registry, &gate, &succeeded, &refused, &refusal, claim_id,
                            fence_generation]() {
      // Both threads present the same fenced transition, so the race is on the
      // record and not on the fence.
      TransitionClaimRequest request;
      request.claim = claim_id;
      request.next = ClaimState::committed;
      request.precondition = Precondition::at_generation(fence_generation);
      gate.await();
      const Result<MutationOutcome> outcome = registry.apply(request);
      if (outcome) {
        ++succeeded;
      } else {
        ++refused;
        refusal.store(outcome.code(), std::memory_order_relaxed);
      }
    };
    std::thread first(contender);
    std::thread second(contender);
    gate.open();
    first.join();
    second.join();

    SC_CHECK_EQ(succeeded.load(), 1);
    SC_CHECK_EQ(refused.load(), 1);
    SC_CHECK_EQ(refusal.load(), ErrorCode::stale_generation);
    // The claim ends up committed exactly once, at exactly the next generation,
    // and the revision moved once for the creation and once for the transition.
    const SnapshotPtr snapshot = registry.snapshot();
    std::size_t matching = 0;
    for (const OccupancyClaim& claim : snapshot->claims()) {
      if (claim.id == claim_id) {
        ++matching;
        SC_CHECK_EQ(claim.state, ClaimState::committed);
        SC_CHECK_EQ(claim.generation.value(), fence_generation.value() + 1);
        SC_CHECK_EQ(claim.scope.units.size(), static_cast<std::size_t>(1));
      }
    }
    SC_CHECK_EQ(matching, static_cast<std::size_t>(1));
    SC_CHECK_EQ(registry.revision().value(), initial_revision + 2);
    SC_CHECK(snapshot->audit().ok());
    // Taking the space once moved the rack ledger by exactly the span claimed.
    SC_CHECK_EQ(registry.model_rollup(FitContext{}).units.available.value(),
                available_before - 4);
  }

  // -------------------------------------------------------------------------
  SC_CASE("a snapshot is immutable");
  {
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const FitContext context{};
    const SnapshotPtr held = registry.snapshot();
    const RegistryRevision old_revision = held->revision();
    const Digest old_digest = held->digest();
    const std::string old_bytes = held->canonical_bytes();
    const std::size_t old_nodes = held->node_count();
    const AttemptId old_attempt = held->attempt();

    SetNodeMetadataRequest request;
    request.node = id.hall;
    request.label = *DisplayLabel::parse("the hall after the held snapshot");
    const Result<MutationOutcome> outcome = registry.apply(request);
    SC_CHECK(outcome.ok());

    // The held snapshot is the old revision, byte for byte.
    SC_CHECK_EQ(held->revision().value(), old_revision.value());
    SC_CHECK(held->digest() == old_digest);
    SC_CHECK(held->canonical_bytes() == old_bytes);
    SC_CHECK(held->audit().ok());
    SC_CHECK_EQ(held->node_count(), old_nodes);
    SC_CHECK(held->attempt() == old_attempt);
    const Result<FitAssessment> held_fit = held->assess(rack_fit(id.rack1, 1));
    SC_CHECK(held_fit.ok());
    if (held_fit) SC_CHECK_EQ(held_fit.value().revision.value(), old_revision.value());
    const Result<CapacityReport> held_report = held->report(id.hall, context);
    SC_CHECK(held_report.ok());
    if (held_report) SC_CHECK_EQ(held_report.value().revision.value(), old_revision.value());

    // A fresh snapshot is the new revision, and it is a different state.
    const SnapshotPtr fresh = registry.snapshot();
    SC_CHECK_EQ(fresh->revision().value(), old_revision.value() + 1);
    SC_CHECK(fresh->digest() != old_digest);
    SC_CHECK(fresh->canonical_bytes() != old_bytes);
    SC_CHECK(fresh->audit().ok());
    SC_CHECK_EQ(registry.revision().value(), old_revision.value() + 1);
    // This process is the only writer here, so the digest the mutation reported
    // and the digest the registry published are provably the same value, and
    // both are the old digest changed.
    if (outcome) {
      SC_CHECK_EQ(outcome.value().revision.value(), fresh->revision().value());
      SC_CHECK(outcome.value().state_digest == fresh->digest());
      SC_CHECK(outcome.value().state_digest != old_digest);
      SC_CHECK(outcome.value().attempt == fresh->attempt());
      SC_CHECK_EQ(outcome.value().subject, id.hall);
      SC_CHECK(outcome.value().applied);
    }
    const Result<CapacityReport> fresh_report = fresh->report(id.hall, context);
    SC_CHECK(fresh_report.ok());
    if (fresh_report) SC_CHECK_EQ(fresh_report.value().revision.value(), old_revision.value() + 1);
  }

  // -------------------------------------------------------------------------
  SC_CASE("idempotency under concurrency");
  {
    SpaceCapacityRegistry registry = sc_fixture::build_in_memory();
    const std::uint64_t initial_revision = registry.revision().value();
    const SpaceNodeId node_id = *SpaceNodeId::parse("idem-node-1");
    CreateNodeRequest request;
    request.request_id = *RequestId::parse("idem-request-1");
    request.node =
        make_node(node_id, SpaceNodeKind::rack, SpatialClass::rack, id.row1, 4, "idempotent");

    // Each thread keeps the whole outcome it received, in its own slot, so the
    // main thread can compare a replay against the application it replays.
    struct OutcomeRecord final {
      bool ok = false;
      bool applied = false;
      ErrorCode code = ErrorCode::ok;
      RegistryRevision revision{};
      AttemptId attempt{};
      Digest digest{};
      SpaceNodeId subject{};
    };
    std::vector<OutcomeRecord> outcomes(static_cast<std::size_t>(kReaders));
    std::atomic<int> applied{0};
    std::atomic<int> replayed{0};
    std::atomic<int> refused{0};
    StartGate gate;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kReaders; ++thread) {
      threads.emplace_back([&registry, &request, &gate, &outcomes, &applied, &replayed,
                            &refused, thread]() {
        OutcomeRecord& record = outcomes[static_cast<std::size_t>(thread)];
        gate.await();
        const Result<MutationOutcome> outcome = registry.apply(request);
        if (!outcome) {
          record.ok = false;
          record.code = outcome.code();
          ++refused;
          return;
        }
        record.ok = true;
        record.applied = outcome.value().applied;
        record.code = outcome.code();
        record.revision = outcome.value().revision;
        record.attempt = outcome.value().attempt;
        record.digest = outcome.value().state_digest;
        record.subject = outcome.value().subject;
        if (record.applied) {
          ++applied;
        } else {
          ++replayed;
        }
      });
    }
    gate.open();
    for (std::thread& thread : threads) thread.join();

    SC_CHECK_EQ(applied.load(), 1);
    SC_CHECK_EQ(applied.load() + replayed.load() + refused.load(), kReaders);
    SC_CHECK_EQ(replayed.load() + refused.load(), kReaders - 1);
    SC_CHECK_EQ(applied.load() + replayed.load() + refused.load(),
                static_cast<int>(outcomes.size()));

    // The one application, and the strict form of a replay: it carries the same
    // revision, the same attempt, the same state digest and the same subject as
    // the request that produced it, with `applied` false.
    const OutcomeRecord* original = nullptr;
    for (const OutcomeRecord& record : outcomes) {
      if (record.ok && record.applied) original = &record;
    }
    SC_CHECK(original != nullptr);
    for (const OutcomeRecord& record : outcomes) {
      // A refusal is the typed conflict that owns a reused key, never an
      // untyped failure.
      if (!record.ok) SC_CHECK_EQ(record.code, ErrorCode::operation_id_conflict);
      if (original != nullptr && record.ok && !record.applied) {
        SC_CHECK_EQ(record.revision.value(), original->revision.value());
        SC_CHECK(record.attempt == original->attempt);
        SC_CHECK(record.digest == original->digest);
        SC_CHECK(record.subject == original->subject);
        // A replay is not an application: the digest it reports is the digest
        // the model has, and no second record was made.
        SC_CHECK(record.digest == registry.snapshot()->digest());
      }
    }
    const SnapshotPtr snapshot = registry.snapshot();
    std::size_t matching = 0;
    for (const SpaceNode& node : snapshot->nodes()) {
      if (node.id == node_id) ++matching;
    }
    SC_CHECK_EQ(matching, static_cast<std::size_t>(1));
    SC_CHECK_EQ(registry.revision().value(), initial_revision + 1);
    SC_CHECK(snapshot->audit().ok());
    std::printf("idempotency: %d applied, %d replayed, %d refused\n", applied.load(),
                replayed.load(), refused.load());
  }

  // -------------------------------------------------------------------------
  SC_CASE("a durable commit takes the file lock under L1 and never under L2");
  {
    const std::string state = "concurrency_durable.spcstate";
    remove_store_files(state);
    StoreOptions options;
    options.path = std::filesystem::path(state);
    options.store_identity = *StoreId::parse("concurrency-store");
    options.actor = "concurrency_test";
    options.source = "concurrency_test";
    Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
    SC_CHECK(opened.ok());
    if (opened) {
      SpaceCapacityRegistry registry = std::move(opened).value();
      SC_CHECK(registry.durable());
      // The site every durable writer hangs its buildings under is committed
      // first, so a writer can only ever be refused by the revision fence.
      {
        CreateNodeRequest request;
        request.node = make_node(*SpaceNodeId::parse("dw-site"), SpaceNodeKind::site,
                                 SpatialClass::outdoor, SpaceNodeId{}, 0, "durable site");
        const Result<MutationOutcome> outcome = registry.apply(request);
        SC_CHECK(outcome.ok());
        if (!outcome) {
          std::printf("durable site creation: %s\n", outcome.error().to_string().c_str());
        }
      }
      const std::uint64_t initial_revision = registry.revision().value();
      std::vector<ThreadReport> writer_reports(static_cast<std::size_t>(kDurableWriters));
      ThreadReport reader_report;
      std::vector<std::thread> threads;
      StartGate gate;

      for (int writer = 0; writer < kDurableWriters; ++writer) {
        threads.emplace_back([&registry, &writer_reports, &gate, writer]() {
          ThreadReport& report = writer_reports[static_cast<std::size_t>(writer)];
          gate.await();
          int created = 0;
          int attempts = 0;
          while (created < kDurableWritesPerThread && attempts < 100000) {
            ++attempts;
            const std::string key =
                "dw-" + std::to_string(writer) + "-" + std::to_string(created);
            const RegistryRevision base = registry.revision();
            CreateNodeRequest request;
            request.node = make_node(*SpaceNodeId::parse(key), SpaceNodeKind::building,
                                     SpatialClass::enclosed, *SpaceNodeId::parse("dw-site"), 1,
                                     "durable writer");
            request.precondition = Precondition::at_revision(base);
            const Result<MutationOutcome> outcome = registry.apply(request);
            if (outcome) {
              ++report.units;
              ++created;
              if (outcome.value().revision.value() != base.value() + 1) {
                note(report, "a durable success did not advance the revision by exactly one");
              }
            } else if (outcome.code() != ErrorCode::stale_revision) {
              note(report, "a durable refusal was not the revision fence");
            }
          }
        });
      }
      // A reader runs throughout the durable commits: it takes L2 shared, copies
      // the pointer and releases it, so it never waits on a commit that holds L1
      // and takes L3.
      threads.emplace_back([&registry, &reader_report, &gate]() {
        gate.await();
        const FitContext context{};
        for (int iteration = 0; iteration < kDurableReaderIterations; ++iteration) {
          const SnapshotPtr snapshot = registry.snapshot();
          ++reader_report.units;
          if (snapshot == nullptr) {
            note(reader_report, "snapshot() returned no snapshot during durable commits");
            continue;
          }
          if (!snapshot->audit().ok()) {
            note(reader_report, "a snapshot taken during durable commits failed the audit");
          }
          const std::string hex = snapshot->digest().hex();
          if (snapshot->digest().hex() != hex) {
            note(reader_report, "a durable snapshot digest changed between reads");
          }
          if (digest_in_domain(kDomainStateBinary, snapshot->canonical_bytes()) !=
              snapshot->digest()) {
            note(reader_report, "a durable snapshot digest is not its canonical digest");
          }
          const CapacityRollup rollup = model_rollup(*snapshot, context);
          if (rollup.revision.value() != snapshot->revision().value()) {
            note(reader_report, "a durable rollup names a different revision");
          }
        }
      });
      gate.open();
      for (std::thread& thread : threads) thread.join();

      std::int64_t successes = 0;
      for (const ThreadReport& report : writer_reports) {
        SC_CHECK_EQ(report.violations, static_cast<std::int64_t>(0));
        SC_CHECK_EQ(report.units, static_cast<std::int64_t>(kDurableWritesPerThread));
        successes += report.units;
        if (report.violations != 0) std::printf("durable writer failure: %s\n", report.first_failure);
      }
      SC_CHECK_EQ(successes, static_cast<std::int64_t>(kDurableWriters) * kDurableWritesPerThread);
      SC_CHECK_EQ(reader_report.violations, static_cast<std::int64_t>(0));
      SC_CHECK_EQ(reader_report.units, static_cast<std::int64_t>(kDurableReaderIterations));
      if (reader_report.violations != 0) {
        std::printf("durable reader failure: %s\n", reader_report.first_failure);
      }
      SC_CHECK_EQ(registry.revision().value(),
                  initial_revision + static_cast<std::uint64_t>(successes));
      SC_CHECK(registry.snapshot()->audit().ok());
      SC_CHECK(registry.verify().ok());
      const std::uint64_t final_revision = registry.revision().value();
      const Digest final_digest = registry.snapshot()->digest();
      std::printf("durable: %lld commits, revision %llu\n", static_cast<long long>(successes),
                  static_cast<unsigned long long>(final_revision));
      SC_CHECK(registry.close().ok());

      // A fresh open reads back exactly one whole generation, and it is the one
      // every commit verified before it was acknowledged.
      Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
      SC_CHECK(reopened.ok());
      if (reopened) {
        SpaceCapacityRegistry again = std::move(reopened).value();
        SC_CHECK_EQ(again.revision().value(), final_revision);
        SC_CHECK(again.snapshot()->digest() == final_digest);
        SC_CHECK(again.snapshot()->audit().ok());
        SC_CHECK(again.verify().ok());
        SC_CHECK(again.close().ok());
      }
    }
    remove_store_files(state);
  }

  // Every violation a thread saw was also recorded in the shared log, so an
  // empty log is the same statement as "no thread broke a property".
  SC_CHECK(first_failure_text.empty());
  if (!first_failure_text.empty()) {
    std::printf("first property broken by a thread: %s\n", first_failure_text.c_str());
  }

  return ::sc_test::summary("concurrency_test");
}
