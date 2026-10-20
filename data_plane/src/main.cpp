#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <Windows.h>

#include "arec/data_plane_client.hpp"
#include "arec/execution_engine.hpp"

namespace {

arec::DataPlaneClient* g_client = nullptr;

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        if (g_client != nullptr) {
            g_client->stop();
        }
        return TRUE;
    }
    return FALSE;
}

void split_address(
    const std::string& address,
    std::string& host,
    std::string& port) {
    const auto separator = address.rfind(':');
    if (separator == std::string::npos ||
        separator == 0U ||
        separator + 1U >= address.size()) {
        throw std::invalid_argument("gateway address must use host:port");
    }
    host = address.substr(0U, separator);
    port = address.substr(separator + 1U);
}

std::string optional_argument(
    int argc,
    char** argv,
    const std::string& name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (name == argv[index]) {
            return argv[index + 1];
        }
    }
    return {};
}

std::string environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string gateway_address =
            optional_argument(argc, argv, "--gateway");
        if (gateway_address.empty()) {
            gateway_address = environment_value("AREC_GATEWAY_DATA_ADDR");
        }
        if (gateway_address.empty()) {
            gateway_address = "127.0.0.1:9000";
        }

        std::string agent_id = optional_argument(argc, argv, "--agent-id");
        if (agent_id.empty()) {
            agent_id = environment_value("AREC_AGENT_ID");
        }
        if (agent_id.empty()) {
            agent_id = "windows-b";
        }

        const std::string secret = environment_value("AREC_HMAC_SECRET");
        if (secret.empty()) {
            std::cerr << "AREC_HMAC_SECRET is required\n";
            return 2;
        }

        arec::DataPlaneConfig config;
        split_address(gateway_address, config.host, config.port);
        config.agent_id = agent_id;

        auto engine = std::make_shared<arec::ExecutionEngine>(secret);
        arec::DataPlaneClient client(config, engine);
        g_client = &client;
        SetConsoleCtrlHandler(console_handler, TRUE);

        std::cout << "AREC data plane starting\n";
        client.run();
        SetConsoleCtrlHandler(console_handler, FALSE);
        g_client = nullptr;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "startup failed: " << exception.what() << '\n';
        return 1;
    }
}
