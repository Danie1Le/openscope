"""Run control over TCP: `SINGle;*OPC?` takes one capture that every later query reads, and
`STOP`, `RUN` or `*RST` from another Session end an `*OPC?` wait (docs/scpi-reference.md, Run
control and Consistency model)."""

import socket
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
from engine_helpers import (
    BULK_PATIENCE,
    LONGEST_RECORD,
    PATIENCE,
    eventually,
    plain_query,
    plain_reply,
    take_capture,
)

from openscope_client import Measurement

#: Seconds a waiting Session is given to show that it still waits.
WAIT = 0.2


def read_twice(scope):
    """The latest capture's channel 1 and its RMS, read twice, `WAIT` seconds apart."""
    first = (scope.waveform.read(), scope.measure(Measurement.RMS, 1))
    time.sleep(WAIT)
    return [first, (scope.waveform.read(), scope.measure(Measurement.RMS, 1))]


def test_queries_after_single_and_opc_all_read_the_singles_capture(scope):
    scope.generator(1).noise = 0.1  # Every capture differs.
    take_capture(scope)

    first, second = read_twice(scope)

    assert first == second


def test_while_running_each_query_reads_the_latest_capture(scope):
    scope.generator(1).noise = 0.1
    take_capture(scope)
    scope.run()
    first = scope.waveform.read()

    # A capture completes every 15 ms at the *RST settings.
    eventually(lambda: scope.waveform.read() != first)


def test_stop_freezes_the_latest_capture(scope):
    scope.generator(1).noise = 0.1
    take_capture(scope)
    scope.run()
    time.sleep(WAIT)

    scope.stop()

    first, second = read_twice(scope)
    assert first == second


def test_commands_after_opc_on_its_line_read_the_singles_capture(scope):
    scope.write("TRIG:POS 25")

    reply = scope.query("SING;*OPC?;:WAV:PRE?")

    assert reply == "1;1E+06,10000,1,0,2500,0"


@pytest.fixture
def waiting(engine, open_client):
    """A client waiting on `*OPC?` for a Single that no trigger event will end (Normal mode, a
    level the sine never reaches), and a second client of the same engine."""
    scope, other = open_client(engine.port), open_client(engine.port)
    scope.write("*RST;:TRIG:MODE NORM;LEV 1.5")
    scope.single()
    scope.timeout = 30
    with ThreadPoolExecutor(1) as pool:
        wait = pool.submit(scope.wait_complete)
        time.sleep(WAIT)
        assert not wait.done()
        yield wait, scope, other


@pytest.mark.parametrize("line", ["STOP", "RUN", "*RST"])
def test_another_session_ends_an_opc_wait(waiting, line):
    wait, scope, other = waiting

    other.write(line)

    wait.result(timeout=30)  # Replied 1.
    assert scope.identify().manufacturer == "OpenScope"  # The Session carries on.


def test_a_stop_ends_the_wait_even_if_another_client_arms_a_single_before_its_line_ends(
    engine, open_client
):
    # The STOP itself ends A's wait (#68 review): a SINGle from C, sent while the rest of B's
    # line (bulk ASCII) still runs, waits for A instead of arming a Single A would wait on again.
    with socket.create_connection(("127.0.0.1", engine.port), timeout=BULK_PATIENCE) as b:
        assert plain_query(b, f"ACQ:POIN {LONGEST_RECORD};:SING;*OPC?") == b"1"
        assert plain_query(b, "TRIG:MODE NORM;LEV 1.5;:SYST:ERR?") == b'0,"No error"'
        a, c = open_client(engine.port), open_client(engine.port)
        a.single()
        a.timeout = PATIENCE
        with ThreadPoolExecutor(1) as pool:
            wait = pool.submit(a.wait_complete)
            time.sleep(WAIT)
            assert not wait.done()

            b.sendall(b"STOP;:WAV:FORM ASC;DATA?;DATA?;DATA?\n")  # About 0.6 s of formatting.
            time.sleep(0.05)
            c.single()

            wait.result(timeout=PATIENCE)  # Replied 1.
        assert plain_reply(b).count(b";") == 2  # B's three blocks.


def test_wai_holds_the_line_until_the_single_completes(waiting):
    wait, scope, other = waiting
    other.write("STOP")
    wait.result(timeout=30)
    scope.single()

    with ThreadPoolExecutor(1) as pool:
        held = pool.submit(scope.query, "*WAI;:WAV:PRE?")
        time.sleep(WAIT)
        assert not held.done()
        other.trigger.level = 0.5  # The armed Single stays armed and now triggers.
        preamble = held.result(timeout=30)

    assert preamble.endswith(",5000,0")  # Triggered, at the 50% position.
