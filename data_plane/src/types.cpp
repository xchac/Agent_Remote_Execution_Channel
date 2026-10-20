#include "arec/types.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace arec {

std::string to_string(Status status) {
    switch (status) {
        case Status::pending:
            return "pending";
        case Status::running:
            return "running";
        case Status::ok:
            return "ok";
        case Status::timeout:
            return "timeout";
        case Status::failed:
            return "failed";
        case Status::unknown:
            return "unknown";
    }
    return "unknown";
}

nlohmann::json ToolResponse::to_json() const {
    nlohmann::json value = {
        {"status", to_string(status)},
        {"retryable", retryable},
        {"platform", platform},
    };

    if (!result.empty()) {
        value["result"] = result;
    }
    if (!error_code.empty()) {
        value["error_code"] = error_code;
    }
    if (!message.empty()) {
        value["message"] = message;
    }
    if (!executed_at.empty()) {
        value["executed_at"] = executed_at;
    }
    return value;
}

nlohmann::json ToolDefinition::to_json() const {
    return {
        {"name", name},
        {"description", description},
        {"platform", platforms},
        {"idempotent", idempotent},
        {"params_schema", params_schema},
    };
}

std::int64_t unix_time_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string utc_now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

}  // namespace arec
