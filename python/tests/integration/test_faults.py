"""Faults over TCP: a glitch, a dropout and a frequency step show in captures and measurements as
their magnitude and duration say (docs/scpi-reference.md, Faults). A fault starts at the next
sample index, which a client cannot pick: each test lets a trigger event or a Single find it."""

import time

import pytest
from engine_helpers import (
    RECORD_LENGTH,
    SETTLE,
    eventually,
    read_channel,
    take_capture,
    take_fault_capture,
)
from ground_truth import Signal, assert_measures

from openscope_client import FaultKind, Function, Measurement

#: The longest fault, in samples: over a second at the `*RST` sample rate.
LONGEST_FAULT = 1048576


def test_a_glitch_is_a_trigger_event_and_a_dropout_is_not(scope):
    # The dropout replaces 0.25 V by 0.375 V, under the 0.5 V level and its band (added like a
    # glitch, it would reach 0.625 V), and lasts through the whole capture; the default glitch
    # (+2 V for 1 sample) adds to it. Had the dropout triggered, time zero would be its start,
    # with 0.25 V before it.
    Signal(Function.DC, offset=0.25).apply(scope)
    scope.write("TRIG:MODE NORM;LEV 0.5")
    scope.single()
    time.sleep(SETTLE)  # The restarted trigger has seen 0.25 V, past its band: it is armed.
    generator = scope.generator(1)
    generator.inject_fault(FaultKind.DROPOUT, 0.375, LONGEST_FAULT)
    time.sleep(SETTLE)  # Every pre-trigger sample falls in the dropout.
    generator.inject_fault(FaultKind.GLITCH)
    scope.wait_complete()

    waveform = scope.waveform.read()
    zero = waveform.preamble.time_zero
    assert not waveform.preamble.untriggered
    assert waveform.samples == (0.375,) * zero + (2.375,) + (0.375,) * (RECORD_LENGTH - zero - 1)


def test_a_glitch_shows_its_magnitude_and_duration_after_the_pre_trigger_samples(scope):
    Signal(Function.DC, offset=0.25).apply(scope)
    scope.write("TRIG:MODE NORM;LEV 0.75")
    take_fault_capture(scope, FaultKind.GLITCH, 1.0, 3)

    waveform = scope.waveform.read()
    zero = waveform.preamble.time_zero
    assert waveform.samples == (0.25,) * zero + (1.25,) * 3 + (0.25,) * (RECORD_LENGTH - zero - 3)
    assert scope.measure(Measurement.MAXIMUM, 1) == 1.25
    assert scope.measure(Measurement.PEAK_TO_PEAK, 1) == 1.0


def test_a_dropout_replaces_the_signal_by_its_level_for_its_duration(scope):
    Signal(Function.DC, offset=0.5).apply(scope)
    scope.write("TRIG:MODE NORM;SLOP NEG;LEV 0;POS 0")
    take_fault_capture(scope, FaultKind.DROPOUT, -0.5, 200)

    assert scope.waveform.read().samples == (-0.5,) * 200 + (0.5,) * (RECORD_LENGTH - 200)
    assert scope.measure(Measurement.MINIMUM, 1) == -0.5
    mean = (0.5 * (RECORD_LENGTH - 200) - 0.5 * 200) / RECORD_LENGTH
    assert scope.measure(Measurement.AVERAGE, 1) == pytest.approx(mean)


def test_a_frequency_step_is_the_frequency_measured_until_it_ends(scope):
    # On one line the step and the Single start at the same sample index, so the whole capture
    # (about 10500 samples from there) falls inside the 100000-sample step.
    scope.write("SOUR1:FAUL FST,1000,100000;:SING")
    scope.wait_complete()
    assert_measures(scope, Signal(Function.SINE, 2e3), Measurement.FREQUENCY)

    def back_to_the_generators_frequency() -> bool:
        take_capture(scope)
        return scope.measure(Measurement.FREQUENCY, 1) == Signal(Function.SINE).approx(
            Measurement.FREQUENCY
        )

    eventually(back_to_the_generators_frequency)  # A capture after the step.


def test_a_frequency_step_leaves_its_phase_shift_until_reset(scope):
    # 250 Hz more for 1000 samples gains Generator 2 a quarter cycle on Generator 1: 250 samples
    # of the 1 kHz sine. *RST restarts both from phase zero at sample index 0, in step again.
    scope.channel(2).enabled = True
    scope.generator(2).inject_fault(FaultKind.FREQUENCY_STEP, 250, 1000)
    take_capture(scope)  # Time zero is over 5000 samples after the step ended.
    first, second = read_channel(scope, 1), read_channel(scope, 2).samples
    zero = first.preamble.time_zero
    shifted = first.samples[zero + 250 : zero + 1250]
    assert second[zero : zero + 1000] == pytest.approx(shifted, abs=1e-4)

    scope.write("*RST;:CHAN2:DISP ON")
    take_capture(scope)
    assert read_channel(scope, 2).samples == read_channel(scope, 1).samples
