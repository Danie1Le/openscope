// The SCPI command catalogue of Instrument: every command it registers and the helpers that
// register settings and apply their changes. How a command runs under the mutex, and how held
// lines are carried on, is in instrument.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
#include "openscope/instrument.hpp"
#include "openscope/measurement.hpp"
#include "openscope/measurement_statistics.hpp"
#include "openscope/running_statistics.hpp"
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
static_assert(kWindowKeywords.size() == kFftWindowCount);

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

/// The channel, and so the Generator, that a `CHANnel<n>` or `SOURce<n>` header names, counted
/// from 0.
std::size_t channel_of(const Arguments& arguments) { return arguments.suffixes.at(0) - 1; }

/// The value of a command's parameter at `index`, the only one by default.
template <typename Value>
Value value_of(const Arguments& arguments, std::size_t index = 0) {
  return std::get<Value>(arguments.values.at(index));
}

/// A parameter naming a channel: `CHANnel<1-4>`.
scpi::KeywordParameter channel_parameter() { return {.choices = {"CHANnel<1-4>"}}; }

/// Pushes the error queue entry that reports a measurement's `status` onto `caller`'s queue; OK
/// has none. The entry reports on a reply that was given, so unlike an execution error it does not
/// stop the line.
void report_status(Session& caller, MeasurementStatus status) {
  switch (status) {
    case MeasurementStatus::kOk:
      break;
    case MeasurementStatus::kNoSignal:
      caller.push_error(kNoSignal);
      break;
    case MeasurementStatus::kClipped:
      caller.push_error(kClipped);
      break;
    case MeasurementStatus::kTooFewCycles:
      caller.push_error(kTooFewCycles);
      break;
  }
}

/// A measured value as a query replies it: at float32 precision, the precision of the samples it
/// comes from, or SCPI's not-a-number when there is no value.
std::string value_reply(std::optional<double> value) {
  return value ? scpi::format_real(static_cast<float>(*value)) : std::string(scpi::kNotANumber);
}

/// A count of samples sent as a number: a fraction rounds to the nearest whole number, half away
/// from zero.
std::size_t whole_samples(double count) { return static_cast<std::size_t>(std::llround(count)); }

}  // namespace

void Instrument::add_system_commands() {
  add_lock_free_command({.header = "*IDN?",
                         .parameters = {},
                         .handler = [](Session&, const Arguments&) -> std::optional<std::string> {
                           return "OpenScope,OS-4,000001," OPENSCOPE_VERSION;
                         }});
  // Resets instrument-wide settings only; error queues survive *RST.
  add_command({.header = "*RST",
               .parameters = {},
               .handler = [this](Session&, const Arguments&) -> std::optional<std::string> {
                 reset();
                 return std::nullopt;
               }});
  add_lock_free_command(
      {.header = "*CLS",
       .parameters = {},
       .handler = [](Session& caller, const Arguments&) -> std::optional<std::string> {
         caller.clear_errors();
         return std::nullopt;
       }});
  add_lock_free_command(
      {.header = "SYSTem:ERRor[:NEXT]?",
       .parameters = {},
       .handler = [](Session& caller, const Arguments&) -> std::optional<std::string> {
         return error_reply(caller.pop_error());
       }});
}

void Instrument::add_run_control_commands() {
  const auto enters = [this](RunState state) {
    return [this, state](Session&, const Arguments&) -> scpi::HandlerResult {
      enter(state);
      return std::nullopt;
    };
  };
  add_command({.header = "RUN", .parameters = {}, .handler = enters(RunState::kRunning)});
  add_command({.header = "STOP", .parameters = {}, .handler = enters(RunState::kStopped)});
  add_command({.header = "SINGle", .parameters = {}, .handler = enters(RunState::kArmedSingle)});
  // While a Single is armed, the command holds its line and replies once the line is resumed.
  const auto waits = [this](std::optional<std::string> reply) {
    return [this, reply = std::move(reply)](Session&, const Arguments&) -> scpi::HandlerResult {
      if (pending()) {
        return scpi::Hold{.reply = reply};
      }
      return reply;
    };
  };
  add_command({.header = "*OPC?", .parameters = {}, .handler = waits("1")});
  add_command({.header = "*WAI", .parameters = {}, .handler = waits(std::nullopt)});
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
  add_command(
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
  // ADR 0004: the timebase only zooms the display, so it neither discards nor reshapes captures,
  // and the statistics over them stay.
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
  add_lock_free_command(
      {.header = "WAVeform:DATA?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(
             caller.waveform().source,
             [&caller](const std::shared_ptr<const Capture>&, const CapturedChannel& channel) {
               return caller.waveform().format == WaveformFormat::kReal
                          ? scpi::float32_block(channel.samples)
                          : ascii_values(channel.samples);
             });
       }});
  add_lock_free_command(
      {.header = "WAVeform:PREamble?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(
             caller.waveform().source,
             [](const std::shared_ptr<const Capture>& capture, const CapturedChannel& channel) {
               return std::format("{},{},{},{},{},{}", scpi::format_real(capture->sample_rate),
                                  capture->record_length, scpi::format_real(channel.settings.scale),
                                  scpi::format_real(channel.settings.offset),
                                  capture->time_zero_sample, capture->untriggered ? 1 : 0);
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
  add_lock_free_command(
      {.header = "FFT:DATA?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         const std::size_t source = caller.fft().source;
         return reply_with_source(
             source, [&](const std::shared_ptr<const Capture>& capture, const CapturedChannel&) {
               return scpi::float32_block(*spectra_.spectrum(capture, source, caller.fft().window));
             });
       }});
  add_lock_free_command(
      {.header = "FFT:SPACing?",
       .parameters = {},
       .handler = [this](Session& caller, const Arguments&) {
         return reply_with_source(
             caller.fft().source,
             [](const std::shared_ptr<const Capture>& capture, const CapturedChannel& channel) {
               // N from the samples FFT:DATA? transforms, so the two replies always
               // agree.
               return scpi::format_real(capture->sample_rate / static_cast<double>(spectrum_length(
                                                                   channel.samples.size())));
             });
       }});
}

void Instrument::add_measurement_commands() {
  const std::span<const Measurement> all = measurements();
  for (std::size_t measurement = 0; measurement < all.size(); ++measurement) {
    add_lock_free_command(
        {.header = std::format("MEASure:{}?", all[measurement].mnemonic),
         .parameters = {channel_parameter()},
         .handler = [this, measurement](Session& caller, const Arguments& arguments) {
           const std::size_t channel = value_of<scpi::Choice>(arguments).suffix;
           return reply_with_source(channel, [&](const std::shared_ptr<const Capture>& capture,
                                                 const CapturedChannel&) {
             const MeasurementResult result = measurements_.measure(capture, channel, measurement);
             report_status(caller, result.status);
             return value_reply(result.value);
           });
         }});
  }
  add_statistics_commands();
}

void Instrument::add_statistics_commands() {
  std::vector<std::string_view> mnemonics;
  for (const Measurement& measurement : measurements()) {
    mnemonics.push_back(measurement.mnemonic);
  }
  add_command(
      {.header = "MEASure:STATistics?",
       .parameters = {scpi::KeywordParameter{.choices = std::move(mnemonics)}, channel_parameter()},
       .handler = [this](Session& caller, const Arguments& arguments) -> scpi::HandlerResult {
         // Captures already published but still being measured would be left out (#71): the
         // query waits, holding its line, and runs again once those are counted
         // (count_measured). Captures published after it began to wait do not hold it up.
         if (SessionState* state = state_of(caller)) {
           state->counted_by = state->counted_by.value_or(handed_off_);
           if (waits_for_count(*state)) {
             return scpi::Hold{.reply = std::nullopt, .again = true};
           }
           state->counted_by.reset();
         }
         if (statistics_.empty()) {
           return kDataCorruptOrStale;  // No capture since the statistics were last reset.
         }
         const std::size_t measurement = value_of<scpi::Choice>(arguments, 0).index;
         const std::size_t channel = value_of<scpi::Choice>(arguments, 1).suffix;
         // Every capture published before this query is counted by now, so `current` is the
         // newest counted capture's result. While Running it can be older than the latest
         // capture: one completed while the query waited may not be counted yet. The latest
         // capture still has the channels of every capture they hold (a channel change resets
         // them).
         return reply_with_source(
             channel, [&](const std::shared_ptr<const Capture>&, const CapturedChannel&) {
               const MeasurementSeries& series = statistics_.at(channel, measurement);
               report_status(caller, series.current.status);
               // A Clipped value counted earlier makes the statistics questionable too.
               if (series.clipped && series.current.status != MeasurementStatus::kClipped) {
                 caller.push_error(kClipped);
               }
               const RunningStatistics& values = series.values;
               // With nothing counted, the statistics have no value.
               const auto statistic = [counted = values.count() > 0](double value) {
                 return value_reply(counted ? std::optional(value) : std::nullopt);
               };
               return std::format("{},{},{},{},{},{}", value_reply(series.current.value),
                                  statistic(values.mean()), statistic(values.minimum()),
                                  statistic(values.maximum()),
                                  statistic(values.standard_deviation()), values.count());
             });
       }});
  add_command({.header = "MEASure:STATistics:RESet",
               .parameters = {},
               .handler = [this](Session&, const Arguments&) -> scpi::HandlerResult {
                 reset_statistics();
                 return std::nullopt;
               }});
}

scpi::HandlerResult Instrument::reply_with_source(
    std::size_t source,
    const std::function<std::string(const std::shared_ptr<const Capture>&, const CapturedChannel&)>&
        reply) const {
  const std::shared_ptr<const Capture> capture = latest_capture();
  if (capture == nullptr) {
    return kDataCorruptOrStale;
  }
  const std::optional<CapturedChannel>& channel = capture->channels.at(source - 1);
  if (!channel) {
    return kSettingsConflict;
  }
  return std::optional<std::string>(reply(capture, *channel));
}

void Instrument::add_setting(std::string_view header, scpi::ParameterSpec parameter,
                             std::function<void(Session&, const Arguments&)> set,
                             std::function<std::string(Session&, const Arguments&)> get) {
  add_command({.header = header,
               .parameters = {std::move(parameter)},
               .handler = [set = std::move(set)](
                              Session& caller, const Arguments& arguments) -> scpi::HandlerResult {
                 set(caller, arguments);
                 return std::nullopt;
               }});
  const std::string query = std::string(header) + '?';
  add_command({.header = query,
               .parameters = {},
               .handler = [get = std::move(get)](
                              Session& caller, const Arguments& arguments) -> scpi::HandlerResult {
                 return std::optional<std::string>(get(caller, arguments));
               }});
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
  capture_setting_changed();
}

void Instrument::change_channel(const Arguments& arguments,
                                const std::function<void(ChannelSettings&)>& change) {
  change(channels_.at(channel_of(arguments)));
  capture_setting_changed();
}

void Instrument::change_acquisition(const std::function<void(AcquisitionSettings&)>& change) {
  change(acquisition_);
  for (Generator& generator : generators_) {
    generator.configure(generator.settings(), acquisition_.sample_rate, clock_.now());
  }
  capture_setting_changed();
}

void Instrument::change_trigger(const std::function<void(TriggerSettings&)>& change) {
  change(trigger_);
  capture_setting_changed();
}

void Instrument::capture_setting_changed() {
  discard_capture();
  reset_statistics();
}

}  // namespace openscope
