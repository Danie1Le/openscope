#include "openscope/instrument.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope {
namespace {

/// The `*RST` channel settings: CH1 enabled, the others disabled.
std::array<ChannelSettings, kChannelCount> default_channels() {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels.front().enabled = true;
  return channels;
}

/// One Generator per channel, each with its own seed derived from the instrument's, so the four
/// channels never share noise.
template <std::size_t... N>
std::array<Generator, kChannelCount> make_generators(std::uint64_t seed, double sample_rate,
                                                     std::index_sequence<N...> /*channels*/) {
  return {Generator{(seed * kChannelCount) + N, sample_rate}...};
}

}  // namespace

using scpi::Arguments;

Instrument::Instrument(std::uint64_t seed)
    : channels_(default_channels()),
      generators_(make_generators(seed, acquisition_.sample_rate,
                                  std::make_index_sequence<kChannelCount>{})) {
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
  return commands_.execute(caller, line);
}

void Instrument::advance(std::size_t samples) {
  while (samples > 0) {
    const std::size_t count = std::min(samples, kMaxBlockLength);
    const SampleBlock block = sample_channels(generators_, channels_, clock_.take(count), count);
    captures_.push(block, acquisition_, channels_);
    samples -= count;
  }
}

}  // namespace openscope
