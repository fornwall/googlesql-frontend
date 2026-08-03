#include "frontend/compatibility.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "googlesql/common/options_utils.h"
#include "googlesql/local_service/local_service.h"
#include "googlesql/local_service/local_service.pb.h"
#include "googlesql/parser/parser.h"
#include "googlesql/proto/options.pb.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/id_string.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/testdata/sample_catalog_impl.h"

ABSL_DECLARE_FLAG(bool, output_asc_explicitly);

namespace googlesql_frontend {
namespace {

using ::googlesql::AnalyzerOptionsProto;
using ::googlesql::BuiltinFunctionOptions;
using ::googlesql::IdStringPool;
using ::googlesql::LanguageOptions;
using ::googlesql::ParseResumeLocation;
using ::googlesql::ParserOptions;
using ::googlesql::ParserOutput;
using ::googlesql::ResolvedExpr;
using ::googlesql::ResolvedNode;
using ::googlesql::ResolvedStatement;
using ::googlesql::SampleCatalogImpl;
using ::googlesql::SimpleCatalog;
using ::googlesql::local_service::AnalyzeRequest;
using ::googlesql::local_service::AnalyzeResponse;
using ::googlesql::local_service::GoogleSqlLocalServiceImpl;

absl::Status ValidateOptions(const CompatibilityOptions& options) {
  if (options.mode != "parse" && options.mode != "analyze") {
    return absl::InvalidArgumentError(
        "--mode must be 'parse' or 'analyze' in compatibility mode");
  }
  if (options.sql_mode != "query" && options.sql_mode != "expression" &&
      options.sql_mode != "script" && options.sql_mode != "type" &&
      options.sql_mode != "parse_multiple") {
    return absl::InvalidArgumentError(
        "--sql_mode must be query, expression, script, type, or "
        "parse_multiple");
  }
  if (options.catalog != "none" && options.catalog != "sample") {
    return absl::InvalidArgumentError("--catalog must be 'none' or 'sample'");
  }
  return absl::OkStatus();
}

absl::StatusOr<LanguageOptions> MakeLanguageOptions(
    const CompatibilityOptions& options) {
  LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();
  language_options.SetSupportsAllStatementKinds();
  language_options.EnableAllReservableKeywords();

  if (!options.enabled_language_features.empty()) {
    absl::StatusOr<
        googlesql::internal::EnumOptionsEntry<googlesql::LanguageFeature>>
        features = googlesql::internal::ParseEnabledLanguageFeatures(
            options.enabled_language_features);
    if (!features.ok()) {
      return features.status();
    }
    language_options.SetEnabledLanguageFeatures(
        {features->options.begin(), features->options.end()});
  }
  return language_options;
}

absl::StatusOr<std::string> Parse(const CompatibilityOptions& options,
                                  const std::string& sql,
                                  const LanguageOptions& language_options) {
  ParserOptions parser_options(language_options);
  std::unique_ptr<ParserOutput> parser_output;

  if (options.sql_mode == "query") {
    absl::Status status =
        googlesql::ParseStatement(sql, parser_options, &parser_output);
    if (!status.ok()) return status;
    return parser_output->statement()->DebugString();
  }
  if (options.sql_mode == "expression") {
    absl::Status status =
        googlesql::ParseExpression(sql, parser_options, &parser_output);
    if (!status.ok()) return status;
    return parser_output->expression()->DebugString();
  }
  if (options.sql_mode == "script") {
    absl::Status status = googlesql::ParseScript(
        sql, parser_options, parser_options.error_message_options(),
        &parser_output);
    if (!status.ok()) return status;
    return parser_output->script()->DebugString();
  }
  if (options.sql_mode == "type") {
    absl::Status status =
        googlesql::ParseType(sql, parser_options, &parser_output);
    if (!status.ok()) return status;
    return parser_output->type()->DebugString();
  }

  ParseResumeLocation location = ParseResumeLocation::FromString(sql);
  bool at_end_of_input = false;
  std::string output;
  while (!at_end_of_input) {
    absl::Status status = googlesql::ParseNextScriptStatement(
        &location, parser_options, &parser_output, &at_end_of_input);
    if (!status.ok()) return status;
    if (!output.empty()) output.push_back('\n');
    absl::StrAppend(&output, parser_output->statement()->DebugString());
  }
  return output;
}

void SetRequestOptions(const LanguageOptions& language_options,
                       AnalyzerOptionsProto* options) {
  language_options.Serialize(options->mutable_language_options());
  // These match the compatibility tool's analysis path: unused columns are
  // pruned, while constant evaluation is available for analyzer-time folding
  // only.
  options->set_prune_unused_columns(true);
  options->set_use_constant_evaluator(true);
}

void AddBuiltinPool(AnalyzeRequest* request) {
  request->mutable_descriptor_pool_list()->add_definitions()->mutable_builtin();
}

absl::StatusOr<std::string> RestoreDebugString(const AnalyzeResponse& response,
                                               SimpleCatalog* catalog) {
  std::vector<const google::protobuf::DescriptorPool*> pools = {
      google::protobuf::DescriptorPool::generated_pool()};
  IdStringPool string_pool;
  ResolvedNode::RestoreParams params(pools, catalog, catalog->type_factory(),
                                     &string_pool);
  ResolvedNode::DebugStringConfig debug_options{
      .use_box_glyphs = true,
  };

  if (response.has_resolved_statement()) {
    absl::StatusOr<std::unique_ptr<ResolvedStatement>> statement =
        ResolvedStatement::RestoreFrom(response.resolved_statement(), params);
    if (!statement.ok()) return statement.status();
    return (*statement)->DebugString(debug_options);
  }
  if (response.has_resolved_expression()) {
    absl::StatusOr<std::unique_ptr<ResolvedExpr>> expression =
        ResolvedExpr::RestoreFrom(response.resolved_expression(), params);
    if (!expression.ok()) return expression.status();
    return (*expression)->DebugString(debug_options);
  }
  return absl::InternalError("analysis returned no resolved AST");
}

absl::Status SetAnalyzeTarget(const CompatibilityOptions& options,
                              const std::string& sql, AnalyzeRequest* request) {
  if (options.sql_mode == "query") {
    request->set_sql_statement(sql);
    return absl::OkStatus();
  }
  if (options.sql_mode == "expression") {
    request->set_sql_expression(sql);
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("analysis does not support sql_mode=", options.sql_mode));
}

absl::StatusOr<std::string> Analyze(const CompatibilityOptions& options,
                                    const std::string& sql,
                                    const LanguageOptions& language_options) {
  AnalyzeRequest request;
  SetRequestOptions(language_options, request.mutable_options());
  AddBuiltinPool(&request);
  absl::Status target_status = SetAnalyzeTarget(options, sql, &request);
  if (!target_status.ok()) return target_status;

  GoogleSqlLocalServiceImpl service;
  AnalyzeResponse response;

  if (options.catalog == "sample") {
    SampleCatalogImpl sample_catalog;
    absl::Status status = sample_catalog.LoadCatalogImpl(
        BuiltinFunctionOptions(language_options));
    if (!status.ok()) return status;
    const std::vector<const google::protobuf::DescriptorPool*> pools = {
        google::protobuf::DescriptorPool::generated_pool()};
    status = service.AnalyzeImpl(request, pools, sample_catalog.catalog(),
                                 &response);
    if (!status.ok()) return status;
    return RestoreDebugString(response, sample_catalog.catalog());
  }

  googlesql::SimpleCatalogProto* catalog = request.mutable_simple_catalog();
  catalog->set_name("simple_catalog");
  language_options.Serialize(
      catalog->mutable_builtin_function_options()->mutable_language_options());

  absl::Status status = service.Analyze(request, &response);
  if (!status.ok()) return status;

  const std::vector<const google::protobuf::DescriptorPool*> pools = {
      google::protobuf::DescriptorPool::generated_pool()};
  absl::StatusOr<std::unique_ptr<SimpleCatalog>> restored_catalog =
      SimpleCatalog::Deserialize(request.simple_catalog(), pools);
  if (!restored_catalog.ok()) return restored_catalog.status();
  return RestoreDebugString(response, restored_catalog->get());
}

}  // namespace

absl::StatusOr<std::string> ProcessCompatibility(
    const CompatibilityOptions& options, const std::string& sql) {
  absl::Status options_status = ValidateOptions(options);
  if (!options_status.ok()) return options_status;

  absl::StatusOr<LanguageOptions> language_options =
      MakeLanguageOptions(options);
  if (!language_options.ok()) return language_options.status();

  const bool old_output_asc_explicitly =
      absl::GetFlag(FLAGS_output_asc_explicitly);
  absl::Cleanup restore_flag = [old_output_asc_explicitly] {
    absl::SetFlag(&FLAGS_output_asc_explicitly, old_output_asc_explicitly);
  };
  absl::SetFlag(&FLAGS_output_asc_explicitly, options.output_asc_explicitly);
  if (options.mode == "parse") {
    return Parse(options, sql, *language_options);
  }
  return Analyze(options, sql, *language_options);
}

}  // namespace googlesql_frontend
