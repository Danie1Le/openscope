"""The FFT over TCP against ground truth from the windows' formulas: a tone's level and bin, how
far each window spreads it, DC, the bin spacing, and per-Session settings
(docs/scpi-reference.md, FFT)."""

import cmath
import functools
import math

import pytest
from engine_helpers import take_capture
from ground_truth import SAMPLE_RATE, Signal

from openscope_client import Function, ScpiError, Window

#: The samples the FFT takes: a power-of-two record length, so every sample, and the first 8192
#: of the `*RST` record of 10000. Bins of 1E6 / 8192 = 122.0703125 Hz.
TRANSFORM_LENGTH = 8192
SPACING = SAMPLE_RATE / TRANSFORM_LENGTH
#: The bin the tones sit on or next to.
BIN = 82
#: A 2 Vpp sine, 1 V peak, in dBV RMS: 20 log10(1 / sqrt(2)).
SINE_LEVEL = 20 * math.log10(1 / math.sqrt(2))
#: A level below every window's leakage that the tests check: dB.
EMPTY = -100.0

#: Each window's coefficients a_k in sum (-1)^k a_k cos(2 pi k n / N).
COEFFICIENTS = {
    Window.RECTANGULAR: (1.0,),
    Window.HANN: (0.5, 0.5),
    Window.BLACKMAN_HARRIS: (0.35875, 0.48829, 0.14128, 0.01168),
}


@functools.cache
def window_values(window: Window) -> tuple[float, ...]:
    """The window over the record, from its documented formula (periodic, period N)."""
    terms = COEFFICIENTS[window]
    return tuple(
        sum(
            (-1) ** k * a * math.cos(2 * math.pi * k * n / TRANSFORM_LENGTH)
            for k, a in enumerate(terms)
        )
        for n in range(TRANSFORM_LENGTH)
    )


def response(window: Window, offset: float) -> float:
    """What a bin reads, in dB from a tone's level, for a tone `offset` bins above it: one term
    of the discrete Fourier transform of the windowed tone, worked out directly, with the
    window's coherent gain divided out. A complex tone: a real one adds its image at the
    negative frequency, which only matters far from the tone (see the leakage test)."""
    values = window_values(window)
    total = sum(
        value * cmath.exp(2j * math.pi * offset * n / TRANSFORM_LENGTH)
        for n, value in enumerate(values)
    )
    return 20 * math.log10(max(abs(total) / sum(values), 1e-300))


def spectrum_of(scope, frequency: float, window: Window) -> tuple[float, ...]:
    """The spectrum through `window` of one capture of a 2 Vpp sine at `frequency`."""
    Signal(Function.SINE, frequency).apply(scope)
    scope.acquisition.record_length = TRANSFORM_LENGTH
    scope.fft.window = window
    take_capture(scope)
    assert scope.fft.bin_spacing() == SPACING
    return scope.fft.spectrum()


@pytest.mark.parametrize("window", list(Window))
def test_a_bin_centred_tone_reads_its_rms_level_and_spreads_as_its_window(scope, window):
    spectrum = spectrum_of(scope, BIN * SPACING, window)

    assert len(spectrum) == TRANSFORM_LENGTH // 2 + 1
    assert max(range(len(spectrum)), key=spectrum.__getitem__) == BIN
    # The widest window spreads it over 7 bins. A bin the window leaves empty reads far under
    # -100 dB (the -300 floor, or rounding), so both sides are held at -100 there.
    for offset in range(-3, 4):
        expected = max(SINE_LEVEL + response(window, offset), EMPTY)
        assert max(spectrum[BIN + offset], EMPTY) == pytest.approx(expected, abs=0.01), offset


# The scalloping loss each window has half-way between bins, from its formula (documented as
# 3.92, 1.42 and 0.83 dB).
@pytest.mark.parametrize("window", list(Window))
def test_a_tone_between_bins_reads_low_by_its_windows_scalloping_loss(scope, window):
    spectrum = spectrum_of(scope, (BIN + 0.5) * SPACING, window)

    half_way = pytest.approx(SINE_LEVEL + response(window, 0.5), abs=0.05)
    assert spectrum[BIN : BIN + 2] == (half_way, half_way)


def test_the_windows_trade_leakage_far_from_a_tone(scope):
    far = BIN + 20  # 19.5 bins above a tone half-way between bins.
    leakage = {window: spectrum_of(scope, (BIN + 0.5) * SPACING, window)[far] for window in Window}

    assert leakage[Window.RECTANGULAR] > leakage[Window.HANN] > leakage[Window.BLACKMAN_HARRIS]
    # The real tone's image, 184.5 bins below, adds up to a tenth of the rectangular window's
    # leakage there, in a phase the test cannot know: within 1 dB.
    assert leakage[Window.RECTANGULAR] == pytest.approx(
        SINE_LEVEL + response(Window.RECTANGULAR, -19.5), abs=1.0
    )
    assert leakage[Window.BLACKMAN_HARRIS] < SINE_LEVEL - 92  # Its highest sidelobe.


@pytest.mark.parametrize("window", list(Window))
def test_a_dc_level_reads_its_voltage_in_the_first_bin(scope, window):
    Signal(Function.DC, offset=0.5).apply(scope)
    scope.fft.window = window
    take_capture(scope)

    assert scope.fft.spectrum()[0] == pytest.approx(20 * math.log10(0.5), abs=1e-4)


def test_the_reset_record_length_uses_its_first_8192_samples(scope):
    take_capture(scope)

    assert scope.fft.bin_spacing() == SPACING
    assert len(scope.fft.spectrum()) == TRANSFORM_LENGTH // 2 + 1


def test_fft_settings_belong_to_each_session(engine, open_client):
    first, second = open_client(engine.port), open_client(engine.port)
    first.channel(2).enabled = True
    take_capture(first)

    first.fft.source, first.fft.window = 2, Window.RECTANGULAR
    first.reset()

    assert (first.fft.source, first.fft.window) == (2, Window.RECTANGULAR)  # *RST keeps them.
    assert (second.fft.source, second.fft.window) == (1, Window.HANN)


def test_the_spectrum_of_a_channel_not_in_the_capture_is_a_settings_conflict(scope):
    take_capture(scope)
    scope.fft.source = 2  # Off at *RST.

    with pytest.raises(ScpiError, match="Settings conflict"):
        scope.fft.spectrum()
