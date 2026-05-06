/**
 * @file        tests/unit/rexglue/include_scan_test.cpp
 * @brief       Tests for stale-include scanner
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include "rexglue/commands/include_scan.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

struct TempSrc {
  fs::path root;
  TempSrc() : root(fs::temp_directory_path() / "include_scan_test") {
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~TempSrc() { fs::remove_all(root); }

  void write(const std::string& rel_path, const std::string& content) const {
    fs::path target = root / rel_path;
    fs::create_directories(target.parent_path());
    std::ofstream f(target);
    f << content;
  }
};

}  // namespace

TEST_CASE("IncludeScan: matches quoted #include with stale basename", "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("main.cpp", R"(#include "generated/foo_config.h"
int main() { return 0; }
)");
  std::unordered_set<std::string> removed{"foo_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  REQUIRE(results.size() == 1u);
  CHECK(results[0].file == tmp.root / "main.cpp");
  CHECK(results[0].line_number == 1u);
  CHECK(results[0].include_target == "generated/foo_config.h");
}

TEST_CASE("IncludeScan: matches angle-bracket #include with stale basename",
          "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("a.cpp", "#include <ppc_config.h>\n");
  std::unordered_set<std::string> removed{"ppc_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  REQUIRE(results.size() == 1u);
  CHECK(results[0].include_target == "ppc_config.h");
}

TEST_CASE("IncludeScan: ignores unrelated #include directives", "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("a.cpp", R"(#include <vector>
#include "bar.h"
)");
  std::unordered_set<std::string> removed{"foo_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  CHECK(results.empty());
}

TEST_CASE("IncludeScan: walks subdirectories and supports common extensions",
          "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("a.cpp", "#include \"foo_config.h\"\n");
  tmp.write("sub/b.h", "#include \"foo_config.h\"\n");
  tmp.write("sub/c.hpp", "#include <foo_config.h>\n");
  tmp.write("sub/d.inl", "#include \"foo_config.h\"\n");
  tmp.write("sub/e.txt", "#include \"foo_config.h\"\n");  // not a source extension
  std::unordered_set<std::string> removed{"foo_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  CHECK(results.size() == 4u);
}

TEST_CASE("IncludeScan: handles whitespace and trailing comments", "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("a.cpp", "  #  include   \"foo_config.h\"   // legacy header\n");
  std::unordered_set<std::string> removed{"foo_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  REQUIRE(results.size() == 1u);
  CHECK(results[0].include_target == "foo_config.h");
}

TEST_CASE("IncludeScan: returns empty when src dir does not exist", "[rexglue][include_scan]") {
  fs::path nonexistent = fs::temp_directory_path() / "include_scan_does_not_exist";
  fs::remove_all(nonexistent);
  std::unordered_set<std::string> removed{"foo.h"};
  auto results = rexglue::cli::ScanForStaleIncludes(nonexistent, removed);
  CHECK(results.empty());
}

TEST_CASE("IncludeScan: matches case-insensitively", "[rexglue][include_scan]") {
  TempSrc tmp;
  tmp.write("a.cpp", "#include \"Foo_Config.H\"\n");
  std::unordered_set<std::string> removed{"foo_config.h"};

  auto results = rexglue::cli::ScanForStaleIncludes(tmp.root, removed);
  REQUIRE(results.size() == 1u);
  CHECK(results[0].include_target == "Foo_Config.H");
}
