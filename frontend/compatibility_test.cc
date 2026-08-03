#include "frontend/compatibility.h"

#include <string>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"

namespace googlesql_frontend {
namespace {

TEST(CompatibilityTest, ParsesAllSupportedRoots) {
  CompatibilityOptions options{.mode = "parse", .sql_mode = "query"};
  absl::StatusOr<std::string> query = ProcessCompatibility(options, "SELECT 1");
  ASSERT_TRUE(query.ok()) << query.status();
  EXPECT_NE(query->find("QueryStatement"), std::string::npos);

  options.sql_mode = "expression";
  absl::StatusOr<std::string> expression =
      ProcessCompatibility(options, "1 + 2");
  ASSERT_TRUE(expression.ok()) << expression.status();
  EXPECT_NE(expression->find("BinaryExpression"), std::string::npos);

  options.sql_mode = "script";
  absl::StatusOr<std::string> script =
      ProcessCompatibility(options, "DECLARE x INT64; SET x = 1;");
  ASSERT_TRUE(script.ok()) << script.status();
  EXPECT_NE(script->find("Script"), std::string::npos);

  options.sql_mode = "type";
  absl::StatusOr<std::string> type =
      ProcessCompatibility(options, "ARRAY<STRUCT<x INT64>>");
  ASSERT_TRUE(type.ok()) << type.status();
  EXPECT_NE(type->find("ArrayType"), std::string::npos);
}

TEST(CompatibilityTest, ParsesMultipleStatements) {
  CompatibilityOptions options{.mode = "parse", .sql_mode = "parse_multiple"};
  absl::StatusOr<std::string> output =
      ProcessCompatibility(options, "SELECT 1; SELECT 2;");
  ASSERT_TRUE(output.ok()) << output.status();
  const size_t first = output->find("QueryStatement");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(output->find("QueryStatement", first + 1), std::string::npos);
}

TEST(CompatibilityTest, ControlsExplicitAscendingDebugText) {
  CompatibilityOptions options{
      .mode = "parse",
      .sql_mode = "query",
      .output_asc_explicitly = true,
  };
  absl::StatusOr<std::string> explicit_asc =
      ProcessCompatibility(options, "SELECT 1 ORDER BY 1 ASC");
  ASSERT_TRUE(explicit_asc.ok()) << explicit_asc.status();
  EXPECT_NE(explicit_asc->find("ASC EXPLICITLY"), std::string::npos);

  options.output_asc_explicitly = false;
  absl::StatusOr<std::string> regular_asc =
      ProcessCompatibility(options, "SELECT 1 ORDER BY 1 ASC");
  ASSERT_TRUE(regular_asc.ok()) << regular_asc.status();
  EXPECT_EQ(regular_asc->find("ASC EXPLICITLY"), std::string::npos);
}

TEST(CompatibilityTest, AnalyzesWithEmptyAndLiveSampleCatalogs) {
  CompatibilityOptions options{.mode = "analyze", .sql_mode = "query"};
  absl::StatusOr<std::string> builtin =
      ProcessCompatibility(options, "SELECT ABS(-1)");
  ASSERT_TRUE(builtin.ok()) << builtin.status();
  EXPECT_NE(builtin->find("QueryStmt"), std::string::npos);

  options.catalog = "sample";
  absl::StatusOr<std::string> sample =
      ProcessCompatibility(options, "SELECT key FROM KeyValue");
  ASSERT_TRUE(sample.ok()) << sample.status();
  EXPECT_NE(sample->find("KeyValue"), std::string::npos);
}

TEST(CompatibilityTest, FoldsDateToDatetimeWithoutEpochRegression) {
  CompatibilityOptions options{.mode = "analyze", .sql_mode = "query"};
  absl::StatusOr<std::string> output = ProcessCompatibility(
      options, "SELECT CAST(DATE '2024-03-14' AS DATETIME)");
  ASSERT_TRUE(output.ok()) << output.status();
  EXPECT_NE(output->find("2024-03-14 00:00:00"), std::string::npos);
  EXPECT_EQ(output->find("1970-01-01 00:00:00"), std::string::npos);
}

TEST(CompatibilityTest, RejectsUnsupportedOrMalformedOptions) {
  CompatibilityOptions options{.mode = "analyze", .sql_mode = "type"};
  EXPECT_FALSE(ProcessCompatibility(options, "INT64").ok());

  options.sql_mode = "query";
  options.catalog = "unknown";
  EXPECT_FALSE(ProcessCompatibility(options, "SELECT 1").ok());

  options.catalog = "none";
  options.enabled_language_features = "NOT_A_FEATURE";
  EXPECT_FALSE(ProcessCompatibility(options, "SELECT 1").ok());
}

}  // namespace
}  // namespace googlesql_frontend
