#include "arec/token.hpp"

#include <CommonCrypto/CommonHMAC.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace arec {
namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::vector<std::uint8_t> HmacSha256(const std::string& message,
                                     const std::string& secret) {
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
  CCHmac(kCCHmacAlgSHA256, secret.data(), secret.size(), message.data(),
         message.size(), digest.data());
  return std::vector<std::uint8_t>(digest.begin(), digest.end());
}

bool ConstantTimeEqual(const std::vector<std::uint8_t>& left,
                       const std::vector<std::uint8_t>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0;
}

}  // namespace

std::string Base64UrlEncode(const std::vector<std::uint8_t>& value) {
  std::string result;
  result.reserve((value.size() * 4U + 2U) / 3U);
  std::size_t index = 0;
  while (index + 3 <= value.size()) {
    const std::uint32_t chunk =
        (static_cast<std::uint32_t>(value[index]) << 16U) |
        (static_cast<std::uint32_t>(value[index + 1]) << 8U) |
        static_cast<std::uint32_t>(value[index + 2]);
    result.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
    result.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
    result.push_back(kAlphabet[(chunk >> 6U) & 0x3fU]);
    result.push_back(kAlphabet[chunk & 0x3fU]);
    index += 3;
  }
  if (value.size() - index == 1) {
    const std::uint32_t chunk =
        static_cast<std::uint32_t>(value[index]) << 16U;
    result.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
    result.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
  } else if (value.size() - index == 2) {
    const std::uint32_t chunk =
        (static_cast<std::uint32_t>(value[index]) << 16U) |
        (static_cast<std::uint32_t>(value[index + 1]) << 8U);
    result.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
    result.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
    result.push_back(kAlphabet[(chunk >> 6U) & 0x3fU]);
  }
  return result;
}

bool Base64UrlDecode(const std::string& value,
                     std::vector<std::uint8_t>* decoded) {
  if (decoded == nullptr || value.size() % 4U == 1U) {
    return false;
  }
  decoded->clear();
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (char character : value) {
    int index = -1;
    if (character >= 'A' && character <= 'Z') {
      index = character - 'A';
    } else if (character >= 'a' && character <= 'z') {
      index = character - 'a' + 26;
    } else if (character >= '0' && character <= '9') {
      index = character - '0' + 52;
    } else if (character == '-') {
      index = 62;
    } else if (character == '_') {
      index = 63;
    } else {
      return false;
    }
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(index);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded->push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return true;
}

std::string SignCapabilityToken(const CapabilityPayload& payload,
                                const std::string& secret) {
  nlohmann::json body = {
      {"version", payload.version},
      {"token_id", payload.token_id},
      {"session_id", payload.session_id},
      {"allowed_tools", payload.allowed_tools},
      {"issued_at", payload.issued_at},
      {"expires_at", payload.expires_at},
      {"max_calls", payload.max_calls},
  };
  const std::string serialized = body.dump();
  const std::vector<std::uint8_t> bytes(serialized.begin(), serialized.end());
  const std::string encoded = Base64UrlEncode(bytes);
  return encoded + "." + Base64UrlEncode(HmacSha256(encoded, secret));
}

bool VerifyCapabilityToken(const std::string& token, const std::string& secret,
                           std::int64_t now_unix,
                           CapabilityPayload* payload, std::string* error) {
  if (payload == nullptr || secret.empty()) {
    if (error != nullptr) {
      *error = "invalid verifier input";
    }
    return false;
  }
  const std::size_t separator = token.find('.');
  if (separator == std::string::npos ||
      token.find('.', separator + 1) != std::string::npos ||
      separator == 0 || separator + 1 == token.size()) {
    if (error != nullptr) {
      *error = "malformed token";
    }
    return false;
  }
  const std::string encoded_payload = token.substr(0, separator);
  std::vector<std::uint8_t> provided_signature;
  std::vector<std::uint8_t> payload_bytes;
  if (!Base64UrlDecode(token.substr(separator + 1), &provided_signature) ||
      !Base64UrlDecode(encoded_payload, &payload_bytes)) {
    if (error != nullptr) {
      *error = "malformed token encoding";
    }
    return false;
  }
  if (!ConstantTimeEqual(provided_signature,
                         HmacSha256(encoded_payload, secret))) {
    if (error != nullptr) {
      *error = "invalid token signature";
    }
    return false;
  }

  try {
    const std::string serialized(payload_bytes.begin(), payload_bytes.end());
    const nlohmann::json body = nlohmann::json::parse(serialized);
    CapabilityPayload parsed;
    parsed.version = body.at("version").get<int>();
    parsed.token_id = body.at("token_id").get<std::string>();
    parsed.session_id = body.at("session_id").get<std::string>();
    parsed.allowed_tools =
        body.at("allowed_tools").get<std::vector<std::string>>();
    parsed.issued_at = body.at("issued_at").get<std::int64_t>();
    parsed.expires_at = body.at("expires_at").get<std::int64_t>();
    parsed.max_calls = body.at("max_calls").get<int>();
    if (parsed.version != 1 || parsed.token_id.empty() ||
        parsed.session_id.empty() || parsed.expires_at <= 0 ||
        parsed.max_calls <= 0) {
      if (error != nullptr) {
        *error = "invalid token payload";
      }
      return false;
    }
    if (now_unix >= parsed.expires_at) {
      if (error != nullptr) {
        *error = "expired token";
      }
      return false;
    }
    *payload = std::move(parsed);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "invalid token payload";
    }
    return false;
  }
}

}  // namespace arec

