#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "arec/cache.hpp"
#include "arec/data_plane.hpp"
#include "arec/frame.hpp"
#include "arec/protocol.hpp"
#include "arec/token.hpp"
#include "arec/tools.hpp"

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ReadFrame(asio::ip::tcp::socket& socket,
                      arec::FrameDecoder& decoder) {
  std::string payload;
  arec::FrameError error = arec::FrameError::kNone;
  while (!decoder.Next(&payload, &error)) {
    if (error != arec::FrameError::kNone &&
        error != arec::FrameError::kIncomplete) {
      throw std::runtime_error("frame decode failed");
    }
    std::array<char, 4096> buffer{};
    const std::size_t count = socket.read_some(asio::buffer(buffer));
    if (!decoder.Feed(buffer.data(), count, &error)) {
      throw std::runtime_error("frame feed failed");
    }
  }
  return payload;
}

void SendJson(asio::ip::tcp::socket& socket, const nlohmann::json& message) {
  const std::string payload = message.dump();
  const auto frame = arec::EncodeFrame(payload);
  asio::write(socket, asio::buffer(frame));
}

nlohmann::json ReadJson(asio::ip::tcp::socket& socket,
                        arec::FrameDecoder& decoder) {
  return nlohmann::json::parse(ReadFrame(socket, decoder));
}

void TestFrames() {
  arec::FrameDecoder decoder;
  const auto first = arec::EncodeFrame("first");
  const auto second = arec::EncodeFrame("second");
  std::vector<std::uint8_t> combined = first;
  combined.insert(combined.end(), second.begin(), second.end());
  for (std::uint8_t byte : combined) {
    Check(decoder.Feed(&byte, 1), "byte feed failed");
  }
  std::string payload;
  Check(decoder.Next(&payload) && payload == "first", "first frame failed");
  Check(decoder.Next(&payload) && payload == "second", "second frame failed");
  Check(arec::EncodeFrame("").empty(), "zero frame should fail");

  arec::FrameDecoder invalid;
  const std::array<std::uint8_t, 4> zero{0, 0, 0, 0};
  Check(invalid.Feed(zero.data(), zero.size()), "zero feed failed");
  arec::FrameError error = arec::FrameError::kNone;
  Check(!invalid.Next(&payload, &error) &&
            error == arec::FrameError::kZeroLength,
        "zero frame not rejected");
}

void TestTokens() {
  arec::CapabilityPayload payload;
  payload.version = 1;
  payload.token_id = "token-1";
  payload.session_id = "session-1";
  payload.allowed_tools = {"system_info"};
  payload.issued_at = 100;
  payload.expires_at = 200;
  payload.max_calls = 10;
  const auto token = arec::SignCapabilityToken(payload, "test-secret");
  arec::CapabilityPayload verified;
  std::string error;
  Check(arec::VerifyCapabilityToken(token, "test-secret", 150, &verified,
                                    &error),
        "valid token rejected: " + error);
  Check(verified.session_id == "session-1", "session mismatch");
  Check(!arec::VerifyCapabilityToken(token, "other-secret", 150, &verified,
                                     &error),
        "wrong secret accepted");
  Check(!arec::VerifyCapabilityToken(token + "x", "test-secret", 150,
                                     &verified, &error),
        "tampered token accepted");
  Check(!arec::VerifyCapabilityToken(token, "test-secret", 200, &verified,
                                     &error),
        "expired token accepted");
}

arec::ExecutionRecord TestRecord(const std::string& request_id,
                                 const std::string& key) {
  return {
      request_id,
      "session-1",
      key,
      "system_info",
      "hash",
      "pending",
      nlohmann::json::object(),
      std::nullopt,
      false,
      "",
      "",
      std::chrono::system_clock::now(),
  };
}

void TestCache() {
  arec::ExecutionCache cache(std::chrono::minutes(10), 2);
  const auto first = cache.Begin(TestRecord("request-1", "key-1"));
  Check(first.decision == arec::CacheDecision::kInserted, "insert failed");
  const auto duplicate = cache.Begin(TestRecord("request-2", "key-1"));
  Check(duplicate.decision == arec::CacheDecision::kDuplicate &&
            duplicate.record.request_id == "request-1",
        "duplicate failed");
  auto conflict = TestRecord("request-3", "key-1");
  conflict.params_hash = "different";
  Check(cache.Begin(conflict).decision == arec::CacheDecision::kConflict,
        "conflict failed");
  cache.Begin(TestRecord("request-2", "key-2"));
  cache.Begin(TestRecord("request-3", "key-3"));
  Check(!cache.Get("request-1").has_value(), "oldest cache entry not evicted");
}

void TestTools() {
  const auto definitions = arec::ToolDefinitions();
  Check(definitions.size() == 6, "tool registry should expose six tools");
  const auto system = arec::ToolExecutor::Execute(
      "system_info", nlohmann::json::object());
  Check(system.status == "ok" && system.result.contains("hostname") &&
            system.result.contains("os") && system.result.contains("arch"),
        "system_info failed");

  const auto path =
      std::filesystem::temp_directory_path() /
      ("arec-data-plane-test-" + std::to_string(getpid()) + ".txt");
  {
    std::ofstream output(path, std::ios::binary);
    output << std::string(300000, 'a');
  }
  const auto read = arec::ToolExecutor::Execute(
      "read_file", {{"path", path.string()}, {"max_bytes", 16}});
  Check(read.status == "ok" && read.result["content"] == std::string(16, 'a') &&
            read.result["truncated"].get<bool>(),
        "read_file truncation failed");
  std::filesystem::remove(path);

  const auto tool_root =
      std::filesystem::temp_directory_path() /
      ("arec-tools-test-" + std::to_string(getpid()));
  const auto nested = tool_root / "nested" / "child";
  std::filesystem::remove_all(tool_root);
  const auto created = arec::ToolExecutor::Execute(
      "create_dir", {{"path", nested.string()}});
  Check(created.status == "ok" && created.result["created"].get<bool>() &&
            std::filesystem::is_directory(nested),
        "create_dir failed");
  const auto created_again = arec::ToolExecutor::Execute(
      "create_dir", {{"path", nested.string()}});
  Check(created_again.status == "ok" &&
            !created_again.result["created"].get<bool>(),
        "existing create_dir should report created=false");

  const auto written = nested / "written.txt";
  const std::string content = "atomic content";
  const auto write = arec::ToolExecutor::Execute(
      "write_file", {{"path", written.string()}, {"content", content}});
  Check(write.status == "ok" && write.result["created"].get<bool>(),
        "write_file failed");
  {
    std::ifstream input(written, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    Check(actual == content, "write_file content mismatch");
  }

  const auto list = arec::ToolExecutor::Execute(
      "list_dir", {{"path", nested.string()}});
  Check(list.status == "ok" && list.result["entries"].size() == 1 &&
            list.result["entries"][0]["name"] == "written.txt" &&
            list.result["entries"][0]["type"] == "file",
        "list_dir failed");
  std::filesystem::remove_all(tool_root);

  const auto command = arec::ToolExecutor::Execute(
      "exec_command", {{"command", "printf hello; printf error >&2; exit 7"}},
      1000);
  Check(command.status == "ok" &&
            command.result["stdout"].get<std::string>() == "hello" &&
            command.result["stderr"].get<std::string>() == "error" &&
            command.result["exit_code"].get<int>() == 7,
        "exec_command streams or exit code failed");

  const auto started = std::chrono::steady_clock::now();
  const auto timeout = arec::ToolExecutor::Execute(
      "exec_command", {{"command", "sleep 2"}}, 100);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  Check(timeout.status == "timeout" &&
            timeout.error_code.value_or("") == "EXECUTION_TIMEOUT" &&
            elapsed < std::chrono::milliseconds(1500),
        "exec_command timeout or process-group termination failed");
}

void TestDataPlaneIntegration() {
  const std::string secret = "integration-test-secret";
  asio::io_context context;
  asio::ip::tcp::acceptor acceptor(
      context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  const auto address = "127.0.0.1:" +
                       std::to_string(acceptor.local_endpoint().port());

  arec::DataPlaneConfig config;
  config.gateway_address = address;
  config.agent_id = "mac-integration";
  config.hmac_secret = secret;
  arec::DataPlane data_plane(config);
  std::thread data_plane_thread([&data_plane]() { data_plane.Run(); });

  try {
    asio::ip::tcp::socket socket(context);
    acceptor.accept(socket);
    arec::FrameDecoder gateway_decoder;
    const auto registration = ReadJson(socket, gateway_decoder);
    Check(registration["type"] == "register" &&
              registration["platform"] == "darwin" &&
              registration["agent_id"] == "mac-integration" &&
              registration["tools"].size() == 6,
          "unexpected registration");
    SendJson(socket, {{"type", "register_ack"},
                      {"protocol_version", "1"},
                      {"accepted", true}});

    arec::CapabilityPayload payload;
    payload.version = 1;
    payload.token_id = "token-integration";
    payload.session_id = "session-integration";
    payload.allowed_tools = {
        "system_info", "exec_command", "read_file",
        "write_file", "list_dir", "create_dir"};
    payload.issued_at = static_cast<std::int64_t>(std::time(nullptr));
    payload.expires_at = payload.issued_at + 60;
    payload.max_calls = 100;
    const auto token = arec::SignCapabilityToken(payload, secret);

    const nlohmann::json request = {
        {"type", "request"},
        {"protocol_version", "1"},
        {"request_id", "request-integration"},
        {"session_id", payload.session_id},
        {"tool", "system_info"},
        {"params", nlohmann::json::object()},
        {"idempotency_key", "idem-integration"},
        {"deadline_ms", 1000},
        {"token", token},
    };
    SendJson(socket, request);
    const auto response = ReadJson(socket, gateway_decoder);
    Check(response["request_id"] == "request-integration" &&
              response["status"] == "ok" &&
              response["platform"] == "darwin",
          "data-plane request failed");

    SendJson(socket, request);
    const auto duplicate = ReadJson(socket, gateway_decoder);
    Check(duplicate["request_id"] == "request-integration" &&
              duplicate["status"] == "ok",
          "data-plane idempotency failed");

    SendJson(socket, {{"type", "query"},
                      {"protocol_version", "1"},
                      {"request_id", "request-integration"}});
    const auto queried = ReadJson(socket, gateway_decoder);
    Check(queried["type"] == "query_response" &&
              queried["status"] == "ok",
          "data-plane query failed");

    auto unauthorized = request;
    unauthorized["request_id"] = "request-unauthorized";
    unauthorized["token"] = token + "x";
    SendJson(socket, unauthorized);
    const auto unauthorized_response = ReadJson(socket, gateway_decoder);
    Check(unauthorized_response["status"] == "failed" &&
              unauthorized_response["error_code"] == "UNAUTHORIZED",
          "unauthorized token was not rejected");

    const nlohmann::json conflicting {
        {"type", "request"},
        {"protocol_version", "1"},
        {"request_id", "request-conflict"},
        {"session_id", payload.session_id},
        {"tool", "exec_command"},
        {"params", {{"command", "true"}}},
        {"idempotency_key", "idem-integration"},
        {"deadline_ms", 1000},
        {"token", token},
    };
    SendJson(socket, conflicting);
    const auto conflict_response = ReadJson(socket, gateway_decoder);
    Check(conflict_response["status"] == "failed" &&
              conflict_response["error_code"] == "IDEMPOTENCY_CONFLICT",
          "idempotency conflict was not rejected");

    SendJson(socket, {{"type", "query"},
                      {"protocol_version", "1"},
                      {"request_id", "request-missing"}});
    const auto missing_query = ReadJson(socket, gateway_decoder);
    Check(missing_query["type"] == "query_response" &&
              missing_query["status"] == "unknown",
          "missing query did not return unknown");
    socket.close();

    asio::ip::tcp::socket reconnected(context);
    acceptor.accept(reconnected);
    arec::FrameDecoder reconnect_decoder;
    const auto second_registration = ReadJson(reconnected, reconnect_decoder);
    Check(second_registration["type"] == "register",
          "data plane did not reconnect");
    reconnected.close();
  } catch (...) {
    data_plane.Stop();
    if (data_plane_thread.joinable()) {
      data_plane_thread.join();
    }
    throw;
  }

  data_plane.Stop();
  if (data_plane_thread.joinable()) {
    data_plane_thread.join();
  }
}

}  // namespace

int main() {
  try {
    TestFrames();
    TestTokens();
    TestCache();
    TestTools();
    TestDataPlaneIntegration();
    std::cout << "arec data-plane tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "arec data-plane tests failed: " << error.what() << '\n';
    return 1;
  }
}

