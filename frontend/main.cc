#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/flags/usage_config.h"
#include "absl/log/initialize.h"
#include "frontend/frontend.h"

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
  // GoogleSQL logs through Abseil. Without this the first message it writes is
  // preceded by a warning that logging was never initialized, which would put
  // a line of Abseil's own diagnostics in front of the tool's.
  absl::InitializeLog();

  googlesql_frontend::Frontend frontend;
  std::string line;
  int line_number = 0;
  while (std::getline(std::cin, line)) {
    ++line_number;
    if (googlesql_frontend::IsBlankInputLine(line)) {
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
