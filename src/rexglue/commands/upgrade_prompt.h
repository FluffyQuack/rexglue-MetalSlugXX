/**
 * @file        rexglue/commands/upgrade_prompt.h
 * @brief       TTY-aware consent prompt for codegen upgrade
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>
#include <vector>

#include <rex/result.h>

#include "upgrade_detect.h"

namespace rexglue::cli {

/**
 * Possible outcomes of the consent prompt.
 */
enum class UpgradeConsent {
  Granted,   ///< User said yes (or --force passed).
  Declined,  ///< User said no (or empty input on TTY).
};

/**
 * Prompt the user for consent to apply the overwrite plan.
 *
 * - If `force` is true, returns Granted without prompting.
 * - If stdin is a TTY, prints the plan and reads a y/n answer.
 *   "y" or "yes" (case-insensitive) returns Granted; everything else returns
 *   Declined.
 * - If stdin is not a TTY, prints the plan and returns an Err with a
 *   "re-run with --force" message; CodegenFromConfig translates that into a
 *   non-zero exit.
 */
rex::Result<UpgradeConsent> PromptForUpgradeConsent(const std::vector<OverwriteEntry>& plan,
                                                    std::string_view from_version,
                                                    std::string_view to_version, bool force);

}  // namespace rexglue::cli
