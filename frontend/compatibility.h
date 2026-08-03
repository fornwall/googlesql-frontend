#ifndef GOOGLESQL_FRONTEND_FRONTEND_COMPATIBILITY_H_
#define GOOGLESQL_FRONTEND_FRONTEND_COMPATIBILITY_H_

#include <string>

#include "absl/status/statusor.h"

namespace googlesql_frontend {

struct CompatibilityOptions {
  std::string mode;
  std::string sql_mode = "query";
  std::string enabled_language_features;
  bool output_asc_explicitly = false;
  std::string catalog = "none";
};

// Runs one transitional, human-readable parser or analyzer request. This API
// never evaluates SQL or returns rows.
absl::StatusOr<std::string> ProcessCompatibility(
    const CompatibilityOptions& options, const std::string& sql);

}  // namespace googlesql_frontend

#endif  // GOOGLESQL_FRONTEND_FRONTEND_COMPATIBILITY_H_
