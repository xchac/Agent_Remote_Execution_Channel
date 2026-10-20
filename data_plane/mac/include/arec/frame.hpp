#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arec {

constexpr std::uint32_t kMaxFrameBytes = 4U * 1024U * 1024U;

enum class FrameError {
  kNone,
  kZeroLength,
  kTooLarge,
  kIncomplete,
};

class FrameDecoder {
 public:
  explicit FrameDecoder(std::uint32_t max_bytes = kMaxFrameBytes);

  bool Feed(const void* data, std::size_t size, FrameError* error = nullptr);
  bool Next(std::string* payload, FrameError* error = nullptr);
  void Reset();
  std::size_t buffered_bytes() const { return buffer_.size(); }

 private:
  std::uint32_t max_bytes_;
  std::size_t offset_ = 0;
  std::vector<std::uint8_t> buffer_;
  bool failed_ = false;
  FrameError error_ = FrameError::kNone;
};

std::vector<std::uint8_t> EncodeFrame(const std::string& payload,
                                      std::uint32_t max_bytes = kMaxFrameBytes,
                                      FrameError* error = nullptr);

}  // namespace arec

