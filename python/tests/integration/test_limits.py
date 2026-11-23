"""The server's protection limits: 4 KB lines, 64 MiB replies, 8 clients and the idle timeout
(docs/scpi-reference.md, Connecting and Lines)."""

import threading
import time
from collections.abc import Callable

import pytest
from engine_helpers import LONGEST_RECORD, PATIENCE
from server_limits import MAX_CLIENTS, MAX_LINE, TOO_MUCH_DATA

from openscope_client import ErrorEntry, ScpiError, TriggerMode
from openscope_client.transport import MAX_RESPONSE_BYTES

#: How long another client's `*IDN?` may take while a line of bulk queries runs: generous for a
#: loaded runner and an ASan engine, yet far below the minute that transforming a 1M-sample
#: capture for every `FFT:DATA?` a line holds would take.
OTHER_CLIENT_CEILING = 10.0

#: How long another client's `*IDN?` may take while a line formats bulk data: lines of different
#: clients run in parallel, so it waits for no bulk query (a few ms locally); generous for a loaded
#: runner and an ASan engine.
UNSTALLED_CEILING = 0.25


def test_a_line_of_4096_bytes_runs(scope):
    line = "*IDN?".ljust(MAX_LINE)  # Trailing white space is allowed.

    assert scope.query(line).startswith("OpenScope,")


@pytest.mark.parametrize("length", [MAX_LINE + 1, 1_000_000])
def test_a_longer_line_is_rejected_and_the_session_carries_on(scope, length):
    with pytest.raises(ScpiError) as raised:
        scope.query("*IDN?".ljust(length))

    assert raised.value.errors == (ErrorEntry(-363, "Input buffer overrun"),)
    assert raised.value.reply is None
    assert scope.identify().manufacturer == "OpenScope"


def bulk_line(first: str) -> str:
    """`first`, a line ending in `DATA?`, then as many `;DATA?` as fit in a line."""
    return first + ";DATA?" * ((MAX_LINE - len(first)) // len(";DATA?"))


def waits_while(other: Callable[[str], str], line: Callable[[], object]) -> list[float]:
    """Runs `line`, which sends a line on one Session and reads its reply, while `other`, the query
    of another Session, asks `*IDN?` every 5 ms on its own thread; returns how long each `*IDN?`
    asked while the line ran waited for its reply."""
    asked: list[tuple[float, float]] = []  # When each *IDN? was sent, and its wait.
    failures: list[BaseException] = []
    done = threading.Event()

    def ask() -> None:
        try:
            while not done.is_set():
                sent = time.monotonic()
                assert other("*IDN?").startswith("OpenScope,")
                asked.append((sent, time.monotonic() - sent))
                time.sleep(0.005)
        except BaseException as error:  # Checked in the test's thread.
            failures.append(error)

    asking = threading.Thread(target=ask)
    asking.start()
    try:
        started = time.monotonic()
        line()
        ended = time.monotonic()
    finally:
        done.set()
        asking.join(PATIENCE)
    assert not asking.is_alive()
    assert failures == []
    waits = [wait for sent, wait in asked if started <= sent < ended]
    assert waits  # Asked while the line ran.
    return waits


@pytest.fixture
def bulk(engine):
    """A raw Session of `engine` that has taken a Single of the longest record."""
    session = engine.raw_session()
    assert session.query(f"ACQ:POIN {LONGEST_RECORD};:SING;*OPC?") == "1"
    return session


def test_a_line_of_bulk_queries_stops_at_the_reply_limit_and_others_are_served(engine, bulk):
    levels = 4 * (LONGEST_RECORD // 2 + 1)  # float32 bytes of the spectrum: 2 MiB.
    spectrum = len(f"#{len(str(levels))}{levels}") + levels  # As one IEEE 488.2 block.
    line = bulk_line("FFT:DATA?")
    replies: list[bytes] = []

    waits = waits_while(engine.raw_session().query, lambda: replies.append(bulk.query_bytes(line)))

    fitting = (MAX_RESPONSE_BYTES + 1) // (spectrum + 1)  # Blocks joined with ';' that fit.
    assert fitting < line.count("DATA?")
    assert len(replies[0]) == fitting * (spectrum + 1) - 1
    assert bulk.next_error() == TOO_MUCH_DATA
    assert max(waits) < OTHER_CLIENT_CEILING


def test_another_clients_commands_do_not_wait_for_a_bulk_line_to_be_formatted(engine, bulk):
    # About 3.5 ASCII blocks of 16 MiB fit the reply: 0.6 s of formatting, which no other client
    # waits for, since lines of different Sessions run in parallel.
    waits = waits_while(
        engine.raw_session().query, lambda: bulk.query_bytes(bulk_line("WAV:FORM ASC;DATA?"))
    )

    assert bulk.next_error() == TOO_MUCH_DATA
    assert max(waits) < UNSTALLED_CEILING


def test_a_ninth_client_is_refused_until_one_leaves(engine, open_client, transport):
    clients = [open_client(engine.port) for _ in range(MAX_CLIENTS)]
    for client in clients:  # Each has a Session: a reply proves it was admitted.
        client.identify()

    refused = open_client(engine.port)
    refused.timeout = 0.5
    engine.wait_for("refuse", reason="too many clients")
    with pytest.raises(transport.connection_lost):
        refused.identify()

    clients[0].close()
    engine.wait_for("disconnect", reason="closed by client")
    assert open_client(engine.port).identify().manufacturer == "OpenScope"
    assert all(client.identify() for client in clients[1:])


@pytest.fixture
def idle_engine(start_engine):
    """An engine that closes a connection after 1 second with no command and no reply."""
    return start_engine("--idle-timeout", "1")


def test_an_idle_client_is_disconnected(idle_engine, open_client, transport):
    scope = open_client(idle_engine.port)
    # Stamped before the command is sent: the server's idle timer starts from the reply, later
    # still, so no delay on either side can make the measured interval shorter than the timeout.
    sent = time.monotonic()
    scope.identify()

    idle_engine.wait_for("disconnect", reason="idle timeout")

    assert 1 <= time.monotonic() - sent < 5  # Generous: a loaded CI runner is slow.
    with pytest.raises(transport.connection_lost):
        scope.identify()


def test_commands_keep_a_client_from_being_idle(idle_engine, open_client):
    scope = open_client(idle_engine.port)

    for _ in range(8):  # 2 seconds, twice the idle timeout, never more than 0.25 s apart.
        scope.identify()
        time.sleep(0.25)

    assert [event for event in idle_engine.events if event["event"] == "disconnect"] == []


def test_a_client_waiting_on_opc_is_never_idle(idle_engine, open_client):
    waiting = open_client(idle_engine.port)
    waiting.trigger.mode = TriggerMode.NORMAL
    waiting.trigger.level = 100  # The 2 Vpp sine never reaches it: the Single stays armed.
    waiting.single()
    waiting.timeout = 30
    failures: list[BaseException] = []

    def wait() -> None:
        try:
            waiting.wait_complete()
        except BaseException as error:  # Checked in the test's thread.
            failures.append(error)

    waiter = threading.Thread(target=wait)
    waiter.start()
    time.sleep(2.5)  # Over twice the idle timeout.
    assert waiter.is_alive()

    assert idle_engine.raw_session().query("STOP;*OPC?") == "1"  # Ends the wait.
    waiter.join(30)
    assert not waiter.is_alive()

    assert failures == []
    assert waiting.identify().manufacturer == "OpenScope"
    assert [event for event in idle_engine.events if event["event"] == "disconnect"] == []
