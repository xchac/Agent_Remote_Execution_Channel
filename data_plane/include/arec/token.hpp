#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arec {

struct TokenClaims {
    int version = 1;
    std::string token_id;
    std::string session_id;
    std::vector<std::string> allowed_tools;
    std::int64_t issued_at = 0;
    std::int64_t expires_at = 0;
    std::int32_t max_calls = 0;
};

struct TokenValidation {
    bool ok = false;
    std::string error;
    TokenClaims claims;
};

std::string make_capability_token(const TokenClaims& claims, const std::string& secret);

TokenValidation validate_capability_token(
    const std::string& token,
    const std::string& secret,
    const std::string& session_id,
    const std::string& required_tool,
    std::int64_t now_seconds);

}  // namespace arec
