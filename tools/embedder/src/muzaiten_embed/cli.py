from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

from .model import MODEL_NAME, MODEL_VERSION, RealClapEmbedder
from .ops import neighbors, query_embedding, scan, status


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="muzaiten-embed")
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan", help="embed content-group representatives")
    scan_parser.add_argument("--features", required=True, type=Path)
    scan_parser.add_argument("--limit", type=int)
    scan_parser.add_argument("--json", action="store_true")

    neighbors_parser = subparsers.add_parser("neighbors", help="rebuild cosine neighbors")
    neighbors_parser.add_argument("--features", required=True, type=Path)
    neighbors_parser.add_argument("--top-k", type=int, default=100)
    neighbors_parser.add_argument("--json", action="store_true")

    status_parser = subparsers.add_parser("status", help="show embedding coverage")
    status_parser.add_argument("--features", required=True, type=Path)
    status_parser.add_argument("--json", action="store_true")

    query_parser = subparsers.add_parser("query", help="embed a text query")
    query_parser.add_argument("text")
    query_parser.add_argument("--json", action="store_true")

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "scan":
            embedder = RealClapEmbedder()
            result = scan(args.features, embedder, limit=args.limit)
            payload = result.as_dict() | {"model": embedder.model, "version": embedder.version}
            return _emit(payload, args.json)
        if args.command == "neighbors":
            count = neighbors(args.features, MODEL_NAME, MODEL_VERSION, top_k=args.top_k)
            return _emit({"neighbor_rows": count, "model": MODEL_NAME, "version": MODEL_VERSION}, args.json)
        if args.command == "status":
            payload = status(args.features, MODEL_NAME, MODEL_VERSION).as_dict()
            payload.update({"model": MODEL_NAME, "version": MODEL_VERSION})
            return _emit(payload, args.json)
        if args.command == "query":
            embedder = RealClapEmbedder()
            vector = query_embedding(args.text, embedder)
            payload = {
                "model": embedder.model,
                "version": embedder.version,
                "dim": len(vector),
                "vector": list(vector),
            }
            return _emit(payload, args.json)
    except Exception as exc:  # noqa: BLE001 - CLI boundary
        print(f"muzaiten-embed: {exc}", file=sys.stderr)
        return 1
    return 2


def _emit(payload: dict[str, object], as_json: bool) -> int:
    if as_json:
        print(json.dumps(payload, sort_keys=True, separators=(",", ":")))
    else:
        for key, value in payload.items():
            print(f"{key}: {value}")
    return 0
