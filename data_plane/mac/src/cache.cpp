#include "arec/cache.hpp"

#include <algorithm>

namespace arec {

ExecutionCache::ExecutionCache(std::chrono::milliseconds retention,
                               std::size_t max_items)
    : retention_(retention), max_items_(max_items) {}

CacheBeginResult ExecutionCache::Begin(
    ExecutionRecord record, std::chrono::system_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  CleanupLocked(now);
  const std::string key =
      IdempotencyKey(record.session_id, record.idempotency_key);
  const auto existing_id = idempotency_.find(key);
  if (existing_id != idempotency_.end()) {
    const auto existing_record = records_.find(existing_id->second);
    if (existing_record == records_.end()) {
      idempotency_.erase(existing_id);
    } else if (existing_record->second.tool != record.tool ||
               existing_record->second.params_hash != record.params_hash) {
      return {CacheDecision::kConflict, existing_record->second};
    } else {
      return {CacheDecision::kDuplicate, existing_record->second};
    }
  }

  record.updated_at = now;
  records_[record.request_id] = record;
  order_.push_back(record.request_id);
  idempotency_[key] = record.request_id;
  TrimLocked();
  return {CacheDecision::kInserted, record};
}

bool ExecutionCache::Update(const std::string& request_id,
                            const ExecutionRecord& updated_record) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(request_id);
  if (found == records_.end()) {
    return false;
  }
  records_[request_id] = updated_record;
  records_[request_id].updated_at = std::chrono::system_clock::now();
  return true;
}

std::optional<ExecutionRecord> ExecutionCache::Get(
    const std::string& request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  CleanupLocked(std::chrono::system_clock::now());
  const auto found = records_.find(request_id);
  if (found == records_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::size_t ExecutionCache::size() {
  std::lock_guard<std::mutex> lock(mutex_);
  CleanupLocked(std::chrono::system_clock::now());
  return records_.size();
}

void ExecutionCache::CleanupLocked(
    std::chrono::system_clock::time_point now) {
  const auto cutoff = now - retention_;
  std::vector<std::string> expired;
  for (const auto& [request_id, record] : records_) {
    if (record.updated_at < cutoff) {
      expired.push_back(request_id);
    }
  }
  for (const auto& request_id : expired) {
    const auto found = records_.find(request_id);
    if (found == records_.end()) {
      continue;
    }
    idempotency_.erase(
        IdempotencyKey(found->second.session_id,
                       found->second.idempotency_key));
    records_.erase(found);
    RemoveOrderLocked(request_id);
  }
}

void ExecutionCache::TrimLocked() {
  while (records_.size() > max_items_ && !order_.empty()) {
    const std::string request_id = order_.front();
    order_.erase(order_.begin());
    const auto found = records_.find(request_id);
    if (found == records_.end()) {
      continue;
    }
    idempotency_.erase(
        IdempotencyKey(found->second.session_id,
                       found->second.idempotency_key));
    records_.erase(found);
  }
}

void ExecutionCache::RemoveOrderLocked(const std::string& request_id) {
  order_.erase(std::remove(order_.begin(), order_.end(), request_id),
               order_.end());
}

std::string ExecutionCache::IdempotencyKey(const std::string& session_id,
                                           const std::string& key) {
  return session_id + '\0' + key;
}

}  // namespace arec

