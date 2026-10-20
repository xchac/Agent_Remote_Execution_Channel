#include "arec/frame.hpp"

#include <algorithm>
#include <cstring>

namespace arec {

FrameDecoder::FrameDecoder(std::uint32_t max_bytes) : max_bytes_(max_bytes) {}

bool FrameDecoder::Feed(const void* data, std::size_t size, FrameError* error) {
  if (failed_) {
    if (error != nullptr) {
      *error = error_;
    }
    return false;
  }
  if (size == 0) {
    return true;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + size);
  if (buffer_.size() > (1ULL << 31)) {
    failed_ = true;
    error_ = FrameError::kTooLarge;
    if (error != nullptr) {
      *error = error_;
    }
    return false;
  }
  return true;
}

bool FrameDecoder::Next(std::string* payload, FrameError* error) {
  if (payload == nullptr) {
    return false;
  }
  payload->clear();
  if (failed_) {
    if (error != nullptr) {
      *error = error_;
    }
    return false;
  }
  if (buffer_.size() - offset_ < 4) {
    if (error != nullptr) {
      *error = FrameError::kIncomplete;
    }
    return false;
  }

  const std::uint32_t length =
      (static_cast<std::uint32_t>(buffer_[offset_]) << 24U) |
      (static_cast<std::uint32_t>(buffer_[offset_ + 1]) << 16U) |
      (static_cast<std::uint32_t>(buffer_[offset_ + 2]) << 8U) |
      static_cast<std::uint32_t>(buffer_[offset_ + 3]);
  if (length == 0) {
    failed_ = true;
    error_ = FrameError::kZeroLength;
    if (error != nullptr) {
      *error = error_;
    }
    return false;
  }
  if (length > max_bytes_) {
    failed_ = true;
    error_ = FrameError::kTooLarge;
    if (error != nullptr) {
      *error = error_;
    }
    return false;
  }
  if (buffer_.size() - offset_ < 4ULL + length) {
    if (error != nullptr) {
      *error = FrameError::kIncomplete;
    }
    return false;
  }

  payload->assign(reinterpret_cast<const char*>(buffer_.data() + offset_ + 4),
                  length);
  offset_ += 4ULL + length;
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  } else if (offset_ > 64U * 1024U && offset_ * 2 > buffer_.size()) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  if (error != nullptr) {
    *error = FrameError::kNone;
  }
  return true;
}

void FrameDecoder::Reset() {
  offset_ = 0;
  buffer_.clear();
  failed_ = false;
  error_ = FrameError::kNone;
}

std::vector<std::uint8_t> EncodeFrame(const std::string& payload,
                                      std::uint32_t max_bytes,
                                      FrameError* error) {
  if (payload.empty()) {
    if (error != nullptr) {
      *error = FrameError::kZeroLength;
    }
    return {};
  }
  if (payload.size() > max_bytes) {
    if (error != nullptr) {
      *error = FrameError::kTooLarge;
    }
    return {};
  }
  const auto length = static_cast<std::uint32_t>(payload.size());
  std::vector<std::uint8_t> frame(4 + payload.size());
  frame[0] = static_cast<std::uint8_t>((length >> 24U) & 0xffU);
  frame[1] = static_cast<std::uint8_t>((length >> 16U) & 0xffU);
  frame[2] = static_cast<std::uint8_t>((length >> 8U) & 0xffU);
  frame[3] = static_cast<std::uint8_t>(length & 0xffU);
  std::memcpy(frame.data() + 4, payload.data(), payload.size());
  if (error != nullptr) {
    *error = FrameError::kNone;
  }
  return frame;
}

}  // namespace arec

