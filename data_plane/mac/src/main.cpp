#include <asio.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "arec/data_plane.hpp"

namespace {

std::string EnvOr(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

}  // namespace

int main() {
  const std::string secret = EnvOr("AREC_HMAC_SECRET", "");
  if (secret.empty()) {
    std::cerr << "arec-data-plane: AREC_HMAC_SECRET is required\n";
    return 2;
  }
  arec::DataPlaneConfig config;
  config.gateway_address =
      EnvOr("AREC_GATEWAY_DATA_ADDR", "127.0.0.1:9000");
  config.agent_id = EnvOr("AREC_AGENT_ID", "mac-a");
  config.hmac_secret = secret;

  try {
    arec::DataPlane data_plane(config);
    std::thread worker([&data_plane]() { data_plane.Run(); });

    asio::io_context signal_context;
    asio::signal_set signals(signal_context, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code&, int) {
      data_plane.Stop();
      signal_context.stop();
    });
    std::cerr << "arec-data-plane: connecting to " << config.gateway_address
              << " as " << config.agent_id << '\n';
    signal_context.run();
    data_plane.Stop();
    if (worker.joinable()) {
      worker.join();
    }
  } catch (const std::exception& error) {
    std::cerr << "arec-data-plane: fatal: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

