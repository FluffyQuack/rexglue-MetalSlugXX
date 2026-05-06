/**
 * @file        rexglue/commands/upgrade_detect.cpp
 * @brief       Detect SDK-managed files needing overwrite on codegen upgrade
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "upgrade_detect.h"
#include "template_utils.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <rex/codegen/template_registry.h>

namespace rexglue::cli {

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

UpgradeDetect::UpgradeDetect() = default;

std::string UpgradeDetect::RenderRexglueCmake(std::string_view project_name,
                                              std::string_view sdk_version) {
  rex::codegen::TemplateRegistry registry;
  auto names = parse_app_name(std::string(project_name));
  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"sdk_version", std::string(sdk_version)},
  };
  return registry.render("init/rexglue_cmake", data.dump());
}

std::vector<OverwriteEntry> UpgradeDetect::Plan(const fs::path& project_root,
                                                std::string_view project_name,
                                                std::string_view sdk_version) {
  std::vector<OverwriteEntry> plan;

  // Currently registered SDK-managed files (live outside per-binary outDirectoryPath).
  // Add new entries here as more templates become upgrade-aware.
  fs::path rexglue_cmake = project_root / "generated" / "rexglue.cmake";
  std::string rendered = RenderRexglueCmake(project_name, sdk_version);
  std::string on_disk = fs::exists(rexglue_cmake) ? ReadFile(rexglue_cmake) : std::string{};
  if (rendered != on_disk) {
    plan.push_back({rexglue_cmake, std::move(rendered)});
  }

  return plan;
}

}  // namespace rexglue::cli
