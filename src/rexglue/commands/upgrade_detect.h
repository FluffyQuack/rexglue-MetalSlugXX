/**
 * @file        rexglue/commands/upgrade_detect.h
 * @brief       Detect SDK-managed files that need overwriting on codegen upgrade
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rexglue::cli {

struct OverwriteEntry {
  std::filesystem::path path;    ///< Target file on disk
  std::string rendered_content;  ///< Content to write
};

/**
 * Detects SDK-managed files outside the per-binary output directory whose
 * content has drifted from their template, and produces an overwrite plan.
 */
class UpgradeDetect {
 public:
  UpgradeDetect();

  /**
   * Build the overwrite plan for the project rooted at `project_root`.
   * Compares each SDK-managed template's freshly-rendered content against
   * the file on disk; entries that differ (or are missing) are returned.
   */
  std::vector<OverwriteEntry> Plan(const std::filesystem::path& project_root,
                                   std::string_view project_name, std::string_view sdk_version);

  /**
   * Render the init/rexglue_cmake template fresh. Exposed so tests can
   * produce content that matches Plan()'s expected on-disk version.
   */
  std::string RenderRexglueCmake(std::string_view project_name, std::string_view sdk_version);
};

}  // namespace rexglue::cli
