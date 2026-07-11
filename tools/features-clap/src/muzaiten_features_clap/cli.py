from __future__ import annotations

import contextlib
import json
import signal
import sys
import threading
from typing import Sequence, TextIO

from .protocol import ProtocolError, encode_event, event, parse_request, run_request

EXIT_INVALID = 2
EXIT_COMPONENT_MISSING = 3
EXIT_OPERATIONAL = 4
EXIT_CANCELED = 130


def main(argv: Sequence[str] | None = None) -> int:
    if argv:
        print("muzaiten-features-clap accepts one JSON request on stdin", file=sys.stderr)
        return EXIT_INVALID

    canceled = threading.Event()
    previous = signal.getsignal(signal.SIGTERM)
    signal.signal(signal.SIGTERM, lambda _signum, _frame: canceled.set())
    protocol_stdout = sys.stdout

    def emit(payload: dict[str, object]) -> None:
        _emit(payload, protocol_stdout)

    request_id = "invalid"
    try:
        try:
            payload = json.load(sys.stdin)
        except (json.JSONDecodeError, UnicodeError) as exc:
            emit(event(request_id, "error", code="invalid_request", message=str(exc)))
            return EXIT_INVALID
        request = parse_request(payload)
        request_id = request.request_id
        # Model libraries print checkpoint diagnostics to stdout. Keep the
        # provider's stdout exclusively JSONL by routing incidental Python
        # output to stderr while retaining an explicit protocol stream.
        with contextlib.redirect_stdout(sys.stderr):
            result = run_request(request, emit, canceled.is_set)
        if canceled.is_set():
            raise InterruptedError("operation canceled")
        emit(event(request_id, "result", result=result))
        return 0
    except ProtocolError as exc:
        emit(event(request_id, "error", code="invalid_request", message=str(exc)))
        return EXIT_INVALID
    except FileNotFoundError as exc:
        emit(event(request_id, "error", code="model_missing", message=str(exc)))
        return EXIT_COMPONENT_MISSING
    except ModuleNotFoundError as exc:
        emit(event(request_id, "error", code="component_missing", message=str(exc)))
        return EXIT_COMPONENT_MISSING
    except InterruptedError as exc:
        emit(event(request_id, "error", code="canceled", message=str(exc)))
        return EXIT_CANCELED
    except Exception as exc:  # noqa: BLE001 - protocol boundary
        message = str(exc)
        if "optional dependencies" in message:
            emit(event(request_id, "error", code="component_missing", message=message))
            return EXIT_COMPONENT_MISSING
        emit(event(request_id, "error", code="operation_failed", message=message))
        return EXIT_OPERATIONAL
    finally:
        signal.signal(signal.SIGTERM, previous)


def _emit(payload: dict[str, object], stream: TextIO = sys.stdout) -> None:
    print(encode_event(payload), file=stream, flush=True)
