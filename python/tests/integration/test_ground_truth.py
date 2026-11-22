"""The ground truth helper checked against the shapes as docs/scpi-reference.md (Generators)
defines them, sampled finely over whole cycles: its closed forms are what the engine is held to,
so they must not share a mistake with it. No engine runs here."""

import math

import pytest
from ground_truth import AMPLITUDES, NOISE_SHARE, Signal

from openscope_client import Function, Measurement

#: Points per cycle the shapes are sampled at.
POINTS_PER_CYCLE = 100_000


def shape(function: Function, phase: float) -> float:
    """One cycle of `function` at `phase` (0 to 1), per volt of peak, as documented."""
    match function:
        case Function.SINE:
            return math.sin(2 * math.pi * phase)
        case Function.SQUARE:
            return 1.0 if phase < 0.5 else -1.0
        case Function.TRIANGLE:
            return 1 - abs(4 * ((phase + 0.25) % 1) - 2)
        case Function.RAMP:
            return 2 * phase - 1
        case _:
            raise AssertionError(function)


def sampled(signal: Signal) -> list[float]:
    """One cycle of `signal` in volts, at POINTS_PER_CYCLE evenly spaced phases."""
    peak = signal.amplitude / 2
    return [
        signal.offset + peak * shape(signal.function, k / POINTS_PER_CYCLE)
        for k in range(POINTS_PER_CYCLE)
    ]


def edge_share(values: list[float], rising: bool) -> float:
    """The share of the cycle spent strictly between 10% and 90% of the swing while moving the
    way the edge goes."""
    low, high = min(values), max(values)
    lower, upper = low + 0.1 * (high - low), low + 0.9 * (high - low)
    moving = [
        lower < value < upper and (value > before) == rising
        for before, value in zip(values[-1:] + values[:-1], values, strict=True)
    ]
    return sum(moving) / POINTS_PER_CYCLE


SIGNALS = [
    Signal(Function.SINE, 1e3, 2.0, 0.5),
    Signal(Function.SQUARE, 2.5e3, 3.0, -0.25),
    Signal(Function.TRIANGLE, 50e3, 1.0, 1.0),
    Signal(Function.RAMP, 12.5e3, 4.0, 0.0),
]


@pytest.mark.parametrize("signal", SIGNALS, ids=lambda signal: signal.function)
def test_amplitudes_match_the_sampled_shape(signal):
    values = sampled(signal)
    step = 2 * signal.amplitude / POINTS_PER_CYCLE  # A ramp's top falls between two points.

    assert signal.expected(Measurement.PEAK_TO_PEAK) == pytest.approx(
        max(values) - min(values), abs=step
    )
    assert signal.expected(Measurement.MINIMUM) == pytest.approx(min(values), abs=step)
    assert signal.expected(Measurement.MAXIMUM) == pytest.approx(max(values), abs=step)
    assert signal.expected(Measurement.AVERAGE) == pytest.approx(
        sum(values) / POINTS_PER_CYCLE, abs=step
    )
    rms = math.sqrt(sum(value * value for value in values) / POINTS_PER_CYCLE)
    assert signal.expected(Measurement.RMS) == pytest.approx(rms, rel=1e-4)


@pytest.mark.parametrize("signal", SIGNALS, ids=lambda signal: signal.function)
def test_edges_match_the_sampled_shape(signal):
    values = sampled(signal)
    period = 1 / signal.frequency
    resolution = 2 * period / POINTS_PER_CYCLE

    assert signal.expected(Measurement.RISE_TIME) == pytest.approx(
        edge_share(values, rising=True) * period, abs=resolution
    )
    assert signal.expected(Measurement.FALL_TIME) == pytest.approx(
        edge_share(values, rising=False) * period, abs=resolution
    )
    above = sum(value > signal.offset for value in values) / POINTS_PER_CYCLE
    assert signal.expected(Measurement.DUTY_CYCLE) == pytest.approx(100 * above, abs=1e-2)
    assert signal.expected(Measurement.PERIOD) == period


def test_noise_adds_its_power_to_the_rms():
    clean = Signal(Function.SINE, amplitude=2.0)
    noisy = clean.with_noise()

    assert noisy.noise == NOISE_SHARE * 2.0
    assert noisy.expected(Measurement.RMS) == pytest.approx(math.sqrt(0.5 + noisy.noise**2))


def test_dc_ignores_the_amplitude():
    level = Signal(Function.DC, amplitude=2.0, offset=0.75)

    assert [level.expected(measurement) for measurement in AMPLITUDES] == [0.0] + [0.75] * 4


def test_tolerances_are_the_specs():
    clean = Signal(Function.SINE, frequency=2e3, amplitude=4.0)
    noisy = clean.with_noise()

    assert clean.tolerance(Measurement.PEAK_TO_PEAK) == pytest.approx(0.04)  # NFR-5: 1%.
    assert clean.tolerance(Measurement.AVERAGE) == pytest.approx(0.04)
    assert clean.tolerance(Measurement.FREQUENCY) == pytest.approx(2.0)  # NFR-5: 0.1%.
    assert noisy.tolerance(Measurement.FREQUENCY) == pytest.approx(10.0)  # NFR-6: 0.5%.
    assert noisy.tolerance(Measurement.RISE_TIME) == 2e-6  # NFR-6: 2 samples.
    assert clean.tolerance(Measurement.DUTY_CYCLE) == pytest.approx(1.0)
    with pytest.raises(ValueError, match="no duty-cycle tolerance"):
        noisy.tolerance(Measurement.DUTY_CYCLE)


def test_approx_accepts_a_reading_within_the_tolerance_only():
    clean = Signal(Function.SINE, frequency=2e3)

    assert clean.approx(Measurement.FREQUENCY) == 2001.9
    assert clean.approx(Measurement.FREQUENCY) == 1998.1
    assert clean.approx(Measurement.FREQUENCY) != 2002.1
