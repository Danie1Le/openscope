"""Waveform transfer against the real engine: binary blocks decoded with `WAV:PRE?`, the ASCII
form, and PyVISA's own block reader (docs/scpi-reference.md, Waveform transfer)."""

import array

import pytest
import pyvisa

from openscope_client import Function, Preamble, WaveformFormat

#: The `*RST` record length, and the sample time zero sits at for the 50% trigger position:
#: round(0.5 x 9999), half away from zero.
POINTS = 10000
TIME_ZERO = 5000


def take_square_wave(scope):
    """Takes one capture of Generator 1's 1 kHz square wave (1000 samples per cycle at the
    `*RST` 1 MS/s), triggered on its rising edge at 0 V."""
    scope.generator(1).function = Function.SQUARE
    scope.single()
    scope.wait_complete()


def test_read_decodes_the_block_and_places_it_with_the_preamble(scope):
    take_square_wave(scope)

    waveform = scope.waveform.read()

    assert waveform.preamble == Preamble(1e6, POINTS, 1.0, 0.0, TIME_ZERO, False)
    assert len(waveform.samples) == POINTS
    assert set(waveform.samples) == {-1.0, 1.0}  # 2 Vpp, exact in float32.
    # The rising edge is the trigger event at time zero; a cycle is 1000 samples, high for the
    # first half (give or take the sample the phase rounds into at the half-cycle).
    assert waveform.samples[TIME_ZERO - 1 : TIME_ZERO + 1] == (-1.0, 1.0)
    assert waveform.samples[TIME_ZERO : TIME_ZERO + 499] == (1.0,) * 499
    assert waveform.samples[TIME_ZERO + 501 : TIME_ZERO + 999] == (-1.0,) * 498
    times = waveform.times()
    assert (times[0], times[TIME_ZERO], times[-1]) == (-0.005, 0.0, 0.004999)


def test_a_dc_level_reads_back_exactly(scope):
    scope.generator(1).function = Function.DC
    scope.generator(1).offset = 0.375
    scope.single()
    scope.wait_complete()

    waveform = scope.waveform.read()

    assert waveform.samples == (0.375,) * POINTS
    assert waveform.preamble.untriggered  # A flat signal never triggers: Auto took it.


def test_the_ascii_form_holds_the_same_samples(scope):
    scope.generator(1).noise = 0.1  # Values that float32 rounds.
    scope.single()
    scope.wait_complete()
    real = scope.waveform.read().samples

    scope.waveform.format = WaveformFormat.ASCII
    text = scope.query("WAV:DATA?").split(",")

    assert tuple(array.array("f", map(float, text))) == real  # Each value read as a float32.


def test_pyvisa_query_binary_values_reads_the_block_with_its_defaults(engine, scope):
    scope.generator(1).noise = 0.1
    scope.single()
    scope.wait_complete()
    expected = scope.waveform.read().samples

    # The manager stays open: pyvisa may share it with the client's own resource.
    manager = pyvisa.ResourceManager("@py")
    resource_name = f"TCPIP0::127.0.0.1::{engine.port}::SOCKET"
    with manager.open_resource(
        resource_name, read_termination="\n", write_termination="\n"
    ) as resource:
        resource.write("WAV:FORM REAL")
        values = resource.query_binary_values("WAV:DATA?")
        assert resource.query("SYST:ERR?") == '0,"No error"'

    assert tuple(values) == expected


def test_a_large_record_transfers_whole(scope):
    scope.acquisition.record_length = 1048576  # The longest record: a 4 MB block.
    scope.generator(1).noise = 0.1
    scope.timeout = 30
    scope.single()
    scope.wait_complete()

    waveform = scope.waveform.read()

    assert waveform.preamble.record_length == len(waveform.samples) == 1048576
    assert len(set(waveform.samples)) > 1000


@pytest.mark.parametrize("channel", [2, 4])
def test_each_channel_reads_its_own_generator(scope, channel):
    scope.channel(channel).enabled = True
    scope.generator(channel).function = Function.DC
    scope.generator(channel).offset = -0.5 * channel
    scope.single()
    scope.wait_complete()

    scope.waveform.source = channel
    waveform = scope.waveform.read()

    assert waveform.samples == (-0.5 * channel,) * POINTS
