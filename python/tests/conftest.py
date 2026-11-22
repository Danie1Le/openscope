"""Fixtures shared by every test: a client over each transport the package offers, so each test
that uses one runs over the raw socket and over PyVISA. Tests marked `soak` run only with
`--soak`."""

from collections.abc import Callable, Iterator
from typing import NamedTuple

import pytest

from openscope_client import OpenScope

#: Seconds a client waits for a reply: short, so a reply that never comes fails a test fast.
TIMEOUT = 2.0


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--soak", action="store_true", help="also run the tests marked soak")


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Deselects the soak tests unless `--soak` was given, whatever `-m` selects: a marker
    expression alone never starts a run that long."""
    if config.getoption("--soak"):
        return
    soak = [item for item in items if item.get_closest_marker("soak")]
    if soak:
        config.hook.pytest_deselected(items=soak)
        items[:] = [item for item in items if item not in soak]


class Transport(NamedTuple):
    """How a test reaches the instrument through one transport."""

    #: Opens a client on a loopback port.
    open: Callable[[int], OpenScope]
    #: What a read raises once the instrument has closed the connection.
    connection_lost: type[OSError]


TRANSPORTS = {
    "socket": Transport(
        lambda port: OpenScope.connect("127.0.0.1", port, TIMEOUT), ConnectionError
    ),
    # pyvisa-py does not report a closed connection: it waits for more bytes until the timeout.
    "visa": Transport(
        lambda port: OpenScope.open_visa(f"TCPIP0::127.0.0.1::{port}::SOCKET", TIMEOUT, "@py"),
        TimeoutError,
    ),
}


@pytest.fixture(params=TRANSPORTS)
def transport(request: pytest.FixtureRequest) -> Transport:
    """Each transport the client offers."""
    return TRANSPORTS[request.param]


@pytest.fixture
def open_client(transport: Transport) -> Iterator[Callable[[int], OpenScope]]:
    """Connects clients to the instrument on a loopback port over `transport`, and closes them
    after the test."""
    clients: list[OpenScope] = []

    def open_on(port: int) -> OpenScope:
        clients.append(transport.open(port))
        return clients[-1]

    yield open_on
    for client in clients:
        client.close()
