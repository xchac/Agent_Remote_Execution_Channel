#include "arec/protocol.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "arec/tools.hpp"

namespace arec {

bool IsFinalStatus(const std::string& status) {
  return status == kStatusOK || status == kStatusTimeout ||
         status == kStatusFailed;
}

std::string NowRfc3339() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer.data();
}

std::string Sha256Hex(const std::string& value) {
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
  CC_SHA256(value.data(), static_cast<CC_LONG>(value.size()), digest.data());
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    output << std::setw(2) << static_cast<int>(byte);
  }
  return output.str();
}

std::string CanonicalParamsHash(const nlohmann::json& params) {
  return Sha256Hex(params.dump());
}

nlohmann::json ToolDefinitions() {
  return nlohmann::json::array(
      {ToolDefinition("system_info"), ToolDefinition("exec_command"),
       ToolDefinition("read_file"), ToolDefinition("write_file"),
       ToolDefinition("list_dir"), ToolDefinition("create_dir")});
}

nlohmann::json ToolDefinition(const std::string& name) {
  if (name == "system_info") {
    return {
        {"name", "system_info"},
        {"description", "Return hostname, operating system, and architecture."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", true},
        {"params_schema",
         {{"type", "object"},
          {"properties", nlohmann::json::object()},
          {"additionalProperties", false}}},
    };
  }
  if (name == "exec_command") {
    return {
        {"name", "exec_command"},
        {"description",
         "Execute a shell command and return stdout, stderr, and exit code."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", false},
        {"params_schema",
         {{"type", "object"},
          {"properties",
           {{"command", {{"type", "string"}, {"minLength", 1}}},
            {"cwd", {{"type", "string"}}}}},
          {"required", {"command"}},
          {"additionalProperties", false}}},
    };
  }
  if (name == "read_file") {
    return {
        {"name", "read_file"},
        {"description", "Read a file and return its content."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", true},
        {"params_schema",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"minLength", 1}}},
            {"max_bytes",
             {{"type", "integer"},
              {"minimum", 1},
              {"maximum", 262144}}}}},
          {"required", {"path"}},
          {"additionalProperties", false}}},
    };
  }
  if (name == "write_file") {
    return {
        {"name", "write_file"},
        {"description", "Write UTF-8 content atomically to a file."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", true},
        {"params_schema",
         {{"type", "object"},
          {"properties",
           {{"path",
             {{"type", "string"}, {"minLength", 1}, {"maxLength", 32768}}},
            {"content",
             {{"type", "string"}, {"maxLength", kMaxWriteBytes}}}}},
          {"required", {"path", "content"}},
          {"additionalProperties", false}}},
    };
  }
  if (name == "list_dir") {
    return {
        {"name", "list_dir"},
        {"description", "List directory entries in deterministic name order."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", true},
        {"params_schema",
         {{"type", "object"},
          {"properties",
           {{"path",
             {{"type", "string"}, {"minLength", 1}, {"maxLength", 32768}}}}},
          {"required", {"path"}},
          {"additionalProperties", false}}},
    };
  }
  if (name == "create_dir") {
    return {
        {"name", "create_dir"},
        {"description", "Create a directory and any missing parents."},
        {"platform", {"darwin", "windows"}},
        {"idempotent", true},
        {"params_schema",
         {{"type", "object"},
          {"properties",
           {{"path",
             {{"type", "string"}, {"minLength", 1}, {"maxLength", 32768}}}}},
          {"required", {"path"}},
          {"additionalProperties", false}}},
    };
  }
  return nlohmann::json::object();
}

}  // namespace arec

