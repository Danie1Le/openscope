#include "openscope/generator.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "openscope/fault.hpp"
#include "openscope/phase.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {
namespace {

/// SplitMix64's finaliser: a bijection that spreads every input bit over the whole output.
constexpr std::uint64_t mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15U;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9U;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBU;
  return x ^ (x >> 31U);
}

/// Independent random streams of one Generator, so the noise function and the added noise never
/// share values.
enum class Stream : std::uint8_t { kNoiseFunction, kGaussianRadius, kGaussianAngle };

/// A uniform value in [0, 1) that depends only on the seed, the stream and the sample index: a
/// counter-based generator, so any sample can be produced without producing the ones before it.
double uniform(std::uint64_t seed, Stream stream, SampleIndex index) {
  const std::uint64_t bits = mix(mix(mix(seed) + static_cast<std::uint64_t>(stream)) + index);
  return static_cast<double>(bits >> 11U) * 0x1p-53;  // The top 53 bits fill a double exactly.
}

/// A standard normal value for `index` (Box-Muller).
double gaussian(std::uint64_t seed, SampleIndex index) {
  // In (0, 1], so the log is finite.
  const double radius = 1.0 - uniform(seed, Stream::kGaussianRadius, index);
  const double angle = uniform(seed, Stream::kGaussianAngle, index);
  return std::sqrt(-2.0 * std::log(radius)) * std::cos(2.0 * std::numbers::pi * angle);
}

/// One cycle of the periodic `function` at `phase` in [0, 1), scaled to [-1, 1]. Every shape
/// starts its cycle the way the sine does, at zero and rising, except square (high first half)
/// and ramp (rising from -1).
double shape(GeneratorFunction function, double phase) {
  switch (function) {
    case GeneratorFunction::kSine:
      return std::sin(2.0 * std::numbers::pi * phase);
    case GeneratorFunction::kSquare:
      return phase < 0.5 ? 1.0 : -1.0;
    case GeneratorFunction::kTriangle:
      if (phase < 0.25) {
        return 4.0 * phase;
      }
      return phase < 0.75 ? 2.0 - (4.0 * phase) : (4.0 * phase) - 4.0;
    case GeneratorFunction::kRamp:
      return (2.0 * phase) - 1.0;
    case GeneratorFunction::kDc:
    case GeneratorFunction::kNoise:
      break;
  }
  return 0.0;
}

}  // namespace

Generator::Generator(std::uint64_t seed, double sample_rate)
    : seed_(seed), cycles_per_sample_(cycles_per_sample(settings_.frequency, sample_rate)) {}

void Generator::configure(const GeneratorSettings& settings, double sample_rate, SampleIndex at) {
  // The anchor holds the settings' phase alone: the frequency steps add theirs on top.
  anchor_phase_ = wrap_phase(settings_cycles_at(at));
  anchor_index_ = at;
  settings_ = settings;
  cycles_per_sample_ = cycles_per_sample(settings.frequency, sample_rate);
}

bool Generator::inject(const Fault& fault) {
  // A Fault that has ended only leaves the phase it gained, which moves into the anchor.
  std::erase_if(faults_, [&](const Fault& old) {
    if (old.end() > fault.start()) {
      return false;
    }
    anchor_phase_ = wrap_phase(anchor_phase_ + old.phase_shift(old.end()));
    return true;
  });
  if (faults_.size() >= kMaxFaults) {
    return false;
  }
  faults_.push_back(fault);
  return true;
}

double Generator::settings_cycles_at(SampleIndex index) const {
  // Signed, so an index before the anchor still has a well-defined phase.
  const auto elapsed = static_cast<double>(static_cast<std::int64_t>(index - anchor_index_));
  return anchor_phase_ + (elapsed * cycles_per_sample_);
}

double Generator::phase_at(SampleIndex index) const {
  double cycles = settings_cycles_at(index);
  for (const Fault& fault : faults_) {
    cycles += fault.phase_shift(index);
  }
  return wrap_phase(cycles);
}

void Generator::generate(SampleIndex first, std::span<float> samples) const {
  const double peak = settings_.amplitude / 2.0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const SampleIndex index = first + i;
    double volts = settings_.offset;
    if (settings_.function == GeneratorFunction::kNoise) {
      volts += peak * ((2.0 * uniform(seed_, Stream::kNoiseFunction, index)) - 1.0);
    } else {
      volts += peak * shape(settings_.function, phase_at(index));
    }
    if (settings_.noise > 0.0) {
      volts += settings_.noise * gaussian(seed_, index);
    }
    for (const Fault& fault : faults_) {
      volts = fault.apply(index, volts);
    }
    samples[i] = static_cast<float>(volts);
  }
}

}  // namespace openscope
