#pragma once

#include "apf/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// Platform process and filesystem utilities. These are the only places where
/// platform-specific process handling appears; core governance code is
/// portable.
struct ProcessOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
  /// Child inherits this process's stdout/stderr, which keeps daemon output
  /// visible to the test harness and the operator.
  bool inherit_output{true};
  std::vector<std::pair<std::string, std::string>> environment;
};

/// A spawned child process. No timeouts are used: wait() blocks until the child
/// exits, and terminate() is reserved for tests whose scenario is process death.
class ChildProcess {
 public:
  ChildProcess() noexcept = default;
  ~ChildProcess();

  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  static Result<ChildProcess> spawn(const ProcessOptions& options);

  bool valid() const noexcept;
  bool running();
  std::uint64_t pid() const noexcept;
  /// Blocks until the child exits and returns its exit code.
  Result<int> wait();
  /// Forceful termination. Only used when process death is the scenario under
  /// test; normal shutdown signals the child instead.
  Status terminate();
  int exit_code() const noexcept { return exit_code_; }

 private:
  std::uintptr_t handle_{0};
  std::uint64_t pid_{0};
  int exit_code_{-1};
  bool reaped_{false};
};

/// Absolute path of the currently executing binary.
std::string current_executable_path();
/// Directory containing the currently executing binary, which is where the
/// coordination tools live next to the test binaries in a build tree.
std::string current_executable_directory();
/// Sibling executable path, used by tests and tools to locate apfcoord and
/// apfworker without hard-coded machine paths.
Result<std::string> sibling_executable(std::string_view name);

std::string temp_directory();
std::string join_path(std::string_view base, std::string_view leaf);
bool is_path_safe(std::string_view path);
bool file_exists(std::string_view path);
std::uint64_t file_size(std::string_view path);
Status remove_file(std::string_view path);
Status ensure_directory(std::string_view path);
Status remove_directory_recursive(std::string_view path);
Result<std::string> read_file(std::string_view path);
/// write temporary -> flush -> verify -> replace authoritative file.
Status write_file_atomic(std::string_view path, std::string_view data);
Result<std::vector<std::string>> list_directory(std::string_view path);
/// Monotonic-ish entropy for boot identity derivation.
std::uint64_t entropy_seed();
/// Creates a unique temporary directory beneath the system temp directory.
Result<std::string> make_temp_directory(std::string_view prefix);

}  // namespace apf
