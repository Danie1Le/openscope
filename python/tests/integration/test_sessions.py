"""Several clients at once: each connection is a Session with its own error queue and waveform
settings (ADR 0002), while instrument settings are shared."""

from concurrent.futures import ThreadPoolExecutor

import pytest
from server_limits import MAX_CLIENTS

from openscope_client import NO_ERROR, ErrorEntry, Function, ScpiError, WaveformFormat

UNDEFINED_HEADER = ErrorEntry(-113, "Undefined header")


def noisy_session(engine):
    """A raw Session that has queued an undefined-header error and keeps it unread."""
    noisy = engine.raw_session()
    # *OPC? replies once the line before it has run, so the error is queued when this returns.
    assert noisy.query("SYST:BOGUS\n*OPC?") == "1"
    return noisy


def test_an_error_stays_in_the_queue_of_the_session_that_caused_it(engine, scope):
    noisy = noisy_session(engine)

    assert scope.next_error() == NO_ERROR
    scope.write("TRIG:LEV 0.5")  # Its own check finds nothing either.

    assert noisy.next_error() == UNDEFINED_HEADER
    assert noisy.next_error() == NO_ERROR


def test_clear_status_clears_only_the_callers_queue(engine, scope):
    noisy = noisy_session(engine)

    scope.clear_status()

    assert noisy.next_error() == UNDEFINED_HEADER


def test_waveform_settings_belong_to_each_session(engine, open_client):
    first, second = open_client(engine.port), open_client(engine.port)
    for channel, level in ((1, 0.25), (2, -0.75)):
        first.channel(channel).enabled = True
        first.generator(channel).function = Function.DC
        first.generator(channel).offset = level
    first.single()
    first.wait_complete()

    first.waveform.source = 2
    assert first.waveform.read().samples[:3] == (-0.75,) * 3  # Sets the format to REAL.

    assert (second.waveform.source, second.waveform.format) == (1, WaveformFormat.ASCII)
    assert second.query("WAV:DATA?").split(",")[:3] == ["0.25"] * 3
    assert (first.waveform.source, first.waveform.format) == (2, WaveformFormat.REAL)


def test_instrument_settings_are_shared(engine, open_client):
    first, second = open_client(engine.port), open_client(engine.port)

    first.trigger.level = 0.125

    assert second.trigger.level == 0.125


def test_eight_concurrent_clients_never_see_each_others_errors_or_settings(engine, open_client):
    clients = [open_client(engine.port) for _ in range(MAX_CLIENTS)]

    def work(number: int) -> None:
        scope = clients[number]
        channel = number % 4 + 1
        form = (WaveformFormat.ASCII, WaveformFormat.REAL)[number % 2]
        for _ in range(25):
            scope.waveform.source = channel
            scope.waveform.format = form
            with pytest.raises(ScpiError) as raised:
                scope.write(f"BOGUS{number}")
            assert raised.value.errors == (UNDEFINED_HEADER,)
            assert (scope.waveform.source, scope.waveform.format) == (channel, form)

    with ThreadPoolExecutor(len(clients)) as pool:
        for done in [pool.submit(work, number) for number in range(len(clients))]:
            done.result()
