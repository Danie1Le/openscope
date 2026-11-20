#include "openscope/instrument.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope {

using scpi::Arguments;

Instrument::Instrument() {
  commands_.add({.header = "*IDN?",
                 .parameters = {},
                 .handler = [](Session&, const Arguments&) -> std::optional<std::string> {
                   return "OpenScope,OS-4,000001," OPENSCOPE_VERSION;
                 }});
  // Resets instrument-wide settings only; error queues survive *RST.
  commands_.add({.header = "*RST",
                 .parameters = {},
                 .handler = [](Session&, const Arguments&) -> std::optional<std::string> {
                   return std::nullopt;
                 }});
  commands_.add({.header = "*CLS",
                 .parameters = {},
                 .handler = [](Session& caller, const Arguments&) -> std::optional<std::string> {
                   caller.clear_errors();
                   return std::nullopt;
                 }});
  commands_.add({.header = "SYSTem:ERRor[:NEXT]?",
                 .parameters = {},
                 .handler = [](Session& caller, const Arguments&) -> std::optional<std::string> {
                   const ScpiError error = caller.pop_error();
                   return std::format(R"({},"{}")", error.code, error.message);
                 }});
}

SessionId Instrument::open_session() {
  const SessionId id{next_session_id_++};
  sessions_.try_emplace(id);
  return id;
}

std::optional<std::string> Instrument::execute(SessionId session, std::string_view line) {
  Session& caller = sessions_.at(session);
  if (line.size() > kMaxLineLength) {
    caller.push_error(kInputBufferOverrun);
    return std::nullopt;
  }
  return commands_.execute(line, caller);
}

}  // namespace openscope
