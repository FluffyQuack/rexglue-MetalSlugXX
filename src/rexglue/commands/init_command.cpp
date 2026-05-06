/**
 * @file        rexglue/commands/init_command.cpp
 * @brief       Project initialization command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "init_command.h"
#include "template_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <rex/codegen/manifest.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/version.h>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace rexglue::cli {

using rex::Err;
using rex::Error;
using rex::ErrorCategory;
using rex::Ok;

Result<void> InitProject(const InitOptions& opts, const CliContext& ctx) {
  (void)ctx;  // Currently unused

  // Validate required options
  if (opts.app_name.empty()) {
    return Err<void>(ErrorCategory::Config, "--app_name is required");
  }
  if (opts.app_root.empty()) {
    return Err<void>(ErrorCategory::Config, "--app_root is required");
  }

  // Validate and parse app name
  std::string validation_error;
  if (!validate_app_name(opts.app_name, validation_error)) {
    return Err<void>(ErrorCategory::Config, validation_error);
  }
  auto names = parse_app_name(opts.app_name);

  rex::codegen::TemplateRegistry registry;
  if (!opts.template_dir.empty())
    registry.loadOverrides(opts.template_dir);

  nlohmann::json data = {{"names", names_to_json(names)},
                         {"sdk_version", REXGLUE_VERSION_NUMERIC},
                         {"include_stamp", true}};
  std::string jsonStr = data.dump();

  fs::path root = fs::absolute(opts.app_root);

  REXLOG_INFO("Initializing project '{}' at: {}", names.snake_case, root.string());
  REXLOG_INFO("Mode: {}", opts.sdk_example ? "SDK example" : "standalone");

  // Check if directory exists and has contents
  if (fs::exists(root)) {
    if (!fs::is_directory(root)) {
      return Err<void>(ErrorCategory::IO, "Path exists but is not a directory: " + root.string());
    }

    bool has_contents = false;
    for (const auto& entry : fs::directory_iterator(root)) {
      (void)entry;
      has_contents = true;
      break;
    }

    if (has_contents && !opts.force) {
      return Err<void>(ErrorCategory::IO,
                       "Directory is not empty. Use --force to overwrite: " + root.string());
    }
  }

  // Create directory structure
  REXLOG_INFO("Creating directory structure...");

  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create root directory: " + ec.message());
  }

  fs::create_directories(root / "src", ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create src directory: " + ec.message());
  }

  fs::create_directories(root / "generated", ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create generated directory: " + ec.message());
  }

  // Generate files
  REXLOG_INFO("Generating project files...");

  if (!write_file(root / "CMakeLists.txt", registry.render("init/cmakelists", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write CMakeLists.txt");
  }
  REXLOG_DEBUG("  Created CMakeLists.txt");

  // generated/rexglue.cmake (SDK-managed)
  if (!write_file(root / "generated" / "rexglue.cmake",
                  registry.render("init/rexglue_cmake", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write generated/rexglue.cmake");
  }
  REXLOG_DEBUG("  Created generated/rexglue.cmake");

  if (!write_file(root / "src" / "main.cpp", registry.render("init/main_cpp", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write main.cpp");
  }
  REXLOG_DEBUG("  Created src/main.cpp");

  // src/{name}_app.h (user-owned)
  std::string app_header_filename = names.snake_case + "_app.h";
  if (!write_file(root / "src" / app_header_filename,
                  registry.render("init/app_header", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write src/" + app_header_filename);
  }
  REXLOG_DEBUG("  Created src/{}", app_header_filename);

  std::string config_filename = names.snake_case + "_default_xex.toml";
  if (!write_file(root / config_filename, registry.render("init/config_toml", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write " + config_filename);
  }
  REXLOG_DEBUG("  Created {}", config_filename);

  // Generate manifest TOML (project-level entry point, references per-binary configs)
  std::string manifest_filename = names.snake_case + "_manifest.toml";
  if (!write_file(root / manifest_filename, registry.render("init/manifest_toml", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write " + manifest_filename);
  }
  REXLOG_DEBUG("  Created {}", manifest_filename);

  if (!write_file(root / "CMakePresets.json", registry.render("init/cmake_presets", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write CMakePresets.json");
  }
  REXLOG_DEBUG("  Created CMakePresets.json");

  // Print success message with next steps
  REXLOG_INFO("Project '{}' initialized in '{}' successfully!", names.snake_case, opts.app_root);

  return Ok();
}

Result<void> InitModule(const InitModuleOptions& opts, const CliContext& ctx) {
  namespace fs = std::filesystem;

  // Find manifest in project root
  fs::path root = fs::absolute(opts.app_root);
  std::error_code dir_ec;
  fs::path manifestPath;
  for (const auto& entry : fs::directory_iterator(root, dir_ec)) {
    if (entry.path().extension() == ".toml" &&
        rex::codegen::ManifestConfig::IsManifest(entry.path())) {
      manifestPath = entry.path();
      break;
    }
  }
  if (dir_ec) {
    return Err<void>(ErrorCategory::IO, fmt::format("Cannot read project root '{}': {}",
                                                    root.string(), dir_ec.message()));
  }
  if (manifestPath.empty()) {
    return Err<void>(rex::ErrorCategory::Config,
                     "No manifest found in project root. Run 'rexglue init' first.");
  }

  auto manifest = rex::codegen::ManifestConfig::Load(manifestPath);
  if (!manifest) {
    return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
  }

  // Derive module name from XEX filename (dots/spaces -> underscores)
  fs::path xexFile(opts.xex_path);
  std::string moduleName = xexFile.stem().string();
  std::replace(moduleName.begin(), moduleName.end(), '.', '_');
  std::replace(moduleName.begin(), moduleName.end(), ' ', '_');
  // Lowercase the module name for consistency
  std::transform(moduleName.begin(), moduleName.end(), moduleName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  std::string configName = manifest->projectName + "_" + moduleName + ".toml";
  fs::path configPath = manifestPath.parent_path() / configName;

  if (fs::exists(configPath) && !ctx.overwrite_existing) {
    return Err<void>(
        rex::ErrorCategory::Config,
        fmt::format("Config already exists: {}. Use --force to overwrite.", configPath.string()));
  }

  // Validate xex_path exists
  fs::path absoluteXexPath = fs::weakly_canonical(fs::absolute(opts.xex_path));
  if (!fs::exists(absoluteXexPath)) {
    return Err<void>(rex::ErrorCategory::IO,
                     fmt::format("XEX file not found: {} (resolved from {})",
                                 absoluteXexPath.string(), opts.xex_path));
  }

  // Generate per-module config from template
  rex::codegen::TemplateRegistry registry;
  std::string manifestFilename = manifestPath.filename().string();

  auto names = parse_app_name(manifest->projectName);

  // xex_path: project-relative for codegen joins.
  fs::path relXexPath = fs::relative(absoluteXexPath, root);
  std::string xexPath = relXexPath.generic_string();  // forward slashes

  // guest_path: canonicalize to the guest-visible module path used at runtime.
  std::string guestPath =
      rex::codegen::CanonicalizeModuleGuestPath(opts.guest_path, manifest->projectName);

  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"module_name", moduleName},
      {"xex_path", xexPath},
      {"guest_path", guestPath},
      {"manifest_filename", manifestFilename},
  };
  std::string jsonStr = data.dump();

  if (!write_file(configPath, registry.render("init/module_config_toml", jsonStr))) {
    return Err<void>(rex::ErrorCategory::IO, "Failed to write " + configName);
  }
  REXLOG_DEBUG("  Created {}", configName);

  // Create generated output directory for this module
  fs::create_directories(root / "generated" / moduleName);

  toml::table manifestTbl;
  try {
    manifestTbl = toml::parse_file(manifestPath.string());
  } catch (const toml::parse_error& err) {
    return Err<void>(rex::ErrorCategory::Config,
                     fmt::format("Manifest parse error: {}", err.what()));
  }

  auto* modulesArr = manifestTbl["modules"].as_array();
  if (!modulesArr) {
    manifestTbl.insert_or_assign("modules", toml::array{});
    modulesArr = manifestTbl["modules"].as_array();
  }

  bool already_present = false;
  for (const auto& mod : *modulesArr) {
    auto* modTbl = mod.as_table();
    if (!modTbl) {
      continue;
    }
    auto existing_config = (*modTbl)["config"].value_or<std::string>("");
    auto existing_guest = (*modTbl)["guestPath"].value_or<std::string>("");
    if (existing_config == configName || existing_guest == guestPath) {
      already_present = true;
      break;
    }
  }

  if (!already_present) {
    toml::table newEntry;
    newEntry.insert_or_assign("config", configName);
    newEntry.insert_or_assign("guestPath", guestPath);
    modulesArr->push_back(std::move(newEntry));

    auto tmpPath = manifestPath;
    tmpPath += ".tmp";
    {
      std::ofstream out(tmpPath);
      if (!out) {
        return Err<void>(rex::ErrorCategory::IO,
                         "Failed to open manifest tmp for writing: " + tmpPath.string());
      }
      out << manifestTbl;
      if (!out.good()) {
        std::error_code ignore;
        fs::remove(tmpPath, ignore);
        return Err<void>(rex::ErrorCategory::IO,
                         "Failed while writing manifest tmp: " + tmpPath.string());
      }
    }
    std::error_code ec;
    fs::rename(tmpPath, manifestPath, ec);
    if (ec) {
      std::error_code ignore;
      fs::remove(tmpPath, ignore);
      return Err<void>(rex::ErrorCategory::IO,
                       "Failed to rename manifest tmp into place: " + ec.message());
    }
  } else {
    REXLOG_INFO("  Manifest already lists this module; skipping append");
  }

  REXLOG_INFO("Module '{}' added to project", moduleName);
  REXLOG_INFO("  Config:     {}", configName);
  REXLOG_INFO("  Guest path: {}", guestPath);
  REXLOG_INFO("  Output dir: generated/{}", moduleName);

  return Ok();
}

}  // namespace rexglue::cli
