"""Byte streams to the instrument, and reading IEEE 488.2 response messages from them."""

import re
import socket
import time
from abc import ABC, abstractmethod

from openscope_client.errors import ProtocolError

#: The most bytes one response message may hold, its `\n` aside; a longer one raises
#: ProtocolError before it is read. The longest reply OpenScope sends is `WAV:DATA?` in ASCII
#: at the longest record: 1048576 points (`ACQuire:POINts` in docs/scpi-reference.md,
#: `Capture::kMaxRecordLength` in src/openscope/capture.hpp), each at most 16 bytes with its
#: comma (the shortest float32 form is at most 9 digits, a sign, a point and `E-38`): 16 MiB.
#: REAL blocks take 4 bytes per point (4 MiB), `FFT:DATA?` 4 bytes per bin, at most half a
#: record plus one (2 MiB). Four times the longest leaves room; a line of several such queries
#: can exceed it.
MAX_RESPONSE_BYTES = 64 * 2**20

_NEWLINE = ord("\n")
_HASH = ord("#")
_QUOTES = (ord('"'), ord("'"))
#: Outside a string: the bytes that end a response or start a string or a block.
_MARK = re.compile(rb"[\n\"'#]")
#: Inside a string opened by each quote: the bytes that end it, or end the response.
_STRING_END = {quote: re.compile(b"[\n" + bytes([quote]) + b"]") for quote in _QUOTES}


class _Deadline:
    """When the response being read must have arrived whole: `timeout` seconds from now."""

    def __init__(self, timeout: float | None) -> None:
        self._end = None if timeout is None else time.monotonic() + timeout

    def remaining(self) -> float | None:
        """Seconds left, or None for no deadline. Raises TimeoutError when none are left."""
        if self._end is None:
            return None
        left = self._end - time.monotonic()
        if left <= 0:
            raise TimeoutError("the response did not arrive whole within the timeout")
        return left


def _too_long() -> ProtocolError:
    return ProtocolError(f"a response longer than {MAX_RESPONSE_BYTES} bytes")


class Stream(ABC):
    """A connection to the instrument as a byte stream. Each transport (raw socket, PyVISA) is
    one, supplying how bytes are written and received; reading response messages is shared, so
    the client has one API and one set of limits over both.

    Reads raise TimeoutError when a response has not arrived whole within `timeout` seconds of
    the read starting (None waits for ever) and ConnectionError when the connection is lost.
    """

    def __init__(self) -> None:
        self._received = bytearray()

    @property
    @abstractmethod
    def timeout(self) -> float | None:
        """Seconds a whole response may take to arrive; None waits for ever."""

    @timeout.setter
    @abstractmethod
    def timeout(self, seconds: float | None) -> None: ...

    @abstractmethod
    def write(self, data: bytes) -> None:
        """Sends all of `data`."""

    @abstractmethod
    def close(self) -> None:
        """Closes the connection."""

    @abstractmethod
    def _receive(self, timeout: float | None) -> bytes:
        """The bytes that have arrived, waiting up to `timeout` seconds (None: for ever) for
        some. Raises TimeoutError when none arrive in time, ConnectionError when the connection
        is lost."""

    def read_response(self) -> bytes:
        """Reads one response message and returns it without its `\\n`.

        A `\\n` ends the message only outside definite-length blocks (`#<n><length><bytes>`),
        whose bytes may take any value, `\\n` included, and outside quoted strings (IEEE 488.2).
        Raises ProtocolError for a malformed block header, a string that the line ends inside,
        or a message longer than MAX_RESPONSE_BYTES.
        """
        deadline = _Deadline(self.timeout)
        message = bytearray(self._read_line(MAX_RESPONSE_BYTES, deadline))
        position = 0
        quote: int | None = None
        while True:
            if position == len(message):  # A block ended the bytes read so far.
                message += self._read_line(MAX_RESPONSE_BYTES - len(message), deadline)
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
                if end > MAX_RESPONSE_BYTES:
                    raise _too_long()
                if end > len(message):
                    message += self._read_exact(end - len(message), deadline)
                position = end
                continue
            position += 1

    def _read_line(self, limit: int, deadline: _Deadline) -> bytes:
        """Reads up to and including the next `\\n`, which must come within `limit` bytes."""
        searched = 0
        while (newline := self._received.find(b"\n", searched, limit + 1)) < 0:
            if len(self._received) > limit:
                raise _too_long()
            searched = len(self._received)
            self._receive_by(deadline)
        return self._take(newline + 1)

    def _read_exact(self, count: int, deadline: _Deadline) -> bytes:
        """Reads exactly `count` bytes, whatever their values."""
        while len(self._received) < count:
            self._receive_by(deadline)
        return self._take(count)

    def _receive_by(self, deadline: _Deadline) -> None:
        """Adds what arrives to the bytes received, raising TimeoutError past `deadline`."""
        chunk = self._receive(deadline.remaining())
        deadline.remaining()  # A transport's read may run past its timeout (VisaStream).
        self._received += chunk

    def _take(self, count: int) -> bytes:
        taken = bytes(self._received[:count])
        del self._received[:count]
        return taken


class SocketStream(Stream):
    """A Stream over a TCP socket: the raw-socket transport. Its members do what Stream says."""

    _CHUNK = 65536

    def __init__(self, host: str, port: int, timeout: float | None) -> None:
        """Connects to `host`:`port`, waiting up to `timeout` seconds for it and for each
        response."""
        super().__init__()
        self._socket = socket.create_connection((host, port), timeout)
        self._timeout = timeout

    @property
    def timeout(self) -> float | None:
        return self._timeout

    @timeout.setter
    def timeout(self, seconds: float | None) -> None:
        self._timeout = seconds

    def write(self, data: bytes) -> None:
        self._socket.settimeout(self._timeout)
        self._socket.sendall(data)

    def close(self) -> None:
        self._socket.close()

    def _receive(self, timeout: float | None) -> bytes:
        self._socket.settimeout(timeout)
        chunk = self._socket.recv(self._CHUNK)
        if not chunk:
            raise ConnectionError("the instrument closed the connection")
        return chunk


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
