#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arec {

std::string base64url_encode(const std::vector<std::uint8_t>& input);
bool base64url_decode(const std::string& input, std::vector<std::uint8_t>& output);

std::vector<std::uint8_t> hmac_sha256(
    const std::string& key,
    const std::vector<std::uint8_t>& data);

std::string sha256_hex(const std::string& data);

bool constant_time_equal(
    const std::vector<std::uint8_t>& left,
    const std::vector<std::uint8_t>& right);

std::vector<std::uint8_t> random_bytes(std::size_t size);

}  // namespace arec
