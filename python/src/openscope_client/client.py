"""Typed access to an OpenScope instrument over SCPI (docs/scpi-reference.md)."""

import re
import struct
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from enum import StrEnum
from types import TracebackType
from typing import Any, NamedTuple, Self, overload

from openscope_client.errors import (
    NO_ERROR,
    ErrorEntry,
    ProtocolError,
    ScpiError,
    parse_error_entry,
    parsed_reply,
)
from openscope_client.transport import SocketStream, Stream, block_payload

#: The line that reads the next entry of a Session's error queue.
_NEXT_ERROR = b"SYST:ERR?\n"
#: The most entries a Session's error queue holds (docs/scpi-reference.md, Error queue): a peer
#: that reports more for one line is not following the protocol.
_QUEUE_DEPTH = 20
#: A query of the error queue, in any of its forms: the client reads the queue itself after
#: every line, so a line that replied an entry could not be told from that check. Compound
#: lines are refused too: `*CLS;SYST:ERR?` replies an entry alone.
_ERROR_QUERY = re.compile(r"ERR(?:OR)?(?::NEXT)?\?", re.IGNORECASE)
#: The default port of the engine's SCPI server.
DEFAULT_PORT = 5025


class Function(StrEnum):
    """A Generator's waveform shape (`SOURce<n>:FUNCtion`)."""

    SINE = "SIN"
    SQUARE = "SQU"
    TRIANGLE = "TRI"
    RAMP = "RAMP"
    DC = "DC"
    NOISE = "NOIS"


class FaultKind(StrEnum):
    """A kind of fault `SOURce<n>:FAULt` injects."""

    GLITCH = "GLIT"
    DROPOUT = "DROP"
    FREQUENCY_STEP = "FST"


class Slope(StrEnum):
    """The edge that is a trigger event (`TRIGger:SLOPe`)."""

    POSITIVE = "POS"
    NEGATIVE = "NEG"


class TriggerMode(StrEnum):
    """What the instrument does when no trigger event arrives (`TRIGger:MODE`)."""

    AUTO = "AUTO"
    NORMAL = "NORM"


class WaveformFormat(StrEnum):
    """How `WAV:DATA?` writes samples (`WAVeform:FORMat`)."""

    ASCII = "ASC"
    REAL = "REAL"


class Window(StrEnum):
    """The FFT window (`FFT:WINDow`)."""

    RECTANGULAR = "RECT"
    HANN = "HANN"
    BLACKMAN_HARRIS = "BHAR"


class Measurement(StrEnum):
    """An automatic measurement (`MEASure:<type>?`)."""

    PEAK_TO_PEAK = "VPP"
    MINIMUM = "VMIN"
    MAXIMUM = "VMAX"
    AVERAGE = "VAV"
    RMS = "VRMS"
    FREQUENCY = "FREQ"
    PERIOD = "PER"
    DUTY_CYCLE = "DUTY"
    RISE_TIME = "RIS"
    FALL_TIME = "FALL"


class Identity(NamedTuple):
    """The `*IDN?` reply: `OpenScope,OS-4,<serial>,<version>`."""

    manufacturer: str
    model: str
    serial: str
    version: str


class Preamble(NamedTuple):
    """The `WAV:PRE?` reply: how the latest capture was taken, to turn its samples into volts
    and seconds."""

    sample_rate: float
    record_length: int
    scale: float
    offset: float
    #: The sample (from 0) where time zero sits.
    time_zero: int
    untriggered: bool


class Statistics(NamedTuple):
    """The `MEAS:STAT?` reply. Fields with no value are 9.91E37, SCPI's not-a-number."""

    current: float
    mean: float
    minimum: float
    maximum: float
    standard_deviation: float
    count: int


@dataclass(frozen=True)
class WaveformData:
    """One channel of the latest capture: its samples in volts, oldest first, and the preamble
    that places them in time."""

    preamble: Preamble
    samples: tuple[float, ...]

    def times(self) -> tuple[float, ...]:
        """The time of each sample in seconds, 0 at the trigger position."""
        rate, position = self.preamble.sample_rate, self.preamble.time_zero
        return tuple((index - position) / rate for index in range(len(self.samples)))


def _number(text: str) -> float:
    return parsed_reply(float, text)


def _whole(text: str) -> int:
    return parsed_reply(int, text)


def _flag(text: str) -> bool:
    if text not in ("0", "1"):
        raise ProtocolError(f"expected 0 or 1, got {text!r}")
    return text == "1"


_CHANNEL_REPLY = re.compile(r"CHAN([1-4])")


def _channel(text: str) -> int:
    match = _CHANNEL_REPLY.fullmatch(text)
    if match is None:
        raise ProtocolError(f"expected CHAN<1-4>, got {text!r}")
    return int(match[1])


def _fields(text: str, parsers: tuple[Callable[[str], Any], ...]) -> list[Any]:
    """The comma-separated fields of `text`, each read by its parser."""
    fields = text.split(",")
    if len(fields) != len(parsers):
        raise ProtocolError(f"expected {len(parsers)} fields, got {text!r}")
    return [parse(field) for parse, field in zip(parsers, fields, strict=True)]


def _preamble(text: str) -> Preamble:
    """The `WAV:PRE?` reply `text`, read."""
    return Preamble(*_fields(text, (_number, _whole, _number, _number, _whole, _flag)))


def _float32_values(data: bytes) -> tuple[float, ...]:
    """Little-endian IEEE 754 float32 values, as `WAV:FORM REAL` and `FFT:DATA?` write them."""
    if len(data) % 4:
        raise ProtocolError(f"a float32 block of {len(data)} bytes, not a multiple of 4")
    return struct.unpack(f"<{len(data) // 4}f", data)


class _Codec[T](NamedTuple):
    """How a setting's value is read from its query reply and written into its setter."""

    parse: Callable[[str], T]
    format: Callable[[T], str]


_REAL = _Codec(_number, lambda value: repr(float(value)))
_WHOLE = _Codec(_whole, lambda value: str(int(value)))
_BOOLEAN = _Codec(_flag, lambda value: "ON" if value else "OFF")
_CHANNEL = _Codec(_channel, lambda channel: f"CHAN{channel}")


def _keyword[T](choices: type[T]) -> _Codec[T]:
    """The codec of a keyword setting whose choices are the StrEnum `choices`."""
    return _Codec(lambda text: parsed_reply(choices, text), str)


class _Subsystem:
    """A subsystem of SCPI headers that share a path, such as `SOUR2:`."""

    def __init__(self, client: "OpenScope", path: str) -> None:
        self._client = client
        self._path = path


class _Setting[T]:
    """A setting of a subsystem: reading the attribute sends `<path><mnemonic>?` and parses the
    reply; assigning it sends `<path><mnemonic> <value>`."""

    def __init__(self, mnemonic: str, codec: _Codec[T]) -> None:
        self._mnemonic = mnemonic
        self._codec = codec

    @overload
    def __get__(self, node: None, owner: type[_Subsystem]) -> Self: ...
    @overload
    def __get__(self, node: _Subsystem, owner: type[_Subsystem]) -> T: ...
    def __get__(self, node: _Subsystem | None, owner: type[_Subsystem]) -> "Self | T":
        if node is None:
            return self
        return self._codec.parse(node._client.query(f"{node._path}{self._mnemonic}?"))

    def __set__(self, node: _Subsystem, value: T) -> None:
        node._client.write(f"{node._path}{self._mnemonic} {self._codec.format(value)}")


class Generator(_Subsystem):
    """Generator n (`SOURce<n>:...`), which feeds Channel n."""

    function = _Setting("FUNC", _keyword(Function))
    #: Hertz.
    frequency = _Setting("FREQ", _REAL)
    #: Volts peak to peak.
    amplitude = _Setting("AMPL", _REAL)
    #: Volts.
    offset = _Setting("OFFS", _REAL)
    #: Volts RMS of Gaussian noise.
    noise = _Setting("NOIS", _REAL)

    def inject_fault(
        self, kind: FaultKind, magnitude: float | None = None, duration: int | None = None
    ) -> None:
        """Injects one fault at the next sample index; a left-out argument takes the kind's
        default. `duration` (samples) needs `magnitude`, as in SCPI."""
        if duration is not None and magnitude is None:
            raise ValueError("a fault's duration needs its magnitude")
        parameters = [str(kind)]
        if magnitude is not None:
            parameters.append(_REAL.format(magnitude))
        if duration is not None:
            parameters.append(_WHOLE.format(duration))
        self._client.write(f"{self._path}FAUL {','.join(parameters)}")


class Channel(_Subsystem):
    """Channel n (`CHANnel<n>:...`)."""

    enabled = _Setting("DISP", _BOOLEAN)
    #: Volts per division.
    scale = _Setting("SCAL", _REAL)
    #: Volts at the centre of the input range.
    offset = _Setting("OFFS", _REAL)


class Acquisition(_Subsystem):
    """The sample clock and record length (`ACQuire:...`)."""

    #: Samples per second.
    sample_rate = _Setting("SRAT", _REAL)
    #: The record length.
    record_length = _Setting("POIN", _WHOLE)


class Timebase(_Subsystem):
    """The display viewport (`TIMebase:...`); it never changes what is captured."""

    #: Seconds per division.
    scale = _Setting("SCAL", _REAL)


class Trigger(_Subsystem):
    """The edge trigger (`TRIGger:...`)."""

    #: The trigger channel, 1 to 4.
    source = _Setting("SOUR", _CHANNEL)
    #: Volts.
    level = _Setting("LEV", _REAL)
    slope = _Setting("SLOP", _keyword(Slope))
    mode = _Setting("MODE", _keyword(TriggerMode))
    #: Seconds.
    holdoff = _Setting("HOLD", _REAL)
    #: Percent of the way from the first sample to the last.
    position = _Setting("POS", _REAL)


class Waveform(_Subsystem):
    """Waveform transfer (`WAVeform:...`). Its settings belong to this client's Session."""

    #: The channel read, 1 to 4.
    source = _Setting("SOUR", _CHANNEL)
    format = _Setting("FORM", _keyword(WaveformFormat))

    def preamble(self) -> Preamble:
        """How the latest capture was taken."""
        return _preamble(self._client.query(f"{self._path}PRE?"))

    def read(self) -> WaveformData:
        """The source channel of the latest capture, transferred as a binary block (this sets
        the format to REAL). The preamble and the data are two queries on one line, so while
        Running they may read two captures: take one with `single()` and `wait_complete()` first
        to read one."""
        reply = self._client._query(f"{self._path}FORM REAL;PRE?;DATA?")
        text, _, block = reply.partition(b";")
        preamble = _preamble(_text(text))
        samples = _float32_values(block_payload(block))
        if len(samples) != preamble.record_length:
            raise ProtocolError(
                f"WAV:DATA? sent {len(samples)} samples, WAV:PRE? said {preamble.record_length}"
            )
        return WaveformData(preamble, samples)


class Fft(_Subsystem):
    """The spectrum of one channel of the latest capture (`FFT:...`). Its settings belong to this
    client's Session."""

    #: The channel transformed, 1 to 4.
    source = _Setting("SOUR", _CHANNEL)
    window = _Setting("WIND", _keyword(Window))

    def spectrum(self) -> tuple[float, ...]:
        """The magnitude spectrum in dBV RMS, from DC up in steps of `bin_spacing()`."""
        return _float32_values(self._client.query_block(f"{self._path}DATA?"))

    def bin_spacing(self) -> float:
        """The bin spacing in hertz."""
        return _number(self._client.query(f"{self._path}SPAC?"))


class OpenScope:
    """An OpenScope instrument, reached through one Session.

    Every command checks the Session's error queue: the client sends `SYST:ERR?` behind each line
    and raises ScpiError, after reading every queued entry, when the line queued errors. A query
    the instrument rejects sends no reply, so this check is also how the client knows not to
    wait for one.

    Lines run one at a time, each after the reply to the one before. Any exception while a line
    and its error check are on the wire closes the client: a transport error (TimeoutError,
    ConnectionError), a malformed response, a KeyboardInterrupt. A late or unread reply would
    otherwise be read as the answer to the next line. Each response must arrive whole within
    `timeout` and hold at most `transport.MAX_RESPONSE_BYTES` bytes.
    """

    def __init__(self, stream: Stream) -> None:
        """Drives the instrument over `stream`; `connect` and `open_visa` make one."""
        self._stream: Stream | None = stream
        self.acquisition = Acquisition(self, "ACQ:")
        self.timebase = Timebase(self, "TIM:")
        self.trigger = Trigger(self, "TRIG:")
        self.waveform = Waveform(self, "WAV:")
        self.fft = Fft(self, "FFT:")

    @classmethod
    def connect(
        cls, host: str = "127.0.0.1", port: int = DEFAULT_PORT, timeout: float | None = 10.0
    ) -> "OpenScope":
        """Connects over a raw TCP socket. `timeout` is in seconds; None waits for ever."""
        return cls(SocketStream(host, port, timeout))

    @classmethod
    def open_visa(
        cls,
        resource_name: str = f"TCPIP0::localhost::{DEFAULT_PORT}::SOCKET",
        timeout: float | None = 10.0,
        visa_library: str = "",
    ) -> "OpenScope":
        """Connects through PyVISA (the `visa` extra). `visa_library` picks the VISA library as
        pyvisa.ResourceManager does: `@py` is pyvisa-py; empty is PyVISA's default.

        pyvisa-py does not report a connection the instrument closes (the 9th client, or an idle
        timeout): a read then raises TimeoutError once `timeout` has passed, where the raw socket
        raises ConnectionError at once."""
        from openscope_client.visa import VisaStream  # noqa: PLC0415 (an optional dependency)

        return cls(VisaStream(resource_name, timeout, visa_library))

    @property
    def timeout(self) -> float | None:
        """Seconds each reply may take to arrive whole, such as `*OPC?` waiting for a Single."""
        return self._open_stream().timeout

    @timeout.setter
    def timeout(self, seconds: float | None) -> None:
        self._open_stream().timeout = seconds

    def close(self) -> None:
        """Closes the connection, which ends the Session."""
        if self._stream is not None:
            stream, self._stream = self._stream, None
            stream.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    # SCPI text.

    def write(self, line: str) -> None:
        """Runs `line`, which must not reply. Raises ScpiError when it queued errors."""
        reply = self._run(line)
        if reply is not None:
            raise ProtocolError(f"{line!r} replied {reply[:40]!r}; send it with query()")

    def query(self, line: str) -> str:
        """Runs `line` and returns its reply. Raises ScpiError when it queued errors. Read the
        error queue itself with `next_error()`."""
        return _text(self._query(line))

    def query_block(self, line: str) -> bytes:
        """Runs `line`, whose reply is one definite-length block, and returns the block's data."""
        return block_payload(self._query(line))

    def next_error(self) -> ErrorEntry:
        """Removes and returns the oldest entry of the Session's error queue."""
        with self._transport() as stream:
            return _next_error(stream)

    # Common commands and run control.

    def identify(self) -> Identity:
        """The instrument's identity (`*IDN?`)."""
        return Identity(*_fields(self.query("*IDN?"), (str, str, str, str)))

    def reset(self) -> None:
        """Restores every instrument-wide setting to its `*RST` value (`*RST`)."""
        self.write("*RST")

    def clear_status(self) -> None:
        """Empties this Session's error queue (`*CLS`)."""
        self.write("*CLS")

    def wait_complete(self) -> None:
        """Waits until no operation is pending (`*OPC?`), such as an armed Single. The wait is
        bounded by `timeout` only."""
        reply = self.query("*OPC?")
        if reply != "1":
            raise ProtocolError(f"*OPC? replied {reply!r}")

    def wait(self) -> None:
        """Holds this Session's later commands until no operation is pending (`*WAI`)."""
        self.write("*WAI")

    def run(self) -> None:
        """Enters the Running run state (`RUN`)."""
        self.write("RUN")

    def stop(self) -> None:
        """Enters the Stopped run state (`STOP`)."""
        self.write("STOP")

    def single(self) -> None:
        """Arms one capture, then stops (`SINGle`)."""
        self.write("SING")

    # Subsystems.

    def generator(self, number: int) -> Generator:
        """Generator `number`, 1 to 4."""
        return Generator(self, f"SOUR{number}:")

    def channel(self, number: int) -> Channel:
        """Channel `number`, 1 to 4."""
        return Channel(self, f"CHAN{number}:")

    # Measurements.

    def measure(self, measurement: Measurement, channel: int) -> float:
        """`measurement` of `channel` in the latest capture. A result that is not OK raises
        ScpiError naming its status; a Clipped one carries its value in `reply`."""
        return _number(self.query(f"MEAS:{measurement}? CHAN{channel}"))

    def statistics(self, measurement: Measurement, channel: int) -> Statistics:
        """The statistics of `measurement` of `channel` since the last reset."""
        reply = self.query(f"MEAS:STAT? {measurement},CHAN{channel}")
        return Statistics(*_fields(reply, (_number,) * 5 + (_whole,)))

    def reset_statistics(self) -> None:
        """Resets the statistics of every measurement (`MEAS:STAT:RES`)."""
        self.write("MEAS:STAT:RES")

    # Internals.

    def _query(self, line: str) -> bytes:
        reply = self._run(line)
        if reply is None:
            raise ProtocolError(f"{line!r} sent no reply")
        return reply

    def _run(self, line: str) -> bytes | None:
        """Runs `line` and checks the error queue. Returns the line's response, or None when it
        sent none. Raises ScpiError, after reading every queued entry, when there were errors."""
        if "\n" in line:
            raise ValueError("a line cannot hold a newline; send each line on its own")
        if _ERROR_QUERY.search(line):
            raise ValueError("read the error queue with next_error()")
        sent = f"{line}\n".encode("ascii") + _NEXT_ERROR
        with self._transport() as stream:
            stream.write(sent)
            # A line that fails sends no reply, so the first response may be the check's.
            response = stream.read_response()
            entry = parse_error_entry(response)
            reply = None if entry is not None else response
            if entry is None:
                entry = _error_entry(stream.read_response())
            errors: list[ErrorEntry] = []
            while entry.code != NO_ERROR.code:
                if len(errors) == _QUEUE_DEPTH:
                    raise ProtocolError(f"more than {_QUEUE_DEPTH} error-queue entries")
                errors.append(entry)
                entry = _next_error(stream)
        if errors:
            raise ScpiError(tuple(errors), reply)
        return reply

    def _open_stream(self) -> Stream:
        if self._stream is None:
            raise ConnectionError("the client is closed")
        return self._stream

    @contextmanager
    def _transport(self) -> Iterator[Stream]:
        """The stream, closed when an exchange on it ends part way, for whatever reason
        (KeyboardInterrupt included): its replies may still be on the wire."""
        stream = self._open_stream()
        try:
            yield stream
        except BaseException:
            self.close()
            raise


def _next_error(stream: Stream) -> ErrorEntry:
    """Removes and returns the oldest entry of the Session's error queue."""
    stream.write(_NEXT_ERROR)
    return _error_entry(stream.read_response())


def _error_entry(response: bytes) -> ErrorEntry:
    entry = parse_error_entry(response)
    if entry is None:
        raise ProtocolError(f"expected an error-queue entry, got {response[:40]!r}")
    return entry


def _text(response: bytes) -> str:
    try:
        return response.decode("ascii")
    except UnicodeDecodeError as error:
        raise ProtocolError(f"a reply that is not ASCII text: {response[:40]!r}") from error
