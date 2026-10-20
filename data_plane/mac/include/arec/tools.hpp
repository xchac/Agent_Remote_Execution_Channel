#pragma once

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace arec {

constexpr std::size_t kToolOutputLimit = 256U * 1024U;

struct ToolResult {
  std::string status;
  std::optional<std::string> error_code;
  bool retryable = false;
  std::string message;
  nlohmann::json result = nlohmann::json::object();
  std::string executed_at;
};

class ToolExecutor {
 public:
  using Cancelled = std::function<bool()>;

  static ToolResult Execute(const std::string& tool,
                            const nlohmann::json& params,
                            int deadline_ms = 5000,
                            const Cancelled& cancelled = {});
  static bool ValidateParams(const std::string& tool,
                             const nlohmann::json& params,
                             std::string* error = nullptr);

 private:
  static ToolResult SystemInfo();
  static ToolResult ReadFile(const nlohmann::json& params);
  static ToolResult RunCommand(const nlohmann::json& params, int deadline_ms,
                               const Cancelled& cancelled);
};

}  // namespace arec

