#include "arec/data_plane.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <thread>

#include "arec/protocol.hpp"
#include "arec/token.hpp"
#include "arec/tools.hpp"

namespace arec {
namespace {

bool SupportsTool(const CapabilityPayload& payload, const std::string& tool) {
  for (const auto& allowed : payload.allowed_tools) {
    if (allowed == tool) {
      return true;
    }
  }
  return false;
}

std::int64_t UnixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

DataPlane::DataPlane(DataPlaneConfig config)
    : config_(std::move(config)), socket_(io_context_) {}

DataPlane::~DataPlane() {
  Stop();
  JoinWorkers();
}

void DataPlane::Run() {
  auto delay = std::chrono::seconds(1);
  while (!stop_requested_.load()) {
    try {
      ConnectAndServe();
      delay = std::chrono::seconds(1);
    } catch (const std::exception& error) {
      if (!stop_requested_.load()) {
        std::cerr << "arec-data-plane: connection error: " << error.what()
                  << '\n';
      }
    }
    CloseSocket();
    CancelActive();
    if (!stop_requested_.load()) {
      SleepBeforeReconnect(delay);
      delay = std::min(delay * 2, std::chrono::seconds(30));
    }
  }
  JoinWorkers();
}

void DataPlane::Stop() {
  if (stop_requested_.exchange(true)) {
    return;
  }
  reconnect_cv_.notify_all();
  CancelActive();
  CloseSocket();
}

void DataPlane::ConnectAndServe() {
  const auto [host, port] = SplitAddress(config_.gateway_address);
  if (host.empty() || port.empty()) {
    throw std::runtime_error("invalid AREC_GATEWAY_DATA_ADDR");
  }
  asio::ip::tcp::resolver resolver(io_context_);
  const auto endpoints = resolver.resolve(host, port);
  socket_ = asio::ip::tcp::socket(io_context_);
  asio::connect(socket_, endpoints);
  socket_.set_option(asio::ip::tcp::no_delay(true));
  connected_.store(true);
  decoder_.Reset();
  SendRegister();

  FrameError frame_error = FrameError::kNone;
  std::string payload;
  while (!decoder_.Next(&payload, &frame_error)) {
    std::array<char, 8192> buffer{};
    const std::size_t count = socket_.read_some(asio::buffer(buffer));
    decoder_.Feed(buffer.data(), count, &frame_error);
    if (frame_error != FrameError::kNone &&
        frame_error != FrameError::kIncomplete) {
      throw std::runtime_error("invalid register acknowledgement frame");
    }
  }
  const auto acknowledgement = nlohmann::json::parse(payload);
  if (acknowledgement.value("type", "") != "register_ack" ||
      !acknowledgement.value("accepted", false)) {
    throw std::runtime_error("gateway rejected registration");
  }
  ReadLoop();
}

void DataPlane::ReadLoop() {
  std::array<char, 8192> buffer{};
  while (!stop_requested_.load()) {
    std::string payload;
    FrameError buffered_error = FrameError::kNone;
    while (decoder_.Next(&payload, &buffered_error)) {
      HandleMessage(payload);
    }
    const std::size_t count = socket_.read_some(asio::buffer(buffer));
    FrameError error = FrameError::kNone;
    if (!decoder_.Feed(buffer.data(), count, &error)) {
      throw std::runtime_error("frame decoder rejected input");
    }
    while (decoder_.Next(&payload, &error)) {
      HandleMessage(payload);
    }
    if (error != FrameError::kNone && error != FrameError::kIncomplete) {
      throw std::runtime_error("invalid data-plane frame");
    }
  }
}

void DataPlane::HandleMessage(const std::string& payload) {
  nlohmann::json message;
  try {
    message = nlohmann::json::parse(payload);
  } catch (const std::exception&) {
    SendJson(ErrorResponse("", kErrorProtocol, "invalid JSON"));
    throw std::runtime_error("invalid JSON message");
  }
  if (!message.is_object() ||
      message.value("protocol_version", "") != kProtocolVersion) {
    SendJson(ErrorResponse(message.value("request_id", ""), kErrorProtocol,
                           "unsupported protocol version"));
    throw std::runtime_error("invalid protocol version");
  }
  const std::string type = message.value("type", "");
  if (type == "request") {
    HandleRequest(message);
  } else if (type == "query") {
    HandleQuery(message);
  } else {
    SendJson(ErrorResponse(message.value("request_id", ""), kErrorProtocol,
                           "unknown message type"));
    throw std::runtime_error("unknown message type");
  }
}

void DataPlane::HandleRequest(const nlohmann::json& message) {
  const std::string request_id = message.value("request_id", "");
  const std::string session_id = message.value("session_id", "");
  const std::string tool = message.value("tool", "");
  const std::string idempotency_key = message.value("idempotency_key", "");
  const std::string token = message.value("token", "");
  if (request_id.empty() || session_id.empty() || tool.empty() ||
      idempotency_key.empty() || token.empty() || !message.contains("params")) {
    SendJson(ErrorResponse(request_id, kErrorProtocol,
                           "request is missing required fields"));
    return;
  }

  CapabilityPayload payload;
  std::string token_error;
  if (!VerifyCapabilityToken(token, config_.hmac_secret, UnixNow(), &payload,
                             &token_error)) {
    SendJson(ErrorResponse(request_id, kErrorUnauthorized,
                           "capability token validation failed"));
    return;
  }
  if (payload.session_id != session_id || !SupportsTool(payload, tool) ||
      ToolDefinition(tool).empty()) {
    SendJson(ErrorResponse(request_id, kErrorUnauthorized,
                           "tool is outside the capability scope"));
    return;
  }

  const int deadline_ms = message.value("deadline_ms", 5000);
  if (deadline_ms < 1 || deadline_ms > 30000) {
    SendJson(ErrorResponse(request_id, kErrorInvalidParams,
                           "deadline_ms must be between 1 and 30000"));
    return;
  }
  const nlohmann::json params = message["params"];
  std::string validation_error;
  if (!ToolExecutor::ValidateParams(tool, params, &validation_error)) {
    SendJson(ErrorResponse(request_id, kErrorInvalidParams, validation_error));
    return;
  }

  ExecutionRecord record;
  record.request_id = request_id;
  record.session_id = session_id;
  record.idempotency_key = idempotency_key;
  record.tool = tool;
  record.params_hash = CanonicalParamsHash(params);
  record.status = kStatusPending;

  const auto begin = cache_.Begin(record);
  if (begin.decision == CacheDecision::kConflict) {
    SendJson(ErrorResponse(request_id, kErrorIdempotencyConflict,
                           "idempotency key was already used with different input"));
    return;
  }
  if (begin.decision == CacheDecision::kDuplicate) {
    SendJson(RecordResponse(begin.record));
    return;
  }

  ExecutionRecord running = begin.record;
  running.status = kStatusRunning;
  cache_.Update(request_id, running);
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  {
    std::lock_guard<std::mutex> lock(active_mutex_);
    active_[request_id] = cancelled;
  }
  {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.emplace_back(
        [this, message, record = running, cancelled]() mutable {
          ExecuteRequest(message, std::move(record), std::move(cancelled));
        });
  }
}

void DataPlane::ExecuteRequest(const nlohmann::json& message,
                               ExecutionRecord record,
                               std::shared_ptr<std::atomic_bool> cancelled) {
  const ToolResult result =
      ToolExecutor::Execute(record.tool, message.at("params"),
                            message.value("deadline_ms", 5000),
                            [&cancelled]() { return cancelled->load(); });
  record.status = result.status;
  record.error_code = result.error_code;
  record.retryable = result.retryable;
  record.message = result.message;
  record.result = result.result;
  record.executed_at = result.executed_at;
  cache_.Update(record.request_id, record);
  {
    std::lock_guard<std::mutex> lock(active_mutex_);
    active_.erase(record.request_id);
  }
  SendJson(RecordResponse(record));
}

void DataPlane::HandleQuery(const nlohmann::json& message) {
  const std::string request_id = message.value("request_id", "");
  if (request_id.empty()) {
    SendJson(ErrorResponse("", kErrorProtocol, "query is missing request_id"));
    return;
  }
  auto record = cache_.Get(request_id);
  nlohmann::json response;
  if (!record.has_value()) {
    response = {
        {"type", "query_response"},
        {"protocol_version", kProtocolVersion},
        {"request_id", request_id},
        {"status", kStatusUnknown},
        {"error_code", nullptr},
        {"retryable", false},
        {"message", "request not found"},
        {"platform", kPlatformDarwin},
    };
  } else {
    response = RecordResponse(*record);
    response["type"] = "query_response";
  }
  SendJson(response);
}

void DataPlane::SendRegister() {
  const nlohmann::json registration = {
      {"type", "register"},
      {"protocol_version", kProtocolVersion},
      {"agent_id", config_.agent_id},
      {"platform", kPlatformDarwin},
      {"tools", ToolDefinitions()},
  };
  if (!SendJson(registration)) {
    throw std::runtime_error("failed to send registration");
  }
}

bool DataPlane::SendJson(const nlohmann::json& message) {
  const std::string payload =
      message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  FrameError error = FrameError::kNone;
  const auto frame = EncodeFrame(payload, kMaxFrameBytes, &error);
  if (error != FrameError::kNone) {
    return false;
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!connected_.load()) {
    return false;
  }
  asio::error_code write_error;
  asio::write(socket_, asio::buffer(frame), write_error);
  if (write_error) {
    connected_.store(false);
    return false;
  }
  return true;
}

nlohmann::json DataPlane::RecordResponse(const ExecutionRecord& record) const {
  nlohmann::json response = {
      {"type", "response"},
      {"protocol_version", kProtocolVersion},
      {"request_id", record.request_id},
      {"status", record.status},
      {"error_code",
       record.error_code.has_value() ? nlohmann::json(*record.error_code)
                                     : nlohmann::json(nullptr)},
      {"retryable", record.retryable},
      {"message", record.message},
      {"executed_at", record.executed_at},
      {"platform", kPlatformDarwin},
  };
  if (!record.result.is_null()) {
    response["result"] = record.result;
  }
  return response;
}

nlohmann::json DataPlane::ErrorResponse(const std::string& request_id,
                                        const std::string& code,
                                        const std::string& message,
                                        const std::string& status) const {
  return {
      {"type", "response"},
      {"protocol_version", kProtocolVersion},
      {"request_id", request_id},
      {"status", status},
      {"error_code", code},
      {"retryable", false},
      {"message", message},
      {"executed_at", NowRfc3339()},
      {"platform", kPlatformDarwin},
  };
}

void DataPlane::CancelActive() {
  std::lock_guard<std::mutex> lock(active_mutex_);
  for (auto& [request_id, cancelled] : active_) {
    (void)request_id;
    cancelled->store(true);
  }
}

void DataPlane::JoinWorkers() {
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers.swap(workers_);
  }
  for (auto& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void DataPlane::CloseSocket() {
  connected_.store(false);
  asio::error_code error;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, error);
  socket_.close(error);
}

void DataPlane::SleepBeforeReconnect(std::chrono::milliseconds delay) {
  std::random_device device;
  std::mt19937 generator(device());
  std::uniform_int_distribution<int> jitter(-200, 200);
  delay += std::chrono::milliseconds(jitter(generator));
  if (delay < std::chrono::milliseconds(100)) {
    delay = std::chrono::milliseconds(100);
  }
  std::unique_lock<std::mutex> lock(reconnect_mutex_);
  reconnect_cv_.wait_for(lock, delay,
                         [this]() { return stop_requested_.load(); });
}

std::pair<std::string, std::string> DataPlane::SplitAddress(
    const std::string& address) {
  const std::size_t separator = address.rfind(':');
  if (separator == std::string::npos) {
    return {"", ""};
  }
  return {address.substr(0, separator), address.substr(separator + 1)};
}

}  // namespace arec

