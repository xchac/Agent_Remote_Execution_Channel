#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "arec/crypto.hpp"
#include "arec/execution_engine.hpp"
#include "arec/frame_codec.hpp"
#include "arec/token.hpp"
#include "arec/types.hpp"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string runtime_secret() {
    return arec::base64url_encode(arec::random_bytes(32));
}

arec::TokenClaims claims_for(
    const std::string& session_id,
    const std::vector<std::string>& tools,
    std::int64_t ttl_seconds = 120) {
    arec::TokenClaims claims;
    claims.version = 1;
    claims.token_id = "runtime-token";
    claims.session_id = session_id;
    claims.allowed_tools = tools;
    claims.issued_at = arec::unix_time_seconds();
    claims.expires_at = claims.issued_at + ttl_seconds;
    claims.max_calls = 100;
    return claims;
}

nlohmann::json request_for(
    const std::string& request_id,
    const std::string& session_id,
    const std::string& tool,
    const nlohmann::json& params,
    const std::string& idempotency_key,
    const std::string& token,
    std::int64_t deadline_ms = 5000) {
    return {
        {"type", "request"},
        {"protocol_version", arec::kProtocolVersion},
        {"request_id", request_id},
        {"session_id", session_id},
        {"tool", tool},
        {"params", params},
        {"idempotency_key", idempotency_key},
        {"deadline_ms", deadline_ms},
        {"token", token},
    };
}

void test_frame_codec() {
    const auto frame = arec::encode_frame(R"({"value":"one"})");
    arec::FrameCodec codec;

    expect(
        codec.push(reinterpret_cast<const std::uint8_t*>(frame.data()), 2U) ==
            arec::FrameDecodeResult::need_more,
        "partial header should need more data");
    expect(
        codec.push(reinterpret_cast<const std::uint8_t*>(frame.data() + 2U), frame.size() - 2U) ==
            arec::FrameDecodeResult::need_more,
        "push should not parse immediately");

    std::string payload;
    std::string error;
    expect(
        codec.next(payload, error) == arec::FrameDecodeResult::frame,
        "complete frame should decode");
    expect(payload == R"({"value":"one"})", "decoded payload mismatch");

    const auto second_frame = arec::encode_frame(R"({"value":"two"})");
    codec.push(second_frame);
    codec.push(frame);
    expect(codec.next(payload, error) == arec::FrameDecodeResult::frame, "first coalesced frame missing");
    expect(payload == R"({"value":"two"})", "first coalesced payload mismatch");
    expect(codec.next(payload, error) == arec::FrameDecodeResult::frame, "second coalesced frame missing");
    expect(payload == R"({"value":"one"})", "second coalesced payload mismatch");

    arec::FrameCodec zero_length;
    zero_length.push(std::string("\0\0\0\0", 4U));
    expect(
        zero_length.next(payload, error) == arec::FrameDecodeResult::error,
        "zero-length frame must fail");

    arec::FrameCodec oversized;
    oversized.push(std::string("\x00\x40\x00\x01", 4U));
    expect(
        oversized.next(payload, error) == arec::FrameDecodeResult::error,
        "oversized frame must fail");
}

void test_crypto_and_token() {
    const std::vector<std::uint8_t> bytes = {0U, 1U, 2U, 250U, 251U, 252U, 253U, 254U, 255U};
    const std::string encoded = arec::base64url_encode(bytes);
    std::vector<std::uint8_t> decoded;
    expect(arec::base64url_decode(encoded, decoded), "base64url decode failed");
    expect(decoded == bytes, "base64url round trip mismatch");

    expect(
        arec::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 empty vector mismatch");

    const std::string secret = runtime_secret();
    const std::string session_id = "runtime-session";
    const auto claims = claims_for(session_id, {"system_info", "read_file"});
    const auto token = arec::make_capability_token(claims, secret);

    const auto valid = arec::validate_capability_token(
        token,
        secret,
        session_id,
        "system_info",
        arec::unix_time_seconds());
    expect(valid.ok, "valid token should be accepted");
    expect(valid.claims.session_id == session_id, "token session mismatch");

    const auto unauthorized = arec::validate_capability_token(
        token,
        secret,
        session_id,
        "exec_command",
        arec::unix_time_seconds());
    expect(!unauthorized.ok, "tool outside scope should be rejected");

    const auto wrong_session = arec::validate_capability_token(
        token,
        secret,
        "different-session",
        "system_info",
        arec::unix_time_seconds());
    expect(!wrong_session.ok, "different session should be rejected");

    const auto expired = arec::validate_capability_token(
        token,
        secret,
        session_id,
        "system_info",
        claims.expires_at + 1);
    expect(!expired.ok, "expired token should be rejected");
}

void test_execution_engine() {
    const std::string secret = runtime_secret();
    const std::string session_id = "runtime-session";
    arec::ExecutionEngine engine(
        secret,
        "windows",
        32,
        std::chrono::milliseconds(60000));

    const auto all_tools = std::vector<std::string>{
        "system_info",
        "exec_command",
        "read_file",
    };
    const auto token = arec::make_capability_token(
        claims_for(session_id, all_tools),
        secret);

    const auto system_request = request_for(
        "request-system",
        session_id,
        "system_info",
        nlohmann::json::object(),
        "idempotency-system",
        token);
    const auto system_response = engine.handle_request(system_request);
    expect(system_response.status == arec::Status::ok, "system_info should succeed");
    expect(
        system_response.platform == "windows" ||
            system_response.result.at("os").get<std::string>().find("Windows") == 0U,
        "system_info should report Windows");

    const auto duplicate = engine.handle_request(
        request_for(
            "request-system-copy",
            session_id,
            "system_info",
            nlohmann::json::object(),
            "idempotency-system",
            token));
    expect(duplicate.status == arec::Status::ok, "idempotent repeat should succeed");
    expect(duplicate.result == system_response.result, "idempotent result should be stable");

    const auto conflict = engine.handle_request(
        request_for(
            "request-conflict",
            session_id,
            "read_file",
            {{"path", "unused"}},
            "idempotency-system",
            token));
    expect(
        conflict.error_code == "IDEMPOTENCY_CONFLICT",
        "same idempotency key with different params must conflict");

    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() /
        ("arec-test-" + arec::sha256_hex(runtime_secret()) + ".txt");
    {
        std::ofstream output(temporary, std::ios::binary);
        output << "hello from AREC";
    }

    const auto read_response = engine.handle_request(
        request_for(
            "request-read",
            session_id,
            "read_file",
            {{"path", temporary.u8string()}, {"max_bytes", 64}},
            "idempotency-read",
            token));
    expect(read_response.status == arec::Status::ok, "read_file should succeed");
    expect(
        read_response.result.at("content").get<std::string>() == "hello from AREC",
        "read_file content mismatch");

    const auto truncated_response = engine.handle_request(
        request_for(
            "request-read-truncated",
            session_id,
            "read_file",
            {{"path", temporary.u8string()}, {"max_bytes", 5}},
            "idempotency-read-truncated",
            token));
    expect(
        truncated_response.result.at("truncated").get<bool>(),
        "read_file should mark truncated content");
    expect(
        truncated_response.result.at("bytes_read").get<std::size_t>() == 5U,
        "read_file should enforce max_bytes");
    std::filesystem::remove(temporary);

    const auto exec_response = engine.handle_request(
        request_for(
            "request-exec",
            session_id,
            "exec_command",
            {{"command", "echo arec-ok"}},
            "idempotency-exec",
            token));
    expect(exec_response.status == arec::Status::ok, "exec_command should succeed");
    expect(
        exec_response.result.at("stdout").get<std::string>().find("arec-ok") !=
            std::string::npos,
        "exec_command stdout mismatch");
    expect(
        exec_response.result.at("exit_code").get<std::int64_t>() == 0,
        "exec_command exit code should be zero");

    const auto truncated_output = engine.handle_request(
        request_for(
            "request-exec-truncated",
            session_id,
            "exec_command",
            {
                {
                    "command",
                    "for /L %i in (1,1,10000) do @echo 0123456789012345678901234567890123456789",
                },
            },
            "idempotency-exec-truncated",
            token));
    expect(truncated_output.status == arec::Status::ok, "truncated output command should succeed");
    expect(
        truncated_output.result.at("stdout_truncated").get<bool>(),
        "large stdout should be marked truncated");

    const std::filesystem::path concurrent_marker =
        std::filesystem::temp_directory_path() /
        ("arec-concurrent-" + arec::sha256_hex(runtime_secret()) + ".txt");
    std::filesystem::remove(concurrent_marker);
    const std::string concurrent_command =
        "echo duplicate>>\"" + concurrent_marker.u8string() +
        "\" & ping -n 2 127.0.0.1 >NUL";
    arec::ToolResponse concurrent_responses[2];
    std::thread first_worker(
        [&] {
            concurrent_responses[0] = engine.handle_request(
                request_for(
                    "request-concurrent-a",
                    session_id,
                    "exec_command",
                    {{"command", concurrent_command}},
                    "idempotency-concurrent",
                    token));
        });
    std::thread second_worker(
        [&] {
            concurrent_responses[1] = engine.handle_request(
                request_for(
                    "request-concurrent-b",
                    session_id,
                    "exec_command",
                    {{"command", concurrent_command}},
                    "idempotency-concurrent",
                    token));
        });
    first_worker.join();
    second_worker.join();
    expect(
        concurrent_responses[0].status == arec::Status::ok &&
            concurrent_responses[1].status == arec::Status::ok,
        "concurrent idempotent requests should both succeed");
    {
        std::ifstream marker_input(concurrent_marker);
        std::string line;
        std::size_t lines = 0;
        while (std::getline(marker_input, line)) {
            ++lines;
        }
        expect(lines == 1U, "concurrent idempotent command should execute once");
    }
    std::filesystem::remove(concurrent_marker);

    const auto timeout_started = std::chrono::steady_clock::now();
    const auto timeout_response = engine.handle_request(
        request_for(
            "request-timeout",
            session_id,
            "exec_command",
            {{"command", "ping -n 5 127.0.0.1 >NUL"}},
            "idempotency-timeout",
            token,
            100));
    const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_started;
    expect(timeout_response.status == arec::Status::timeout, "exec_command should time out");
    expect(!timeout_response.retryable, "exec_command timeout must not be retryable");
    expect(
        timeout_elapsed < std::chrono::seconds(3),
        "command process tree should be terminated promptly");

    const auto query = engine.handle_query(
        {{"type", "query"}, {"protocol_version", "1"}, {"request_id", "request-timeout"}});
    expect(query.at("status").get<std::string>() == "timeout", "query should retain timeout state");
    expect(!query.at("retryable").get<bool>(), "query timeout should preserve non-retryable state");

    const auto unknown_query = engine.handle_query(
        {{"type", "query"}, {"protocol_version", "1"}, {"request_id", "missing-request"}});
    expect(unknown_query.at("status").get<std::string>() == "unknown", "missing query should be unknown");

    const std::string limited_secret = runtime_secret();
    arec::ExecutionEngine limited_engine(limited_secret);
    const std::string limited_session = "limited-session";
    const auto limited_token = arec::make_capability_token(
        claims_for(limited_session, {"system_info"}),
        limited_secret);
    const auto unauthorized = limited_engine.handle_request(
        request_for(
            "request-unauthorized",
            limited_session,
            "exec_command",
            {{"command", "echo denied"}},
            "idempotency-unauthorized",
            limited_token));
    expect(unauthorized.error_code == "UNAUTHORIZED", "out-of-scope tool should be unauthorized");

    const auto invalid_deadline = engine.handle_request(
        request_for(
            "request-invalid-deadline",
            session_id,
            "system_info",
            nlohmann::json::object(),
            "idempotency-deadline",
            token,
            0));
    expect(
        invalid_deadline.error_code == "INVALID_PARAMS",
        "invalid deadline should be rejected");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"frame codec", test_frame_codec},
        {"crypto and token", test_crypto_and_token},
        {"execution engine", test_execution_engine},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << exception.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << "all unit tests passed\n";
    return 0;
}
