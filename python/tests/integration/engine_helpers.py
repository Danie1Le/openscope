"""Helpers shared by the tests against the engine."""

from collections.abc import Callable

import pytest

from openscope_client import ErrorEntry, OpenScope, ScpiError

#: Lines the instrument rejects, each with the one error it queues; no two errors alike
#: (docs/scpi-reference.md, Error codes).
REJECTED_LINES = (
    ("SYST:BOGUS", ErrorEntry(-113, "Undefined header")),
    ("CHAN5:SCAL 1", ErrorEntry(-114, "Header suffix out of range")),
    ("SOUR1:FREQ 2E9", ErrorEntry(-222, "Data out of range")),
    ("TRIG:MODE SING", ErrorEntry(-224, "Illegal parameter value")),
    ("SOUR1:FREQ", ErrorEntry(-109, "Missing parameter")),
    ("RUN 1", ErrorEntry(-108, "Parameter not allowed")),
    ("SOUR1:FREQ 1,", ErrorEntry(-102, "Syntax error")),
    ("SOUR1:FREQ 1 V", ErrorEntry(-131, "Invalid suffix")),
)

#: What a measurement that has no value replies: SCPI's not-a-number (docs/scpi-reference.md,
#: Measurement status), and the entries that name its status.
NOT_A_NUMBER = 9.91e37
NO_SIGNAL = ErrorEntry(-200, "Execution error;No signal")
TOO_FEW_CYCLES = ErrorEntry(-200, "Execution error;Too few cycles")

#: The longest record length (`ACQ:POIN`).
LONGEST_RECORD = 1048576


def take_capture(scope: OpenScope) -> None:
    """Arms a Single and waits until its capture is complete (`SING`, then `*OPC?`)."""
    scope.single()
    scope.wait_complete()


def scpi_error(call: Callable[[], object]) -> ScpiError:
    """What `call` raised: it must raise ScpiError, carrying the queued entries and any reply
    the line sent despite them (such as a status entry's not-a-number)."""
    with pytest.raises(ScpiError) as raised:
        call()
    return raised.value
