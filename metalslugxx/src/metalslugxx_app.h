// metalslugxx - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/rex_app.h>
#include <rex/thread.h>
#include <rex/ui/keybinds.h>

#include "metalslugxx_settings.h"
#include "metalslugxx_crash.h"
#include "metalslugxx_dlc.h"
#include "metalslugxx_screenshot.h"
#include "metalslugxx_timing.h"

class MetalslugxxApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MetalslugxxApp>(new MetalslugxxApp(ctx, "metalslugxx",
        PPCImageConfig));
  }

  // Install last-chance native-crash diagnostics as soon as the logger is up,
  // so an access violation in recompiled code yields a symbolized backtrace
  // (the recompiled `sub_<guestaddr>` frames) instead of a silent exit. Also
  // raise the host timer resolution to 1 ms so guest frame-pacing waits don't
  // overshoot the default ~15.6 ms quantum (see metalslugxx_timing.h).
  void OnPostInitLogging() override {
    metalslugxx::InstallCrashHandler();
    metalslugxx::RaiseTimerResolution();

    // Read metalslugxx.ini (writing defaults on first run). When Sleep = False,
    // switch guest frame-pacing waits to the spin-to-deadline path so they don't
    // overshoot the OS timer granularity (see metalslugxx_settings.h).
    const metalslugxx::Config& cfg = metalslugxx::LoadConfig();
    rex::thread::SetPreciseTimedWait(!cfg.sleep);

    // Drive the XBLA trial/full state. The game asks "am I purchased?" via
    // XamContentGetLicenseMask, which the SDK answers from the `license_mask`
    // cvar (bit 0 = full license). TrialMode = False grants that bit so the full
    // game is unlocked; TrialMode = True leaves the mask at 0 (demo content only).
    // This is the game's first applied code patch -- we control what the license
    // API reports rather than touching guest code.
    rex::cvar::SetFlagByName("license_mask", cfg.trial_mode ? "0" : "1");
    REXLOG_INFO("license: TrialMode = {} -> license_mask = {}",
                cfg.trial_mode ? "True" : "False", cfg.trial_mode ? "0" : "1");

    // Unlock the playable character Leona (originally a marketplace add-on).
    // When enabled, synthesize the add-on's content package -- a `0.id` file
    // containing "leona" -- in the SDK content root (user_data_root, resolved
    // just above) so the game's own enumerate/mount/read path (DLC0:\0.id) finds
    // it and unlocks her, with no guest-code patch. See metalslugxx_dlc.h. This
    // runs before the runtime mounts content, so the package is in place in time.
    metalslugxx::ApplyLeonaUnlock(user_data_root(), cfg.unlock_leona);

    // The game opens its DLC by passing the marketplace content id it expects
    // (which it may hand us as raw, non-UTF-8 bytes). Rather than reproduce that
    // exact id, point every marketplace content OPEN at our synthetic package via
    // the SDK's marketplace_content_redirect cvar, so the DLC0 mount resolves to
    // our `0.id`. Cleared when disabled so stock behavior returns. See
    // metalslugxx_dlc.h and src/kernel/xam/xam_content.cpp.
    rex::cvar::SetFlagByName("marketplace_content_redirect",
                             cfg.unlock_leona ? metalslugxx::kLeonaPackageName : "");
    REXLOG_INFO("leona: marketplace_content_redirect = '{}'",
                cfg.unlock_leona ? metalslugxx::kLeonaPackageName : "");

    // Playfield filter selector. GameUpscaleFilter used to force all SDK
    // texture sampling, including menus; now it scopes the game's own sampler
    // state to the 320x240 playfield render/upscale path while shader-side
    // playfield smoothing fixes read this cvar. See metalslugxx_settings.h.
    rex::cvar::SetFlagByName("game_upscale_filter", cfg.game_upscale_filter);
    REXLOG_INFO("graphics: GameUpscaleFilter -> game_upscale_filter = {}", cfg.game_upscale_filter);

    // The guest renders a 1280x720 front buffer; the presenter then stretches it
    // to the host window (e.g. 1080p) in the final pass, which by default uses a
    // bilinear filter. When the user asks for Point, force that host-window
    // stretch to nearest-neighbour too, so the scoped playfield point path stays
    // crisp after it leaves the guest 1280x720 frame.
    // Set before SetupPresentation (OnPostInitLogging runs first), so the present
    // sampler is built with this value.
    if (cfg.game_upscale_filter == "point") {
      rex::cvar::SetFlagByName("present_point_filter", "true");
      REXLOG_INFO("graphics: GameUpscaleFilter=point -> present_point_filter = true");
    }

    // Diagnostic sample-phase bias for the in-game upscale "shuffling". Drives
    // the SDK `game_sample_texel_bias` cvar (texels added to 2D sample coords).
    // 0 = stock. See metalslugxx_settings.h [Graphics] SampleTexelBias. Only
    // applied when the INI explicitly set it, so a command-line
    // `--game_sample_texel_bias=...` (parsed during cvar::Init) is not clobbered.
    if (cfg.sample_texel_bias_set) {
      rex::cvar::SetFlagByName("game_sample_texel_bias", std::to_string(cfg.sample_texel_bias));
      REXLOG_INFO("graphics: SampleTexelBias -> game_sample_texel_bias = {} (from INI)",
                  cfg.sample_texel_bias);
    } else {
      REXLOG_INFO("graphics: SampleTexelBias not in INI; leaving game_sample_texel_bias "
                  "at its command-line/default value");
    }

    // Empirical fix for the in-game ~2px up-left offset. Drives the SDK
    // `game_lowres_tiled_bias` cvar (texels added to LOW-RES tiled 2D sample
    // coords only). 0 = stock. See metalslugxx_settings.h [Graphics] LowresTiledBias.
    // Only applied when the INI explicitly set it, so a command-line
    // `--game_lowres_tiled_bias=...` is not clobbered.
    if (cfg.lowres_tiled_bias_set) {
      rex::cvar::SetFlagByName("game_lowres_tiled_bias", std::to_string(cfg.lowres_tiled_bias));
      REXLOG_INFO("graphics: LowresTiledBias -> game_lowres_tiled_bias = {} (from INI)",
                  cfg.lowres_tiled_bias);
    } else {
      REXLOG_INFO("graphics: LowresTiledBias not in INI; leaving game_lowres_tiled_bias "
                  "at its command-line/default value");
    }

    // Keyboard-as-controller bindings. Drive the SDK keyboard input driver's
    // cvars from the INI [Keyboard1]/[Keyboard2] sections (key NAMES, validated
    // on load). The drivers read these per-frame, so setting them here is in
    // time. [Keyboard2] is a second local player on the same keyboard and is
    // disabled by default. See metalslugxx_settings.h and
    // src/input/keyboard/keyboard_input_driver.cpp.
    rex::cvar::SetFlagByName("keyboard_mode", cfg.keyboard_enabled ? "true" : "false");
    rex::cvar::SetFlagByName("kb_dpad_up", cfg.kb_dpad_up);
    rex::cvar::SetFlagByName("kb_dpad_down", cfg.kb_dpad_down);
    rex::cvar::SetFlagByName("kb_dpad_left", cfg.kb_dpad_left);
    rex::cvar::SetFlagByName("kb_dpad_right", cfg.kb_dpad_right);
    rex::cvar::SetFlagByName("kb_x", cfg.kb_button_x);
    rex::cvar::SetFlagByName("kb_a", cfg.kb_button_a);
    rex::cvar::SetFlagByName("kb_b", cfg.kb_button_b);
    rex::cvar::SetFlagByName("kb_y", cfg.kb_button_y);
    rex::cvar::SetFlagByName("kb_lshoulder", cfg.kb_lshoulder);
    rex::cvar::SetFlagByName("kb_rshoulder", cfg.kb_rshoulder);
    rex::cvar::SetFlagByName("kb_ltrigger", cfg.kb_ltrigger);
    rex::cvar::SetFlagByName("kb_rtrigger", cfg.kb_rtrigger);
    rex::cvar::SetFlagByName("kb_start", cfg.kb_start);
    rex::cvar::SetFlagByName("kb_back", cfg.kb_back);
    REXLOG_INFO("keyboard1: Enabled = {} | DpadU/D/L/R = {}/{}/{}/{} | X/A/B/Y = {}/{}/{}/{} | "
                "LB/RB = {}/{} | LT/RT = {}/{} | Start = {} | Back = {}",
                cfg.keyboard_enabled ? "True" : "False", cfg.kb_dpad_up, cfg.kb_dpad_down,
                cfg.kb_dpad_left, cfg.kb_dpad_right, cfg.kb_button_x, cfg.kb_button_a,
                cfg.kb_button_b, cfg.kb_button_y, cfg.kb_lshoulder, cfg.kb_rshoulder,
                cfg.kb_ltrigger, cfg.kb_rtrigger, cfg.kb_start, cfg.kb_back);

    rex::cvar::SetFlagByName("keyboard2_mode", cfg.keyboard2_enabled ? "true" : "false");
    rex::cvar::SetFlagByName("kb2_dpad_up", cfg.kb2_dpad_up);
    rex::cvar::SetFlagByName("kb2_dpad_down", cfg.kb2_dpad_down);
    rex::cvar::SetFlagByName("kb2_dpad_left", cfg.kb2_dpad_left);
    rex::cvar::SetFlagByName("kb2_dpad_right", cfg.kb2_dpad_right);
    rex::cvar::SetFlagByName("kb2_x", cfg.kb2_button_x);
    rex::cvar::SetFlagByName("kb2_a", cfg.kb2_button_a);
    rex::cvar::SetFlagByName("kb2_b", cfg.kb2_button_b);
    rex::cvar::SetFlagByName("kb2_y", cfg.kb2_button_y);
    rex::cvar::SetFlagByName("kb2_lshoulder", cfg.kb2_lshoulder);
    rex::cvar::SetFlagByName("kb2_rshoulder", cfg.kb2_rshoulder);
    rex::cvar::SetFlagByName("kb2_ltrigger", cfg.kb2_ltrigger);
    rex::cvar::SetFlagByName("kb2_rtrigger", cfg.kb2_rtrigger);
    rex::cvar::SetFlagByName("kb2_start", cfg.kb2_start);
    rex::cvar::SetFlagByName("kb2_back", cfg.kb2_back);
    REXLOG_INFO("keyboard2: Enabled = {} | DpadU/D/L/R = {}/{}/{}/{} | X/A/B/Y = {}/{}/{}/{} | "
                "LB/RB = {}/{} | LT/RT = {}/{} | Start = {} | Back = {}",
                cfg.keyboard2_enabled ? "True" : "False", cfg.kb2_dpad_up, cfg.kb2_dpad_down,
                cfg.kb2_dpad_left, cfg.kb2_dpad_right, cfg.kb2_button_x, cfg.kb2_button_a,
                cfg.kb2_button_b, cfg.kb2_button_y, cfg.kb2_lshoulder, cfg.kb2_rshoulder,
                cfg.kb2_ltrigger, cfg.kb2_rtrigger, cfg.kb2_start, cfg.kb2_back);
  }

  // Register project-specific keybinds. Called by the SDK right after the
  // built-in overlay binds (F3/F4/Backtick) are set up and the presenter is
  // live, so the presenter is reachable from the callback via runtime().
  //
  // F12 -> dump the guest-output framebuffer (pre present-scaling) to a PNG next
  // to the exe (via lodepng). General-purpose screenshot + a diagnostic for the
  // in-game upscale work: see metalslugxx_screenshot.h.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    rex::ui::RegisterBind("bind_guest_capture", "F12", "Save guest-output screenshot (PNG)",
                          [this] { metalslugxx::DumpGuestOutput(runtime()); });
  }

  // On quit, re-write metalslugxx.ini from the live config so an INI from an
  // older build gets any newly-added keys populated (and comments refreshed),
  // while keeping whatever the user already set. See metalslugxx_settings.h.
  void OnShutdown() override { metalslugxx::SaveConfig(); }

  // Resolve the writable-data paths before the runtime locks them in. Two
  // project conventions (see metalslugxx_settings.h):
  //   1. If --game_data_root was not given, default it to the working directory
  //      (`./`) so the game runs against the data sitting next to it.
  //   2. [System] Portable (default True) keeps the install self-contained: the
  //      SDK's user data, shader cache, and `<name>.toml` go in `./` instead of
  //      the per-user platform folders. This is the earliest hook that can read
  //      the INI (LoadConfig is idempotent; OnPostInitLogging reuses the result).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) return;  // can't resolve `./`; leave SDK defaults untouched

    if (paths.game_data_root.empty()) {
      paths.game_data_root = cwd;
      REXLOG_INFO("paths: --game_data_root not given; defaulting to working dir {}", cwd.string());
    }

    if (metalslugxx::LoadConfig().portable) {
      paths.user_data_root = cwd;
      paths.cache_root = cwd / "cache";
      paths.config_path = cwd / (GetName() + ".toml");
      REXLOG_INFO("paths: [System] Portable = True; writable data in working dir {}", cwd.string());
    }
  }

  // Override virtual hooks for customization:
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
};
