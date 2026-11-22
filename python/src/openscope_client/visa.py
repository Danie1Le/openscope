"""The PyVISA transport. Imported only when it is used, so the raw-socket transport needs nothing
beyond the standard library (install the `visa` extra for this one)."""

from collections.abc import Iterator
from contextlib import contextmanager

import pyvisa
from pyvisa.constants import StatusCode
from pyvisa.resources import MessageBasedResource

from openscope_client.transport import Stream


class VisaStream(Stream):
    """A Stream over a PyVISA resource, such as `TCPIP0::localhost::5025::SOCKET`. Its members
    do what Stream says."""

    _CHUNK = 65536

    def __init__(self, resource_name: str, timeout: float | None, visa_library: str = "") -> None:
        """Opens `resource_name` with the VISA library `visa_library` (PyVISA's default when
        empty; `@py` is pyvisa-py)."""
        super().__init__()
        # Kept: a resource manager closes its resources when it is collected.
        self._manager = pyvisa.ResourceManager(visa_library)
        with _as_os_errors():
            resource = self._manager.open_resource(resource_name)
        if not isinstance(resource, MessageBasedResource):
            resource.close()
            raise ValueError(f"{resource_name} is not a message-based resource")
        # The termination character makes a read return at a `\n`, not wait for a full chunk.
        resource.read_termination = "\n"
        self._resource = resource
        self._timeout = timeout

    @property
    def timeout(self) -> float | None:
        return self._timeout

    @timeout.setter
    def timeout(self, seconds: float | None) -> None:
        self._timeout = seconds

    def write(self, data: bytes) -> None:
        self._set_resource_timeout(self._timeout)
        with _as_os_errors():
            self._resource.write_raw(data)

    def close(self) -> None:
        self._resource.close()

    def _receive(self, timeout: float | None) -> bytes:
        self._set_resource_timeout(timeout)
        with _as_os_errors():
            # One VISA read: it returns at a `\n`, a full chunk, or a pause in what arrives.
            # shortcut: pyvisa-py checks its timeout only in a pause, so bytes trickling in
            # without one keep a read going past `timeout` until the chunk fills (Stream raises
            # TimeoutError once it returns); read in smaller chunks if that ever matters.
            return self._resource.read_bytes(
                self._CHUNK, chunk_size=self._CHUNK, break_on_termchar=True
            )

    def _set_resource_timeout(self, seconds: float | None) -> None:
        self._resource.timeout = None if seconds is None else seconds * 1000


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
