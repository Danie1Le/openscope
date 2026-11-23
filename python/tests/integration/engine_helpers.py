"""Helpers shared by the tests against the engine."""

import time
from collections.abc import Callable

import pytest
from ground_truth import SAMPLE_RATE, Signal

from openscope_client import ErrorEntry, FaultKind, Function, OpenScope, ScpiError, WaveformData

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
#: The warning a Clipped value queues, and what a query of capture data queues before any capture
#: (or `MEAS:STAT?` before any since the last reset).
CLIPPED = ErrorEntry(-231, "Data questionable;Clipped")
STALE = ErrorEntry(-230, "Data corrupt or stale")

#: The fields of the engine's `live` and `stopping` log events that count what live mode lost.
LIVE_COUNTERS = ("dropped_blocks", "skipped_samples", "discarded_captures")

#: The `*RST` record length, and the longest (`ACQ:POIN`).
RECORD_LENGTH = 10000
LONGEST_RECORD = 1048576

#: Seconds to wait for the engine to start, stop, log an event or reach a state.
PATIENCE = 20.0

#: How many of its newest log events the `engine` fixture keeps: far more than a test reads or
#: waits on, and bounded however long the engine runs (the soak logs two per connection, #65).
KEPT_EVENTS = 1000

#: Runs a test over the raw socket only, where the other transport adds nothing to what it
#: checks and would only slow it.
SOCKET_ONLY = pytest.mark.parametrize("transport", ["socket"], indirect=True)

#: The channel `start_index_clock` turns into a clock, and the clock's frequency in hertz: one
#: 2 Vpp ramp a second, so it rises 2 uV per sample index at 1 MS/s, about 17 times float32's
#: resolution at 1 V.
CLOCK_CHANNEL = 4
CLOCK_FREQUENCY = 1.0

#: Seconds to let pass for the samples after a command to be taken: far more than the few
#: milliseconds a restarted trigger takes to see the signal and fill its pre-trigger samples at
#: 1 MS/s, or a short fault takes to end.
SETTLE = 0.1


def eventually(check: Callable[[], bool]) -> None:
    """Calls `check` until it returns True, 10 ms apart; fails the test after PATIENCE seconds."""
    deadline = time.monotonic() + PATIENCE
    while not check():
        assert time.monotonic() < deadline, "not within the patience"
        time.sleep(0.01)


def resident_kib(pid: int) -> int:
    """The resident memory of process `pid` in KiB, from /proc (Linux only)."""
    with open(f"/proc/{pid}/status") as status:
        for line in status:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    raise AssertionError(f"no VmRSS line in /proc/{pid}/status")


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


def read_channel(scope: OpenScope, channel: int) -> WaveformData:
    """Channel `channel` of the latest capture. It sets this Session's `WAV:SOUR`."""
    scope.waveform.source = channel
    return scope.waveform.read()


def take_fault_capture(
    scope: OpenScope, kind: FaultKind, magnitude: float | None = None, duration: int | None = None
) -> None:
    """Arms a Single, waits until the trigger is ready, injects a fault into Generator 1 and
    waits for the capture a trigger event on the fault places (in Normal mode, at a level only
    the fault reaches). A fault starts at the next sample index; until the restarted trigger has
    seen the signal past its band and holds its pre-trigger samples, it would pass unseen."""
    scope.single()
    time.sleep(SETTLE)
    scope.generator(1).inject_fault(kind, magnitude, duration)
    scope.wait_complete()


def start_index_clock(scope: OpenScope) -> None:
    """Turns Generator 4 into a clock on the sample index and enables its channel: the time is
    the sample index (ADR 0003), which a test over TCP cannot read, but a ramp's value at a sample
    places it. Read it with `indexes_between`."""
    scope.channel(CLOCK_CHANNEL).enabled = True
    Signal(Function.RAMP, CLOCK_FREQUENCY).apply(scope, CLOCK_CHANNEL)


def clock_at_time_zero(scope: OpenScope) -> float:
    """The index clock's value at time zero of the latest capture. It sets this Session's
    `WAV:SOUR` to the clock's channel."""
    waveform = read_channel(scope, CLOCK_CHANNEL)
    return waveform.samples[waveform.preamble.time_zero]


def indexes_between(earlier: float, later: float) -> int:
    """How many sample indexes after the clock read `earlier` it reads `later` at the `*RST`
    sample rate: under one ramp (one second), since the ramp drops back once a cycle."""
    return round((later - earlier) / 2 % 1 * SAMPLE_RATE / CLOCK_FREQUENCY)
