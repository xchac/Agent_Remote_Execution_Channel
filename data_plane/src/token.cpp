#include "arec/token.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "arec/crypto.hpp"

namespace arec {
namespace {

std::vector<std::uint8_t> as_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

bool contains(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

TokenClaims parse_claims(const nlohmann::json& payload) {
    TokenClaims claims;
    claims.version = payload.at("version").get<int>();
    claims.token_id = payload.at("token_id").get<std::string>();
    claims.session_id = payload.at("session_id").get<std::string>();
    claims.allowed_tools = payload.at("allowed_tools").get<std::vector<std::string>>();
    claims.issued_at = payload.at("issued_at").get<std::int64_t>();
    claims.expires_at = payload.at("expires_at").get<std::int64_t>();
    claims.max_calls = payload.at("max_calls").get<std::int32_t>();
    return claims;
}

nlohmann::json claims_to_json(const TokenClaims& claims) {
    return {
        {"version", claims.version},
        {"token_id", claims.token_id},
        {"session_id", claims.session_id},
        {"allowed_tools", claims.allowed_tools},
        {"issued_at", claims.issued_at},
        {"expires_at", claims.expires_at},
        {"max_calls", claims.max_calls},
    };
}

}  // namespace

std::string make_capability_token(const TokenClaims& claims, const std::string& secret) {
    if (secret.empty()) {
        throw std::invalid_argument("HMAC secret must not be empty");
    }
    const std::string payload_text = claims_to_json(claims).dump();
    const std::string encoded_payload = base64url_encode(as_bytes(payload_text));
    const auto signature = hmac_sha256(secret, as_bytes(encoded_payload));
    return encoded_payload + "." + base64url_encode(signature);
}

TokenValidation validate_capability_token(
    const std::string& token,
    const std::string& secret,
    const std::string& session_id,
    const std::string& required_tool,
    std::int64_t now_seconds) {
    TokenValidation result;

    if (token.empty() || secret.empty()) {
        result.error = "token or signing secret is missing";
        return result;
    }

    const std::size_t separator = token.find('.');
    if (separator == std::string::npos ||
        separator == 0U ||
        separator + 1U >= token.size() ||
        token.find('.', separator + 1U) != std::string::npos) {
        result.error = "token format is invalid";
        return result;
    }

    const std::string encoded_payload = token.substr(0U, separator);
    const std::string encoded_signature = token.substr(separator + 1U);

    std::vector<std::uint8_t> provided_signature;
    if (!base64url_decode(encoded_signature, provided_signature)) {
        result.error = "token signature encoding is invalid";
        return result;
    }

    const auto expected_signature = hmac_sha256(secret, as_bytes(encoded_payload));
    if (!constant_time_equal(provided_signature, expected_signature)) {
        result.error = "token signature is invalid";
        return result;
    }

    std::vector<std::uint8_t> payload_bytes;
    if (!base64url_decode(encoded_payload, payload_bytes)) {
        result.error = "token payload encoding is invalid";
        return result;
    }

    try {
        const auto payload = nlohmann::json::parse(
            std::string(payload_bytes.begin(), payload_bytes.end()));
        result.claims = parse_claims(payload);
    } catch (const std::exception&) {
        result.error = "token payload is invalid";
        return result;
    }

    if (result.claims.version != 1) {
        result.error = "token version is unsupported";
        return result;
    }
    if (result.claims.token_id.empty() || result.claims.session_id.empty()) {
        result.error = "token identity is incomplete";
        return result;
    }
    if (result.claims.max_calls <= 0) {
        result.error = "token call limit is invalid";
        return result;
    }
    if (result.claims.expires_at <= now_seconds) {
        result.error = "token has expired";
        return result;
    }
    if (result.claims.issued_at > now_seconds + 60) {
        result.error = "token issue time is invalid";
        return result;
    }
    if (result.claims.session_id != session_id) {
        result.error = "token session does not match request";
        return result;
    }
    if (!contains(result.claims.allowed_tools, required_tool)) {
        result.error = "tool is not authorized by token";
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace arec
