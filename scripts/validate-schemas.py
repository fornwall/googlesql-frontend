#!/usr/bin/env python3
"""Validate the protocol schemas, examples, and important rejection cases."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

try:
    from jsonschema import Draft202012Validator
except ModuleNotFoundError:
    sys.exit(
        "jsonschema is required; run:\n"
        "  uv run --with 'jsonschema>=4.23,<5' python3 scripts/validate-schemas.py"
    )


ROOT = Path(__file__).resolve().parent.parent
SCHEMA_DIR = ROOT / "schema"
EXAMPLE_DIR = SCHEMA_DIR / "examples"

parser = argparse.ArgumentParser(
    description="validate protocol schemas, examples, and optional binary output"
)
parser.add_argument(
    "--binary",
    type=Path,
    help="run every request example as one NDJSON batch through this binary",
)
args = parser.parse_args()


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object member: {key}")
        value[key] = item
    return value


def load(path: Path) -> object:
    with path.open(encoding="utf-8") as source:
        return json.load(source, object_pairs_hook=reject_duplicate_keys)


request_schema = load(SCHEMA_DIR / "request.schema.json")
response_schema = load(SCHEMA_DIR / "response.schema.json")
Draft202012Validator.check_schema(request_schema)
Draft202012Validator.check_schema(response_schema)

request_validator = Draft202012Validator(request_schema)
response_validator = Draft202012Validator(response_schema)

response_validator.validate(
    {
        "protocolVersion": 1,
        "error": {
            "origin": "protocol",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "no source position",
            "inputLine": 1,
        },
    }
)

request_examples: list[tuple[Path, object]] = []
for path in sorted(EXAMPLE_DIR.glob("*.json")):
    value = load(path)
    validator = (
        request_validator if path.name.endswith("-request.json") else response_validator
    )
    errors = sorted(validator.iter_errors(value), key=lambda error: list(error.path))
    if errors:
        detail = "\n".join(f"  {path}: {error.message}" for error in errors)
        sys.exit(f"example validation failed:\n{detail}")
    if path.name.endswith("-request.json"):
        request_examples.append((path, value))

rejected_requests = [
    {"protocolVersion": 1},
    {
        "protocolVersion": 1,
        "parse": {"request": {"sqlStatement": "SELECT 1"}},
        "analyze": {"request": {"sqlStatement": "SELECT 1"}},
    },
    {
        "protocolVersion": 1,
        "parse": {"request": {"sqlStatement": "SELECT 1", "sqlExpression": "1"}},
    },
    {
        "protocolVersion": 1,
        "analyze": {"request": {"sqlStatement": "SELECT 1", "sqlExpression": "1"}},
    },
    {
        "protocolVersion": 1,
        "analyze": {"request": {"sqlStatement": "SELECT 1", "unknown": True}},
    },
    {
        "protocolVersion": 1,
        "analyze": {
            "request": {"sqlStatement": "SELECT 1", "simpleCatalog": {}},
            "namedCatalog": "CATALOG_NONE",
        },
    },
    {
        "protocolVersion": 1,
        "parse": {
            "request": {"sqlStatement": "SELECT 1"},
            "extendedRequest": {"sql": "1", "root": "EXPRESSION"},
        },
    },
    {
        "protocolVersion": 1,
        "parse": {"extendedRequest": {"sql": "1"}},
    },
    {
        "protocolVersion": 1,
        "parse": {
            "request": {"parseResumeLocation": {"input": "SELECT 1", "unknown": True}}
        },
    },
    {"protocolVersion": 1, "id": "", "parse": {"request": {"sqlStatement": ""}}},
    {"protocolVersion": 2, "parse": {"request": {"sqlStatement": "SELECT 1"}}},
]

rejected_responses = [
    {"protocolVersion": 1},
    {
        "protocolVersion": 1,
        "parse": {"response": {"parsedStatement": {}}},
    },
    {
        "protocolVersion": 1,
        "parse": {
            "response": {"parsedStatement": {}, "parsedScript": {}},
            "debugString": "",
        },
    },
    {
        "protocolVersion": 1,
        "parse": {
            "extendedResponse": {"parsedStatement": []},
            "debugString": "",
        },
    },
    {
        "protocolVersion": 1,
        "parse": {
            "response": {"parsedStatement": {}},
            "extendedResponse": {"parsedExpression": {}},
            "debugString": "",
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "bad input",
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "old field name",
            "line": 1,
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "partial location",
            "inputLine": 1,
            "location": {"line": 1, "column": 1, "byteOffset": 0},
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "invalid location line",
            "inputLine": 1,
            "location": {
                "line": 0,
                "column": 1,
                "byteOffset": 0,
                "filename": "",
            },
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "invalid byte offset",
            "inputLine": 1,
            "location": {
                "line": 1,
                "column": 1,
                "byteOffset": -1,
                "filename": "",
            },
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "missing byte offset",
            "inputLine": 1,
            "location": {
                "line": 1,
                "column": 1,
                "filename": "",
            },
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "invalid location column",
            "inputLine": 1,
            "location": {
                "line": 1,
                "column": 0,
                "byteOffset": 0,
                "filename": "",
            },
        },
    },
    {
        "protocolVersion": 1,
        "error": {
            "origin": "googlesql",
            "statusCode": 3,
            "statusName": "INVALID_ARGUMENT",
            "message": "extra location member",
            "inputLine": 1,
            "location": {
                "line": 1,
                "column": 1,
                "byteOffset": 0,
                "filename": "",
                "characterOffset": 0,
            },
        },
    },
]

for index, value in enumerate(rejected_requests, start=1):
    if request_validator.is_valid(value):
        sys.exit(f"invalid request case {index} was accepted: {value!r}")

for index, value in enumerate(rejected_responses, start=1):
    if response_validator.is_valid(value):
        sys.exit(f"invalid response case {index} was accepted: {value!r}")

try:
    json.loads(
        '{"protocolVersion":1,"protocolVersion":1}',
        object_pairs_hook=reject_duplicate_keys,
    )
except ValueError:
    pass
else:
    sys.exit("duplicate JSON object members were accepted")

if args.binary is not None:
    requests = [value for _, value in request_examples]
    ndjson = "".join(
        json.dumps(value, separators=(",", ":")) + "\n" for value in requests
    )
    try:
        process = subprocess.run(
            [str(args.binary.resolve())],
            input=ndjson,
            text=True,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        sys.exit(f"failed to run {args.binary}: {error}")
    if process.returncode != 0:
        sys.exit(f"{args.binary} exited with {process.returncode}:\n{process.stderr}")

    output_lines = process.stdout.splitlines()
    if len(output_lines) != len(requests):
        sys.exit(
            f"{args.binary} returned {len(output_lines)} responses for "
            f"{len(requests)} requests"
        )
    for index, ((path, request), line) in enumerate(
        zip(request_examples, output_lines, strict=True), start=1
    ):
        try:
            response = json.loads(line, object_pairs_hook=reject_duplicate_keys)
        except (json.JSONDecodeError, ValueError) as error:
            sys.exit(f"response {index} for {path} is invalid JSON: {error}")
        errors = sorted(
            response_validator.iter_errors(response),
            key=lambda error: list(error.path),
        )
        if errors:
            detail = "\n".join(f"  {error.message}" for error in errors)
            sys.exit(f"response {index} for {path} failed schema validation:\n{detail}")
        if response.get("id") != request.get("id"):
            sys.exit(
                f"response {index} for {path} has id {response.get('id')!r}; "
                f"expected {request.get('id')!r}"
            )
        operation = next(
            name for name in ("analyze", "parse", "builtinFunctions") if name in request
        )
        if operation not in response:
            sys.exit(
                f"response {index} for {path} did not return successful "
                f"{operation} result: {response!r}"
            )

print(
    f"validated 2 schemas, {len(list(EXAMPLE_DIR.glob('*.json')))} examples, "
    f"and {len(rejected_requests) + len(rejected_responses) + 1} rejection cases"
    + (
        f"; exercised {len(request_examples)} requests through {args.binary}"
        if args.binary is not None
        else ""
    )
)
