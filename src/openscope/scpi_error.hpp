#pragma once

#include <string_view>

namespace openscope {

/// One entry in a Session's error queue: a SCPI error code and its message, as `SYST:ERR?`
/// reports them.
struct ScpiError {
  int code;
  std::string_view message;
};

/// What `SYST:ERR?` reports when the error queue is empty.
inline constexpr ScpiError kNoError{.code = 0, .message = "No error"};

/// A command header that matches no command.
inline constexpr ScpiError kUndefinedHeader{.code = -113, .message = "Undefined header"};

}  // namespace openscope
