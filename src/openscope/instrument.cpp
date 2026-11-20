#include "openscope/instrument.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope {
namespace {

std::string to_upper(std::string_view text) {
  std::string upper(text);
  std::ranges::transform(upper, upper.begin(),
                         [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return upper;
}

}  // namespace

SessionId Instrument::open_session() {
  const SessionId id{next_session_id_++};
  sessions_.try_emplace(id);
  return id;
}

std::optional<std::string> Instrument::execute(SessionId session, std::string_view line) {
  Session& caller = sessions_.at(session);

  // shortcut: whole-line, short-form-only matching; #3 replaces it with the full SCPI grammar.
  const std::string header = to_upper(line);
  if (header.empty()) {
    return std::nullopt;  // An empty program message is valid SCPI and does nothing.
  }
  if (header == "*IDN?") {
    return "OpenScope,OS-4,000001," OPENSCOPE_VERSION;
  }
  if (header == "*RST") {
    return std::nullopt;  // Resets instrument-wide settings only; error queues survive *RST.
  }
  if (header == "*CLS") {
    caller.clear_errors();
    return std::nullopt;
  }
  if (header == "SYST:ERR?") {
    const ScpiError error = caller.pop_error();
    return std::format(R"({},"{}")", error.code, error.message);
  }
  caller.push_error(kUndefinedHeader);
  return std::nullopt;
}

}  // namespace openscope
