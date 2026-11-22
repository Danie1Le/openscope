"""The long stability test (spec story 79): the engine runs live for OPENSCOPE_SOAK_SECONDS
(default 600) while several clients use every part of it at once, and must keep measuring right,
keep its memory flat and exit cleanly; the test's own memory must stay flat too. It runs only on
demand: `pytest --soak -m soak`, or the Soak workflow (workflow_dispatch) in CI."""

import functools
import os
import threading
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import pytest
from engine_helpers import RECORD_LENGTH, SOCKET_ONLY, resident_kib, take_capture
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
#: How much the test's own (pytest's) resident memory may grow after the baseline. It keeps a
#: bounded engine log, so this much more is a leak in the tests: an unbounded log grew it by up
#: to 3.4 MiB a second, past a hosted runner's 7 GB within the hour (#65).
TEST_MEMORY_GROWTH_KIB = 32 * 1024
#: Seconds the churn worker pauses after each round: at most 100 connections a second. Unpaused,
#: it made up to 2,600 a second, and the sockets each leaves in TIME_WAIT for a minute took every
#: ephemeral port connect() picks from (#65).
CHURN_PAUSE = 0.01
#: The levels `FFT:DATA?` replies for the `*RST` record, whose first 8192 samples it takes.
SPECTRUM_LENGTH = 8192 // 2 + 1

#: The signal of each channel; channel 1's is measured against its ground truth.
SIGNALS = (
    Signal(Function.SINE).with_noise(),
    Signal(Function.SQUARE, 2.5e3),
    Signal(Function.TRIANGLE, 500.0),
    Signal(Function.RAMP, 5e3),
)


def rounds(step: Callable[[int], object], until: float, stop: threading.Event) -> int:
    """Runs `step(round)` over and over until `until` (time.monotonic()) or until `stop` is set;
    returns the count. A step that raises sets `stop`, so every worker stops within a round."""
    count = 0
    while not stop.is_set() and time.monotonic() < until:
        try:
            step(count)
        except BaseException:
            stop.set()
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
    """Connects, makes a mistake, reads it, disconnects, and pauses."""
    with OpenScope.connect("127.0.0.1", port, 10.0) as scope:
        assert scope.identify().manufacturer == "OpenScope"
        with pytest.raises(ScpiError, match="Undefined header"):
            scope.write("SYST:BOGUS")
    time.sleep(CHURN_PAUSE)


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
    stop = threading.Event()
    engine_memory: list[int] = []
    test_memory: list[int] = []
    with ThreadPoolExecutor(len(steps)) as pool:
        running = [pool.submit(rounds, step, until, stop) for step in steps]
        try:
            while not all(work.done() for work in running):
                if stop.wait(MEMORY_INTERVAL if time.monotonic() < until else 0.1):
                    break  # A worker failed; the others stop within a round.
                engine_memory.append(engine.resident_kib())
                test_memory.append(resident_kib(os.getpid()))
        finally:
            stop.set()  # Also when interrupted (the Soak workflow's timeout sends SIGINT).
        counts = [work.result() for work in running]  # Raises what a worker raised.

    print(
        f"\nRounds per worker {counts}; resident KiB warmed up and max: "
        f"engine {engine_memory[0]} {max(engine_memory)}, test {test_memory[0]} {max(test_memory)}"
    )
    assert min(counts) > 0, counts
    assert max(engine_memory) - engine_memory[0] < MEMORY_GROWTH_KIB, engine_memory
    assert max(test_memory) - test_memory[0] < TEST_MEMORY_GROWTH_KIB, test_memory
    assert scope.identify().manufacturer == "OpenScope"
