"""The typed API: the SCPI each method sends and how it reads the reply
(docs/scpi-reference.md)."""

import struct

import pytest
from fake_instrument import block_reply, scripted

from openscope_client import (
    FaultKind,
    Function,
    Identity,
    Measurement,
    Preamble,
    ProtocolError,
    Slope,
    Statistics,
    TriggerMode,
    WaveformData,
    WaveformFormat,
    Window,
)

PREAMBLE = b"1E+06,4,0.5,-1,2,1\n"
SAMPLES = (0.75, -0.25, 1.5, 3.0)


def float32_block(values: tuple[float, ...]) -> bytes:
    """`values` as the instrument writes a float32 block, with its newline."""
    return block_reply(struct.pack(f"<{len(values)}f", *values))


def waveform_reply(values: tuple[float, ...]) -> bytes:
    """The reply to `WAV:FORM REAL;PRE?;DATA?`: PREAMBLE, then `values` as a block."""
    return PREAMBLE[:-1] + b";" + float32_block(values)


#: The line Waveform.read() sends.
READ_WAVEFORM = "WAV:FORM REAL;PRE?;DATA?"


# (action, replies, the lines it sends, what it returns). A setting's query sends the one line
# it has a reply for, so those cases leave the lines empty.
CASES = {
    "identify": (
        lambda s: s.identify(),
        {"*IDN?": b"OpenScope,OS-4,000001,0.1.0\n"},
        ["*IDN?"],
        Identity("OpenScope", "OS-4", "000001", "0.1.0"),
    ),
    "reset": (lambda s: s.reset(), {}, ["*RST"], None),
    "clear_status": (lambda s: s.clear_status(), {}, ["*CLS"], None),
    "wait_complete": (lambda s: s.wait_complete(), {"*OPC?": b"1\n"}, ["*OPC?"], None),
    "wait": (lambda s: s.wait(), {}, ["*WAI"], None),
    "run": (lambda s: s.run(), {}, ["RUN"], None),
    "stop": (lambda s: s.stop(), {}, ["STOP"], None),
    "single": (lambda s: s.single(), {}, ["SING"], None),
    "set function": (
        lambda s: setattr(s.generator(3), "function", Function.SQUARE),
        {},
        ["SOUR3:FUNC SQU"],
        None,
    ),
    "get function": (
        lambda s: s.generator(3).function,
        {"SOUR3:FUNC?": b"NOIS\n"},
        [],
        Function.NOISE,
    ),
    "set frequency": (
        lambda s: setattr(s.generator(2), "frequency", 1500),
        {},
        ["SOUR2:FREQ 1500.0"],
        None,
    ),
    "get frequency": (lambda s: s.generator(1).frequency, {"SOUR1:FREQ?": b"1E+06\n"}, [], 1e6),
    "set amplitude": (
        lambda s: setattr(s.generator(1), "amplitude", 0.25),
        {},
        ["SOUR1:AMPL 0.25"],
        None,
    ),
    "get amplitude": (lambda s: s.generator(4).amplitude, {"SOUR4:AMPL?": b"2\n"}, [], 2.0),
    "set generator offset": (
        lambda s: setattr(s.generator(1), "offset", -1e-5),
        {},
        ["SOUR1:OFFS -1e-05"],
        None,
    ),
    "get generator offset": (lambda s: s.generator(1).offset, {"SOUR1:OFFS?": b"-0.5\n"}, [], -0.5),
    "set noise": (lambda s: setattr(s.generator(1), "noise", 0.01), {}, ["SOUR1:NOIS 0.01"], None),
    "get noise": (lambda s: s.generator(2).noise, {"SOUR2:NOIS?": b"0\n"}, [], 0.0),
    "fault default": (
        lambda s: s.generator(2).inject_fault(FaultKind.GLITCH),
        {},
        ["SOUR2:FAUL GLIT"],
        None,
    ),
    "fault magnitude": (
        lambda s: s.generator(1).inject_fault(FaultKind.DROPOUT, 0.5),
        {},
        ["SOUR1:FAUL DROP,0.5"],
        None,
    ),
    "fault magnitude and duration": (
        lambda s: s.generator(4).inject_fault(FaultKind.FREQUENCY_STEP, 500, 200),
        {},
        ["SOUR4:FAUL FST,500.0,200"],
        None,
    ),
    "set enabled on": (
        lambda s: setattr(s.channel(2), "enabled", True),
        {},
        ["CHAN2:DISP ON"],
        None,
    ),
    "set enabled off": (
        lambda s: setattr(s.channel(1), "enabled", False),
        {},
        ["CHAN1:DISP OFF"],
        None,
    ),
    "get enabled on": (lambda s: s.channel(3).enabled, {"CHAN3:DISP?": b"1\n"}, [], True),
    "get enabled off": (lambda s: s.channel(3).enabled, {"CHAN3:DISP?": b"0\n"}, [], False),
    "set channel scale": (
        lambda s: setattr(s.channel(4), "scale", 0.001),
        {},
        ["CHAN4:SCAL 0.001"],
        None,
    ),
    "get channel scale": (lambda s: s.channel(4).scale, {"CHAN4:SCAL?": b"0.001\n"}, [], 0.001),
    "set channel offset": (
        lambda s: setattr(s.channel(1), "offset", 2),
        {},
        ["CHAN1:OFFS 2.0"],
        None,
    ),
    "get channel offset": (lambda s: s.channel(1).offset, {"CHAN1:OFFS?": b"2\n"}, [], 2.0),
    "set sample rate": (
        lambda s: setattr(s.acquisition, "sample_rate", 1e6),
        {},
        ["ACQ:SRAT 1000000.0"],
        None,
    ),
    "get sample rate": (lambda s: s.acquisition.sample_rate, {"ACQ:SRAT?": b"1E+06\n"}, [], 1e6),
    "set record length": (
        lambda s: setattr(s.acquisition, "record_length", 1024),
        {},
        ["ACQ:POIN 1024"],
        None,
    ),
    "get record length": (
        lambda s: s.acquisition.record_length,
        {"ACQ:POIN?": b"10000\n"},
        [],
        10000,
    ),
    "set timebase": (lambda s: setattr(s.timebase, "scale", 1e-3), {}, ["TIM:SCAL 0.001"], None),
    "get timebase": (lambda s: s.timebase.scale, {"TIM:SCAL?": b"0.001\n"}, [], 0.001),
    "set trigger source": (
        lambda s: setattr(s.trigger, "source", 3),
        {},
        ["TRIG:SOUR CHAN3"],
        None,
    ),
    "get trigger source": (lambda s: s.trigger.source, {"TRIG:SOUR?": b"CHAN4\n"}, [], 4),
    "set level": (lambda s: setattr(s.trigger, "level", -0.5), {}, ["TRIG:LEV -0.5"], None),
    "get level": (lambda s: s.trigger.level, {"TRIG:LEV?": b"0.1\n"}, [], 0.1),
    "set slope": (
        lambda s: setattr(s.trigger, "slope", Slope.NEGATIVE),
        {},
        ["TRIG:SLOP NEG"],
        None,
    ),
    "get slope": (lambda s: s.trigger.slope, {"TRIG:SLOP?": b"POS\n"}, [], Slope.POSITIVE),
    "set mode": (
        lambda s: setattr(s.trigger, "mode", TriggerMode.NORMAL),
        {},
        ["TRIG:MODE NORM"],
        None,
    ),
    "get mode": (lambda s: s.trigger.mode, {"TRIG:MODE?": b"AUTO\n"}, [], TriggerMode.AUTO),
    "set holdoff": (lambda s: setattr(s.trigger, "holdoff", 0.002), {}, ["TRIG:HOLD 0.002"], None),
    "get holdoff": (lambda s: s.trigger.holdoff, {"TRIG:HOLD?": b"0\n"}, [], 0.0),
    "set position": (lambda s: setattr(s.trigger, "position", 10), {}, ["TRIG:POS 10.0"], None),
    "get position": (lambda s: s.trigger.position, {"TRIG:POS?": b"50\n"}, [], 50.0),
    "set waveform source": (
        lambda s: setattr(s.waveform, "source", 2),
        {},
        ["WAV:SOUR CHAN2"],
        None,
    ),
    "get waveform source": (lambda s: s.waveform.source, {"WAV:SOUR?": b"CHAN1\n"}, [], 1),
    "set format": (
        lambda s: setattr(s.waveform, "format", WaveformFormat.ASCII),
        {},
        ["WAV:FORM ASC"],
        None,
    ),
    "get format": (lambda s: s.waveform.format, {"WAV:FORM?": b"REAL\n"}, [], WaveformFormat.REAL),
    "preamble": (
        lambda s: s.waveform.preamble(),
        {"WAV:PRE?": PREAMBLE},
        ["WAV:PRE?"],
        Preamble(1e6, 4, 0.5, -1.0, 2, True),
    ),
    "read waveform": (
        lambda s: s.waveform.read(),
        {READ_WAVEFORM: waveform_reply(SAMPLES)},
        [READ_WAVEFORM],
        WaveformData(Preamble(1e6, 4, 0.5, -1.0, 2, True), SAMPLES),
    ),
    "set fft source": (lambda s: setattr(s.fft, "source", 4), {}, ["FFT:SOUR CHAN4"], None),
    "get fft source": (lambda s: s.fft.source, {"FFT:SOUR?": b"CHAN2\n"}, [], 2),
    "set window": (
        lambda s: setattr(s.fft, "window", Window.BLACKMAN_HARRIS),
        {},
        ["FFT:WIND BHAR"],
        None,
    ),
    "get window": (lambda s: s.fft.window, {"FFT:WIND?": b"RECT\n"}, [], Window.RECTANGULAR),
    "fft data": (
        lambda s: s.fft.spectrum(),
        {"FFT:DATA?": float32_block((-3.0, -300.0))},
        ["FFT:DATA?"],
        (-3.0, -300.0),
    ),
    "fft spacing": (
        lambda s: s.fft.bin_spacing(),
        {"FFT:SPAC?": b"122.0703125\n"},
        ["FFT:SPAC?"],
        122.0703125,
    ),
    "measure": (
        lambda s: s.measure(Measurement.RISE_TIME, 3),
        {"MEAS:RIS? CHAN3": b"8E-07\n"},
        ["MEAS:RIS? CHAN3"],
        8e-7,
    ),
    "statistics": (
        lambda s: s.statistics(Measurement.PEAK_TO_PEAK, 2),
        {"MEAS:STAT? VPP,CHAN2": b"2,1.5,1,2,0.5,2\n"},
        ["MEAS:STAT? VPP,CHAN2"],
        Statistics(2.0, 1.5, 1.0, 2.0, 0.5, 2),
    ),
    "statistics without values": (
        lambda s: s.statistics(Measurement.FREQUENCY, 1),
        {"MEAS:STAT? FREQ,CHAN1": b"9.91E37,9.91E37,9.91E37,9.91E37,9.91E37,0\n"},
        ["MEAS:STAT? FREQ,CHAN1"],
        Statistics(9.91e37, 9.91e37, 9.91e37, 9.91e37, 9.91e37, 0),
    ),
    "reset statistics": (lambda s: s.reset_statistics(), {}, ["MEAS:STAT:RES"], None),
}


@pytest.mark.parametrize(("action", "replies", "lines", "result"), CASES.values(), ids=CASES)
def test_each_method_sends_its_command_and_reads_its_reply(connect, action, replies, lines, result):
    connected = connect(scripted(replies))

    assert action(connected.scope) == result
    sent = connected.commands()
    assert sent == (lines or list(replies))
    assert connected.fake.lines.count("SYST:ERR?") == len(sent)  # Each line is checked.


def test_every_measurement_has_its_mnemonic(connect):
    mnemonics = ["VPP", "VMIN", "VMAX", "VAV", "VRMS", "FREQ", "PER", "DUTY", "RIS", "FALL"]
    connected = connect(scripted({f"MEAS:{m}? CHAN1": b"1\n" for m in mnemonics}))

    for measurement in Measurement:
        connected.scope.measure(measurement, 1)

    assert connected.commands() == [f"MEAS:{m}? CHAN1" for m in mnemonics]


def test_waveform_times_count_from_the_time_zero_sample():
    waveform = WaveformData(Preamble(1e3, 4, 1.0, 0.0, 1, False), (0.0,) * 4)

    assert waveform.times() == (-0.001, 0.0, 0.001, 0.002)


def test_a_fault_duration_needs_its_magnitude(connect):
    connected = connect(scripted())

    with pytest.raises(ValueError, match="magnitude"):
        connected.scope.generator(1).inject_fault(FaultKind.GLITCH, duration=3)
    assert connected.fake.lines == []


@pytest.mark.parametrize(
    ("action", "replies"),
    [
        pytest.param(lambda s: s.generator(1).frequency, {"SOUR1:FREQ?": b"fast\n"}, id="number"),
        pytest.param(lambda s: s.acquisition.record_length, {"ACQ:POIN?": b"1E+04\n"}, id="whole"),
        pytest.param(lambda s: s.generator(1).function, {"SOUR1:FUNC?": b"SINE\n"}, id="keyword"),
        pytest.param(lambda s: s.channel(1).enabled, {"CHAN1:DISP?": b"ON\n"}, id="flag"),
        pytest.param(lambda s: s.trigger.source, {"TRIG:SOUR?": b"CHAN5\n"}, id="channel-range"),
        pytest.param(lambda s: s.waveform.source, {"WAV:SOUR?": b"CH1\n"}, id="channel-form"),
        pytest.param(
            lambda s: s.identify(), {"*IDN?": b"OpenScope,OS-4,000001\n"}, id="identity-fields"
        ),
        pytest.param(lambda s: s.wait_complete(), {"*OPC?": b"0\n"}, id="opc"),
        pytest.param(
            lambda s: s.waveform.preamble(), {"WAV:PRE?": b"1E+06,4,0.5,-1,2\n"}, id="preamble"
        ),
        pytest.param(
            lambda s: s.statistics(Measurement.RMS, 1),
            {"MEAS:STAT? VRMS,CHAN1": b"1,1,1,1,0,1.5\n"},
            id="count",
        ),
        pytest.param(lambda s: s.fft.spectrum(), {"FFT:DATA?": b"#16abcdef\n"}, id="float32"),
        pytest.param(lambda s: s.fft.spectrum(), {"FFT:DATA?": b"-3,-300\n"}, id="not-a-block"),
        pytest.param(
            lambda s: s.waveform.read(),
            {READ_WAVEFORM: waveform_reply(SAMPLES[:3])},
            id="waveform-length",
        ),
        pytest.param(lambda s: s.waveform.read(), {READ_WAVEFORM: PREAMBLE}, id="waveform-no-data"),
        pytest.param(
            lambda s: s.waveform.read(),
            {READ_WAVEFORM: b"1E+06,4;" + float32_block(SAMPLES)},
            id="waveform-preamble",
        ),
    ],
)
def test_a_reply_of_the_wrong_form_is_a_protocol_error(connect, action, replies):
    connected = connect(scripted(replies))

    with pytest.raises(ProtocolError):
        action(connected.scope)


def test_statistics_parse_reads_five_numbers_and_a_whole_count():
    parsed = Statistics.parse("2,1.5,1,2,0.5,2")

    assert parsed == Statistics(2.0, 1.5, 1.0, 2.0, 0.5, 2)
    assert type(parsed.count) is int


@pytest.mark.parametrize(
    "reply", ["2,1.5,1,2,0.5", "2,1.5,1,2,0.5,2,2", "2,1.5,1,2,0.5,2.5", "x,1.5,1,2,0.5,2", ""]
)
def test_statistics_parse_refuses_any_other_reply(reply):
    with pytest.raises(ProtocolError):
        Statistics.parse(reply)
