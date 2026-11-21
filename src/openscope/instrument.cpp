#include "openscope/instrument.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi/response.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope {
namespace {

using scpi::Arguments;
using scpi::NumericParameter;

/// The `SOURce<n>:FUNCtion` keywords, in GeneratorFunction order.
constexpr std::array<std::string_view, 6> kFunctionKeywords{"SINusoid", "SQUare", "TRIangle",
                                                            "RAMP",     "DC",     "NOISe"};
static_assert(kFunctionKeywords.size() == static_cast<std::size_t>(GeneratorFunction::kNoise) + 1);

/// The `WAVeform:FORMat` keywords, in WaveformFormat order.
constexpr std::array<std::string_view, 2> kFormatKeywords{"ASCii", "REAL"};
static_assert(kFormatKeywords.size() == static_cast<std::size_t>(WaveformFormat::kReal) + 1);

/// Samples as comma-separated values, each in the shortest form that reads back as the same float.
std::string ascii_values(std::span<const float> samples) {
  std::string text;
  text.reserve(samples.size() * 16);  // A typical value plus its comma.
  for (const float sample : samples) {
    if (!text.empty()) {
      text += ',';
    }
    text += scpi::format_real(sample);
  }
  return text;
}

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

/// The channel, and so the Generator, that a `CHANnel<n>` or `SOURce<n>` header names, counted
/// from 0.
std::size_t channel_of(const Arguments& arguments) { return arguments.suffixes.at(0) - 1; }

/// The value of a command's only parameter.
template <typename Value>
Value value_of(const Arguments& arguments) {
  return std::get<Value>(arguments.values.at(0));
}

}  // namespace

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
                 .handler = [this](Session&, const Arguments&) -> std::optional<std::string> {
                   reset();
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
  add_generator_commands();
  add_channel_commands();
  add_acquisition_commands();
  add_timebase_commands();
  add_waveform_commands();
}

void Instrument::add_generator_commands() {
  add_keyword_setting(
      "SOURce<1-4>:FUNCtion", kFunctionKeywords,
      [this](Session&, const Arguments& arguments, std::size_t keyword) {
        change_generator(arguments, [keyword](GeneratorSettings& settings) {
          settings.function = static_cast<GeneratorFunction>(keyword);
        });
      },
      [this](Session&, const Arguments& arguments) {
        return static_cast<std::size_t>(generators_.at(channel_of(arguments)).settings().function);
      });
  add_generator_number("SOURce<1-4>:FREQuency", {.min = 1e-3, .max = 1e9, .unit = "HZ"},
                       &GeneratorSettings::frequency);
  add_generator_number("SOURce<1-4>:AMPLitude", {.min = 0.0, .max = 50.0, .unit = "V"},
                       &GeneratorSettings::amplitude);
  add_generator_number("SOURce<1-4>:OFFSet", {.min = -50.0, .max = 50.0, .unit = "V"},
                       &GeneratorSettings::offset);
  add_generator_number("SOURce<1-4>:NOISe", {.min = 0.0, .max = 50.0, .unit = "V"},
                       &GeneratorSettings::noise);
}

void Instrument::add_channel_commands() {
  add_setting(
      "CHANnel<1-4>:DISPlay", scpi::BooleanParameter{},
      [this](Session&, const Arguments& arguments) {
        channel_to_change(arguments).enabled = value_of<bool>(arguments);
      },
      [this](Session&, const Arguments& arguments) {
        return std::string(channels_.at(channel_of(arguments)).enabled ? "1" : "0");
      });
  add_channel_number("CHANnel<1-4>:SCALe", {.min = 1e-3, .max = 10.0, .unit = "V"},
                     &ChannelSettings::scale);
  add_channel_number("CHANnel<1-4>:OFFSet", {.min = -50.0, .max = 50.0, .unit = "V"},
                     &ChannelSettings::offset);
}

void Instrument::add_acquisition_commands() {
  add_setting(
      "ACQuire:SRATe", NumericParameter{.min = 1.0, .max = 1e9, .unit = ""},
      [this](Session&, const Arguments& arguments) {
        AcquisitionSettings settings = acquisition_;
        settings.sample_rate = value_of<double>(arguments);
        set_acquisition(settings);
      },
      [this](Session&, const Arguments&) { return scpi::format_real(acquisition_.sample_rate); });
  add_setting(
      "ACQuire:POINts",
      NumericParameter{.min = 1.0,
                       .max = static_cast<double>(AcquisitionSettings::kMaxRecordLength),
                       .unit = ""},
      [this](Session&, const Arguments& arguments) {
        AcquisitionSettings settings = acquisition_;
        // A count of samples: a fraction rounds to the nearest whole number, half away from zero.
        settings.record_length =
            static_cast<std::size_t>(std::llround(value_of<double>(arguments)));
        set_acquisition(settings);
      },
      [this](Session&, const Arguments&) { return std::to_string(acquisition_.record_length); });
}

void Instrument::add_timebase_commands() {
  // ADR 0004: the timebase only zooms the display, so it neither discards nor reshapes captures.
  add_setting(
      "TIMebase:SCALe", NumericParameter{.min = 1e-9, .max = 100.0, .unit = "S"},
      [this](Session&, const Arguments& arguments) {
        timebase_scale_ = value_of<double>(arguments);
      },
      [this](Session&, const Arguments&) { return scpi::format_real(timebase_scale_); });
}

void Instrument::add_waveform_commands() {
  // ADR 0002: the source and format belong to the calling Session.
  add_setting(
      "WAVeform:SOURce", scpi::KeywordParameter{.choices = {"CHANnel<1-4>"}},
      [](Session& caller, const Arguments& arguments) {
        caller.waveform().source = value_of<scpi::Choice>(arguments).suffix;
      },
      [](Session& caller, const Arguments&) {
        return std::format("CHAN{}", caller.waveform().source);
      });
  add_keyword_setting(
      "WAVeform:FORMat", kFormatKeywords,
      [](Session& caller, const Arguments&, std::size_t keyword) {
        caller.waveform().format = static_cast<WaveformFormat>(keyword);
      },
      [](Session& caller, const Arguments&) {
        return static_cast<std::size_t>(caller.waveform().format);
      });
  commands_.add({.header = "WAVeform:DATA?",
                 .parameters = {},
                 .handler = [this](Session& caller, const Arguments&) {
                   return reply_with_source(
                       caller, [&caller](const Capture&, const CapturedChannel& channel) {
                         return caller.waveform().format == WaveformFormat::kReal
                                    ? scpi::float32_block(channel.samples)
                                    : ascii_values(channel.samples);
                       });
                 }});
  commands_.add({.header = "WAVeform:PREamble?",
                 .parameters = {},
                 .handler = [this](Session& caller, const Arguments&) {
                   return reply_with_source(caller, [](const Capture& capture,
                                                       const CapturedChannel& channel) {
                     return std::format("{},{},{},{},{},{}", scpi::format_real(capture.sample_rate),
                                        capture.record_length, scpi::format_real(channel.scale),
                                        scpi::format_real(channel.offset), capture.trigger_position,
                                        capture.untriggered ? 1 : 0);
                   });
                 }});
}

scpi::HandlerResult Instrument::reply_with_source(
    Session& caller,
    const std::function<std::string(const Capture&, const CapturedChannel&)>& reply) const {
  const std::shared_ptr<const Capture> capture = captures_.latest();
  if (capture == nullptr) {
    return kDataCorruptOrStale;
  }
  const std::optional<CapturedChannel>& channel =
      capture->channels.at(caller.waveform().source - 1);
  if (!channel) {
    return kSettingsConflict;
  }
  return std::optional<std::string>(reply(*capture, *channel));
}

void Instrument::add_setting(std::string_view header, scpi::ParameterSpec parameter,
                             std::function<void(Session&, const Arguments&)> set,
                             std::function<std::string(Session&, const Arguments&)> get) {
  commands_.add({.header = header,
                 .parameters = {std::move(parameter)},
                 .handler = [set = std::move(set)](Session& caller, const Arguments& arguments)
                     -> scpi::HandlerResult {
                   set(caller, arguments);
                   return std::nullopt;
                 }});
  const std::string query = std::string(header) + '?';
  commands_.add(
      {.header = query,
       .parameters = {},
       .handler = [get = std::move(get)](Session& caller, const Arguments& arguments)
           -> scpi::HandlerResult { return std::optional<std::string>(get(caller, arguments)); }});
}

void Instrument::add_keyword_setting(
    std::string_view header, std::span<const std::string_view> keywords,
    std::function<void(Session&, const Arguments&, std::size_t)> set,
    std::function<std::size_t(Session&, const Arguments&)> get) {
  add_setting(
      header, scpi::KeywordParameter{.choices = {keywords.begin(), keywords.end()}},
      [set = std::move(set)](Session& caller, const Arguments& arguments) {
        set(caller, arguments, value_of<scpi::Choice>(arguments).index);
      },
      [keywords, get = std::move(get)](Session& caller, const Arguments& arguments) {
        return std::string(scpi::short_form(keywords[get(caller, arguments)]));
      });
}

void Instrument::add_generator_number(std::string_view header, NumericParameter range,
                                      double GeneratorSettings::*field) {
  add_setting(
      header, std::move(range),
      [this, field](Session&, const Arguments& arguments) {
        change_generator(arguments, [&](GeneratorSettings& settings) {
          settings.*field = value_of<double>(arguments);
        });
      },
      [this, field](Session&, const Arguments& arguments) {
        return scpi::format_real(generators_.at(channel_of(arguments)).settings().*field);
      });
}

void Instrument::add_channel_number(std::string_view header, NumericParameter range,
                                    double ChannelSettings::*field) {
  add_setting(
      header, std::move(range),
      [this, field](Session&, const Arguments& arguments) {
        channel_to_change(arguments).*field = value_of<double>(arguments);
      },
      [this, field](Session&, const Arguments& arguments) {
        return scpi::format_real(channels_.at(channel_of(arguments)).*field);
      });
}

void Instrument::change_generator(const Arguments& arguments,
                                  const std::function<void(GeneratorSettings&)>& change) {
  Generator& generator = generators_.at(channel_of(arguments));
  GeneratorSettings settings = generator.settings();
  change(settings);
  generator.configure(settings, acquisition_.sample_rate, clock_.now());
}

ChannelSettings& Instrument::channel_to_change(const Arguments& arguments) {
  captures_.discard();
  return channels_.at(channel_of(arguments));
}

void Instrument::set_acquisition(const AcquisitionSettings& settings) {
  acquisition_ = settings;
  for (Generator& generator : generators_) {
    generator.configure(generator.settings(), settings.sample_rate, clock_.now());
  }
  captures_.discard();
}

void Instrument::reset() {
  acquisition_ = {};
  channels_ = default_channels();
  timebase_scale_ = kDefaultTimebaseScale;
  for (Generator& generator : generators_) {
    generator.configure({}, acquisition_.sample_rate, clock_.now());
  }
  captures_ = {};
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
