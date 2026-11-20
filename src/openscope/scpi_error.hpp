#pragma once

#include <string>

namespace openscope {

/// One entry in a Session's error queue: a SCPI error code and its message, as `SYST:ERR?`
/// reports them. The message is owned, so handlers may build it at run time.
struct ScpiError {
  int code;
  std::string message;
};

/// What `SYST:ERR?` reports when the error queue is empty.
inline const ScpiError kNoError{.code = 0, .message = "No error"};

/// A command header that matches no command.
inline const ScpiError kUndefinedHeader{.code = -113, .message = "Undefined header"};

}  // namespace openscope
