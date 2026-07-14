// metalslugxx - ReXGlue Recompiled Project
//
// Tiny INI-style settings read at startup from `metalslugxx.ini`, located next to
// the executable (the same folder the SDK uses for `<name>.toml` and `logs/`).
//
// Design contract:
//   * Running with no INI is fine -- a default INI is written on first run.
//   * A missing file, missing section, or missing key all fall back to the
//     compiled-in defaults (the `Config` member initializers below).
//   * The format is forgiving: `[Section]` headers, `Key = Value` pairs,
//     `//`, `;` and `#` line comments, blank lines, and case-insensitive
//     section/key names.

#pragma once

#include <filesystem>
#include <string>

namespace metalslugxx {

// Parsed settings, pre-populated with the defaults used when a value is absent.
struct Config {
  // [System]
  //
  // Sleep = True : guest frame-pacing waits use a plain OS sleep (default;
  //                the proven-good behavior after the timeBeginPeriod(1) fix).
  // Sleep = False: waits sleep off the bulk of the interval then spin-wait the
  //                final ~1.5 ms so they land exactly on the deadline instead of
  //                overshooting the OS timer granularity (tighter pacing, a
  //                little more CPU). Wired to rex::thread::SetPreciseTimedWait.
  bool sleep = true;

  // Portable = True : keep this install self-contained -- the SDK's writable
  //                   data (user_data_root, the shader cache, and the runtime's
  //                   `<name>.toml`) lives in the working directory `./` instead
  //                   of the per-user platform folders (default). Combined with
  //                   game_data_root defaulting to `./`, the whole thing can be
  //                   dropped on a USB stick and run in place.
  // Portable = False: use the SDK's platform defaults (e.g. %USERPROFILE% on
  //                   Windows) for that writable data.
  //
  // Note `metalslugxx.ini` itself always lives in `./` -- it is the bootstrap
  // file that carries this flag, so its location can't depend on it. Applied in
  // OnConfigurePaths (which runs before the runtime's paths are locked in).
  bool portable = true;

  // [Game]
  //
  // TrialMode = True : the game runs in trial/demo mode -- only the content the
  //                    demo unlocks is accessible. This is what the XBLA build
  //                    does when the marketplace has not granted a full license.
  // TrialMode = False: full game; every mode/level is unlocked (default).
  //
  // Implemented by driving the XBLA license the game reads via
  // XamContentGetLicenseMask: False sets license bit 0 (purchased), True clears
  // it (trial). Applied to the `license_mask` cvar in OnPostInitLogging.
  bool trial_mode = false;

  // SkipLogos = True : skip the boot logo sequence (XBLA "Arcade" logo, the
  //                    US-region ESRB logo, and the SNK Playmore "Vendor" logo)
  //                    and go straight to the title screen.
  // SkipLogos = False: play the logos as on hardware (default).
  //
  // Implemented as the project's first guest-code patch: a [[midasm_hook]] at
  // 0x825715E8 (inside the boot-logo scene sub_82571500 = ms_boot_logo_sequence)
  // jumps over all three logo-display loops to the scene teardown at 0x82571700
  // when this is True. See metalslugxx_config.toml and metalslugxx_hooks.cpp.
  bool skip_logos = true;

  // DisableGoPopups = True : suppress the idle "GO NOW" direction prompts and
  //                          their se001 sound cue during gameplay.
  // DisableGoPopups = False: keep the prompts as on hardware.
  //
  // Implemented with [[midasm_hook]] return gates on the right/left prompt
  // show/play state functions. When suppressed, the prompt object is pushed
  // back into its native idle-delay state so it does not retry every frame.
  bool disable_go_popups = false;

  // UnlockLeona = True : the playable character Leona is unlocked (default).
  // UnlockLeona = False: Leona stays locked, as on a console without the add-on.
  //
  // On Xbox 360 Leona was a marketplace add-on: a DLC package whose 5-byte
  // `0.id` file held the text "leona". The game mounts that package as `DLC0`,
  // reads `DLC0:\0.id`, and unlocks her on a match. When True we synthesize that
  // package on disk (a `0.id` containing "leona") in the SDK content root so the
  // game's normal enumerate/mount/read path finds it -- no guest-code patch
  // needed. See metalslugxx_dlc.h. Applied in OnPostInitLogging.
  bool unlock_leona = true;

  // FpsReplayDryRun = True : enable the 60fps interpolation "replay dry-run"
  //                         diagnostic (step A of the 60fps plan). On the second
  //                         (off-)pass of each frame, clear the just-built render
  //                         list and REBUILD it by replaying the captured
  //                         ms_render_list_add_sprite / ms_render_list_add_entry
  //                         calls verbatim (phase 0 -- identical positions, no
  //                         lerp), then force the scene to render so the rebuilt
  //                         list is flushed. With phase 0 the off-pass frame must
  //                         be pixel-identical to the pass-0 frame; this validates
  //                         the capture/replay machinery before the real
  //                         interpolator is wired in. Compare frames via the F12
  //                         guest-output screenshot path (metalslugxx_screenshot).
  // FpsReplayDryRun = False: stock behaviour (default). The capture/replay hooks
  //                         are inert (they early-out on the flag).
  //
  // See metalslugxx_hooks.cpp and the [[midasm_hook]]s in metalslugxx_config.toml
  // (msxx_fps_*), plus MSXX_60FPS_FUNCTION_TARGETS.md (NEXT STEP, task A).
  bool fps_replay_dryrun = false;

  // FpsInterpolate = True : the real 60fps payoff. Arms the full off-pass path:
  //                        capture/replay, guest buffer-B redirect, sprite pose
  //                        interpolation, page-background interpolation, and the
  //                        matching sub-pixel camera midpoint. Replayed sprites are
  //                        re-emitted at the inter-tick MIDPOINT: the replay
  //                        wrapper reads the native prev/current committed pose
  //                        (pose+0x20/0x24 vs pose+0x18/0x1c, 16.16) and writes the
  //                        phase-blended value (phase = pass/pass_count = 0.5 for
  //                        the 2-pass case) into the current pose fields, calls
  //                        ms_render_list_add_sprite (which bakes the lerped quad,
  //                        incl. the anchor copy, since it reads the same fields),
  //                        then RESTORES the real current pose so the sim is
  //                        unaffected. UI (add_entry) and the cel/frame index are
  //                        snapped, never lerped. A teleport clamp snaps sprites
  //                        whose |cur-prev| exceeds ~48px (scene cuts / pose-commit
  //                        bypassers) to avoid one-frame streaks. Requires the
  //                        dry-run plumbing (validated 2026-06-24, log 175).
  // FpsInterpolate = False: phase 0 (dry-run behaviour if FpsReplayDryRun, else
  //                        inert). Default off.
  bool fps_interpolate = false;

  // [Graphics]
  //
  // GameUpscaleFilter controls the texture filter for the in-game playfield path
  // only: sprite/background draws into the 320x240 scene target, the resolved
  // render-target upscale, and the final playfield quad. Menus and other UI keep
  // the game's own sampler choices.
  //   "default": keep the game's own filtering (stock; the native frame is
  //              point-doubled then bilinear-scaled, as on hardware).
  //   "linear" : force bilinear on the playfield path.
  //   "point"  : force nearest on the playfield path (crisp pixel-art look).
  //
  // Applied through scoped game hooks plus the SDK `game_upscale_filter` cvar
  // for playfield shader workarounds. Stored lowercased; an unrecognized value
  // falls back to "default".
  std::string game_upscale_filter = "point";

  // SampleTexelBias shifts the source sample coordinate of every 2D texture
  // fetch by this many texels. It is a DIAGNOSTIC knob for the in-game upscale
  // "shuffling": the playfield is built by sampling EDRAM-resolved render
  // targets through a multi-pass bilinear upscale, and a half-texel tap
  // misalignment makes that bilinear degenerate toward nearest (scrambling 1-px
  // HUD glyphs). Sweep this (e.g. -0.5..0.5) against a hardware capture to find
  // the phase that un-scrambles it. 0 = stock (default; no change). Currently
  // global -- it also shifts menu/background samples -- so it is for finding the
  // correction, not yet a shippable default. Drives the SDK
  // `game_sample_texel_bias` cvar in OnPostInitLogging.
  double sample_texel_bias = 0.0;
  // True only if the INI explicitly contained [Graphics] SampleTexelBias. When
  // false, OnPostInitLogging leaves the SDK cvar untouched so a command-line
  // `--game_sample_texel_bias=...` (parsed earlier, during cvar::Init) survives
  // instead of being clobbered back to the default.
  bool sample_texel_bias_set = false;

  // LowresTiledBias is the fix for the in-game ~2px up-left offset. It shifts the
  // sample coord by this many texels, but ONLY for the bottom-of-chain low-res
  // tiled render target (the ~320x240 playfield) -- unlike SampleTexelBias (every
  // tiled fetch, which cancels through the upscale chain), this is applied at a
  // single non-cancelling pass so the shift survives the ~4x upscale (-0.5 texel
  // ~= 2px down-right). -0.5 is the confirmed value (verified by eye 2026-06-14);
  // 0 disables. This is only an override of the SDK cvar's own -0.5 default; the
  // value here mirrors it so a freshly-generated INI documents the active setting.
  // Drives the SDK `game_lowres_tiled_bias` cvar in OnPostInitLogging.
  double lowres_tiled_bias = -0.5;
  // True only if the INI explicitly contained [Graphics] LowresTiledBias, so a
  // command-line `--game_lowres_tiled_bias=...` is not clobbered back to default.
  bool lowres_tiled_bias_set = false;

  // BlackBorder = True : fill the area around the 320x240 playfield with solid
  //                      black instead of the game's decorative background.
  // BlackBorder = False: draw the stock background (default).
  //
  // In HD the game scales the 320x240 playfield into the middle of the screen
  // and covers the rest with two stretched fullscreen layers, GameBgTexture and
  // GameBgTextureA (globals 0x82D2EE04 / 0x82D2EE08), drawn by
  // ms_present_draw_backdrop_texture_layers (0x823E70A0) just before the
  // playfield quad. That call is gated on the render core's HD flag
  // ([[0x82D41B40]+0x280], set to 1 for 1280x720 and 0 for 640x480, where the
  // playfield fills the screen and no border exists).
  //
  // Implemented as a [[midasm_hook]] with return_on_true on that function's
  // entry: when True we skip both textured quads and instead draw one opaque
  // black fullscreen quad through the game's own fade-quad helper
  // (ms_present_draw_fullscreen_fade_quad at alpha 255). The border has to be
  // *painted*, not merely skipped -- those quads are what covers the frame, so
  // dropping them without a replacement would leave stale pixels rather than
  // black. See metalslugxx_hooks.cpp (msxx_black_border).
  bool black_border = false;

  // [Keyboard1] / [Keyboard2]
  //
  // Basic keyboard-as-controller support. [Keyboard1] drives player 1's emulated
  // Xbox 360 pad and [Keyboard2] a second player's, both alongside any real
  // controller and both reading the same physical keyboard -- so for local co-op
  // their bindings must not overlap (player 2 defaults to the numpad). Each
  // binding is a key NAME (the same vocabulary the in-game settings use), NOT an
  // SDL keycode: on Windows the game window is a native Win32 window and key
  // presses arrive as Win32 virtual keys, so there is no SDL keycode at the
  // binding layer. Accepted names include the letters A-Z, the digits 0-9,
  // F1-F24, and: Up, Down, Left, Right, Return, Escape, Space, Tab, Backspace,
  // Delete, Insert, Home, End, PageUp, PageDown, Shift, Control, Alt,
  // Numpad0-Numpad9, NumpadEnter, NumpadPlus, NumpadMinus, NumpadStar,
  // NumpadSlash. An unknown name disables that one binding (and is logged).
  // [Keyboard1] drives the SDK `keyboard_mode` / `kb_*` cvars and [Keyboard2] the
  // `keyboard2_mode` / `kb2_*` cvars in OnPostInitLogging.
  //
  // Player 1 default layout (D-pad on the arrows, face buttons on WASD):
  //   Up/Down/Left/Right -> D-pad      W -> Y    A -> X
  //   Return -> Start                  S -> A    D -> B
  //   Backspace -> Back/Select
  bool keyboard_enabled = true;
  std::string kb_dpad_up = "Up";
  std::string kb_dpad_down = "Down";
  std::string kb_dpad_left = "Left";
  std::string kb_dpad_right = "Right";
  std::string kb_button_x = "A";
  std::string kb_button_a = "S";
  std::string kb_button_b = "D";
  std::string kb_button_y = "W";
  std::string kb_lshoulder = "Q";
  std::string kb_rshoulder = "E";
  std::string kb_ltrigger = "1";
  std::string kb_rtrigger = "3";
  std::string kb_start = "Return";
  std::string kb_back = "Backspace";

  // Player 2. Disabled by default; defaults to the numpad so it does not collide
  // with player 1's keys when both share one keyboard.
  //   Numpad8/2/4/6 -> D-pad           Numpad9 -> Y   Numpad7 -> X
  //   NumpadEnter -> Start             Numpad1 -> A   Numpad3 -> B
  //   Numpad0 -> Back/Select
  bool keyboard2_enabled = false;
  std::string kb2_dpad_up = "I";
  std::string kb2_dpad_down = "K";
  std::string kb2_dpad_left = "J";
  std::string kb2_dpad_right = "L";
  std::string kb2_button_x = "F";
  std::string kb2_button_a = "G";
  std::string kb2_button_b = "H";
  std::string kb2_button_y = "T";
  std::string kb2_lshoulder = "R";
  std::string kb2_rshoulder = "Y";
  std::string kb2_ltrigger = "4";
  std::string kb2_rtrigger = "6";
  std::string kb2_start = "P";
  std::string kb2_back = "O";
};

// Loads `metalslugxx.ini` from the executable folder, writing a commented
// default file if none exists. Logs what it resolved. Safe to call once at
// startup (after logging is up). Returns a reference to the process-wide config.
const Config& LoadConfig();

// Returns the config resolved by LoadConfig() (defaults if never loaded).
const Config& config();

// Re-writes `metalslugxx.ini` from the live config, so any keys absent from an
// older INI get populated (and comments refreshed) while keeping the values the
// user already set. Written atomically (temp file + rename). Intended to be
// called once on shutdown. Logs the outcome; never throws.
void SaveConfig();

// Resolved path of the INI file (executable folder / "metalslugxx.ini").
std::filesystem::path ConfigPath();

}  // namespace metalslugxx
