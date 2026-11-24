#include "openscope/capture.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>

namespace openscope {

const CapturedChannel& captured_channel(const Capture& capture, std::size_t channel) {
  const std::optional<CapturedChannel>& captured = capture.channels.at(channel - 1);
  if (!captured) {
    throw std::out_of_range("channel is not in the capture");
  }
  return *captured;
}

}  // namespace openscope
