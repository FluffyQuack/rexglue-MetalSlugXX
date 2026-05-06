/**
 * @file        rexglue/commands/codegen_command.cpp
 * @brief       Code generation command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_command.h"
#include "include_scan.h"
#include "template_utils.h"
#include "upgrade_detect.h"
#include "upgrade_prompt.h"

#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rex/codegen/codegen.h>
#include <rex/codegen/config.h>
#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/version.h>

namespace rexglue::cli {

namespace {

namespace fs = std::filesystem;

Result<fs::path> EnsureManifest(const fs::path& legacy_config_path) {
  rex::codegen::RecompilerConfig legacy;
  if (!legacy.Load(legacy_config_path.string())) {
    return Err<fs::path>(rex::ErrorCategory::IO,
                         "Failed to load config: " + legacy_config_path.string());
  }
  if (legacy.projectName.empty() || legacy.projectName == "rex") {
    return Err<fs::path>(rex::ErrorCategory::Config,
                         "Config has no projectName; cannot auto-generate manifest");
  }

  auto names = parse_app_name(legacy.projectName);
  fs::path manifest_path = legacy_config_path.parent_path() / (names.snake_case + "_manifest.toml");
  if (fs::exists(manifest_path)) {
    return rex::Ok(std::move(manifest_path));
  }

  rex::codegen::TemplateRegistry registry;
  nlohmann::json data = {{"names", names_to_json(names)},
                         {"sdk_version", REXGLUE_VERSION_NUMERIC},
                         {"include_stamp", false}};
  std::string content = registry.render("init/manifest_toml", data.dump());

  std::string legacy_name = legacy_config_path.filename().string();
  std::string expected = names.snake_case + "_default_xex.toml";
  if (legacy_name != expected) {
    auto pos = content.find(expected);
    if (pos != std::string::npos) {
      content.replace(pos, expected.size(), legacy_name);
    }
  }

  if (!write_file(manifest_path, content)) {
    return Err<fs::path>(rex::ErrorCategory::IO,
                         "Failed to write manifest: " + manifest_path.string());
  }
  REXLOG_INFO("Created {} (wrapping legacy config {})", manifest_path.filename().string(),
              legacy_name);
  return rex::Ok(std::move(manifest_path));
}

Result<void> RunUpgradeIfNeeded(const fs::path& manifest_path, const CliContext& ctx) {
  auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  if (!manifest) {
    return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest for upgrade detection");
  }

  std::string current_version = REXGLUE_VERSION_NUMERIC;
  if (manifest->sdkVersion && *manifest->sdkVersion == current_version) {
    return rex::Ok();  // Fast path: stamp matches current SDK.
  }

  UpgradeDetect detect;
  auto plan = detect.Plan(manifest->manifestDir, manifest->projectName, current_version);
  if (plan.empty()) {
    return rex::Ok();  // Nothing to overwrite; stamp at end of CodegenFromConfig.
  }

  std::string from_version = manifest->sdkVersion.value_or("");
  auto consent =
      PromptForUpgradeConsent(plan, from_version, current_version, ctx.skip_upgrade_consent);
  if (!consent) {
    return Err<void>(consent.error());
  }
  if (*consent == UpgradeConsent::Declined) {
    return Err<void>(rex::ErrorCategory::UserAbort, "Upgrade declined; codegen aborted.");
  }

  for (const auto& entry : plan) {
    std::error_code mk_ec;
    fs::create_directories(entry.path.parent_path(), mk_ec);
    if (mk_ec) {
      return Err<void>(rex::ErrorCategory::IO, fmt::format("Failed to create directory for {}: {}",
                                                           entry.path.string(), mk_ec.message()));
    }
    std::ofstream out(entry.path);
    if (!out) {
      return Err<void>(rex::ErrorCategory::IO,
                       fmt::format("Failed to open for write: {}", entry.path.string()));
    }
    out << entry.rendered_content;
    if (!out.good()) {
      return Err<void>(rex::ErrorCategory::IO,
                       fmt::format("Failed to write: {}", entry.path.string()));
    }
    REXLOG_INFO("Upgraded: {}", entry.path.generic_string());
  }
  return rex::Ok();
}

void RunStaleIncludeScan(const fs::path& manifest_path,
                         const rex::codegen::ProjectRecompiler& recompiler) {
  std::unordered_set<std::string> written(recompiler.writtenFiles().begin(),
                                          recompiler.writtenFiles().end());
  std::unordered_set<std::string> removed;
  for (const auto& f : recompiler.deletedFiles()) {
    if (!written.contains(f)) {
      removed.insert(f);
    }
  }
  if (removed.empty())
    return;

  fs::path src_dir = manifest_path.parent_path() / "src";
  auto matches = ScanForStaleIncludes(src_dir, removed);
  if (matches.empty())
    return;

  REXLOG_WARN("{} source file(s) reference headers no longer emitted by codegen:", matches.size());
  for (const auto& m : matches) {
    REXLOG_WARN("  {}:{}: {}", m.file.generic_string(), m.line_number, m.raw_line);
  }
  REXLOG_WARN("These headers were emitted by older SDK versions. Update the includes by hand.");
}

Result<void> RunManifest(const fs::path& manifest_path, const CliContext& ctx) {
  auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  if (!manifest) {
    return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
  }
  rex::codegen::ProjectRecompiler recompiler(std::move(*manifest));
  rex::codegen::ProjectRecompilerOptions opts{
      .targets = ctx.targets,
      .force = ctx.generate_despite_errors,
      .enableExceptionHandlers = ctx.enableExceptionHandlers,
  };
  auto result = recompiler.Run(opts);
  if (!result)
    return result;

  RunStaleIncludeScan(manifest_path, recompiler);
  return rex::Ok();
}

}  // namespace

Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx) {
  REXLOG_INFO("Generating code with config: {}", config_path);

  fs::path manifest_path;
  if (rex::codegen::ManifestConfig::IsManifest(config_path)) {
    manifest_path = config_path;
  } else {
    auto ensured = EnsureManifest(config_path);
    if (!ensured) {
      if (ensured.error().category != rex::ErrorCategory::Config) {
        return Err<void>(ensured.error());
      }
      auto pipeline = rex::codegen::CodegenPipeline::Create(config_path);
      if (!pipeline) {
        return Err<void>(pipeline.error());
      }
      if (ctx.enableExceptionHandlers) {
        pipeline->context().Config().generateExceptionHandlers = true;
        REXLOG_INFO("Exception handler generation enabled");
      }
      return pipeline->Run(ctx.generate_despite_errors);
    }
    manifest_path = std::move(*ensured);
  }

  auto upgraded = RunUpgradeIfNeeded(manifest_path, ctx);
  if (!upgraded)
    return upgraded;

  auto run_result = RunManifest(manifest_path, ctx);
  if (!run_result)
    return run_result;

  if (!rex::codegen::ManifestConfig::WriteSdkVersionStamp(manifest_path, REXGLUE_VERSION_NUMERIC)) {
    REXLOG_WARN("Failed to stamp manifest sdkVersion; next run may re-prompt");
  }

  return rex::Ok();
}

}  // namespace rexglue::cli
