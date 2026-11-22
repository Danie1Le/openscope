"""`*IDN?` and error handling against the real engine: the client raises the code and message the
instrument queues (docs/scpi-reference.md, Error codes)."""

import re

import pytest
from engine_helpers import REJECTED_LINES, take_capture

from openscope_client import NO_ERROR, ErrorEntry, Function, Measurement, ScpiError, TriggerMode


def test_identify_names_the_instrument(scope):
    identity = scope.identify()

    assert (identity.manufacturer, identity.model) == ("OpenScope", "OS-4")
    assert re.fullmatch(r"[0-9]{6}", identity.serial)
    assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", identity.version)


@pytest.mark.parametrize(
    ("line", "error"), REJECTED_LINES, ids=[str(e.code) for _, e in REJECTED_LINES]
)
def test_a_rejected_command_raises_its_code_and_message(scope, line, error):
    with pytest.raises(ScpiError) as raised:
        scope.write(line)

    assert raised.value.errors == (error,)
    assert scope.next_error() == NO_ERROR


def test_a_rejected_setting_keeps_its_value(scope):
    scope.generator(1).frequency = 2500

    with pytest.raises(ScpiError, match="Data out of range"):
        scope.generator(1).frequency = -1

    assert scope.generator(1).frequency == 2500


def test_a_query_before_any_capture_raises_instead_of_waiting_for_a_reply(scope):
    # One line, so no capture can complete between the reset and Normal mode with a level the
    # signal never reaches.
    scope.write("*RST;:TRIG:MODE NORM;LEV 100")

    with pytest.raises(ScpiError) as raised:
        scope.waveform.read()

    assert (raised.value.code, raised.value.message) == (-230, "Data corrupt or stale")
    assert raised.value.reply is None
    assert scope.trigger.mode == TriggerMode.NORMAL  # The client carries on.


def test_a_query_of_a_channel_not_in_the_capture_raises_settings_conflict(scope):
    take_capture(scope)
    scope.waveform.source = 2  # Channel 2 is off at *RST.

    with pytest.raises(ScpiError, match="Settings conflict") as raised:
        scope.waveform.read()

    assert raised.value.code == -221


def test_a_measurement_with_no_signal_raises_its_status_and_carries_not_a_number(scope):
    scope.generator(1).function = Function.DC
    take_capture(scope)

    with pytest.raises(ScpiError) as raised:
        scope.measure(Measurement.FREQUENCY, 1)

    assert raised.value.errors == (ErrorEntry(-200, "Execution error;No signal"),)
    assert raised.value.reply == b"9.91E37"
    assert scope.measure(Measurement.PEAK_TO_PEAK, 1) == 0.0


def test_every_error_of_a_line_is_raised_with_the_replies(scope):
    scope.channel(1).scale = 0.001  # A 2 Vpp sine clips at +/- 4 mV.
    take_capture(scope)

    with pytest.raises(ScpiError) as raised:
        scope.query("MEAS:VPP? CHAN1;VRMS? CHAN1")

    assert raised.value.errors == (ErrorEntry(-231, "Data questionable;Clipped"),) * 2
    assert raised.value.reply is not None
    peak_to_peak, rms = (float(value) for value in raised.value.reply.split(b";"))
    assert peak_to_peak == pytest.approx(0.008)
    assert 0 < rms <= 0.004
