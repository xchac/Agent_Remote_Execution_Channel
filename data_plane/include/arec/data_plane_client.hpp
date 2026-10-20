#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "arec/execution_engine.hpp"

namespace arec {

struct DataPlaneConfig {
    std::string host = "127.0.0.1";
    std::string port = "9000";
    std::string agent_id = "windows-b";
    std::chrono::milliseconds reconnect_min{1000};
    std::chrono::milliseconds reconnect_max{30000};
};

class DataPlaneClient {
public:
    DataPlaneClient(
        DataPlaneConfig config,
        std::shared_ptr<ExecutionEngine> engine);
    ~DataPlaneClient();

    void run();
    void stop();

private:
    struct Session {
        explicit Session(asio::io_context& context)
            : socket(context) {}

        asio::ip::tcp::socket socket;
        std::mutex write_mutex;
        std::atomic<bool> stopped{false};
    };

    void run_session(const std::shared_ptr<Session>& session);
    bool send_json(const std::shared_ptr<Session>& session, const nlohmann::json& message);
    bool send_registration(const std::shared_ptr<Session>& session);
    bool wait_for_registration_ack(const std::shared_ptr<Session>& session);
    void dispatch_message(
        const std::shared_ptr<Session>& session,
        const nlohmann::json& message);
    void send_protocol_error(
        const std::shared_ptr<Session>& session,
        const std::string& request_id,
        const std::string& detail);
    void join_workers();

    DataPlaneConfig config_;
    std::shared_ptr<ExecutionEngine> engine_;
    std::atomic<bool> stop_requested_{false};

    std::mutex active_session_mutex_;
    std::shared_ptr<Session> active_session_;

    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
};

}  // namespace arec
