// The radix-2 FFT and the windowed magnitude spectrum in dBV RMS.

#include "openscope/fft.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace openscope {
namespace {

using Complex = std::complex<double>;

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The DFT by its definition, X[k] = sum x[n] e^(-2 pi i k n / N): the ground truth the FFT must
/// match, computed the slow way.
std::vector<Complex> direct_dft(const std::vector<Complex>& x) {
  const std::size_t n = x.size();
  std::vector<Complex> result(n);
  for (std::size_t k = 0; k < n; ++k) {
    for (std::size_t i = 0; i < n; ++i) {
      const auto turns = static_cast<double>((k * i) % n) / static_cast<double>(n);
      result[k] += x[i] * std::polar(1.0, -kTwoPi * turns);
    }
  }
  return result;
}

/// `count` samples of a sine with peak amplitude `peak` that completes `cycles` cycles over
/// `period` samples, rounded to float as captured samples are.
std::vector<float> sine(std::size_t count, double cycles, std::size_t period, double peak) {
  std::vector<float> samples(count);
  for (std::size_t n = 0; n < count; ++n) {
    samples[n] = static_cast<float>(
        peak * std::sin(kTwoPi * cycles * static_cast<double>(n) / static_cast<double>(period)));
  }
  return samples;
}

/// `volts` RMS in dBV.
double dbv(double volts) { return 20.0 * std::log10(volts); }

TEST(FftTest, MatchesTheDirectDftAtEveryPowerOfTwoUpTo1024) {
  for (std::size_t length = 1; length <= 1024; length *= 2) {
    std::vector<Complex> data(length);
    for (std::size_t i = 0; i < length; ++i) {
      const auto t = static_cast<double>(i);
      data[i] = {std::sin(0.3 * t) + (0.1 * t), std::cos(1.7 * t) - 0.5};
    }
    const std::vector<Complex> expected = direct_dft(data);
    Fft(length).transform(data);
    for (std::size_t k = 0; k < length; ++k) {
      EXPECT_NEAR(data[k].real(), expected[k].real(), 1e-9 * static_cast<double>(length))
          << "length " << length << " bin " << k;
      EXPECT_NEAR(data[k].imag(), expected[k].imag(), 1e-9 * static_cast<double>(length))
          << "length " << length << " bin " << k;
    }
  }
}

TEST(FftTest, AnImpulseHasAFlatSpectrum) {
  std::vector<Complex> data(8);
  data[0] = 1.0;
  Fft(8).transform(data);
  for (const Complex& bin : data) {
    EXPECT_EQ(bin, Complex(1.0, 0.0));
  }
}

TEST(FftTest, AComplexExponentialLandsInItsBin) {
  constexpr std::size_t kLength = 64;
  std::vector<Complex> data(kLength);
  for (std::size_t n = 0; n < kLength; ++n) {
    data[n] = std::polar(1.0, kTwoPi * 5.0 * static_cast<double>(n) / kLength);
  }
  Fft(kLength).transform(data);
  for (std::size_t k = 0; k < kLength; ++k) {
    EXPECT_NEAR(std::abs(data[k]), k == 5 ? 64.0 : 0.0, 1e-12) << "bin " << k;
  }
}

TEST(FftTest, RejectsALengthThatIsNotAPowerOfTwo) {
  EXPECT_THROW(Fft(0), std::invalid_argument);
  EXPECT_THROW(Fft(3), std::invalid_argument);
  EXPECT_THROW(Fft(1000), std::invalid_argument);
  EXPECT_NO_THROW(Fft(std::size_t{1} << 20U));
}

TEST(FftTest, RejectsDataOfAnotherLength) {
  const Fft fft(8);
  std::vector<Complex> data(4);
  EXPECT_THROW(fft.transform(data), std::invalid_argument);
  data.resize(16);
  EXPECT_THROW(fft.transform(data), std::invalid_argument);
}

TEST(SpectrumLengthTest, IsTheLargestPowerOfTwoInTheRecord) {
  EXPECT_EQ(spectrum_length(1), 1);
  EXPECT_EQ(spectrum_length(2), 2);
  EXPECT_EQ(spectrum_length(3), 2);
  EXPECT_EQ(spectrum_length(1023), 512);
  EXPECT_EQ(spectrum_length(1024), 1024);
  EXPECT_EQ(spectrum_length(10'000), 8192);
  EXPECT_EQ(spectrum_length(std::size_t{1} << 20U), std::size_t{1} << 20U);
}

/// Each window with the level, relative to the tone, of the bins either side of a bin-centred
/// tone: the window's cosine coefficients a1/2a0, a2/2a0 and a3/2a0 (Harris 1978), which is
/// exactly how far a periodic cosine-sum window spreads such a tone.
struct MainLobe {
  FftWindow window;
  std::vector<double> neighbours;
};

class BinCentredToneTest : public testing::TestWithParam<MainLobe> {};

TEST_P(BinCentredToneTest, ReadsItsRmsLevelAndSpreadsOnlyOverTheMainLobe) {
  constexpr std::size_t kLength = 1024;
  constexpr std::size_t kToneBin = 100;
  const MainLobe& lobe = GetParam();
  const std::vector<float> spectrum =
      magnitude_spectrum_db(sine(kLength, kToneBin, kLength, 1.0), lobe.window);

  ASSERT_EQ(spectrum.size(), (kLength / 2) + 1);
  const double tone = dbv(1.0 / std::numbers::sqrt2);  // 1 V peak is -3.0103 dBV RMS.
  EXPECT_NEAR(spectrum[kToneBin], tone, 1e-4);
  for (std::size_t k = 0; k < spectrum.size(); ++k) {
    const std::size_t distance = k > kToneBin ? k - kToneBin : kToneBin - k;
    if (distance == 0) {
      continue;
    }
    if (distance <= lobe.neighbours.size()) {
      EXPECT_NEAR(spectrum[k], tone + dbv(lobe.neighbours[distance - 1]), 1e-3) << "bin " << k;
    } else {
      EXPECT_LT(spectrum[k], -120.0) << "bin " << k;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Windows, BinCentredToneTest,
    testing::Values(MainLobe{.window = FftWindow::kRectangular, .neighbours = {}},
                    MainLobe{.window = FftWindow::kHann, .neighbours = {0.5}},
                    MainLobe{.window = FftWindow::kBlackmanHarris,
                             .neighbours = {0.48829 / (2 * 0.35875), 0.14128 / (2 * 0.35875),
                                            0.01168 / (2 * 0.35875)}}));

/// A window with its scalloping loss, the dB a tone exactly half-way between two bins reads below
/// its level (Harris 1978, table 1), and the bounds on the level ten bins further out, relative to
/// the tone: rectangular leaks far, Hann much less, Blackman-Harris below -92 dB.
struct Leakage {
  FftWindow window;
  double scalloping_loss_db;
  double far_min_db;
  double far_max_db;
};

class OffBinToneTest : public testing::TestWithParam<Leakage> {};

TEST_P(OffBinToneTest, LeaksAsTheWindowPredicts) {
  constexpr std::size_t kLength = 4096;
  constexpr std::size_t kLowerBin = 300;  // The tone sits at bin 300.5.
  const Leakage& leakage = GetParam();
  const std::vector<float> spectrum = magnitude_spectrum_db(
      sine(kLength, static_cast<double>(kLowerBin) + 0.5, kLength, 1.0), leakage.window);

  const double tone = dbv(1.0 / std::numbers::sqrt2);
  EXPECT_NEAR(spectrum[kLowerBin], tone - leakage.scalloping_loss_db, 0.01);
  EXPECT_NEAR(spectrum[kLowerBin + 1], tone - leakage.scalloping_loss_db, 0.01);
  const double far = spectrum[kLowerBin + 11] - tone;  // 10.5 bins above the tone.
  EXPECT_GT(far, leakage.far_min_db);
  EXPECT_LT(far, leakage.far_max_db);
}

INSTANTIATE_TEST_SUITE_P(Windows, OffBinToneTest,
                         testing::Values(Leakage{.window = FftWindow::kRectangular,
                                                 .scalloping_loss_db = 3.92,
                                                 .far_min_db = -35.0,
                                                 .far_max_db = -25.0},
                                         Leakage{.window = FftWindow::kHann,
                                                 .scalloping_loss_db = 1.42,
                                                 .far_min_db = -80.0,
                                                 .far_max_db = -60.0},
                                         Leakage{.window = FftWindow::kBlackmanHarris,
                                                 .scalloping_loss_db = 0.83,
                                                 .far_min_db = -200.0,
                                                 .far_max_db = -92.0}));

TEST(MagnitudeSpectrumTest, DcReadsItsVoltageInEveryWindow) {
  for (const FftWindow window :
       {FftWindow::kRectangular, FftWindow::kHann, FftWindow::kBlackmanHarris}) {
    const std::vector<float> spectrum = magnitude_spectrum_db(std::vector<float>(256, -0.5F), window);
    EXPECT_NEAR(spectrum[0], dbv(0.5), 1e-4);
  }
}

TEST(MagnitudeSpectrumTest, AToneAtNyquistReadsItsRmsLevel) {
  std::vector<float> samples(256);
  for (std::size_t n = 0; n < samples.size(); ++n) {
    samples[n] = n % 2 == 0 ? 0.25F : -0.25F;  // 0.25 V RMS at half the sample rate.
  }
  const std::vector<float> spectrum = magnitude_spectrum_db(samples, FftWindow::kRectangular);
  ASSERT_EQ(spectrum.size(), 129);
  EXPECT_NEAR(spectrum.back(), dbv(0.25), 1e-4);
  EXPECT_LT(spectrum[0], -120.0);
}

TEST(MagnitudeSpectrumTest, SilenceReadsTheFloor) {
  const std::vector<float> spectrum =
      magnitude_spectrum_db(std::vector<float>(64, 0.0F), FftWindow::kHann);
  ASSERT_EQ(spectrum.size(), 33);
  for (const float level : spectrum) {
    EXPECT_EQ(level, kSpectrumFloorDb);
  }
}

TEST(MagnitudeSpectrumTest, UsesOnlyTheFirstSpectrumLengthSamples) {
  std::vector<float> samples = sine(1024, 10, 1024, 1.0);
  const std::vector<float> power_of_two = magnitude_spectrum_db(samples, FftWindow::kHann);
  samples.resize(1500, 7.0F);  // Samples past 1024 must not change the spectrum.
  EXPECT_EQ(magnitude_spectrum_db(samples, FftWindow::kHann), power_of_two);
}

TEST(MagnitudeSpectrumTest, OneSampleIsOneDcBin) {
  const std::vector<float> spectrum = magnitude_spectrum_db(std::vector<float>{2.0F},
                                                            FftWindow::kRectangular);
  ASSERT_EQ(spectrum.size(), 1);
  EXPECT_NEAR(spectrum[0], dbv(2.0), 1e-4);
}

TEST(MagnitudeSpectrumTest, EveryWindowGivesFiniteLevelsForTheShortestRecords) {
  for (const FftWindow window :
       {FftWindow::kRectangular, FftWindow::kHann, FftWindow::kBlackmanHarris}) {
    for (std::size_t count = 1; count <= 4; ++count) {
      for (const float level : magnitude_spectrum_db(std::vector<float>(count, 1.0F), window)) {
        EXPECT_TRUE(std::isfinite(level)) << count << " samples";
        EXPECT_GE(level, kSpectrumFloorDb);
      }
    }
  }
}

TEST(MagnitudeSpectrumTest, RejectsNoSamples) {
  EXPECT_THROW(magnitude_spectrum_db({}, FftWindow::kHann), std::invalid_argument);
}

}  // namespace
}  // namespace openscope
