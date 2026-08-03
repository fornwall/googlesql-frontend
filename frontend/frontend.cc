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

absl::Status AnalyzeWithCatalog(
    googlesql::local_service::GoogleSqlLocalServiceImpl* service,
    const googlesql::local_service::AnalyzeRequest& request,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    googlesql::SimpleCatalog* catalog, AnalyzeResult* result) {
  googlesql::TypeFactory type_factory;
  googlesql::AnalyzerOptions options;
  absl::Status status = googlesql::AnalyzerOptions::Deserialize(
      request.options(), pools, &type_factory, &options);
  if (!status.ok()) {
    return status;
  }
  // Locations are a typed protocol field. Keep the default prose message free
  // of rendered coordinates, while preserving an explicit display-oriented
  // error mode and attaching the typed payload independently.
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
    const AnalyzeOperation& operation, AnalyzeResult* result) {
  googlesql::local_service::AnalyzeRequest effective_request =
      operation.request();
  if (!effective_request.options().has_language_options()) {
    googlesql::LanguageOptions language_options;
    language_options.EnableMaximumLanguageFeatures();
    language_options.SetSupportsAllStatementKinds();
    language_options.EnableAllReservableKeywords();
    language_options.Serialize(
        effective_request.mutable_options()->mutable_language_options());
  }
  const googlesql::local_service::AnalyzeRequest& request = effective_request;
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
  absl::Status options_status = googlesql::AnalyzerOptions::Deserialize(
      request.options(), pools->ordered, &options_type_factory,
      &analyzer_options);
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

  switch (operation.named_catalog()) {
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

std::optional<FrontendRequest> ParseErrorEnvelope(absl::string_view input) {
  if (JsonDuplicateMemberScanner(input).Scan() !=
      JsonScanResult::kNoDuplicate) {
    return std::nullopt;
  }

  google::protobuf::Struct object;
  if (!google::protobuf::util::JsonStringToMessage(input, &object).ok()) {
    return std::nullopt;
  }
  const auto id = object.fields().find("id");
  if (id == object.fields().end() ||
      id->second.kind_case() != google::protobuf::Value::kStringValue) {
    return std::nullopt;
  }

  FrontendRequest envelope;
  envelope.set_id(id->second.string_value());
  if (!ValidateId(envelope).ok()) {
    return std::nullopt;
  }
  return envelope;
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
    case FrontendRequest::kBuiltinFunctions:
      return absl::OkStatus();
    case FrontendRequest::OPERATION_NOT_SET:
      return absl::InvalidArgumentError("operation is required");
  }
  return absl::InvalidArgumentError("unknown operation");
}

std::string OperationName(const FrontendRequest& request) {
  switch (request.operation_case()) {
    case FrontendRequest::kAnalyze:
      return "analyze";
    case FrontendRequest::kParse:
      return "parse";
    case FrontendRequest::kBuiltinFunctions:
      return "builtinFunctions";
    case FrontendRequest::OPERATION_NOT_SET:
      return "";
  }
  return "";
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
    std::optional<FrontendRequest> envelope = ParseErrorEnvelope(input);
    return Render(ErrorResponse(parse_status, "proto_json", line_number, "",
                                envelope.has_value() ? &*envelope : nullptr),
                  false);
  }
  absl::Status id_status = ValidateId(request);
  if (!id_status.ok()) {
    return Render(ErrorResponse(id_status, "protocol", line_number,
                                OperationName(request), nullptr),
                  false);
  }
  if (!request.IsInitialized()) {
    return Render(ErrorResponse(absl::InvalidArgumentError(
                                    request.InitializationErrorString()),
                                "protocol", line_number, "", &request),
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
      if (request.analyze().has_named_catalog()) {
        status = AnalyzeNamedCatalog(&service_, request.analyze(), result);
      } else {
        status = AnalyzeInlineCatalog(&service_, request.analyze().request(),
                                      result);
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

      ParseResult* result = response.mutable_parse();
      if (request.parse().has_request()) {
        googlesql::local_service::ParseResponse local_response;
        status = service_.Parse(request.parse().request(), &local_response);
        if (!status.ok()) {
          status = RecoverScriptParseError(request.parse().request(), status);
          break;
        }
        absl::StatusOr<std::string> debug =
            NativeParseDebugString(request.parse().request());
        if (!debug.ok()) {
          status = debug.status();
          break;
        }
        result->mutable_response()->Swap(&local_response);
        result->set_debug_string(*std::move(debug));
      } else {
        std::string debug_string;
        status =
            ParseExtended(request.parse().extended_request(),
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
        BuiltinFunctionsResult* result = response.mutable_builtin_functions();
        result->mutable_response()->Swap(&local_response);
        result->set_debug_string("");
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
  return Render(std::move(response), true);
}

std::string VersionString() {
  return absl::StrCat("googlesql-frontend ", kVersion, " (protocol ",
                      kProtocolVersion, ", googlesql ", kGoogleSqlCommit, ")");
}

}  // namespace googlesql_frontend
