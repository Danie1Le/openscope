"""Runs the real engine for each test: `openscope-engine` at the path in OPENSCOPE_ENGINE_PATH,
listening on a free loopback port, stopped and checked for a clean exit after the test."""

import json
import os
import signal
import subprocess
import threading
import time
from collections import deque
from collections.abc import Callable, Iterator
from typing import Any

import pytest
from engine_helpers import KEPT_EVENTS, PATIENCE

from openscope_client import ErrorEntry, OpenScope, SocketStream
from openscope_client.errors import parse_error_entry

#: How many of the newest events a failure message shows; a sanitizer's report comes last.
REPORTED_EVENTS = 100


class Engine:
    """A running `openscope-engine` and the newest events it has logged: its JSON log lines,
    and any other line it writes to standard error (a sanitizer's report) as an `output` event."""

    def __init__(self, path: str, options: tuple[str, ...]) -> None:
        """Starts the engine; `listen()` waits until it accepts connections."""
        self._process = subprocess.Popen(
            [path, "--port", "0", *options],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        self._events: deque[dict[str, Any]] = deque(maxlen=KEPT_EVENTS)
        #: Where in `_events` `wait_for` looks from, past the events it has passed; negative when
        #: events it had not looked at were dropped.
        self._seen = 0
        self._raw_sessions: list[RawSession] = []
        self._logged = threading.Condition()
        self._reader = threading.Thread(target=self._read_log, daemon=True)
        self._reader.start()
        #: The port it listens on, once `listen()` has returned.
        self.port = 0

    def listen(self) -> None:
        """Waits for the `listening` event, which names the port."""
        self.port = self.wait_for("listening")["port"]

    @property
    def events(self) -> list[dict[str, Any]]:
        """The newest KEPT_EVENTS events, oldest first: a copy, safe to read while more come."""
        with self._logged:
            return list(self._events)

    def _read_log(self) -> None:
        assert self._process.stderr is not None
        for line in self._process.stderr:
            try:
                event = json.loads(line)
            except ValueError:
                event = {"event": "output", "text": line.decode(errors="replace")}
            with self._logged:
                if len(self._events) == self._events.maxlen:  # The append drops the oldest.
                    self._seen -= 1
                self._events.append(event)
                self._logged.notify_all()
        with self._logged:  # The engine has exited: wake waiters to see it.
            self._logged.notify_all()

    def wait_for(self, event: str, **fields: Any) -> dict[str, Any]:
        """The next event named `event` with `fields` among those logged after the one the last
        call returned, waiting for it to be logged. Fails if events it had not looked at have
        been dropped from the history: the one it waits for could be among them."""
        deadline = time.monotonic() + PATIENCE
        with self._logged:
            while True:
                if self._seen < 0:
                    raise AssertionError(
                        f"{-self._seen} events dropped before a wait for {event} looked at them; "
                        f"the newest: {self.newest()}"
                    )
                for index in range(self._seen, len(self._events)):
                    logged = self._events[index]
                    if logged["event"] == event and fields.items() <= logged.items():
                        self._seen = index + 1
                        return logged
                left = deadline - time.monotonic()
                if left <= 0 or not self._reader.is_alive():
                    raise AssertionError(f"no {event} event with {fields} in {self.newest()}")
                self._logged.wait(left)

    def last(self, event: str) -> dict[str, Any]:
        """The newest event named `event` that is kept; fails if there is none."""
        for logged in reversed(self.events):
            if logged["event"] == event:
                return logged
        raise AssertionError(f"no {event} event in {self.newest()}")

    def newest(self) -> list[dict[str, Any]]:
        """The newest REPORTED_EVENTS events, for a failure message."""
        return self.events[-REPORTED_EVENTS:]

    @property
    def pid(self) -> int:
        """The engine's process ID."""
        return self._process.pid

    def raw_session(self) -> "RawSession":
        """A Session over a raw socket, closed when the engine stops."""
        session = RawSession(self.port)
        self._raw_sessions.append(session)
        return session

    def stop(self) -> int:
        """Stops the engine with SIGTERM, or kills it when it has not exited after PATIENCE
        seconds, and returns its exit status (negative when killed)."""
        for session in self._raw_sessions:
            session.close()
        self._process.send_signal(signal.SIGTERM)
        try:
            self._process.wait(PATIENCE)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
        self._reader.join(PATIENCE)  # It ends at the end of the log.
        assert self._process.stderr is not None
        self._process.stderr.close()
        return self._process.returncode


class RawSession:
    """A Session over a raw socket with no error checks, for lines whose errors a test reads
    itself."""

    def __init__(self, port: int) -> None:
        self._stream = SocketStream("127.0.0.1", port, PATIENCE)

    def query(self, line: str) -> str:
        """Sends `line` and returns its reply."""
        return self.query_bytes(line).decode()

    def query_bytes(self, line: str) -> bytes:
        """Sends `line` and returns its reply as bytes, for replies that hold binary blocks."""
        self._stream.write(f"{line}\n".encode())
        return self._stream.read_response()

    def next_error(self) -> ErrorEntry | None:
        """The oldest entry of the Session's error queue (None if the reply is not one)."""
        return parse_error_entry(self.query("SYST:ERR?").encode())

    def close(self) -> None:
        self._stream.close()


@pytest.fixture
def start_engine() -> Iterator[Callable[..., Engine]]:
    """Starts engines with extra command-line options; after the test, stops each and checks it
    exits with status 0 (a sanitizer's report makes it non-zero)."""
    path = os.environ.get("OPENSCOPE_ENGINE_PATH")
    if not path:
        pytest.fail("set OPENSCOPE_ENGINE_PATH to the openscope-engine binary to run these tests")
    engines: list[Engine] = []

    def start(*options: str) -> Engine:
        engines.append(Engine(path, options))  # Before waiting, so a failed start is stopped too.
        engines[-1].listen()
        return engines[-1]

    yield start
    statuses = [engine.stop() for engine in engines]
    assert statuses == [0] * len(engines), [engine.newest() for engine in engines]


@pytest.fixture
def engine(start_engine: Callable[..., Engine]) -> Engine:
    """An engine at its defaults."""
    return start_engine()


@pytest.fixture
def scope(engine: Engine, open_client: Callable[[int], OpenScope]) -> OpenScope:
    """A client of `engine` over each transport."""
    return open_client(engine.port)
