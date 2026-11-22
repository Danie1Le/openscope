"""Measurement statistics over TCP: every capture since the last reset is counted, each statistic
matches the values the captures measured, and the status rules and resets hold
(docs/scpi-reference.md, Statistics)."""

import statistics

import pytest
from engine_helpers import (
    CLIPPED,
    CLOCK_CHANNEL,
    NO_SIGNAL,
    NOT_A_NUMBER,
    PATIENCE,
    RECORD_LENGTH,
    STALE,
    eventually,
    read_channel,
    scpi_error,
    take_capture,
    take_fault_capture,
)
from ground_truth import SAMPLE_RATE, Signal, assert_measures

from openscope_client import (
    ErrorEntry,
    FaultKind,
    Function,
    Measurement,
    OpenScope,
    ScpiError,
    Statistics,
)

#: Singles a test takes, each one capture counted.
SINGLES = 10


def test_a_reset_then_a_single_gives_the_statistics_of_one_capture(scope):
    signal = Signal(Function.SINE, amplitude=3.0)
    signal.apply(scope)

    reply = scope.query("MEAS:STAT:RES;:SING;*OPC?;:MEAS:STAT? VPP,CHAN1")

    done, _, fields = reply.partition(";")
    current, mean, minimum, maximum, deviation, count = map(float, fields.split(","))
    assert (done, count, deviation) == ("1", 1, 0.0)
    assert mean == minimum == maximum == current
    assert_measures(scope, signal, Measurement.PEAK_TO_PEAK)  # The same capture.
    assert scope.measure(Measurement.PEAK_TO_PEAK, 1) == current


def test_each_statistic_matches_the_values_of_the_captures_counted(scope):
    # A flat level with noise never triggers: each Single is an untriggered capture, and each
    # one's mean differs.
    signal = Signal(Function.DC, offset=0.5, noise=0.1)
    signal.apply(scope)
    scope.write("STOP;:MEAS:STAT:RES")  # Nothing is captured between the Singles.
    values = []
    for _ in range(SINGLES):
        take_capture(scope)
        assert scope.waveform.preamble().untriggered
        values.append(scope.measure(Measurement.AVERAGE, 1))

    counted = scope.statistics(Measurement.AVERAGE, 1)

    assert counted == Statistics(
        values[-1],
        pytest.approx(statistics.fmean(values), rel=1e-6),
        min(values),
        max(values),
        pytest.approx(statistics.pstdev(values), abs=1e-6),
        SINGLES,
    )
    assert_measures(scope, signal, Measurement.AVERAGE)
    assert statistics.pstdev(values) > 0  # Distinct values, so each statistic is distinct.


#: A ramp on Generator 4 so slow (1000 s a cycle, 2 mV a second) that its mean over a capture
#: places the capture on the sample clock.
SLOW_CLOCK = Signal(Function.RAMP, 1e-3)
#: The longest a test may run on SLOW_CLOCK, in seconds: its wait for captures (at most PATIENCE)
#: and as long again for the commands around it.
LONGEST_CLOCKED_TEST = 2 * PATIENCE
#: The highest SLOW_CLOCK may start from: LONGEST_CLOCKED_TEST short of its wrap from +1 V to -1 V.
LATEST_CLOCK_START = SLOW_CLOCK.offset + SLOW_CLOCK.amplitude * (
    0.5 - SLOW_CLOCK.frequency * LONGEST_CLOCKED_TEST
)
#: Restarts allowed to find a start before LATEST_CLOCK_START; each one is a fresh draw.
CLOCK_STARTS = 10


def start_slow_clock(scope: OpenScope) -> None:
    """Takes a Single with SLOW_CLOCK on channel 4 under a flat channel 1 and no pre-trigger
    samples, its ramp far enough from its wrap that no test sees it wrap. Its phase carries over
    from the `*RST` 1 kHz sine at the sample index the setting lands at, which the client cannot
    pick, so a start too close to the wrap is drawn again after a `*RST`."""
    for _ in range(CLOCK_STARTS):
        scope.write("*RST")
        SLOW_CLOCK.apply(scope, CLOCK_CHANNEL)
        scope.write(f"CHAN{CLOCK_CHANNEL}:DISP ON;:SOUR1:FUNC DC;:TRIG:POS 0")
        take_capture(scope)
        clock = read_channel(scope, CLOCK_CHANNEL)
        if clock.samples[clock.preamble.time_zero] < LATEST_CLOCK_START:
            return
    pytest.fail(
        f"the clock started within {LONGEST_CLOCKED_TEST} s of its wrap {CLOCK_STARTS} times"
    )


def test_while_running_every_capture_is_counted(scope):
    # Untriggered captures follow each other, one record length apart, so the clock's mean rises
    # by one step per capture, and the captures counted are the steps from the first (the
    # minimum) to the last (the maximum), plus one.
    start_slow_clock(scope)
    scope.write("MEAS:STAT:RES;:RUN")

    def counted_several() -> bool:
        try:
            return scope.statistics(Measurement.AVERAGE, CLOCK_CHANNEL).count > 5
        except ScpiError:  # Data corrupt or stale: no capture since the reset yet.
            return False

    eventually(counted_several)  # A capture every 10 ms.
    scope.stop()

    counted = scope.statistics(Measurement.AVERAGE, CLOCK_CHANNEL)
    step = SLOW_CLOCK.amplitude * SLOW_CLOCK.frequency * RECORD_LENGTH / SAMPLE_RATE
    steps = (counted.maximum - counted.minimum) / step
    assert counted.current == counted.maximum  # The latest capture is the last.
    assert steps == pytest.approx(round(steps), abs=0.1), steps  # Whole records apart.
    assert counted.count == round(steps) + 1, steps


def test_a_reset_instrument_counts_from_its_next_capture_for_every_session(engine, open_client):
    scope, other = open_client(engine.port), open_client(engine.port)
    take_capture(scope)
    take_capture(scope)
    assert other.statistics(Measurement.RMS, 1).count >= 2

    scope.write("*RST;:SING")  # On one line: nothing is captured between them.
    scope.wait_complete()

    assert scope.statistics(Measurement.RMS, 1).count == 1
    assert other.statistics(Measurement.RMS, 1).count == 1


def stat_errors(
    scope, measurement: Measurement
) -> tuple[tuple[ErrorEntry, ...], Statistics | None]:
    """The entries `MEAS:STAT?` of `measurement` on channel 1 queued, and its reply (None when
    it replied nothing)."""
    error = scpi_error(lambda: scope.statistics(measurement, 1))
    counted = None if error.reply is None else Statistics(*map(float, error.reply.split(b",")))
    return error.errors, counted


def test_a_result_with_no_value_is_left_out_of_every_statistic(scope):
    Signal(Function.DC).apply(scope)
    scope.write("STOP;:MEAS:STAT:RES")
    take_capture(scope)

    errors, counted = stat_errors(scope, Measurement.FREQUENCY)

    assert errors == (NO_SIGNAL,)
    assert counted[:5] == (NOT_A_NUMBER,) * 5
    assert counted.count == 0
    assert scope.statistics(Measurement.PEAK_TO_PEAK, 1).count == 1  # Its value was counted.


def test_a_clipped_value_counted_warns_until_the_next_reset(scope):
    # The 5 V glitch clips at the 4 V limit; the 2 V one stays in range. Faults keep statistics.
    scope.write("TRIG:MODE NORM;LEV 1.5;POS 0;:SOUR1:FUNC DC;:MEAS:STAT:RES")
    take_fault_capture(scope, FaultKind.GLITCH, 5.0)
    take_fault_capture(scope, FaultKind.GLITCH, 2.0)

    errors, counted = stat_errors(scope, Measurement.MAXIMUM)

    assert errors == (CLIPPED,)
    assert counted == Statistics(2.0, 3.0, 2.0, 4.0, 1.0, 2)
    scope.reset_statistics()
    take_fault_capture(scope, FaultKind.GLITCH, 2.0)
    assert scope.statistics(Measurement.MAXIMUM, 1) == Statistics(2.0, 2.0, 2.0, 2.0, 0.0, 1)


@pytest.mark.parametrize("line", ["SOUR1:OFFS 0", "CHAN1:SCAL 1", "ACQ:POIN 10000", "TRIG:LEV 0"])
def test_a_setting_that_shapes_captures_resets_the_statistics_for_every_session(
    engine, open_client, line
):
    scope, other = open_client(engine.port), open_client(engine.port)
    take_capture(scope)
    assert other.statistics(Measurement.RMS, 1).count >= 1

    scope.write(line)  # The value already in place: it still resets.

    errors, _ = stat_errors(other, Measurement.RMS)
    assert errors == (STALE,)


@pytest.mark.parametrize(
    "line", ["TIM:SCAL 0.002", "WAV:SOUR CHAN1", "FFT:WIND RECT", "STOP", "SOUR1:FAUL GLIT"]
)
def test_other_commands_keep_the_statistics(scope, line):
    take_capture(scope)  # Then Stopped: no capture is counted after it.
    before = scope.statistics(Measurement.RMS, 1)

    scope.write(line)

    assert scope.statistics(Measurement.RMS, 1) == before
    assert before.count >= 1
