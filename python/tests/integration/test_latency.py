"""SCPI latency over TCP, the spec's NFR-4 (setter p95 under 5 ms, a measurement query on a
1M-sample record p95 under 20 ms): each test times 10,000 commands and reports the percentiles.
The numbers on a fixed machine are in docs/benchmarks.md; here the tests assert only ceilings
generous enough for a shared CI runner and a sanitizer build. Run them alone, with the numbers
printed, with `pytest -m latency -s`. Over the raw socket only: PyVISA adds its own costs."""

import functools
import itertools
import statistics
import time
from collections.abc import Callable, Iterable
from typing import NamedTuple

import pytest
from engine_helpers import LONGEST_RECORD, NOT_A_NUMBER, SOCKET_ONLY, take_capture

from openscope_client import Measurement

pytestmark = [pytest.mark.latency, SOCKET_ONLY]

#: Commands each test times.
COMMANDS = 10_000
#: Fresh captures whose first query of every measurement is timed.
FRESH_CAPTURES = 3


class Latency(NamedTuple):
    """Percentiles of a set of round-trip times, in milliseconds."""

    median: float
    p95: float
    p99: float
    worst: float


def timed(commands: Iterable[Callable[[], object]]) -> list[float]:
    """The seconds each of `commands` took, run one after another."""
    times = []
    for command in commands:
        start = time.perf_counter()
        command()
        times.append(time.perf_counter() - start)
    return times


def report(record: Callable[[str, object], None], name: str, times: list[float]) -> Latency:
    """The percentiles of `times`, printed and recorded as `name` (by `record_testsuite_property`,
    in the JUnit XML)."""
    cuts = statistics.quantiles(times, n=100, method="inclusive")
    latency = Latency(*(1e3 * value for value in (cuts[49], cuts[94], cuts[98], max(times))))
    summary = ", ".join(f"{field} {value:.3f} ms" for field, value in latency._asdict().items())
    print(f"\n{name} over {len(times)}: {summary}")
    record(name, summary)
    return latency


def test_setter_latency(scope, record_testsuite_property):
    # A setter and the client's error check (`SYST:ERR?`, sent with it): one round trip, as a
    # script sees it. SOUR1:FREQ also drops the capture in progress, as every setting does.
    setter = functools.partial(scope.write, "SOUR1:FREQ 1000")

    latency = report(record_testsuite_property, "setter", timed(itertools.repeat(setter, COMMANDS)))

    assert latency.p95 < 50  # NFR-4's target is 5 ms; this ceiling only catches a hang.


@pytest.fixture
def measurements(engine, scope):
    """One query of each measurement of channel 1, each alone on its line with no error check
    behind it (the client's pipelined check waits for a delayed acknowledgement until #58 is
    fixed), on an engine Stopped after one capture of the longest record."""
    scope.acquisition.record_length = LONGEST_RECORD
    scope.timeout = 60
    take_capture(scope)
    session = engine.raw_session()
    return [functools.partial(session.query, f"MEAS:{m}? CHAN1") for m in Measurement]


def test_measurement_latency_on_the_longest_record(measurements, record_testsuite_property):
    # Results are cached per capture: after this first round every query reads the cache. The
    # first queries are timed on their own below.
    assert all(float(query()) != NOT_A_NUMBER for query in measurements)

    queries = itertools.islice(itertools.cycle(measurements), COMMANDS)
    latency = report(record_testsuite_property, "measurement_1M", timed(queries))

    assert latency.p95 < 100


def test_first_measurement_of_a_fresh_longest_record(
    measurements, scope, record_testsuite_property
):
    times = []
    for _ in range(FRESH_CAPTURES):
        take_capture(scope)
        times += timed(measurements)

    latency = report(record_testsuite_property, "first_measurement_1M", times)

    assert latency.p95 < 2000


@pytest.mark.xfail(
    strict=True, raises=AssertionError, reason="#58: a pipelined reply waits for a delayed ACK"
)
def test_pipelined_replies_are_not_delayed(scope):
    # The client sends `SYST:ERR?` behind every query: two replies to one write.
    times = timed(itertools.repeat(scope.identify, 50))

    assert statistics.median(times) < 0.020  # NFR-4's measurement target, 20 ms.
