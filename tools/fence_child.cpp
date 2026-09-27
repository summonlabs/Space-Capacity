// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - writer authority harness.
//
// This program attempts to take the writer lease on a store and reports what
// happened, so that a test can prove with real independent operating-system
// processes that writer authority is an operating-system lock rather than the
// contents of a file, that contention is reported and never waited out, and
// that the death of the holder relinquishes authority immediately.
//
// Exit statuses
//   0  the lease was acquired
//   3  the lease is held by another process
//   4  the open itself failed
//   5  the command line was malformed

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

constexpr int kExitAcquired = 0;
constexpr int kExitContended = 3;
constexpr int kExitOpenFailed = 4;
constexpr int kExitUsage = 5;

}  // namespace

int main(int argc, char** argv) {
  std::string state;
  std::string store;
  bool hold = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view token(argv[i]);
    if (token == "--state" && i + 1 < argc) {
      state = argv[++i];
    } else if (token == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (token == "--hold") {
      hold = true;
    } else {
      std::fputs("sc_fence_child --state PATH [--store ID] [--hold]\n", stderr);
      return kExitUsage;
    }
  }
  if (state.empty()) {
    std::fputs("sc_fence_child --state PATH [--store ID] [--hold]\n", stderr);
    return kExitUsage;
  }

  StoreOptions options;
  options.path = state;
  options.actor = "fence-child";
  options.source = "sc_fence_child";
  options.create = CreateMode::create_if_missing;
  if (!store.empty()) {
    Result<StoreId> identity = StoreId::parse(store);
    if (!identity) return kExitUsage;
    options.store_identity = identity.value();
  }

  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  if (!opened) {
    std::fprintf(stderr, "open-failed %s\n", opened.error().to_string().c_str());
    return kExitOpenFailed;
  }
  SpaceCapacityRegistry registry = std::move(opened).value();

  const Status acquired = registry.acquire_writer_lease();
  if (!acquired) {
    std::printf("contended %s\n", std::string(error_code_name(acquired.code())).c_str());
    std::fflush(stdout);
    return kExitContended;
  }
  std::printf("acquired\n");
  std::fflush(stdout);

  if (hold) {
    // Hold the lease and never release it. The parent process ends this by
    // terminating the process, which is exactly the case being proved: process
    // death must relinquish the operating-system lock immediately.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  const Status released = registry.release_writer_lease();
  if (!released) {
    std::fprintf(stderr, "release-failed %s\n", released.error().to_string().c_str());
    return kExitOpenFailed;
  }
  std::printf("released\n");
  return kExitAcquired;
}
