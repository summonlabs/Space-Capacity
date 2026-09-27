// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - process-death injection harness.
//
// This program performs one real durable commit and terminates the process
// without unwinding at a named step of the commit protocol. It exists so that a
// test can prove, with a real independent operating-system process, that dying
// at any durable step leaves exactly one whole authoritative generation behind.
//
// Exit status 9 is the injected death. Any other exit status means the run did
// not reach the requested step, which the test treats as a failure of the proof
// rather than as a pass.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

int usage() {
  std::fputs(
      "sc_crash_child --state PATH --stage STAGE [--store ID]\n"
      "\n"
      "stage is one of: after-lock, after-staging-write, after-staging-flush,\n"
      "after-staging-verify, after-previous, before-publish, after-publish,\n"
      "after-directory, after-lock-release, after-published-verify\n",
      stderr);
  return 2;
}

bool stage_from(std::string_view name, WriteStage& out) {
  if (name == "after-lock") {
    out = WriteStage::lock_acquired;
    return true;
  }
  if (name == "after-staging-write") {
    out = WriteStage::staging_written;
    return true;
  }
  if (name == "after-staging-flush") {
    out = WriteStage::staging_flushed;
    return true;
  }
  if (name == "after-staging-verify") {
    out = WriteStage::staging_verified;
    return true;
  }
  if (name == "after-previous") {
    out = WriteStage::previous_retained;
    return true;
  }
  if (name == "before-publish") {
    out = WriteStage::before_publish;
    return true;
  }
  if (name == "after-publish") {
    out = WriteStage::after_publish;
    return true;
  }
  if (name == "after-directory") {
    out = WriteStage::directory_flushed;
    return true;
  }
  if (name == "after-lock-release") {
    out = WriteStage::lock_released;
    return true;
  }
  if (name == "after-published-verify") {
    out = WriteStage::published_verified;
    return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::string state;
  std::string store;
  WriteStage target = WriteStage::none;
  bool have_stage = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view token(argv[i]);
    if (token == "--state" && i + 1 < argc) {
      state = argv[++i];
    } else if (token == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (token == "--stage" && i + 1 < argc) {
      have_stage = stage_from(argv[++i], target);
      if (!have_stage) return usage();
    } else {
      return usage();
    }
  }
  if (state.empty() || !have_stage) return usage();

  StoreOptions options;
  options.path = state;
  options.actor = "crash-child";
  options.source = "sc_crash_child";
  if (!store.empty()) {
    Result<StoreId> identity = StoreId::parse(store);
    if (!identity) {
      std::fprintf(stderr, "bad store identity\n");
      return 2;
    }
    options.store_identity = identity.value();
  }
  options.fault_hook = [target](WriteStage stage) {
    if (stage != target) return;
    std::fprintf(stdout, "injected death at %s\n", std::string(write_stage_name(stage)).c_str());
    std::fflush(stdout);
    // Terminate without unwinding, without running destructors and without
    // releasing anything the operating system will release for us.
    std::_Exit(kFaultExitStatus);
  };

  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
  if (!opened) {
    std::fprintf(stderr, "open failed: %s\n", opened.error().to_string().c_str());
    return 1;
  }
  SpaceCapacityRegistry registry = std::move(opened).value();

  // One real mutation that the crash will interrupt. The fixture identity is
  // fixed so the test can find the record after recovery.
  CreateNodeRequest request;
  Result<SpaceNodeId> id = SpaceNodeId::parse("crash-node");
  if (!id) return 1;
  request.node.id = id.value();
  request.node.generation = EntityGeneration{1};
  request.node.kind = SpaceNodeKind::site;
  request.node.spatial_class = SpatialClass::outdoor;
  request.node.lifecycle = NodeLifecycle::available;
  request.node.label = *DisplayLabel::parse("crash node");

  Result<MutationOutcome> outcome = registry.apply(request);
  if (!outcome) {
    std::fprintf(stderr, "apply failed: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  const Status verified = registry.verify();
  if (!verified) {
    std::fprintf(stderr, "verify failed: %s\n", verified.error().to_string().c_str());
    return 1;
  }
  std::printf("committed revision %s\n", outcome.value().revision.to_string().c_str());
  return 0;
}
