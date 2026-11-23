"""The shared helpers in engine_helpers that need no engine: eventually and scpi_error."""

import engine_helpers
import pytest
from engine_helpers import NO_SIGNAL, eventually, scpi_error

from openscope_client import ScpiError


def test_eventually_polls_until_the_check_holds():
    polls = []

    eventually(lambda: polls.append(None) or len(polls) == 3)

    assert len(polls) == 3


def test_eventually_fails_after_the_patience(monkeypatch):
    monkeypatch.setattr(engine_helpers, "PATIENCE", 0.05)

    with pytest.raises(AssertionError, match="not within the patience"):
        eventually(lambda: False)


def test_scpi_error_is_what_the_call_raised():
    error = ScpiError((NO_SIGNAL,), b"9.91E37")

    def fail() -> None:
        raise error

    assert scpi_error(fail) is error


def test_scpi_error_fails_when_the_call_raises_nothing():
    with pytest.raises(pytest.fail.Exception, match="DID NOT RAISE"):
        scpi_error(lambda: None)
