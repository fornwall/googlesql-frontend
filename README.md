# googlesql-frontend

`googlesql-frontend` is a machine-readable command-line adapter for the
[GoogleSQL](https://github.com/google/googlesql) parser, analyzer, and builtin
function catalog. It accepts newline-delimited JSON (NDJSON) on standard input
and writes NDJSON on standard output.

All SQL input uses the GoogleSQL dialect; there is no dialect-selection mode.

The tool is deliberately a front end only. It does not expose the GoogleSQL
evaluator, execute queries, read table contents, or return rows.

## Protocol

Each non-empty input line is one request. The corresponding output line is one
success or error response, in the same order. A line-local error does not stop
the stream. Blank input lines are ignored. Standard output is reserved for
protocol responses; process diagnostics belong on standard error.

Every request contains `protocolVersion: 1`, an optional string `id` of 1 to
256 Unicode characters, and exactly one operation. The response repeats
`protocolVersion`, echoes a valid `id`, and contains either the matching
operation or `error`.

`analyze.request` and native `parse.request` values are canonical ProtoJSON
representations of GoogleSQL local-service requests. Builtin-function options
and successful responses likewise use canonical ProtoJSON for their GoogleSQL
messages. Extended parser roots use the explicitly documented frontend
messages below. Among other ProtoJSON rules, field names use lower camel case,
enums normally use their symbolic names, bytes use base64, and 64-bit integers
are decimal JSON strings rather than JSON numbers.

The protocol schemas are [schema/request.schema.json](schema/request.schema.json)
and [schema/response.schema.json](schema/response.schema.json). They strictly
validate the envelope and the known fields of each local-service message.
Nested catalog, analyzer-option, language-option, AST, type, and function
messages remain open to compatible upstream additions; the protobuf decoder is
the authoritative validator for those evolving messages and rejects unknown
fields. Duplicate members in any JSON object are rejected rather than silently
using the first or last value.

### Analyze

`analyze.request` is a `googlesql.local_service.AnalyzeRequest`. Its target is
exactly one of `sqlStatement`, `sqlExpression`, or `parseResumeLocation`.
Catalogs are supplied inline as a `googlesql.SimpleCatalogProto` in
`simpleCatalog`:

```json
{"protocolVersion":1,"id":"a1","analyze":{"request":{"sqlStatement":"SELECT value FROM numbers","simpleCatalog":{"name":"example","table":[{"name":"numbers","column":[{"name":"value","type":{"typeKind":"TYPE_INT64"}}]}]}}}}
```

A successful reply contains an `AnalyzeResponse` and the human-readable
resolved-tree rendering:

```json
{"protocolVersion":1,"id":"a1","analyze":{"response":{"resolvedStatement":{"resolvedQueryStmtNode":{}}},"debugString":"QueryStmt\n+-output_column_list=..."}}
```

`registeredCatalogId` remains a recognized `AnalyzeRequest` field for faithful
ProtoJSON decoding, but this CLI does not register catalogs. Standalone clients
should use `simpleCatalog`.

For compatibility with GoogleSQL's named analyzer-test catalogs,
`analyze.namedCatalog` accepts `CATALOG_NONE` or `CATALOG_SAMPLE`. It is
mutually exclusive with `request.simpleCatalog` and
`request.registeredCatalogId`:

```json
{"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT key FROM KeyValue"}}}
```

### Parse

`parse.request` is a `googlesql.local_service.ParseRequest`. Its target is
exactly one of `sqlStatement` or `parseResumeLocation`. Set `allowScript` to
parse a complete script from `sqlStatement`, or use `parseResumeLocation` to
parse the next statement:

```json
{"protocolVersion":1,"id":"p1","parse":{"request":{"sqlStatement":"SELECT 1"}}}
```

The native `request` branch remains a direct call to the local-service parse
API. Roots that exist in the GoogleSQL parser but not in that API use
`extendedRequest`. Its `root` is `EXPRESSION`, `TYPE`, or `PARSE_MULTIPLE`:

```json
{"protocolVersion":1,"id":"p2","parse":{"extendedRequest":{"sql":"ARRAY<STRUCT<x INT64>>","root":"TYPE"}}}
```

An extended response uses `extendedResponse.parsedExpression`,
`extendedResponse.parsedType`, or the repeated
`extendedResponse.parsedStatement` field. These fields contain the canonical
ProtoJSON representation of the corresponding GoogleSQL AST protos.

`parse.renderOptions.outputAscExplicitly` controls whether an explicit `ASC`
is marked in that request's `debugString`. Rendering options never change the
AST response:

```json
{"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1 ORDER BY 1 ASC"},"renderOptions":{"outputAscExplicitly":true}}}
```

A successful parse reply contains a `ParseResponse` and the parse tree's human
debug rendering:

```json
{"protocolVersion":1,"id":"p1","parse":{"response":{"parsedStatement":{"astQueryStatementNode":{}}},"debugString":"QueryStatement [0-8]\n+-query=..."}}
```

### Builtin functions

`builtinFunctions.request` is a `GoogleSQLBuiltinFunctionOptionsProto`. The
response is a `googlesql.local_service.GetBuiltinFunctionsResponse`:

```json
{"protocolVersion":1,"id":"b1","builtinFunctions":{"request":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL"}}}}
```

```json
{"protocolVersion":1,"id":"b1","builtinFunctions":{"response":{"function":[],"types":{},"tableValuedFunction":[]},"debugString":""}}
```

### Errors

Malformed JSON, invalid envelopes, invalid ProtoJSON, and GoogleSQL failures
all produce one error object for that input line. `origin` identifies the layer
that rejected the request. `statusCode` and `statusName` use the canonical
Abseil status code, while `line` is the physical NDJSON input line number.
`operation` is present when the operation could be identified.

```json
{"protocolVersion":1,"id":"p2","error":{"origin":"googlesql","statusCode":3,"statusName":"INVALID_ARGUMENT","message":"Syntax error: Expected end of input but got identifier","line":7,"operation":"parse"}}
```

The process exits successfully after consuming a valid stream even when some
lines contain request errors. Nonzero exit status is reserved for process-level
failures such as invalid command-line arguments, startup failure, or an
unrecoverable standard-input/standard-output error.

## Versioning

`protocolVersion` versions the NDJSON contract. Unsupported versions are
rejected per line. `googlesql-frontend --version` reports the CLI version,
protocol version, and exact pinned GoogleSQL commit so recorded results can be
attributed to their implementation.

## Validating the schemas

The examples under `schema/examples` and important negative cases are checked
with a Draft 2020-12 validator:

```sh
uv run --with 'jsonschema>=4.23,<5' python3 scripts/validate-schemas.py
```

The command is non-interactive and exits nonzero on failure, so it can be used
directly as a continuous-integration test target. After building the binary,
the same validator can stream every request example through one process and
validate the real responses, IDs, and ordering:

```sh
uv run --with 'jsonschema>=4.23,<5' python3 scripts/validate-schemas.py \
  --binary bazel-bin/frontend/googlesql-frontend
```

## Build

The repository pins Bazel 9.2.0 and builds GoogleSQL as C++20. A local release
build with Clang is:

```sh
bazel build -c opt \
  --repo_env=CC="$(command -v clang)" \
  --repo_env=CXX="$(command -v clang++)" \
  //frontend:googlesql-frontend
```

The binary is written to `bazel-bin/frontend/googlesql-frontend`. Run the full
test suite with `bazel test -c opt --test_output=errors //...`.

## Install from a release

Each GitHub release provides `tar.zst` archives for `x86_64-linux-gnu` and
`aarch64-apple-darwin`, plus a `SHA256SUMS` file. Verify an archive before
extracting it:

```sh
sha256sum --check --ignore-missing SHA256SUMS
tar --zstd -xf googlesql-frontend-vVERSION-PLATFORM.tar.zst
install -m 0755 googlesql-frontend /usr/local/bin/googlesql-frontend
```

The Linux binary is built on Ubuntu 24.04 and uses that platform's glibc
baseline. The macOS binary is built natively for arm64 on macOS 15. ICU data is
embedded in both binaries; no separate ICU data package is required.
