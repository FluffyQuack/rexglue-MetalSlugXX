/**
 * @file        rexglue/commands/include_scan.cpp
 * @brief       Scan source tree for #include directives matching a stale-set
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "include_scan.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>

namespace rexglue::cli {

namespace fs = std::filesystem;

namespace {

bool IsSourceExtension(const fs::path& p) {
  static const std::unordered_set<std::string> kExts = {".cpp", ".cc",  ".cxx", ".c",  ".h",
                                                        ".hh",  ".hpp", ".hxx", ".inl"};
  auto ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return kExts.contains(ext);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

std::vector<StaleIncludeMatch> ScanForStaleIncludes(
    const fs::path& src_dir, const std::unordered_set<std::string>& removed_basenames) {
  std::vector<StaleIncludeMatch> matches;
  if (!fs::exists(src_dir) || !fs::is_directory(src_dir)) {
    return matches;
  }

  std::unordered_set<std::string> removed_lower;
  removed_lower.reserve(removed_basenames.size());
  for (const auto& name : removed_basenames) {
    removed_lower.insert(ToLower(name));
  }

  static const std::regex include_re(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");

  for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file())
      continue;
    if (!IsSourceExtension(entry.path()))
      continue;

    std::ifstream in(entry.path());
    if (!in)
      continue;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
      ++line_number;
      std::smatch m;
      if (!std::regex_search(line, m, include_re))
        continue;
      std::string target = m[1].str();
      auto pos = target.find_last_of("/\\");
      std::string basename = (pos == std::string::npos) ? target : target.substr(pos + 1);
      if (removed_lower.contains(ToLower(basename))) {
        matches.push_back({entry.path(), line_number, std::move(target), line});
      }
    }
  }

  std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
    if (a.file != b.file)
      return a.file < b.file;
    return a.line_number < b.line_number;
  });
  return matches;
}

}  // namespace rexglue::cli
