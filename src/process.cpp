#include "apf/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <climits>
#endif

namespace apf {
namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() {
#if defined(_WIN32)
  if (handle_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = 0;
  }
#endif
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), exit_code_(other.exit_code_), reaped_(other.reaped_) {
  other.handle_ = 0;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
#if defined(_WIN32)
    if (handle_ != 0) {
      CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
#endif
    handle_ = other.handle_;
    pid_ = other.pid_;
    exit_code_ = other.exit_code_;
    reaped_ = other.reaped_;
    other.handle_ = 0;
    other.pid_ = 0;
  }
  return *this;
}

bool ChildProcess::valid() const noexcept { return handle_ != 0; }

std::uint64_t ChildProcess::pid() const noexcept { return pid_; }

Result<ChildProcess> ChildProcess::spawn(const ProcessOptions& options) {
  if (options.executable.empty()) {
    return make_error(ErrorCode::InvalidArgument, "process executable is empty");
  }
  if (options.arguments.size() > 64) {
    return make_error(ErrorCode::LimitExceeded, "process argument list is too long");
  }
#if defined(_WIN32)
  // Arguments are passed as a properly quoted wide command line. No shell is
  // ever involved, so no caller-supplied text can become a command.
  const auto quote = [](const std::wstring& value) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : value) {
      if (ch == L'\\') {
        ++backslashes;
        continue;
      }
      if (ch == L'"') {
        out.append(backslashes * 2 + 1, L'\\');
        out.push_back(L'"');
        backslashes = 0;
        continue;
      }
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
  };
  const std::wstring application = widen(options.executable);
  std::wstring command = quote(application);
  for (const std::string& argument : options.arguments) {
    command += L" ";
    command += quote(widen(argument));
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const std::wstring working = widen(options.working_directory);
  std::wstring environment_block;
  void* environment_pointer = nullptr;
  if (!options.environment.empty()) {
    for (const auto& entry : options.environment) {
      environment_block += widen(entry.first);
      environment_block.push_back(L'=');
      environment_block += widen(entry.second);
      environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');
    environment_pointer = environment_block.data();
  }
  const BOOL created = CreateProcessW(
      application.empty() ? nullptr : application.c_str(), mutable_command.data(), nullptr,
      nullptr, options.inherit_output ? TRUE : FALSE, 0, environment_pointer,
      options.working_directory.empty() ? nullptr : working.c_str(), &startup, &info);
  if (created == FALSE) {
    return make_error(ErrorCode::DeviceUnavailable, "failed to spawn child process",
                      options.executable + " error=" + std::to_string(GetLastError()));
  }
  CloseHandle(info.hThread);
  ChildProcess process;
  process.handle_ = reinterpret_cast<std::uintptr_t>(info.hProcess);
  process.pid_ = static_cast<std::uint64_t>(info.dwProcessId);
  return process;
#else
  std::vector<std::string> storage;
  storage.push_back(options.executable);
  for (const std::string& argument : options.arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& entry : storage) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) {
    return make_error(ErrorCode::DeviceUnavailable, "failed to fork child process");
  }
  if (child == 0) {
    if (!options.working_directory.empty()) {
      if (chdir(options.working_directory.c_str()) != 0) {
        _exit(127);
      }
    }
    if (!options.inherit_output) {
      // Detach the child's stdio from the parent when output inheritance is off.
      (void)freopen("/dev/null", "w", stdout);
      (void)freopen("/dev/null", "w", stderr);
    }
    execv(options.executable.c_str(), argv.data());
    _exit(127);
  }
  ChildProcess process;
  process.handle_ = static_cast<std::uintptr_t>(child);
  process.pid_ = static_cast<std::uint64_t>(child);
  return process;
#endif
}

bool ChildProcess::running() {
#if defined(_WIN32)
  if (handle_ == 0) {
    return false;
  }
  DWORD code = 0;
  if (GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code) == FALSE) {
    return false;
  }
  if (code == STILL_ACTIVE) {
    return true;
  }
  exit_code_ = static_cast<int>(code);
  return false;
#else
  if (handle_ == 0 || reaped_) {
    return false;
  }
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(handle_), &status, WNOHANG);
  if (result == static_cast<pid_t>(handle_)) {
    reaped_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
  }
  return true;
#endif
}

Result<int> ChildProcess::wait() {
#if defined(_WIN32)
  if (handle_ == 0) {
    return make_error(ErrorCode::InvalidArgument, "process handle is not valid");
  }
  // No timeout: the caller waits for the child to finish naturally.
  if (WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE) != WAIT_OBJECT_0) {
    return make_error(ErrorCode::Internal, "failed to wait for the child process");
  }
  DWORD code = 0;
  if (GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code) == FALSE) {
    return make_error(ErrorCode::Internal, "failed to read the child exit code");
  }
  exit_code_ = static_cast<int>(code);
  reaped_ = true;
  return exit_code_;
#else
  if (handle_ == 0) {
    return make_error(ErrorCode::InvalidArgument, "process handle is not valid");
  }
  if (reaped_) {
    return exit_code_;
  }
  int status = 0;
  if (waitpid(static_cast<pid_t>(handle_), &status, 0) < 0) {
    return make_error(ErrorCode::Internal, "failed to wait for the child process");
  }
  reaped_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return exit_code_;
#endif
}

Status ChildProcess::terminate() {
#if defined(_WIN32)
  if (handle_ == 0) {
    return failure(ErrorCode::InvalidArgument, "process handle is not valid");
  }
  if (!running()) {
    // Terminating a process that has already exited is a successful no-op.
    return success();
  }
  if (TerminateProcess(reinterpret_cast<HANDLE>(handle_), 9) == FALSE) {
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED && !running()) {
      return success();
    }
    return failure(ErrorCode::Internal, "failed to terminate the child process",
                   std::to_string(error));
  }
  return success();
#else
  if (handle_ == 0) {
    return failure(ErrorCode::InvalidArgument, "process handle is not valid");
  }
  if (kill(static_cast<pid_t>(handle_), SIGKILL) != 0) {
    return failure(ErrorCode::Internal, "failed to terminate the child process");
  }
  return success();
#endif
}

std::string current_executable_path() {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::string();
    }
    if (written < buffer.size()) {
      buffer.resize(written);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return narrow(buffer);
#else
  std::vector<char> buffer(PATH_MAX, '\0');
  const ssize_t written = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (written <= 0) {
    return std::string();
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
#endif
}

std::string current_executable_directory() {
  const std::string path = current_executable_path();
  if (path.empty()) {
    return std::string();
  }
  return std::filesystem::path(path).parent_path().string();
}

Result<std::string> sibling_executable(std::string_view name) {
#if defined(_WIN32)
  const std::string file = std::string(name) + ".exe";
#else
  const std::string file = std::string(name);
#endif
  std::vector<std::filesystem::path> candidates;
  if (const char* directory = std::getenv("APF_TOOL_DIR")) {
    candidates.emplace_back(std::filesystem::path(directory) / file);
  }
  const std::string own_directory = current_executable_directory();
  if (!own_directory.empty()) {
    candidates.emplace_back(std::filesystem::path(own_directory) / file);
    candidates.emplace_back(std::filesystem::path(own_directory) / ".." / "bin" / file);
    candidates.emplace_back(std::filesystem::path(own_directory) / ".." / ".." / "bin" / file);
  }
  for (const std::filesystem::path& candidate : candidates) {
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      return std::filesystem::absolute(candidate).lexically_normal().string();
    }
  }
  return make_error(ErrorCode::NotFound, "could not locate helper executable", std::string(name));
}

std::string temp_directory() {
  std::error_code error;
  const std::filesystem::path path = std::filesystem::temp_directory_path(error);
  if (error) {
    return ".";
  }
  return path.string();
}

std::string join_path(std::string_view base, std::string_view leaf) {
  return (std::filesystem::path(base) / std::filesystem::path(leaf)).string();
}

bool is_path_safe(std::string_view path) {
  if (path.empty() || path.size() > 4096) {
    return false;
  }
  if (path.find('\0') != std::string_view::npos) {
    return false;
  }
  const std::filesystem::path candidate(path);
  if (candidate.is_absolute()) {
    return true;
  }
  for (const auto& part : candidate) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

bool file_exists(std::string_view path) {
  std::error_code error;
  return std::filesystem::exists(std::filesystem::path(path), error) && !error;
}

std::uint64_t file_size(std::string_view path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

Status remove_file(std::string_view path) {
  if (!is_path_safe(path)) {
    return failure(ErrorCode::InvalidArgument, "refusing to remove an unsafe path",
                   std::string(path));
  }
  std::error_code error;
  std::filesystem::remove(std::filesystem::path(path), error);
  if (error) {
    return failure(ErrorCode::Internal, "could not remove file",
                   std::string(path) + ": " + error.message());
  }
  return success();
}

Status ensure_directory(std::string_view path) {
  if (!is_path_safe(path)) {
    return failure(ErrorCode::InvalidArgument, "refusing to create an unsafe path",
                   std::string(path));
  }
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(path), error);
  if (error) {
    return failure(ErrorCode::Internal, "could not create directory",
                   std::string(path) + ": " + error.message());
  }
  return success();
}

Status remove_directory_recursive(std::string_view path) {
  if (!is_path_safe(path)) {
    return failure(ErrorCode::InvalidArgument, "refusing to remove an unsafe path",
                   std::string(path));
  }
  std::error_code error;
  std::filesystem::remove_all(std::filesystem::path(path), error);
  if (error) {
    return failure(ErrorCode::Internal, "could not remove directory tree",
                   std::string(path) + ": " + error.message());
  }
  return success();
}

Result<std::string> read_file(std::string_view path) {
  if (!is_path_safe(path)) {
    return make_error(ErrorCode::InvalidArgument, "refusing to read an unsafe path",
                      std::string(path));
  }
  std::ifstream stream(std::filesystem::path(path), std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::NotFound, "could not open file", std::string(path));
  }
  std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (stream.bad()) {
    return make_error(ErrorCode::Internal, "read failed", std::string(path));
  }
  return data;
}

Status write_file_atomic(std::string_view path, std::string_view data) {
  if (!is_path_safe(path)) {
    return failure(ErrorCode::InvalidArgument, "refusing to write an unsafe path",
                   std::string(path));
  }
  const std::filesystem::path target(path);
  const std::filesystem::path directory = target.parent_path();
  if (!directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      return failure(ErrorCode::Internal, "could not create the target directory",
                     directory.string());
    }
  }
  const std::string temporary =
      (directory / (target.filename().string() + ".tmp")).string();
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return failure(ErrorCode::Internal, "could not open the temporary file", temporary);
    }
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.flush();
    if (!stream) {
      return failure(ErrorCode::Internal, "could not write the temporary file", temporary);
    }
  }
  // Verify the temporary file before it becomes authoritative.
  const std::uint64_t size = file_size(temporary);
  if (size != data.size()) {
    (void)remove_file(temporary);
    return failure(ErrorCode::Internal, "temporary file size does not match the payload",
                   std::to_string(size) + " != " + std::to_string(data.size()));
  }
#if defined(_WIN32)
  const std::wstring wide_temporary = widen(temporary);
  const std::wstring wide_target = widen(target.string());
  if (MoveFileExW(wide_temporary.c_str(), wide_target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    (void)remove_file(temporary);
    return failure(ErrorCode::Internal, "atomic replacement failed",
                   std::to_string(GetLastError()));
  }
#else
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    (void)remove_file(temporary);
    return failure(ErrorCode::Internal, "atomic replacement failed", error.message());
  }
#endif
  return success();
}

Result<std::vector<std::string>> list_directory(std::string_view path) {
  if (!is_path_safe(path)) {
    return make_error(ErrorCode::InvalidArgument, "refusing to list an unsafe path",
                      std::string(path));
  }
  std::error_code error;
  std::vector<std::string> out;
  std::filesystem::directory_iterator iterator(std::filesystem::path(path), error);
  if (error) {
    return make_error(ErrorCode::NotFound, "could not list directory",
                      std::string(path) + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    out.push_back(entry.path().filename().string());
    if (out.size() > 4096) {
      break;
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::uint64_t entropy_seed() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  std::uint64_t seed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#if defined(_WIN32)
  seed ^= static_cast<std::uint64_t>(GetCurrentProcessId()) << 32;
  seed ^= static_cast<std::uint64_t>(GetTickCount64());
#else
  seed ^= static_cast<std::uint64_t>(getpid()) << 32;
#endif
  return seed;
}

Result<std::string> make_temp_directory(std::string_view prefix) {
  const std::filesystem::path base =
      std::filesystem::path(temp_directory()) / (std::string(prefix) + "-" + std::to_string(entropy_seed()));
  const Status created = ensure_directory(base.string());
  if (!created.ok()) {
    return created.error();
  }
  return base.string();
}

}  // namespace apf
