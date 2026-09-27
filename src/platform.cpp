// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - platform file and lock primitives.
//
// Windows is the exercised platform. The POSIX path exists, is written to the
// same contract, and is not claimed as proved by this repository's validation.
//
// Everything here is defensive: sizes are bounded before allocation, links and
// reparse points are refused rather than followed, and every IO call reports
// its own failure instead of being folded into a generic error.

#include "platform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/space_capacity/limits.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dccp::space_capacity::internal {
namespace {

Error io_error(std::string_view operation, const std::filesystem::path& path,
               std::string_view detail) {
  std::string message(operation);
  message += " failed for ";
  const std::string name = path.filename().string();
  message += name.empty() ? path.string() : name;
  if (!detail.empty()) {
    message += ": ";
    message += detail;
  }
  return Error::with_subject(ErrorCode::io_failure, std::move(message), path.filename().string());
}

}  // namespace

bool path_is_regular_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool file_exists(const std::filesystem::path& path) noexcept {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool is_link_or_reparse_point(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) return false;
  if (std::filesystem::is_symlink(status)) return true;
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

Result<std::uint64_t> path_file_size(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return io_error("size query", path, error.message());
  return static_cast<std::uint64_t>(size);
}

Status validate_state_path(const std::filesystem::path& path, bool allow_missing) {
  if (path.empty()) {
    return Status::failure(ErrorCode::path_rejected, "the state path is empty");
  }
  const std::string name = path.filename().string();
  if (name.empty() || name == "." || name == "..") {
    return Status::failure(ErrorCode::path_rejected,
                           "the state path must name a file, not a directory");
  }
  if (is_link_or_reparse_point(path)) {
    return Status::failure(ErrorCode::path_rejected,
                           "the state path is a link or a reparse point and is refused rather "
                           "than followed");
  }
  if (file_exists(path)) {
    if (!path_is_regular_file(path)) {
      return Status::failure(ErrorCode::path_rejected, "the state path is not a regular file");
    }
  } else if (!allow_missing) {
    return Status::failure(ErrorCode::not_found, "the state path does not exist");
  }
  return Status::success();
}

Status ensure_parent_directory(const std::filesystem::path& path) {
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty()) return Status::success();
  std::error_code error;
  if (std::filesystem::exists(parent, error)) {
    if (error) return io_error("directory query", parent, error.message());
    if (!std::filesystem::is_directory(parent, error)) {
      return Status::failure(ErrorCode::path_rejected,
                             "the parent of the state path is not a directory");
    }
    return Status::success();
  }
  std::filesystem::create_directories(parent, error);
  if (error) return io_error("directory creation", parent, error.message());
  return Status::success();
}

Result<std::string> read_file_bounded(const std::filesystem::path& path,
                                      std::uint64_t max_bytes) {
  if (is_link_or_reparse_point(path)) {
    return Error::with_subject(ErrorCode::path_rejected,
                               "the file is a link or a reparse point", path.filename().string());
  }
  Result<std::uint64_t> size = path_file_size(path);
  if (!size) return size.error();
  if (size.value() > max_bytes) {
    return Error::with_subject(ErrorCode::oversized,
                               "the file declares " + std::to_string(size.value()) +
                                   " bytes, above the bound of " + std::to_string(max_bytes),
                               path.filename().string());
  }
  if (size.value() == 0) return std::string{};

#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("open", path, "the operating system refused the open");
  }
  std::string buffer;
  buffer.resize(static_cast<std::size_t>(size.value()));
  std::size_t total = 0;
  while (total < buffer.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(buffer.size() - total, 1u << 20));
    DWORD read = 0;
    if (ReadFile(handle, buffer.data() + total, chunk, &read, nullptr) == 0) {
      CloseHandle(handle);
      return io_error("read", path, "the operating system refused the read");
    }
    if (read == 0) break;
    total += read;
  }
  CloseHandle(handle);
  if (total != buffer.size()) {
    return Error::with_subject(ErrorCode::truncated_input,
                               "the file shrank while it was being read",
                               path.filename().string());
  }
  return buffer;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return io_error("open", path, std::strerror(errno));
  std::string buffer;
  buffer.resize(static_cast<std::size_t>(size.value()));
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t read = ::read(fd, buffer.data() + total, buffer.size() - total);
    if (read < 0) {
      ::close(fd);
      return io_error("read", path, std::strerror(errno));
    }
    if (read == 0) break;
    total += static_cast<std::size_t>(read);
  }
  ::close(fd);
  if (total != buffer.size()) {
    return Error::with_subject(ErrorCode::truncated_input,
                               "the file shrank while it was being read",
                               path.filename().string());
  }
  return buffer;
#endif
}

Status write_file_flushed(const std::filesystem::path& path, std::string_view bytes) {
  if (is_link_or_reparse_point(path)) {
    return Status::failure(ErrorCode::path_rejected,
                           "the file is a link or a reparse point and is refused");
  }
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("create", path, "the operating system refused the create");
  }
  std::size_t total = 0;
  while (total < bytes.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, bytes.data() + total, chunk, &written, nullptr) == 0) {
      CloseHandle(handle);
      return io_error("write", path, "the operating system refused the write");
    }
    if (written == 0) {
      CloseHandle(handle);
      return io_error("write", path, "the operating system wrote nothing");
    }
    total += written;
  }
  if (FlushFileBuffers(handle) == 0) {
    CloseHandle(handle);
    return io_error("flush", path, "the operating system refused the flush");
  }
  if (CloseHandle(handle) == 0) {
    return io_error("close", path, "the operating system refused the close");
  }
  return Status::success();
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return io_error("open", path, std::strerror(errno));
  std::size_t total = 0;
  while (total < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + total, bytes.size() - total);
    if (written < 0) {
      ::close(fd);
      return io_error("write", path, std::strerror(errno));
    }
    total += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return io_error("flush", path, std::strerror(errno));
  }
  if (::close(fd) != 0) return io_error("close", path, std::strerror(errno));
  return Status::success();
#endif
}

Status flush_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("open for flush", path, "the operating system refused the open");
  }
  const BOOL flushed = FlushFileBuffers(handle);
  CloseHandle(handle);
  if (flushed == 0) return io_error("flush", path, "the operating system refused the flush");
  return Status::success();
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return io_error("open for flush", path, std::strerror(errno));
  const int flushed = ::fsync(fd);
  ::close(fd);
  if (flushed != 0) return io_error("flush", path, std::strerror(errno));
  return Status::success();
#endif
}

Status atomic_replace(const std::filesystem::path& source, const std::filesystem::path& target) {
  if (is_link_or_reparse_point(target)) {
    return Status::failure(ErrorCode::path_rejected,
                           "the publication target is a link or a reparse point");
  }
  if (!path_is_regular_file(source)) {
    return io_error("publish", source, "the staging file is not a regular file");
  }
#if defined(_WIN32)
  if (MoveFileExW(source.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return io_error("atomic replace", target, "the operating system refused the replacement");
  }
  return Status::success();
#else
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return io_error("atomic replace", target, std::strerror(errno));
  }
  return Status::success();
#endif
}

Status sync_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  // Windows has no directory fsync. The replace that precedes this call is
  // issued with MOVEFILE_WRITE_THROUGH, and the staging file was flushed with
  // FlushFileBuffers before the replace, so the ordering guarantee is carried
  // by those two calls rather than by this one.
  (void)directory;
  return Status::success();
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return io_error("directory open", directory, std::strerror(errno));
  const int synced = ::fsync(fd);
  ::close(fd);
  if (synced != 0) return io_error("directory flush", directory, std::strerror(errno));
  return Status::success();
#endif
}

Status remove_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return Status(Error::with_subject(ErrorCode::io_failure, "removing the file failed",
                                      path.filename().string()));
  }
  return Status::success();
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory,
                                                std::uint32_t max_entries) {
  std::error_code error;
  std::vector<std::string> names;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) return io_error("directory listing", directory, error.message());
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    if (names.size() >= max_entries) {
      return Error::with_subject(ErrorCode::limit_exceeded,
                                 "the directory holds more entries than the bound allows",
                                 directory.filename().string());
    }
    names.push_back(iterator->path().filename().string());
    iterator.increment(error);
    if (error) return io_error("directory listing", directory, error.message());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept
    : handle_(other.handle_), fd_(other.fd_), exclusive_(other.exclusive_),
      path_(std::move(other.path_)) {
  other.handle_ = nullptr;
  other.fd_ = -1;
  other.exclusive_ = false;
  other.path_.clear();
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    fd_ = other.fd_;
    exclusive_ = other.exclusive_;
    path_ = std::move(other.path_);
    other.handle_ = nullptr;
    other.fd_ = -1;
    other.exclusive_ = false;
    other.path_.clear();
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, bool exclusive) {
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("lock file open", path, "the operating system refused the open");
  }
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (exclusive) flags |= LOCKFILE_EXCLUSIVE_LOCK;
  if (LockFileEx(handle, flags, 0, 1, 0, &overlapped) == 0) {
    CloseHandle(handle);
    return Error::with_subject(ErrorCode::lock_conflict,
                               "another process holds the writer lock",
                               path.filename().string());
  }
  FileLock lock;
  lock.handle_ = handle;
  lock.exclusive_ = exclusive;
  lock.path_ = path;
  return lock;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return io_error("lock file open", path, std::strerror(errno));
  if (::flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
    ::close(fd);
    return Error::with_subject(ErrorCode::lock_conflict,
                               "another process holds the writer lock",
                               path.filename().string());
  }
  FileLock lock;
  lock.fd_ = fd;
  lock.exclusive_ = exclusive;
  lock.path_ = path;
  return lock;
#endif
}

Status FileLock::release() {
  if (handle_ == nullptr && fd_ < 0) return Status::success();
#if defined(_WIN32)
  if (handle_ != nullptr) {
    OVERLAPPED overlapped{};
    UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
#endif
  return Status::success();
}

Status FileLock::write_holder(std::string_view text) {
  if (!held()) {
    return Status::failure(ErrorCode::writer_lock_invalid,
                           "the lock file cannot be written without holding the lock");
  }
  return write_file_flushed(path_, text);
}

}  // namespace dccp::space_capacity::internal
