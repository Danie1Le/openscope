"""The long stability test (spec story 79): the engine runs live for OPENSCOPE_SOAK_SECONDS
(default 600) while several clients use every part of it at once, and must keep measuring right,
keep its memory flat and exit cleanly. It runs only on demand: `pytest --soak -m soak`, or the
Soak workflow (workflow_dispatch) in CI."""

import functools
import os
import threading
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import pytest
from engine_helpers import RECORD_LENGTH, SOCKET_ONLY, take_capture
from ground_truth import Signal, assert_measures

from openscope_client import FaultKind, Function, Measurement, OpenScope, ScpiError

pytestmark = [pytest.mark.soak, SOCKET_ONLY]

SECONDS = float(os.environ.get("OPENSCOPE_SOAK_SECONDS", "600"))
#: Seconds between two memory readings; the first, after the clients have warmed up, is the
#: baseline.
MEMORY_INTERVAL = 5.0
#: How much the engine's resident memory may grow after the baseline. A release engine stays
#: near 5 MiB under this load, so this much more is growth, not noise.
MEMORY_GROWTH_KIB = 32 * 1024
#: The levels `FFT:DATA?` replies for the `*RST` record, whose first 8192 samples it takes.
SPECTRUM_LENGTH = 8192 // 2 + 1

#: The signal of each channel; channel 1's is measured against its ground truth.
SIGNALS = (
    Signal(Function.SINE).with_noise(),
    Signal(Function.SQUARE, 2.5e3),
    Signal(Function.TRIANGLE, 500.0),
    Signal(Function.RAMP, 5e3),
)


def rounds(step: Callable[[int], object], until: float, failed: threading.Event) -> int:
    """Runs `step(round)` over and over until `until` (time.monotonic()) or until a worker has
    failed; returns the count. A step that raises sets `failed`, so every worker stops within a
    round."""
    count = 0
    while not failed.is_set() and time.monotonic() < until:
        try:
            step(count)
        except BaseException:
            failed.set()
            raise
        count += 1
    return count


def take_single(scope: OpenScope, _: int) -> None:
    """Takes a Single and checks it: statistics of exactly that capture, channel 1's frequency
    against its Generator (NFR-6), channel 2's whole record (the Session reads channel 2)."""
    reply = scope.query("MEAS:STAT:RES;:SING;*OPC?;:MEAS:STAT? FREQ,CHAN1")
    assert reply.startswith("1;"), reply
    assert reply.endswith(",1"), reply  # A count of one.
    assert_measures(scope, SIGNALS[0], Measurement.FREQUENCY)
    assert len(scope.waveform.read().samples) == RECORD_LENGTH


def read_latest(scope: OpenScope, _: int) -> None:
    """Reads the latest capture as it comes: a waveform, a spectrum and a measurement (the
    Session reads channel 3's waveform and channel 4's spectrum)."""
    assert len(scope.waveform.read().samples) == RECORD_LENGTH
    assert len(scope.fft.spectrum()) == SPECTRUM_LENGTH
    assert scope.measure(Measurement.PEAK_TO_PEAK, 2) == SIGNALS[1].approx(Measurement.PEAK_TO_PEAK)


def change_settings(scope: OpenScope, round_number: int) -> None:
    """Changes a setting that drops the capture in progress (channel 3's frequency) and one that
    does not (the timebase), and injects a fault on channel 4."""
    scope.generator(3).frequency = 500.0 if round_number % 2 else 750.0
    scope.timebase.scale = 1e-3 if round_number % 2 else 2e-3
    scope.generator(4).inject_fault(FaultKind.GLITCH, 0.5, 10)
    time.sleep(0.05)


def churn(port: int, _: int) -> None:
    """Connects, makes a mistake, reads it, and disconnects."""
    with OpenScope.connect("127.0.0.1", port, 10.0) as scope:
        assert scope.identify().manufacturer == "OpenScope"
        with pytest.raises(ScpiError, match="Undefined header"):
            scope.write("SYST:BOGUS")


def test_the_engine_holds_up_under_load(engine, open_client):
    scope = open_client(engine.port)
    for channel, signal in enumerate(SIGNALS, start=1):
        scope.channel(channel).enabled = True
        signal.apply(scope, channel)
    take_capture(scope)  # Every reader finds a capture from the start.
    scope.waveform.source = 2
    reading, changing = open_client(engine.port), open_client(engine.port)
    reading.waveform.source, reading.fft.source = 3, 4
    steps = [
        functools.partial(take_single, scope),
        functools.partial(read_latest, reading),
        functools.partial(change_settings, changing),
        functools.partial(churn, engine.port),
    ]

    until = time.monotonic() + SECONDS
    failed = threading.Event()
    memory: list[int] = []
    with ThreadPoolExecutor(len(steps)) as pool:
        running = [pool.submit(rounds, step, until, failed) for step in steps]
        while not all(work.done() for work in running):
            if failed.wait(MEMORY_INTERVAL if time.monotonic() < until else 0.1):
                break  # The other workers stop within a round.
            memory.append(engine.resident_kib())
        counts = [work.result() for work in running]  # Raises what a worker raised.

    print(f"\nRounds per worker {counts}; resident KiB {memory[0]} warmed up, {max(memory)} max")
    assert min(counts) > 0, counts
    assert max(memory) - memory[0] < MEMORY_GROWTH_KIB, memory
    assert scope.identify().manufacturer == "OpenScope"
