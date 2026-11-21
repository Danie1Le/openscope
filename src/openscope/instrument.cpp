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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/fault.hpp"
#include "openscope/fft.hpp"
#include "openscope/fft_window.hpp"
#include "openscope/generator.hpp"
#include "openscope/measurement.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi/response.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"
#include "openscope/trigger.hpp"

namespace openscope {
namespace {

using scpi::Arguments;
using scpi::NumericParameter;

/// The `SOURce<n>:FUNCtion` keywords, in GeneratorFunction order.
constexpr std::array<std::string_view, 6> kFunctionKeywords{"SINusoid", "SQUare", "TRIangle",
                                                            "RAMP",     "DC",     "NOISe"};
static_assert(kFunctionKeywords.size() == static_cast<std::size_t>(GeneratorFunction::kNoise) + 1);

/// The `SOURce<n>:FAULt` keywords, in FaultKind order.
constexpr std::array<std::string_view, 3> kFaultKeywords{"GLITch", "DROPout", "FSTep"};
static_assert(kFaultKeywords.size() == static_cast<std::size_t>(FaultKind::kFrequencyStep) + 1);

/// The widest magnitude any kind of Fault accepts. The command tree rejects past it; the handler
/// then applies the limit of the kind that was named.
double largest_fault_magnitude() {
  double largest = 0.0;
  for (std::size_t kind = 0; kind < kFaultKeywords.size(); ++kind) {
    largest = std::max(largest, fault_limits(static_cast<FaultKind>(kind)).max_magnitude);
  }
  return largest;
}

/// The `WAVeform:FORMat` keywords, in WaveformFormat order.
constexpr std::array<std::string_view, 2> kFormatKeywords{"ASCii", "REAL"};
static_assert(kFormatKeywords.size() == static_cast<std::size_t>(WaveformFormat::kReal) + 1);

/// The `FFT:WINDow` keywords, in FftWindow order.
constexpr std::array<std::string_view, 3> kWindowKeywords{"RECTangular", "HANNing", "BHARris"};
static_assert(kWindowKeywords.size() == static_cast<std::size_t>(FftWindow::kBlackmanHarris) + 1);

/// The `TRIGger:SLOPe` keywords, in TriggerSlope order.
constexpr std::array<std::string_view, 2> kSlopeKeywords{"POSitive", "NEGative"};
static_assert(kSlopeKeywords.size() == static_cast<std::size_t>(TriggerSlope::kNegative) + 1);

/// The `TRIGger:MODE` keywords, in TriggerMode order. There is no `SINGle`: Single is run control.
constexpr std::array<std::string_view, 2> kModeKeywords{"AUTO", "NORMal"};
static_assert(kModeKeywords.size() == static_cast<std::size_t>(TriggerMode::kNormal) + 1);

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

/// One fresh Generator per channel, each with its own seed derived from the instrument's, so the
/// four channels never share noise.
std::array<Generator, kChannelCount> make_generators(std::uint64_t seed, double sample_rate) {
  return [&]<std::size_t... N>(std::index_sequence<N...> /*channels*/) {
    return std::array<Generator, kChannelCount>{
        Generator{(seed * kChannelCount) + N, sample_rate}...};
  }(std::make_index_sequence<kChannelCount>{});
}

/// The channel, and so the Generator, that a `CHANnel<n>` or `SOURce<n>` header names, counted
/// from 0.
std::size_t channel_of(const Arguments& arguments) { return arguments.suffixes.at(0) - 1; }

/// Matches the held lines of `session`.
auto of_session(SessionId session) {
  return [session](const auto& held) { return held.session == session; };
}

/// The value of a command's parameter at `index`, the only one by default.
template <typename Value>
Value value_of(const Arguments& arguments, std::size_t index = 0) {
  return std::get<Value>(arguments.values.at(index));
}

/// A parameter naming a channel: `CHANnel<1-4>`.
scpi::KeywordParameter channel_parameter() { return {.choices = {"CHANnel<1-4>"}}; }

/// The error queue entry that reports a measurement's `status`, or nullopt for OK.
std::optional<ScpiError> status_entry(MeasurementStatus status) {
  switch (status) {
    case MeasurementStatus::kOk:
      break;
    case MeasurementStatus::kNoSignal:
      return kNoSignal;
    case MeasurementStatus::kClipped:
      return kClipped;
    case MeasurementStatus::kTooFewCycles:
      return kTooFewCycles;
  }
  return std::nullopt;
}

/// A count of samples sent as a number: a fraction rounds to the nearest whole number, half away
/// from zero.
std::size_t whole_samples(double count) { return static_cast<std::size_t>(std::llround(count)); }

}  // namespace

Instrument::Instrument(std::uint64_t seed)
    : seed_(seed),
      channels_(default_channels()),
      generators_(make_generators(seed, acquisition_.sample_rate)) {
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
  add_run_control_commands();
  add_generator_commands();
  add_channel_commands();
  add_acquisition_commands();
  add_timebase_commands();
  add_trigger_commands();
  add_waveform_commands();
  add_fft_commands();
  add_measurement_commands();
}

void Instrument::add_run_control_commands() {
  const auto enters = [this](RunState state) {
    return [this, state](Session&, const Arguments&) -> scpi::HandlerResult {
      enter(state);
      return std::nullopt;
    };
  };
  commands_.add({.header = "RUN", .parameters = {}, .handler = enters(RunState::kRunning)});
  commands_.add({.header = "STOP", .parameters = {}, .handler = enters(RunState::kStopped)});
  commands_.add({.header = "SINGle", .parameters = {}, .handler = enters(RunState::kArmedSingle)});
  // While a Single is armed, the command holds its line and replies once resume_held resumes it.
  const auto waits = [this](std::optional<std::string> reply) {
    return [this, reply = std::move(reply)](Session&, const Arguments&) -> scpi::HandlerResult {
      if (pending()) {
        return scpi::Hold{.reply = reply};
      }
      return reply;
    };
  };
  commands_.add({.header = "*OPC?", .parameters = {}, .handler = waits("1")});
  commands_.add({.header = "*WAI", .parameters = {}, .handler = waits(std::nullopt)});
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
  add_generator_number("SOURce<1-4>:AMPLitude", {.min = 0.0, .max = 100.0, .unit = "V"},
                       &GeneratorSettings::amplitude);
  add_generator_number("SOURce<1-4>:OFFSet", {.min = -50.0, .max = 50.0, .unit = "V"},
                       &GeneratorSettings::offset);
  add_generator_number("SOURce<1-4>:NOISe", {.min = 0.0, .max = 50.0, .unit = "V"},
                       &GeneratorSettings::noise);
  // An action, not a setting: it has no query. The magnitude's unit depends on the kind of fault
  // (volts or hertz), so the parameter takes none; each kind's own limit is checked in the handler.
  const double widest = largest_fault_magnitude();
  commands_.add(
      {.header = "SOURce<1-4>:FAULt",
       .parameters = {scpi::KeywordParameter{
                          .choices = {kFaultKeywords.begin(), kFaultKeywords.end()}},
                      NumericParameter{.min = -widest, .max = widest, .unit = ""},
                      NumericParameter{
                          .min = 1.0, .max = static_cast<double>(kMaxFaultDuration), .unit = ""}},
       .optional_parameters = 2,
       .handler = [this](Session&, const Arguments& arguments) {
         return inject_fault(arguments);
       }});
}

scpi::HandlerResult Instrument::inject_fault(const Arguments& arguments) {
  const auto kind = static_cast<FaultKind>(value_of<scpi::Choice>(arguments).index);
  const FaultLimits limits = fault_limits(kind);
  const std::size_t sent = arguments.values.size();
  const double magnitude = sent > 1 ? value_of<double>(arguments, 1) : limits.default_magnitude;
  if (std::abs(magnitude) > limits.max_magnitude) {
    return kDataOutOfRange;
  }
  const std::size_t duration =
      sent > 2 ? whole_samples(value_of<double>(arguments, 2)) : limits.default_duration;
  // From the next sample index on, so the fault lands in the next sample block (ADR 0003).
  const Fault fault{kind, magnitude, duration, clock_.now(), acquisition_.sample_rate};
  if (!generators_.at(channel_of(arguments)).inject(fault)) {
    return kSettingsConflict;
  }
  return std::nullopt;
}

void Instrument::add_channel_commands() {
  add_setting(
      "CHANnel<1-4>:DISPlay", scpi::BooleanParameter{},
      [this](Session&, const Arguments& arguments) {
        change_channel(arguments, [&](ChannelSettings& settings) {
          settings.enabled = value_of<bool>(arguments);
        });
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
        change_acquisition([&](AcquisitionSettings& settings) {
          settings.sample_rate = value_of<double>(arguments);
        });
      },
      [this](Session&, const Arguments&) { return scpi::format_real(acquisition_.sample_rate); });
  add_setting(
      "ACQuire:POINts",
      NumericParameter{.min = 1.0,
                       .max = static_cast<double>(AcquisitionSettings::kMaxRecordLength),
                       .unit = ""},
      [this](Session&, const Arguments& arguments) {
        change_acquisition([&](AcquisitionSettings& settings) {
          settings.record_length = whole_samples(value_of<double>(arguments));
        });
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

void Instrument::add_trigger_commands() {
  add_channel_choice(
      "TRIGger:SOURce",
      [this](Session&, std::size_t channel) {
        change_trigger([channel](TriggerSettings& settings) { settings.source = channel; });
      },
      [this](Session&) { return trigger_.source; });
  add_trigger_number("TRIGger:LEVel", {.min = -100.0, .max = 100.0, .unit = "V"},
                     &TriggerSettings::level);
  add_trigger_keyword("TRIGger:SLOPe", kSlopeKeywords, &TriggerSettings::slope);
  add_trigger_keyword("TRIGger:MODE", kModeKeywords, &TriggerSettings::mode);
  add_trigger_number("TRIGger:HOLDoff", {.min = 0.0, .max = 10.0, .unit = "S"},
                     &TriggerSettings::holdoff);
  // A percentage of the record, so it takes no unit (like ACQ:POIN).
  add_trigger_number("TRIGger:POSition", {.min = 0.0, .max = 100.0, .unit = ""},
                     &TriggerSettings::position);
}

void Instrument::add_waveform_commands() {
  // ADR 0002: the source and format belong to the calling Session.
  add_channel_choice(
      "WAVeform:SOURce",
      [](Session& caller, std::size_t channel) { caller.waveform().source = channel; },
      [](Session& caller) { return caller.waveform().source; });
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
                       caller.waveform().source,
                       [&caller](const Capture&, const CapturedChannel& channel) {
                         return caller.waveform().format == WaveformFormat::kReal
                                    ? scpi::float32_block(channel.samples)
                                    : ascii_values(channel.samples);
                       });
                 }});
  commands_.add(
      {.header = "WAVeform:PREamble?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(
             caller.waveform().source, [](const Capture& capture, const CapturedChannel& channel) {
               return std::format("{},{},{},{},{},{}", scpi::format_real(capture.sample_rate),
                                  capture.record_length, scpi::format_real(channel.settings.scale),
                                  scpi::format_real(channel.settings.offset),
                                  capture.trigger_position, capture.untriggered ? 1 : 0);
             });
       }});
}

void Instrument::add_fft_commands() {
  // Spec #1 and ADR 0002: the source and window belong to the calling Session.
  add_channel_choice(
      "FFT:SOURce", [](Session& caller, std::size_t channel) { caller.fft().source = channel; },
      [](Session& caller) { return caller.fft().source; });
  add_keyword_setting(
      "FFT:WINDow", kWindowKeywords,
      [](Session& caller, const Arguments&, std::size_t keyword) {
        caller.fft().window = static_cast<FftWindow>(keyword);
      },
      [](Session& caller, const Arguments&) {
        return static_cast<std::size_t>(caller.fft().window);
      });
  commands_.add(
      {.header = "FFT:DATA?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(caller.fft().source, [&caller](const Capture&,
                                                                 const CapturedChannel& channel) {
           return scpi::float32_block(magnitude_spectrum_db(channel.samples, caller.fft().window));
         });
       }});
  commands_.add(
      {.header = "FFT:SPACing?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(caller.fft().source, [](const Capture& capture,
                                                          const CapturedChannel& channel) {
           // N from the samples FFT:DATA? transforms, so the two replies always agree.
           return scpi::format_real(capture.sample_rate /
                                    static_cast<double>(spectrum_length(channel.samples.size())));
         });
       }});
}

void Instrument::add_measurement_commands() {
  const std::span<const Measurement> all = measurements();
  for (std::size_t measurement = 0; measurement < all.size(); ++measurement) {
    commands_.add({.header = std::format("MEASure:{}?", all[measurement].mnemonic),
                   .parameters = {channel_parameter()},
                   .handler = [this, measurement](Session& caller, const Arguments& arguments) {
                     const std::size_t channel = value_of<scpi::Choice>(arguments).suffix;
                     return reply_with_source(channel, [&](const Capture&, const CapturedChannel&) {
                       // The engine keys its cache on the shared capture, which is the one
                       // reply_with_source just checked: nothing advances the sample clock in
                       // between.
                       const MeasurementResult result =
                           measurements_.measure(captures_.latest(), channel, measurement);
                       // The status entry reports on a reply that was given, so unlike an execution
                       // error it goes on the queue without stopping the line.
                       if (std::optional<ScpiError> entry = status_entry(result.status)) {
                         caller.push_error(std::move(*entry));
                       }
                       // Written as float32, the precision of the samples the value comes from.
                       return result.value ? scpi::format_real(static_cast<float>(*result.value))
                                           : std::string(scpi::kNotANumber);
                     });
                   }});
  }
}

scpi::HandlerResult Instrument::reply_with_source(
    std::size_t source,
    const std::function<std::string(const Capture&, const CapturedChannel&)>& reply) const {
  const std::shared_ptr<const Capture> capture = captures_.latest();
  if (capture == nullptr) {
    return kDataCorruptOrStale;
  }
  const std::optional<CapturedChannel>& channel = capture->channels.at(source - 1);
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
        const std::size_t index = get(caller, arguments);
        if (index >= keywords.size()) {
          throw std::out_of_range("setting has no keyword");  // An enum value without a keyword.
        }
        return std::string(scpi::short_form(keywords[index]));
      });
}

template <typename Settings, typename Change, typename Lookup>
void Instrument::add_number_setting(std::string_view header, const NumericParameter& range,
                                    double Settings::*field, Change change, Lookup lookup) {
  add_setting(
      header, range,
      [field, change = std::move(change)](Session&, const Arguments& arguments) {
        change(arguments,
               [&](Settings& settings) { settings.*field = value_of<double>(arguments); });
      },
      [field, lookup = std::move(lookup)](Session&, const Arguments& arguments) {
        return scpi::format_real(lookup(arguments).*field);
      });
}

void Instrument::add_generator_number(std::string_view header, const NumericParameter& range,
                                      double GeneratorSettings::*field) {
  add_number_setting(
      header, range, field,
      [this](const Arguments& arguments, const std::function<void(GeneratorSettings&)>& change) {
        change_generator(arguments, change);
      },
      [this](const Arguments& arguments) -> const GeneratorSettings& {
        return generators_.at(channel_of(arguments)).settings();
      });
}

void Instrument::add_channel_number(std::string_view header, const NumericParameter& range,
                                    double ChannelSettings::*field) {
  add_number_setting(
      header, range, field,
      [this](const Arguments& arguments, const std::function<void(ChannelSettings&)>& change) {
        change_channel(arguments, change);
      },
      [this](const Arguments& arguments) -> const ChannelSettings& {
        return channels_.at(channel_of(arguments));
      });
}

void Instrument::add_channel_choice(std::string_view header,
                                    std::function<void(Session&, std::size_t)> set,
                                    std::function<std::size_t(Session&)> get) {
  add_setting(
      header, channel_parameter(),
      [set = std::move(set)](Session& caller, const Arguments& arguments) {
        set(caller, value_of<scpi::Choice>(arguments).suffix);
      },
      [get = std::move(get)](Session& caller, const Arguments&) {
        return std::format("CHAN{}", get(caller));
      });
}

void Instrument::add_trigger_number(std::string_view header, const NumericParameter& range,
                                    double TriggerSettings::*field) {
  add_number_setting(
      header, range, field,
      [this](const Arguments&, const std::function<void(TriggerSettings&)>& change) {
        change_trigger(change);
      },
      [this](const Arguments&) -> const TriggerSettings& { return trigger_; });
}

template <typename Keyword>
void Instrument::add_trigger_keyword(std::string_view header,
                                     std::span<const std::string_view> keywords,
                                     Keyword TriggerSettings::*field) {
  add_keyword_setting(
      header, keywords,
      [this, field](Session&, const Arguments&, std::size_t keyword) {
        change_trigger(
            [&](TriggerSettings& settings) { settings.*field = static_cast<Keyword>(keyword); });
      },
      [this, field](Session&, const Arguments&) {
        return static_cast<std::size_t>(trigger_.*field);
      });
}

void Instrument::change_generator(const Arguments& arguments,
                                  const std::function<void(GeneratorSettings&)>& change) {
  Generator& generator = generators_.at(channel_of(arguments));
  GeneratorSettings settings = generator.settings();
  change(settings);
  generator.configure(settings, acquisition_.sample_rate, clock_.now());
  captures_.discard();
}

void Instrument::change_channel(const Arguments& arguments,
                                const std::function<void(ChannelSettings&)>& change) {
  change(channels_.at(channel_of(arguments)));
  captures_.discard();
}

void Instrument::change_acquisition(const std::function<void(AcquisitionSettings&)>& change) {
  change(acquisition_);
  for (Generator& generator : generators_) {
    generator.configure(generator.settings(), acquisition_.sample_rate, clock_.now());
  }
  captures_.discard();
}

void Instrument::change_trigger(const std::function<void(TriggerSettings&)>& change) {
  change(trigger_);
  captures_.discard();
}

void Instrument::reset() {
  acquisition_ = {};
  channels_ = default_channels();
  timebase_scale_ = kDefaultTimebaseScale;
  trigger_ = {};
  // Fresh Generators rather than reconfigured ones: phase continuity would carry the phase built
  // up by earlier settings past *RST, so the output would depend on the commands sent before it.
  generators_ = make_generators(seed_, acquisition_.sample_rate);
  captures_ = {};
  run_state_ = RunState::kRunning;
}

void Instrument::enter(RunState state) {
  run_state_ = state;
  if (state != RunState::kRunning) {
    captures_.discard();
  }
}

void Instrument::resume_held() {
  // Every line held when the operation ended carries on, even after an earlier one arms another
  // Single: what each waited for is over. A line that waits again joins the next round, which
  // runs at once if a resumed line has already ended that Single too (STOP).
  while (!held_.empty() && !pending()) {
    for (HeldLine& line : std::exchange(held_, {})) {
      commands_.resume(sessions_.at(line.session), line.execution);
      if (line.execution.held()) {
        held_.push_back(std::move(line));
      } else {
        line.on_reply(line.execution.take_reply());
      }
    }
  }
}

SessionId Instrument::open_session() {
  const SessionId id{next_session_id_++};
  sessions_.try_emplace(id);
  return id;
}

void Instrument::close_session(SessionId session) {
  if (sessions_.erase(session) == 0) {
    throw std::out_of_range("unknown Session");
  }
  std::erase_if(held_, of_session(session));
}

void Instrument::execute(SessionId session, std::string_view line, ReplyHandler on_reply) {
  Session& caller = sessions_.at(session);
  if (std::ranges::any_of(held_, of_session(session))) {
    throw std::logic_error("Session sent a line while its previous line is held");
  }
  if (line.size() > kMaxLineLength) {
    caller.push_error(kInputBufferOverrun);
    on_reply(std::nullopt);
    return;
  }
  scpi::Execution execution = commands_.execute(caller, line);
  if (execution.held()) {
    held_.push_back(
        {.session = session, .execution = std::move(execution), .on_reply = std::move(on_reply)});
  } else {
    on_reply(execution.take_reply());
  }
  resume_held();  // This line may have ended the operation (STOP, RUN, *RST).
}

void Instrument::advance(std::size_t samples) {
  while (samples > 0) {
    std::size_t count = std::min(samples, kMaxBlockLength);
    std::optional<std::size_t> single_end;
    // Stopped samples nothing: Generators are pure functions of the sample index (ADR 0003), and
    // the capture in progress was dropped when the instrument stopped.
    if (run_state_ != RunState::kStopped) {
      const SampleBlock block =
          sample_channels(generators_, channels_, trigger_.source, clock_.now(), count);
      single_end = captures_.push(block, acquisition_, channels_, trigger_,
                                  pending() ? CaptureLimit::kFirst : CaptureLimit::kEvery);
      // The Single's capture ends the operation at its last sample: the lines it resumes run with
      // the clock there, and the next block starts there under whatever they changed, so how
      // `advance` is chunked never changes a capture or a reply (ADR 0003).
      if (single_end) {
        count = *single_end;
      }
    }
    clock_.take(count);
    samples -= count;
    if (single_end) {
      enter(RunState::kStopped);
      resume_held();
    }
  }
}

}  // namespace openscope
