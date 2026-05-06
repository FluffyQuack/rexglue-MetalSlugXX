/**
 * @file        rexglue/commands/include_scan.h
 * @brief       Scan source tree for #include directives matching a stale-set
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace rexglue::cli {

struct StaleIncludeMatch {
  std::filesystem::path file;  ///< Source file containing the directive
  std::size_t line_number;     ///< 1-based line number
  std::string include_target;  ///< The path inside the include quotes/brackets
  std::string raw_line;        ///< Verbatim line for warning output
};

/**
 * Recursively scan `src_dir` for `#include` directives whose target basename
 * is a member of `removed_basenames`. Returns matches sorted by file then
 * line number. If `src_dir` does not exist, returns an empty vector.
 */
std::vector<StaleIncludeMatch> ScanForStaleIncludes(
    const std::filesystem::path& src_dir, const std::unordered_set<std::string>& removed_basenames);

}  // namespace rexglue::cli
