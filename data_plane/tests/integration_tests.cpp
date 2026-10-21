#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

#include "arec/crypto.hpp"
#include "arec/data_plane_client.hpp"
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

nlohmann::json read_json(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, 4> header{};
    asio::read(socket, asio::buffer(header));
    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[0]) << 24U) |
        (static_cast<std::uint32_t>(header[1]) << 16U) |
        (static_cast<std::uint32_t>(header[2]) << 8U) |
        static_cast<std::uint32_t>(header[3]);
    expect(length > 0U && length <= arec::kMaxFrameBytes, "mock received invalid frame length");
    std::string payload(length, '\0');
    asio::read(socket, asio::buffer(payload));
    return nlohmann::json::parse(payload);
}

void write_json(asio::ip::tcp::socket& socket, const nlohmann::json& message) {
    const std::string frame = arec::encode_frame(message.dump());
    asio::write(socket, asio::buffer(frame));
}

void set_read_timeout(asio::ip::tcp::socket& socket) {
#ifdef _WIN32
    const DWORD timeout = 5000;
    setsockopt(
        socket.native_handle(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout),
        sizeof(timeout));
#endif
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

void expect_registration(const nlohmann::json& registration) {
    expect(registration.at("type").get<std::string>() == "register", "expected registration");
    expect(
        registration.at("protocol_version").get<std::string>() == arec::kProtocolVersion,
        "registration protocol mismatch");
    expect(
        registration.at("platform").get<std::string>() == "windows",
        "registration platform mismatch");
    const auto& tools = registration.at("tools");
    expect(tools.is_array() && tools.size() == 6U, "registration should expose six tools");
    for (const auto& tool : tools) {
        expect(tool.at("name").is_string(), "registered tool name is invalid");
        expect(tool.at("platform").is_array(), "registered tool platform is invalid");
        expect(tool.at("params_schema").is_object(), "registered tool schema is invalid");
        expect(tool.at("idempotent").is_boolean(), "registered tool idempotency is invalid");
    }
}

void send_ack(asio::ip::tcp::socket& socket) {
    write_json(
        socket,
        {
            {"type", "register_ack"},
            {"protocol_version", arec::kProtocolVersion},
            {"accepted", true},
        });
}

}  // namespace

int main() {
    try {
        const std::string secret = runtime_secret();
        const std::string session_id = "integration-session";
        arec::TokenClaims claims;
        claims.version = 1;
        claims.token_id = "integration-token";
        claims.session_id = session_id;
        claims.allowed_tools = {
            "system_info",
            "exec_command",
            "read_file",
            "write_file",
            "list_dir",
            "create_dir",
        };
        claims.issued_at = arec::unix_time_seconds();
        claims.expires_at = claims.issued_at + 120;
        claims.max_calls = 100;
        const std::string token = arec::make_capability_token(claims, secret);

        asio::io_context context;
        asio::ip::tcp::acceptor acceptor(
            context,
            asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        const auto port = acceptor.local_endpoint().port();

        arec::DataPlaneConfig config;
        config.host = "127.0.0.1";
        config.port = std::to_string(port);
        config.agent_id = "windows-integration";
        config.reconnect_min = std::chrono::milliseconds(50);
        config.reconnect_max = std::chrono::milliseconds(100);

        auto engine = std::make_shared<arec::ExecutionEngine>(
            secret,
            "windows",
            128,
            std::chrono::milliseconds(60000));
        arec::DataPlaneClient client(config, engine);

        std::string server_error;
        std::thread server(
            [&] {
                try {
                    asio::ip::tcp::socket first(context);
                    acceptor.accept(first);
                    set_read_timeout(first);
                    expect_registration(read_json(first));
                    send_ack(first);

                    write_json(
                        first,
                        request_for(
                            "integration-system",
                            session_id,
                            "system_info",
                            nlohmann::json::object(),
                            "integration-idempotency",
                            token));
                    const auto first_response = read_json(first);
                    expect(
                        first_response.at("request_id").get<std::string>() == "integration-system",
                        "first response id mismatch");
                    expect(
                        first_response.at("status").get<std::string>() == "ok",
                        "first system_info should succeed");

                    const std::string invalid_json = "{not-json";
                    const std::string invalid_frame = arec::encode_frame(invalid_json);
                    asio::write(first, asio::buffer(invalid_frame));
                    const auto invalid_json_response = read_json(first);
                    expect(
                        invalid_json_response.at("error_code").get<std::string>() ==
                            "PROTOCOL_ERROR",
                        "invalid JSON should return protocol error");

                    write_json(first, {{"type", "unexpected"}, {"protocol_version", "1"}});
                    const auto protocol_error = read_json(first);
                    expect(
                        protocol_error.at("error_code").get<std::string>() == "PROTOCOL_ERROR",
                        "unknown message should return protocol error");

                    first.close();

                    asio::ip::tcp::socket second(context);
                    acceptor.accept(second);
                    set_read_timeout(second);
                    expect_registration(read_json(second));
                    send_ack(second);

                    write_json(
                        second,
                        {
                            {"type", "query"},
                            {"protocol_version", arec::kProtocolVersion},
                            {"request_id", "integration-system"},
                        });
                    const auto query_response = read_json(second);
                    expect(
                        query_response.at("type").get<std::string>() == "query_response",
                        "expected query response");
                    expect(
                        query_response.at("status").get<std::string>() == "ok",
                        "query should recover state after reconnect");

                    write_json(
                        second,
                        request_for(
                            "integration-timeout",
                            session_id,
                            "exec_command",
                            {{"command", "ping -n 5 127.0.0.1 >NUL"}},
                            "integration-timeout-key",
                            token,
                            100));
                    const auto timeout_response = read_json(second);
                    expect(
                        timeout_response.at("status").get<std::string>() == "timeout",
                        "integration timeout status mismatch");
                    expect(
                        !timeout_response.at("retryable").get<bool>(),
                        "exec_command timeout must not be retryable");

                    write_json(
                        second,
                        request_for(
                            "integration-unauthorized",
                            session_id,
                            "system_info",
                            nlohmann::json::object(),
                            "integration-unauthorized-key",
                            "invalid.signature"));
                    const auto unauthorized = read_json(second);
                    expect(
                        unauthorized.at("error_code").get<std::string>() == "UNAUTHORIZED",
                        "invalid token should be unauthorized");

                    second.close();
                } catch (const std::exception& exception) {
                    server_error = exception.what();
                    client.stop();
                }
            });

        std::thread client_thread([&] { client.run(); });
        server.join();
        client.stop();
        client_thread.join();

        if (!server_error.empty()) {
            throw std::runtime_error(server_error);
        }
        std::cout << "all integration tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "integration test failed: " << exception.what() << '\n';
        return 1;
    }
}
