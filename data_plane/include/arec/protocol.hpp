#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace arec {

constexpr const char* kProtocolVersion = "1";
constexpr const char* kPlatformDarwin = "darwin";

constexpr const char* kStatusPending = "pending";
constexpr const char* kStatusRunning = "running";
constexpr const char* kStatusOK = "ok";
constexpr const char* kStatusTimeout = "timeout";
constexpr const char* kStatusFailed = "failed";
constexpr const char* kStatusUnknown = "unknown";

constexpr const char* kErrorToolNotFound = "TOOL_NOT_FOUND";
constexpr const char* kErrorUnauthorized = "UNAUTHORIZED";
constexpr const char* kErrorInvalidParams = "INVALID_PARAMS";
constexpr const char* kErrorExecutionTimeout = "EXECUTION_TIMEOUT";
constexpr const char* kErrorExecutionFailed = "EXECUTION_FAILED";
constexpr const char* kErrorOutputTooLarge = "OUTPUT_TOO_LARGE";
constexpr const char* kErrorProtocol = "PROTOCOL_ERROR";
constexpr const char* kErrorIdempotencyConflict = "IDEMPOTENCY_CONFLICT";

bool IsFinalStatus(const std::string& status);
std::string NowRfc3339();
std::string Sha256Hex(const std::string& value);
std::string CanonicalParamsHash(const nlohmann::json& params);
nlohmann::json ToolDefinitions();
nlohmann::json ToolDefinition(const std::string& name);

}  // namespace arec

