"""The log events the `engine` fixture keeps (conftest.Engine), and the memory reading the soak
relies on (engine_helpers.resident_kib)."""

import os
import socket
import subprocess

import pytest
from engine_helpers import KEPT_EVENTS, resident_kib


def connect_and_leave(port: int) -> str:
    """Connects to the engine and closes the connection; returns the peer the engine logs."""
    with socket.create_connection(("127.0.0.1", port)) as connection:
        host, client_port = connection.getsockname()
    return f"{host}:{client_port}"


def test_the_engine_keeps_its_newest_events_and_waits_past_the_dropped_ones(engine):
    peers = []
    for _ in range(KEPT_EVENTS):  # Two events each: twice as many as it keeps.
        peers.append(connect_and_leave(engine.port))
        assert engine.wait_for("disconnect", reason="closed by client")["peer"] == peers[-1]

    # A `live` event, logged should the engine lose samples meanwhile, takes an older one's place.
    events = [event for event in engine.events if event["event"] != "live"]
    events = events[len(events) % 2 :]  # Whole connect and disconnect pairs.
    assert len(events) > KEPT_EVENTS - 10
    newest_peers = peers[-(len(events) // 2) :]
    assert [event["event"] for event in events] == ["connect", "disconnect"] * len(newest_peers)
    assert [event["peer"] for event in events] == [peer for peer in newest_peers for _ in range(2)]


def test_a_wait_fails_when_events_it_has_not_looked_at_were_dropped(engine):
    for _ in range(KEPT_EVENTS):
        connect_and_leave(engine.port)

    with pytest.raises(AssertionError, match=r"events dropped before a wait for refuse"):
        engine.wait_for("refuse", reason="never logged")


def test_last_is_the_newest_kept_event_of_its_name(engine):
    peers = [connect_and_leave(engine.port) for _ in range(2)]
    for peer in peers:
        engine.wait_for("disconnect", peer=peer)

    assert engine.last("disconnect")["peer"] == peers[1]
    assert engine.last("listening")["port"] == engine.port
    with pytest.raises(AssertionError, match="no refuse event"):
        engine.last("refuse")


def test_resident_kib_reads_a_process_resident_memory():
    before = resident_kib(os.getpid())
    held = b"x" * (64 * 1024 * 1024)

    assert resident_kib(os.getpid()) - before >= 60 * 1024
    del held


def test_resident_kib_fails_for_a_process_with_no_memory():
    child = subprocess.Popen(["true"])
    os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOWAIT)  # Exited, not reaped: a zombie.
    try:
        with pytest.raises(AssertionError, match=f"no VmRSS line in /proc/{child.pid}/status"):
            resident_kib(child.pid)
    finally:
        child.wait()
