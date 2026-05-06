/**
 * @file        rexglue/cli_utils.h
 * @brief       CLI utility functions and helpers
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
#include <vector>

namespace rexglue::cli {

/**
 * Shared CLI context passed to command handlers
 */
struct CliContext {
  bool verbose = false;
  bool overwrite_existing = false;       ///< Overwrite non-empty init dir / existing config
  bool generate_despite_errors = false;  ///< Codegen: emit output despite validation errors
  bool skip_upgrade_consent = false;     ///< Codegen: auto-accept SDK upgrade overwrites
  bool enableExceptionHandlers = false;  ///< Enable SEH exception handler generation
  std::vector<std::string> targets;      ///< --target filter for multi-binary codegen
};

}  // namespace rexglue::cli
