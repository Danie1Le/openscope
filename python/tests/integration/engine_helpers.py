"""Helpers shared by the tests against the engine."""

from openscope_client import ErrorEntry, OpenScope

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


def take_capture(scope: OpenScope) -> None:
    """Arms a Single and waits until its capture is complete (`SING`, then `*OPC?`)."""
    scope.single()
    scope.wait_complete()
