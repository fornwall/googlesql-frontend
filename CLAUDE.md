# Repository guidance

## Purpose and scope

This repository builds `googlesql-frontend`, a standalone C++ command-line
adapter around GoogleSQL. It reads newline-delimited JSON requests on stdin and
writes one JSON response per non-empty input line on stdout. It parses and
analyzes GoogleSQL; it does not execute queries or return rows.

Support GoogleSQL only. Do not add a legacy SQL dialect or silently translate
legacy syntax. `FEATURE_SPANNER_LEGACY_DDL` is an upstream GoogleSQL feature
for Spanner DDL, not a Legacy SQL dialect. Preserve it for request analysis;
disable it only while constructing sample-catalog fixtures so unsupported
statements return a normal error instead of terminating the process.

Keep the repository standalone. Documentation, comments, tests, release notes,
and metadata must describe this project on its own terms and must not refer to
unrelated projects or external implementation history.

## Source and build invariants

- `MODULE.bazel` pins the exact GoogleSQL commit recorded in `GOOGLESQL_COMMIT`.
- The GoogleSQL archive applies exactly
  `patches/googlesql/pull-5.patch`. Do not add, replace, or edit upstream
  patches unless the task explicitly requires it.
- The patch checksum is enforced by the release workflow.
- Bazel is pinned by `.bazelversion`; CI and releases compile with LLVM 21.
- Release platforms are Linux x86-64 and macOS arm64 only.
- Do not edit generated `bazel-*` trees or fetched external repositories.

Representative local verification:

```sh
bazelisk test -c opt \
  --repo_env=CC=/usr/bin/clang-21 \
  --repo_env=CXX=/usr/bin/clang++-21 \
  --test_output=errors //...
bazelisk build -c opt \
  --repo_env=CC=/usr/bin/clang-21 \
  --repo_env=CXX=/usr/bin/clang++-21 \
  //frontend:googlesql-frontend
```

## Protocol invariants

- `protocolVersion` is currently `1`. Reject unsupported versions per input
  line without terminating a valid NDJSON stream.
- `frontend/protocol.proto` is the protocol source of truth. ProtoJSON field
  names are the public JSON names.
- Keep exactly two public JSON Schemas: `schema/request.schema.json` for stdin
  and `schema/response.schema.json` for stdout. Both are strict and versioned.
- Errors use `inputLine` for the physical NDJSON line. A GoogleSQL source
  position, when mappable, is the separate optional `location` object with
  1-based `line`/`column`, 0-based UTF-8 `byteOffset`, and `filename`.
- Default error prose is unpositioned. An explicit analyzer
  `errorMessageMode` remains effective, while the typed location payload is
  still attached independently.
- NDJSON `analyze.debugString` must be rendered from the live `AnalyzerOutput`.
  Do not restore the serialized resolved AST solely to render that field:
  upstream restore code drops some resolved-node parse locations.
- Property-graph definition analyzer outputs must remain alive through the
  main analysis, response serialization, and debug rendering.
- A successful `parseResumeLocation` request returns `resumeBytePosition`; a
  client can feed it back as the next `bytePosition` to walk statements.
- When named-catalog language options are omitted and no
  `languageOptionsPreset` is given, use maximum released features, all
  statement kinds, and all reservable keywords. An explicit preset replaces
  that baseline; the request's own language options are then merged on top of
  the preset's expansion, and are preserved exactly when there is no preset.
- One expansion of `LanguageOptionsPreset` serves both applying it (`analyze`,
  `parse`) and reporting it (`languageOptions.preset`), so what a client reads
  back cannot drift from what the analyzer and parser were configured with.
- Analyzer-option scalars the request leaves unset take the values of a
  default-constructed `AnalyzerOptions`, not the protobuf zero value.
  `enabled_rewrites` is repeated and carries no presence bit, so its baseline
  is named by `analyze.rewrites` instead; `REWRITES_AS_REQUESTED` is the
  default and must keep meaning exactly what the request lists.
- `responseOptions.omitResponseProto` drops only the serialized AST. It keeps
  `debugString` and any `resumeBytePosition`, omits a response message left
  with no field rather than sending an empty object, and is rejected for the
  operations whose entire reply is the response proto.
- Blank input lines, including whitespace-only ones, are skipped rather than
  reported as errors.

## Validation and release

Before pushing, run the C++ tests, schema validator, formatting checks,
Buildifier, Ruff, actionlint, and version check. The schema validator can also
exercise a built binary:

```sh
uv run --with 'jsonschema>=4.23,<5' \
  python3 scripts/validate-schemas.py \
  --binary bazel-bin/frontend/googlesql-frontend
python3 scripts/check-version.py \
  --binary bazel-bin/frontend/googlesql-frontend
```

`VERSION`, `frontend/frontend.h`, and `MODULE.bazel` must agree. Rehearse the
release workflow from `main` and require both platform builds to pass before
pushing a version tag. After tagging, wait for the release workflow and verify
that both archives and `SHA256SUMS` are published.
