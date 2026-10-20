#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "arec/types.hpp"

namespace arec {

class ExecutionEngine {
public:
    explicit ExecutionEngine(
        std::string hmac_secret,
        std::string platform = "windows",
        std::size_t cache_capacity = kDefaultCacheCapacity,
        std::chrono::milliseconds cache_ttl =
            std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultCacheTtl));

    ToolResponse handle_request(const nlohmann::json& request);
    nlohmann::json handle_query(const nlohmann::json& query);

    static nlohmann::json tool_definitions();

private:
    struct IdempotencyEntry {
        std::string params_hash;
        bool complete = false;
        ToolResponse response;
        std::chrono::steady_clock::time_point expires_at;
    };

    struct RequestState {
        ToolResponse response;
        std::chrono::steady_clock::time_point expires_at;
    };

    ToolResponse execute_tool(
        const std::string& tool,
        const nlohmann::json& params,
        std::int64_t deadline_ms);

    ToolResponse execute_system_info();
    ToolResponse execute_read_file(const nlohmann::json& params);
    ToolResponse execute_command(const nlohmann::json& params, std::int64_t deadline_ms);

    void store_request_state(
        const std::string& request_id,
        const ToolResponse& response,
        std::chrono::steady_clock::time_point now);

    std::string hmac_secret_;
    std::string platform_;
    std::size_t cache_capacity_;
    std::chrono::milliseconds cache_ttl_;

    std::mutex cache_mutex_;
    std::condition_variable cache_cv_;
    std::unordered_map<std::string, IdempotencyEntry> idempotency_entries_;
    std::deque<std::string> idempotency_order_;
    std::unordered_map<std::string, RequestState> request_states_;
    std::deque<std::string> request_order_;
};

}  // namespace arec
