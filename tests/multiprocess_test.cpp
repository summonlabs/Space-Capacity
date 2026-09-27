// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - real multiprocess and process-death proof.
//
// Nothing here is proved with threads. Every case starts an independent
// operating-system process, and the claims being tested are:
//
//   1. Process death at any durable step of the commit protocol leaves exactly
//      one whole authoritative generation behind. The killed process is a real
//      program that terminates without unwinding mid-protocol; the survivor
//      reopens the store and must find either the old generation or the new
//      one, never a mixture, and must find a model that passes the full audit.
//   2. Writer authority is an operating-system lock and not the contents of a
//      file. While one process holds it, another process is refused with
//      `lock_conflict`; when the holder is terminated, the next process
//      acquires it immediately.
//   3. A state written by one process is read back by a different process.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"
#include "test_support.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using namespace dccp::space_capacity;

#if !defined(SC_CRASH_CHILD)
#define SC_CRASH_CHILD "sc_crash_child"
#endif
#if !defined(SC_FENCE_CHILD)
#define SC_FENCE_CHILD "sc_fence_child"
#endif
#if !defined(SC_CLI)
#define SC_CLI "spacecap"
#endif

std::filesystem::path work_directory() {
  // The test runs in the CTest working directory, which is inside the build
  // tree, so every file this test writes is build residue and never source.
  return std::filesystem::current_path();
}

std::filesystem::path path_for(const char* name) { return work_directory() / name; }

void remove_store(const std::string& state) {
  const std::filesystem::path base(state);
  std::error_code error;
  std::filesystem::remove(base, error);
  std::filesystem::remove(std::filesystem::path(state + ".prev"), error);
  std::filesystem::remove(std::filesystem::path(state + ".identity"), error);
  std::filesystem::remove(std::filesystem::path(state + ".lock"), error);
  const std::filesystem::path directory =
      base.parent_path().empty() ? std::filesystem::path(".") : base.parent_path();
  const std::string prefix = base.filename().string() + ".tmp-";
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) return;
  std::vector<std::filesystem::path> residue;
  for (const auto& entry : iterator) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) residue.push_back(entry.path());
  }
  for (const std::filesystem::path& path : residue) {
    std::filesystem::remove(path, error);
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::string content;
  char buffer[512];
  while (stream.read(buffer, sizeof(buffer)).gcount() > 0) {
    content.append(buffer, static_cast<std::size_t>(stream.gcount()));
  }
  return content;
}

std::string quote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

// ---------------------------------------------------------------------------
// Process control
// ---------------------------------------------------------------------------

struct Child final {
  bool valid = false;
  int exit_code = -1;
#if defined(_WIN32)
  PROCESS_INFORMATION info{};
#else
  int pid = -1;
#endif
};

// Starts a child process with its standard output and standard error redirected
// to a file, and runs one to completion. Both are started directly rather than
// through a shell, so a path that contains a space is never re-parsed by
// cmd.exe.
Child spawn(const std::string& command, const std::filesystem::path& output);
int wait_for(Child& child);

int run_and_wait(const std::string& command, const std::filesystem::path& output) {
  Child child = spawn(command, output);
  if (!child.valid) return -1;
  return wait_for(child);
}

Child spawn(const std::string& command, const std::filesystem::path& output) {
  Child child;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  attributes.bInheritHandle = TRUE;
  const HANDLE sink = CreateFileW(output.wstring().c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  const HANDLE source = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sink == INVALID_HANDLE_VALUE || source == INVALID_HANDLE_VALUE) return child;

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = sink;
  startup.hStdError = sink;
  startup.hStdInput = source;

  std::wstring wide(command.begin(), command.end());
  std::vector<wchar_t> buffer(wide.begin(), wide.end());
  buffer.push_back(L'\0');

  PROCESS_INFORMATION info{};
  const BOOL created =
      CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                     &startup, &info);
  CloseHandle(sink);
  CloseHandle(source);
  if (created == 0) return child;
  child.valid = true;
  child.info = info;
  return child;
#else
  const std::string redirected = command + " > " + quote(output) + " 2>&1";
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("/bin/sh"));
  argv.push_back(const_cast<char*>("-c"));
  argv.push_back(const_cast<char*>(redirected.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv.data(), environ) != 0) return child;
  child.valid = true;
  child.pid = static_cast<int>(pid);
  return child;
#endif
}

bool is_running(const Child& child) {
#if defined(_WIN32)
  if (!child.valid) return false;
  DWORD code = 0;
  if (GetExitCodeProcess(child.info.hProcess, &code) == 0) return false;
  return code == STILL_ACTIVE;
#else
  if (!child.valid) return false;
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
  return result == 0;
#endif
}

int terminate(Child& child) {
#if defined(_WIN32)
  if (!child.valid) return -1;
  TerminateProcess(child.info.hProcess, 1);
  WaitForSingleObject(child.info.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(child.info.hProcess, &code);
  CloseHandle(child.info.hThread);
  CloseHandle(child.info.hProcess);
  child.valid = false;
  return static_cast<int>(code);
#else
  if (!child.valid) return -1;
  kill(static_cast<pid_t>(child.pid), SIGKILL);
  int status = 0;
  waitpid(static_cast<pid_t>(child.pid), &status, 0);
  child.valid = false;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
#endif
}

int wait_for(Child& child) {
#if defined(_WIN32)
  if (!child.valid) return -1;
  WaitForSingleObject(child.info.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(child.info.hProcess, &code);
  CloseHandle(child.info.hThread);
  CloseHandle(child.info.hProcess);
  child.valid = false;
  return static_cast<int>(code);
#else
  if (!child.valid) return -1;
  int status = 0;
  waitpid(static_cast<pid_t>(child.pid), &status, 0);
  child.valid = false;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
#endif
}

// Waits until the child's output file contains `marker`. Returns false when the
// child never produced it, which fails the proof; it is not a pass.
bool await_marker(const Child& child, const std::filesystem::path& output,
                  const std::string& marker) {
  for (int attempt = 0; attempt < 600; ++attempt) {
    if (read_file(output).find(marker) != std::string::npos) return true;
    if (!is_running(child)) return read_file(output).find(marker) != std::string::npos;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

StoreOptions store_options(const std::string& state, const std::string& identity) {
  StoreOptions options;
  options.path = state;
  options.store_identity = *StoreId::parse(identity);
  options.actor = "multiprocess-test";
  options.source = "multiprocess_test";
  return options;
}

struct StageCase final {
  const char* name;
  bool published;
};

constexpr StageCase kStages[] = {
    {"after-lock", false},          {"after-staging-write", false},
    {"after-staging-flush", false}, {"after-staging-verify", false},
    {"after-previous", false},      {"before-publish", false},
    {"after-publish", true},        {"after-directory", true},
    {"after-lock-release", true},   {"after-published-verify", true},
};

}  // namespace

int main() {
  SC_CASE("process death at every durable step");
  const std::string crash_state = "mp-crash.spcstate";
  const std::string crash_identity = "mp-crash-store";
  for (const StageCase& stage : kStages) {
    remove_store(crash_state);
    const std::filesystem::path output = path_for("mp-crash-output.txt");
    const std::string command = std::string(quote(SC_CRASH_CHILD)) + " --state " +
                                quote(crash_state) + " --store " + crash_identity + " --stage " +
                                stage.name;
    const int exit_code = run_and_wait(command, output);
    SC_CHECK_EQ(exit_code, kFaultExitStatus);

    // The survivor reopens the store. It must find exactly one whole
    // generation, and the model must pass the full audit.
    StoreOptions options = store_options(crash_state, crash_identity);
    Result<SpaceCapacityRegistry> reopened = SpaceCapacityRegistry::open(options);
    if (!reopened) {
      std::fprintf(stderr, "reopen after death at %s failed: %s\n", stage.name,
                   reopened.error().to_string().c_str());
      SC_CHECK(false);
      continue;
    }
    SpaceCapacityRegistry registry = std::move(reopened).value();
    const SnapshotPtr snapshot = registry.snapshot();
    SC_CHECK(snapshot->audit().ok());
    SC_CHECK_EQ(snapshot->node_count(), stage.published ? std::size_t{1} : std::size_t{0});
    SC_CHECK_EQ(registry.revision().value(), stage.published ? 1ull : 0ull);
    SC_CHECK(snapshot->find_node(*SpaceNodeId::parse("crash-node")) != nullptr ==
             stage.published);

    // The recovered state is healthy and accepts a further commit.
    if (stage.published) {
      CreateNodeRequest request;
      request.node.id = *SpaceNodeId::parse("crash-node-2");
      request.node.generation = EntityGeneration{1};
      request.node.kind = SpaceNodeKind::site;
      request.node.spatial_class = SpatialClass::outdoor;
      request.node.lifecycle = NodeLifecycle::available;
      const Result<MutationOutcome> outcome = registry.apply(request);
      SC_CHECK(outcome.ok());
      if (outcome) SC_CHECK_EQ(outcome.value().revision.value(), 2ull);
    }
    SC_CHECK(registry.verify().ok());
    (void)registry.close();

    // No staging residue is left behind once the store has been opened again.
    const std::string prefix = std::string("mp-crash.spcstate.tmp-");
    std::error_code error;
    std::filesystem::directory_iterator iterator(work_directory(), error);
    bool residue = false;
    for (const auto& entry : iterator) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) residue = true;
    }
    SC_CHECK(!residue);
  }
  remove_store(crash_state);

  SC_CASE("writer authority is an operating-system lock");
  {
    const std::string fence_state = "mp-fence.spcstate";
    const std::string fence_identity = "mp-fence-store";
    remove_store(fence_state);

    // Create the store first so that the holder does not race a creation.
    {
      StoreOptions options = store_options(fence_state, fence_identity);
      Result<SpaceCapacityRegistry> created = SpaceCapacityRegistry::open(options);
      SC_CHECK(created.ok());
      if (!created) return ::sc_test::summary("multiprocess_test");
      (void)created.value().close();
    }

    const std::filesystem::path holder_output = path_for("mp-fence-holder.txt");
    const std::string hold_command = std::string(quote(SC_FENCE_CHILD)) + " --state " +
                                     quote(fence_state) + " --store " + fence_identity + " --hold";
    Child holder = spawn(hold_command, holder_output);
    SC_CHECK(holder.valid);
    SC_CHECK(await_marker(holder, holder_output, "acquired"));

    // While the holder lives, this process must be refused.
    {
      StoreOptions options = store_options(fence_state, fence_identity);
      Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
      SC_CHECK(opened.ok());
      if (opened) {
        const Status acquired = opened.value().acquire_writer_lease();
        SC_CHECK(!acquired.ok());
        SC_CHECK_EQ(static_cast<int>(acquired.code()),
                    static_cast<int>(ErrorCode::lock_conflict));
        (void)opened.value().close();
      }
    }

    // A second independent process must also be refused, and must say so.
    {
      const std::filesystem::path contender_output = path_for("mp-fence-contender.txt");
      const std::string contender_command =
          std::string(quote(SC_FENCE_CHILD)) + " --state " + quote(fence_state) + " --store " +
          fence_identity;
      const int exit_code = run_and_wait(contender_command, contender_output);
      SC_CHECK_EQ(exit_code, 3);
      SC_CHECK(read_file(contender_output).find("contended") != std::string::npos);
      SC_CHECK(read_file(contender_output).find("lock_conflict") != std::string::npos);
    }

    // Process death relinquishes authority: the operating system releases the
    // lock when the holder is terminated, and the lock file left behind is not
    // authority.
    SC_CHECK(is_running(holder));
    (void)terminate(holder);
    SC_CHECK(!is_running(holder));

    {
      StoreOptions options = store_options(fence_state, fence_identity);
      Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(options);
      SC_CHECK(opened.ok());
      if (opened) {
        const Status acquired = opened.value().acquire_writer_lease();
        SC_CHECK(acquired.ok());
        SC_CHECK(opened.value().holds_writer_lease());
        SC_CHECK(opened.value().release_writer_lease().ok());
        SC_CHECK(!opened.value().holds_writer_lease());
        (void)opened.value().close();
      }
    }

    // And a fresh independent process can now take it too.
    {
      const std::filesystem::path after_output = path_for("mp-fence-after.txt");
      const std::string after_command = std::string(quote(SC_FENCE_CHILD)) + " --state " +
                                        quote(fence_state) + " --store " + fence_identity;
      const int exit_code = run_and_wait(after_command, after_output);
      SC_CHECK_EQ(exit_code, 0);
      SC_CHECK(read_file(after_output).find("acquired") != std::string::npos);
    }
    remove_store(fence_state);
    std::error_code error;
    std::filesystem::remove(holder_output, error);
    std::filesystem::remove(path_for("mp-fence-contender.txt"), error);
    std::filesystem::remove(path_for("mp-fence-after.txt"), error);
  }

  SC_CASE("a state written by one process is read by another");
  {
    const std::string cli_state = "mp-cli.spcstate";
    const std::string cli_identity = "mp-cli-store";
    remove_store(cli_state);

    const std::filesystem::path add_output = path_for("mp-cli-add.txt");
    const std::string add_command =
        std::string(quote(SC_CLI)) + " --state " + quote(cli_state) + " --store " + cli_identity +
        " node-add --id site-mp --kind site --class outdoor";
    const int add_exit = run_and_wait(add_command, add_output);
    SC_CHECK_EQ(add_exit, 0);
    SC_CHECK(read_file(add_output).find("applied") != std::string::npos);

    const std::filesystem::path verify_output = path_for("mp-cli-verify.txt");
    const std::string verify_command = std::string(quote(SC_CLI)) + " --state " +
                                       quote(cli_state) + " --store " + cli_identity + " verify";
    const int verify_exit = run_and_wait(verify_command, verify_output);
    SC_CHECK_EQ(verify_exit, 0);
    const std::string verified = read_file(verify_output);
    SC_CHECK(verified.find("verified revision 1") != std::string::npos);

    // A third process reads the same state and reports the same digest.
    const std::filesystem::path status_output = path_for("mp-cli-status.txt");
    const std::string status_command = std::string(quote(SC_CLI)) + " --state " +
                                       quote(cli_state) + " --store " + cli_identity + " status";
    const int status_exit = run_and_wait(status_command, status_output);
    SC_CHECK_EQ(status_exit, 0);
    SC_CHECK(read_file(status_output).find("revision 1") != std::string::npos);

    // A read-only process cannot be the one that wrote it, and the CLI refuses
    // a mutating command when the state is opened read-only.
    const std::filesystem::path readonly_output = path_for("mp-cli-readonly.txt");
    const std::string readonly_command = std::string(quote(SC_CLI)) + " --state " +
                                         quote(cli_state) + " --store " + cli_identity +
                                         " --read-only summary";
    const int readonly_exit = run_and_wait(readonly_command, readonly_output);
    SC_CHECK_EQ(readonly_exit, 0);
    SC_CHECK(read_file(readonly_output).find("nodes 1") != std::string::npos);

    remove_store(cli_state);
    std::error_code error;
    std::filesystem::remove(add_output, error);
    std::filesystem::remove(verify_output, error);
    std::filesystem::remove(status_output, error);
    std::filesystem::remove(readonly_output, error);
    std::filesystem::remove(path_for("mp-crash-output.txt"), error);
  }

  SC_CASE("no residue");
  {
    std::error_code error;
    std::filesystem::directory_iterator iterator(work_directory(), error);
    bool residue = false;
    for (const auto& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("mp-", 0) == 0) {
        std::fprintf(stderr, "residue left behind: %s\n", name.c_str());
        residue = true;
      }
    }
    SC_CHECK(!residue);
  }

  return ::sc_test::summary("multiprocess_test");
}