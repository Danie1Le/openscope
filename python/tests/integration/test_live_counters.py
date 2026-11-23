"""What live mode has lost (spec story 41, NFR-1): the engine logs its dropped blocks, skipped
samples and discarded captures as a `live` event after each second in which they changed, and in
its `stopping` event."""

from datetime import datetime
from typing import Any

from engine_helpers import LIVE_COUNTERS


def logged_at(event: dict[str, Any]) -> datetime:
    return datetime.fromisoformat(event["time"])


def counters(event: dict[str, Any]) -> list[int]:
    return [event[name] for name in LIVE_COUNTERS]


def test_the_stopping_event_reports_the_counters(engine):
    engine.stop()

    stopping = counters(engine.last("stopping"))
    assert all(isinstance(count, int) and count >= 0 for count in stopping)


def test_skipped_samples_are_logged_at_most_once_a_second_and_when_stopping(engine):
    # Far more samples a second than acquisition keeps up with: it skips some every round.
    assert engine.raw_session().query("ACQ:SRAT 1E9;SRAT?") == "1E+09"
    first = engine.wait_for("live")
    while first["skipped_samples"] == 0:  # Lost before the setter, if this machine is slow.
        first = engine.wait_for("live")
    second = engine.wait_for("live")
    engine.stop()

    assert second["skipped_samples"] > first["skipped_samples"]
    assert (logged_at(second) - logged_at(first)).total_seconds() >= 0.99  # ms in the log.
    stopping = engine.last("stopping")
    assert all(now >= then for now, then in zip(counters(stopping), counters(second), strict=True))
