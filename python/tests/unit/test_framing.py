"""Reading replies off the wire: text lines, definite-length blocks, and what the client does
when the bytes are malformed, late, or cut off."""

import os
import time

import pytest
from fake_instrument import Hangup, Trickle, block_reply, scripted

from openscope_client import ProtocolError, transport


def test_query_returns_the_reply_without_its_newline_and_checks_the_error_queue(connect):
    connected = connect(scripted({"*IDN?": b"OpenScope,OS-4,000001,0.1.0\n"}))

    assert connected.scope.query("*IDN?") == "OpenScope,OS-4,000001,0.1.0"
    assert connected.fake.lines == ["*IDN?", "SYST:ERR?"]


def test_a_carriage_return_before_the_newline_is_part_of_the_reply(connect):
    connected = connect(scripted({"X?": b"1\r\n"}))

    assert connected.scope.query("X?") == "1\r"


def test_an_empty_reply_is_an_empty_string(connect):
    connected = connect(scripted({"X?": b"\n"}))

    assert connected.scope.query("X?") == ""


def test_write_sends_the_line_and_checks_the_error_queue(connect):
    connected = connect(scripted())

    connected.scope.write("SOUR1:FREQ 1000")

    assert connected.fake.lines == ["SOUR1:FREQ 1000", "SYST:ERR?"]


def test_a_write_that_replies_is_reported_and_the_client_carries_on(connect):
    connected = connect(scripted({"*IDN?": b"OpenScope\n", "X?": b"2\n"}))

    with pytest.raises(ProtocolError, match="send it with query"):
        connected.scope.write("*IDN?")
    assert connected.scope.query("X?") == "2"


def test_a_query_that_sends_no_reply_and_no_error_is_reported_and_the_client_carries_on(connect):
    connected = connect(scripted({"X?": b"2\n"}))

    with pytest.raises(ProtocolError, match="sent no reply"):
        connected.scope.query("SOUR1:FREQ 1000")
    assert connected.scope.query("X?") == "2"


def test_a_line_holding_a_newline_is_refused_before_anything_is_sent(connect):
    connected = connect(scripted({"X?": b"2\n"}))

    with pytest.raises(ValueError, match="newline"):
        connected.scope.write("RUN\nSTOP")
    assert connected.scope.query("X?") == "2"
    assert connected.fake.lines == ["X?", "SYST:ERR?"]


def test_quoted_strings_may_hold_hashes_and_semicolons(connect):
    reply = b'"a#1;\'b""c",\'d"#9\'\n'
    connected = connect(scripted({"X?": reply}))

    assert connected.scope.query("X?") == reply[:-1].decode()


@pytest.mark.parametrize(
    "data",
    [
        b"",
        b"\n",
        b"a\nb;c\"d'#12\r\n\x00\xff",
        bytes(range(256)) * 4,
        os.urandom(3_000_000),  # Spans many reads.
    ],
    ids=["empty", "newline", "terminators-and-quotes", "every-byte", "3MB"],
)
def test_query_block_returns_the_data_whatever_its_bytes(connect, data):
    connected = connect(scripted({"WAV:DATA?": block_reply(data)}))

    assert connected.scope.query_block("WAV:DATA?") == data
    assert connected.fake.lines == ["WAV:DATA?", "SYST:ERR?"]


def test_blocks_may_share_a_reply_with_text(connect):
    reply = b"1;#14\n\n\n\n;#13a;b,2\n"
    connected = connect(scripted({"X?": reply, "Y?": b"3\n"}))

    assert connected.scope.query("X?") == reply[:-1].decode()
    assert connected.scope.query("Y?") == "3"


@pytest.mark.parametrize(
    "reply",
    [
        b"#\n",
        b"#A5hello\n",
        b"#0hello\n",  # An indefinite-length block: never sent by OpenScope.
        b"#2 5hello\n",
        b"#3\n5hello\n",
        b"#9123\n",
        b"1,#Hff\n",
    ],
    ids=["bare", "letter", "indefinite", "space", "newline-in-length", "short-length", "hex"],
)
def test_a_malformed_block_header_is_a_protocol_error_that_closes_the_client(connect, reply):
    connected = connect(scripted({"X?": reply}))

    with pytest.raises(ProtocolError, match="block header"):
        connected.scope.query("X?")
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


@pytest.mark.parametrize(
    ("reply", "match"),
    [
        (b"hello\n", "expected a binary block"),
        (b"#15hello;1\n", "after a binary block"),
        (b"1;#15hello\n", "expected a binary block"),
    ],
)
def test_query_block_needs_one_block_and_nothing_else(connect, reply, match):
    connected = connect(scripted({"X?": reply}))

    with pytest.raises(ProtocolError, match=match):
        connected.scope.query_block("X?")


def test_a_reply_that_is_not_ascii_text_is_a_protocol_error(connect):
    connected = connect(scripted({"X?": b"\xff\n"}))

    with pytest.raises(ProtocolError, match="not ASCII"):
        connected.scope.query("X?")


def test_a_reply_ending_inside_a_string_is_a_protocol_error(connect):
    connected = connect(scripted({"X?": b'1,"ab\ncd"\n'}))

    with pytest.raises(ProtocolError, match="inside a string"):
        connected.scope.query("X?")


def test_a_reply_that_never_comes_times_out_after_the_timeout_and_closes_the_client(connect):
    connected = connect(lambda line: None)  # Not even SYST:ERR? replies.
    connected.scope.timeout = 0.25
    assert connected.scope.timeout == pytest.approx(0.25)
    started = time.monotonic()

    with pytest.raises(TimeoutError):
        connected.scope.query("X?")
    assert 0.2 <= time.monotonic() - started < 1.25
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


@pytest.mark.parametrize(
    "sent",
    [b"", b"OpenSco", b"#3100" + b"\n" * 40, b"#3100", b"#14ab", b"1;#13abc"],
    ids=["nothing", "part-line", "part-block", "header-only", "part-short-block", "no-newline"],
)
def test_a_connection_closed_mid_reply_is_an_error_that_closes_the_client(connect, transport, sent):
    connected = connect(scripted({"X?": Hangup(sent)}))
    connected.scope.timeout = 0.5

    with pytest.raises(transport.connection_lost):
        connected.scope.query("X?")
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


def test_a_reply_that_trickles_in_past_the_timeout_times_out_and_closes_the_client(connect):
    # Each byte comes well within the timeout; the whole reply does not. (The raw socket raises
    # at the timeout; pyvisa-py's read runs on to the `\n` first, see VisaStream._receive.)
    connected = connect(scripted({"X?": Trickle(b"123456789\n", 0.1)}))
    connected.scope.timeout = 0.5

    with pytest.raises(TimeoutError):
        connected.scope.query("X?")
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


def test_the_longest_reply_the_instrument_sends_is_read(connect):
    # `WAV:DATA?` in ASCII at the longest record, every value as long as a float32 gets.
    reply = b",".join([b"-1.23456789E-38"] * 1048576)
    connected = connect(scripted({"WAV:DATA?": reply + b"\n"}))

    assert connected.scope.query("WAV:DATA?").encode() == reply


def test_a_block_header_claiming_more_than_the_limit_is_refused_before_its_data(connect):
    connected = connect(scripted({"WAV:DATA?": b"#9999999999\n"}))
    started = time.monotonic()

    with pytest.raises(ProtocolError, match="longer than"):
        connected.scope.query("WAV:DATA?")
    assert time.monotonic() - started < 1  # It did not wait for the data.
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


@pytest.mark.parametrize(
    ("reply", "fits"),
    [
        (b"1" * 12 + b"\n", True),
        (b"1" * 13 + b"\n", False),
        (b"1" * 13, False),  # No newline comes at all.
        (b"#19" + b"\n" * 9 + b"\n", True),
        (b"1#19" + b"\n" * 9 + b"\n", False),
        (b"1;#13\n\n\n;234\n", True),
        (b"1;#13\n\n\n;2345\n", False),  # The line after a block counts too.
    ],
    ids=["line", "long-line", "endless-line", "block", "long-block", "mixed", "long-mixed"],
)
def test_a_response_longer_than_the_limit_is_a_protocol_error_that_closes_the_client(
    connect, monkeypatch, reply, fits
):
    # 12 bytes: the error check's `0,"No error"` fits.
    monkeypatch.setattr(transport, "MAX_RESPONSE_BYTES", 12)
    connected = connect(scripted({"X?": reply, "Y?": b"2\n"}))

    if fits:
        assert connected.scope.query("X?").encode() == reply[:-1]
        assert connected.scope.query("Y?") == "2"
    else:
        with pytest.raises(ProtocolError, match="longer than 12 bytes"):
            connected.scope.query("X?")
        with pytest.raises(ConnectionError, match="closed"):
            connected.scope.query("Y?")


def test_a_closed_client_refuses_every_call(connect):
    connected = connect(scripted())

    with connected.scope as scope:
        pass
    scope.close()  # Closing twice does nothing.
    for call in (lambda: scope.write("RUN"), scope.next_error, lambda: scope.timeout):
        with pytest.raises(ConnectionError, match="closed"):
            call()
