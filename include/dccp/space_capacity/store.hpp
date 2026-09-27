// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - versioned, integrity-checked durable state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ---------------------------------------------------------------------------
// File set
// ---------------------------------------------------------------------------
//
//   <state>            the committed state image, immutable once published
//   <state>.prev       the previous committed state, retained for recovery
//   <state>.identity   the store identity anchor
//   <state>.lock       the writer lock file, an OS-level exclusive lock target
//   <state>.tmp-<pid>-<seq>  staging files, never authoritative, retired on
//                            every successful commit and on every clean open
//
// Nothing but `<state>` and `<state>.prev` is authoritative, and neither is
// authoritative until its header, its length, its integrity digest and its
// store identity have been checked and the decoded model has passed the same
// audit every read path runs.
//
// ---------------------------------------------------------------------------
// Commit protocol
// ---------------------------------------------------------------------------
//
//   plan -> validate -> reserve generation and attempt -> write staging ->
//   flush -> read back and verify -> retain previous -> atomic publish
//   (commit point) -> flush directory -> release lock -> re-read and verify
//
// The commit point is the atomic replace of the staging file onto the state
// file. Before it, nothing changed. After it, exactly one whole generation is
// visible; there is no window in which a reader sees half of one. The caller's
// in-memory state is published only after the effects have been verified by
// re-reading what was actually written, so acknowledgement and verified effect
// are separate steps and the second one is the one that decides success.
//
// ---------------------------------------------------------------------------
// Writer authority
// ---------------------------------------------------------------------------
//
// Writer authority is an exclusive operating-system lock on the lock file,
// taken for the duration of a commit and, in lease mode, for the lifetime of a
// lease. It is never inferred from the contents of the lock file: a lock file
// left behind by a dead process is a stale file, not a lock, and it is
// overwritten the moment the lock is genuinely acquired. Process death
// therefore relinquishes authority immediately and correctly, which the
// multiprocess tests prove with real independent processes.
//
// ---------------------------------------------------------------------------
// What the store refuses
// ---------------------------------------------------------------------------
//
// A store is refused, with a specific code and before any allocation that its
// declared size would drive, when it is corrupt, truncated, malformed,
// oversized, of an unimplemented format or model version, written by a
// different byte order, written under a different batch coordinate model, or
// carrying a store identity that does not match its anchor. A swapped or
// unrelated file at the expected path is refused, never adopted.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/export.hpp"
#include "dccp/space_capacity/observation.hpp"
#include "dccp/space_capacity/snapshot.hpp"
#include "dccp/space_capacity/strong_id.hpp"

namespace dccp::space_capacity {

// ---------------------------------------------------------------------------
// Layout constants. Part of the durable format; never renumbered.
// ---------------------------------------------------------------------------

// The frame is a fixed 136-byte header, then the store identity text, then an
// 8-byte creation instant, then the payload, then a 40-byte trailer.
//
// The header carries an early CRC-32 check value so a damaged header is refused
// before any field in it is interpreted, and the trailer carries a
// domain-separated SHA-256 over every byte that precedes it. The store identity
// is written as text rather than only as a digest so that a reader can tell
// which store an image belongs to without a side file, and so that a swapped
// image is refused by name.
inline constexpr std::size_t kStoreHeaderBytes = 136;
inline constexpr std::size_t kStoreTrailerBytes = 40;
inline constexpr std::size_t kMaxIdentityBlockBytes = 128;
inline constexpr std::uint32_t kEndianTag = 0x01020304u;
inline constexpr std::string_view kStoreTrailerMagic = "SPCEND";
inline constexpr std::string_view kStateSuffix = ".spcstate";
inline constexpr std::string_view kPreviousSuffix = ".prev";
inline constexpr std::string_view kIdentitySuffix = ".identity";
inline constexpr std::string_view kLockSuffix = ".lock";
inline constexpr std::string_view kStagingMarker = ".tmp-";
inline constexpr std::uint64_t kMaxRetainedGenerations = 4;

// The default state file a CLI invocation uses when no path is given.
inline constexpr std::string_view kDefaultStateFile = "space-capacity.spcstate";

// ---------------------------------------------------------------------------
// Fault injection for failure-proof tests
// ---------------------------------------------------------------------------

// Every durable step the commit protocol passes through, in order.
enum class WriteStage : std::uint8_t {
  none = 0,
  lock_acquired = 1,
  staging_planned = 2,
  staging_written = 3,
  staging_flushed = 4,
  staging_verified = 5,
  previous_retained = 6,
  before_publish = 7,
  after_publish = 8,
  directory_flushed = 9,
  lock_released = 10,
  published_verified = 11,
};

inline constexpr std::size_t kWriteStageCount = 12;

SC_API std::string_view write_stage_name(WriteStage stage) noexcept;

enum class FaultAction : std::uint8_t {
  none = 0,
  // Return an error at the stage instead of completing it.
  fail = 1,
  // Terminate the process without unwinding, at the stage. Used to prove that
  // process death at a durable step leaves exactly one whole generation.
  crash = 2,
};

using FaultHook = std::function<void(WriteStage)>;

// The exit status a crash injection uses. Distinct from every status the CLI
// returns for an ordinary error.
inline constexpr int kFaultExitStatus = 9;

// ---------------------------------------------------------------------------
// Options and reports
// ---------------------------------------------------------------------------

enum class OpenMode : std::uint8_t {
  read_only = 0,
  read_write = 1,
};

enum class CreateMode : std::uint8_t {
  must_exist = 0,
  create_if_missing = 1,
};

enum class RecoveryAction : std::uint8_t {
  none = 0,
  created = 1,
  loaded_current = 2,
  loaded_previous = 3,
  no_state_accepted = 4,
  retired_staging = 5,
};

SC_API std::string_view recovery_action_name(RecoveryAction action) noexcept;

struct SC_API StoreOptions final {
  // Path of the state file. Not a directory.
  std::filesystem::path path{std::string(kDefaultStateFile)};

  // The durable identity of this store. When it is empty, the identity is
  // derived from the state file's own name, which keeps the library free of
  // any entropy source and of any hidden randomness. An operator who wants a
  // name that is not a valid DCCP identifier supplies one explicitly.
  std::optional<StoreId> store_identity{};

  OpenMode mode = OpenMode::read_write;
  CreateMode create = CreateMode::create_if_missing;

  // Keep the previous generation beside the state file. Recovery prefers the
  // current generation and falls back to the previous one only when the
  // current is refused, and it says which happened.
  bool retain_previous = true;

  // Operator identity recorded in the lock file and in provenance fields.
  // Bounded, validated, and never used as authority.
  std::string actor{"unknown"};
  std::string source{"library"};

  // Terminate or fail at a named durable step. Empty in production.
  FaultHook fault_hook{};

  // Timeout-free: the lock is never waited for. Contention is reported.
  bool never_wait_for_lock = true;

  friend bool operator==(const StoreOptions&, const StoreOptions&) = delete;
};

struct SC_API StoreStatus final {
  std::filesystem::path path{};
  std::filesystem::path identity_path{};
  bool state_exists = false;
  bool previous_exists = false;
  bool identity_exists = false;
  bool lock_held_by_this_process = false;
  bool lock_held_by_another_process = false;
  std::uint64_t state_bytes = 0;
  std::uint64_t previous_bytes = 0;
  StoreId store{};
  StoreIncarnation incarnation{};
  RegistryRevision revision{};
  AttemptId attempt{};
  Digest digest{};
};

struct SC_API RecoveryReport final {
  RecoveryAction action = RecoveryAction::none;
  // Stage of the interrupted commit that was detected, when one was.
  WriteStage interrupted_at = WriteStage::none;
  std::uint32_t retired_staging_files = 0;
  std::uint32_t quarantined_files = 0;
  bool previous_was_used = false;
  ExplanationSet explanations{};
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

class SC_API Store final {
 public:
  Store() = default;
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;

  // Opens or creates a store. On success the authoritative state is loaded,
  // integrity-checked, decoded and audited, and exactly one whole state is
  // current: never a mixture of the state file and its previous generation.
  [[nodiscard]] static Result<Store> open(const StoreOptions& options);

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] const StoreId& store_id() const noexcept { return store_; }
  [[nodiscard]] StoreIncarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] RegistryRevision revision() const noexcept { return revision_; }
  [[nodiscard]] AttemptId attempt() const noexcept { return attempt_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] const SnapshotPtr& snapshot() const noexcept { return snapshot_; }

  [[nodiscard]] Result<StoreStatus> status() const;

  // Writes the next generation. `next` must be a snapshot built from this
  // store's current state, must carry the same store identity and incarnation,
  // and must be exactly one revision ahead. Returns the attempt identifier the
  // commit was made under.
  [[nodiscard]] Result<AttemptId> commit(const SnapshotPtr& next);

  // Re-reads the state file from disk and verifies that its digest is the one
  // this store believes is committed. Used by the CLI's `verify` command.
  [[nodiscard]] Status verify_on_disk() const;

  // Re-reads the state file and verifies that it is exactly the snapshot that
  // was just committed. This is the verified-effect step of the commit
  // protocol, and it runs before the store accepts the new revision.
  [[nodiscard]] Status verify_published(const Snapshot& expected) const;

  // Takes the writer lease, which is an exclusive operating-system lock held
  // until release or process death. Contention is reported as lock_conflict and
  // is never waited out.
  [[nodiscard]] Status acquire_writer_lease();
  [[nodiscard]] Status release_writer_lease();
  [[nodiscard]] bool holds_writer_lease() const noexcept;

  // Closes the store and releases the lease. Idempotent.
  [[nodiscard]] Status close();

  // Decodes and audits a state image without opening a store. Used by the
  // `verify` and `inspect` CLI commands, and by tests, and it is at least as
  // strict as the open path.
  struct Decoded final {
    StoreId store{};
    StoreIncarnation incarnation{};
    RegistryRevision revision{};
    AttemptId attempt{};
    Timestamp created_at{};
    // Digest of the whole frame, which is what the trailer carries.
    Digest digest{};
    // Digest of the payload, which is exactly `Snapshot::digest()` of the
    // snapshot the image decodes to. This is the content identity an operator
    // compares between two stores.
    Digest content_digest{};
    std::uint32_t format_version = 0;
    std::uint32_t model_revision = 0;
  };
  [[nodiscard]] static Result<Decoded> inspect_image(std::string_view bytes);
  [[nodiscard]] static Result<SnapshotPtr> decode_image(std::string_view bytes);
  [[nodiscard]] static Result<std::string> encode_image(const Snapshot& snapshot);

  // The canonical bytes of one image, exposed for tests and tooling that must
  // corrupt a specific byte.
  [[nodiscard]] static std::string frame(const Snapshot& snapshot);

 private:
  struct LockState;

  StoreOptions options_{};
  bool open_ = false;
  StoreId store_{};
  StoreIncarnation incarnation_{};
  RegistryRevision revision_{};
  AttemptId attempt_{};
  Digest digest_{};
  Timestamp created_at_{};
  SnapshotPtr snapshot_{};
  RecoveryReport recovery_{};
  std::shared_ptr<LockState> lock_;
  std::uint64_t staging_counter_ = 0;
};

}  // namespace dccp::space_capacity
