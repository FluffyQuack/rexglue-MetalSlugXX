/**
 * @file        tests/unit/rexglue/upgrade_detect_test.cpp
 * @brief       Tests for upgrade detection plan builder
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include "rexglue/commands/upgrade_detect.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempProject {
  fs::path root;
  TempProject() : root(fs::temp_directory_path() / "upgrade_detect_test") {
    fs::remove_all(root);
    fs::create_directories(root / "generated");
  }
  ~TempProject() { fs::remove_all(root); }

  void writeRexglueCmake(const std::string& content) const {
    std::ofstream f(root / "generated" / "rexglue.cmake");
    f << content;
  }
};

}  // namespace

TEST_CASE("UpgradeDetect: empty plan when rexglue.cmake matches template",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  rexglue::cli::UpgradeDetect detect;
  std::string rendered = detect.RenderRexglueCmake("mygame", "0.8.0");
  tp.writeRexglueCmake(rendered);

  auto plan = detect.Plan(tp.root, "mygame", "0.8.0");
  CHECK(plan.empty());
}

TEST_CASE("UpgradeDetect: non-empty plan when rexglue.cmake differs", "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeRexglueCmake("# obviously stale content\n");

  rexglue::cli::UpgradeDetect detect;
  auto plan = detect.Plan(tp.root, "mygame", "0.8.0");
  REQUIRE(plan.size() == 1u);
  CHECK(plan[0].path.filename() == "rexglue.cmake");
  CHECK(plan[0].rendered_content.find("rex::runtime") != std::string::npos);
}

TEST_CASE("UpgradeDetect: non-empty plan when rexglue.cmake is missing",
          "[rexglue][upgrade_detect]") {
  TempProject tp;  // generated/rexglue.cmake never written

  rexglue::cli::UpgradeDetect detect;
  auto plan = detect.Plan(tp.root, "mygame", "0.8.0");
  REQUIRE(plan.size() == 1u);
  CHECK(plan[0].path == tp.root / "generated" / "rexglue.cmake");
}
