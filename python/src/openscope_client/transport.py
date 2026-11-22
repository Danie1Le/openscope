"""Byte streams to the instrument, and reading IEEE 488.2 response messages from them."""

import re
import socket
from typing import Protocol

from openscope_client.errors import ProtocolError

_NEWLINE = ord("\n")
_HASH = ord("#")
_QUOTES = (ord('"'), ord("'"))
#: Outside a string: the bytes that end a response or start a string or a block.
_MARK = re.compile(rb"[\n\"'#]")
#: Inside a string opened by each quote: the bytes that end it, or end the response.
_STRING_END = {quote: re.compile(b"[\n" + bytes([quote]) + b"]") for quote in _QUOTES}


class Stream(Protocol):
    """A connection to the instrument as a byte stream. Each transport (raw socket, PyVISA)
    provides one, so the client has one API over both.

    Reads raise TimeoutError when nothing arrives within `timeout` seconds (None waits for ever)
    and ConnectionError when the connection is lost.
    """

    timeout: float | None

    def write(self, data: bytes) -> None:
        """Sends all of `data`."""

    def read_line(self) -> bytes:
        """Reads up to and including the next `\\n`."""

    def read_exact(self, count: int) -> bytes:
        """Reads exactly `count` bytes, whatever their values."""

    def close(self) -> None:
        """Closes the connection."""


class SocketStream:
    """A Stream over a TCP socket: the raw-socket transport. Its members do what Stream says."""

    _CHUNK = 65536

    def __init__(self, host: str, port: int, timeout: float | None) -> None:
        """Connects to `host`:`port`, waiting up to `timeout` seconds for it and for each read."""
        self._socket = socket.create_connection((host, port), timeout)
        self._received = bytearray()

    @property
    def timeout(self) -> float | None:
        return self._socket.gettimeout()

    @timeout.setter
    def timeout(self, seconds: float | None) -> None:
        self._socket.settimeout(seconds)

    def write(self, data: bytes) -> None:
        self._socket.sendall(data)

    def read_line(self) -> bytes:
        searched = 0
        while (newline := self._received.find(b"\n", searched)) < 0:
            searched = len(self._received)
            self._receive()
        return self._take(newline + 1)

    def read_exact(self, count: int) -> bytes:
        while len(self._received) < count:
            self._receive()
        return self._take(count)

    def close(self) -> None:
        self._socket.close()

    def _receive(self) -> None:
        chunk = self._socket.recv(self._CHUNK)
        if not chunk:
            raise ConnectionError("the instrument closed the connection")
        self._received += chunk

    def _take(self, count: int) -> bytes:
        taken = bytes(self._received[:count])
        del self._received[:count]
        return taken


def read_response(stream: Stream) -> bytes:
    """Reads one response message and returns it without its `\\n`.

    A `\\n` ends the message only outside definite-length blocks (`#<n><length><bytes>`), whose
    bytes may take any value, `\\n` included, and outside quoted strings (IEEE 488.2). Raises
    ProtocolError for a malformed block header or a string that the line ends inside.
    """
    message = bytearray(stream.read_line())
    position = 0
    quote: int | None = None
    while True:
        if position == len(message):  # A block ended the bytes read so far.
            message += stream.read_line()
        # Skip to the next byte that can end the message, or a string or block (a long ASCII
        # waveform is megabytes of digits and commas).
        mark = (_MARK if quote is None else _STRING_END[quote]).search(message, position)
        assert mark is not None, "every line read ends with a newline"
        position = mark.start()
        byte = message[position]
        if byte == _NEWLINE:
            if quote is not None:
                raise ProtocolError(f"response ends inside a string: {bytes(message)!r}")
            return bytes(message[:position])
        if quote is not None:  # The closing quote.
            quote = None
        elif byte in _QUOTES:
            quote = byte
        elif byte == _HASH:
            _, end = _block_extent(message, position)
            if end > len(message):
                message += stream.read_exact(end - len(message))
            position = end
            continue
        position += 1


def block_payload(response: bytes) -> bytes:
    """The data bytes of `response`, which must be one definite-length block and nothing else.
    Raises ProtocolError otherwise."""
    if not response.startswith(b"#"):
        raise ProtocolError(f"expected a binary block, got {response[:40]!r}")
    start, end = _block_extent(response, 0)
    if end != len(response):
        raise ProtocolError(f"unexpected bytes after a binary block: {response[end : end + 40]!r}")
    return response[start:end]


def _block_extent(data: bytes | bytearray, start: int) -> tuple[int, int]:
    """Where the data of the block whose `#` is at `start` begins and ends. The header is whole
    in `data`, since a response line ends after it; the data need not be. Raises ProtocolError
    for anything but a definite-length block header: `#`, a digit n from 1 to 9, then n digits."""
    digits = bytes(data[start + 1 : start + 2])
    count = int(digits) if digits.isdigit() else 0
    length = bytes(data[start + 2 : start + 2 + count])
    if count == 0 or not length.isdigit():
        header = bytes(data[start : start + 12])
        raise ProtocolError(f"not a definite-length block header: {header!r}")
    data_start = start + 2 + count
    return data_start, data_start + int(length)
