#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "arec/cache.hpp"
#include "arec/frame.hpp"

namespace arec {

struct DataPlaneConfig {
  std::string gateway_address = "127.0.0.1:9000";
  std::string agent_id = "mac-a";
  std::string hmac_secret;
};

class DataPlane {
 public:
  explicit DataPlane(DataPlaneConfig config);
  ~DataPlane();

  void Run();
  void Stop();
  bool running() const { return !stop_requested_.load(); }

 private:
  void ConnectAndServe();
  void ReadLoop();
  void HandleMessage(const std::string& payload);
  void HandleRequest(const nlohmann::json& message);
  void HandleQuery(const nlohmann::json& message);
  void ExecuteRequest(const nlohmann::json& message, ExecutionRecord record,
                      std::shared_ptr<std::atomic_bool> cancelled);
  void SendRegister();
  bool SendJson(const nlohmann::json& message);
  nlohmann::json RecordResponse(const ExecutionRecord& record) const;
  nlohmann::json ErrorResponse(const std::string& request_id,
                               const std::string& code,
                               const std::string& message,
                               const std::string& status = "failed") const;
  void CancelActive();
  void JoinWorkers();
  void CloseSocket();
  void SleepBeforeReconnect(std::chrono::milliseconds delay);
  static std::pair<std::string, std::string> SplitAddress(
      const std::string& address);

  DataPlaneConfig config_;
  asio::io_context io_context_;
  asio::ip::tcp::socket socket_;
  mutable std::mutex write_mutex_;
  std::atomic_bool connected_{false};
  std::atomic_bool stop_requested_{false};
  FrameDecoder decoder_;
  ExecutionCache cache_;

  std::mutex active_mutex_;
  std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> active_;

  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;

  std::mutex reconnect_mutex_;
  std::condition_variable reconnect_cv_;
};

}  // namespace arec

