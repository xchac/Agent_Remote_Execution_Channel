#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace arec {

constexpr std::chrono::minutes kCacheRetention{10};
constexpr std::size_t kMaxCacheItems = 1024;

struct ExecutionRecord {
  std::string request_id;
  std::string session_id;
  std::string idempotency_key;
  std::string tool;
  std::string params_hash;
  std::string status = "pending";
  nlohmann::json result;
  std::optional<std::string> error_code;
  bool retryable = false;
  std::string message;
  std::string executed_at;
  std::chrono::system_clock::time_point updated_at;
};

enum class CacheDecision {
  kInserted,
  kDuplicate,
  kConflict,
};

struct CacheBeginResult {
  CacheDecision decision = CacheDecision::kInserted;
  ExecutionRecord record;
};

class ExecutionCache {
 public:
  explicit ExecutionCache(std::chrono::milliseconds retention =
                              std::chrono::milliseconds(kCacheRetention),
                          std::size_t max_items = kMaxCacheItems);

  CacheBeginResult Begin(ExecutionRecord record,
                         std::chrono::system_clock::time_point now =
                             std::chrono::system_clock::now());
  bool Update(const std::string& request_id,
              const ExecutionRecord& updated_record);
  std::optional<ExecutionRecord> Get(const std::string& request_id);
  std::size_t size();

 private:
  void CleanupLocked(std::chrono::system_clock::time_point now);
  void TrimLocked();
  void RemoveOrderLocked(const std::string& request_id);
  static std::string IdempotencyKey(const std::string& session_id,
                                    const std::string& key);

  std::chrono::milliseconds retention_;
  std::size_t max_items_;
  std::mutex mutex_;
  std::unordered_map<std::string, ExecutionRecord> records_;
  std::unordered_map<std::string, std::string> idempotency_;
  std::vector<std::string> order_;
};

}  // namespace arec

