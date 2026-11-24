"""Ground truth for the TCP suite: what each measurement of a Generator's signal must read, worked
out from the Generator's settings alone (docs/scpi-reference.md, Generators), and the accuracy the
spec requires of it.

The tolerances are the spec's NFR-5 and NFR-6 (spec #1, Further Notes; tickets #9 and #10 state
them). They are never widened to make a test pass: a measurement that misses one is a bug.
"""

import math
from dataclasses import dataclass, replace
from typing import Any

import pytest

from openscope_client import Function, Measurement, OpenScope

#: NFR-5: peak-to-peak and RMS within 1% on a clean signal. The spec names no tolerance for the
#: minimum, maximum and mean; they are held to the same 1%, of the peak-to-peak amplitude (the
#: mean of a signal centred on 0 V is 0, so a share of the value itself would demand exactness).
#: Nor does it name one for the duty cycle: on a clean signal that is held to the same 1%, of the
#: cycle (1 percentage point); with noise it is not checked.
AMPLITUDE_TOLERANCE = 0.01
#: NFR-5: frequency within 0.1% on a clean sine with at least 10 cycles and 10 samples per cycle.
CLEAN_FREQUENCY_TOLERANCE = 1e-3
#: NFR-6: frequency within 0.5% with noise at 1% of the amplitude.
NOISY_FREQUENCY_TOLERANCE = 5e-3
#: NFR-6: the noise its tolerances hold for, as RMS volts per volt of peak-to-peak amplitude.
NOISE_SHARE = 0.01
#: NFR-6: rise and fall time within 2 sample periods (for fast edges: noise moves a slow edge's
#: 10% and 90% crossings further, docs/scpi-reference.md, Edges and cycles).
EDGE_TOLERANCE_SAMPLES = 2

#: The `*RST` sample rate, samples per second.
SAMPLE_RATE = 1e6

#: The mean square of each shape over a whole cycle, per square volt of peak (half the
#: peak-to-peak amplitude).
_MEAN_SQUARE = {
    Function.SINE: 1 / 2,
    Function.SQUARE: 1.0,
    Function.TRIANGLE: 1 / 3,
    Function.RAMP: 1 / 3,
    Function.DC: 0.0,
}
#: The share of a cycle each shape takes to rise (then fall) from 10% to 90% of its swing: a sine
#: from -0.8 to +0.8 of its peak, a triangle over 0.8 of each half cycle, a ramp over 0.8 of its
#: cycle and dropping back at once, a square wave stepping at once.
_RISE_SHARE = {
    Function.SINE: math.asin(0.8) / math.pi,
    Function.SQUARE: 0.0,
    Function.TRIANGLE: 0.4,
    Function.RAMP: 0.8,
}
_FALL_SHARE = {**_RISE_SHARE, Function.RAMP: 0.0}

#: The measurements of voltage; they never report No signal or Too few cycles.
AMPLITUDES = (
    Measurement.PEAK_TO_PEAK,
    Measurement.MINIMUM,
    Measurement.MAXIMUM,
    Measurement.AVERAGE,
    Measurement.RMS,
)
#: The measurements that need edges or cycles; only they report No signal.
TIMING = tuple(measurement for measurement in Measurement if measurement not in AMPLITUDES)


@dataclass(frozen=True)
class Signal:
    """A Generator's settings (`SOUR<n>:...`): the source of every expected value here."""

    function: Function
    #: Hertz.
    frequency: float = 1e3
    #: Volts peak to peak.
    amplitude: float = 2.0
    #: Volts.
    offset: float = 0.0
    #: Volts RMS.
    noise: float = 0.0

    def apply(self, scope: OpenScope, channel: int = 1) -> None:
        """Sets Generator `channel` to this signal, in one line."""
        scope.write(
            f"SOUR{channel}:FUNC {self.function};FREQ {self.frequency};AMPL {self.amplitude};"
            f"OFFS {self.offset};NOIS {self.noise}"
        )

    def expected(self, measurement: Measurement) -> float:
        """What `measurement` of a capture of whole cycles of this signal reads (NaN for the
        timing of a flat signal, which has none)."""
        peak = 0.0 if self.function == Function.DC else self.amplitude / 2
        mean_square = self.offset**2 + _MEAN_SQUARE[self.function] * peak**2 + self.noise**2
        period = 1 / self.frequency
        return {
            Measurement.PEAK_TO_PEAK: 2 * peak,
            Measurement.MINIMUM: self.offset - peak,
            Measurement.MAXIMUM: self.offset + peak,
            Measurement.AVERAGE: self.offset,
            Measurement.RMS: math.sqrt(mean_square),
            Measurement.FREQUENCY: self.frequency,
            Measurement.PERIOD: period,
            # Every shape spends half its cycle above its 50% level.
            Measurement.DUTY_CYCLE: 50.0,
            Measurement.RISE_TIME: _RISE_SHARE.get(self.function, math.nan) * period,
            Measurement.FALL_TIME: _FALL_SHARE.get(self.function, math.nan) * period,
        }[measurement]

    def tolerance(self, measurement: Measurement) -> float:
        """How far `measurement` may read from `expected`: NFR-5 for a clean signal, NFR-6 for
        one with noise."""
        timing = NOISY_FREQUENCY_TOLERANCE if self.noise else CLEAN_FREQUENCY_TOLERANCE
        match measurement:
            case Measurement.PEAK_TO_PEAK | Measurement.RMS:
                return AMPLITUDE_TOLERANCE * self.expected(measurement)
            case Measurement.MINIMUM | Measurement.MAXIMUM | Measurement.AVERAGE:
                return AMPLITUDE_TOLERANCE * self.amplitude
            case Measurement.FREQUENCY | Measurement.PERIOD:
                return timing * self.expected(measurement)
            case Measurement.DUTY_CYCLE:
                if self.noise:
                    raise ValueError("the spec sets no duty-cycle tolerance under noise")
                return 100 * AMPLITUDE_TOLERANCE
            case Measurement.RISE_TIME | Measurement.FALL_TIME:
                return EDGE_TOLERANCE_SAMPLES / SAMPLE_RATE

    def approx(self, measurement: Measurement) -> Any:
        """What `measurement` of this signal may read: equal to it is a reading within the
        spec's tolerance of the ground truth."""
        return pytest.approx(self.expected(measurement), abs=self.tolerance(measurement))

    def with_noise(self) -> "Signal":
        """This signal with the noise NFR-6 holds for: 1% of its amplitude, RMS."""
        return replace(self, noise=NOISE_SHARE * self.amplitude)


def dbv(volts: float) -> float:
    """`volts` RMS in dBV, the unit of spectrum levels (the twin of tests/decibels.hpp)."""
    return 20 * math.log10(volts)


def assert_measures(scope: OpenScope, signal: Signal, measurement: Measurement) -> None:
    """Checks that `measurement` of channel 1 in the latest capture of `signal` reads its ground
    truth within the spec's tolerance, with an OK status (the client raises otherwise)."""
    value = scope.measure(measurement, 1)
    assert value == signal.approx(measurement), f"{measurement} read {value}"
