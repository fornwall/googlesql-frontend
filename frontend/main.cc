#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/flags/usage_config.h"
#include "absl/status/statusor.h"
#include "frontend/compatibility.h"
#include "frontend/frontend.h"

ABSL_FLAG(std::string, mode, "",
          "Compatibility mode: parse or analyze; empty selects NDJSON");
ABSL_FLAG(std::string, sql_mode, "query",
          "SQL root: query, expression, script, type, or parse_multiple");
ABSL_FLAG(std::string, enabled_language_features, "",
          "Comma-separated GoogleSQL language feature selection");
ABSL_FLAG(std::string, catalog, "none", "Catalog: none or sample");
ABSL_DECLARE_FLAG(bool, output_asc_explicitly);

int main(int argc, char** argv) {
  absl::FlagsUsageConfig usage_config;
  usage_config.version_string = [] {
    return googlesql_frontend::VersionString() + "\n";
  };
  absl::SetFlagsUsageConfig(usage_config);
  absl::SetProgramUsageMessage(
      "Read one googlesql-frontend JSON request per line on stdin and write "
      "one JSON response per line on stdout.");
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);
  if (positional.size() != 1) {
    std::cerr << "unexpected positional arguments\n";
    return 2;
  }

  const std::string mode = absl::GetFlag(FLAGS_mode);
  if (!mode.empty()) {
    const std::string sql((std::istreambuf_iterator<char>(std::cin)),
                          std::istreambuf_iterator<char>());
    if (std::cin.bad()) {
      std::cerr << "failed to read stdin\n";
      return 1;
    }
    googlesql_frontend::CompatibilityOptions options{
        .mode = mode,
        .sql_mode = absl::GetFlag(FLAGS_sql_mode),
        .enabled_language_features =
            absl::GetFlag(FLAGS_enabled_language_features),
        .output_asc_explicitly = absl::GetFlag(FLAGS_output_asc_explicitly),
        .catalog = absl::GetFlag(FLAGS_catalog),
    };
    absl::StatusOr<std::string> output =
        googlesql_frontend::ProcessCompatibility(options, sql);
    if (!output.ok()) {
      std::cerr << output.status().message() << '\n';
      return 1;
    }
    std::cout << *output << '\n';
    return std::cout ? 0 : 1;
  }

  googlesql_frontend::Frontend frontend;
  std::string line;
  int line_number = 0;
  while (std::getline(std::cin, line)) {
    ++line_number;
    if (line.empty()) {
      continue;
    }
    googlesql_frontend::ProcessResult result =
        frontend.ProcessLine(line, line_number);
    std::cout << result.output << std::endl;
    if (!std::cout) {
      std::cerr << "failed to write response\n";
      return 1;
    }
  }
  if (!std::cin.eof()) {
    std::cerr << "failed to read stdin\n";
    return 1;
  }
  return 0;
}
