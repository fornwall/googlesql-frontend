#ifndef GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_
#define GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "frontend/protocol.pb.h"
#include "googlesql/local_service/local_service.h"
#include "googlesql/proto/options.pb.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/testdata/sample_catalog_impl.h"

namespace googlesql_frontend {

inline constexpr int kProtocolVersion = 1;
inline constexpr absl::string_view kVersion = "0.5.0";
inline constexpr absl::string_view kGoogleSqlCommit =
    "3eec7971a4706b04c10019f89d09dda854442c30";

// The named catalog most recently built for a Frontend, held so that a stream
// of requests naming the same one is answered from a single build.
//
// Which catalog and the language options are the only inputs to a build: the
// rest of the analyzer options a request carries reach the analysis, not the
// catalog. So a build is reusable exactly as long as neither changes. One
// entry is enough for a client that analyzes many statements under one
// configuration, which is what the named catalogs are for; a client that
// alternates between configurations rebuilds on each change, exactly as every
// request did before.
//
// A held catalog is handed out to be read, never to be added to. Analysis does
// not register anything a statement declares, so a CREATE TABLE resolved
// against a held catalog stays invisible to the next request, which is the
// isolation a per-request build gave. The one write a request would otherwise
// make, resolving the catalog's property-graph definitions, is done once here
// instead, and the entry owns the analyzer outputs it produces so they last as
// long as the catalog that points into them.
//
// A Frontend is used sequentially and holds its own cache, so no entry is
// shared between threads.
class NamedCatalogCache {
 public:
  // Returns the catalog to analyze against, building it unless the entry held
  // already answers for this catalog and these language options. The catalog
  // belongs to the cache and stays valid until the next call.
  absl::StatusOr<googlesql::SimpleCatalog*> Get(
      NamedCatalog named_catalog,
      const googlesql::LanguageOptionsProto& language_options);

 private:
  // A built catalog and everything it borrows. Exactly one of the two owning
  // members holds the catalog `catalog` points at.
  //
  // An entry is only ever created whole and destroyed whole, which is what
  // makes the member order below the order teardown runs in. Assignment is
  // deleted to keep it that way: assigning one entry over another would run
  // member by member in declaration order and so free the catalog while the
  // outputs that read it were still alive. Replacing a held entry therefore
  // goes through the owning unique_ptr, which destroys the old entry as a
  // whole.
  struct Entry {
    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry& operator=(Entry&&) = delete;

    std::unique_ptr<googlesql::SimpleCatalog> simple_catalog;
    std::unique_ptr<googlesql::SampleCatalogImpl> sample_catalog;
    googlesql::SimpleCatalog* catalog = nullptr;
    // Declared last so it is destroyed first. These outputs own the resolved
    // expressions the catalog's property graphs point at, and hold types from
    // the catalog's own type factory in return; a value of a container type
    // reads its type as it is destroyed, so the outputs have to go first.
    std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>>
        property_graph_artifacts;
  };

  static absl::StatusOr<std::unique_ptr<Entry>> Build(
      NamedCatalog named_catalog,
      const googlesql::LanguageOptions& language_options);

  // What entry_ was built from. Meaningful only while entry_ holds a catalog.
  std::string key_;
  std::unique_ptr<Entry> entry_;
};

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
  NamedCatalogCache named_catalogs_;
};

// True when an input line carries no request and is therefore skipped rather
// than reported as an error. Lines holding only whitespace are blank: an
// NDJSON stream should behave the way it looks when read or edited by hand.
bool IsBlankInputLine(absl::string_view input);

std::string VersionString();

}  // namespace googlesql_frontend

#endif  // GOOGLESQL_FRONTEND_FRONTEND_FRONTEND_H_
