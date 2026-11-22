"""The PyVISA transport. Imported only when it is used, so the raw-socket transport needs nothing
beyond the standard library (install the `visa` extra for this one)."""

from collections.abc import Iterator
from contextlib import contextmanager

import pyvisa
from pyvisa.constants import StatusCode
from pyvisa.resources import MessageBasedResource


class VisaStream:
    """A Stream over a PyVISA resource, such as `TCPIP0::localhost::5025::SOCKET`. Its members
    do what Stream says."""

    def __init__(self, resource_name: str, timeout: float | None, visa_library: str = "") -> None:
        """Opens `resource_name` with the VISA library `visa_library` (PyVISA's default when
        empty; `@py` is pyvisa-py)."""
        # Kept: a resource manager closes its resources when it is collected.
        self._manager = pyvisa.ResourceManager(visa_library)
        with _as_os_errors():
            resource = self._manager.open_resource(resource_name)
        if not isinstance(resource, MessageBasedResource):
            resource.close()
            raise ValueError(f"{resource_name} is not a message-based resource")
        # The termination character makes read_line stop at a `\n`; read_exact ignores it.
        resource.read_termination = "\n"
        self._resource = resource
        self.timeout = timeout

    @property
    def timeout(self) -> float | None:
        milliseconds = self._resource.timeout
        return None if milliseconds == float("inf") else milliseconds / 1000

    @timeout.setter
    def timeout(self, seconds: float | None) -> None:
        self._resource.timeout = None if seconds is None else seconds * 1000

    def write(self, data: bytes) -> None:
        with _as_os_errors():
            self._resource.write_raw(data)

    def read_line(self) -> bytes:
        with _as_os_errors():
            return self._resource.read_raw()

    def read_exact(self, count: int) -> bytes:
        with _as_os_errors():
            return self._resource.read_bytes(count, break_on_termchar=False)

    def close(self) -> None:
        self._resource.close()


@contextmanager
def _as_os_errors() -> Iterator[None]:
    """Raises PyVISA's I/O errors as the socket transport raises them: TimeoutError for a
    timeout, ConnectionError for anything else."""
    try:
        yield
    except pyvisa.VisaIOError as error:
        if error.error_code == StatusCode.error_timeout:
            raise TimeoutError(str(error)) from error
        raise ConnectionError(str(error)) from error
