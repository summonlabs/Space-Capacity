// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal platform layer: bounded file IO, durable flush, atomic replacement,
// directory synchronisation and advisory writer locking. Not installed.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/error.hpp"

namespace dccp::space_capacity::internal {

// Reads at most `max_bytes` from a regular file. A file larger than the bound
// is refused with `oversized` before any buffer of that size is allocated, and
// a file that grows between the size check and the read is refused the same
// way rather than being silently truncated.
Result<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes);

// Writes a whole file and flushes it to the device before returning. The file
// is created if absent and replaced if present. A partial write is reported,
// never ignored.
Status write_file_flushed(const std::filesystem::path& path, std::string_view bytes);

// Flushes an already written file to the device.
Status flush_file(const std::filesystem::path& path);

// Atomically replaces `target` with `source`. This is the commit point of
// every durable publication in this library. `source` must be a regular file
// in the same directory as `target`.
Status atomic_replace(const std::filesystem::path& source, const std::filesystem::path& target);

// Flushes a directory entry so that a rename survives a power loss. A real
// fsync on POSIX; a documented no-op on Windows, where the replace itself is
// issued with MOVEFILE_WRITE_THROUGH.
Status sync_directory(const std::filesystem::path& directory);

Status remove_file(const std::filesystem::path& path) noexcept;

bool path_is_regular_file(const std::filesystem::path& path) noexcept;
bool file_exists(const std::filesystem::path& path) noexcept;

// True when the path is a symbolic link, a Windows reparse point or a
// junction. A state file that is one of those is refused rather than followed,
// so a link cannot redirect a store to a different file.
bool is_link_or_reparse_point(const std::filesystem::path& path) noexcept;

// Size of a regular file, or an error when it is not one.
Result<std::uint64_t> path_file_size(const std::filesystem::path& path);

// Creates the parent directory of a path when it does not exist.
Status ensure_parent_directory(const std::filesystem::path& path);

// Refuses a path that is empty, that names a directory, that ends in `.` or
// `..`, or that is a link or reparse point. Called before any file in the set
// is touched.
Status validate_state_path(const std::filesystem::path& path, bool allow_missing);

// Names of the entries of a directory, in a deterministic order. An entry
// count above `max_entries` is refused.
Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory,
                                                std::uint32_t max_entries);

std::uint64_t process_id() noexcept;

// An exclusive or shared advisory lock over one file, held by the process that
// acquired it and released by the operating system when that process dies.
class FileLock final {
 public:
  FileLock() = default;
  ~FileLock();

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  // Never waits. Contention is reported as `lock_conflict`.
  [[nodiscard]] static Result<FileLock> acquire(const std::filesystem::path& path, bool exclusive);

  [[nodiscard]] bool held() const noexcept { return handle_ != nullptr || fd_ >= 0; }
  [[nodiscard]] bool exclusive() const noexcept { return exclusive_; }

  Status release();

  // Replaces the diagnostic contents of the lock file. The contents are never
  // authority: authority is the held lock, and a lock file left behind by a
  // dead process is overwritten the moment the lock is genuinely acquired.
  Status write_holder(std::string_view text);

 private:
  void* handle_ = nullptr;
  int fd_ = -1;
  bool exclusive_ = false;
  std::filesystem::path path_{};
};

}  // namespace dccp::space_capacity::internal
