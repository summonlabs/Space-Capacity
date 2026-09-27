# Space Capacity

**Physical-space capacity accounting for the Data Center Control Plane.**

Space Capacity is DCCP Tranche 2, repository 11 of the canonical 72-runtime Data
Center Control Plane. It is a portable C++20 library, a command line tool and a
versioned CMake package. It has no third-party dependencies, opens no network
socket, and transmits no telemetry.

It answers one question, at one hierarchy level and one generation, with exact
integer arithmetic:

> What physical space is genuinely available now, after occupancy,
> incompatibilities, reserved footprint, service clearance and expansion
> constraints have been applied?

It answers that question and **never grants placement**. A query observes. Taking
space requires a separate, explicitly preconditioned mutation, and that mutation
is refused when the space is not free.

---

## 1. What it is, and what it is not

### It owns

* **Containment.** A tree of physical space nodes: site, building, hall, row,
  rack, rack-unit band. Levels may be skipped; nothing walks a fixed chain.
* **Spatial class.** Every node declares what kind of space it is (floor, rack,
  aisle, overhead, outdoor, service, enclosed, cage). `unspecified` is refused.
* **Envelopes.** A planar envelope in whole square millimetres with optional
  declared rectangles, and a vertical rack envelope in whole rack units.
* **Occupancy claims.** What is taken, by what, and in which lifecycle state.
* **Reserved footprint.** Holds on space that a runtime with reservation
  authority has already decided, recorded here with the reference that proves it.
* **Expansion zones.** Space earmarked by a future build-out programme.
* **Exclusion regions.** Space that must not be used, for a stated reason and for
  stated kinds of occupant.
* **Service clearance.** Space that must stay clear for access, either enforced
  (subtracted and refused) or reported only, never silently ignored.
* **Capacity arithmetic.** Rollups that cannot double count, contiguous-fit
  queries, fragmentation, diffs, explanations, revalidation and stale-state
  fencing.
* **The durable store.** A versioned, integrity-checked, single-generation state
  file with an explicit commit point and operating-system writer authority.

### It does not own, and does not act as

| Concern | Owner |
| --- | --- |
| Location identity, addressing and paths | Physical Location Registry |
| The facility containment graph and its generations | Facility Topology |
| Rack composition, mounting and rack occupancy | Rack Registry |
| Asset identity and asset lifecycle | Asset Registry |
| Aggregate facility capacity | Facility Capacity |
| Power capacity | Power Capacity |
| Cooling capacity | Cooling Capacity |
| Future reservation authority | Facility Capacity Reservation |
| Placement planning | Facility Placement Planner |
| Accelerator execution, memory, serving and workload scheduling | ASI |
| Network topology, paths, transport, congestion and fabric federation | DFI |
| Electrical and thermal actuation of any kind | power and cooling control planes |

Space Capacity consumes identities from the runtimes above as **opaque,
byte-preserved references** and owns none of them. It never reinterprets a value
it was given, never contacts those runtimes, and never resolves a reference.

### The two rules that follow from the boundary

1. **No grant of placement.** `FitVerdict::fits` describes space. It is not
   permission. `FitAssessment::grants_placement()` is a constant `false`, and
   every assessment carries the revision and attempt it was taken at so a caller
   must fence before acting.
2. **No hardware claim.** There is no PDU, UPS, generator, cooling unit, GPU or
   switch code here, and no test implies one. Nothing in this repository has been
   run against facility hardware.

---

## 2. Build, test and install

Requirements: CMake 3.20 or newer, a C++20 compiler with no extensions, and
nothing else. Windows/MSVC is the exercised platform.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
cmake --install build --config Release --prefix /opt/space-capacity
```

A downstream project then writes:

```cmake
find_package(SpaceCapacity 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::space_capacity)
```

No build-tree path is baked into the package, so it works from any install
prefix. `tests/downstream/` is a complete, independent consumer of exactly that
shape and is deliberately not part of this project's own build.

The library is built and installed as a **static** library. A shared build is not
supported by this release and is not claimed as proved.

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `SPACE_CAPACITY_BUILD_TESTS` | `ON` | build the test suite |
| `SPACE_CAPACITY_BUILD_TOOLS` | `ON` | build `spacecap` and the process-level proof harnesses |
| `SPACE_CAPACITY_BUILD_EXAMPLES` | `ON` | build the examples |
| `SPACE_CAPACITY_BUILD_BENCHMARKS` | `ON` | build the benchmark |
| `SPACE_CAPACITY_WARNINGS_AS_ERRORS` | `ON` | `/W4 /WX` on MSVC, `-Wall -Wextra -Wpedantic -Wshadow -Werror` elsewhere |
| `SPACE_CAPACITY_ENABLE_ASAN` | `OFF` | build with AddressSanitizer |

---

## 3. The model

### 3.1 Containment, and why a rollup cannot double count

A node either **declares a plane** of its own or is a **grouping node**. A row of
racks with no floor of its own is a grouping node.

A node's **placement** is where it sits inside the envelope above it. The rule
that makes the arithmetic exact is this:

> A node that declares a plane inside another plane is a **subdivision** of that
> plane, not a separate consumption of it. Its area is removed from the enclosing
> plane's own declared area, and its placement is not counted in the enclosing
> plane's `structural` measure.

So the building below declares 2 400 000 000 mm² and the hall inside it declares
600 000 000 mm². The building's own ledger reports **1 800 000 000 mm² declared**,
not 2 400 000 000, because 600 000 000 of it is the hall and the hall accounts for
it separately. The rollup over the site is the plain sum over both planes and is
exactly 2 400 000 000 mm² — the area that physically exists, counted once. The
identity

```
declared == excluded + structural + claimed + held + earmarked + available
```

holds at **every** subtree root, at every level, with no exceptions. A property
test checks it against an independent reference model.

A rack declares no plane. It occupies floor through its placement, which is
attributed to the nearest plane-owning ancestor — so a rack standing on a hall
floor consumes the hall's floor even when it hangs off a grouping row.

### 3.2 Every measure is a union measure

Two exclusion rectangles over the same area measure once. Two claims over the same
area measure once — and are separately refused as a conflict at commit time. The
ledger cannot count the same square millimetre or the same rack unit twice even
if the input overlaps, because every figure is the measure of a **union**, not a
sum. The union kernel is a sweep over x with a segment tree over compressed y
coordinates, and a property test checks it against a brute-force grid count.

### 3.3 Units, exactly

| Quantity | Unit | Type |
| --- | --- | --- |
| Planar distance | whole millimetres | `int64` |
| Planar area | whole square millimetres | `int64` |
| Vertical space | whole rack units, 1-based | `int32` |
| Counts | unsigned 64-bit | — |

There is no floating point anywhere in a capacity number, a footprint, a
clearance, a rollup, a fragmentation index or a persisted field. Fragmentation is
reported in **parts per million** by exact integer division; the remainder is
discarded, never rounded up into a claim.

**Rack unit convention.** Space Capacity uses **half-open** intervals: `[first,
last)` contains `first` and excludes `last`, so `[1,3)` and `[3,5)` do not
overlap. Rack Registry's `RackUnitRange` and Asset Registry's `UnitSpan` use the
same rule. Physical Location Registry instead stores an inclusive envelope
`{first, height}` whose last unit is `first + height - 1`; the explicit
conversions live in `units.hpp` as `RackUnitInterval::from_inclusive` and
`inclusive_height`. The sibling caps differ too — 512 in Physical Location
Registry and Asset Registry, 1024 in Rack Registry — and Space Capacity's own
hard bound is 2048, a superset. A value that is representable here and not there
is the operator's to reconcile; Space Capacity never narrows a value silently.

### 3.4 Records

Six families, each with a stable identity, a per-record generation, an optional
explicit scope, bounded references and bounded evidence:

* **`SpaceNode`** — kind, spatial class, lifecycle, containment, placement,
  planar envelope, rack envelope, upstream references, lineage.
* **`OccupancyClaim`** — node, scope, state, occupant kind, references, evidence.
  `planned` and `submitted` are **pending**: reported, subtracted from nothing.
  `committed` and `releasing` **consume**. `released` and `retired` are terminal.
* **`FootprintReservation`** — a hold, which must name the reservation reference
  that authorised it. Space Capacity records holds; it does not decide them.
* **`ExclusionRegion`** — reason, state, and a mask of the occupant kinds it
  blocks. An exclusion that excludes nothing is refused.
* **`ClearanceConstraint`** — a planar band or a set of rack units, either
  enforceable (subtracted and refused) or reported only. Every report says which.
* **`ExpansionZone`** — state, from identified through funded and active to
  consumed or retired. An earmarking zone removes space from ordinary
  availability and is reported separately, so "free floor" never quietly includes
  area a construction programme has already claimed.

### 3.5 Zero, unknown and unavailable are different

A measure is `known` when its inputs are declared, `unknown` when the inputs were
never declared, and `unavailable` when the inputs exist but could not be read.
Only `known` measures carry a number. A grouping node reports
`MeasureState::unknown` with every area value zero **and names the plane that owns
the space**, so the zero cannot be read as a measurement. The same distinction is
kept between `unsupported` and `unavailable`, and between a measured zero and an
absent optional.

---

## 4. State and authority

### 4.1 Identity is not address

A `SpaceNodeId` names a region for the life of that region. It never changes,
it is never recycled, and moving a node changes only its address — the parent and
the placement — while the identity survives and the model records that it did.
`StrongId` families do not convert into one another: `static_assert`s in the
tests prove a node identity cannot stand in for a claim identity.

Retired and replaced locations follow upstream lineage rather than being
forgotten. A node moving to `replaced` must name its successor, the successor
records what it replaced, and a capacity query over the old identity is answered
with the lineage rather than a stale number.

### 4.2 Every mutation carries a precondition

A mutation that depends on current state carries a `Precondition`: an optional
whole-model `RegistryRevision` fence and an optional per-record
`EntityGeneration` fence. A stale precondition is **refused**, with the expected
and actual values in the error, and the committed state is byte-identical
afterwards. Setting no precondition is legal and explicit; the outcome records
the revision it actually landed on.

### 4.3 Observations, freshness and revalidation

An observation is a fact about a moment. A token carries the subject, its
generation, the revision, and the attempt it was taken at. Revalidating it
against current state returns one of:

| Freshness | Meaning |
| --- | --- |
| `fresh` | observed by this session, above the attempt floor it opened from |
| `recovered` | observed at or before that floor: read out of a store, not revalidated |
| `stale` | the subject or the revision moved on |
| `orphaned` | the subject is no longer in the model |
| `superseded` | a newer observation of the same subject exists |

**Reopening a store does not make anything fresh.** Evidence observed before a
restart keeps its original attempt identity, and `fresh_floor()` is the persisted
attempt sequence the store had reached when this session opened it. A recovered
number is never presented as a current one.

### 4.4 Idempotency is bounded and explicit

A mutating request may carry a `RequestId`. Applying the same key again while the
registry still stands at the revision that request produced returns the recorded
outcome with `applied == false`. The same key arriving at any other revision is
refused with `operation_id_conflict`. The retained key table is bounded by
`Limits::kMaxIdempotencyRecords`; a key older than the window is applied again,
and the ordinary conflict checks then refuse it rather than double-applying.

### 4.5 Validation precedence

Every mutation runs the same stages in the same order, and the first failing
stage decides the reported code:

| Stage | What it checks | Codes |
| --- | --- | --- |
| 1 `argument_shape` | the request's own fields, independent of state | 1xx |
| 2 `precondition_authority` | revision and generation fences | 3xx |
| 3 `referential` | existence and uniqueness of the subjects named | 2xx |
| 4 `compatibility` | kind, hierarchy, lifecycle, transitions, conflicts | 4xx |
| 5 `capacity` | envelope fit, occupancy arithmetic, checked totals | 5xx, 6xx |
| 6 `persistence` | the durable step | 7xx |

`validation_stage_of(ErrorCode)` is part of the public API and is exercised by a
dedicated test: a request that is wrong in two ways reports the earlier stage.

---

## 5. Persistence and recovery

### 5.1 File set

```
<state>                   the committed state image, immutable once published
<state>.prev              the previous committed generation, retained
<state>.identity          the store identity anchor
<state>.lock              the writer lock file, an OS-level lock target
<state>.tmp-<pid>-<seq>   staging files, never authoritative
```

Only `<state>` and `<state>.prev` are authoritative, and neither is authoritative
until its frame has been checked, its payload digest verified, its store identity
confirmed, and the decoded model has passed the **same full audit every read path
runs**.

### 5.2 The frame

```
  0    8    magic "SPCSTAT"
  8    4    format version            (u32, big endian)
 12    4    model revision            (u32)
 16    4    endian tag 0x01020304     (u32, detects a different byte order)
 20    4    mount slots per rack unit (u32, the rack coordinate model)
 24    8    revision                  (u64)
 32    8    store incarnation         (u64)
 40    8    attempt sequence          (u64)
 48    8    payload length            (u64)
 56    4    flags                     (u32, must be zero)
 60    4    header CRC-32 over [0,60) (u32)
 64   32    payload SHA-256
 96   32    store identity digest
128    4    store identity length     (u32)
132    4    reserved, must be zero    (u32)
136  ...   store identity text, then an 8-byte creation instant, then the payload
 end  40    image SHA-256 over everything before it, then "SPCEND\0\0"
```

### 5.3 The commit protocol

```
plan -> validate -> reserve the next revision and attempt -> write staging ->
flush -> read back and verify -> retain the previous generation ->
atomic publish (THE COMMIT POINT) -> flush the directory -> release the lock ->
re-read and verify what is actually on disk
```

Nothing is authoritative before the commit point. Exactly one whole generation is
visible after it; there is no window in which a reader sees half of one. The
caller's in-memory state is published **only after the visible bytes have been
read back and compared byte for byte with the frame the committed state encodes
to**. Acknowledgement and verified effect are separate steps, and the second one
decides success.

Retained staging files from a dead process are retired on the next open, because
a staging file is never authoritative.

### 5.4 What the store refuses

With a specific code, and before any allocation that a declared size would drive,
the store refuses an image that is corrupt, truncated, malformed, oversized, of
an unimplemented format or model version, written by a different byte order,
written under a different rack coordinate model, carrying an identity that is not
a valid identifier, carrying a payload that is not exactly the records of a model
the audit accepts, or carrying a store identity that does not match its anchor.
A file that carries no identity digest match is refused as `wrong_store`. A
swapped or unrelated file at the expected path is **refused, never adopted**.

### 5.5 Writer authority

Writer authority is an exclusive **operating-system lock** on the lock file, held
for the duration of a commit and, in lease mode, for the lifetime of a lease. It
is never inferred from the contents of a file: a lock file left behind by a dead
process is a stale file, not a lock, and it is overwritten the moment the lock is
genuinely acquired. Contention is reported as `lock_conflict` and is never waited
out. **Process death relinquishes authority immediately and correctly**, which
the multiprocess tests prove with real independent operating-system processes.

---

## 6. Concurrency

Two locks exist in process:

| Lock | Role |
| --- | --- |
| `L1` `commit_mutex_` | serializes writers; held for a whole mutation |
| `L2` `state_mutex_` | a `shared_mutex` over the published snapshot pointer and the retained history |
| `L3` the OS file lock | taken inside a durable commit while `L1` is held and `L2` is not |

**The lock order is `L1` then `L2`, and `L1` then `L3`. It is never reversed.**
Readers take `L2` shared, copy the `shared_ptr` and release immediately, so a
report, a diff or a fit search runs entirely on an immutable snapshot and blocks
nobody. The library invokes no caller-supplied callback anywhere, so no callback
can re-enter a held lock; the store's fault-injection hook is the one exception,
it exists only for tests, it runs inside the commit path, and it must not
re-enter a registry.

A mutation builds a candidate snapshot and audits it, hands it to the store, and
publishes it in memory only after the store has published it on disk and verified
the visible bytes. A failed durable step therefore leaves memory unchanged as well
as disk, so the two never disagree.

---

## 7. Error model

Every fallible entry point returns `Result<T>`. The code is the stable,
machine-readable outcome; the message is a stable human explanation. Codes are
grouped by the stage of validation that owns them and are never reused for a
different meaning. Distinct conditions keep distinct codes:

* `unknown_state` is not a value of zero;
* `unsupported` is not `unavailable`;
* `stale_generation` is not `conflict`;
* `not_found` is not `already_exists`;
* `capacity_exceeded` is not authority to consume the space.

Codes are stable and grouped `1xx` input, `2xx` structure, `3xx` authority,
`4xx` lifecycle and conflict, `5xx` capacity, `6xx` limits, `7xx` persistence,
`8xx` uncertainty, `9xx` internal. `error_code_name`, `error_category`,
`error_code_is_retryable` and `validation_stage_of` are public, and
`Error::to_string()` renders deterministically as
`code: message [subject=S] [expected=E actual=A]`. `error_code_is_retryable` is a
hint about the caller's next step, not a promise.

Every refusal and every decision also carries a deterministic, deduplicated,
ordered `ExplanationSet` of stable `ReasonCode`s — so a report can say not only
what the number is but why.

---

## 8. The command line tool

```
spacecap [--state PATH] [--store ID] [--actor NAME] [--source NAME] [--read-only] <command>
```

| Command | What it does |
| --- | --- |
| `version` | print the version banner |
| `status` | print the store status, including the content digest |
| `verify` | verify the durable state and audit it |
| `show` | print the canonical text of the state |
| `summary [--node ID]` | print the whole-model or per-subtree rollup |
| `report --node ID` | print the capacity report of one node, with every plane in its subtree |
| `fit-units --rack ID --units N` | assess a rack-unit fit |
| `fit-rect --node ID --width MM --depth MM` | assess a planar rectangle fit |
| `diff --from REV --to REV` | print the diff between two retained revisions |
| `node-add`, `node-lifecycle`, `node-reparent` | mutate a node |
| `claim-add`, `claim-state` | mutate a claim |
| `exclusion-add` | add an exclusion region |
| `lease` | take the writer lease and hold it until standard input ends |

Exit status `0` on success, `1` on a typed refusal (printed to stderr with its
stable code), `2` for a malformed command line, and `9` is reserved for the
process-death injection harness.

A read-only open of a path with no state file reports `no_authoritative_state`,
so an inspection can never create one.

---

## 9. Examples

| File | What it shows |
| --- | --- |
| `examples/example_containment_ledger.cpp` | a containment tree with a subdivided plane, one node's own ledger against the subtree rollup, and why the rollup does not double count |
| `examples/example_durable_store.cpp` | the commit protocol from outside: apply, verify, close, reopen, and evidence that is recovered rather than fresh |
| `examples/example_fit_assessment.cpp` | a rack-unit and a planar fit, the fragmentation figures, and the refusal of an overlapping claim |

Each is a self-contained program. Two write nothing; the durable one writes one
state file in the current directory and removes it again.

---

## 10. Validation

Everything in this section was executed on the platform named below. Nothing in
this repository has been run against facility hardware, and no claim here rests
on hardware behaviour.

### 10.1 Platform exercised

* Windows 11, x64, MSVC 19.44.35222 (Visual Studio 2022 Build Tools), CMake 4.3.2,
  Ninja 1.13.2.
* Release and Debug both build with `/W4 /WX /permissive- /Zc:__cplusplus
  /utf-8 /EHsc` and produce **zero first-party warnings**. No warning is
  suppressed anywhere.
* The POSIX path exists for every platform operation and is written to the same
  contract. It was **not** built or executed during this validation and is
  therefore **not claimed as proved**.

### 10.2 The suite

Run plainly, with no timeout of any kind:

```sh
ctest --test-dir build --output-on-failure -C Release
```

| Area | What is proved |
| --- | --- |
| Units and intervals | half-open convention, normalization, complement, free runs, first fit, fragmentation, union area against a brute-force grid, checked arithmetic |
| Identity and text | identifier grammar, non-convertible families, counter parsing and ordering, deterministic text, SHA-256 against published vectors |
| Model | the full 7×7 containment table, spatial class admissibility, the full lifecycle transition table, node shape validation field by field |
| Claims and regions | the full 6×6, 5×5, 4×4 and 6×6 transition tables, occupant masks, scope validation, reservation expiry at a caller-supplied reading |
| Accounting | exact ledger figures for a fixture whose numbers are stated in the test, conflicts, pending against committed, expiry, retirement, and the zero-is-not-unknown invariant |
| Rollup | no double counting, checked against an independent reference model that walks the tree itself |
| Fit | candidate completeness against brute force over random layouts, alignment, fragmentation reporting, and that a query grants nothing |
| Observations | freshness classification, recovery across a real reopen, stale generation and revision refusals, idempotent replay |
| Diffs | canonical ordering, field names, ledger deltas, retained-revision limits |
| Persistence | real close and reopen, byte-identical state, recovery from a corrupt current generation, refusal to adopt a swapped store |
| Corruption | truncation at every offset, single-byte mutation across the header and payload, wrong endianness, wrong versions, oversized declarations, bad digests |
| Adversarial | duplicate identity, containment cycles, depth attacks, integer overflow, resource exhaustion, path attacks, aliasing, stale authority |
| Property | union area, containment accounting, move and reparent and replacement sequences, ledger reconstruction and interval algebra against independent reference models |
| Randomized | seeded mutation streams, determinism, order independence, candidate validity, diff consistency |
| Concurrency | many readers with one writer, many writers, a race on one claim, snapshot immutability, idempotency under concurrency |
| Multiprocess | process death at every durable step of the commit protocol, writer-authority contention between real processes, and state written by one process read by another |
| Installed API | the public umbrella header alone, plus a complete lifecycle |

### 10.3 Process death, proved with real processes

`sc_crash_child` is a real program that performs one real durable commit and
terminates without unwinding at a named step of the protocol. The test runs it
once per step — after the lock, after the staging write, after the staging flush,
after the staging verification, after the previous generation is retained, before
the publish, after the publish, after the directory flush, after the lock
release, and after the published bytes are verified. After each death the
survivor reopens the store and asserts:

* the process really did die at the requested step (exit status 9);
* exactly one whole generation is present — either the old one or the new one,
  never a mixture;
* the model passes the full audit;
* the node the dead process was creating is present exactly once if the death
  was at or after the commit point, and absent if it was before;
* the store accepts a further commit afterwards; and
* no staging file is left behind.

`sc_fence_child` is a real program that takes the writer lease and holds it until
it is terminated. The test asserts that a second independent process is refused
with `lock_conflict`, that a direct attempt from the test process is refused the
same way, and that after the holder is **terminated** the next process acquires
the lease immediately — which is the proof that authority is an operating-system
lock rather than a file.

### 10.4 Install, export and downstream

The library, headers, CLI and package files are installed to a chosen prefix. An
independent CMake project in `tests/downstream/` — not part of this build —
configures against that prefix with `find_package(SpaceCapacity 1.0 REQUIRED)`,
links `dccp::space_capacity`, builds under `/W4 /WX`, and runs a real minimal
lifecycle: create a store, create a hall, take floor with a committed claim,
check the arithmetic, run a fit, verify, close, reopen read-only, compare
digests, and remove its residue.

### 10.5 Benchmarks

`bench_space_capacity` measures **completed operations only**. Each figure is the
wall time of a completed-operation loop divided by the number of operations that
completed, reported as the median of five runs with the observed spread.

* The durable figure is timed from the call to `apply` through the whole commit
  protocol to the point where the effect has been read back and verified, so the
  staging write, the device flush, the atomic publish and the read-back
  verification are **inside** the measurement and nothing durable is excluded.
* In-memory figures are labelled separately, because comparing them with a
  durable operation would not be like for like.
* The workload is **SYNTHETIC**: a generated single-hall facility with 400 racks.
  It is not a model of any real facility and no hardware is involved.
* Every case verifies its own final state afterwards and the durable cases remove
  every file they created.
* There are no before/after pairs in this release, so no comparison is published
  that a methodology would have to isolate.

### 10.6 AddressSanitizer

The suite is also built and run with `SPACE_CAPACITY_ENABLE_ASAN=ON`. The exact
result and any limitation are recorded in section 12.

---

## 11. Honest limitations

* **Validation is Windows/MSVC only.** The POSIX code path exists and is written
  to the same contract, but it was not built or executed here and is not claimed
  as proved.
* **One writer per state file.** Writer authority is a per-file operating-system
  lock. It fences concurrent writers correctly; it does not make a distributed
  store.
* **The state file is one generation, not a log.** Each commit rewrites the whole
  generation. History is retained only as the single previous generation plus an
  in-process window of `kRetainedRevisions` snapshots for diffs. A revision
  outside that window is refused with `stale_revision` rather than approximated.
* **Whole-state cost per mutation.** A mutation decodes nothing but rebuilds the
  candidate snapshot, re-encodes it for its digest, and runs the full audit. On
  the benchmark's synthetic 400-rack facility that is on the order of a few
  milliseconds per in-memory mutation and several milliseconds per durable one,
  dominated by the file write and the device flush. This is a control-plane
  runtime sized for facility-scale modelling, not a high-frequency one.
* **Command line limits.** The CLI exposes node, claim and exclusion creation,
  node lifecycle and reparenting, and read-only inspection. Reservation,
  clearance, expansion-zone and claim-detail mutations are reachable through the
  library but have no CLI verb in this release.
* **Path safety is deliberate but bounded.** Links, junctions and reparse points
  are refused rather than followed, and declared sizes are bounded before
  allocation. The model defends against corruption and stale writers, not against
  a hostile writer that already has filesystem access to the store directory.
* **A store directory is trusted-local.** There is no encryption, no signing and
  no remote authentication. The integrity digest is a corruption detector and a
  content identity, not an authenticity mechanism.
* **No hardware anything.** No PDU, UPS, generator, cooling unit, GPU, NIC,
  switch or fabric code exists here, and no measurement in this repository came
  from hardware.
* **Upstream references are opaque.** Space Capacity preserves an identity
  byte for byte and records the generation and state it observed. It cannot and
  does not verify that an upstream record still exists in the state it was
  observed in.

---

## 12. Validation results

Measured on Windows 11 x64 with MSVC 19.44.35222 (Visual Studio 2022 Build
Tools), CMake 4.3.2 and Ninja 1.13.2, from the commands in sections 2, 10.2,
10.4 and 10.5. Nothing below is projected or estimated.

### 12.1 Build

| Configuration | Result |
| --- | --- |
| Release, `cmake --build` | success, **zero warnings** under `/W4 /WX /permissive-` |
| Debug, `cmake --build` | success, **zero warnings** under `/W4 /WX /permissive-` |
| Release + `SPACE_CAPACITY_ENABLE_ASAN=ON` (Debug) | success, **zero warnings** |
| Independent downstream consumer, `/W4 /WX` | success, **zero warnings** |

The library is 23 translation units and 23 public headers; the repository is
30 851 lines of C++ including the tests.

### 12.2 Test suite

Seventeen suites, 34 CTest entries (each suite has a preparation step that
empties its own working directory). **34 of 34 pass, 0 fail**, in Release, in
Debug, and under AddressSanitizer. The suite is re-runnable: two consecutive
`ctest` invocations both report 34/34.

| Suite | Checks | Failures |
| --- | ---: | ---: |
| `units_test` | 18 367 | 0 |
| `identity_test` | 424 | 0 |
| `model_test` | 477 | 0 |
| `claim_region_test` | 479 | 0 |
| `accounting_test` | 201 | 0 |
| `rollup_test` | 496 | 0 |
| `fit_test` | 6 537 | 0 |
| `observation_test` | 135 | 0 |
| `diff_test` | 363 | 0 |
| `persistence_test` | 290 | 0 |
| `corruption_test` | 131 | 0 |
| `adversarial_test` | 9 655 | 0 |
| `property_test` | 60 876 | 0 |
| `randomized_test` | 15 621 | 0 |
| `concurrency_test` | 108 | 0 |
| `multiprocess_test` | 105 | 0 |
| `installed_api_test` | 120 | 0 |
| **Total** | **114 385** | **0** |

The randomized suites print their seeds, so every run is exactly replayable.
`fit_test` and `property_test` compare the library against independent reference
implementations written inside the tests: a brute-force grid count for union
area, a brute-force placement search for fit completeness, and a reference that
walks the containment chain itself for the rollup.

### 12.3 Process death and writer authority

`multiprocess_test` passes 105 of 105 checks in Release and in Debug. It kills a
real independent process at each of the ten durable steps of the commit
protocol and, after every death, reopens the store and confirms exactly one
whole generation, a passing audit, the expected presence or absence of the
record the dead process was creating, an accepted further commit, and no
staging residue. It then holds the writer lease in one real process while a
second real process is refused with `lock_conflict`, terminates the holder, and
confirms the next process acquires the lease immediately.

### 12.4 Sanitizer

The whole 34-entry suite runs clean under **AddressSanitizer** (MSVC
`/fsanitize=address`, Debug runtime), with no report of any kind. MSVC provides
no UndefinedBehaviorSanitizer, so undefined-behaviour checking on this platform
rests on the compiler, the invariant tests and the adversarial suite.

An additional throwaway sweep, linked against the AddressSanitizer build and run
outside the repository, reported **zero defects**:

* every single-bit flip and every `0x00`/`0xFF` replacement across a 527-byte
  frame — 1 581 mutations, none accepted as a different valid state;
* every truncation of that frame at every length, and 64 extensions — all
  refused;
* 20 000 randomly generated buffers, and 5 000 frames spliced from a real frame
  and random noise — all refused, no crash;
* 100 extreme width/depth pairs and 5 700 out-of-range rack-unit spans through
  the public API — every one classified, no crash, no unclassified outcome;
* 200 pathological rectangle layouts compared against a brute-force union
  count — all exact;
* a directory, `..` and the empty path as the state path — all refused with
  `path_rejected`, nothing created.

### 12.5 Install and downstream

`cmake --install` produced a complete package at an arbitrary prefix: 23 public
headers under `include/dccp/space_capacity`, the static library, `spacecap`, the
versioned `SpaceCapacityConfig.cmake` and `SpaceCapacityConfigVersion.cmake`, the
exported `SpaceCapacityTargets.cmake` with the namespaced target
`dccp::space_capacity`, and `README.md`, `LICENSE` and `NOTICE`.

An independent CMake project in `tests/downstream/` — built out of tree against
that prefix with `find_package(SpaceCapacity 1.0 REQUIRED)` — configured,
compiled under `/W4 /WX` with zero warnings, and ran a real minimal lifecycle:
create a store, create a site and a hall, take 10 m² of a 100 m² floor with a
committed claim, check that the available area fell from 100 000 000 to
90 000 000 mm², run a fit, verify, close, reopen read-only, compare digests, and
remove its residue. Result: `downstream consumer: PASS`, exit 0, **no residue**.

### 12.6 Benchmark

The workload is **SYNTHETIC**: one hall of 60 m with 400 racks, built through
the public API. Median of five completed-operation runs, with the observed
spread, on the machine named above.

| Case | Median | Spread | Operations |
| --- | ---: | --- | ---: |
| in-memory mutation | 2 106.8 µs/op | 2 022.3 – 2 769.2 | 2 000 |
| durable mutation (write + flush + publish + verify) | 7 179.7 µs/op | 6 780.2 – 8 379.0 | 40 |
| whole-model rollup | 543.3 µs/op | 541.4 – 555.8 | 5 |
| whole-model audit | 1 277.1 µs/op | 1 254.6 – 1 282.9 | 5 |
| rack-unit fit assessment | 891.3 µs/op | 806.6 – 969.0 | 2 000 |
| canonical encode | 149.8 µs/op | 146.7 – 189.0 | 5 |
| frame + decode + audit | 2 722.8 µs/op | 2 557.1 – 2 920.5 | 5 |

The canonical payload for that model is 62 637 bytes. The durable figure
contains the staging write, the device flush, the atomic publish and the
read-back verification; nothing durable is excluded from it. The benchmark
re-reads the published state afterwards (revision 18, 18 records) and removes
every file it created, and the run left **no residue**. There are no before/after
pairs in this release, so no comparison is published that a methodology would
have to isolate.

---

## 13. Repository contents

| Document | Contents |
| --- | --- |
| `README.md` | this file: boundary, model, semantics, usage, validation, limitations |
| `CONTRIBUTING.md` | licensing of contributions, scope rules, build and test expectations |
| `LICENSE` | Apache License 2.0 |
| `NOTICE` | copyright, DCCP placement and third-party notice |
| `include/dccp/space_capacity/` | the public headers; `space_capacity.hpp` is the umbrella |
| `src/` | the implementation; nothing in `src/` is installed |
| `examples/` | the public API in use |
| `tools/` | `spacecap` and the process-level proof harnesses |
| `benchmarks/` | the completed-operation benchmark |
| `tests/` | the test suite, including `tests/downstream/`, an independent consumer of the installed package |

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
