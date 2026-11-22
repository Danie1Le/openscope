"""What the client raises, and the error-queue entries the instrument reports."""

import re
from collections.abc import Callable
from typing import NamedTuple


class ErrorEntry(NamedTuple):
    """One entry of a Session's error queue, as `SYST:ERR?` replies it: `<code>,"<message>"`."""

    code: int
    message: str


#: What `SYST:ERR?` replies once the queue is empty.
NO_ERROR = ErrorEntry(0, "No error")


class ScpiError(Exception):
    """The instrument queued errors while it ran a line, such as `-113,"Undefined header"`.

    `code` and `message` are the first error (the oldest, which names the first cause); `errors`
    holds every one that was queued, in order. `reply` is what the line replied before or
    despite the errors, or None when it replied nothing: a measurement that is Clipped still
    replies its value (docs/scpi-reference.md, Measurement status).
    """

    def __init__(self, errors: tuple[ErrorEntry, ...], reply: bytes | None) -> None:
        first = errors[0]
        more = f" (and {len(errors) - 1} more)" if len(errors) > 1 else ""
        super().__init__(f'{first.code},"{first.message}"{more}')
        self.code = first.code
        self.message = first.message
        self.errors = errors
        self.reply = reply


class ProtocolError(Exception):
    """The instrument sent something that is not the reply the client expected: a malformed
    response, such as a bad binary block header, or a well-formed one of the wrong kind, such as
    a reply to a line sent with `write()`. After a malformed response the client also closes the
    connection, because it no longer knows where the next reply starts."""


def parsed_reply[T](parse: Callable[[str], T], text: str) -> T:
    """`parse(text)`, with a ValueError (a malformed reply, or a number too long for `int`)
    raised as ProtocolError."""
    try:
        return parse(text)
    except ValueError as error:
        raise ProtocolError(f"unexpected reply {text[:40]!r}") from error


_ERROR_ENTRY = re.compile(rb'([+-]?[0-9]+),"((?:[^"]|"")*)"')


def parse_error_entry(response: bytes) -> ErrorEntry | None:
    """The error-queue entry `response` holds, or None when it is not one."""
    match = _ERROR_ENTRY.fullmatch(response)
    if match is None:
        return None
    message = match[2].replace(b'""', b'"').decode("utf-8", "replace")
    return ErrorEntry(parsed_reply(int, match[1].decode("ascii")), message)
