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

/// A line that breaks the SCPI grammar. Nothing on the line runs.
inline const ScpiError kSyntaxError{.code = -102, .message = "Syntax error"};

/// A parameter of the wrong kind: a keyword where a number belongs, a number where a keyword
/// belongs, or a string, non-decimal number or block, which no command takes.
inline const ScpiError kDataTypeError{.code = -104, .message = "Data type error"};

/// More parameters than the command takes.
inline const ScpiError kParameterNotAllowed{.code = -108, .message = "Parameter not allowed"};

/// Fewer parameters than the command takes.
inline const ScpiError kMissingParameter{.code = -109, .message = "Missing parameter"};

/// A command header that matches no command, including a suffix on a node that takes none.
inline const ScpiError kUndefinedHeader{.code = -113, .message = "Undefined header"};

/// A header that matches a command except that a node's suffix is outside its range (`CHAN5`).
inline const ScpiError kHeaderSuffixOutOfRange{.code = -114,
                                               .message = "Header suffix out of range"};

/// A numeric parameter's suffix is not a unit, with an optional multiplier, the parameter accepts.
inline const ScpiError kInvalidSuffix{.code = -131, .message = "Invalid suffix"};

/// A numeric parameter outside the range the command accepts.
inline const ScpiError kDataOutOfRange{.code = -222, .message = "Data out of range"};

/// A keyword the parameter does not accept.
inline const ScpiError kIllegalParameterValue{.code = -224, .message = "Illegal parameter value"};

/// Recorded in place of the newest error when a Session's error queue is full.
inline const ScpiError kQueueOverflow{.code = -350, .message = "Queue overflow"};

/// A line longer than the instrument accepts. None of it runs.
inline const ScpiError kInputBufferOverrun{.code = -363, .message = "Input buffer overrun"};

}  // namespace openscope
