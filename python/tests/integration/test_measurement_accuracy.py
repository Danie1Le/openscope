"""All ten measurements over TCP against the Generator's ground truth, within the spec's NFR-5
(clean signals) and NFR-6 (noise at 1% of the amplitude) tolerances (docs/scpi-reference.md,
Measurements)."""

import math

import pytest
from engine_helpers import (
    LONGEST_RECORD,
    NO_SIGNAL,
    NOT_A_NUMBER,
    TOO_FEW_CYCLES,
    scpi_error,
    take_capture,
)
from ground_truth import AMPLITUDES, TIMING, Signal, assert_measures

from openscope_client import Function, Measurement

#: Signals that complete whole cycles in the `*RST` record of 10000 samples at 1 MS/s, within the
#: +/- 4 V input range at 1 V/div. VPP, VMIN and VMAX are extreme samples, and the phase a sample
#: falls at follows the settings sent before, so each signal has enough samples per cycle that its
#: peaks never fall 1% short between two samples: 23 for a sine (cos(pi/23) > 0.99), 100 for a
#: ramp, 200 for a triangle; a square wave holds its levels.
WHOLE_CYCLES = [
    Signal(Function.SINE),
    Signal(Function.SINE, 2.5e3, 5.0, 0.5),
    Signal(Function.SINE, 25e3, 1.0, -1.0),
    Signal(Function.SQUARE, 1e3, 3.0, -0.25),
    Signal(Function.SQUARE, 50e3),
    Signal(Function.TRIANGLE, 2.5e3, 4.0, 1.0),
    Signal(Function.RAMP),
    Signal(Function.RAMP, 2.5e3, 1.5, 0.75),
]


def signal_id(signal: Signal) -> str:
    """A test id that names the signal's shape and settings."""
    return f"{signal.function}-{signal.frequency:g}Hz-{signal.amplitude:g}Vpp-{signal.offset:g}V"


@pytest.mark.parametrize("signal", WHOLE_CYCLES, ids=signal_id)
def test_every_measurement_of_a_clean_signal_reads_its_ground_truth(scope, signal):
    signal.apply(scope)
    take_capture(scope)

    for measurement in Measurement:
        assert_measures(scope, signal, measurement)


def test_a_sine_at_ten_samples_per_cycle_has_its_frequency_rms_and_mean(scope):
    # NFR-5's fewest samples per cycle: 1000 cycles. Its sampled peaks may fall 5% short.
    signal = Signal(Function.SINE, 100e3, 1.0, -1.0)
    signal.apply(scope)
    take_capture(scope)

    for measurement in (
        Measurement.FREQUENCY,
        Measurement.PERIOD,
        Measurement.DUTY_CYCLE,
        Measurement.RMS,
        Measurement.AVERAGE,
    ):
        assert_measures(scope, signal, measurement)


#: NFR-5's corner: a record of 10 cycles at 10 samples per cycle (100 kHz at 1 MS/s).
CORNER_RECORD = 100
CORNER = Signal(Function.SINE, 100e3, 1.0, -1.0)
#: The gap between 1 and the next float32.
FLOAT32_EPSILON = 2.0**-23


def test_ten_cycles_at_ten_samples_per_cycle_keep_their_frequency(scope):
    scope.acquisition.record_length = CORNER_RECORD
    CORNER.apply(scope)
    take_capture(scope)

    assert_measures(scope, CORNER, Measurement.FREQUENCY)
    assert_measures(scope, CORNER, Measurement.PERIOD)
    # Extreme samples fall at most half a sample period (pi/10 of the cycle) from each peak, so
    # this bound holds wherever the samples fall. With two samples either side of each peak the
    # reading is the bound itself, less the float32 rounding of the samples and the reply (under
    # 2 float32 epsilons of the amplitude).
    expected = CORNER.expected(Measurement.PEAK_TO_PEAK)
    lowest = (math.cos(math.pi / 10) - 2 * FLOAT32_EPSILON) * expected
    peak_to_peak = scope.measure(Measurement.PEAK_TO_PEAK, 1)
    assert lowest <= peak_to_peak <= expected, peak_to_peak


def test_ten_cycles_at_ten_samples_per_cycle_with_noise_keep_their_frequency(scope):
    signal = CORNER.with_noise()
    scope.acquisition.record_length = CORNER_RECORD
    signal.apply(scope)

    for _ in range(NOISY_CAPTURES):
        take_capture(scope)
        assert_measures(scope, signal, Measurement.FREQUENCY)
        assert_measures(scope, signal, Measurement.PERIOD)


#: Sines whose record ends part-way through a cycle (12.345 and 33.5 cycles): NFR-5 still holds for
#: the frequency, and for the peak-to-peak and RMS of a sine centred on 0 V.
PART_CYCLES = [Signal(Function.SINE, 1234.5), Signal(Function.SINE, 3350.0, 0.5)]
PART_CYCLE_MEASUREMENTS = (
    Measurement.FREQUENCY,
    Measurement.PERIOD,
    Measurement.PEAK_TO_PEAK,
    Measurement.RMS,
)


@pytest.mark.parametrize("signal", PART_CYCLES, ids=signal_id)
def test_a_record_ending_part_way_through_a_cycle_keeps_its_accuracy(scope, signal):
    signal.apply(scope)
    take_capture(scope)

    for measurement in PART_CYCLE_MEASUREMENTS:
        assert_measures(scope, signal, measurement)


def test_the_longest_record_measures_a_slow_sine(scope):
    signal = Signal(Function.SINE, 10.0)  # 10.49 cycles of 100000 samples.
    scope.acquisition.record_length = LONGEST_RECORD
    signal.apply(scope)
    scope.timeout = 30
    take_capture(scope)

    for measurement in PART_CYCLE_MEASUREMENTS:
        assert_measures(scope, signal, measurement)


#: Each capture holds different noise: the sample index it starts at follows the wall clock.
NOISY_CAPTURES = 5

NOISY = [
    Signal(Function.SINE).with_noise(),
    Signal(Function.SINE, 100e3).with_noise(),
    Signal(Function.SQUARE).with_noise(),
    Signal(Function.TRIANGLE).with_noise(),
]


# NFR-6 covers frequency, so period; the RMS, noise included, is held to NFR-5's 1%. The spec sets
# no duty-cycle tolerance under noise.
@pytest.mark.parametrize("signal", NOISY, ids=signal_id)
def test_frequency_with_one_percent_noise_reads_its_ground_truth(scope, signal):
    signal.apply(scope)

    for _ in range(NOISY_CAPTURES):
        take_capture(scope)
        assert_measures(scope, signal, Measurement.FREQUENCY)
        assert_measures(scope, signal, Measurement.PERIOD)
        assert_measures(scope, signal, Measurement.RMS)


def test_square_wave_edges_with_one_percent_noise_stay_within_two_sample_periods(scope):
    signal = Signal(Function.SQUARE).with_noise()
    signal.apply(scope)

    for _ in range(NOISY_CAPTURES):
        take_capture(scope)
        assert_measures(scope, signal, Measurement.RISE_TIME)
        assert_measures(scope, signal, Measurement.FALL_TIME)


def assert_status(scope, measurement, entry):
    """Checks that `measurement` of channel 1 replies not-a-number and queues `entry` alone."""
    error = scpi_error(lambda: scope.measure(measurement, 1))
    assert error.errors == (entry,), measurement
    assert float(error.reply) == NOT_A_NUMBER, measurement


def test_a_flat_signal_has_amplitudes_but_no_timing(scope):
    signal = Signal(Function.DC, offset=-0.625)
    signal.apply(scope)
    take_capture(scope)

    for measurement in AMPLITUDES:
        assert scope.measure(measurement, 1) == signal.expected(measurement)  # Exact.
    for measurement in TIMING:
        assert_status(scope, measurement, NO_SIGNAL)


def test_under_one_cycle_is_too_few_cycles_for_frequency_period_and_duty(scope):
    Signal(Function.SINE, 50.0).apply(scope)  # Half a cycle in the record.
    take_capture(scope)

    for measurement in (Measurement.FREQUENCY, Measurement.PERIOD, Measurement.DUTY_CYCLE):
        assert_status(scope, measurement, TOO_FEW_CYCLES)
