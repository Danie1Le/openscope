#pragma once

#include <chrono>
#include <functional>
#include <thread>

namespace openscope::test {

/// How long a test waits for something that runs on another thread (the live pipeline, the
/// server's I/O thread) before it fails: far longer than it takes, even under TSan on a loaded
/// machine.
inline constexpr std::chrono::seconds kPatience{20};

/// Polls `done` until it holds; false if it still does not after kPatience.
inline bool eventually(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + kPatience;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace openscope::test
