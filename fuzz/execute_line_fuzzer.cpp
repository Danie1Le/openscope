#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "openscope/instrument.hpp"

// libFuzzer entry point: raw bytes into the facade's execute-line entry point, so the parser, the
// command tree and every handler are fuzzed together. Each input is split on '\n' into lines run
// in order on one fresh Instrument: the fuzzer reaches sequences of commands, and every input
// still reproduces on its own.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  openscope::Instrument instrument;
  const openscope::SessionId session = instrument.open_session();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer passes bytes as uint8_t.
  std::string_view input(reinterpret_cast<const char*>(data), size);
  while (true) {
    const std::size_t end = std::min(input.find('\n'), input.size());
    instrument.execute(session, input.substr(0, end));
    if (end == input.size()) {
      return 0;
    }
    input.remove_prefix(end + 1);
  }
}
