#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace arec {

enum class FrameDecodeResult {
    need_more,
    frame,
    error,
};

class FrameCodec {
public:
    FrameDecodeResult push(const std::uint8_t* data, std::size_t size);
    FrameDecodeResult push(const std::string& data);
    FrameDecodeResult next(std::string& payload, std::string& error);
    void reset();

private:
    std::string buffer_;
    bool failed_ = false;
};

std::string encode_frame(const std::string& payload);

}  // namespace arec
