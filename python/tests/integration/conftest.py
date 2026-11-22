"""Runs the real engine for each test: `openscope-engine` at the path in OPENSCOPE_ENGINE_PATH,
listening on a free loopback port, stopped and checked for a clean exit after the test."""

import json
import os
import signal
import subprocess
import threading
import time
from collections.abc import Callable, Iterator
from typing import Any

import pytest
from engine_helpers import PATIENCE

from openscope_client import ErrorEntry, OpenScope, SocketStream
from openscope_client.errors import parse_error_entry


class Engine:
    """A running `openscope-engine` and the events it has logged: its JSON log lines, and any
    other line it writes to standard error (a sanitizer's report) as an `output` event."""

    def __init__(self, path: str, options: tuple[str, ...]) -> None:
        """Starts the engine; `listen()` waits until it accepts connections."""
        self._process = subprocess.Popen(
            [path, "--port", "0", *options],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        self.events: list[dict[str, Any]] = []
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

    def _read_log(self) -> None:
        assert self._process.stderr is not None
        for line in self._process.stderr:
            try:
                event = json.loads(line)
            except ValueError:
                event = {"event": "output", "text": line.decode(errors="replace")}
            with self._logged:
                self.events.append(event)
                self._logged.notify_all()
        with self._logged:  # The engine has exited: wake waiters to see it.
            self._logged.notify_all()

    def wait_for(self, event: str, **fields: Any) -> dict[str, Any]:
        """The next event named `event` with `fields` among those logged after the one the last
        call returned, waiting for it to be logged."""
        deadline = time.monotonic() + PATIENCE
        with self._logged:
            while True:
                for index in range(self._seen, len(self.events)):
                    logged = self.events[index]
                    if logged["event"] == event and fields.items() <= logged.items():
                        self._seen = index + 1
                        return logged
                left = deadline - time.monotonic()
                if left <= 0 or not self._reader.is_alive():
                    raise AssertionError(f"no {event} event with {fields} in {self.events}")
                self._logged.wait(left)

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
        self._stream.write(f"{line}\n".encode())
        return self._stream.read_response().decode()

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
    assert statuses == [0] * len(engines), [engine.events for engine in engines]


@pytest.fixture
def engine(start_engine: Callable[..., Engine]) -> Engine:
    """An engine at its defaults."""
    return start_engine()


@pytest.fixture
def scope(engine: Engine, open_client: Callable[[int], OpenScope]) -> OpenScope:
    """A client of `engine` over each transport."""
    return open_client(engine.port)
