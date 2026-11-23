"""What live mode has lost (spec story 41, NFR-1): the engine logs its dropped blocks, skipped
samples and discarded captures as a `live` event after each second in which they changed, and in
its `stopping` event."""

import time
from datetime import datetime
from typing import Any

from engine_helpers import live_counters


def logged_at(event: dict[str, Any]) -> datetime:
    return datetime.fromisoformat(event["time"])


def test_nothing_is_logged_while_nothing_is_lost(engine):
    # A thousand samples a second: nothing to drop or skip on any machine.
    assert engine.raw_session().query("ACQ:SRAT 1E3;SRAT?") == "1000"
    time.sleep(1.5)  # A loss before the setter, on a slow machine, is logged by now.
    logged = len([event for event in engine.events if event["event"] == "live"])

    time.sleep(2.5)

    assert len([event for event in engine.events if event["event"] == "live"]) == logged


def test_skipped_samples_are_logged_at_most_once_a_second_and_when_stopping(engine):
    # Far more samples a second than acquisition keeps up with: it skips some every round.
    assert engine.raw_session().query("ACQ:SRAT 1E9;SRAT?") == "1E+09"
    first = engine.wait_for("live")
    while first["skipped_samples"] == 0:  # A loss before the setter, on a slow machine.
        first = engine.wait_for("live")
    second = engine.wait_for("live")
    # A round of acquisition may take over a second on a slow machine, and skipped samples are
    # counted once a round: wait for the next count.
    later = second
    while later["skipped_samples"] == first["skipped_samples"]:
        later = engine.wait_for("live")
    engine.stop()

    assert (logged_at(second) - logged_at(first)).total_seconds() >= 0.99  # ms in the log.
    assert later["skipped_samples"] > first["skipped_samples"]
    at_stop = live_counters(engine.last("stopping"))  # The run's totals, never less.
    assert all(isinstance(count, int) for count in at_stop.values())
    assert all(at_stop[name] >= count for name, count in live_counters(later).items())
