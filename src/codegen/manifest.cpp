/**
 * @file        codegen/manifest.cpp
 * @brief       Manifest TOML parser for multi-binary projects
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/manifest.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include <toml++/toml.hpp>

#include <rex/logging.h>
#include <rex/system/guest_path.h>

namespace rex::codegen {

std::string CanonicalizeModuleGuestPath(std::string_view path, std::string_view project_name) {
  std::string guest_path = rex::system::NormalizeGuestPath(path);

  if (!project_name.empty()) {
    std::string lower_project(project_name);
    std::transform(lower_project.begin(), lower_project.end(), lower_project.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string project_assets_prefix = lower_project + "/assets/";
    if (guest_path.rfind(project_assets_prefix, 0) == 0) {
      guest_path.erase(0, project_assets_prefix.size());
    }
  }

  return guest_path;
}

std::optional<ManifestConfig> ManifestConfig::Load(const std::filesystem::path& path) {
  toml::table tbl;
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse manifest {}: {}", path.string(), err.what());
    return std::nullopt;
  }

  ManifestConfig manifest;
  manifest.manifestDir = path.parent_path();

  // [project]
  if (auto project = tbl["project"].as_table()) {
    manifest.projectName = (*project)["projectName"].value_or<std::string>("");
    auto stamp = (*project)["sdkVersion"].value<std::string>();
    if (stamp && !stamp->empty()) {
      manifest.sdkVersion = *stamp;
    }
  }
  if (manifest.projectName.empty()) {
    REXLOG_ERROR("Manifest missing [project].projectName");
    return std::nullopt;
  }

  // [entrypoint]
  if (auto ep = tbl["entrypoint"].as_table()) {
    auto configPath = (*ep)["config"].value_or<std::string>("");
    if (configPath.empty()) {
      REXLOG_ERROR("Manifest missing [entrypoint].config");
      return std::nullopt;
    }
    manifest.entrypointConfig = manifest.manifestDir / configPath;
  } else {
    REXLOG_ERROR("Manifest missing [entrypoint] section");
    return std::nullopt;
  }

  // [[modules]]
  if (auto modules = tbl["modules"].as_array()) {
    size_t index = 0;
    for (const auto& mod : *modules) {
      auto* modTbl = mod.as_table();
      if (!modTbl) {
        REXLOG_ERROR("Manifest [[modules]] entry #{} is not a table", index);
        return std::nullopt;
      }

      ManifestModuleEntry entry;
      entry.config = manifest.manifestDir / (*modTbl)["config"].value_or<std::string>("");
      entry.guestPath = CanonicalizeModuleGuestPath(
          (*modTbl)["guestPath"].value_or<std::string>(""), manifest.projectName);

      if (entry.config.filename().empty() || entry.guestPath.empty()) {
        REXLOG_ERROR("Manifest [[modules]] entry missing config or guestPath");
        return std::nullopt;
      }
      manifest.modules.push_back(std::move(entry));
      ++index;
    }
  }

  return manifest;
}

bool ManifestConfig::IsManifest(const std::filesystem::path& path) {
  try {
    auto tbl = toml::parse_file(path.string());
    return tbl.contains("entrypoint");
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool ManifestConfig::WriteSdkVersionStamp(const std::filesystem::path& path,
                                          std::string_view version) {
  toml::table tbl;
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse manifest for stamping {}: {}", path.string(), err.what());
    return false;
  }

  auto* project = tbl["project"].as_table();
  if (!project) {
    REXLOG_ERROR("Manifest missing [project] table; cannot stamp: {}", path.string());
    return false;
  }
  project->insert_or_assign("sdkVersion", std::string(version));

  auto tmp_path = path;
  tmp_path += ".tmp";

  {
    std::ofstream out(tmp_path);
    if (!out) {
      REXLOG_ERROR("Failed to open manifest tmp for writing: {}", tmp_path.string());
      return false;
    }
    out << tbl;
    if (!out.good()) {
      REXLOG_ERROR("Failed while writing manifest tmp: {}", tmp_path.string());
      std::error_code ignore;
      std::filesystem::remove(tmp_path, ignore);
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    REXLOG_ERROR("Failed to rename manifest tmp into place: {}", ec.message());
    std::error_code ignore;
    std::filesystem::remove(tmp_path, ignore);
    return false;
  }
  return true;
}

}  // namespace rex::codegen
