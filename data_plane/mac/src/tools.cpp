#include "arec/tools.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "arec/protocol.hpp"

namespace arec {
namespace {

ToolResult Failure(const std::string& code, const std::string& message) {
  return {
      kStatusFailed, code, false, message, nlohmann::json::object(),
      NowRfc3339(),
  };
}

void SetNonBlocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
  }
}

void CloseDescriptor(int* descriptor) {
  if (*descriptor >= 0) {
    close(*descriptor);
    *descriptor = -1;
  }
}

void ReadAvailable(int descriptor, std::string* output, bool* truncated,
                   bool* open) {
  std::array<char, 8192> buffer{};
  while (*open) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      const std::size_t remaining =
          output->size() < kToolOutputLimit
              ? kToolOutputLimit - output->size()
              : 0;
      const std::size_t consumed =
          std::min<std::size_t>(remaining, static_cast<std::size_t>(count));
      output->append(buffer.data(), consumed);
      if (consumed < static_cast<std::size_t>(count)) {
        *truncated = true;
      }
      continue;
    }
    if (count == 0) {
      *open = false;
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    *open = false;
    return;
  }
}

bool HasNullByte(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

}  // namespace

bool ToolExecutor::ValidateParams(const std::string& tool,
                                  const nlohmann::json& params,
                                  std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!params.is_object()) {
    return fail("params must be an object");
  }
  if (tool == "system_info") {
    if (!params.empty()) {
      return fail("system_info does not accept parameters");
    }
    return true;
  }
  if (tool == "exec_command") {
    for (auto iterator = params.begin(); iterator != params.end(); ++iterator) {
      if (iterator.key() != "command" && iterator.key() != "cwd") {
        return fail("unknown exec_command parameter");
      }
    }
    if (!params.contains("command") || !params["command"].is_string()) {
      return fail("command must be a string");
    }
    const std::string command = params["command"].get<std::string>();
    if (command.empty() || command.size() > 65536 || HasNullByte(command)) {
      return fail("command must contain 1 to 65536 bytes without NUL");
    }
    if (params.contains("cwd")) {
      if (!params["cwd"].is_string()) {
        return fail("cwd must be a string");
      }
      const std::string cwd = params["cwd"].get<std::string>();
      if (cwd.empty() || cwd.size() > 4096 || HasNullByte(cwd)) {
        return fail("cwd must contain 1 to 4096 bytes without NUL");
      }
    }
    return true;
  }
  if (tool == "read_file") {
    for (auto iterator = params.begin(); iterator != params.end(); ++iterator) {
      if (iterator.key() != "path" && iterator.key() != "max_bytes") {
        return fail("unknown read_file parameter");
      }
    }
    if (!params.contains("path") || !params["path"].is_string()) {
      return fail("path must be a string");
    }
    const std::string path = params["path"].get<std::string>();
    if (path.empty() || path.size() > 4096 || HasNullByte(path)) {
      return fail("path must contain 1 to 4096 bytes without NUL");
    }
    if (params.contains("max_bytes")) {
      if (!params["max_bytes"].is_number_integer()) {
        return fail("max_bytes must be an integer");
      }
      const auto max_bytes = params["max_bytes"].get<std::int64_t>();
      if (max_bytes < 1 || max_bytes > 262144) {
        return fail("max_bytes must be between 1 and 262144");
      }
    }
    return true;
  }
  if (tool == "write_file") {
    for (auto iterator = params.begin(); iterator != params.end(); ++iterator) {
      if (iterator.key() != "path" && iterator.key() != "content") {
        return fail("unknown write_file parameter");
      }
    }
    if (!params.contains("path") || !params["path"].is_string() ||
        !params.contains("content") || !params["content"].is_string()) {
      return fail("path and content must be strings");
    }
    const std::string path = params["path"].get<std::string>();
    const std::string content = params["content"].get<std::string>();
    if (path.empty() || path.size() > 32768 || HasNullByte(path)) {
      return fail("path must contain 1 to 32768 bytes without NUL");
    }
    if (content.size() > kMaxWriteBytes) {
      return fail("content exceeds the 1 MiB limit");
    }
    return true;
  }
  if (tool == "list_dir" || tool == "create_dir") {
    for (auto iterator = params.begin(); iterator != params.end(); ++iterator) {
      if (iterator.key() != "path") {
        return fail("unknown directory parameter");
      }
    }
    if (!params.contains("path") || !params["path"].is_string()) {
      return fail("path must be a string");
    }
    const std::string path = params["path"].get<std::string>();
    if (path.empty() || path.size() > 32768 || HasNullByte(path)) {
      return fail("path must contain 1 to 32768 bytes without NUL");
    }
    return true;
  }
  return fail("unsupported tool");
}

ToolResult ToolExecutor::Execute(const std::string& tool,
                                 const nlohmann::json& params, int deadline_ms,
                                 const Cancelled& cancelled) {
  std::string validation_error;
  if (!ValidateParams(tool, params, &validation_error)) {
    return Failure(kErrorInvalidParams, validation_error);
  }
  if (tool == "system_info") {
    return SystemInfo();
  }
  if (tool == "read_file") {
    return ReadFile(params);
  }
  if (tool == "write_file") {
    return WriteFile(params);
  }
  if (tool == "list_dir") {
    return ListDir(params);
  }
  if (tool == "create_dir") {
    return CreateDir(params);
  }
  if (tool == "exec_command") {
    return RunCommand(params, deadline_ms, cancelled);
  }
  return Failure(kErrorToolNotFound, "unsupported tool");
}

ToolResult ToolExecutor::SystemInfo() {
  struct utsname system{};
  if (uname(&system) != 0) {
    return Failure(kErrorExecutionFailed, "uname failed");
  }
  const std::string release = system.release;
  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"hostname", system.nodename},
          {"os", std::string("macOS ") + release},
          {"arch", system.machine},
      },
      NowRfc3339(),
  };
}

ToolResult ToolExecutor::ReadFile(const nlohmann::json& params) {
  const std::string path = params.at("path").get<std::string>();
  std::size_t limit = kToolOutputLimit;
  if (params.contains("max_bytes")) {
    limit = static_cast<std::size_t>(params["max_bytes"].get<std::int64_t>());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Failure(kErrorExecutionFailed, "unable to open file");
  }
  std::string content(limit + 1, '\0');
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  const std::size_t bytes_read = static_cast<std::size_t>(input.gcount());
  const bool truncated = bytes_read > limit;
  if (truncated) {
    content.resize(limit);
  } else {
    content.resize(bytes_read);
  }
  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"path", path},
          {"content", content},
          {"bytes_read", content.size()},
          {"truncated", truncated},
      },
      NowRfc3339(),
  };
}

ToolResult ToolExecutor::WriteFile(const nlohmann::json& params) {
  const std::string path = params.at("path").get<std::string>();
  const std::string content = params.at("content").get<std::string>();
  std::error_code error;
  const std::filesystem::path target(path);
  std::filesystem::path parent = target.parent_path();
  if (parent.empty()) {
    parent = std::filesystem::current_path(error);
    if (error) {
      return Failure(kErrorExecutionFailed, "working directory is unavailable");
    }
  }
  if (!std::filesystem::is_directory(parent, error) || error) {
    return Failure(kErrorExecutionFailed, "parent directory is unavailable");
  }
  const bool created = !std::filesystem::exists(target, error);
  if (error) {
    return Failure(kErrorExecutionFailed, "target file state is unavailable");
  }

  std::string temporary =
      (parent / std::filesystem::path(".arec-tmp-XXXXXX")).string();
  std::vector<char> temporary_buffer(temporary.begin(), temporary.end());
  temporary_buffer.push_back('\0');
  const int descriptor = mkstemp(temporary_buffer.data());
  if (descriptor < 0) {
    return Failure(kErrorExecutionFailed, "temporary file cannot be created");
  }

  bool write_failed = false;
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written =
        write(descriptor, content.data() + offset, content.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    write_failed = true;
    break;
  }
  if (!write_failed && fsync(descriptor) != 0) {
    write_failed = true;
  }
  if (close(descriptor) != 0) {
    write_failed = true;
  }

  const std::filesystem::path temporary_path(temporary_buffer.data());
  if (write_failed) {
    unlink(temporary_path.c_str());
    return Failure(kErrorExecutionFailed, "file content could not be written");
  }

  std::filesystem::rename(temporary_path, target, error);
  if (error) {
    unlink(temporary_path.c_str());
    return Failure(kErrorExecutionFailed, "atomic file replacement failed");
  }

  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"path", path},
          {"bytes_written", content.size()},
          {"created", created},
      },
      NowRfc3339(),
  };
}

ToolResult ToolExecutor::ListDir(const nlohmann::json& params) {
  const std::string path = params.at("path").get<std::string>();
  std::error_code error;
  const std::filesystem::path directory(path);
  if (!std::filesystem::is_directory(directory, error) || error) {
    return Failure(kErrorExecutionFailed, "path is not a readable directory");
  }

  std::vector<nlohmann::json> entries;
  entries.reserve(kMaxDirectoryEntries + 1U);
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied,
      error);
  if (error) {
    return Failure(kErrorExecutionFailed, "directory cannot be opened");
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end && entries.size() <= kMaxDirectoryEntries) {
    const auto entry = *iterator;
    const std::string name = entry.path().filename().string();
    std::error_code status_error;
    const auto status = entry.symlink_status(status_error);
    if (status_error) {
      return Failure(kErrorExecutionFailed, "directory entry cannot be inspected");
    }

    std::string type = "other";
    std::uint64_t size = 0;
    if (std::filesystem::is_regular_file(status)) {
      type = "file";
      size = entry.file_size(status_error);
      if (status_error) {
        size = 0;
      }
    } else if (std::filesystem::is_directory(status)) {
      type = "directory";
    } else if (std::filesystem::is_symlink(status)) {
      type = "symlink";
    }
    entries.push_back({
        {"name", name},
        {"type", type},
        {"size", size},
    });
    iterator.increment(error);
    if (error) {
      return Failure(kErrorExecutionFailed, "directory iteration failed");
    }
  }

  const bool truncated = entries.size() > kMaxDirectoryEntries;
  if (truncated) {
    entries.resize(kMaxDirectoryEntries);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& left, const auto& right) {
              return left.at("name").get<std::string>() <
                     right.at("name").get<std::string>();
            });
  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"path", path},
          {"entries", entries},
          {"count", entries.size()},
          {"truncated", truncated},
      },
      NowRfc3339(),
  };
}

ToolResult ToolExecutor::CreateDir(const nlohmann::json& params) {
  const std::string path = params.at("path").get<std::string>();
  std::error_code error;
  const std::filesystem::path directory(path);
  const bool created = std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory, error)) {
    return Failure(kErrorExecutionFailed, "directory could not be created");
  }
  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"path", path},
          {"created", created},
      },
      NowRfc3339(),
  };
}

ToolResult ToolExecutor::RunCommand(const nlohmann::json& params,
                                    int deadline_ms,
                                    const Cancelled& cancelled) {
  const std::string command = params.at("command").get<std::string>();
  std::string cwd;
  if (params.contains("cwd")) {
    cwd = params["cwd"].get<std::string>();
    std::error_code error;
    if (!std::filesystem::is_directory(cwd, error)) {
      return Failure(kErrorExecutionFailed, "cwd is not a directory");
    }
  }
  if (deadline_ms <= 0) {
    deadline_ms = 5000;
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    CloseDescriptor(&stdout_pipe[0]);
    CloseDescriptor(&stdout_pipe[1]);
    CloseDescriptor(&stderr_pipe[0]);
    CloseDescriptor(&stderr_pipe[1]);
    return Failure(kErrorExecutionFailed, "unable to create pipes");
  }

  const pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      _exit(126);
    }
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }
  CloseDescriptor(&stdout_pipe[1]);
  CloseDescriptor(&stderr_pipe[1]);
  if (child < 0) {
    CloseDescriptor(&stdout_pipe[0]);
    CloseDescriptor(&stderr_pipe[0]);
    return Failure(kErrorExecutionFailed, "unable to start command");
  }
  setpgid(child, child);
  SetNonBlocking(stdout_pipe[0]);
  SetNonBlocking(stderr_pipe[0]);

  std::string stdout_text;
  std::string stderr_text;
  bool stdout_open = true;
  bool stderr_open = true;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
  bool timed_out = false;
  bool was_cancelled = false;

  while (stdout_open || stderr_open) {
    if (cancelled && cancelled()) {
      was_cancelled = true;
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    std::array<struct pollfd, 2> descriptors{{
        {stdout_pipe[0], static_cast<short>(stdout_open ? POLLIN : 0), 0},
        {stderr_pipe[0], static_cast<short>(stderr_open ? POLLIN : 0), 0},
    }};
    const int polled =
        poll(descriptors.data(), descriptors.size(),
             static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
    if (polled < 0 && errno != EINTR) {
      break;
    }
    if (polled == 0) {
      continue;
    }
    if (stdout_open &&
        (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      ReadAvailable(stdout_pipe[0], &stdout_text, &stdout_truncated,
                    &stdout_open);
    }
    if (stderr_open &&
        (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      ReadAvailable(stderr_pipe[0], &stderr_text, &stderr_truncated,
                    &stderr_open);
    }
  }

  if (timed_out || was_cancelled) {
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  }
  int wait_status = 0;
  while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
  }
  CloseDescriptor(&stdout_pipe[0]);
  CloseDescriptor(&stderr_pipe[0]);

  if (timed_out || was_cancelled) {
    return {
        kStatusTimeout,
        kErrorExecutionTimeout,
        false,
        was_cancelled ? "connection closed; execution state unknown"
                      : "deadline exceeded; execution state unknown",
        {
            {"stdout", stdout_text},
            {"stderr", stderr_text},
            {"exit_code", -1},
            {"stdout_truncated", stdout_truncated},
            {"stderr_truncated", stderr_truncated},
        },
        NowRfc3339(),
    };
  }

  int exit_code = -1;
  if (WIFEXITED(wait_status)) {
    exit_code = WEXITSTATUS(wait_status);
  }
  return {
      kStatusOK,
      std::nullopt,
      false,
      "",
      {
          {"stdout", stdout_text},
          {"stderr", stderr_text},
          {"exit_code", exit_code},
          {"stdout_truncated", stdout_truncated},
          {"stderr_truncated", stderr_truncated},
      },
      NowRfc3339(),
  };
}

}  // namespace arec

