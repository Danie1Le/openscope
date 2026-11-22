"""The index clock's arithmetic (engine_helpers): a 2 Vpp ramp at 1 Hz read at two sample indexes
gives the indexes between them. No engine runs here."""

import struct

import pytest
from engine_helpers import CLOCK_FREQUENCY, indexes_between
from ground_truth import SAMPLE_RATE


def ramp(index: int, start: float = 0.0) -> float:
    """The clock's value at `index`, starting `start` of the way into its cycle."""
    return -1 + 2 * ((start + index * CLOCK_FREQUENCY / SAMPLE_RATE) % 1)


def float32(value: float) -> float:
    """`value` as a float32 sample holds it."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


@pytest.mark.parametrize("start", [0.0, 0.3, 0.999])
@pytest.mark.parametrize("gap", [0, 1, 15000, 999_999])
def test_the_indexes_between_two_readings_survive_the_ramps_drop(start, gap):
    assert indexes_between(ramp(100, start), ramp(100 + gap, start)) == gap


@pytest.mark.parametrize("index", [0, 123_457, 499_999, 900_001])
def test_float32_readings_place_a_sample_exactly(index):
    gap = 31000
    assert indexes_between(float32(ramp(index)), float32(ramp(index + gap))) == gap
