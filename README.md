> [!WARNING]
> **Experimental.** Project is AI-generated and has not seen real world usage.

# googlesql-frontend

`googlesql-frontend` is a machine-readable command-line adapter for the
[GoogleSQL](https://github.com/google/googlesql) parser, analyzer, and builtin
function catalog. It accepts newline-delimited JSON (NDJSON) on standard input
and writes NDJSON on standard output.

All SQL input uses the GoogleSQL dialect; there is no dialect-selection mode.

The tool is deliberately a front end only. It does not expose the GoogleSQL
evaluator, execute queries, read table contents, or return rows.

## Protocol

Each non-blank input line is one request; a line holding nothing but whitespace
is blank and is skipped. The corresponding output line is one success or error
response, in the same order. A line-local error does not stop the stream.
Standard output is reserved for protocol responses; process diagnostics belong
on standard error.

Every request contains `protocolVersion: 1`, an optional string `id` of 1 to
256 Unicode characters, an optional
[`responseOptions`](#response-options), and exactly one operation. The response
repeats `protocolVersion`, echoes a valid `id`, and contains either the
matching operation or `error`. A request that omits a required protocol field is
rejected by that field's JSON name, as in `protocolVersion is required`. A
request carrying more than one operation is rejected with `exactly one
operation is allowed`, by the same protocol layer that answers a request
carrying none with `operation is required`.

`analyze.request` and native `parse.request` values are canonical ProtoJSON
representations of GoogleSQL local-service requests. Builtin-function options
and successful responses likewise use canonical ProtoJSON for their GoogleSQL
messages. Extended parser roots use the explicitly documented frontend
messages below. Among other ProtoJSON rules, field names use lower camel case,
bytes use base64, and 64-bit integers are decimal JSON strings rather than JSON
numbers.

The protocol schemas are [schema/request.schema.json](schema/request.schema.json)
and [schema/response.schema.json](schema/response.schema.json). They strictly
validate the envelope and the known fields of each local-service message.
Nested catalog, analyzer-option, language-option, AST, type, and function
messages remain open to compatible upstream additions; the protobuf decoder is
the authoritative validator for those evolving messages and rejects unknown
fields. Duplicate members in any JSON object are rejected rather than silently
using the first or last value.

Write enum members as their symbolic names. ProtoJSON also accepts an enum's
numeric value, but a number that names no value of that enum is discarded as an
unknown field, so the request is answered as though the member had been
omitted rather than rejected. The schemas accept only the symbolic names for
this protocol's own enums, which is where that is caught.

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

[`responseOptions.omitResponseProto`](#response-options) asks for that
rendering without the `AnalyzeResponse` payload.

`registeredCatalogId` remains a recognized `AnalyzeRequest` field for faithful
ProtoJSON decoding, but this CLI does not register catalogs, and a request that
sets it is rejected by the protocol layer. The same holds for a
`descriptorPoolList` definition that names a `registeredId`, or that names
nothing at all. Standalone clients should use `simpleCatalog`.

For compatibility with GoogleSQL's named analyzer-test catalogs,
`analyze.namedCatalog` accepts `CATALOG_NONE` or `CATALOG_SAMPLE`. It is
mutually exclusive with `request.simpleCatalog` and
`request.registeredCatalogId`. Named catalogs use GoogleSQL's maximum released
language features, all statement kinds, and all reservable keywords when
neither `request.options.languageOptions` nor `analyze.languageOptionsPreset`
is given. An explicit `languageOptions` is otherwise used exactly as sent, or
merged over a preset as [described below](#language-option-presets):

```json
{"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT key FROM KeyValue"}}}
```

A named catalog is built once and reused by the requests that follow it naming
the same catalog under the same language options, so a stream that keeps both
fixed pays for `CATALOG_SAMPLE` once rather than per request; a stream that
changes either rebuilds on each change. Reuse does not carry anything between
requests: analysis never registers what a statement declares, so a
`CREATE TABLE` analyzed against a named catalog stays invisible to the request
after it.

Scalar fields of `request.options` that the request does not set follow
GoogleSQL's own `AnalyzerOptions` defaults rather than the protobuf zero value
for their type. Sending `"options": {}` therefore analyzes exactly as sending
no `options` at all, and an explicitly supplied value is always preserved,
including an explicit `false`. The rule covers `statementContext`,
`createNewColumnForEachProjectedOutput`, `pruneUnusedColumns`,
`allowUndeclaredParameters`, `parameterMode`, `preserveColumnAliases`,
`preserveUnnecessaryCast`, and
`replaceTableNotFoundErrorWithTvfErrorIfApplicable`. The
[`analyzerOptions`](#language-and-analyzer-options) operation reports those
defaults as this build's GoogleSQL actually computes them.

It matters most for `preserveColumnAliases`, whose `AnalyzerOptions` default is
`true`: with it unset, `SELECT SUM(Key) AS n FROM KeyValue` resolves to
`$aggregate.n#3 AS n` rather than `$aggregate.$agg1#3 AS n`.
`replaceTableNotFoundErrorWithTvfErrorIfApplicable` also defaults to `true`.
The one deliberate exception is `errorMessageMode`: an explicit mode is
preserved, while an absent one selects the unpositioned error prose described
under [Errors](#errors) instead of GoogleSQL's default.

### Resolved-AST rewrites

The analyzer-option defaults rule above cannot reach
`request.options.enabledRewrites`, and the reason is structural rather than an
oversight. It is a *repeated* field, so it carries no presence bit: an omitted
list and an explicitly empty one are the same bytes on the wire, and no amount
of inspection can tell "I did not say" from "I want none". GoogleSQL's
deserializer resolves that ambiguity towards none, which is not the set a
default-constructed `AnalyzerOptions` carries. Restoring the C++ default the
way the scalar options are restored would silently take "no rewrites" away from
every client that relies on it.

`analyze.rewrites` names the baseline instead, so both meanings stay
expressible:

| Value | Rewrites applied |
|---|---|
| `REWRITES_AS_REQUESTED` (the default) | exactly the ones `request.options.enabledRewrites` names; an omitted or empty list applies none |
| `REWRITES_DEFAULT` | GoogleSQL's `AnalyzerOptions::DefaultRewrites()`, plus the ones the request names |

It follows the same "preset plus explicit additions" shape as
[`languageOptionsPreset`](#language-option-presets): `REWRITES_DEFAULT` is a
starting point that `enabledRewrites` adds to, never a set that
`enabledRewrites` replaces. Naming a rewrite the baseline already contains is
harmless, because the enabled rewrites are a set. To subtract from the default
set, list the rewrites you want explicitly and leave `rewrites` alone.

The field applies to named and inline catalogs alike. Its default value is the
behaviour every earlier request already had, so adding it changes no existing
answer. `REWRITE_PIVOT` is in the default set, which makes it a convenient way
to see the difference:

```json
{"protocolVersion":1,"id":"r1","analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_DEFAULT","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(Value) FOR Key IN (1))"}}}
```

```json
{"protocolVersion":1,"id":"r1","analyze":{"response":{"resolvedStatement":{"resolvedQueryStmtNode":{}}},"debugString":"QueryStmt\n+-output_column_list=\n| +-$pivot._1#4 AS _1 [STRING]\n+-query=\n  +-ProjectScan\n    +-input_scan=\n      +-AggregateScan..."}}
```

Without `rewrites`, the same statement resolves to a `PivotScan` carrying
`pivot_expr_list`, `for_expr`, and `pivot_column_list`. With
`REWRITES_DEFAULT` it resolves to the `AggregateScan` over a `ProjectScan`
above, which compares a `$pivot.$pivot_value` column using
`$is_not_distinct_from`. `debugString` and the `response` payload always agree,
because both render the tree the analyzer produced after rewriting.

The rewrite baseline is an `analyze` member only, unlike
`languageOptionsPreset`. Rewriters run over the resolved AST, and `parse` stops
at the parse tree; its `request.options` is a `LanguageOptionsProto`, which has
no `enabledRewrites` member for a baseline to extend.

### Language option presets

`LanguageOptionsProto` can express only the expanded result of GoogleSQL's
`LanguageOptions` calls, so naming a baseline means listing a few hundred
feature names, derived from a source checkout that may not match the commit
this tool was built against. `analyze.languageOptionsPreset` and
`parse.languageOptionsPreset` name the baseline instead, and it is expanded by
the tool's own GoogleSQL:

| Member | Effect |
|---|---|
| `features` | `LANGUAGE_FEATURES_DEFAULT` (no optional feature), `LANGUAGE_FEATURES_MAXIMUM` (`EnableMaximumLanguageFeatures`), or `LANGUAGE_FEATURES_DEVELOPMENT` (`EnableMaximumLanguageFeaturesForDevelopment`) |
| `languageVersion` | `SetLanguageVersion`, for example `VERSION_1_3` |
| `allReservableKeywordsReserved` | `EnableAllReservableKeywords` |
| `allStatementKindsSupported` | `SetSupportsAllStatementKinds` |

The preset is expanded into a complete `LanguageOptionsProto` and the request's
own `languageOptions` is then merged on top of it. Precedence follows protobuf
merge semantics, and is worth stating exactly:

- A scalar the request sets replaces the preset's value. `productMode`,
  `nameResolutionMode`, and `errorOnDeprecatedSyntax` are the scalars.
- A repeated member the request sets is **added** to the preset's entries. That
  covers `enabledLanguageFeatures`, `supportedStatementKinds`,
  `reservedKeywords`, and the two generic entity type lists. A preset can
  therefore be extended but never trimmed; to subtract from a baseline, send
  the whole list explicitly and no preset.
- `allStatementKindsSupported` expands to an *empty* `supportedStatementKinds`,
  which GoogleSQL reads as "all kinds". Naming `supportedStatementKinds` in the
  same request therefore narrows the set to exactly those kinds instead of
  widening it.
- `languageVersion` and `features` both choose a feature set. The version is
  applied first and `features` only adds to what it leaves, so naming both
  keeps the wider set. Name one or the other.
- A language version enables only features annotated with that version.
  Unversioned features such as `FEATURE_ANALYTIC_FUNCTIONS` are not part of any
  version and have to be named explicitly.
- `LANGUAGE_FEATURES_MAXIMUM` already reserves `QUALIFY`, because GoogleSQL
  folds that one launched keyword into the maximum set;
  `LANGUAGE_FEATURES_DEVELOPMENT` reserves every reservable keyword.
- `LANGUAGE_FEATURES_DEVELOPMENT` also enables features still under
  development. Their behaviour is unstable by definition and changes between
  GoogleSQL commits, so results recorded under it are only comparable against
  the same pinned commit.
- An explicit preset replaces the named-catalog baseline described above,
  rather than layering on top of it. `"languageOptionsPreset": {}` is a
  deliberate request for GoogleSQL's own defaults.

"The maximum set plus one more feature" — the spelling that motivates the
preset — is a preset next to a one-element list:

```json
{"protocolVersion":1,"id":"a2","analyze":{"namedCatalog":"CATALOG_SAMPLE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM","allReservableKeywordsReserved":true,"allStatementKindsSupported":true},"request":{"sqlStatement":"SELECT Key + 1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_COLLATION_SUPPORT"]}}}}}
```

```json
{"protocolVersion":1,"id":"a2","analyze":{"response":{"resolvedStatement":{"resolvedQueryStmtNode":{}}},"debugString":"QueryStmt\n+-output_column_list=\n| +-$query.c#5 AS c [INT64]\n+-query=..."}}
```

That request reads as maximum-plus-collation, not as collation-only:
`FEATURE_COLLATION_SUPPORT` requires `FEATURE_ANNOTATION_FRAMEWORK`, which only
the preset supplies, and the `QUALIFY` keyword only parses because the preset
reserves it.

On `parse` the preset applies to whichever input the operation carries, merging
into `parse.request.options` or into `parse.extendedRequest.options`:

```json
{"protocolVersion":1,"parse":{"languageOptionsPreset":{"allReservableKeywordsReserved":true},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}}
```

Without the preset that statement is a *syntax* error, `QUALIFY clause must be
used in conjunction with WHERE or GROUP BY or HAVING clause`, which reads as a
dialect difference rather than as a configuration choice.

A preset is also readable: [`languageOptions.preset`](#language-and-analyzer-options)
reports the `LanguageOptionsProto` any of these presets expands to on this
build, so a configuration that was named rather than listed can still be
recorded in full.

### Parse

`parse.request` is a `googlesql.local_service.ParseRequest`. Its target is
exactly one of `sqlStatement` or `parseResumeLocation`. Set `allowScript` to
parse a complete script from `sqlStatement`, or use `parseResumeLocation` to
parse the next statement. A successful resume response includes
`parse.response.resumeBytePosition`; pass that value back as the next
`parseResumeLocation.bytePosition` to walk the input one statement at a time:

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

[`responseOptions.omitResponseProto`](#response-options) asks for that
rendering without the `ParseResponse` or `extendedResponse` payload, while
still reporting `resumeBytePosition`.

### Response options

A successful `analyze` or `parse` reply carries two renderings of the same
tree: the serialized AST in `response`, and the human-readable `debugString`.
The AST is by far the larger of the two, and a client that compares dumps never
reads it. `responseOptions.omitResponseProto` drops it:

```json
{"protocolVersion":1,"id":"omit-1","responseOptions":{"omitResponseProto":true},"parse":{"request":{"sqlStatement":"SELECT 1"}}}
```

```json
{"protocolVersion":1,"id":"omit-1","parse":{"debugString":"QueryStatement [0-8]\n  Query [0-8]\n    Select [0-8]\n..."}}
```

`responseOptions` belongs to the request envelope, beside `id`, and applies to
whichever operation the request carries. It changes only what is sent back. The
statement is parsed and analyzed exactly as it would be otherwise, so
`debugString` is byte-for-byte identical and a failing request produces the
same error object with the option as without it. On statements of a few dozen
tokens the reply loses roughly four fifths of its bytes.

Exactly these members become conditional, and on nothing but this option:

| Member | Present when |
|---|---|
| `analyze.response` | `omitResponseProto` is unset, or the request used `parseResumeLocation` |
| `analyze.response.resolvedStatement`, `analyze.response.resolvedExpression` | `omitResponseProto` is unset |
| `parse.response` | `omitResponseProto` is unset, or the request used `parseResumeLocation` |
| `parse.response.parsedStatement`, `parse.response.parsedScript` | `omitResponseProto` is unset |
| `parse.extendedResponse` | `omitResponseProto` is unset |

`debugString` is always present. `resumeBytePosition` is the one member of the
response proto that survives the option, because walking a multi-statement
input depends on it:

```json
{"protocolVersion":1,"id":"omit-resume-1","responseOptions":{"omitResponseProto":true},"analyze":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":0,"allowResume":true}}}}
```

```json
{"protocolVersion":1,"id":"omit-resume-1","analyze":{"response":{"resumeBytePosition":9},"debugString":"QueryStmt\n+-output_column_list=..."}}
```

A response message that the option would leave with no member at all is
omitted rather than sent as an empty object. That is why `analyze.response`
disappears completely from a `sqlStatement` reply, while a
`parseResumeLocation` reply keeps it for the resume position alone. The
extended parser roots have no resume position, so `parse.extendedResponse` is
always absent under the option.

`builtinFunctions`, `languageOptions`, and `analyzerOptions` answer with their
response proto and nothing else, so omitting it would leave an empty reply
rather than a cheaper one. The request schema rules the combination out, and
the protocol layer rejects it:

```json
{"protocolVersion":1,"error":{"origin":"protocol","statusCode":3,"statusName":"INVALID_ARGUMENT","message":"responseOptions.omitResponseProto would leave the builtinFunctions reply empty; it applies to analyze and parse only","inputLine":1,"operation":"builtinFunctions"}}
```

### Builtin functions

`builtinFunctions.request` is a `GoogleSQLBuiltinFunctionOptionsProto`. The
response is a `googlesql.local_service.GetBuiltinFunctionsResponse`:

```json
{"protocolVersion":1,"id":"b1","builtinFunctions":{"request":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL"}}}}
```

```json
{"protocolVersion":1,"id":"b1","builtinFunctions":{"response":{"function":[{"namePath":["concat"],"group":"GoogleSQL","mode":"SCALAR"}]}}}
```

The reply is the `function` and `tableValuedFunction` arrays and the `types`
map, and nothing else. Unlike `analyze` and `parse`, this operation has no
`debugString`: enumerating a catalog has no human-readable rendering of its
own. Each of the three members is omitted from the response object entirely
when it is empty, rather than serialized as `[]` or `{}`; the reply above
enables no table-valued function and no builtin type, so it carries only
`function`.

`response.types` is GoogleSQL's map from builtin type name to
`googlesql.TypeProto`. It holds the named types the enabled builtin functions
expose, so it is populated only when the request enables a language feature
that contributes one. Neither the default nor `PRODUCT_EXTERNAL` enables such
a feature, which is why the map is absent above. That absence is a measured
"no builtin types under these options", not an unimplemented field.

Features observed to contribute a type include `FEATURE_MULTIWAY_UNNEST` and
`FEATURE_ARRAY_ZIP` (`ARRAY_ZIP_MODE`), `FEATURE_ARRAY_FIND_FUNCTIONS`
(`ARRAY_FIND_MODE`), `FEATURE_BITWISE_AGGREGATE_BYTES_SIGNATURES`
(`BITWISE_AGG_MODE`), `FEATURE_KLL_QUANTILES_EXTRACT_RELATIVE_RANK`
(`RANK_TYPE`), `FEATURE_ROUND_WITH_ROUNDING_MODE` (`ROUNDING_MODE`), and the
differential-privacy features. That set belongs to GoogleSQL rather than to
this tool, so enable a feature and read the reply rather than treating the
list as exhaustive:

```json
{"protocolVersion":1,"builtinFunctions":{"request":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL","enabledLanguageFeatures":["FEATURE_MULTIWAY_UNNEST"]}}}}
```

That request differs from the one above only by the enabled feature, and its
reply gains a `types` member (with `function` truncated here for length):

```json
{"protocolVersion":1,"builtinFunctions":{"response":{"function":[{"namePath":["concat"],"group":"GoogleSQL","mode":"SCALAR"}],"types":{"ARRAY_ZIP_MODE":{"typeKind":"TYPE_ENUM","enumType":{"enumName":"googlesql.functions.ArrayZipEnums.ArrayZipMode","enumFileName":"googlesql/public/functions/array_zip_mode.proto","isOpaque":true}}}}}}
```

### Language and analyzer options

Two operations report the option values this build of GoogleSQL computes for
itself. They exist so a client can read the feature set and the analyzer
defaults from the tool's own vintage instead of re-deriving them from a source
checkout that may be pinned to a different commit.

`languageOptions` takes at most one of two members, and reports a
`googlesql.LanguageOptionsProto` either way. Both are optional, so
`{"languageOptions":{}}` reports GoogleSQL's defaults.

`languageOptions.request` is a
`googlesql.local_service.LanguageOptionsRequest`, with the optional members
`maximumFeatures` and `languageVersion`:

```json
{"protocolVersion":1,"id":"l1","languageOptions":{"request":{"maximumFeatures":true}}}
```

```json
{"protocolVersion":1,"id":"l1","languageOptions":{"response":{"nameResolutionMode":"NAME_RESOLUTION_DEFAULT","productMode":"PRODUCT_INTERNAL","errorOnDeprecatedSyntax":false,"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_NUMERIC_TYPE"],"supportedStatementKinds":["RESOLVED_QUERY_STMT"],"reservedKeywords":["QUALIFY"]}}}
```

`enabledLanguageFeatures` is truncated above; the real reply names every
feature in the requested set. It is a set, and GoogleSQL serializes it from an
unordered container, so treat the member order as unspecified and compare the
features as a set.

`languageOptions.preset` is the same
[`LanguageOptionsPreset`](#language-option-presets) that `analyze` and `parse`
accept, expanded and reported instead of applied. Its members cover those of
`request` — `features: LANGUAGE_FEATURES_MAXIMUM` is `maximumFeatures`, and
`languageVersion` is the same member — and add `allReservableKeywordsReserved`
and `allStatementKindsSupported`, which upstream's request has no room for.
Every configuration a preset can name is therefore one this operation can
report:

```json
{"protocolVersion":1,"id":"l2","languageOptions":{"preset":{"allReservableKeywordsReserved":true,"allStatementKindsSupported":true}}}
```

```json
{"protocolVersion":1,"id":"l2","languageOptions":{"response":{"nameResolutionMode":"NAME_RESOLUTION_DEFAULT","productMode":"PRODUCT_INTERNAL","errorOnDeprecatedSyntax":false,"reservedKeywords":["ALIGN","QUALIFY","GRAPH_TABLE","MATCH_RECOGNIZE","PER"]}}}
```

That reply is how `allStatementKindsSupported` is read as well as written:
GoogleSQL spells "every statement kind" as an *empty* `supportedStatementKinds`
list, and an empty repeated field is omitted from the response object, so the
absent member is the answer.

The expansion is produced by the code path `analyze` and `parse` run, not by a
second implementation of the same rules. A reported expansion, sent back as
`analyze.request.options.languageOptions` or as `parse.request.options`,
therefore configures exactly the analysis or the parse that naming the preset
would have configured.

The two spellings agree on every configuration but one: setting
`maximumFeatures` and `languageVersion` together makes `request` narrow to the
version, because GoogleSQL enables the maximum set first and then lets
`SetLanguageVersion` replace it, while the equivalent preset keeps the wider
set for the reason given above. Name one of the two in either spelling and the
question does not arise.

`request` and `preset` are mutually exclusive, because they describe one
configuration at two widths rather than two configurations to combine:

```json
{"protocolVersion":1,"error":{"origin":"protocol","statusCode":3,"statusName":"INVALID_ARGUMENT","message":"request and preset are mutually exclusive","inputLine":1,"operation":"languageOptions"}}
```

`analyzerOptions.request` is a
`googlesql.local_service.AnalyzerOptionsRequest`, which carries no fields. The
response is a `googlesql.AnalyzerOptionsProto` holding a default-constructed
`AnalyzerOptions`:

```json
{"protocolVersion":1,"id":"n1","analyzerOptions":{"request":{}}}
```

```json
{"protocolVersion":1,"id":"n1","analyzerOptions":{"response":{"languageOptions":{"nameResolutionMode":"NAME_RESOLUTION_DEFAULT","productMode":"PRODUCT_INTERNAL","errorOnDeprecatedSyntax":false,"supportedStatementKinds":["RESOLVED_QUERY_STMT"]},"errorMessageMode":"ERROR_MESSAGE_ONE_LINE","defaultTimezone":"America/Los_Angeles","pruneUnusedColumns":false,"allowUndeclaredParameters":false,"parameterMode":"PARAMETER_NAMED","statementContext":"CONTEXT_DEFAULT","preserveColumnAliases":true,"createNewColumnForEachProjectedOutput":false,"preserveUnnecessaryCast":false,"replaceTableNotFoundErrorWithTvfErrorIfApplicable":true}}}
```

That reply is abridged: it names the scalars this section is about and leaves
out `allowedHintsAndOptions`, `enabledRewrites`, `rewriteOptions`,
`defaultAnonFunctionReportFormat`, `defaultAnonKappaValue`, and
`logImpactOfLateralColumnReferences`, which the real reply also carries.
`enabledRewrites` is worth a second look: it is the
`AnalyzerOptions::DefaultRewrites()` set that
[`analyze.rewrites`](#resolved-ast-rewrites) names as `REWRITES_DEFAULT`, so
this operation is also how a client reads that baseline as a list.

This is the authoritative reading of the defaults [Analyze](#analyze) restores
for `request.options` scalars the request leaves unset: `preserveColumnAliases`
and `replaceTableNotFoundErrorWithTvfErrorIfApplicable` are `true` here, which
is why an omitted field is not the same as a protobuf zero value. The one field
this operation does not predict is `errorMessageMode`: it reports GoogleSQL's
`ERROR_MESSAGE_ONE_LINE`, while an `analyze` request that omits the mode gets
the unpositioned prose described under [Errors](#errors).

Both responses are open ProtoJSON objects. Their members follow the pinned
GoogleSQL commit, which is the point of asking the tool rather than a checkout,
so a client should read the members it needs and ignore the rest.

### Errors

Malformed JSON, invalid envelopes, invalid ProtoJSON, and GoogleSQL failures
all produce one error object for that input line. `origin` identifies the layer
that rejected the request, and is one of:

| `origin` | Rejected by |
|---|---|
| `proto_json` | the ProtoJSON decoder: malformed JSON, an unknown field, an unknown enum name, or a duplicate object member |
| `protocol` | this tool's own envelope and operation rules |
| `googlesql` | GoogleSQL, while parsing or analyzing the supplied SQL |
| `internal` | this tool, on a failure that indicates a bug in it |

`statusCode` and `statusName` use the canonical
Abseil status code, while `inputLine` is the physical NDJSON input line number.
`operation` is present when the operation could be identified, including on a
`proto_json` rejection whose line still names exactly one. When a
GoogleSQL source position can be mapped to the supplied SQL, `location`
contains its 1-based line and column, 0-based UTF-8 byte offset into that SQL
buffer, and filename. `location` is absent when no source position is
available. By default the message is kept free of rendered `[at line:column]`
coordinates. An explicit analyzer `errorMessageMode` is preserved, while the
typed location remains available independently.

```json
{"protocolVersion":1,"id":"p2","error":{"origin":"googlesql","statusCode":3,"statusName":"INVALID_ARGUMENT","message":"Syntax error: SELECT list must not be empty","inputLine":7,"operation":"parse","location":{"line":1,"column":8,"byteOffset":7,"filename":""}}}
```

The process exits successfully after consuming a valid stream even when some
lines contain request errors. Nonzero exit status is reserved for process-level
failures such as invalid command-line arguments, startup failure, or an
unrecoverable standard-input/standard-output error.

## Command line

The NDJSON protocol is the whole interface. The tool defines no flags of its
own, takes no positional arguments, and reads no SQL from the command line:
every request, and every option that shapes it, is a member of the request
object. Requests are read from standard input and responses are written to
standard output, so the tool composes with anything that can write a line and
read one back.

`--version` and `--help` come from Abseil's flag library, as do the flags that
GoogleSQL and Abseil register for themselves and that `--helpfull` lists. Those
belong to the linked libraries rather than to this interface; none of them is
needed to drive the tool, and a request member always wins over one.

## Versioning

`protocolVersion` versions the NDJSON contract. Unsupported versions are
rejected per line. `googlesql-frontend --version` reports the CLI version,
protocol version, and exact pinned GoogleSQL commit so recorded results can be
attributed to their implementation.

## Validating the schemas

The examples under `schema/examples`, and important cases that must be accepted
or rejected, are checked with a Draft 2020-12 validator:

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
