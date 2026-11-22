"""A scripted stand-in for the instrument's TCP server."""

import contextlib
import socket
import threading
from collections import deque
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field

from openscope_client import OpenScope


@dataclass(frozen=True)
class Hangup:
    """A response that sends `data` and then closes the connection."""

    data: bytes = b""


#: What a fake instrument does with one line: bytes to send back, None to send nothing, or
#: a Hangup.
Response = bytes | None | Hangup


def block_reply(data: bytes) -> bytes:
    """`data` in a definite-length block, as a reply line: `#<n><length><data>\\n`."""
    length = str(len(data))
    return f"#{len(length)}{length}".encode() + data + b"\n"


def scripted(
    replies: Mapping[str, Response] | None = None, errors: Iterable[str] = ()
) -> Callable[[str], Response]:
    """A responder like the instrument's: each line in `replies` gets its response (lines not in
    it reply nothing, as setters do), and `SYST:ERR?` pops `errors`, then replies `0,"No error"`."""
    queue = deque(errors)
    replies = replies or {}

    def respond(line: str) -> Response:
        if line == "SYST:ERR?":
            return (queue.popleft() if queue else '0,"No error"').encode() + b"\n"
        return replies.get(line)

    return respond


@dataclass
class FakeInstrument:
    """A TCP server on a free loopback port that serves one connection: it answers each line it
    receives with `respond(line)` and records the lines in `lines`."""

    respond: Callable[[str], Response]
    lines: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._listener = socket.create_server(("127.0.0.1", 0))
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        try:
            connection, _ = self._listener.accept()
        except OSError:  # Closed before a client came.
            return
        with connection, contextlib.suppress(ConnectionError):  # The client gave up first.
            received = b""
            while chunk := connection.recv(65536):
                received += chunk
                *complete, received = received.split(b"\n")
                for line in map(bytes.decode, complete):
                    self.lines.append(line)
                    response = self.respond(line)
                    if isinstance(response, Hangup):
                        connection.sendall(response.data)
                        return
                    if response is not None:
                        connection.sendall(response)

    def close(self) -> None:
        self._listener.close()
        self._thread.join(timeout=10)


@dataclass(frozen=True)
class Connected:
    """A client connected to a fake instrument."""

    scope: OpenScope
    fake: FakeInstrument

    def commands(self) -> list[str]:
        """The lines the fake received, without the client's `SYST:ERR?` checks."""
        return [line for line in self.fake.lines if line != "SYST:ERR?"]
