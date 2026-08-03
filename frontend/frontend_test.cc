#include "frontend/frontend.h"

#include <algorithm>
#include <string>
#include <vector>

#include "frontend/protocol.pb.h"
#include "google/protobuf/util/json_util.h"
#include "gtest/gtest.h"

namespace googlesql_frontend {
namespace {

// LanguageOptions holds its features and keywords in unordered containers, so
// a serialized LanguageOptionsProto carries them in an unspecified order.
// Compare them as sets.
std::vector<int> SortedFeatures(
    const googlesql::LanguageOptionsProto& options) {
  std::vector<int> features(options.enabled_language_features().begin(),
                            options.enabled_language_features().end());
  std::sort(features.begin(), features.end());
  return features;
}

std::vector<std::string> SortedReservedKeywords(
    const googlesql::LanguageOptionsProto& options) {
  std::vector<std::string> keywords(options.reserved_keywords().begin(),
                                    options.reserved_keywords().end());
  std::sort(keywords.begin(), keywords.end());
  return keywords;
}

FrontendResponse ParseResponse(const std::string& json) {
  FrontendResponse response;
  EXPECT_TRUE(
      google::protobuf::util::JsonStringToMessage(json, &response).ok());
  return response;
}

std::string RequestJson(const FrontendRequest& request) {
  std::string json;
  EXPECT_TRUE(google::protobuf::util::MessageToJsonString(request, &json).ok());
  return json;
}

void AddTvfSignature(googlesql::FunctionSignatureProto* signature,
                     googlesql::SignatureArgumentKind argument_kind) {
  googlesql::FunctionArgumentTypeProto* argument = signature->add_argument();
  argument->set_kind(argument_kind);
  argument->set_num_occurrences(1);
  if (argument_kind == googlesql::ARG_TYPE_FIXED) {
    argument->mutable_type()->set_type_kind(googlesql::TYPE_STRING);
  }

  googlesql::FunctionArgumentTypeProto* result =
      signature->mutable_return_type();
  result->set_kind(googlesql::ARG_TYPE_RELATION);
  result->set_num_occurrences(1);
  googlesql::TVFRelationColumnProto* column =
      result->mutable_options()->mutable_relation_input_schema()->add_column();
  column->set_name("value");
  column->mutable_type()->set_type_kind(googlesql::TYPE_INT64);
}

TEST(FrontendTest, AnalyzesStatement) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"q1","analyze":{"request":{"sqlStatement":"SELECT 1"}}})",
      1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.id(), "q1");
  ASSERT_TRUE(response.has_analyze());
  EXPECT_TRUE(response.analyze().response().has_resolved_statement());
  EXPECT_NE(response.analyze().debug_string().find("QueryStmt"),
            std::string::npos);
}

TEST(FrontendTest, AnalyzesWithInlineTypedCatalog) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"analyze":{"request":{"sqlStatement":"SELECT value FROM numbers","simpleCatalog":{"name":"example","table":[{"name":"numbers","column":[{"name":"value","type":{"typeKind":"TYPE_INT64"}}]}]}}}})",
      1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  ASSERT_TRUE(response.has_analyze());
  EXPECT_TRUE(response.analyze().response().has_resolved_statement());
  EXPECT_NE(response.analyze().debug_string().find("TableScan"),
            std::string::npos);
}

TEST(FrontendTest, AnalyzesWithNamedCatalogs) {
  Frontend frontend;
  ProcessResult none = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT ABS(-1)"}}})json",
      1);
  ASSERT_TRUE(none.ok) << none.output;
  EXPECT_NE(ParseResponse(none.output).analyze().debug_string().find("abs"),
            std::string::npos);

  ProcessResult sample = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT key FROM KeyValue"}}})json",
      2);
  ASSERT_TRUE(sample.ok) << sample.output;
  EXPECT_NE(
      ParseResponse(sample.output).analyze().debug_string().find("KeyValue"),
      std::string::npos);
}

TEST(FrontendTest, UnsetAnalyzerOptionsUseGoogleSqlDefaults) {
  Frontend frontend;
  // preserve_column_aliases defaults to true in AnalyzerOptions, while the
  // protobuf zero value is false. An absent field keeps the select-list alias.
  ProcessResult absent = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT SUM(Key) AS n FROM KeyValue"}}})json",
      1);
  ASSERT_TRUE(absent.ok) << absent.output;
  FrontendResponse absent_response = ParseResponse(absent.output);
  const std::string& absent_debug = absent_response.analyze().debug_string();
  EXPECT_NE(absent_debug.find("$aggregate.n#3 AS n"), std::string::npos)
      << absent_debug;

  ProcessResult empty_options = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT SUM(Key) AS n FROM KeyValue","options":{}}}})json",
      2);
  ASSERT_TRUE(empty_options.ok) << empty_options.output;
  FrontendResponse empty_options_response = ParseResponse(empty_options.output);
  EXPECT_EQ(empty_options_response.analyze().debug_string(), absent_debug);

  // An explicit value still wins, including an explicit false.
  ProcessResult explicit_false = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT SUM(Key) AS n FROM KeyValue","options":{"preserveColumnAliases":false}}}})json",
      3);
  ASSERT_TRUE(explicit_false.ok) << explicit_false.output;
  FrontendResponse explicit_false_response =
      ParseResponse(explicit_false.output);
  const std::string& explicit_false_debug =
      explicit_false_response.analyze().debug_string();
  EXPECT_NE(explicit_false_debug.find("$aggregate.$agg1#3 AS n"),
            std::string::npos)
      << explicit_false_debug;

  // The same rule applies to the inline-catalog path.
  ProcessResult inline_catalog = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"request":{"sqlStatement":"SELECT SUM(value) AS n FROM numbers","simpleCatalog":{"name":"example","builtinFunctionOptions":{},"table":[{"name":"numbers","column":[{"name":"value","type":{"typeKind":"TYPE_INT64"}}]}]}}}})json",
      4);
  ASSERT_TRUE(inline_catalog.ok) << inline_catalog.output;
  FrontendResponse inline_response = ParseResponse(inline_catalog.output);
  const std::string& inline_debug = inline_response.analyze().debug_string();
  EXPECT_NE(inline_debug.find("$aggregate.n#2 AS n"), std::string::npos)
      << inline_debug;
}

TEST(FrontendTest, UnsetTableNotFoundOptionUsesGoogleSqlDefault) {
  Frontend frontend;
  // replace_table_not_found_error_with_tvf_error_if_applicable also defaults
  // to true while the protobuf zero value is false.
  ProcessResult absent = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT 1 FROM tvf_exactly_1_int64_arg"}}})json",
      1);
  ASSERT_FALSE(absent.ok);
  FrontendResponse absent_response = ParseResponse(absent.output);
  EXPECT_EQ(absent_response.error().origin(), "googlesql");
  EXPECT_NE(absent_response.error().message().find(
                "Table-valued function must be called with an argument list"),
            std::string::npos)
      << absent_response.error().message();

  ProcessResult explicit_false = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT 1 FROM tvf_exactly_1_int64_arg","options":{"replaceTableNotFoundErrorWithTvfErrorIfApplicable":false}}}})json",
      2);
  ASSERT_FALSE(explicit_false.ok);
  FrontendResponse explicit_false_response =
      ParseResponse(explicit_false.output);
  EXPECT_NE(explicit_false_response.error().message().find(
                "Table not found: tvf_exactly_1_int64_arg"),
            std::string::npos)
      << explicit_false_response.error().message();
}

TEST(FrontendTest, ResolvedDebugStringRetainsParseLocations) {
  Frontend frontend;
  ProcessResult graph = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"GRAPH aml MATCH (a:%) RETURN 1 AS x"}}})json",
      1);
  ASSERT_TRUE(graph.ok) << graph.output;
  EXPECT_NE(graph.output.find(
                R"("parseLocationRange":{"filename":"","start":19,"end":20})"),
            std::string::npos)
      << graph.output;
  FrontendResponse graph_response = ParseResponse(graph.output);
  EXPECT_NE(graph_response.analyze().debug_string().find(
                "GraphWildCardLabel(parse_location=19-20)"),
            std::string::npos)
      << graph_response.analyze().debug_string();

  ProcessResult pivot = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(value) FOR key IN (1))"}}})json",
      2);
  ASSERT_TRUE(pivot.ok) << pivot.output;
  FrontendResponse pivot_response = ParseResponse(pivot.output);
  const std::string& pivot_debug = pivot_response.analyze().debug_string();
  EXPECT_NE(pivot_debug.find("parse_location=30-40"), std::string::npos)
      << pivot_debug;
  EXPECT_NE(pivot_debug.find("ColumnRef(parse_location=45-48"),
            std::string::npos)
      << pivot_debug;
}

TEST(FrontendTest, SampleCatalogSurvivesSpannerDdlLanguageMode) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"json({"protocolVersion":1,"id":"one","analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_SPANNER_LEGACY_DDL"]}}}}})json",
      1);
  ASSERT_FALSE(result.ok);
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.id(), "one");
  EXPECT_EQ(response.error().origin(), "googlesql");
  EXPECT_EQ(response.error().status_code(), 3);
  EXPECT_EQ(response.error().status_name(), "INVALID_ARGUMENT");
  EXPECT_NE(response.error().message().find("Spanner DDL"), std::string::npos);

  ProcessResult next = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"two","parse":{"request":{"sqlStatement":"SELECT 2"}}})",
      2);
  ASSERT_TRUE(next.ok) << next.output;
  EXPECT_EQ(ParseResponse(next.output).id(), "two");
}

TEST(FrontendTest, SampleCatalogPrevalidatesAnalyzerOptions) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"collation","analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_COLLATION_SUPPORT"]}}}}})",
      1);
  ASSERT_FALSE(result.ok);
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.id(), "collation");
  EXPECT_EQ(response.error().origin(), "googlesql");
  EXPECT_EQ(response.error().status_code(), 3);
  EXPECT_EQ(response.error().status_name(), "INVALID_ARGUMENT");
  EXPECT_NE(response.error().message().find("ANNOTATION_FRAMEWORK"),
            std::string::npos);
}

TEST(FrontendTest, PresetLanguageFeaturesMergeWithExplicitOnes) {
  Frontend frontend;
  // FEATURE_COLLATION_SUPPORT alone is rejected because it needs
  // FEATURE_ANNOTATION_FRAMEWORK, which the maximum set already enables. The
  // request therefore reads as maximum-plus-collation rather than
  // collation-only.
  ProcessResult maximum_plus = frontend.ProcessLine(
      R"json({"protocolVersion":1,"id":"max-plus","analyze":{"namedCatalog":"CATALOG_SAMPLE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM"},"request":{"sqlStatement":"SELECT 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_COLLATION_SUPPORT"]}}}}})json",
      1);
  ASSERT_TRUE(maximum_plus.ok) << maximum_plus.output;
  EXPECT_EQ(ParseResponse(maximum_plus.output).id(), "max-plus");

  // The same explicit list without a preset keeps failing, so the success
  // above comes from the preset rather than from the explicit feature.
  ProcessResult explicit_only = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_COLLATION_SUPPORT"]}}}}})json",
      2);
  ASSERT_FALSE(explicit_only.ok);
  EXPECT_NE(ParseResponse(explicit_only.output)
                .error()
                .message()
                .find("ANNOTATION_FRAMEWORK"),
            std::string::npos);

  // Explicit scalars replace the preset's rather than adding to it:
  // PRODUCT_EXTERNAL survives the maximum feature set and hides INT32.
  ProcessResult scalar_override = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM"},"request":{"sqlStatement":"SELECT CAST(1 AS INT32)","options":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL"}}}}})json",
      3);
  ASSERT_FALSE(scalar_override.ok);
  EXPECT_NE(
      ParseResponse(scalar_override.output).error().message().find("INT32"),
      std::string::npos);
}

TEST(FrontendTest, PresetReservesAllReservableKeywords) {
  Frontend frontend;
  // Without reserved keywords the parser rejects the statement from the
  // qualify_clause_nonreserved production, before the analyzer sees it.
  ProcessResult nonreserved = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","languageOptionsPreset":{},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}}})json",
      1);
  ASSERT_FALSE(nonreserved.ok);
  EXPECT_NE(ParseResponse(nonreserved.output)
                .error()
                .message()
                .find("QUALIFY clause must be used in conjunction with"),
            std::string::npos);

  ProcessResult reserved = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","languageOptionsPreset":{"allReservableKeywordsReserved":true},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}}})json",
      2);
  ASSERT_TRUE(reserved.ok) << reserved.output;
  EXPECT_NE(ParseResponse(reserved.output)
                .analyze()
                .debug_string()
                .find("AnalyticScan"),
            std::string::npos);
}

TEST(FrontendTest, PresetAppliesToBothParseInputs) {
  Frontend frontend;
  ProcessResult nonreserved = frontend.ProcessLine(
      R"json({"protocolVersion":1,"parse":{"languageOptionsPreset":{},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}})json",
      1);
  ASSERT_FALSE(nonreserved.ok);
  EXPECT_NE(ParseResponse(nonreserved.output)
                .error()
                .message()
                .find("QUALIFY clause must be used in conjunction with"),
            std::string::npos);

  ProcessResult native = frontend.ProcessLine(
      R"json({"protocolVersion":1,"parse":{"languageOptionsPreset":{"allReservableKeywordsReserved":true},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}})json",
      2);
  ASSERT_TRUE(native.ok) << native.output;
  EXPECT_NE(ParseResponse(native.output).parse().debug_string().find("Qualify"),
            std::string::npos);

  // The extended parser roots read the same preset.
  ProcessResult extended_nonreserved = frontend.ProcessLine(
      R"json({"protocolVersion":1,"parse":{"languageOptionsPreset":{},"extendedRequest":{"root":"PARSE_MULTIPLE","sql":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}})json",
      3);
  ASSERT_FALSE(extended_nonreserved.ok);
  EXPECT_NE(ParseResponse(extended_nonreserved.output)
                .error()
                .message()
                .find("QUALIFY clause must be used in conjunction with"),
            std::string::npos);

  ProcessResult extended = frontend.ProcessLine(
      R"json({"protocolVersion":1,"parse":{"languageOptionsPreset":{"allReservableKeywordsReserved":true},"extendedRequest":{"root":"PARSE_MULTIPLE","sql":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS","FEATURE_QUALIFY"]}}}})json",
      4);
  ASSERT_TRUE(extended.ok) << extended.output;
  EXPECT_EQ(ParseResponse(extended.output)
                .parse()
                .extended_response()
                .parsed_statement_size(),
            1);
}

TEST(FrontendTest, PresetSupportsAllStatementKinds) {
  Frontend frontend;
  // The maximum feature set alone keeps LanguageOptions' default of query
  // statements only.
  ProcessResult query_only = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM"},"request":{"sqlStatement":"CREATE TABLE t (x INT64)"}}})json",
      1);
  ASSERT_FALSE(query_only.ok);
  EXPECT_NE(ParseResponse(query_only.output)
                .error()
                .message()
                .find("Statement not supported: CreateTableStatement"),
            std::string::npos);

  ProcessResult all_kinds = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM","allStatementKindsSupported":true},"request":{"sqlStatement":"CREATE TABLE t (x INT64)"}}})json",
      2);
  ASSERT_TRUE(all_kinds.ok) << all_kinds.output;
  EXPECT_NE(ParseResponse(all_kinds.output)
                .analyze()
                .debug_string()
                .find("CreateTableStmt"),
            std::string::npos);

  // Because the empty statement-kind list means "all", naming kinds explicitly
  // alongside allStatementKindsSupported narrows the set to exactly those.
  ProcessResult narrowed = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_MAXIMUM","allStatementKindsSupported":true},"request":{"sqlStatement":"SELECT 1","options":{"languageOptions":{"supportedStatementKinds":["RESOLVED_CREATE_TABLE_STMT"]}}}}})json",
      3);
  ASSERT_FALSE(narrowed.ok);
  EXPECT_NE(ParseResponse(narrowed.output)
                .error()
                .message()
                .find("Statement not supported: QueryStatement"),
            std::string::npos);
}

TEST(FrontendTest, PresetDevelopmentAndLanguageVersionAreAccepted) {
  Frontend frontend;
  ProcessResult development = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{"features":"LANGUAGE_FEATURES_DEVELOPMENT"},"request":{"sqlStatement":"SELECT 1"}}})json",
      1);
  ASSERT_TRUE(development.ok) << development.output;

  // A language version enables the features frozen into that version.
  // FEATURE_QUALIFY belongs to v1.3, while FEATURE_ANALYTIC_FUNCTIONS carries
  // no version annotation and so must be named explicitly on top of it.
  ProcessResult version = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","languageOptionsPreset":{"languageVersion":"VERSION_1_3","allReservableKeywordsReserved":true},"request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_ANALYTIC_FUNCTIONS"]}}}}})json",
      2);
  ASSERT_TRUE(version.ok) << version.output;
  EXPECT_NE(ParseResponse(version.output)
                .analyze()
                .debug_string()
                .find("AnalyticScan"),
            std::string::npos);
}

TEST(FrontendTest, NamedCatalogBaselineSurvivesWithoutAPreset) {
  Frontend frontend;
  // Neither languageOptions nor languageOptionsPreset: the named-catalog
  // baseline of maximum features, all statement kinds, and all reservable
  // keywords still applies.
  ProcessResult baseline = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY Key) = 1"}}})json",
      1);
  ASSERT_TRUE(baseline.ok) << baseline.output;

  ProcessResult statement_kind = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"CREATE TABLE t (x INT64)"}}})json",
      2);
  ASSERT_TRUE(statement_kind.ok) << statement_kind.output;

  // An explicit preset takes over from that baseline.
  ProcessResult preset = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":{},"request":{"sqlStatement":"CREATE TABLE t (x INT64)"}}})json",
      3);
  ASSERT_FALSE(preset.ok);
  EXPECT_NE(ParseResponse(preset.output)
                .error()
                .message()
                .find("Statement not supported: CreateTableStatement"),
            std::string::npos);
}

TEST(FrontendTest, DefaultRewriteSetIsOptIn) {
  Frontend frontend;
  // AnalyzerOptionsProto.enabled_rewrites is repeated and therefore has no
  // presence bit, so an absent list keeps meaning "no rewrite" as it always
  // has. The PIVOT clause survives into the resolved tree.
  ProcessResult absent = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(Value) FOR Key IN (1))"}}})json",
      1);
  ASSERT_TRUE(absent.ok) << absent.output;
  FrontendResponse absent_response = ParseResponse(absent.output);
  const std::string& absent_debug = absent_response.analyze().debug_string();
  EXPECT_NE(absent_debug.find("PivotScan"), std::string::npos) << absent_debug;
  EXPECT_NE(absent_debug.find("pivot_column_list="), std::string::npos)
      << absent_debug;

  // REWRITES_DEFAULT asks for AnalyzerOptions::DefaultRewrites() instead,
  // which contains REWRITE_PIVOT: the scan becomes an aggregation over a
  // projection that compares the pivot value.
  ProcessResult defaults = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_DEFAULT","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(Value) FOR Key IN (1))"}}})json",
      2);
  ASSERT_TRUE(defaults.ok) << defaults.output;
  FrontendResponse defaults_response = ParseResponse(defaults.output);
  const std::string& defaults_debug =
      defaults_response.analyze().debug_string();
  EXPECT_EQ(defaults_debug.find("PivotScan"), std::string::npos)
      << defaults_debug;
  EXPECT_NE(defaults_debug.find("AggregateScan"), std::string::npos)
      << defaults_debug;
  EXPECT_NE(defaults_debug.find("$pivot.$pivot_value#5"), std::string::npos)
      << defaults_debug;
  EXPECT_NE(defaults_debug.find("$is_not_distinct_from"), std::string::npos)
      << defaults_debug;

  // REWRITES_AS_REQUESTED is the default value, so naming it explicitly beside
  // an explicit list is byte-for-byte what the same request meant before the
  // field existed.
  ProcessResult explicit_list = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(Value) FOR Key IN (1))","options":{"enabledRewrites":["REWRITE_PIVOT"]}}}})json",
      3);
  ASSERT_TRUE(explicit_list.ok) << explicit_list.output;
  ProcessResult as_requested = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_AS_REQUESTED","request":{"sqlStatement":"SELECT * FROM KeyValue PIVOT (MAX(Value) FOR Key IN (1))","options":{"enabledRewrites":["REWRITE_PIVOT"]}}}})json",
      4);
  ASSERT_TRUE(as_requested.ok) << as_requested.output;
  EXPECT_EQ(as_requested.output, explicit_list.output);
  EXPECT_EQ(ParseResponse(as_requested.output).analyze().debug_string(),
            defaults_debug);
}

TEST(FrontendTest, DefaultRewriteSetAddsTheRequestsOwnRewrites) {
  Frontend frontend;
  // One statement that reaches two rewriters: the PIVOT clause belongs to
  // REWRITE_PIVOT, which GoogleSQL enables by default, and the ordered
  // ARRAY_AGG to REWRITE_ORDER_BY_AND_LIMIT_IN_AGGREGATE, which it does not.
  // Each leaves an unmistakable mark, so one tree tells the combinations apart.
  const std::string statement =
      "SELECT ARRAY_AGG(_1 ORDER BY _1) AS a FROM (SELECT * FROM KeyValue "
      "PIVOT (MAX(Value) FOR Key IN (1)))";
  const std::string extra =
      R"json("options":{"enabledRewrites":["REWRITE_ORDER_BY_AND_LIMIT_IN_AGGREGATE"]})json";

  // The named rewrite alone: the ordered aggregate is rewritten, and the
  // default REWRITE_PIVOT is not applied.
  ProcessResult requested = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":")json" +
          statement + R"json(",)json" + extra + "}}}",
      1);
  ASSERT_TRUE(requested.ok) << requested.output;
  FrontendResponse requested_response = ParseResponse(requested.output);
  const std::string& requested_debug =
      requested_response.analyze().debug_string();
  EXPECT_NE(requested_debug.find("PivotScan"), std::string::npos)
      << requested_debug;
  EXPECT_NE(requested_debug.find("$agg_rewriter"), std::string::npos)
      << requested_debug;

  // The default set alone: the reverse.
  ProcessResult defaults = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_DEFAULT","request":{"sqlStatement":")json" +
          statement + "\"}}}",
      2);
  ASSERT_TRUE(defaults.ok) << defaults.output;
  FrontendResponse defaults_response = ParseResponse(defaults.output);
  const std::string& defaults_debug =
      defaults_response.analyze().debug_string();
  EXPECT_EQ(defaults_debug.find("PivotScan"), std::string::npos)
      << defaults_debug;
  EXPECT_EQ(defaults_debug.find("$agg_rewriter"), std::string::npos)
      << defaults_debug;

  // Both together: the explicit entry is added to the baseline rather than
  // replacing it, so each rewriter leaves its mark on the same tree.
  ProcessResult both = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_DEFAULT","request":{"sqlStatement":")json" +
          statement + R"json(",)json" + extra + "}}}",
      3);
  ASSERT_TRUE(both.ok) << both.output;
  FrontendResponse both_response = ParseResponse(both.output);
  const std::string& both_debug = both_response.analyze().debug_string();
  EXPECT_EQ(both_debug.find("PivotScan"), std::string::npos) << both_debug;
  EXPECT_NE(both_debug.find("$pivot.$pivot_value"), std::string::npos)
      << both_debug;
  EXPECT_NE(both_debug.find("$agg_rewriter"), std::string::npos) << both_debug;

  // Naming a rewrite the baseline already contains is not an error and does
  // not change the result: enabled_rewrites is a set.
  ProcessResult redundant = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_SAMPLE","rewrites":"REWRITES_DEFAULT","request":{"sqlStatement":")json" +
          statement +
          R"json(","options":{"enabledRewrites":["REWRITE_PIVOT"]}}}})json",
      4);
  ASSERT_TRUE(redundant.ok) << redundant.output;
  EXPECT_EQ(ParseResponse(redundant.output).analyze().debug_string(),
            defaults_debug);
}

TEST(FrontendTest, RewriteSetReachesTheInlineCatalogPath) {
  Frontend frontend;
  const std::string statement =
      R"json("request":{"sqlStatement":"SELECT * FROM numbers PIVOT (MAX(v) FOR k IN (1))","options":{"languageOptions":{"enabledLanguageFeatures":["FEATURE_PIVOT"]}},"simpleCatalog":{"name":"example","builtinFunctionOptions":{},"table":[{"name":"numbers","column":[{"name":"k","type":{"typeKind":"TYPE_INT64"}},{"name":"v","type":{"typeKind":"TYPE_INT64"}}]}]}}}})json";

  ProcessResult absent = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{)json" + statement, 1);
  ASSERT_TRUE(absent.ok) << absent.output;
  FrontendResponse absent_response = ParseResponse(absent.output);
  EXPECT_NE(absent_response.analyze().debug_string().find("PivotScan"),
            std::string::npos)
      << absent.output;

  ProcessResult defaults = frontend.ProcessLine(
      R"json({"protocolVersion":1,"analyze":{"rewrites":"REWRITES_DEFAULT",)json" +
          statement,
      2);
  ASSERT_TRUE(defaults.ok) << defaults.output;
  FrontendResponse defaults_response = ParseResponse(defaults.output);
  const std::string& defaults_debug =
      defaults_response.analyze().debug_string();
  EXPECT_EQ(defaults_debug.find("PivotScan"), std::string::npos)
      << defaults_debug;
  EXPECT_NE(defaults_debug.find("AggregateScan"), std::string::npos)
      << defaults_debug;
  EXPECT_NE(defaults_debug.find("$pivot.$pivot_value#5"), std::string::npos)
      << defaults_debug;
}

TEST(FrontendTest, ReportsLanguageOptionsOfThisBuild) {
  Frontend frontend;
  ProcessResult defaults = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"lo","languageOptions":{"request":{}}})", 1);
  ASSERT_TRUE(defaults.ok) << defaults.output;
  FrontendResponse defaults_response = ParseResponse(defaults.output);
  EXPECT_EQ(defaults_response.id(), "lo");
  ASSERT_TRUE(defaults_response.has_language_options());
  EXPECT_EQ(defaults_response.language_options()
                .response()
                .enabled_language_features_size(),
            0);

  // The request message is optional; omitting it reads the same defaults.
  ProcessResult omitted =
      frontend.ProcessLine(R"({"protocolVersion":1,"languageOptions":{}})", 2);
  ASSERT_TRUE(omitted.ok) << omitted.output;
  EXPECT_EQ(ParseResponse(omitted.output)
                .language_options()
                .response()
                .enabled_language_features_size(),
            0);

  ProcessResult maximum = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{"request":{"maximumFeatures":true}}})",
      3);
  ASSERT_TRUE(maximum.ok) << maximum.output;
  FrontendResponse maximum_response = ParseResponse(maximum.output);
  const googlesql::LanguageOptionsProto& maximum_options =
      maximum_response.language_options().response();
  EXPECT_GT(maximum_options.enabled_language_features_size(), 100);

  // The reply is this build's own vintage: the same feature set the
  // LANGUAGE_FEATURES_MAXIMUM preset applies, including the QUALIFY
  // reservation that GoogleSQL makes part of it.
  bool has_qualify = false;
  for (const std::string& keyword : maximum_options.reserved_keywords()) {
    has_qualify = has_qualify || keyword == "QUALIFY";
  }
  EXPECT_TRUE(has_qualify) << maximum.output;

  ProcessResult version = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{"request":{"languageVersion":"VERSION_1_3"}}})",
      4);
  ASSERT_TRUE(version.ok) << version.output;
  FrontendResponse version_response = ParseResponse(version.output);
  const googlesql::LanguageOptionsProto& version_options =
      version_response.language_options().response();
  EXPECT_GT(version_options.enabled_language_features_size(), 0);
  EXPECT_LT(version_options.enabled_language_features_size(),
            maximum_options.enabled_language_features_size());
}

TEST(FrontendTest, ReportsPresetExpansionOfThisBuild) {
  Frontend frontend;
  auto expansion = [&frontend](const std::string& preset, int line) {
    ProcessResult result = frontend.ProcessLine(
        R"({"protocolVersion":1,"languageOptions":{"preset":)" + preset + "}}",
        line);
    EXPECT_TRUE(result.ok) << result.output;
    return ParseResponse(result.output).language_options().response();
  };

  // An empty preset is LanguageOptions' own defaults: no optional feature, no
  // reserved keyword, and query statements only.
  const googlesql::LanguageOptionsProto defaults = expansion("{}", 1);
  EXPECT_EQ(defaults.enabled_language_features_size(), 0);
  EXPECT_EQ(defaults.reserved_keywords_size(), 0);
  EXPECT_EQ(defaults.supported_statement_kinds_size(), 1);

  // features, whose three named sets nest: default, maximum, development.
  const googlesql::LanguageOptionsProto maximum =
      expansion(R"({"features":"LANGUAGE_FEATURES_MAXIMUM"})", 2);
  EXPECT_GT(maximum.enabled_language_features_size(), 100);
  const googlesql::LanguageOptionsProto development =
      expansion(R"({"features":"LANGUAGE_FEATURES_DEVELOPMENT"})", 3);
  EXPECT_GT(development.enabled_language_features_size(),
            maximum.enabled_language_features_size());

  // languageVersion, which selects a smaller set than the maximum one.
  const googlesql::LanguageOptionsProto version =
      expansion(R"({"languageVersion":"VERSION_1_3"})", 4);
  EXPECT_GT(version.enabled_language_features_size(), 0);
  EXPECT_LT(version.enabled_language_features_size(),
            maximum.enabled_language_features_size());

  // allReservableKeywordsReserved, the first member upstream's own
  // LanguageOptionsRequest cannot express.
  const googlesql::LanguageOptionsProto reserved =
      expansion(R"({"allReservableKeywordsReserved":true})", 5);
  EXPECT_GT(reserved.reserved_keywords_size(), 1);
  const std::vector<std::string> keywords = SortedReservedKeywords(reserved);
  EXPECT_NE(std::find(keywords.begin(), keywords.end(), "QUALIFY"),
            keywords.end());
  // The knobs are independent: reserving keywords enables no feature.
  EXPECT_EQ(reserved.enabled_language_features_size(), 0);

  // allStatementKindsSupported, the second one. GoogleSQL spells "all kinds"
  // as the empty list, so the expansion is how a client learns that.
  const googlesql::LanguageOptionsProto all_kinds =
      expansion(R"({"allStatementKindsSupported":true})", 6);
  EXPECT_EQ(all_kinds.supported_statement_kinds_size(), 0);

  // preset is a superset of upstream's request: the two members they share
  // expand identically, so a client can move to preset without a change in
  // meaning.
  ProcessResult maximum_request = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{"request":{"maximumFeatures":true}}})",
      7);
  ASSERT_TRUE(maximum_request.ok) << maximum_request.output;
  const googlesql::LanguageOptionsProto maximum_upstream =
      ParseResponse(maximum_request.output).language_options().response();
  EXPECT_EQ(SortedFeatures(maximum), SortedFeatures(maximum_upstream));
  EXPECT_EQ(SortedReservedKeywords(maximum),
            SortedReservedKeywords(maximum_upstream));

  ProcessResult version_request = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{"request":{"languageVersion":"VERSION_1_3"}}})",
      8);
  ASSERT_TRUE(version_request.ok) << version_request.output;
  EXPECT_EQ(
      SortedFeatures(version),
      SortedFeatures(
          ParseResponse(version_request.output).language_options().response()));
}

TEST(FrontendTest, ReportedPresetExpansionIsTheOneParseAndAnalyzeApply) {
  // The guard against introspection drifting away from behaviour. For a preset
  // P, running a statement under `languageOptionsPreset: P` and running it
  // under the expansion `languageOptions` reports for P must be the same run,
  // down to the reply bytes. A reported set that were wider or narrower than
  // the applied one would change some probe's outcome, and the two replies
  // would stop matching.
  //
  // Every request below is processed as input line 1, because a rejection
  // echoes its line number and only the SQL outcome is under test here.
  Frontend frontend;
  auto reported_expansion = [&frontend](const std::string& preset) {
    ProcessResult result = frontend.ProcessLine(
        R"({"protocolVersion":1,"languageOptions":{"preset":)" + preset + "}}",
        1);
    EXPECT_TRUE(result.ok) << result.output;
    return ParseResponse(result.output).language_options().response();
  };

  // Statements whose outcome turns on the reserved-keyword set: a reserved
  // keyword cannot be an alias. QUALIFY additionally reaches the analytic
  // grammar, where reservation decides between a clause and a syntax error.
  const std::vector<std::string> statements = {
      "SELECT 1 AS QUALIFY",
      "SELECT 1 AS ALIGN",
      "SELECT 1 AS GRAPH_TABLE",
      "SELECT 1 AS MATCH_RECOGNIZE",
      "SELECT Key+1 AS c FROM KeyValue QUALIFY ROW_NUMBER() OVER (ORDER BY "
      "Key) = 1",
  };
  const std::vector<std::string> presets = {
      "{}",
      R"({"features":"LANGUAGE_FEATURES_MAXIMUM"})",
      R"({"allReservableKeywordsReserved":true})",
      R"({"features":"LANGUAGE_FEATURES_MAXIMUM","allReservableKeywordsReserved":true})",
  };

  int accepted = 0;
  int rejected = 0;
  for (const std::string& preset : presets) {
    const googlesql::LanguageOptionsProto expansion =
        reported_expansion(preset);
    for (const std::string& statement : statements) {
      ProcessResult by_preset = frontend.ProcessLine(
          R"({"protocolVersion":1,"parse":{"languageOptionsPreset":)" + preset +
              R"(,"request":{"sqlStatement":")" + statement + "\"}}}",
          1);

      FrontendRequest by_report;
      by_report.set_protocol_version(1);
      googlesql::local_service::ParseRequest* parse =
          by_report.mutable_parse()->mutable_request();
      parse->set_sql_statement(statement);
      *parse->mutable_options() = expansion;
      ProcessResult from_report =
          frontend.ProcessLine(RequestJson(by_report), 1);

      EXPECT_EQ(by_preset.output, from_report.output)
          << preset << " / " << statement;
      by_preset.ok ? ++accepted : ++rejected;
    }
  }
  // The matrix is not vacuously equal: it contains both outcomes, so an
  // expansion that lost or gained a keyword would move at least one cell.
  EXPECT_GT(accepted, 0);
  EXPECT_GT(rejected, 0);

  // Statement kinds and most features are invisible to the parser, so the same
  // round trip runs through analyze as well, where the reported expansion
  // travels as request.options.languageOptions.
  const std::string create_table = "CREATE TABLE t (x INT64)";
  const std::string typeof_call = "SELECT TYPEOF(1)";
  const std::vector<std::string> analyze_presets = {
      "{}",
      R"({"languageVersion":"VERSION_1_3"})",
      R"({"features":"LANGUAGE_FEATURES_MAXIMUM"})",
      R"({"features":"LANGUAGE_FEATURES_MAXIMUM","allStatementKindsSupported":true})",
  };
  for (const std::string& preset : analyze_presets) {
    const googlesql::LanguageOptionsProto expansion =
        reported_expansion(preset);
    for (const std::string& statement : {create_table, typeof_call}) {
      ProcessResult by_preset = frontend.ProcessLine(
          R"({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","languageOptionsPreset":)" +
              preset + R"(,"request":{"sqlStatement":")" + statement + "\"}}}",
          1);

      FrontendRequest by_report;
      by_report.set_protocol_version(1);
      by_report.mutable_analyze()->set_named_catalog(CATALOG_NONE);
      googlesql::local_service::AnalyzeRequest* analyze =
          by_report.mutable_analyze()->mutable_request();
      analyze->set_sql_statement(statement);
      *analyze->mutable_options()->mutable_language_options() = expansion;
      ProcessResult from_report =
          frontend.ProcessLine(RequestJson(by_report), 1);

      EXPECT_EQ(by_preset.output, from_report.output)
          << preset << " / " << statement;

      // Each probe is decided by one knob, which is what keeps the comparison
      // above from passing vacuously: CREATE TABLE needs the statement-kind
      // knob, and TYPEOF needs a feature set that version 1.3 already carries.
      const bool expected_ok =
          statement == create_table
              ? preset.find("allStatementKindsSupported") != std::string::npos
              : preset != "{}";
      EXPECT_EQ(by_preset.ok, expected_ok)
          << preset << " / " << statement << ": " << by_preset.output;
    }
  }
}

TEST(FrontendTest, RejectsLanguageOptionsRequestBesidePreset) {
  Frontend frontend;
  ProcessResult conflict = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"lc","languageOptions":{"request":{"maximumFeatures":true},"preset":{"features":"LANGUAGE_FEATURES_MAXIMUM"}}})",
      1);
  ASSERT_FALSE(conflict.ok);
  FrontendResponse conflict_response = ParseResponse(conflict.output);
  EXPECT_EQ(conflict_response.id(), "lc");
  EXPECT_EQ(conflict_response.error().origin(), "protocol");
  EXPECT_EQ(conflict_response.error().operation(), "languageOptions");
  EXPECT_EQ(conflict_response.error().message(),
            "request and preset are mutually exclusive");
  EXPECT_FALSE(conflict_response.error().has_location());

  // Presence is what conflicts, not content: an empty request object beside a
  // preset still states one configuration twice.
  ProcessResult empty_request = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{"request":{},"preset":{}}})",
      2);
  ASSERT_FALSE(empty_request.ok);
  EXPECT_EQ(ParseResponse(empty_request.output).error().message(),
            "request and preset are mutually exclusive");

  // Neither member remains valid, and still reports GoogleSQL's defaults.
  ProcessResult neither =
      frontend.ProcessLine(R"({"protocolVersion":1,"languageOptions":{}})", 3);
  ASSERT_TRUE(neither.ok) << neither.output;
  EXPECT_EQ(ParseResponse(neither.output)
                .language_options()
                .response()
                .enabled_language_features_size(),
            0);
}

TEST(FrontendTest, ReportsAnalyzerOptionDefaultsOfThisBuild) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"ao","analyzerOptions":{"request":{}}})", 1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.id(), "ao");
  ASSERT_TRUE(response.has_analyzer_options());

  // These are the defaults the frontend restores for analyzer options the
  // request leaves unset, so this operation is how a client discovers them.
  const googlesql::AnalyzerOptionsProto& options =
      response.analyzer_options().response();
  EXPECT_TRUE(options.preserve_column_aliases());
  EXPECT_TRUE(
      options.replace_table_not_found_error_with_tvf_error_if_applicable());
  EXPECT_EQ(options.statement_context(), googlesql::CONTEXT_DEFAULT);
  EXPECT_EQ(options.parameter_mode(), googlesql::PARAMETER_NAMED);

  ProcessResult omitted =
      frontend.ProcessLine(R"({"protocolVersion":1,"analyzerOptions":{}})", 2);
  ASSERT_TRUE(omitted.ok) << omitted.output;
  EXPECT_TRUE(ParseResponse(omitted.output)
                  .analyzer_options()
                  .response()
                  .preserve_column_aliases());
}

TEST(FrontendTest, NamesOptionOperationsInErrors) {
  Frontend frontend;
  ProcessResult conflict = frontend.ProcessLine(
      R"({"protocolVersion":1,"languageOptions":{},"analyzerOptions":{}})", 1);
  ASSERT_FALSE(conflict.ok);
  EXPECT_EQ(ParseResponse(conflict.output).error().message(),
            "exactly one operation is allowed; found languageOptions and "
            "analyzerOptions");

  ProcessResult unsupported_version =
      frontend.ProcessLine(R"({"protocolVersion":2,"analyzerOptions":{}})", 2);
  ASSERT_FALSE(unsupported_version.ok);
  FrontendResponse version_response = ParseResponse(unsupported_version.output);
  EXPECT_EQ(version_response.error().origin(), "protocol");
  EXPECT_EQ(version_response.error().operation(), "analyzerOptions");
  EXPECT_FALSE(version_response.error().has_location());
}

TEST(FrontendTest, RejectsConflictingCatalogSelectors) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT 1","simpleCatalog":{"name":"inline"}}}})",
      1);
  ASSERT_FALSE(result.ok);
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.error().origin(), "protocol");
  EXPECT_NE(response.error().message().find("mutually exclusive"),
            std::string::npos);
}

TEST(FrontendTest, AnalyzesConnectionArgumentOnMultiSignatureTvf) {
  FrontendRequest request;
  request.set_protocol_version(1);
  googlesql::local_service::AnalyzeRequest* analyze =
      request.mutable_analyze()->mutable_request();
  analyze->set_sql_statement(
      "SELECT * FROM choose_source(CONNECTION connection1)");
  analyze->mutable_options()
      ->mutable_language_options()
      ->add_enabled_language_features(
          googlesql::FEATURE_TABLE_VALUED_FUNCTIONS);
  googlesql::SimpleCatalogProto* catalog = analyze->mutable_simple_catalog();
  catalog->set_name("inline");
  catalog->add_connection()->set_name("connection1");
  googlesql::TableValuedFunctionProto* tvf = catalog->add_custom_tvf();
  tvf->add_name_path("choose_source");
  tvf->set_group("custom");
  tvf->set_type(googlesql::FunctionEnums::FIXED_OUTPUT_SCHEMA_TVF);
  AddTvfSignature(tvf->add_signatures(), googlesql::ARG_TYPE_CONNECTION);
  AddTvfSignature(tvf->add_signatures(), googlesql::ARG_TYPE_FIXED);

  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(RequestJson(request), 1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_NE(response.analyze().debug_string().find("TVFScan"),
            std::string::npos);
  EXPECT_NE(response.analyze().debug_string().find("connection1"),
            std::string::npos);
}

TEST(FrontendTest, ParsesStatement) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1"}}})",
      1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  ASSERT_TRUE(response.has_parse());
  EXPECT_TRUE(response.parse().response().has_parsed_statement());
  EXPECT_NE(response.parse().debug_string().find("QueryStatement"),
            std::string::npos);
}

TEST(FrontendTest, ParsesNativeScriptAndResumeLocationWithHumanTrees) {
  Frontend frontend;
  ProcessResult script = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"DECLARE x INT64; SET x = 1;","allowScript":true}}})",
      1);
  ASSERT_TRUE(script.ok) << script.output;
  FrontendResponse script_response = ParseResponse(script.output);
  EXPECT_TRUE(script_response.parse().response().has_parsed_script());
  EXPECT_NE(script_response.parse().debug_string().find("Script"),
            std::string::npos);
  EXPECT_EQ(script_response.parse().debug_string().find("parsed_script"),
            std::string::npos);

  ProcessResult resume = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":0,"allowResume":true}}}})",
      2);
  ASSERT_TRUE(resume.ok) << resume.output;
  FrontendResponse resume_response = ParseResponse(resume.output);
  EXPECT_GT(resume_response.parse().response().resume_byte_position(), 0);
  EXPECT_NE(resume_response.parse().debug_string().find("QueryStatement"),
            std::string::npos);
}

TEST(FrontendTest, ParsesExtendedRoots) {
  Frontend frontend;
  ProcessResult expression = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"extendedRequest":{"sql":"1 + 2","root":"EXPRESSION"}}})",
      1);
  ASSERT_TRUE(expression.ok) << expression.output;
  FrontendResponse expression_response = ParseResponse(expression.output);
  EXPECT_TRUE(
      expression_response.parse().extended_response().has_parsed_expression());
  EXPECT_NE(expression_response.parse().debug_string().find("BinaryExpression"),
            std::string::npos);

  ProcessResult type = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"extendedRequest":{"sql":"ARRAY<STRUCT<x INT64>>","root":"TYPE"}}})",
      2);
  ASSERT_TRUE(type.ok) << type.output;
  FrontendResponse type_response = ParseResponse(type.output);
  EXPECT_TRUE(type_response.parse().extended_response().has_parsed_type());
  EXPECT_NE(type_response.parse().debug_string().find("ArrayType"),
            std::string::npos);

  ProcessResult multiple = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"extendedRequest":{"sql":"SELECT 1; SELECT 2;","root":"PARSE_MULTIPLE"}}})",
      3);
  ASSERT_TRUE(multiple.ok) << multiple.output;
  FrontendResponse multiple_response = ParseResponse(multiple.output);
  EXPECT_EQ(
      multiple_response.parse().extended_response().parsed_statement_size(), 2);
  const std::string& debug = multiple_response.parse().debug_string();
  const size_t first = debug.find("QueryStatement");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(debug.find("QueryStatement", first + 1), std::string::npos);
}

TEST(FrontendTest, AppliesPerRequestRenderOptions) {
  Frontend frontend;
  ProcessResult explicit_asc = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1 ORDER BY 1 ASC"},"renderOptions":{"outputAscExplicitly":true}}})",
      1);
  ASSERT_TRUE(explicit_asc.ok) << explicit_asc.output;
  EXPECT_NE(ParseResponse(explicit_asc.output)
                .parse()
                .debug_string()
                .find("ASC EXPLICITLY"),
            std::string::npos);

  ProcessResult regular = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1 ORDER BY 1 ASC"}}})",
      2);
  ASSERT_TRUE(regular.ok) << regular.output;
  EXPECT_EQ(ParseResponse(regular.output)
                .parse()
                .debug_string()
                .find("ASC EXPLICITLY"),
            std::string::npos);
}

TEST(FrontendTest, EnumeratesBuiltinFunctions) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"builtinFunctions":{"request":{}}})", 1);
  ASSERT_TRUE(result.ok) << result.output;
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_GT(response.builtin_functions().response().function_size(), 0);

  // Builtin function enumeration has no human-readable rendering of its own,
  // so the reply carries no debugString for a client to learn to ignore.
  EXPECT_EQ(result.output.find("debugString"), std::string::npos)
      << result.output;
}

TEST(FrontendTest, BuiltinFunctionTypesFollowEnabledLanguageFeatures) {
  Frontend frontend;
  // No enabled feature contributes a builtin type, so the map is empty. An
  // empty map is omitted from the response object rather than printed as {}.
  ProcessResult none = frontend.ProcessLine(
      R"({"protocolVersion":1,"builtinFunctions":{"request":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL"}}}})",
      1);
  ASSERT_TRUE(none.ok) << none.output;
  EXPECT_TRUE(ParseResponse(none.output)
                  .builtin_functions()
                  .response()
                  .types()
                  .empty());
  EXPECT_EQ(none.output.find(R"("types":)"), std::string::npos) << none.output;

  // FEATURE_MULTIWAY_UNNEST contributes the ARRAY_ZIP_MODE enum, so the same
  // operation reports a populated map. Absence above is a measured negative.
  ProcessResult zip_mode = frontend.ProcessLine(
      R"({"protocolVersion":1,"builtinFunctions":{"request":{"languageOptions":{"productMode":"PRODUCT_EXTERNAL","enabledLanguageFeatures":["FEATURE_MULTIWAY_UNNEST"]}}}})",
      2);
  ASSERT_TRUE(zip_mode.ok) << zip_mode.output;
  FrontendResponse zip_mode_response = ParseResponse(zip_mode.output);
  const auto& types = zip_mode_response.builtin_functions().response().types();
  ASSERT_TRUE(types.contains("ARRAY_ZIP_MODE")) << zip_mode.output;
  EXPECT_EQ(types.at("ARRAY_ZIP_MODE").type_kind(), googlesql::TYPE_ENUM);
}

TEST(FrontendTest, OmitsResponseProtoWhileKeepingDebugString) {
  Frontend frontend;
  const std::string statement =
      R"("analyze":{"namedCatalog":"CATALOG_SAMPLE","request":{"sqlStatement":"SELECT Key, Value FROM KeyValue WHERE Key > 3 ORDER BY Value"}}})";

  ProcessResult full =
      frontend.ProcessLine(R"({"protocolVersion":1,)" + statement, 1);
  ASSERT_TRUE(full.ok) << full.output;
  ProcessResult omitted = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},)" +
          statement,
      2);
  ASSERT_TRUE(omitted.ok) << omitted.output;

  FrontendResponse full_response = ParseResponse(full.output);
  FrontendResponse omitted_response = ParseResponse(omitted.output);
  ASSERT_TRUE(omitted_response.has_analyze());
  EXPECT_EQ(omitted_response.analyze().debug_string(),
            full_response.analyze().debug_string());
  EXPECT_TRUE(full_response.analyze().has_response());
  EXPECT_FALSE(omitted_response.analyze().has_response());
  EXPECT_EQ(omitted.output.find("resolvedStatement"), std::string::npos)
      << omitted.output;
  // The payload is the dominant cost of a reply a client only reads
  // debugString from: dropping it saves the great majority of the bytes.
  EXPECT_LT(omitted.output.size() * 3, full.output.size())
      << omitted.output.size() << " of " << full.output.size();

  // An explicit false is the documented default rather than a third mode.
  ProcessResult explicit_false = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":false},)" +
          statement,
      3);
  ASSERT_TRUE(explicit_false.ok) << explicit_false.output;
  EXPECT_EQ(explicit_false.output, full.output);

  // The same holds for parse, whose payload is the serialized parse tree.
  ProcessResult full_parse = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT Key, Value FROM KeyValue WHERE Key > 3 ORDER BY Value"}}})",
      4);
  ASSERT_TRUE(full_parse.ok) << full_parse.output;
  ProcessResult omitted_parse = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"request":{"sqlStatement":"SELECT Key, Value FROM KeyValue WHERE Key > 3 ORDER BY Value"}}})",
      5);
  ASSERT_TRUE(omitted_parse.ok) << omitted_parse.output;
  FrontendResponse omitted_parse_response = ParseResponse(omitted_parse.output);
  EXPECT_EQ(omitted_parse_response.parse().debug_string(),
            ParseResponse(full_parse.output).parse().debug_string());
  EXPECT_FALSE(omitted_parse_response.parse().has_response());
  EXPECT_EQ(omitted_parse.output.find("parsedStatement"), std::string::npos)
      << omitted_parse.output;
  EXPECT_LT(omitted_parse.output.size() * 3, full_parse.output.size())
      << omitted_parse.output.size() << " of " << full_parse.output.size();
}

TEST(FrontendTest, OmitResponseProtoPreservesResumeBytePosition) {
  Frontend frontend;
  // resume_byte_position sits beside the payload in the same upstream message,
  // outside the oneof that carries the AST, so walking a multi-statement input
  // keeps working with the payload dropped.
  ProcessResult parse = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":0,"allowResume":true}}}})",
      1);
  ASSERT_TRUE(parse.ok) << parse.output;
  FrontendResponse parse_response = ParseResponse(parse.output);
  ASSERT_TRUE(parse_response.parse().has_response());
  EXPECT_FALSE(parse_response.parse().response().has_parsed_statement());
  const int parse_resume =
      parse_response.parse().response().resume_byte_position();
  EXPECT_EQ(parse_resume, 9);
  EXPECT_NE(parse_response.parse().debug_string().find("QueryStatement"),
            std::string::npos);

  // Feeding the position back walks to the next statement, as documented.
  ProcessResult parse_next = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":9,"allowResume":true}}}})",
      2);
  ASSERT_TRUE(parse_next.ok) << parse_next.output;
  EXPECT_EQ(ParseResponse(parse_next.output)
                .parse()
                .response()
                .resume_byte_position(),
            18);

  ProcessResult analyze = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"analyze":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":0,"allowResume":true}}}})",
      3);
  ASSERT_TRUE(analyze.ok) << analyze.output;
  FrontendResponse analyze_response = ParseResponse(analyze.output);
  ASSERT_TRUE(analyze_response.analyze().has_response());
  EXPECT_FALSE(analyze_response.analyze().response().has_resolved_statement());
  EXPECT_EQ(analyze_response.analyze().response().resume_byte_position(), 9);
  EXPECT_NE(analyze_response.analyze().debug_string().find("QueryStmt"),
            std::string::npos);

  ProcessResult analyze_next = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"analyze":{"request":{"parseResumeLocation":{"input":"SELECT 1; SELECT 2","bytePosition":9,"allowResume":true}}}})",
      4);
  ASSERT_TRUE(analyze_next.ok) << analyze_next.output;
  EXPECT_EQ(ParseResponse(analyze_next.output)
                .analyze()
                .response()
                .resume_byte_position(),
            18);
}

TEST(FrontendTest, OmitsExtendedParsePayloadForEveryRoot) {
  Frontend frontend;
  ProcessResult expression = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"extendedRequest":{"sql":"1 + 2","root":"EXPRESSION"}}})",
      1);
  ASSERT_TRUE(expression.ok) << expression.output;
  FrontendResponse expression_response = ParseResponse(expression.output);
  EXPECT_FALSE(expression_response.parse().has_extended_response());
  EXPECT_NE(expression_response.parse().debug_string().find("BinaryExpression"),
            std::string::npos);
  EXPECT_EQ(expression.output.find("parsedExpression"), std::string::npos)
      << expression.output;

  ProcessResult type = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"extendedRequest":{"sql":"ARRAY<STRUCT<x INT64>>","root":"TYPE"}}})",
      2);
  ASSERT_TRUE(type.ok) << type.output;
  FrontendResponse type_response = ParseResponse(type.output);
  EXPECT_FALSE(type_response.parse().has_extended_response());
  EXPECT_NE(type_response.parse().debug_string().find("ArrayType"),
            std::string::npos);
  EXPECT_EQ(type.output.find("parsedType"), std::string::npos) << type.output;

  ProcessResult multiple = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"parse":{"extendedRequest":{"sql":"SELECT 1; SELECT 2;","root":"PARSE_MULTIPLE"}}})",
      3);
  ASSERT_TRUE(multiple.ok) << multiple.output;
  FrontendResponse multiple_response = ParseResponse(multiple.output);
  EXPECT_FALSE(multiple_response.parse().has_extended_response());
  EXPECT_EQ(multiple.output.find("parsedStatement"), std::string::npos)
      << multiple.output;
  // Both statements are still rendered, so the dump comparison is unaffected.
  const std::string& debug = multiple_response.parse().debug_string();
  const size_t first = debug.find("QueryStatement");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(debug.find("QueryStatement", first + 1), std::string::npos);
}

TEST(FrontendTest, RejectsOmitResponseProtoWhereItWouldEmptyTheReply) {
  Frontend frontend;
  ProcessResult builtin = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"bf","responseOptions":{"omitResponseProto":true},"builtinFunctions":{"request":{}}})",
      1);
  ASSERT_FALSE(builtin.ok);
  FrontendResponse builtin_response = ParseResponse(builtin.output);
  EXPECT_EQ(builtin_response.id(), "bf");
  EXPECT_EQ(builtin_response.error().origin(), "protocol");
  EXPECT_EQ(builtin_response.error().operation(), "builtinFunctions");
  EXPECT_EQ(builtin_response.error().message(),
            "responseOptions.omitResponseProto would leave the "
            "builtinFunctions reply empty; it applies to analyze and parse "
            "only");

  // The two option-reading operations answer with nothing but their response
  // proto either, so they read the same way.
  ProcessResult language = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"languageOptions":{}})",
      2);
  ASSERT_FALSE(language.ok);
  EXPECT_EQ(ParseResponse(language.output).error().message(),
            "responseOptions.omitResponseProto would leave the languageOptions "
            "reply empty; it applies to analyze and parse only");

  ProcessResult analyzer = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":true},"analyzerOptions":{"request":{}}})",
      3);
  ASSERT_FALSE(analyzer.ok);
  EXPECT_EQ(ParseResponse(analyzer.output).error().message(),
            "responseOptions.omitResponseProto would leave the analyzerOptions "
            "reply empty; it applies to analyze and parse only");

  // An explicit false, and an empty responseOptions, are accepted everywhere.
  ProcessResult kept = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{"omitResponseProto":false},"builtinFunctions":{"request":{}}})",
      4);
  ASSERT_TRUE(kept.ok) << kept.output;
  EXPECT_GT(
      ParseResponse(kept.output).builtin_functions().response().function_size(),
      0);

  ProcessResult empty_options = frontend.ProcessLine(
      R"({"protocolVersion":1,"responseOptions":{},"analyzerOptions":{}})", 5);
  ASSERT_TRUE(empty_options.ok) << empty_options.output;
}

TEST(FrontendTest, OmitResponseProtoLeavesErrorsUnchanged) {
  Frontend frontend;
  ProcessResult full = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"e1","analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT nosuchcolumn"}}})",
      9);
  ASSERT_FALSE(full.ok);
  ProcessResult omitted = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"e1","responseOptions":{"omitResponseProto":true},"analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT nosuchcolumn"}}})",
      9);
  ASSERT_FALSE(omitted.ok);
  EXPECT_EQ(omitted.output, full.output);
  EXPECT_TRUE(ParseResponse(omitted.output).error().has_location());
}

TEST(FrontendTest, ReportsInputLineLocalErrors) {
  Frontend frontend;
  ProcessResult bad_json = frontend.ProcessLine("{", 7);
  ASSERT_FALSE(bad_json.ok);
  FrontendResponse json_response = ParseResponse(bad_json.output);
  EXPECT_EQ(json_response.error().input_line(), 7);
  EXPECT_EQ(json_response.error().origin(), "proto_json");
  EXPECT_FALSE(json_response.error().has_location());

  ProcessResult syntax_error = frontend.ProcessLine(
      R"({"protocolVersion":1,"analyze":{"request":{"sqlStatement":"SELECT FROM"}}})",
      8);
  ASSERT_FALSE(syntax_error.ok);
  FrontendResponse sql_response = ParseResponse(syntax_error.output);
  EXPECT_EQ(sql_response.error().input_line(), 8);
  EXPECT_EQ(sql_response.error().origin(), "googlesql");
  EXPECT_TRUE(sql_response.error().has_location());
}

TEST(FrontendTest, ReportsTypedParseAndAnalyzeErrorLocations) {
  Frontend frontend;
  ProcessResult parse = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"parse-location","parse":{"request":{"sqlStatement":"SELECT FROM"}}})",
      4);
  ASSERT_FALSE(parse.ok);
  FrontendResponse parse_response = ParseResponse(parse.output);
  ASSERT_TRUE(parse_response.error().has_location()) << parse.output;
  EXPECT_EQ(parse_response.error().input_line(), 4);
  EXPECT_EQ(parse_response.error().location().line(), 1);
  EXPECT_EQ(parse_response.error().location().column(), 8);
  EXPECT_EQ(parse_response.error().location().byte_offset(), 7);
  EXPECT_EQ(parse_response.error().location().filename(), "");
  EXPECT_EQ(parse_response.error().message().find("[at"), std::string::npos);

  ProcessResult analyze = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"analyze-location","analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT nosuchcolumn"}}})",
      5);
  ASSERT_FALSE(analyze.ok);
  FrontendResponse analyze_response = ParseResponse(analyze.output);
  ASSERT_TRUE(analyze_response.error().has_location()) << analyze.output;
  EXPECT_EQ(analyze_response.error().location().line(), 1);
  EXPECT_EQ(analyze_response.error().location().column(), 8);
  EXPECT_EQ(analyze_response.error().location().byte_offset(), 7);
  EXPECT_EQ(analyze_response.error().location().filename(), "");
  EXPECT_EQ(analyze_response.error().message().find("[at"), std::string::npos);

  ProcessResult one_line = frontend.ProcessLine(
      R"({"protocolVersion":1,"analyze":{"namedCatalog":"CATALOG_NONE","request":{"sqlStatement":"SELECT nosuchcolumn","options":{"errorMessageMode":"ERROR_MESSAGE_ONE_LINE"}}}})",
      6);
  ASSERT_FALSE(one_line.ok);
  FrontendResponse one_line_response = ParseResponse(one_line.output);
  ASSERT_TRUE(one_line_response.error().has_location()) << one_line.output;
  EXPECT_EQ(one_line_response.error().location().line(), 1);
  EXPECT_EQ(one_line_response.error().location().column(), 8);
  EXPECT_EQ(one_line_response.error().location().byte_offset(), 7);
  EXPECT_NE(one_line_response.error().message().find("[at 1:8]"),
            std::string::npos);
}

TEST(FrontendTest, ReportsResumeUnicodeAndScriptErrorLocations) {
  Frontend frontend;
  ProcessResult resume = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"parseResumeLocation":{"input":"SELECT 1;\nSELECT FROM","bytePosition":10,"filename":"query.sql"}}}})",
      1);
  ASSERT_FALSE(resume.ok);
  FrontendResponse resume_response = ParseResponse(resume.output);
  ASSERT_TRUE(resume_response.error().has_location()) << resume.output;
  EXPECT_EQ(resume_response.error().location().line(), 2);
  EXPECT_EQ(resume_response.error().location().column(), 8);
  EXPECT_EQ(resume_response.error().location().byte_offset(), 17);
  EXPECT_EQ(resume_response.error().location().filename(), "query.sql");

  ProcessResult unicode = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT '😀', FROM"}}})",
      2);
  ASSERT_FALSE(unicode.ok);
  FrontendResponse unicode_response = ParseResponse(unicode.output);
  ASSERT_TRUE(unicode_response.error().has_location()) << unicode.output;
  EXPECT_EQ(unicode_response.error().location().line(), 1);
  EXPECT_EQ(unicode_response.error().location().column(), 17);
  EXPECT_EQ(unicode_response.error().location().byte_offset(), 19);

  ProcessResult script = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"BEGIN\nSELECT FROM;\nEND","allowScript":true}}})",
      3);
  ASSERT_FALSE(script.ok);
  FrontendResponse script_response = ParseResponse(script.output);
  ASSERT_TRUE(script_response.error().has_location()) << script.output;
  EXPECT_EQ(script_response.error().location().line(), 2);
  EXPECT_EQ(script_response.error().location().column(), 8);
  EXPECT_EQ(script_response.error().location().byte_offset(), 13);
  EXPECT_EQ(script_response.error().location().filename(), "");
  EXPECT_EQ(script_response.error().message().find("[at"), std::string::npos);

  ProcessResult tab = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT\tFROM"}}})",
      4);
  ASSERT_FALSE(tab.ok);
  FrontendResponse tab_response = ParseResponse(tab.output);
  ASSERT_TRUE(tab_response.error().has_location()) << tab.output;
  EXPECT_EQ(tab_response.error().location().line(), 1);
  EXPECT_EQ(tab_response.error().location().column(), 9);
  EXPECT_EQ(tab_response.error().location().byte_offset(), 7);
}

TEST(FrontendTest, RejectsUnknownFieldsAndVersions) {
  Frontend frontend;
  EXPECT_FALSE(
      frontend
          .ProcessLine(
              R"({"protocolVersion":1,"wat":{},"parse":{"request":{"sqlStatement":"SELECT 1"}}})",
              1)
          .ok);
  EXPECT_FALSE(
      frontend
          .ProcessLine(
              R"({"protocolVersion":2,"parse":{"request":{"sqlStatement":"SELECT 1"}}})",
              2)
          .ok);
}

TEST(FrontendTest, NamesMissingRequiredProtocolFields) {
  Frontend frontend;
  ProcessResult no_version = frontend.ProcessLine(
      R"({"parse":{"request":{"sqlStatement":"SELECT 1"}}})", 1);
  ASSERT_FALSE(no_version.ok);
  FrontendResponse no_version_response = ParseResponse(no_version.output);
  EXPECT_EQ(no_version_response.error().origin(), "protocol");
  EXPECT_EQ(no_version_response.error().message(),
            "protocolVersion is required");

  // Every other required field reads the same way, in the lower camel case
  // spelling the JSON protocol uses rather than the protobuf field name.
  ProcessResult no_root = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"extendedRequest":{"sql":"1"}}})", 2);
  ASSERT_FALSE(no_root.ok);
  EXPECT_EQ(ParseResponse(no_root.output).error().message(),
            "parse.extendedRequest.root is required");

  ProcessResult several =
      frontend.ProcessLine(R"({"parse":{"extendedRequest":{"sql":"1"}}})", 3);
  ASSERT_FALSE(several.ok);
  EXPECT_EQ(ParseResponse(several.output).error().message(),
            "protocolVersion and parse.extendedRequest.root are required");
}

TEST(FrontendTest, RejectsSeveralOperationsAtTheProtocolLayer) {
  Frontend frontend;
  ProcessResult two = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"two-ops","parse":{"request":{"sqlStatement":"SELECT 1"}},"analyze":{"request":{"sqlStatement":"SELECT 1"}}})",
      1);
  ASSERT_FALSE(two.ok);
  FrontendResponse two_response = ParseResponse(two.output);
  EXPECT_EQ(two_response.id(), "two-ops");
  EXPECT_EQ(two_response.error().origin(), "protocol");
  EXPECT_EQ(two_response.error().message(),
            "exactly one operation is allowed; found analyze and parse");
  EXPECT_FALSE(two_response.error().has_operation());

  ProcessResult three = frontend.ProcessLine(
      R"({"protocolVersion":1,"builtinFunctions":{"request":{}},"parse":{"request":{"sqlStatement":"SELECT 1"}},"analyze":{"request":{"sqlStatement":"SELECT 1"}}})",
      2);
  ASSERT_FALSE(three.ok);
  EXPECT_EQ(ParseResponse(three.output).error().message(),
            "exactly one operation is allowed; found analyze, parse and "
            "builtinFunctions");

  // Zero operations keeps reporting the neighbouring protocol message.
  ProcessResult none = frontend.ProcessLine(R"({"protocolVersion":1})", 3);
  ASSERT_FALSE(none.ok);
  FrontendResponse none_response = ParseResponse(none.output);
  EXPECT_EQ(none_response.error().origin(), "protocol");
  EXPECT_EQ(none_response.error().message(), "operation is required");
}

TEST(FrontendTest, TreatsWhitespaceOnlyInputLinesAsBlank) {
  EXPECT_TRUE(IsBlankInputLine(""));
  EXPECT_TRUE(IsBlankInputLine("   "));
  EXPECT_TRUE(IsBlankInputLine("\t \r"));
  EXPECT_FALSE(IsBlankInputLine("{}"));
  EXPECT_FALSE(IsBlankInputLine(
      R"( {"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1"}}} )"));
}

TEST(FrontendTest, EchoesValidIdOnProtoJsonErrors) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"bogus","parse":{"request":{"sqlStatement":"SELECT 1","options":{"enabledLanguageFeatures":["FEATURE_NO_SUCH_THING_AT_ALL"]}}}})",
      1);
  ASSERT_FALSE(result.ok);
  FrontendResponse response = ParseResponse(result.output);
  EXPECT_EQ(response.id(), "bogus");
  EXPECT_EQ(response.error().origin(), "proto_json");
}

TEST(FrontendTest, RejectsNestedAndEscapedDuplicateMembers) {
  Frontend frontend;

  ProcessResult nested = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1","sqlStatement":"SELECT 2"}}})",
      1);
  ASSERT_FALSE(nested.ok);
  EXPECT_EQ(ParseResponse(nested.output).error().origin(), "proto_json");

  ProcessResult escaped = frontend.ProcessLine(
      R"({"protocolVersion":1,"parse":{"request":{"sqlStatement":"SELECT 1","\u0073qlStatement":"SELECT 2"}}})",
      2);
  ASSERT_FALSE(escaped.ok);
  EXPECT_EQ(ParseResponse(escaped.output).error().origin(), "proto_json");

  ProcessResult nested_in_array = frontend.ProcessLine(
      R"({"protocolVersion":1,"analyze":{"request":{"sqlStatement":"SELECT 1","simpleCatalog":{"name":"catalog","table":[{"name":"first","\u006eame":"second"}]}}}})",
      3);
  ASSERT_FALSE(nested_in_array.ok);
  EXPECT_EQ(ParseResponse(nested_in_array.output).error().origin(),
            "proto_json");

  ProcessResult surrogate_escape = frontend.ProcessLine(
      R"({"protocolVersion":1,"\uD83D\uDE00":null,"😀":null})", 4);
  ASSERT_FALSE(surrogate_escape.ok);
  EXPECT_EQ(ParseResponse(surrogate_escape.output).error().origin(),
            "proto_json");
}

TEST(FrontendTest, RejectsDuplicateMembersAndInvalidIdsThenContinues) {
  Frontend frontend;
  ProcessResult duplicate = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"first","id":"second","parse":{"request":{"sqlStatement":"SELECT 1"}}})",
      4);
  EXPECT_FALSE(duplicate.ok);
  EXPECT_EQ(ParseResponse(duplicate.output).error().origin(), "proto_json");

  ProcessResult empty_id = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"","parse":{"request":{"sqlStatement":"SELECT 1"}}})",
      5);
  ASSERT_FALSE(empty_id.ok);
  FrontendResponse empty_response = ParseResponse(empty_id.output);
  EXPECT_FALSE(empty_response.has_id());
  EXPECT_EQ(empty_response.error().origin(), "protocol");

  FrontendRequest long_id_request;
  long_id_request.set_protocol_version(1);
  long_id_request.set_id(std::string(257, 'x'));
  long_id_request.mutable_parse()->mutable_request()->set_sql_statement(
      "SELECT 1");
  ProcessResult long_id = frontend.ProcessLine(RequestJson(long_id_request), 6);
  EXPECT_FALSE(long_id.ok);
  EXPECT_FALSE(ParseResponse(long_id.output).has_id());

  ProcessResult valid = frontend.ProcessLine(
      R"({"protocolVersion":1,"id":"after-error","parse":{"request":{"sqlStatement":"SELECT 1"}}})",
      7);
  ASSERT_TRUE(valid.ok) << valid.output;
  EXPECT_EQ(ParseResponse(valid.output).id(), "after-error");
}

TEST(FrontendTest, ErrorInputLineIsAlwaysSchemaValid) {
  Frontend frontend;
  ProcessResult result = frontend.ProcessLine("{", 0);
  ASSERT_FALSE(result.ok);
  EXPECT_EQ(ParseResponse(result.output).error().input_line(), 1);
}

TEST(FrontendTest, VersionIdentifiesProtocolAndGoogleSql) {
  const std::string version = VersionString();
  EXPECT_NE(version.find("googlesql-frontend 0.1.0"), std::string::npos);
  EXPECT_NE(version.find("protocol 1"), std::string::npos);
  EXPECT_NE(version.find(kGoogleSqlCommit), std::string::npos);
}

}  // namespace
}  // namespace googlesql_frontend
