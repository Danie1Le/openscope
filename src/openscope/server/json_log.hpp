#pragma once

#include <nlohmann/json.hpp>
#include <ostream>
#include <string_view>

namespace openscope {

/// A structured log: one JSON object per line, so a log collector or a test can parse each event
/// without guessing at free text (#13). Every line starts with `time` (UTC, ISO 8601 with
/// milliseconds) and `event`, followed by the fields the caller gives, in their order.
///
/// It does no locking: one thread writes to it at a time (the ScpiServer's I/O thread).
class JsonLog {
 public:
  /// Writes to `out`, which must outlive the log.
  explicit JsonLog(std::ostream& out) : out_(&out) {}

  /// Writes one line for `event` with `fields`, an object whose members follow `time` and `event`,
  /// and flushes it, so the line is out before anything that happens next. Text that is not valid
  /// UTF-8 is written with U+FFFD in place of the bad bytes rather than failing.
  void write(std::string_view event,
             const nlohmann::ordered_json& fields = nlohmann::ordered_json::object());

 private:
  std::ostream* out_;
};

}  // namespace openscope
