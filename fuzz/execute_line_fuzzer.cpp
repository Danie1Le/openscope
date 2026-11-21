#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "openscope/instrument.hpp"

// libFuzzer entry point: raw bytes into the facade's execute-line entry point, so the parser, the
// command tree and every handler are fuzzed together. Each input is split on '\n' into lines run
// in order on one fresh Instrument with two Sessions: the fuzzer reaches sequences of commands
// and paths that cross Sessions (one Session's *CLS or errors against the other's queue), and
// every input still reproduces on its own. The low bit of a line's first byte picks the Session
// (an empty line goes to the first). The line itself runs unchanged, so a leading space (even) or
// tab (odd) sends any command to either Session. After each line the sample clock advances by
// kSamplesPerLine, so an input that shortens the record length (`ACQ:POIN 8`) reaches captures
// and the waveform queries. A line for a Session whose last line `*OPC?` or `*WAI` still holds is
// skipped, as a transport sends a Session's next line only after the reply.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  constexpr std::size_t kSamplesPerLine = 64;
  openscope::Instrument instrument;
  const std::array sessions{instrument.open_session(), instrument.open_session()};
  std::array<bool, sessions.size()> held{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer passes bytes as uint8_t.
  std::string_view input(reinterpret_cast<const char*>(data), size);
  while (true) {
    const std::size_t end = std::min(input.find('\n'), input.size());
    const std::string_view line = input.substr(0, end);
    const std::size_t pick = line.empty() ? 0 : static_cast<unsigned char>(line.front()) & 1U;
    if (!held.at(pick)) {
      held.at(pick) = true;
      instrument.execute(sessions.at(pick), line, [&held, pick](const std::optional<std::string>&) {
        held.at(pick) = false;
      });
    }
    instrument.advance(kSamplesPerLine);
    if (end == input.size()) {
      return 0;
    }
    input.remove_prefix(end + 1);
  }
}
