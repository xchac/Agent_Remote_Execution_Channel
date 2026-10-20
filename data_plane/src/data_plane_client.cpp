#include "arec/data_plane_client.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

#include "arec/frame_codec.hpp"
#include "arec/types.hpp"

namespace arec {
namespace {

std::string read_frame_payload(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, 4> header{};
    asio::read(socket, asio::buffer(header));
    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[0]) << 24U) |
        (static_cast<std::uint32_t>(header[1]) << 16U) |
        (static_cast<std::uint32_t>(header[2]) << 8U) |
        static_cast<std::uint32_t>(header[3]);
    if (length == 0U || length > kMaxFrameBytes) {
        throw std::invalid_argument("invalid data-plane frame length");
    }

    std::string payload(length, '\0');
    asio::read(socket, asio::buffer(payload));
    return payload;
}

nlohmann::json read_json_frame(asio::ip::tcp::socket& socket) {
    return nlohmann::json::parse(read_frame_payload(socket));
}

std::chrono::milliseconds reconnect_delay(
    std::chrono::milliseconds current,
    std::chrono::milliseconds maximum) {
    std::random_device device;
    std::mt19937 generator(device());
    const auto jitter_bound = std::max<std::int64_t>(1, current.count() / 5);
    std::uniform_int_distribution<std::int64_t> jitter(0, jitter_bound);
    const auto delay = std::min<std::int64_t>(
        maximum.count(),
        current.count() + jitter(generator));
    return std::chrono::milliseconds(delay);
}

std::string string_field(const nlohmann::json& message, const char* name) {
    const auto iterator = message.find(name);
    if (iterator == message.end() || !iterator->is_string()) {
        return {};
    }
    return iterator->get<std::string>();
}

}  // namespace

DataPlaneClient::DataPlaneClient(
    DataPlaneConfig config,
    std::shared_ptr<ExecutionEngine> engine)
    : config_(std::move(config)),
      engine_(std::move(engine)) {
    if (!engine_) {
        throw std::invalid_argument("execution engine is required");
    }
    if (config_.host.empty() || config_.port.empty() || config_.agent_id.empty()) {
        throw std::invalid_argument("gateway address and agent id are required");
    }
    if (config_.reconnect_min.count() <= 0 ||
        config_.reconnect_max < config_.reconnect_min) {
        throw std::invalid_argument("reconnect delay configuration is invalid");
    }
}

DataPlaneClient::~DataPlaneClient() {
    stop();
    join_workers();
}

void DataPlaneClient::run() {
    asio::io_context context;
    auto delay = config_.reconnect_min;

    while (!stop_requested_.load()) {
        try {
            auto session = std::make_shared<Session>(context);
            asio::ip::tcp::resolver resolver(context);
            const auto endpoints = resolver.resolve(config_.host, config_.port);
            asio::connect(session->socket, endpoints);

            {
                std::lock_guard<std::mutex> lock(active_session_mutex_);
                active_session_ = session;
            }

            std::cerr << "connected to gateway " << config_.host << ":" << config_.port
                      << " as " << config_.agent_id << '\n';
            delay = config_.reconnect_min;
            run_session(session);
        } catch (const std::system_error& exception) {
            if (!stop_requested_.load()) {
                std::cerr << "data-plane connection error: " << exception.code().message()
                          << '\n';
            }
        } catch (const std::exception&) {
            if (!stop_requested_.load()) {
                std::cerr << "data-plane session failed; reconnecting\n";
            }
        }

        {
            std::lock_guard<std::mutex> lock(active_session_mutex_);
            if (active_session_) {
                active_session_->stopped.store(true);
                std::error_code ignored;
                active_session_->socket.close(ignored);
                active_session_.reset();
            }
        }

        if (stop_requested_.load()) {
            break;
        }

        const auto sleep_delay = reconnect_delay(delay, config_.reconnect_max);
        const auto wake_at = std::chrono::steady_clock::now() + sleep_delay;
        while (!stop_requested_.load() && std::chrono::steady_clock::now() < wake_at) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        delay = std::min(delay * 2, config_.reconnect_max);
    }

    join_workers();
}

void DataPlaneClient::stop() {
    stop_requested_.store(true);
    std::lock_guard<std::mutex> lock(active_session_mutex_);
    if (active_session_) {
        active_session_->stopped.store(true);
        std::error_code ignored;
        active_session_->socket.close(ignored);
    }
}

void DataPlaneClient::run_session(const std::shared_ptr<Session>& session) {
    if (!send_registration(session) || !wait_for_registration_ack(session)) {
        throw std::runtime_error("gateway registration was rejected");
    }

    while (!stop_requested_.load() && !session->stopped.load()) {
        nlohmann::json message;
        try {
            const std::string payload = read_frame_payload(session->socket);
            message = nlohmann::json::parse(payload);
        } catch (const std::invalid_argument&) {
            send_protocol_error(session, "", "invalid data-plane frame length");
            throw;
        } catch (const nlohmann::json::parse_error&) {
            send_protocol_error(session, "", "invalid JSON payload");
            continue;
        }
        if (!message.is_object()) {
            send_protocol_error(session, "", "message must be a JSON object");
            continue;
        }
        dispatch_message(session, message);
    }
}

bool DataPlaneClient::send_json(
    const std::shared_ptr<Session>& session,
    const nlohmann::json& message) {
    if (session->stopped.load()) {
        return false;
    }
    try {
        const std::string frame = encode_frame(message.dump());
        std::lock_guard<std::mutex> lock(session->write_mutex);
        asio::write(session->socket, asio::buffer(frame));
        return true;
    } catch (const std::exception&) {
        session->stopped.store(true);
        return false;
    }
}

bool DataPlaneClient::send_registration(const std::shared_ptr<Session>& session) {
    return send_json(
        session,
        {
            {"type", "register"},
            {"protocol_version", kProtocolVersion},
            {"agent_id", config_.agent_id},
            {"platform", "windows"},
            {"tools", ExecutionEngine::tool_definitions()},
        });
}

bool DataPlaneClient::wait_for_registration_ack(
    const std::shared_ptr<Session>& session) {
    const auto response = read_json_frame(session->socket);
    return response.value("type", "") == "register_ack" &&
        response.value("protocol_version", "") == kProtocolVersion &&
        response.value("accepted", false);
}

void DataPlaneClient::dispatch_message(
    const std::shared_ptr<Session>& session,
    const nlohmann::json& message) {
    const std::string type = string_field(message, "type");
    const std::string request_id = string_field(message, "request_id");

    if (string_field(message, "protocol_version") != kProtocolVersion) {
        send_protocol_error(session, request_id, "unsupported protocol version");
        return;
    }

    if (type != "request" && type != "query") {
        send_protocol_error(session, request_id, "unknown message type");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back(
            [this, session, message, type] {
                try {
                    if (type == "request") {
                        const auto response = engine_->handle_request(message);
                        auto payload = response.to_json();
                        payload["type"] = "response";
                        payload["protocol_version"] = kProtocolVersion;
                        payload["request_id"] = string_field(message, "request_id");
                        send_json(session, payload);
                    } else {
                        send_json(session, engine_->handle_query(message));
                    }
                } catch (const std::exception&) {
                    send_protocol_error(
                        session,
                        string_field(message, "request_id"),
                        "message processing failed");
                }
            });
    }
}

void DataPlaneClient::send_protocol_error(
    const std::shared_ptr<Session>& session,
    const std::string& request_id,
    const std::string& detail) {
    send_json(
        session,
        {
            {"type", "response"},
            {"protocol_version", kProtocolVersion},
            {"request_id", request_id},
            {
                "status",
                "failed",
            },
            {"error_code", "PROTOCOL_ERROR"},
            {"retryable", false},
            {"message", detail},
            {"platform", "windows"},
        });
}

void DataPlaneClient::join_workers() {
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

}  // namespace arec
