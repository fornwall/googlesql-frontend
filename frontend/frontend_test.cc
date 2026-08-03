#include "frontend/frontend.h"

#include <string>

#include "frontend/protocol.pb.h"
#include "google/protobuf/util/json_util.h"
#include "gtest/gtest.h"

namespace googlesql_frontend {
namespace {

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
