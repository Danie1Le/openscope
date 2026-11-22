"""A client connected to a fake instrument (tests/conftest.py runs it over each transport)."""

from collections.abc import Callable, Iterator

import pytest
from fake_instrument import Connected, FakeInstrument, Response

from openscope_client import OpenScope


@pytest.fixture
def connect(
    open_client: Callable[[int], OpenScope],
) -> Iterator[Callable[[Callable[[str], Response]], Connected]]:
    """Starts a fake instrument that answers with a responder and connects a client to it."""
    opened: list[Connected] = []

    def start(respond: Callable[[str], Response]) -> Connected:
        fake = FakeInstrument(respond)
        connected = Connected(open_client(fake.port), fake)
        opened.append(connected)
        return connected

    yield start
    for connected in opened:
        connected.scope.close()  # First, so the fake's connection ends.
        connected.fake.close()
