#include "arec/crypto.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace arec {
namespace {

constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int base64_value(char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '-') {
        return 62;
    }
    if (value == '_') {
        return 63;
    }
    return -1;
}

#ifdef _WIN32
void check_status(NTSTATUS status, const char* operation) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

class AlgorithmHandle {
public:
    explicit AlgorithmHandle(ULONG flags) {
        check_status(
            BCryptOpenAlgorithmProvider(
                &handle_,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                flags),
            "BCryptOpenAlgorithmProvider");
    }

    ~AlgorithmHandle() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    BCRYPT_ALG_HANDLE get() const {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

std::vector<std::uint8_t> hash_sha256(
    const std::uint8_t* data,
    std::size_t size,
    const std::uint8_t* key,
    std::size_t key_size) {
    const ULONG flags = key != nullptr ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
    AlgorithmHandle algorithm(flags);

    DWORD object_size = 0;
    DWORD bytes_written = 0;
    check_status(
        BCryptGetProperty(
            algorithm.get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &bytes_written,
            0),
        "BCryptGetProperty");

    std::vector<std::uint8_t> object(object_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    check_status(
        BCryptCreateHash(
            algorithm.get(),
            &hash,
            object.data(),
            object_size,
            const_cast<PUCHAR>(key),
            static_cast<ULONG>(key_size),
            0),
        "BCryptCreateHash");

    try {
        if (size != 0U) {
            check_status(
                BCryptHashData(
                    hash,
                    const_cast<PUCHAR>(data),
                    static_cast<ULONG>(size),
                    0),
                "BCryptHashData");
        }

        std::vector<std::uint8_t> digest(32);
        check_status(
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0),
            "BCryptFinishHash");
        BCryptDestroyHash(hash);
        return digest;
    } catch (...) {
        BCryptDestroyHash(hash);
        throw;
    }
}
#endif

}  // namespace

std::string base64url_encode(const std::vector<std::uint8_t>& input) {
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);

    std::size_t index = 0;
    while (index + 3U <= input.size()) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(input[index]) << 16U) |
            (static_cast<std::uint32_t>(input[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(input[index + 2U]);
        output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[(value >> 6U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[value & 0x3FU]);
        index += 3U;
    }

    const std::size_t remaining = input.size() - index;
    if (remaining == 1U) {
        const std::uint32_t value = static_cast<std::uint32_t>(input[index]) << 16U;
        output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
    } else if (remaining == 2U) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(input[index]) << 16U) |
            (static_cast<std::uint32_t>(input[index + 1U]) << 8U);
        output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
        output.push_back(kBase64UrlAlphabet[(value >> 6U) & 0x3FU]);
    }
    return output;
}

bool base64url_decode(const std::string& input, std::vector<std::uint8_t>& output) {
    output.clear();
    std::uint64_t accumulator = 0;
    int bit_count = 0;
    bool padding_started = false;
    int significant_chars = 0;

    for (const char character : input) {
        if (character == '=') {
            padding_started = true;
            continue;
        }
        if (padding_started) {
            return false;
        }

        const int value = base64_value(character);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint64_t>(value);
        bit_count += 6;
        ++significant_chars;

        if (bit_count >= 8) {
            bit_count -= 8;
            output.push_back(static_cast<std::uint8_t>((accumulator >> bit_count) & 0xFFU));
        }
    }

    if (significant_chars == 0 || bit_count >= 6) {
        output.clear();
        return false;
    }
    if (bit_count > 0) {
        const std::uint64_t mask = (1ULL << bit_count) - 1ULL;
        if ((accumulator & mask) != 0U) {
            output.clear();
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> hmac_sha256(
    const std::string& key,
    const std::vector<std::uint8_t>& data) {
#ifdef _WIN32
    return hash_sha256(
        data.data(),
        data.size(),
        reinterpret_cast<const std::uint8_t*>(key.data()),
        key.size());
#else
    (void)key;
    (void)data;
    throw std::runtime_error("HMAC-SHA256 is not implemented for this platform");
#endif
}

std::string sha256_hex(const std::string& data) {
#ifdef _WIN32
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto digest = hash_sha256(bytes, data.size(), nullptr, 0);
#else
    (void)data;
    throw std::runtime_error("SHA-256 is not implemented for this platform");
#endif
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        output.push_back(digits[(byte >> 4U) & 0x0FU]);
        output.push_back(digits[byte & 0x0FU]);
    }
    return output;
}

bool constant_time_equal(
    const std::vector<std::uint8_t>& left,
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

std::vector<std::uint8_t> random_bytes(std::size_t size) {
    std::vector<std::uint8_t> output(size);
#ifdef _WIN32
    if (size != 0U) {
        check_status(
            BCryptGenRandom(
                nullptr,
                output.data(),
                static_cast<ULONG>(output.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "BCryptGenRandom");
    }
    return output;
#else
    throw std::runtime_error("random_bytes is not implemented for this platform");
#endif
}

}  // namespace arec
