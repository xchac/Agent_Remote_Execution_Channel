#include "arec/frame_codec.hpp"

#include <limits>
#include <stdexcept>

#include "arec/types.hpp"

namespace arec {

FrameDecodeResult FrameCodec::push(const std::uint8_t* data, std::size_t size) {
    if (failed_) {
        return FrameDecodeResult::error;
    }
    if (data == nullptr && size != 0) {
        failed_ = true;
        return FrameDecodeResult::error;
    }
    if (size > kMaxFrameBytes + 4U || buffer_.size() > kMaxFrameBytes + 4U - size) {
        failed_ = true;
        return FrameDecodeResult::error;
    }
    buffer_.append(reinterpret_cast<const char*>(data), size);
    return FrameDecodeResult::need_more;
}

FrameDecodeResult FrameCodec::push(const std::string& data) {
    return push(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

FrameDecodeResult FrameCodec::next(std::string& payload, std::string& error) {
    payload.clear();
    error.clear();

    if (failed_) {
        error = "frame decoder is in a failed state";
        return FrameDecodeResult::error;
    }
    if (buffer_.size() < 4U) {
        return FrameDecodeResult::need_more;
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer_.data());
    const std::uint32_t length =
        (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);

    if (length == 0U) {
        failed_ = true;
        error = "zero-length frame";
        return FrameDecodeResult::error;
    }
    if (length > kMaxFrameBytes) {
        failed_ = true;
        error = "frame exceeds 4 MiB limit";
        return FrameDecodeResult::error;
    }

    const std::size_t frame_size = 4U + static_cast<std::size_t>(length);
    if (buffer_.size() < frame_size) {
        return FrameDecodeResult::need_more;
    }

    payload.assign(buffer_.data() + 4U, length);
    buffer_.erase(0U, frame_size);
    return FrameDecodeResult::frame;
}

void FrameCodec::reset() {
    buffer_.clear();
    failed_ = false;
}

std::string encode_frame(const std::string& payload) {
    if (payload.empty()) {
        throw std::invalid_argument("cannot encode an empty frame");
    }
    if (payload.size() > kMaxFrameBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("frame exceeds 4 MiB limit");
    }

    const auto length = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.reserve(4U + payload.size());
    frame.push_back(static_cast<char>((length >> 24U) & 0xFFU));
    frame.push_back(static_cast<char>((length >> 16U) & 0xFFU));
    frame.push_back(static_cast<char>((length >> 8U) & 0xFFU));
    frame.push_back(static_cast<char>(length & 0xFFU));
    frame.append(payload);
    return frame;
}

}  // namespace arec
