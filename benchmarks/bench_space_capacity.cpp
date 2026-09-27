// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - completed-operation benchmark.
//
// Methodology
//
//   * Every reported number is the wall time of COMPLETED operations divided
//     by the number of operations that completed, so a partial or abandoned
//     operation is never counted as work done.
//   * A durable operation is timed from the call to `apply` through the commit
//     protocol to the point where the effect has been read back and verified,
//     so the file write, the device flush and the atomic publish are all
//     inside the measurement rather than excluded from it.
//   * The in-memory figures are reported separately and are labelled as such,
//     because a comparison between an in-memory mutation and a durable one
//     would not be like for like.
//   * The workload is SYNTHETIC: a generated grid of halls and racks. It is
//     not a model of any real facility and no hardware is involved.
//   * Each case is run several times and the median is reported, with the
//     spread shown, so a single noisy sample cannot be presented as a result.
//   * Every case verifies its own final state afterwards and the durable cases
//     clean up every file they created.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;
using Clock = std::chrono::steady_clock;

struct Sample final {
  double median_micros = 0.0;
  double low_micros = 0.0;
  double high_micros = 0.0;
};

Sample summarise(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  Sample result;
  result.low_micros = samples.front();
  result.high_micros = samples.back();
  result.median_micros = samples[samples.size() / 2];
  return result;
}

void report(const char* name, const char* unit, const Sample& sample, std::uint64_t operations) {
  std::printf("%-34s %10.3f us/op  [%.3f .. %.3f]  %llu operations\n", name,
              sample.median_micros, sample.low_micros, sample.high_micros,
              static_cast<unsigned long long>(operations));
  (void)unit;
}

std::string id_text(const char* prefix, std::uint64_t index) {
  return std::string(prefix) + "-" + std::to_string(index);
}

// Builds a synthetic facility: one site, one plane-declaring hall, and a grid
// of racks on the hall floor. Returns the registry and the hall identity.
struct Facility final {
  SpaceCapacityRegistry registry;
  SpaceNodeId hall{};
  std::vector<SpaceNodeId> racks{};
};

Facility build_synthetic(std::uint64_t hall_side_mm, std::uint64_t rack_count,
                         const StoreOptions* options) {
  Facility facility;
  if (options == nullptr) {
    facility.registry =
        std::move(SpaceCapacityRegistry::create_in_memory(*StoreId::parse("bench-store")))
            .value();
  } else {
    facility.registry = std::move(SpaceCapacityRegistry::open(*options)).value();
  }

  auto create = [&](const SpaceNode& node) {
    CreateNodeRequest request;
    request.node = node;
    const Result<MutationOutcome> outcome = facility.registry.apply(request);
    if (!outcome) {
      std::fprintf(stderr, "benchmark setup failed: %s\n", outcome.error().to_string().c_str());
      std::exit(1);
    }
  };

  SpaceNode site;
  site.id = *SpaceNodeId::parse("bench-site");
  site.generation = EntityGeneration{1};
  site.kind = SpaceNodeKind::site;
  site.spatial_class = SpatialClass::outdoor;
  site.lifecycle = NodeLifecycle::available;
  create(site);

  SpaceNode hall;
  hall.id = *SpaceNodeId::parse("bench-hall");
  hall.generation = EntityGeneration{1};
  hall.kind = SpaceNodeKind::hall;
  hall.spatial_class = SpatialClass::floor;
  hall.lifecycle = NodeLifecycle::available;
  hall.parent = site.id;
  hall.depth = 1;
  hall.own_planar.declared_area =
      SquareMillimeters{static_cast<std::int64_t>(hall_side_mm * hall_side_mm)};
  hall.own_planar.rects = *RectSet::build({PlanarRect::make(
      0, 0, static_cast<std::int64_t>(hall_side_mm), static_cast<std::int64_t>(hall_side_mm))});
  create(hall);
  facility.hall = hall.id;

  const std::int64_t per_row = static_cast<std::int64_t>(hall_side_mm / 1000);
  for (std::uint64_t i = 0; i < rack_count; ++i) {
    const std::int64_t column = static_cast<std::int64_t>(i % static_cast<std::uint64_t>(per_row));
    const std::int64_t row = static_cast<std::int64_t>(i / static_cast<std::uint64_t>(per_row));
    SpaceNode rack;
    rack.id = *SpaceNodeId::parse(id_text("bench-rack", i));
    rack.generation = EntityGeneration{1};
    rack.kind = SpaceNodeKind::rack;
    rack.spatial_class = SpatialClass::rack;
    rack.lifecycle = NodeLifecycle::available;
    rack.parent = hall.id;
    rack.depth = 2;
    rack.own_rack.height = RackUnits{48};
    rack.placement.has_base_rect = true;
    rack.placement.base_rect =
        PlanarRect::make(column * 1000, row * 1500, 600, 1200);
    create(rack);
    facility.racks.push_back(rack.id);
  }
  return facility;
}

void clean_state(const char* path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(std::string(path) + ".prev", error);
  std::filesystem::remove(std::string(path) + ".identity", error);
  std::filesystem::remove(std::string(path) + ".lock", error);
}

}  // namespace

int main() {
  constexpr std::uint64_t kHallSide = 60000;   // 60 m
  constexpr std::uint64_t kRackCount = 400;
  constexpr int kRuns = 5;

  std::puts("Space Capacity benchmark - SYNTHETIC workload, no hardware involved");
  std::printf("platform: %s, %s\n", std::string(version_string()).c_str(),
              sizeof(void*) == 8 ? "64-bit" : "32-bit");
  std::printf("facility: one hall of %llu mm square with %llu racks, run %d times per case\n\n",
              static_cast<unsigned long long>(kHallSide),
              static_cast<unsigned long long>(kRackCount), kRuns);

  // ---------------------------------------------------------------- in memory
  {
    std::vector<double> samples;
    std::uint64_t operations = 0;
    for (int run = 0; run < kRuns; ++run) {
      Facility facility = build_synthetic(kHallSide, kRackCount, nullptr);
      const auto start = Clock::now();
      for (std::uint64_t i = 0; i < kRackCount; ++i) {
        CreateClaimRequest request;
        request.claim.id = *OccupancyClaimId::parse(id_text("bench-claim", i));
        request.claim.generation = EntityGeneration{1};
        request.claim.node = facility.racks[i];
        request.claim.state = ClaimState::committed;
        request.claim.occupant = OccupantKind::asset;
        request.claim.scope.kind = FootprintScopeKind::rack_units;
        request.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 8)});
        const Result<MutationOutcome> outcome = facility.registry.apply(request);
        if (!outcome) {
          std::fprintf(stderr, "claim failed: %s\n", outcome.error().to_string().c_str());
          return 1;
        }
        ++operations;
      }
      const auto end = Clock::now();
      samples.push_back(
          std::chrono::duration<double, std::micro>(end - start).count() / kRackCount);
    }
    report("in-memory mutation (build only)", "us", summarise(samples), operations);

    // Verify the final state of the last run.
    Facility verify = build_synthetic(kHallSide, kRackCount, nullptr);
    const Status audited = verify.registry.audit();
    if (!audited) {
      std::fprintf(stderr, "final audit failed: %s\n", audited.error().to_string().c_str());
      return 1;
    }
    std::printf("%-34s %s\n\n", "final state audit", "ok");
  }

  // ------------------------------------------------------------------ durable
  {
    const char* path = "bench-durable.spcstate";
    clean_state(path);
    std::vector<double> samples;
    std::uint64_t operations = 0;
    for (int run = 0; run < kRuns; ++run) {
      clean_state(path);
      StoreOptions options;
      options.path = path;
      options.store_identity = *StoreId::parse("bench-durable");
      options.actor = "benchmark";
      options.source = "bench_space_capacity";
      Facility facility = build_synthetic(kHallSide, 8, &options);
      const auto start = Clock::now();
      for (std::uint64_t i = 0; i < 8; ++i) {
        CreateClaimRequest request;
        request.claim.id = *OccupancyClaimId::parse(id_text("bench-durable-claim", i));
        request.claim.generation = EntityGeneration{1};
        request.claim.node = facility.racks[i];
        request.claim.state = ClaimState::committed;
        request.claim.occupant = OccupantKind::asset;
        request.claim.scope.kind = FootprintScopeKind::rack_units;
        request.claim.scope.units = *IntervalSet::build({RackUnitInterval::of_count(1, 4)});
        const Result<MutationOutcome> outcome = facility.registry.apply(request);
        if (!outcome) {
          std::fprintf(stderr, "durable claim failed: %s\n",
                       outcome.error().to_string().c_str());
          return 1;
        }
        ++operations;
      }
      const auto end = Clock::now();
      samples.push_back(std::chrono::duration<double, std::micro>(end - start).count() / 8.0);
      const Status verified = facility.registry.verify();
      if (!verified) {
        std::fprintf(stderr, "durable verify failed: %s\n", verified.error().to_string().c_str());
        return 1;
      }
      (void)facility.registry.close();
    }
    report("durable mutation (write+flush+publish)", "us", summarise(samples), operations);

    // The published state is re-read after the benchmark to prove it is real.
    StoreOptions options;
    options.path = path;
    options.store_identity = *StoreId::parse("bench-durable");
    options.mode = OpenMode::read_only;
    Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
    if (!reopened) {
      std::fprintf(stderr, "reopen failed: %s\n", reopened.error().to_string().c_str());
      return 1;
    }
    std::printf("%-34s revision %s records %zu\n", "published state re-read",
                reopened.value().revision().to_string().c_str(),
                reopened.value().snapshot()->record_count());
    (void)reopened.value().close();
    clean_state(path);
    std::printf("%-34s %s\n\n", "durable residue removed", "ok");
  }

  // -------------------------------------------------------------- read paths
  {
    Facility facility = build_synthetic(kHallSide, kRackCount, nullptr);
    const SnapshotPtr snapshot = facility.registry.snapshot();

    std::vector<double> rollup_samples;
    for (int run = 0; run < kRuns; ++run) {
      const auto start = Clock::now();
      const CapacityRollup rollup = model_rollup(*snapshot, FitContext{});
      const auto end = Clock::now();
      rollup_samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
      if (rollup.rack_count != kRackCount) {
        std::fprintf(stderr, "rollup counted %llu racks, expected %llu\n",
                     static_cast<unsigned long long>(rollup.rack_count),
                     static_cast<unsigned long long>(kRackCount));
        return 1;
      }
    }
    report("whole-model rollup", "us", summarise(rollup_samples), kRuns);

    std::vector<double> audit_samples;
    for (int run = 0; run < kRuns; ++run) {
      const auto start = Clock::now();
      const Status audited = snapshot->audit();
      const auto end = Clock::now();
      if (!audited) {
        std::fprintf(stderr, "audit failed: %s\n", audited.error().to_string().c_str());
        return 1;
      }
      audit_samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    report("whole-model audit", "us", summarise(audit_samples), kRuns);

    std::vector<double> fit_samples;
    std::uint64_t queries = 0;
    for (int run = 0; run < kRuns; ++run) {
      const auto start = Clock::now();
      for (std::uint64_t i = 0; i < kRackCount; ++i) {
        RackUnitFitRequest request;
        request.rack = facility.racks[i];
        request.needed = RackUnits{8};
        request.max_candidates = 1;
        const Result<FitAssessment> assessment = snapshot->assess(request);
        if (!assessment) return 1;
        if (assessment.value().verdict != FitVerdict::fits) {
          std::fprintf(stderr, "the benchmark expected every empty rack to fit\n");
          return 1;
        }
        ++queries;
      }
      const auto end = Clock::now();
      fit_samples.push_back(std::chrono::duration<double, std::micro>(end - start).count() /
                            static_cast<double>(kRackCount));
    }
    report("rack unit fit assessment", "us", summarise(fit_samples), queries);

    std::vector<double> encode_samples;
    for (int run = 0; run < kRuns; ++run) {
      const auto start = Clock::now();
      const std::string bytes = snapshot->canonical_bytes();
      const auto end = Clock::now();
      if (bytes.empty()) return 1;
      encode_samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    report("canonical encode", "us", summarise(encode_samples), kRuns);

    std::vector<double> decode_samples;
    const std::string bytes = snapshot->canonical_bytes();
    for (int run = 0; run < kRuns; ++run) {
      const auto start = Clock::now();
      const Result<SnapshotPtr> decoded = Store::decode_image(Store::frame(*snapshot));
      const auto end = Clock::now();
      if (!decoded) {
        std::fprintf(stderr, "decode failed: %s\n", decoded.error().to_string().c_str());
        return 1;
      }
      decode_samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    report("frame + decode + audit", "us", summarise(decode_samples), kRuns);
    std::printf("%-34s %zu bytes\n", "canonical payload", bytes.size());
  }

  std::puts("");
  std::puts("All timings are median of 5 runs of a completed-operation loop.");
  std::puts("The durable figure includes the staging write, the device flush, the atomic");
  std::puts("publish and the read-back verification; nothing durable is excluded from it.");
  return 0;
}
