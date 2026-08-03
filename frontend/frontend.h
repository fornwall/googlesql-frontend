#ifndef GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_
#define GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_

#include <string>

#include "absl/strings/string_view.h"
#include "googlesql/local_service/local_service.h"

namespace googlesql_frontend {

inline constexpr int kProtocolVersion = 1;
inline constexpr absl::string_view kVersion = "0.1.0";
inline constexpr absl::string_view kGoogleSqlCommit =
    "1f8aa333f4d6353cd3a64471fc83121df72df3f7";

struct ProcessResult {
  std::string output;
  bool ok;
};

// Processes independent requests through one local-service instance. Calls
// are intentionally sequential: one input line always produces one output
// line before the next request begins.
class Frontend {
 public:
  ProcessResult ProcessLine(absl::string_view input, int line_number);

 private:
  googlesql::local_service::GoogleSqlLocalServiceImpl service_;
};

std::string VersionString();

}  // namespace googlesql_frontend

#endif  // GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_
