"""The client raises the errors a line queued, with their SCPI codes and messages."""

import pytest
from fake_instrument import scripted

from openscope_client import NO_ERROR, ErrorEntry, ProtocolError, ScpiError


def test_a_rejected_command_raises_its_code_and_message(connect):
    connected = connect(scripted(errors=['-222,"Data out of range"']))

    with pytest.raises(ScpiError) as raised:
        connected.scope.write("SOUR1:FREQ 2E9")

    error = raised.value
    assert (error.code, error.message) == (-222, "Data out of range")
    assert error.errors == (ErrorEntry(-222, "Data out of range"),)
    assert error.reply is None
    assert str(error) == '-222,"Data out of range"'


def test_every_queued_error_is_read_and_carried_oldest_first(connect):
    queued = ['-113,"Undefined header"', '-222,"Data out of range"', '-350,"Queue overflow"']
    connected = connect(scripted(errors=queued))

    with pytest.raises(ScpiError) as raised:
        connected.scope.write("X")

    assert raised.value.code == -113
    assert raised.value.errors == (
        ErrorEntry(-113, "Undefined header"),
        ErrorEntry(-222, "Data out of range"),
        ErrorEntry(-350, "Queue overflow"),
    )
    assert str(raised.value) == '-113,"Undefined header" (and 2 more)'
    assert connected.fake.lines == ["X"] + ["SYST:ERR?"] * 4
    assert connected.scope.next_error() == NO_ERROR


def test_a_query_rejected_without_a_reply_raises_instead_of_waiting(connect):
    connected = connect(scripted(errors=['-230,"Data corrupt or stale"']))

    with pytest.raises(ScpiError) as raised:
        connected.scope.query("WAV:DATA?")

    assert raised.value.code == -230
    assert raised.value.reply is None


def test_a_query_rejected_without_a_reply_raises_from_query_block_too(connect):
    connected = connect(scripted(errors=['-221,"Settings conflict"']))

    with pytest.raises(ScpiError, match="Settings conflict"):
        connected.scope.query_block("WAV:DATA?")


def test_a_reply_with_errors_raises_and_carries_the_reply(connect):
    clipped = '-231,"Data questionable;Clipped"'
    connected = connect(scripted({"MEAS:VPP? CHAN1;VRMS? CHAN1": b"8;2.5\n"}, [clipped] * 2))

    with pytest.raises(ScpiError) as raised:
        connected.scope.query("MEAS:VPP? CHAN1;VRMS? CHAN1")

    assert raised.value.errors == (ErrorEntry(-231, "Data questionable;Clipped"),) * 2
    assert raised.value.reply == b"8;2.5"


def test_a_block_reply_with_errors_carries_the_block(connect):
    connected = connect(scripted({"X?": b"#12\n\n\n"}, ['-200,"Execution error"']))

    with pytest.raises(ScpiError) as raised:
        connected.scope.query_block("X?")

    assert raised.value.reply == b"#12\n\n"


def test_quotes_in_a_message_are_undoubled(connect):
    connected = connect(scripted(errors=['+5,"say ""hi"";#1"']))

    with pytest.raises(ScpiError) as raised:
        connected.scope.write("X")

    assert raised.value.errors == (ErrorEntry(5, 'say "hi";#1'),)


def test_next_error_pops_one_entry_without_raising(connect):
    connected = connect(scripted(errors=['-113,"Undefined header"']))

    assert connected.scope.next_error() == ErrorEntry(-113, "Undefined header")
    assert connected.scope.next_error() == NO_ERROR
    assert connected.fake.lines == ["SYST:ERR?", "SYST:ERR?"]


@pytest.mark.parametrize(
    ("entry", "match"),
    [
        (b"1", "error-queue entry"),
        (b'x,"a"', "error-queue entry"),
        (b'-1, "a"', "error-queue entry"),
        (b'-1,"a', "inside a string"),
        (b'-1,"a"b"', "inside a string"),
    ],
)
def test_an_error_check_that_gets_no_entry_is_a_protocol_error(connect, entry, match):
    connected = connect(lambda line: entry + b"\n" if line == "SYST:ERR?" else None)

    with pytest.raises(ProtocolError, match=match):
        connected.scope.next_error()


def test_a_line_queues_at_most_the_queue_depth_of_errors(connect):
    queued = [f'-{number},"Error {number}"' for number in range(100, 120)]
    connected = connect(scripted(errors=queued))

    with pytest.raises(ScpiError) as raised:
        connected.scope.write("X")

    assert len(raised.value.errors) == 20
    assert connected.scope.next_error() == NO_ERROR


def test_more_entries_than_the_queue_holds_is_a_protocol_error_that_closes_the_client(connect):
    connected = connect(scripted(errors=['-113,"Undefined header"'] * 21))

    with pytest.raises(ProtocolError, match="more than 20 error-queue entries"):
        connected.scope.write("X")
    assert connected.fake.lines == ["X"] + ["SYST:ERR?"] * 21
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.next_error()


def test_a_query_whose_error_check_gets_no_entry_is_a_protocol_error_that_closes_the_client(
    connect,
):
    connected = connect(lambda line: b"5\n" if line == "X?" else b"6\n")

    with pytest.raises(ProtocolError, match="error-queue entry"):
        connected.scope.query("X?")
    with pytest.raises(ConnectionError, match="closed"):
        connected.scope.query("X?")


@pytest.mark.parametrize(
    "line", ["SYST:ERR?", "system:error:next?", "*IDN?;:SYST:ERR?", "SYST:ERR?;ERR?"]
)
def test_the_error_queue_is_read_with_next_error_not_query(connect, line):
    connected = connect(scripted({"X?": b"2\n"}))

    for send in (connected.scope.query, connected.scope.write):
        with pytest.raises(ValueError, match="next_error"):
            send(line)
    assert connected.scope.query("X?") == "2"
    assert connected.fake.lines == ["X?", "SYST:ERR?"]
