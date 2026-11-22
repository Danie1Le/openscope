"""The edge trigger over TCP: level, slope, hysteresis, holdoff, pre-trigger position, the Normal
and Auto trigger modes and the untriggered flag (docs/scpi-reference.md, Trigger). Where a test
needs the sample index a capture was taken at, it reads the index clock on channel 4."""

import itertools
import math
import time

import pytest
from engine_helpers import (
    SOCKET_ONLY,
    clock_at_time_zero,
    eventually,
    indexes_between,
    start_index_clock,
    take_capture,
)
from ground_truth import SAMPLE_RATE, Signal

from openscope_client import Function, ScpiError, Slope, WaveformData

#: The most the `*RST` sine (1 kHz, 1 V peak) changes from one sample to the next at 1 MS/s.
SINE_STEP = 2 * math.pi * 1e3 / SAMPLE_RATE


def assert_reaches_level(waveform: WaveformData, level: float, slope: Slope) -> None:
    """Checks that time zero is the first sample at the level in the slope's direction, coming
    from the other side, on the `*RST` sine."""
    sign = 1 if slope == Slope.POSITIVE else -1
    zero = waveform.preamble.time_zero
    assert not waveform.preamble.untriggered
    assert 0 <= sign * (waveform.samples[zero] - level) < SINE_STEP
    if zero > 0:
        assert sign * (waveform.samples[zero - 1] - level) < 0


@pytest.mark.parametrize(
    ("level", "slope"),
    [(-0.5, Slope.POSITIVE), (0.7, Slope.POSITIVE), (0.5, Slope.NEGATIVE), (-0.7, Slope.NEGATIVE)],
)
def test_time_zero_is_where_the_signal_reaches_the_level_along_the_slope(scope, level, slope):
    scope.write(f"TRIG:MODE NORM;LEV {level};SLOP {slope}")
    take_capture(scope)

    assert_reaches_level(scope.waveform.read(), level, slope)


#: Trigger positions in percent, and the sample time zero sits at in the 10000-sample record:
#: round(position / 100 x 9999), half away from zero.
POSITIONS = [(0, 0), (25, 2500), (50, 5000), (100, 9999)]


@pytest.mark.parametrize(("position", "time_zero"), POSITIONS)
def test_the_trigger_position_places_the_pre_trigger_samples(scope, position, time_zero):
    scope.write(f"TRIG:MODE NORM;LEV 0.3;POS {position}")
    take_capture(scope)

    waveform = scope.waveform.read()
    assert waveform.preamble.time_zero == time_zero
    assert_reaches_level(waveform, 0.3, Slope.POSITIVE)


# The band is 0.1 divisions: 0.1 V at 1 V/div, 0.05 V at 0.5 V/div. A square wave centred on the
# level crosses it every half cycle, but is a trigger event only when it swings past the band.
@pytest.mark.parametrize(
    ("scale", "amplitude", "triggered"),
    [(1.0, 0.18, False), (1.0, 0.22, True), (0.5, 0.18, True)],
)
def test_the_hysteresis_band_is_a_tenth_of_a_division(scope, scale, amplitude, triggered):
    Signal(Function.SQUARE, amplitude=amplitude, offset=0.5).apply(scope)
    scope.write(f"CHAN1:SCAL {scale};:TRIG:LEV 0.5")  # Auto: a capture comes either way.
    take_capture(scope)

    waveform = scope.waveform.read()
    assert waveform.preamble.untriggered is not triggered
    if triggered:
        zero = waveform.preamble.time_zero
        low, high = waveform.samples[zero - 1 : zero + 1]
        assert (low, high) == pytest.approx((0.5 - amplitude / 2, 0.5 + amplitude / 2))


def test_a_disabled_channel_can_be_the_trigger_source(scope):
    # Both Generators run at 1 kHz from the same phase: the square steps up through 0.3 V where
    # the sine on channel 1 starts its cycle, at 0 V; the sine reaches 0.3 V 49 samples later.
    Signal(Function.SQUARE).apply(scope, 2)
    scope.write("TRIG:SOUR CHAN2;MODE NORM;LEV 0.3")
    take_capture(scope)

    assert scope.channel(2).enabled is False
    waveform = scope.waveform.read()
    assert not waveform.preamble.untriggered
    assert abs(waveform.samples[waveform.preamble.time_zero]) < SINE_STEP


def test_normal_mode_captures_only_on_a_trigger_event(scope):
    # One line: no capture can complete between the reset and a level the sine never reaches.
    scope.write("*RST;:TRIG:MODE NORM;LEV 1.5")
    time.sleep(0.2)  # Auto would have taken over ten captures by now.

    with pytest.raises(ScpiError, match="Data corrupt or stale"):
        scope.waveform.preamble()

    scope.trigger.level = 0.5
    take_capture(scope)
    assert_reaches_level(scope.waveform.read(), 0.5, Slope.POSITIVE)


def capture_spacing(scope, captures: int = 5) -> list[int]:
    """Sample indexes between the trigger positions of consecutive distinct captures while
    Running, read from the index clock; a capture completed between two polls doubles one."""
    readings: list[float] = []

    def read_clock() -> bool:
        reading = clock_at_time_zero(scope)
        if not readings or reading != readings[-1]:
            readings.append(reading)
        return len(readings) > captures

    eventually(read_clock)
    # The first reading may be a capture taken before RUN, which started collecting afresh.
    return [indexes_between(a, b) for a, b in itertools.pairwise(readings[1:])]


#: The record length of the tests that time consecutive captures: a capture every 100 ms or more,
#: slower than a client polls (over the raw socket: PyVISA takes longer to read 400 kB).
LONG_RECORD = 100_000


# Untriggered captures come one record length after the pre-trigger samples fill, counted on the
# sample clock: samples 50000 to 149999, then 200000 to 299999, and so on.
@SOCKET_ONLY
def test_auto_mode_takes_an_untriggered_capture_every_auto_timeout(scope):
    start_index_clock(scope)
    Signal(Function.DC).apply(scope)
    scope.acquisition.record_length = LONG_RECORD
    take_capture(scope)
    assert scope.waveform.preamble().untriggered

    scope.run()
    spacing = capture_spacing(scope)

    assert min(spacing) == LONG_RECORD + LONG_RECORD // 2, spacing
    assert all(gap % min(spacing) == 0 for gap in spacing), spacing


# The square wave rises every 1000 samples. The next capture collects its 50000 pre-trigger
# samples after the previous one ends, 50000 after its trigger event, so with no holdoff the next
# event that places a capture is 100000 samples on. A holdoff takes the first edge at or after its
# end: 3 samples short of the edge 300000 samples on, it takes that edge; 3 samples past, the next.
@SOCKET_ONLY
@pytest.mark.parametrize(
    ("holdoff", "spacing"),
    [(0.0, 100_000), (0.299997, 300_000), (0.300003, 301_000), (0.3002, 301_000)],
)
def test_the_holdoff_spaces_the_trigger_events_of_captures(scope, holdoff, spacing):
    start_index_clock(scope)
    Signal(Function.SQUARE).apply(scope)
    scope.write(f"ACQ:POIN {LONG_RECORD};:TRIG:MODE NORM;HOLD {holdoff}")
    take_capture(scope)

    scope.run()
    gaps = capture_spacing(scope)

    # The phase at a sample index rounds into one sample or the next: an edge may move by one.
    assert min(gaps) == pytest.approx(spacing, abs=1), gaps
    assert all(gap >= spacing - 1 for gap in gaps), gaps
