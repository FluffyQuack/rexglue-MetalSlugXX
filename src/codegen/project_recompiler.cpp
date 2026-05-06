/**
 * @file        codegen/project_recompiler.cpp
 * @brief       Project-level recompiler driving manifest-based multi-binary codegen
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/project_recompiler.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rex/codegen/analyze.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/config.h>
#include <rex/codegen/template_registry.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/user_module.h>

#include "template_registry_internal.h"

namespace rex::codegen {

namespace {

std::string DeriveTargetName(const std::filesystem::path& configPath) {
  auto stem = configPath.stem().string();
  std::replace(stem.begin(), stem.end(), '.', '_');
  std::replace(stem.begin(), stem.end(), ' ', '_');
  return stem;
}

}  // namespace

ProjectRecompiler::ProjectRecompiler(ManifestConfig manifest) : manifest_(std::move(manifest)) {}

Result<void> ProjectRecompiler::Run(const ProjectRecompilerOptions& opts) {
  namespace fs = std::filesystem;

  deletedFiles_.clear();
  writtenFiles_.clear();

  if (manifest_.modules.empty()) {
    REXLOG_INFO("Recompiling '{}' (entrypoint)", manifest_.projectName);
  } else {
    REXLOG_INFO("Recompiling '{}' (entrypoint + {} DLL{})", manifest_.projectName,
                manifest_.modules.size(), manifest_.modules.size() == 1 ? "" : "s");
  }

  // Collect all module entries
  struct ModuleEntry {
    fs::path configPath;
    std::string targetName;
    std::string guestPath;
    bool isDll;
    RecompilerConfig config;
  };

  std::vector<ModuleEntry> allModules;
  allModules.push_back({manifest_.entrypointConfig, "default_xex", "", false, {}});
  for (const auto& mod : manifest_.modules) {
    allModules.push_back({mod.config, DeriveTargetName(mod.config), mod.guestPath, true, {}});
  }

  if (!opts.targets.empty()) {
    std::vector<std::string> unknown;
    for (const auto& t : opts.targets) {
      bool match = std::any_of(allModules.begin() + 1, allModules.end(),
                               [&t](const ModuleEntry& m) { return m.targetName == t; });
      if (!match) {
        unknown.push_back(t);
      }
    }
    if (!unknown.empty()) {
      std::string known;
      for (size_t i = 1; i < allModules.size(); ++i) {
        if (i > 1)
          known += ", ";
        known += allModules[i].targetName;
      }
      std::string list;
      for (size_t i = 0; i < unknown.size(); ++i) {
        if (i > 0)
          list += ", ";
        list += unknown[i];
      }
      return Err<void>(ErrorCategory::Config,
                       fmt::format("Unknown --target value(s): {}. Known DLL targets: {}", list,
                                   known.empty() ? "(none)" : known));
    }
  }

  // Filter by --target (entrypoint always included)
  std::vector<ModuleEntry> targeted;
  targeted.push_back(std::move(allModules[0]));
  for (size_t i = 1; i < allModules.size(); ++i) {
    if (opts.targets.empty() || std::find(opts.targets.begin(), opts.targets.end(),
                                          allModules[i].targetName) != opts.targets.end()) {
      targeted.push_back(std::move(allModules[i]));
    }
  }

  // Load each targeted config exactly once; moved into CodegenContext below.
  for (auto& m : targeted) {
    if (!m.config.Load(m.configPath.string())) {
      return Err<void>(ErrorCategory::Config,
                       fmt::format("Failed to load config: {}", m.configPath.string()));
    }
  }

  // ---- Phase 0: Create one Runtime and load ALL XEXs upfront ----

  // Resolve entrypoint XEX path (needed to set up the Runtime content root)
  const auto& entryConfig = targeted[0].config;
  auto configDir = targeted[0].configPath.parent_path();
  fs::path entryXexPath = configDir / entryConfig.filePath;
  if (!entryConfig.patchedFilePath.empty()) {
    auto patched = configDir / entryConfig.patchedFilePath;
    if (fs::exists(patched))
      entryXexPath = patched;
  }
  if (!fs::exists(entryXexPath)) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Entrypoint XEX not found: {}", entryXexPath.string()));
  }
  entryXexPath = fs::canonical(entryXexPath);

  // gameRoot anchors VFS root and DLL guest_path derivation. Honor the
  // manifest override if set; otherwise default to the entrypoint's parent.
  fs::path gameRoot;
  if (manifest_.gameRoot && !manifest_.gameRoot->empty()) {
    fs::path resolved = configDir / *manifest_.gameRoot;
    if (!fs::exists(resolved) || !fs::is_directory(resolved)) {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("[project].game_root '{}' does not resolve to a directory",
                                   resolved.string()));
    }
    gameRoot = fs::canonical(resolved);
  } else {
    gameRoot = fs::canonical(entryXexPath.parent_path());
  }

  fs::path entryRel = fs::relative(entryXexPath, gameRoot);
  if (entryRel.empty() || *entryRel.begin() == "..") {
    return Err<void>(ErrorCategory::Validation,
                     fmt::format("Entrypoint XEX '{}' resolves outside game root '{}'",
                                 entryXexPath.string(), gameRoot.string()));
  }

  auto runtime = std::make_unique<Runtime>(gameRoot.string());
  auto rtStatus = runtime->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (rtStatus != X_STATUS_SUCCESS) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Failed to initialize Runtime: {:#x}", rtStatus));
  }

  std::string entryRelStr = entryRel.string();
  std::replace(entryRelStr.begin(), entryRelStr.end(), '/', '\\');
  auto entryVfsPath = "game:\\" + entryRelStr;
  rtStatus = runtime->LoadXexImage(entryVfsPath);
  if (rtStatus != X_STATUS_SUCCESS) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Failed to load entrypoint XEX: {:#x}", rtStatus));
  }

  std::vector<rex::system::object_ref<rex::system::UserModule>> dllModules;
  for (size_t i = 1; i < targeted.size(); ++i) {
    const auto& dllConfig = targeted[i].config;
    fs::path dllXexPath = configDir / dllConfig.filePath;
    if (!dllConfig.patchedFilePath.empty()) {
      auto patched = configDir / dllConfig.patchedFilePath;
      if (fs::exists(patched))
        dllXexPath = patched;
    }
    if (!fs::exists(dllXexPath)) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("DLL XEX not found: {}", dllXexPath.string()));
    }
    dllXexPath = fs::canonical(dllXexPath);

    auto relPath = fs::relative(dllXexPath, gameRoot);
    if (relPath.empty() || *relPath.begin() == "..") {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("DLL '{}' resolves outside game root '{}': {}",
                                   targeted[i].targetName, gameRoot.string(), dllXexPath.string()));
    }
    std::string relStr = relPath.string();
    std::replace(relStr.begin(), relStr.end(), '/', '\\');
    auto dllVfsPath = "game:\\" + relStr;
    auto userMod = runtime->kernel_state()->LoadUserModule(dllVfsPath, false);
    if (!userMod) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed to load DLL module: {}", targeted[i].targetName));
    }
    REXLOG_INFO("Loaded DLL module '{}' at base 0x{:08X}", targeted[i].targetName,
                userMod->xex_module()->base_address());
    dllModules.push_back(std::move(userMod));
  }

  // ---- Phase 1: Create contexts from loaded modules and analyze ----

  struct ContextEntry {
    CodegenContext ctx;
    const ModuleEntry* module;
  };
  std::vector<ContextEntry> contexts;

  auto* resolver = runtime->export_resolver();

  // Entrypoint context from executable module
  {
    auto execMod = runtime->kernel_state()->GetExecutableModule();
    auto bv = BinaryView::fromModule(*execMod->xex_module());

    RecompilerConfig cfg = std::move(targeted[0].config);
    if (opts.enableExceptionHandlers)
      cfg.generateExceptionHandlers = true;

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.setResolver(resolver);
    ctx.setConfigDir(targeted[0].configPath.parent_path());
    ctx.analysisState().format = "xex";
    ctx.analysisState().loadAddress = ctx.binary().baseAddress();
    ctx.analysisState().entryPoint = ctx.binary().entryPoint();
    ctx.analysisState().imageSize = ctx.binary().imageSize();
    ctx.setHasDllModules(!manifest_.modules.empty());
    if (ctx.Config().isDll.has_value())
      ctx.setDllModule(*ctx.Config().isDll);

    contexts.push_back({std::move(ctx), &targeted[0]});
  }

  // DLL contexts from stored user module refs
  for (size_t i = 0; i < dllModules.size(); ++i) {
    auto& userMod = dllModules[i];
    auto bv = BinaryView::fromModule(*userMod->xex_module());

    RecompilerConfig cfg = std::move(targeted[i + 1].config);
    if (opts.enableExceptionHandlers)
      cfg.generateExceptionHandlers = true;

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.setResolver(resolver);
    ctx.setConfigDir(targeted[i + 1].configPath.parent_path());
    ctx.analysisState().format = "xex";
    ctx.analysisState().loadAddress = ctx.binary().baseAddress();
    ctx.analysisState().entryPoint = ctx.binary().entryPoint();
    ctx.analysisState().imageSize = ctx.binary().imageSize();
    ctx.setDllModule(ctx.Config().isDll.value_or(true));
    ctx.setHasDllModules(true);

    contexts.push_back({std::move(ctx), &targeted[i + 1]});
  }

  // Run analysis on each context
  for (auto& entry : contexts) {
    REXLOG_INFO("Analyzing '{}'...", entry.module->targetName);
    auto result = Analyze(entry.ctx);
    if (!result) {
      REXLOG_ERROR("Analysis failed for '{}'", entry.module->targetName);
      return result;
    }
  }

  // ---- Phase 2: Verify non-overlapping address ranges ----

  for (size_t i = 0; i < contexts.size(); ++i) {
    uint32_t a_base = contexts[i].ctx.binary().baseAddress();
    uint32_t a_end = a_base + contexts[i].ctx.binary().imageSize();
    for (size_t j = i + 1; j < contexts.size(); ++j) {
      uint32_t b_base = contexts[j].ctx.binary().baseAddress();
      uint32_t b_end = b_base + contexts[j].ctx.binary().imageSize();
      if (a_base < b_end && b_base < a_end) {
        return Err<void>(ErrorCategory::Validation,
                         fmt::format("Module '{}' [{:08X}, {:08X}) overlaps '{}' [{:08X}, {:08X})",
                                     contexts[i].module->targetName, a_base, a_end,
                                     contexts[j].module->targetName, b_base, b_end));
      }
    }
  }

  // ---- Phase 3: Write output ----

  deletedFiles_.clear();
  writtenFiles_.clear();
  for (auto& entry : contexts) {
    REXLOG_INFO("Writing output for '{}'...", entry.module->targetName);
    CodegenWriter writer(entry.ctx, runtime.get());
    if (!writer.write(opts.force)) {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("Write failed for '{}'", entry.module->targetName));
    }
    deletedFiles_.insert(deletedFiles_.end(), writer.deletedFiles().begin(),
                         writer.deletedFiles().end());
    writtenFiles_.insert(writtenFiles_.end(), writer.writtenFiles().begin(),
                         writer.writtenFiles().end());
  }

  // ---- Phases 4 & 5: Emit project-level artifacts (module_registry + cmake) ----
  // Both phases iterate the same `targeted` list so registry entries always
  // correspond to actually-emitted DLL targets.

  if (manifest_.modules.empty()) {
    REXLOG_INFO("Project recompiler complete");
    return Ok();
  }

  auto outputPath = contexts[0].ctx.configDir() / contexts[0].ctx.Config().outDirectoryPath;
  std::error_code mk_ec;
  fs::create_directories(outputPath, mk_ec);
  if (mk_ec) {
    return Err<void>(ErrorCategory::IO, fmt::format("Failed to create output dir {}: {}",
                                                    outputPath.string(), mk_ec.message()));
  }

  // Phase 4: module_registry.cpp
  {
    nlohmann::json registryData;
    registryData["project"] = manifest_.projectName;

    auto& dllArray = registryData["dll_modules"];
    dllArray = nlohmann::json::array();
    for (size_t i = 1; i < targeted.size(); ++i) {
      auto& mod = targeted[i];
      auto guestPath = mod.guestPath;
      std::replace(guestPath.begin(), guestPath.end(), '\\', '/');
      nlohmann::json dllEntry;
      dllEntry["pe_name"] = fs::path(mod.guestPath).filename().string();
      dllEntry["guest_path"] = guestPath;
      dllEntry["shared_lib_name"] = manifest_.projectName + "_" + mod.targetName;
      dllArray.push_back(dllEntry);
    }

    TemplateRegistry registry;
    auto registryContent = renderWithJson(registry, "codegen/module_registry_cpp", registryData);

    auto registryPath = outputPath / "module_registry.cpp";
    std::ofstream f(registryPath);
    if (!f) {
      return Err<void>(ErrorCategory::IO, fmt::format("Failed to open {}", registryPath.string()));
    }
    f << registryContent;
    if (!f.good()) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed while writing {}", registryPath.string()));
    }
    writtenFiles_.push_back(registryPath.filename().string());
    REXLOG_INFO("Wrote {}", registryPath.string());
  }

  // Phase 5: dll_targets.cmake
  {
    nlohmann::json dllTargetsData;
    auto& dllTargetsArray = dllTargetsData["dll_modules"];
    dllTargetsArray = nlohmann::json::array();
    for (size_t i = 0; i < dllModules.size(); ++i) {
      auto& mod = targeted[i + 1];
      auto& dllCtx = contexts[i + 1].ctx;
      nlohmann::json entry;
      entry["target_name"] = mod.targetName;
      entry["lib_name"] = manifest_.projectName + "_" + mod.targetName;
      entry["output_dir"] = dllCtx.Config().outDirectoryPath;
      dllTargetsArray.push_back(entry);
    }

    TemplateRegistry registry;
    auto dllCmakeContent = renderWithJson(registry, "codegen/dll_targets_cmake", dllTargetsData);

    auto dllCmakePath = outputPath / "dll_targets.cmake";
    std::ofstream cf(dllCmakePath);
    if (!cf) {
      return Err<void>(ErrorCategory::IO, fmt::format("Failed to open {}", dllCmakePath.string()));
    }
    cf << dllCmakeContent;
    if (!cf.good()) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed while writing {}", dllCmakePath.string()));
    }
    writtenFiles_.push_back(dllCmakePath.filename().string());
    REXLOG_INFO("Wrote {}", dllCmakePath.string());
  }

  REXLOG_INFO("Project recompiler complete");
  return Ok();
}

}  // namespace rex::codegen
