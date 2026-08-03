#include "frontend/frontend.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "frontend/protocol.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/struct.pb.h"
#include "google/protobuf/util/json_util.h"
#include "googlesql/common/proto_helper.h"
#include "googlesql/parser/parse_tree_serializer.h"
#include "googlesql/parser/parser.h"
#include "googlesql/proto/options.pb.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/error_location.pb.h"
#include "googlesql/public/evaluator.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/prepared_expression_constant_evaluator.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/simple_catalog_util.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/testdata/sample_catalog_impl.h"

ABSL_DECLARE_FLAG(bool, output_asc_explicitly);

namespace googlesql_frontend {
namespace {

using ::google::protobuf::Message;
using ::google::protobuf::util::JsonParseOptions;
using ::google::protobuf::util::JsonPrintOptions;

enum class JsonScanResult { kNoDuplicate, kDuplicate, kMalformed };

// ProtoJSON accepts duplicate object members and keeps the last value. That is
// undesirable for a line-oriented protocol: intermediaries can disagree about
// which operation or identifier a request contains. Scan the JSON syntax first
// and compare decoded member names within each object. Syntax errors are left
// to protobuf's parser so its canonical ProtoJSON diagnostics are preserved.
class JsonDuplicateMemberScanner {
 public:
  explicit JsonDuplicateMemberScanner(absl::string_view input)
      : input_(input) {}

  JsonScanResult Scan() {
    SkipWhitespace();
    JsonScanResult result = ParseValue(/*depth=*/0);
    if (result != JsonScanResult::kNoDuplicate) {
      return result;
    }
    SkipWhitespace();
    return position_ == input_.size() ? JsonScanResult::kNoDuplicate
                                      : JsonScanResult::kMalformed;
  }

 private:
  static constexpr int kMaxDepth = 256;

  void SkipWhitespace() {
    while (position_ < input_.size()) {
      const char c = input_[position_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return;
      }
      ++position_;
    }
  }

  JsonScanResult ParseValue(int depth) {
    if (depth > kMaxDepth) {
      return JsonScanResult::kMalformed;
    }
    SkipWhitespace();
    if (position_ == input_.size()) {
      return JsonScanResult::kMalformed;
    }
    switch (input_[position_]) {
      case '{':
        return ParseObject(depth + 1);
      case '[':
        return ParseArray(depth + 1);
      case '"':
        return ParseString(nullptr);
      default:
        return ParsePrimitive();
    }
  }

  JsonScanResult ParseObject(int depth) {
    ++position_;  // '{'
    SkipWhitespace();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return JsonScanResult::kNoDuplicate;
    }

    std::unordered_set<std::string> members;
    while (position_ < input_.size()) {
      SkipWhitespace();
      if (position_ == input_.size() || input_[position_] != '"') {
        return JsonScanResult::kMalformed;
      }
      std::string member;
      JsonScanResult result = ParseString(&member);
      if (result != JsonScanResult::kNoDuplicate) {
        return result;
      }
      if (!members.insert(std::move(member)).second) {
        return JsonScanResult::kDuplicate;
      }

      SkipWhitespace();
      if (position_ == input_.size() || input_[position_] != ':') {
        return JsonScanResult::kMalformed;
      }
      ++position_;
      result = ParseValue(depth);
      if (result != JsonScanResult::kNoDuplicate) {
        return result;
      }

      SkipWhitespace();
      if (position_ == input_.size()) {
        return JsonScanResult::kMalformed;
      }
      const char delimiter = input_[position_++];
      if (delimiter == '}') {
        return JsonScanResult::kNoDuplicate;
      }
      if (delimiter != ',') {
        return JsonScanResult::kMalformed;
      }
    }
    return JsonScanResult::kMalformed;
  }

  JsonScanResult ParseArray(int depth) {
    ++position_;  // '['
    SkipWhitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return JsonScanResult::kNoDuplicate;
    }

    while (position_ < input_.size()) {
      JsonScanResult result = ParseValue(depth);
      if (result != JsonScanResult::kNoDuplicate) {
        return result;
      }
      SkipWhitespace();
      if (position_ == input_.size()) {
        return JsonScanResult::kMalformed;
      }
      const char delimiter = input_[position_++];
      if (delimiter == ']') {
        return JsonScanResult::kNoDuplicate;
      }
      if (delimiter != ',') {
        return JsonScanResult::kMalformed;
      }
    }
    return JsonScanResult::kMalformed;
  }

  JsonScanResult ParsePrimitive() {
    const size_t start = position_;
    while (position_ < input_.size()) {
      const char c = input_[position_];
      if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' ||
          c == '\n' || c == '\r') {
        break;
      }
      ++position_;
    }
    return position_ == start ? JsonScanResult::kMalformed
                              : JsonScanResult::kNoDuplicate;
  }

  bool ReadHexCodeUnit(uint32_t* code_unit) {
    if (input_.size() - position_ < 4) {
      return false;
    }
    *code_unit = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = input_[position_++];
      *code_unit <<= 4;
      if (c >= '0' && c <= '9') {
        *code_unit += c - '0';
      } else if (c >= 'a' && c <= 'f') {
        *code_unit += c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        *code_unit += c - 'A' + 10;
      } else {
        return false;
      }
    }
    return true;
  }

  static void AppendUtf8(uint32_t code_point, std::string* output) {
    if (output == nullptr) {
      return;
    }
    if (code_point <= 0x7f) {
      output->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
      output->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
      output->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
      output->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
  }

  JsonScanResult ParseString(std::string* decoded) {
    ++position_;  // '"'
    if (decoded != nullptr) {
      decoded->clear();
    }
    while (position_ < input_.size()) {
      const unsigned char c = input_[position_++];
      if (c == '"') {
        return JsonScanResult::kNoDuplicate;
      }
      if (c < 0x20) {
        return JsonScanResult::kMalformed;
      }
      if (c != '\\') {
        if (decoded != nullptr) {
          decoded->push_back(static_cast<char>(c));
        }
        continue;
      }

      if (position_ == input_.size()) {
        return JsonScanResult::kMalformed;
      }
      const char escape = input_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          if (decoded != nullptr) {
            decoded->push_back(escape);
          }
          break;
        case 'b':
          if (decoded != nullptr) decoded->push_back('\b');
          break;
        case 'f':
          if (decoded != nullptr) decoded->push_back('\f');
          break;
        case 'n':
          if (decoded != nullptr) decoded->push_back('\n');
          break;
        case 'r':
          if (decoded != nullptr) decoded->push_back('\r');
          break;
        case 't':
          if (decoded != nullptr) decoded->push_back('\t');
          break;
        case 'u': {
          uint32_t code_point;
          if (!ReadHexCodeUnit(&code_point)) {
            return JsonScanResult::kMalformed;
          }
          if (code_point >= 0xd800 && code_point <= 0xdbff) {
            if (input_.size() - position_ < 2 || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
              return JsonScanResult::kMalformed;
            }
            position_ += 2;
            uint32_t low_surrogate;
            if (!ReadHexCodeUnit(&low_surrogate) || low_surrogate < 0xdc00 ||
                low_surrogate > 0xdfff) {
              return JsonScanResult::kMalformed;
            }
            code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                         (low_surrogate - 0xdc00);
          } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
            return JsonScanResult::kMalformed;
          }
          AppendUtf8(code_point, decoded);
          break;
        }
        default:
          return JsonScanResult::kMalformed;
      }
    }
    return JsonScanResult::kMalformed;
  }

  absl::string_view input_;
  size_t position_ = 0;
};

absl::Status ParseJson(absl::string_view input, Message* message) {
  if (JsonDuplicateMemberScanner(input).Scan() == JsonScanResult::kDuplicate) {
    return absl::InvalidArgumentError(
        "duplicate JSON object members are not allowed");
  }
  JsonParseOptions options;
  options.ignore_unknown_fields = false;
  options.case_insensitive_enum_parsing = false;
  return google::protobuf::util::JsonStringToMessage(input, message, options);
}

absl::StatusOr<std::string> PrintJson(const Message& message) {
  JsonPrintOptions options;
  options.add_whitespace = false;
  options.preserve_proto_field_names = false;
  options.always_print_fields_with_no_presence = false;
  std::string output;
  absl::Status status =
      google::protobuf::util::MessageToJsonString(message, &output, options);
  if (!status.ok()) {
    return status;
  }
  return output;
}

struct DescriptorPools {
  std::vector<std::unique_ptr<google::protobuf::DescriptorPool>> owned;
  std::vector<const google::protobuf::DescriptorPool*> ordered;
};

absl::StatusOr<DescriptorPools> BuildDescriptorPools(
    const googlesql::local_service::DescriptorPoolListProto& definitions) {
  DescriptorPools pools;
  for (const auto& definition : definitions.definitions()) {
    using Definition =
        googlesql::local_service::DescriptorPoolListProto::Definition;
    switch (definition.definition_case()) {
      case Definition::kFileDescriptorSet: {
        auto pool = std::make_unique<google::protobuf::DescriptorPool>();
        absl::Status status = googlesql::AddFileDescriptorSetToPool(
            &definition.file_descriptor_set(), pool.get());
        if (!status.ok()) {
          return status;
        }
        pools.ordered.push_back(pool.get());
        pools.owned.push_back(std::move(pool));
        break;
      }
      case Definition::kBuiltin:
        pools.ordered.push_back(
            google::protobuf::DescriptorPool::generated_pool());
        break;
      case Definition::kRegisteredId:
        return absl::InvalidArgumentError(
            "debugString is unavailable for registered descriptor pools");
      case Definition::DEFINITION_NOT_SET:
        return absl::InvalidArgumentError(
            "descriptor pool definition is missing");
    }
  }
  return pools;
}

// AnalyzerOptions::Deserialize begins from a default-constructed
// AnalyzerOptions but then assigns these scalar fields unconditionally, so an
// options object that omits one silently receives the protobuf zero value
// instead of the value GoogleSQL documents as the default. Restore the
// default-constructed value for every such field the request did not set, and
// read those defaults from a default-constructed AnalyzerOptions so they track
// GoogleSQL rather than a private copy of its literals.
//
// Repeated fields are left exactly as upstream deserializes them, because
// proto absence and an explicit empty list are indistinguishable there. The
// one repeated field whose C++ default is not empty, enabled_rewrites, is
// therefore restored one level up by AnalyzeOperation.rewrites, where the
// request still says which of the two it meant. See ApplyRewriteSet.
absl::Status DeserializeAnalyzerOptions(
    const googlesql::AnalyzerOptionsProto& proto,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    googlesql::TypeFactory* type_factory, googlesql::AnalyzerOptions* options) {
  absl::Status status = googlesql::AnalyzerOptions::Deserialize(
      proto, pools, type_factory, options);
  if (!status.ok()) {
    return status;
  }
  const googlesql::AnalyzerOptions defaults;
  if (!proto.has_statement_context()) {
    options->set_statement_context(defaults.statement_context());
  }
  if (!proto.has_error_message_mode()) {
    options->set_error_message_mode(defaults.error_message_mode());
  }
  if (!proto.has_create_new_column_for_each_projected_output()) {
    options->set_create_new_column_for_each_projected_output(
        defaults.create_new_column_for_each_projected_output());
  }
  if (!proto.has_prune_unused_columns()) {
    options->set_prune_unused_columns(defaults.prune_unused_columns());
  }
  if (!proto.has_allow_undeclared_parameters()) {
    options->set_allow_undeclared_parameters(
        defaults.allow_undeclared_parameters());
  }
  if (!proto.has_parameter_mode()) {
    options->set_parameter_mode(defaults.parameter_mode());
  }
  if (!proto.has_preserve_column_aliases()) {
    options->set_preserve_column_aliases(defaults.preserve_column_aliases());
  }
  if (!proto.has_preserve_unnecessary_cast()) {
    options->set_preserve_unnecessary_cast(
        defaults.preserve_unnecessary_cast());
  }
  if (!proto.has_replace_table_not_found_error_with_tvf_error_if_applicable()) {
    options->set_replace_table_not_found_error_with_tvf_error_if_applicable(
        defaults.replace_table_not_found_error_with_tvf_error_if_applicable());
  }
  return absl::OkStatus();
}

// Expands a preset into the complete LanguageOptionsProto it names.
// LanguageOptions::Serialize writes every field, so the result stands on its
// own: it is what the preset means, with nothing left implicit.
//
// This is the only expansion in the frontend. Applying a preset to analyze or
// parse and reporting one through the languageOptions operation both go
// through here, so what a client reads back cannot drift from what the
// analyzer and the parser were configured with.
googlesql::LanguageOptionsProto ExpandLanguageOptionsPreset(
    const LanguageOptionsPreset& preset) {
  googlesql::LanguageOptions expanded;
  // SetLanguageVersion replaces the enabled feature set rather than adding to
  // it, so it runs first: when a feature set is named as well, that set is the
  // one that survives.
  if (preset.has_language_version()) {
    expanded.SetLanguageVersion(preset.language_version());
  }
  switch (preset.features()) {
    case LANGUAGE_FEATURES_DEFAULT:
      break;
    case LANGUAGE_FEATURES_MAXIMUM:
      expanded.EnableMaximumLanguageFeatures();
      break;
    case LANGUAGE_FEATURES_DEVELOPMENT:
      expanded.EnableMaximumLanguageFeaturesForDevelopment();
      break;
  }
  if (preset.all_reservable_keywords_reserved()) {
    expanded.EnableAllReservableKeywords();
  }
  if (preset.all_statement_kinds_supported()) {
    expanded.SetSupportsAllStatementKinds();
  }

  googlesql::LanguageOptionsProto serialized;
  expanded.Serialize(&serialized);
  return serialized;
}

// Merges the request's own language options on top of the preset's expansion:
// scalars the request sets replace the preset's, and repeated entries the
// request lists are added to the preset's. The result is "preset plus explicit
// additions" rather than either one alone.
void ApplyLanguageOptionsPreset(const LanguageOptionsPreset& preset,
                                googlesql::LanguageOptionsProto* options) {
  googlesql::LanguageOptionsProto merged = ExpandLanguageOptionsPreset(preset);
  merged.MergeFrom(*options);
  options->Swap(&merged);
}

// Expands a rewrite baseline into the request's own enabled_rewrites list, in
// the same "preset plus explicit additions" shape as the language-options
// preset above.
//
// AnalyzerOptions::Deserialize clears enabled_rewrites and refills it from the
// proto, so it always starts from no rewrite at all. That is not the default a
// default-constructed AnalyzerOptions carries, but the field is repeated and
// therefore has no presence bit, so the deserializer genuinely cannot tell an
// omitted list from a request for none. REWRITES_DEFAULT names the baseline
// instead, and expands it here where the request's intent is still known.
void ApplyRewriteSet(RewriteSet rewrites,
                     googlesql::local_service::AnalyzeRequest* request) {
  if (rewrites == REWRITES_AS_REQUESTED) {
    // Nothing to add, and no analyzer options to materialize in order to add
    // it: an absent field leaves the request byte-for-byte as it arrived.
    return;
  }
  googlesql::AnalyzerOptionsProto* options = request->mutable_options();
  absl::flat_hash_set<int> enabled(options->enabled_rewrites().begin(),
                                   options->enabled_rewrites().end());
  for (const googlesql::ResolvedASTRewrite rewrite :
       googlesql::AnalyzerOptions::DefaultRewrites()) {
    if (enabled.insert(rewrite).second) {
      options->add_enabled_rewrites(rewrite);
    }
  }
}

// Builds the AnalyzeRequest the frontend actually runs. An explicit preset
// always applies. Otherwise a named catalog with no language options at all
// keeps its documented baseline of maximum released features, all statement
// kinds, and all reservable keywords. The rewrite baseline is independent of
// both, and reaches the named-catalog and inline-catalog paths alike because
// they share this request.
googlesql::local_service::AnalyzeRequest EffectiveAnalyzeRequest(
    const AnalyzeOperation& operation) {
  googlesql::local_service::AnalyzeRequest request = operation.request();
  ApplyRewriteSet(operation.rewrites(), &request);
  if (operation.has_language_options_preset()) {
    ApplyLanguageOptionsPreset(
        operation.language_options_preset(),
        request.mutable_options()->mutable_language_options());
  } else if (operation.has_named_catalog() &&
             !request.options().has_language_options()) {
    googlesql::LanguageOptions language_options;
    language_options.EnableMaximumLanguageFeatures();
    language_options.SetSupportsAllStatementKinds();
    language_options.EnableAllReservableKeywords();
    language_options.Serialize(
        request.mutable_options()->mutable_language_options());
  }
  return request;
}

// Builds the ParseOperation the frontend actually runs. The preset applies to
// whichever input the operation carries.
ParseOperation EffectiveParseOperation(const ParseOperation& operation) {
  ParseOperation effective = operation;
  if (!effective.has_language_options_preset()) {
    return effective;
  }
  if (effective.has_request()) {
    ApplyLanguageOptionsPreset(effective.language_options_preset(),
                               effective.mutable_request()->mutable_options());
  } else if (effective.has_extended_request()) {
    ApplyLanguageOptionsPreset(
        effective.language_options_preset(),
        effective.mutable_extended_request()->mutable_options());
  }
  return effective;
}

absl::Status AnalyzeWithCatalog(
    googlesql::local_service::GoogleSqlLocalServiceImpl* service,
    const googlesql::local_service::AnalyzeRequest& request,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    googlesql::SimpleCatalog* catalog, AnalyzeResult* result) {
  googlesql::TypeFactory type_factory;
  googlesql::AnalyzerOptions options;
  absl::Status status = DeserializeAnalyzerOptions(request.options(), pools,
                                                   &type_factory, &options);
  if (!status.ok()) {
    return status;
  }
  // Locations are a typed protocol field. Keep the default prose message free
  // of rendered coordinates, while preserving an explicit display-oriented
  // error mode and attaching the typed payload independently. This frontend
  // policy deliberately overrides the restored AnalyzerOptions default for an
  // absent error message mode.
  if (!request.options().has_error_message_mode()) {
    options.set_error_message_mode(googlesql::ERROR_MESSAGE_WITH_PAYLOAD);
  }
  options.set_attach_error_location_payload(true);

  // Keep these outputs alive until the main analysis and serialization have
  // finished: property graph definitions hold pointers into their resolved
  // expressions.
  std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>>
      property_graph_artifacts;
  absl::flat_hash_set<const googlesql::PropertyGraph*> graphs;
  status = catalog->GetPropertyGraphs(&graphs);
  if (!status.ok()) {
    return status;
  }
  for (const googlesql::PropertyGraph* graph : graphs) {
    status = googlesql::ResolveGraphPropertyDefinitions(
        options.language(), graph, catalog, property_graph_artifacts);
    if (!status.ok()) {
      return status;
    }
  }

  std::unique_ptr<googlesql::PreparedExpressionConstantEvaluator>
      constant_evaluator;
  if (request.options().use_constant_evaluator()) {
    constant_evaluator =
        std::make_unique<googlesql::PreparedExpressionConstantEvaluator>(
            googlesql::EvaluatorOptions{}, options.language());
    options.set_constant_evaluator(constant_evaluator.get());
  }

  std::unique_ptr<const googlesql::AnalyzerOutput> output;
  googlesql::local_service::AnalyzeResponse response;
  std::string sql;
  switch (request.target_case()) {
    case googlesql::local_service::AnalyzeRequest::kSqlStatement:
      sql = request.sql_statement();
      status = googlesql::AnalyzeStatement(sql, options, catalog, &type_factory,
                                           &output);
      break;
    case googlesql::local_service::AnalyzeRequest::kSqlExpression:
      sql = request.sql_expression();
      status = googlesql::AnalyzeExpression(sql, options, catalog,
                                            &type_factory, &output);
      break;
    case googlesql::local_service::AnalyzeRequest::kParseResumeLocation: {
      googlesql::ParseResumeLocation location =
          googlesql::ParseResumeLocation::FromProto(
              request.parse_resume_location());
      bool at_end_of_input = false;
      status = googlesql::AnalyzeNextStatement(&location, options, catalog,
                                               &type_factory, &output,
                                               &at_end_of_input);
      if (status.ok()) {
        sql.assign(location.input().data(), location.input().size());
        response.set_resume_byte_position(location.byte_position());
      }
      break;
    }
    case googlesql::local_service::AnalyzeRequest::TARGET_NOT_SET:
      return absl::InvalidArgumentError("AnalyzeRequest target is missing");
  }
  if (!status.ok()) {
    return status;
  }
  if (output == nullptr || output->resolved_node() == nullptr) {
    return absl::InternalError("analysis result is missing");
  }
  status =
      service->SerializeResolvedOutput(output.get(), pools, sql, &response);
  if (!status.ok()) {
    return status;
  }
  result->set_debug_string(output->resolved_node()->DebugString());
  result->mutable_response()->Swap(&response);
  return absl::OkStatus();
}

absl::Status AnalyzeInlineCatalog(
    googlesql::local_service::GoogleSqlLocalServiceImpl* service,
    const googlesql::local_service::AnalyzeRequest& request,
    AnalyzeResult* result) {
  if (request.has_registered_catalog_id()) {
    return absl::InvalidArgumentError(
        "debugString is unavailable for registered catalogs");
  }
  absl::StatusOr<DescriptorPools> pools =
      BuildDescriptorPools(request.descriptor_pool_list());
  if (!pools.ok()) {
    return pools.status();
  }
  absl::StatusOr<std::unique_ptr<googlesql::SimpleCatalog>> catalog =
      googlesql::SimpleCatalog::Deserialize(request.simple_catalog(),
                                            pools->ordered);
  if (!catalog.ok()) {
    return catalog.status();
  }
  return AnalyzeWithCatalog(service, request, pools->ordered, catalog->get(),
                            result);
}

googlesql::ParserOptions ParserOptionsFrom(
    const googlesql::LanguageOptionsProto* options) {
  return googlesql::ParserOptions(
      /*id_string_pool=*/nullptr, /*arena=*/nullptr,
      options == nullptr ? googlesql::LanguageOptions()
                         : googlesql::LanguageOptions(*options));
}

absl::StatusOr<std::string> NativeParseDebugString(
    const googlesql::local_service::ParseRequest& request) {
  googlesql::ParserOptions parser_options =
      ParserOptionsFrom(request.has_options() ? &request.options() : nullptr);
  std::unique_ptr<googlesql::ParserOutput> parser_output;

  if (request.has_sql_statement()) {
    absl::Status status;
    if (request.allow_script()) {
      status = googlesql::ParseScript(request.sql_statement(), parser_options,
                                      parser_options.error_message_options(),
                                      &parser_output);
    } else {
      status = googlesql::ParseStatement(request.sql_statement(),
                                         parser_options, &parser_output);
    }
    if (!status.ok()) {
      return status;
    }
    return parser_output->node()->DebugString();
  }

  if (request.has_parse_resume_location()) {
    googlesql::ParseResumeLocation location =
        googlesql::ParseResumeLocation::FromProto(
            request.parse_resume_location());
    bool at_end_of_input = false;
    absl::Status status =
        request.allow_script()
            ? googlesql::ParseNextScriptStatement(
                  &location, parser_options, &parser_output, &at_end_of_input)
            : googlesql::ParseNextStatement(&location, parser_options,
                                            &parser_output, &at_end_of_input);
    if (!status.ok()) {
      return status;
    }
    return parser_output->statement()->DebugString();
  }

  return absl::InvalidArgumentError("ParseRequest target is missing");
}

absl::Status RecoverScriptParseError(
    const googlesql::local_service::ParseRequest& request,
    const absl::Status& status) {
  googlesql::ErrorLocation unused_location;
  if (status.ok() || !request.allow_script() || !request.has_sql_statement() ||
      googlesql::GetErrorLocation(status, &unused_location)) {
    return status;
  }

  googlesql::ParserOptions parser_options =
      ParserOptionsFrom(request.has_options() ? &request.options() : nullptr);
  std::unique_ptr<googlesql::ParserOutput> parser_output;
  absl::Status recovered = googlesql::ParseScript(
      request.sql_statement(), parser_options,
      parser_options.error_message_options(), &parser_output);
  return recovered.ok() ? status : recovered;
}

absl::Status ParseExtended(const ExtendedParseRequest& request,
                           ExtendedParseResponse* response,
                           std::string* debug_string) {
  googlesql::ParserOptions parser_options =
      ParserOptionsFrom(request.has_options() ? &request.options() : nullptr);
  std::unique_ptr<googlesql::ParserOutput> parser_output;

  switch (request.root()) {
    case EXPRESSION: {
      absl::Status status = googlesql::ParseExpression(
          request.sql(), parser_options, &parser_output);
      if (!status.ok()) {
        return status;
      }
      *debug_string = parser_output->expression()->DebugString();
      return googlesql::ParseTreeSerializer::Serialize(
          parser_output->expression(), response->mutable_parsed_expression());
    }
    case TYPE: {
      absl::Status status =
          googlesql::ParseType(request.sql(), parser_options, &parser_output);
      if (!status.ok()) {
        return status;
      }
      *debug_string = parser_output->type()->DebugString();
      return googlesql::ParseTreeSerializer::Serialize(
          parser_output->type(), response->mutable_parsed_type());
    }
    case PARSE_MULTIPLE: {
      googlesql::ParseResumeLocation location =
          googlesql::ParseResumeLocation::FromString(request.sql());
      bool at_end_of_input = false;
      while (!at_end_of_input) {
        absl::Status status = googlesql::ParseNextScriptStatement(
            &location, parser_options, &parser_output, &at_end_of_input);
        if (!status.ok()) {
          return status;
        }
        if (!debug_string->empty()) {
          debug_string->push_back('\n');
        }
        debug_string->append(parser_output->statement()->DebugString());
        status = googlesql::ParseTreeSerializer::Serialize(
            parser_output->statement(), response->add_parsed_statement());
        if (!status.ok()) {
          return status;
        }
      }
      return absl::OkStatus();
    }
  }
  return absl::InvalidArgumentError("extended parse root is missing");
}

absl::Status AnalyzeNamedCatalog(
    googlesql::local_service::GoogleSqlLocalServiceImpl* service,
    NamedCatalog named_catalog,
    const googlesql::local_service::AnalyzeRequest& request,
    AnalyzeResult* result) {
  absl::StatusOr<DescriptorPools> pools =
      BuildDescriptorPools(request.descriptor_pool_list());
  if (!pools.ok()) {
    return pools.status();
  }

  // AnalyzeImpl validates these options, but named catalogs must be built
  // first. Validate here as well because SampleCatalogImpl has CHECK-based
  // loaders that assume a valid analyzer configuration.
  googlesql::TypeFactory options_type_factory;
  googlesql::AnalyzerOptions analyzer_options;
  absl::Status options_status =
      DeserializeAnalyzerOptions(request.options(), pools->ordered,
                                 &options_type_factory, &analyzer_options);
  if (!options_status.ok()) {
    return options_status;
  }
  const googlesql::LanguageOptions& analyzer_language_options =
      analyzer_options.language();
  if (analyzer_language_options.LanguageFeatureEnabled(
          googlesql::FEATURE_COLLATION_SUPPORT) &&
      !analyzer_language_options.LanguageFeatureEnabled(
          googlesql::FEATURE_ANNOTATION_FRAMEWORK)) {
    return absl::InvalidArgumentError(
        "FEATURE_COLLATION_SUPPORT requires "
        "FEATURE_ANNOTATION_FRAMEWORK to also be enabled");
  }
  options_status = googlesql::ValidateAnalyzerOptions(analyzer_options);
  if (!options_status.ok()) {
    return options_status;
  }

  const googlesql::LanguageOptions language_options(
      request.options().language_options());
  auto analyze = [&](googlesql::SimpleCatalog* catalog) -> absl::Status {
    return AnalyzeWithCatalog(service, request, pools->ordered, catalog,
                              result);
  };

  switch (named_catalog) {
    case CATALOG_NONE: {
      googlesql::SimpleCatalog catalog("simple_catalog");
      absl::Status status = catalog.AddBuiltinFunctionsAndTypes(
          googlesql::BuiltinFunctionOptions(language_options));
      if (!status.ok()) {
        return status;
      }
      return analyze(&catalog);
    }
    case CATALOG_SAMPLE: {
      // SampleCatalogImpl creates its fixture views while it is initialized.
      // Spanner DDL mode deliberately rejects CREATE VIEW during resolution,
      // and SampleCatalogImpl turns that initialization error into a CHECK.
      // The requested language options still reach AnalyzeImpl below; only
      // disable this parser feature for construction of the sample fixtures.
      googlesql::LanguageOptions catalog_language_options = language_options;
      catalog_language_options.DisableLanguageFeature(
          googlesql::FEATURE_SPANNER_LEGACY_DDL);
      googlesql::SampleCatalogImpl sample_catalog;
      absl::Status status = sample_catalog.LoadCatalogImpl(
          googlesql::BuiltinFunctionOptions(catalog_language_options));
      if (!status.ok()) {
        return status;
      }
      return analyze(sample_catalog.catalog());
    }
  }
  return absl::InvalidArgumentError("unknown named catalog");
}

size_t Utf8CodePointCount(absl::string_view value) {
  return std::count_if(value.begin(), value.end(), [](unsigned char byte) {
    return (byte & 0xc0) != 0x80;
  });
}

absl::Status ValidateId(const FrontendRequest& request) {
  if (!request.has_id()) {
    return absl::OkStatus();
  }
  const size_t length = Utf8CodePointCount(request.id());
  if (length == 0 || length > 256) {
    return absl::InvalidArgumentError(
        "id must contain between 1 and 256 Unicode characters");
  }
  return absl::OkStatus();
}

// Joins names into a sentence fragment: "a", "a and b", or "a, b and c".
std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      joined.append(index + 1 == names.size() ? " and " : ", ");
    }
    joined.append(names[index]);
  }
  return joined;
}

// Rewrites a protobuf field path such as "parse.extended_request.root" into
// the lower camel case spelling the JSON protocol uses.
std::string LowerCamelFieldPath(absl::string_view path) {
  std::string camel;
  camel.reserve(path.size());
  bool capitalize = false;
  for (const char c : path) {
    if (c == '_') {
      capitalize = true;
      continue;
    }
    camel.push_back(capitalize ? absl::ascii_toupper(c) : c);
    capitalize = false;
  }
  return camel;
}

// Reports the required protocol fields the request left unset as a sentence,
// using the public JSON names: "protocolVersion is required".
absl::Status MissingRequiredFieldsError(const FrontendRequest& request) {
  std::vector<std::string> paths;
  request.FindInitializationErrors(&paths);
  if (paths.empty()) {
    return absl::InvalidArgumentError(request.InitializationErrorString());
  }
  for (std::string& path : paths) {
    path = LowerCamelFieldPath(path);
  }
  return absl::InvalidArgumentError(absl::StrCat(
      JoinNames(paths), paths.size() == 1 ? " is required" : " are required"));
}

// What a rejected input line still says about itself: a usable id to echo, and
// which operation members the JSON object carries. Both are read from the
// generic JSON object because the request itself failed to decode.
struct ErrorEnvelope {
  std::optional<FrontendRequest> request;
  std::vector<std::string> operations;
};

ErrorEnvelope ParseErrorEnvelope(absl::string_view input) {
  ErrorEnvelope envelope;
  if (JsonDuplicateMemberScanner(input).Scan() !=
      JsonScanResult::kNoDuplicate) {
    return envelope;
  }

  google::protobuf::Struct object;
  if (!google::protobuf::util::JsonStringToMessage(input, &object).ok()) {
    return envelope;
  }

  const google::protobuf::OneofDescriptor* operation =
      FrontendRequest::descriptor()->FindOneofByName("operation");
  for (int index = 0; index < operation->field_count(); ++index) {
    const google::protobuf::FieldDescriptor* field = operation->field(index);
    if (object.fields().contains(field->json_name()) ||
        object.fields().contains(field->name())) {
      envelope.operations.emplace_back(field->json_name());
    }
  }

  const auto id = object.fields().find("id");
  if (id == object.fields().end() ||
      id->second.kind_case() != google::protobuf::Value::kStringValue) {
    return envelope;
  }
  FrontendRequest identified;
  identified.set_id(id->second.string_value());
  if (ValidateId(identified).ok()) {
    envelope.request = std::move(identified);
  }
  return envelope;
}

std::string OperationName(const FrontendRequest& request) {
  switch (request.operation_case()) {
    case FrontendRequest::kAnalyze:
      return "analyze";
    case FrontendRequest::kParse:
      return "parse";
    case FrontendRequest::kBuiltinFunctions:
      return "builtinFunctions";
    case FrontendRequest::kLanguageOptions:
      return "languageOptions";
    case FrontendRequest::kAnalyzerOptions:
      return "analyzerOptions";
    case FrontendRequest::OPERATION_NOT_SET:
      return "";
  }
  return "";
}

// Only analyze and parse carry a payload separable from the rest of their
// reply. The option-reading operations answer with their response proto and
// nothing else, so omitting it would leave an empty result rather than a
// cheaper one.
absl::Status ValidateResponseOptions(const FrontendRequest& request) {
  if (!request.response_options().omit_response_proto()) {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("responseOptions.omitResponseProto would leave the ",
                   OperationName(request),
                   " reply empty; it applies to analyze and parse only"));
}

absl::Status ValidateRequest(const FrontendRequest& request) {
  switch (request.operation_case()) {
    case FrontendRequest::kAnalyze: {
      const AnalyzeOperation& operation = request.analyze();
      if (operation.request().target_case() ==
          googlesql::local_service::AnalyzeRequest::TARGET_NOT_SET) {
        return absl::InvalidArgumentError("analyze target is required");
      }
      if (operation.has_named_catalog() &&
          operation.request().has_simple_catalog()) {
        return absl::InvalidArgumentError(
            "namedCatalog and request.simpleCatalog are mutually exclusive");
      }
      if (operation.has_named_catalog() &&
          operation.request().has_registered_catalog_id()) {
        return absl::InvalidArgumentError(
            "namedCatalog and request.registeredCatalogId are mutually "
            "exclusive");
      }
      return absl::OkStatus();
    }
    case FrontendRequest::kParse: {
      const ParseOperation& operation = request.parse();
      if (operation.input_case() == ParseOperation::INPUT_NOT_SET) {
        return absl::InvalidArgumentError("parse input is required");
      }
      if (operation.has_request() &&
          operation.request().target_case() ==
              googlesql::local_service::ParseRequest::TARGET_NOT_SET) {
        return absl::InvalidArgumentError("parse target is required");
      }
      return absl::OkStatus();
    }
    case FrontendRequest::kLanguageOptions: {
      // preset expresses everything request does, so a line carrying both
      // states one configuration twice and there is no defensible rule for
      // combining them.
      const LanguageOptionsOperation& operation = request.language_options();
      if (operation.has_request() && operation.has_preset()) {
        return absl::InvalidArgumentError(
            "request and preset are mutually exclusive");
      }
      return ValidateResponseOptions(request);
    }
    case FrontendRequest::kBuiltinFunctions:
    case FrontendRequest::kAnalyzerOptions:
      return ValidateResponseOptions(request);
    case FrontendRequest::OPERATION_NOT_SET:
      return absl::InvalidArgumentError("operation is required");
  }
  return absl::InvalidArgumentError("unknown operation");
}

struct SqlInput {
  absl::string_view text;
  absl::string_view filename;
};

std::optional<SqlInput> ErrorSqlInput(const FrontendRequest& request) {
  switch (request.operation_case()) {
    case FrontendRequest::kAnalyze: {
      const auto& analyze = request.analyze().request();
      switch (analyze.target_case()) {
        case googlesql::local_service::AnalyzeRequest::kSqlStatement:
          return SqlInput{analyze.sql_statement(), ""};
        case googlesql::local_service::AnalyzeRequest::kSqlExpression:
          return SqlInput{analyze.sql_expression(), ""};
        case googlesql::local_service::AnalyzeRequest::kParseResumeLocation:
          return SqlInput{analyze.parse_resume_location().input(),
                          analyze.parse_resume_location().filename()};
        case googlesql::local_service::AnalyzeRequest::TARGET_NOT_SET:
          return std::nullopt;
      }
      return std::nullopt;
    }
    case FrontendRequest::kParse: {
      const ParseOperation& parse = request.parse();
      if (parse.has_extended_request()) {
        return SqlInput{parse.extended_request().sql(), ""};
      }
      if (!parse.has_request()) {
        return std::nullopt;
      }
      switch (parse.request().target_case()) {
        case googlesql::local_service::ParseRequest::kSqlStatement:
          return SqlInput{parse.request().sql_statement(), ""};
        case googlesql::local_service::ParseRequest::kParseResumeLocation:
          return SqlInput{parse.request().parse_resume_location().input(),
                          parse.request().parse_resume_location().filename()};
        case googlesql::local_service::ParseRequest::TARGET_NOT_SET:
          return std::nullopt;
      }
      return std::nullopt;
    }
    case FrontendRequest::kBuiltinFunctions:
    case FrontendRequest::kLanguageOptions:
    case FrontendRequest::kAnalyzerOptions:
    case FrontendRequest::OPERATION_NOT_SET:
      return std::nullopt;
  }
  return std::nullopt;
}

void SetErrorLocation(const absl::Status& status,
                      const FrontendRequest* request, FrontendError* error) {
  if (request == nullptr) {
    return;
  }
  googlesql::ErrorLocation location;
  if (!googlesql::GetErrorLocation(status, &location)) {
    return;
  }
  std::optional<SqlInput> input = ErrorSqlInput(*request);
  if (!input.has_value() ||
      (location.has_filename() &&
       absl::string_view(location.filename()) != input->filename)) {
    return;
  }

  googlesql::ParseLocationTranslator translator(input->text);
  absl::StatusOr<int> byte_offset = translator.GetByteOffsetFromLineAndColumn(
      location.line(), location.column());
  if (!byte_offset.ok()) {
    return;
  }

  const int64_t line = static_cast<int64_t>(location.line()) +
                       location.input_start_line_offset();
  const int64_t column =
      static_cast<int64_t>(location.column()) +
      (location.line() == 1 ? location.input_start_column_offset() : 0);
  if (line < 1 || line > std::numeric_limits<int32_t>::max() || column < 1 ||
      column > std::numeric_limits<int32_t>::max()) {
    return;
  }

  FrontendErrorLocation* output = error->mutable_location();
  output->set_line(static_cast<int32_t>(line));
  output->set_column(static_cast<int32_t>(column));
  output->set_byte_offset(*byte_offset);
  output->set_filename(location.has_filename() ? location.filename()
                                               : std::string(input->filename));
}

FrontendResponse ErrorResponse(const absl::Status& status,
                               absl::string_view origin, int line_number,
                               absl::string_view operation,
                               const FrontendRequest* request) {
  FrontendResponse response;
  response.set_protocol_version(kProtocolVersion);
  if (request != nullptr && request->has_id()) {
    response.set_id(request->id());
  }
  FrontendError* error = response.mutable_error();
  error->set_origin(origin);
  error->set_status_code(static_cast<int32_t>(status.code()));
  error->set_status_name(std::string(absl::StatusCodeToString(status.code())));
  error->set_message(status.message());
  error->set_input_line(std::max(line_number, 1));
  if (!operation.empty()) {
    error->set_operation(operation);
  }
  SetErrorLocation(status, request, error);
  return response;
}

// Applies responseOptions.omitResponseProto to a successful reply: the
// serialized AST goes away, and everything else the reply carries stays. The
// analysis and the parse themselves are unchanged, so debugString and any
// error are exactly what the full reply would have contained.
//
// resume_byte_position lives in the same upstream message as the payload but
// outside its result oneof, so clearing the oneof preserves it. A response
// message left with no field at all is dropped rather than sent as an empty
// object, which keeps every response member meaningful.
void OmitResponseProto(FrontendResponse* response) {
  if (response->has_analyze()) {
    AnalyzeResult* analyze = response->mutable_analyze();
    analyze->mutable_response()->clear_result();
    if (!analyze->response().has_resume_byte_position()) {
      analyze->clear_response();
    }
    return;
  }
  if (!response->has_parse()) {
    return;
  }
  ParseResult* parse = response->mutable_parse();
  if (!parse->has_response()) {
    // The extended roots have no resume position to preserve.
    parse->clear_extended_response();
    return;
  }
  parse->mutable_response()->clear_result();
  if (!parse->response().has_resume_byte_position()) {
    parse->clear_response();
  }
}

ProcessResult Render(FrontendResponse response, bool ok) {
  absl::StatusOr<std::string> output = PrintJson(response);
  if (output.ok()) {
    return {.output = *std::move(output), .ok = ok};
  }

  // FrontendResponse contains only scalar fields and generated protobufs, so
  // serialization failure indicates an internal bug. Keep stdout valid NDJSON
  // even in that case.
  FrontendResponse fallback =
      ErrorResponse(absl::InternalError(output.status().message()), "internal",
                    1, "", nullptr);
  absl::StatusOr<std::string> fallback_json = PrintJson(fallback);
  return {
      .output =
          fallback_json.ok()
              ? *std::move(fallback_json)
              : R"({"protocolVersion":1,"error":{"origin":"internal","statusCode":13,"statusName":"INTERNAL","message":"response serialization failed","inputLine":1}})",
      .ok = false};
}

}  // namespace

ProcessResult Frontend::ProcessLine(absl::string_view input, int line_number) {
  FrontendRequest request;
  absl::Status parse_status = ParseJson(input, &request);
  if (!parse_status.ok()) {
    ErrorEnvelope envelope = ParseErrorEnvelope(input);
    const FrontendRequest* identified =
        envelope.request.has_value() ? &*envelope.request : nullptr;
    // Exactly one operation per request is a protocol rule, like the
    // "operation is required" rejection of zero operations. Report it here
    // rather than letting the ProtoJSON decoder complain about a repeated
    // oneof member and leak an internal message name.
    if (envelope.operations.size() > 1) {
      const absl::Status conflict = absl::InvalidArgumentError(
          absl::StrCat("exactly one operation is allowed; found ",
                       JoinNames(envelope.operations)));
      return Render(
          ErrorResponse(conflict, "protocol", line_number, "", identified),
          false);
    }
    return Render(
        ErrorResponse(parse_status, "proto_json", line_number, "", identified),
        false);
  }
  absl::Status id_status = ValidateId(request);
  if (!id_status.ok()) {
    return Render(ErrorResponse(id_status, "protocol", line_number,
                                OperationName(request), nullptr),
                  false);
  }
  if (!request.IsInitialized()) {
    return Render(ErrorResponse(MissingRequiredFieldsError(request), "protocol",
                                line_number, "", &request),
                  false);
  }
  if (request.protocol_version() != kProtocolVersion) {
    return Render(
        ErrorResponse(
            absl::InvalidArgumentError(absl::StrCat(
                "unsupported protocolVersion: ", request.protocol_version(),
                "; expected ", kProtocolVersion)),
            "protocol", line_number, OperationName(request), &request),
        false);
  }
  absl::Status validation_status = ValidateRequest(request);
  if (!validation_status.ok()) {
    return Render(ErrorResponse(validation_status, "protocol", line_number,
                                OperationName(request), &request),
                  false);
  }

  FrontendResponse response;
  response.set_protocol_version(kProtocolVersion);
  if (request.has_id()) {
    response.set_id(request.id());
  }

  absl::Status status;
  switch (request.operation_case()) {
    case FrontendRequest::kAnalyze: {
      AnalyzeResult* result = response.mutable_analyze();
      const googlesql::local_service::AnalyzeRequest analyze_request =
          EffectiveAnalyzeRequest(request.analyze());
      if (request.analyze().has_named_catalog()) {
        status =
            AnalyzeNamedCatalog(&service_, request.analyze().named_catalog(),
                                analyze_request, result);
      } else {
        status = AnalyzeInlineCatalog(&service_, analyze_request, result);
      }
      break;
    }
    case FrontendRequest::kParse: {
      const bool old_output_asc_explicitly =
          absl::GetFlag(FLAGS_output_asc_explicitly);
      absl::Cleanup restore_flag = [old_output_asc_explicitly] {
        absl::SetFlag(&FLAGS_output_asc_explicitly, old_output_asc_explicitly);
      };
      absl::SetFlag(
          &FLAGS_output_asc_explicitly,
          request.parse().has_render_options() &&
              request.parse().render_options().output_asc_explicitly());

      const ParseOperation parse = EffectiveParseOperation(request.parse());
      ParseResult* result = response.mutable_parse();
      if (parse.has_request()) {
        googlesql::local_service::ParseResponse local_response;
        status = service_.Parse(parse.request(), &local_response);
        if (!status.ok()) {
          status = RecoverScriptParseError(parse.request(), status);
          break;
        }
        absl::StatusOr<std::string> debug =
            NativeParseDebugString(parse.request());
        if (!debug.ok()) {
          status = debug.status();
          break;
        }
        result->mutable_response()->Swap(&local_response);
        result->set_debug_string(*std::move(debug));
      } else {
        std::string debug_string;
        status =
            ParseExtended(parse.extended_request(),
                          result->mutable_extended_response(), &debug_string);
        if (status.ok()) {
          result->set_debug_string(std::move(debug_string));
        }
      }
      break;
    }
    case FrontendRequest::kBuiltinFunctions: {
      googlesql::local_service::GetBuiltinFunctionsResponse local_response;
      status = service_.GetBuiltinFunctions(
          request.builtin_functions().request(), &local_response);
      if (status.ok()) {
        response.mutable_builtin_functions()->mutable_response()->Swap(
            &local_response);
      }
      break;
    }
    case FrontendRequest::kLanguageOptions: {
      const LanguageOptionsOperation& operation = request.language_options();
      googlesql::LanguageOptionsProto local_response;
      if (operation.has_preset()) {
        // Deliberately the same expansion analyze and parse apply, so this
        // operation reports the tool's behaviour rather than a description of
        // it. Nothing is merged on top, because the preset is the whole
        // request here.
        local_response = ExpandLanguageOptionsPreset(operation.preset());
      } else {
        status =
            service_.GetLanguageOptions(operation.request(), &local_response);
      }
      if (status.ok()) {
        response.mutable_language_options()->mutable_response()->Swap(
            &local_response);
      }
      break;
    }
    case FrontendRequest::kAnalyzerOptions: {
      googlesql::AnalyzerOptionsProto local_response;
      status = service_.GetAnalyzerOptions(request.analyzer_options().request(),
                                           &local_response);
      if (status.ok()) {
        response.mutable_analyzer_options()->mutable_response()->Swap(
            &local_response);
      }
      break;
    }
    case FrontendRequest::OPERATION_NOT_SET:
      status = absl::InvalidArgumentError("operation is required");
      break;
  }

  if (!status.ok()) {
    return Render(ErrorResponse(status, "googlesql", line_number,
                                OperationName(request), &request),
                  false);
  }
  if (request.response_options().omit_response_proto()) {
    OmitResponseProto(&response);
  }
  return Render(std::move(response), true);
}

bool IsBlankInputLine(absl::string_view input) {
  return absl::StripAsciiWhitespace(input).empty();
}

std::string VersionString() {
  return absl::StrCat("googlesql-frontend ", kVersion, " (protocol ",
                      kProtocolVersion, ", googlesql ", kGoogleSqlCommit, ")");
}

}  // namespace googlesql_frontend
