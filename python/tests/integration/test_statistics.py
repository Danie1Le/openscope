"""Measurement statistics over TCP: every capture since the last reset is counted, each statistic
matches the values the captures measured, and the status rules and resets hold
(docs/scpi-reference.md, Statistics)."""

import statistics

import pytest
from engine_helpers import (
    CLIPPED,
    NO_SIGNAL,
    NOT_A_NUMBER,
    STALE,
    eventually,
    scpi_error,
    take_capture,
    take_fault_capture,
)
from ground_truth import Signal, assert_measures

from openscope_client import (
    ErrorEntry,
    FaultKind,
    Function,
    Measurement,
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


def test_while_running_every_capture_is_counted(scope):
    signal = Signal(Function.SINE, 2e3)
    signal.apply(scope)
    take_capture(scope)
    scope.write("MEAS:STAT:RES;:RUN")

    def counted_several() -> bool:
        try:
            return scope.statistics(Measurement.FREQUENCY, 1).count > 1
        except ScpiError:  # Data corrupt or stale: no capture since the reset yet.
            return False

    eventually(counted_several)  # A capture every 15 ms at the *RST settings.
    scope.stop()

    counted = scope.statistics(Measurement.FREQUENCY, 1)

    assert counted.count > 1
    for value in (counted.current, counted.mean, counted.minimum, counted.maximum):
        assert value == signal.approx(Measurement.FREQUENCY)


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
