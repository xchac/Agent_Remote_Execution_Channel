#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arec {

constexpr std::size_t kMaxFrameBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxToolOutputBytes = 256U * 1024U;
constexpr std::int64_t kDefaultDeadlineMs = 5000;
constexpr std::int64_t kMaxDeadlineMs = 30000;
constexpr std::size_t kDefaultCacheCapacity = 1024;
constexpr std::chrono::minutes kDefaultCacheTtl{10};
constexpr const char* kProtocolVersion = "1";

enum class Status {
    pending,
    running,
    ok,
    timeout,
    failed,
    unknown,
};

std::string to_string(Status status);

struct ToolResponse {
    Status status = Status::failed;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    bool retryable = false;
    std::string message;
    std::string executed_at;
    std::string platform = "windows";

    nlohmann::json to_json() const;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> platforms;
    bool idempotent = false;
    nlohmann::json params_schema = nlohmann::json::object();

    nlohmann::json to_json() const;
};

std::int64_t unix_time_seconds();
std::string utc_now_iso8601();

}  // namespace arec
