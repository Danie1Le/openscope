"""The SCPI server's limits (docs/scpi-reference.md, Connecting and Lines)."""

from openscope_client import ErrorEntry

#: What a line queues when its replies, joined, would pass the most bytes one reply may hold
#: (the client's `transport.MAX_RESPONSE_BYTES`).
TOO_MUCH_DATA = ErrorEntry(-223, "Too much data")

#: The longest line the instrument runs, without its terminator.
MAX_LINE = 4096
#: The most clients the engine serves at once.
MAX_CLIENTS = 8
