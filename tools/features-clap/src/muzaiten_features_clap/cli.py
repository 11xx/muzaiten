from __future__ import annotations

import json
import signal
import sys
import threading
from typing import Sequence

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
    request_id = "invalid"
    try:
        try:
            payload = json.load(sys.stdin)
        except (json.JSONDecodeError, UnicodeError) as exc:
            _emit(event(request_id, "error", code="invalid_request", message=str(exc)))
            return EXIT_INVALID
        request = parse_request(payload)
        request_id = request.request_id
        result = run_request(request, _emit, canceled.is_set)
        if canceled.is_set():
            raise InterruptedError("operation canceled")
        _emit(event(request_id, "result", result=result))
        return 0
    except ProtocolError as exc:
        _emit(event(request_id, "error", code="invalid_request", message=str(exc)))
        return EXIT_INVALID
    except (FileNotFoundError, ModuleNotFoundError) as exc:
        code = "model_missing" if "checkpoint" in str(exc).lower() else "component_missing"
        _emit(event(request_id, "error", code=code, message=str(exc)))
        return EXIT_COMPONENT_MISSING
    except InterruptedError as exc:
        _emit(event(request_id, "error", code="canceled", message=str(exc)))
        return EXIT_CANCELED
    except Exception as exc:  # noqa: BLE001 - protocol boundary
        message = str(exc)
        if "optional dependencies" in message:
            _emit(event(request_id, "error", code="component_missing", message=message))
            return EXIT_COMPONENT_MISSING
        _emit(event(request_id, "error", code="operation_failed", message=message))
        return EXIT_OPERATIONAL
    finally:
        signal.signal(signal.SIGTERM, previous)


def _emit(payload: dict[str, object]) -> None:
    print(encode_event(payload), flush=True)
