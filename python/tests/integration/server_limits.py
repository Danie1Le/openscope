"""The SCPI server's limits (docs/scpi-reference.md, Connecting and Lines)."""

#: The longest line the instrument runs, without its terminator.
MAX_LINE = 4096
#: The most clients the engine serves at once.
MAX_CLIENTS = 8
