#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arec {

struct CapabilityPayload {
  int version = 0;
  std::string token_id;
  std::string session_id;
  std::vector<std::string> allowed_tools;
  std::int64_t issued_at = 0;
  std::int64_t expires_at = 0;
  int max_calls = 0;
};

bool VerifyCapabilityToken(const std::string& token, const std::string& secret,
                           std::int64_t now_unix,
                           CapabilityPayload* payload,
                           std::string* error = nullptr);

std::string SignCapabilityToken(const CapabilityPayload& payload,
                                const std::string& secret);

std::string Base64UrlEncode(const std::vector<std::uint8_t>& value);
bool Base64UrlDecode(const std::string& value,
                     std::vector<std::uint8_t>* decoded);

}  // namespace arec

