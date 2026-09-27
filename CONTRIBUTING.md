# Contributing to Space Capacity

Copyright 2026 Summon Software Labs.

## Licensing of contributions

Space Capacity is licensed under the Apache License, Version 2.0. By submitting
a contribution you agree that your contribution is licensed under the same terms
and that you have the right to submit it. There is **no Contributor License
Agreement to sign** and no copyright assignment. Do not add a sign-off, a
co-author trailer, an AI attribution line, or a generated-by line to a commit
message; keep commit messages concise, public-facing and neutral.

## Scope

Space Capacity is DCCP Tranche 2, repository 11. It owns physical-space
capacity accounting: containment, spatial class, rack and rack-unit envelopes,
occupancy claims, reserved footprint, expansion zones, exclusions and service
clearance, in exact integer units, plus the durable store that holds them.

It deliberately does **not** become a Physical Location Registry, a Facility
Topology, a Rack Registry, an Asset Registry, a Facility Capacity runtime, a
Power or Cooling Capacity runtime, a Facility Capacity Reservation, or a
Facility Placement Planner. Contributions that pull those boundaries into Space
Capacity will be redirected.

Two rules follow from the boundary and are not negotiable:

* **No grant of placement.** A Space Capacity API may answer what fits and what
  is free. It must never return something a caller could read as permission to
  put a workload, an asset or a rack somewhere.
* **No hardware claim.** There is no PDU, UPS, generator, cooling unit, GPU,
  switch or fabric code here, and no test may imply one. Facility hardware is
  modelled only through opaque references owned by other runtimes.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

Requirements: CMake 3.20 or newer, a C++20 compiler. Windows/MSVC is the
exercised platform; the POSIX path exists and is not claimed as proved.

Build requirements for a change to be accepted:

* Release and Debug both build clean with `/W4 /WX /permissive-` on MSVC and
  with `-Wall -Wextra -Wpedantic -Wshadow -Werror` elsewhere. No warning is
  suppressed, globally or locally, to make a change pass.
* The whole test suite passes. Tests are run plainly, with no timeout of any
  kind: a hanging test is a defect to diagnose, not a test to abort.
* A new behaviour comes with a test that can fail. A test that cannot fail is
  not evidence.

## Code quality requirements

* C++20, standard library only. A third-party dependency needs a correctness
  justification that could not be met another way, and it needs to be argued in
  the pull request.
* Authoritative accounting is exact integer arithmetic. No floating point in a
  capacity number, a footprint, a clearance, a rollup, a fragmentation index or
  a persisted field.
* Every mutation that depends on current state carries an explicit
  precondition, and a stale precondition is refused rather than merged.
* Every externally supplied or persisted size is bounded before it is used to
  size an allocation, a loop or a container.
* Distinguish, in types and in reporting: identity from metadata, observation
  from authority, planned from committed, acknowledgement from verified effect,
  recovered state from revalidated evidence, zero from unknown, unsupported from
  unavailable, and capacity from the authority to consume it.
* One canonical spelling per value. Do not add a fallback that accepts a second
  spelling of the same thing.
* The decoder is a trust boundary. It refuses rather than repairs.
* No lock is held while caller-supplied code runs. The library invokes no
  caller callback at all; the store's fault-injection hook is test-only and must
  never re-enter a registry.
* No wall-clock read except in `system_utc_now`, and no result may depend on
  when a test happened to run. Randomised tests fix and print their seed.
* No telemetry, no network access, no name resolution, no remote service.

## Documentation requirements

Documentation describes only implemented and proven behaviour. Anything not
exercised by this repository's validation is labelled as not proved. Measured
behaviour, synthetic workloads and unsupported claims are labelled precisely and
kept separate.

## Before you open a pull request

1. `git status` is clean apart from your intended change.
2. Release and Debug are warning free.
3. The full suite passes, including the multiprocess and persistence tests.
4. No temporary build directory, state file, benchmark residue or log is left in
   the tree.
