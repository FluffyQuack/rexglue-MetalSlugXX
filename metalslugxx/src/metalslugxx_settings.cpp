// metalslugxx - ReXGlue Recompiled Project
//
// See metalslugxx_settings.h for the contract.

#include "metalslugxx_settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/ui/keybinds.h>

namespace metalslugxx {

namespace {

Config g_config;
std::filesystem::path g_config_path;
bool g_loaded = false;

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view Trim(std::string_view s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// Strips an inline/line comment introduced by `//`, `;` or `#`.
std::string_view StripComment(std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ';' || s[i] == '#') return s.substr(0, i);
    if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') return s.substr(0, i);
  }
  return s;
}

// Lowercased "section.key" -> raw value string, for every pair found.
using KeyMap = std::map<std::string, std::string>;

KeyMap Parse(std::istream& in) {
  KeyMap map;
  std::string section;
  std::string line;
  while (std::getline(in, line)) {
    std::string_view sv = Trim(StripComment(line));
    if (sv.empty()) continue;
    if (sv.front() == '[' && sv.back() == ']') {
      section = ToLower(Trim(sv.substr(1, sv.size() - 2)));
      continue;
    }
    auto eq = sv.find('=');
    if (eq == std::string_view::npos) continue;  // not a key=value line; skip
    std::string key = ToLower(Trim(sv.substr(0, eq)));
    std::string value(Trim(sv.substr(eq + 1)));
    if (key.empty()) continue;
    map[section + "." + key] = std::move(value);
  }
  return map;
}

std::optional<bool> AsBool(const std::string& v) {
  std::string s = ToLower(v);
  if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
  if (s == "false" || s == "0" || s == "no" || s == "off") return false;
  return std::nullopt;
}

// Validates an INI key-name binding against the same vocabulary the in-game
// keybinds use (rex::ui::ParseVirtualKey). An empty value unbinds the control;
// an unrecognized name keeps the existing default and warns.
void ApplyKeyBind(const KeyMap& map, const std::string& full_key, const char* section,
                  const char* label, std::string& target) {
  auto it = map.find(full_key);
  if (it == map.end()) return;
  std::string_view name = Trim(it->second);
  if (name.empty()) {
    target.clear();
  } else if (rex::ui::ParseVirtualKey(name) != rex::ui::VirtualKey::kNone) {
    target = std::string(name);
  } else {
    REXLOG_WARN("config: [{}] {} = '{}' is not a known key name; using default ({})", section, label,
                it->second, target);
  }
}

// --- Serialization (the inverse of Parse) -------------------------------------
// One source of truth for the file we write on first run AND re-write on quit.
// Substituting live `Config` values means a re-save preserves whatever the user
// set while filling in any keys (and refreshing any comments) that were missing.

const char* BoolStr(bool v) { return v ? "True" : "False"; }

// Title-cases the internally-lowercased upscale filter back to its INI spelling.
const char* FilterDisplay(const std::string& v) {
  if (v == "linear") return "Linear";
  if (v == "point") return "Point";
  return "Default";
}

// Compact number formatting (e.g. 0 -> "0", -0.5 -> "-0.5") rather than the
// "0.000000" std::to_string would produce.
std::string NumStr(double v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

// Builds the full commented INI text from the given config. Mirrors the format
// Parse() accepts; kept self-documenting so a fresh install explains itself.
std::string BuildIniText(const Config& c) {
  std::ostringstream os;
  os << "; metalslugxx configuration. Delete this file to regenerate defaults.\n"
        "; Comments may start with // ; or #. Missing keys fall back to defaults.\n"
        "\n"
        "[System]\n"
        "\n"
        "// If Sleep is False, then the game waits until the exact time instead of Sleeping\n"
     << "Sleep = " << BoolStr(c.sleep) << "\n"
     << "\n"
        "// If true, keep all writable data (user data, shader cache, runtime config)\n"
        "// in the working directory so the install stays self-contained / portable.\n"
        "// If false, use the platform's per-user folders instead.\n"
     << "Portable = " << BoolStr(c.portable) << "\n"
     << "\n"
        "[Game]\n"
        "\n"
        "// If true, the game will be in trial mode with only certain content accessible\n"
     << "TrialMode = " << BoolStr(c.trial_mode) << "\n"
     << "\n"
        "// If true, skip the XBLA and SNK Playmore boot logos and go straight to the title screen\n"
     << "SkipLogos = " << BoolStr(c.skip_logos) << "\n"
     << "\n"
        "// If true, hide the idle \"GO NOW\" direction prompts and silence their cue\n"
     << "DisableGoPopups = " << BoolStr(c.disable_go_popups) << "\n"
     << "\n"
        "// If true, unlock the playable character Leona\n"
     << "UnlockLeona = " << BoolStr(c.unlock_leona) << "\n"
     << "\n"
        "// Diagnostic for the 60fps work\n"
        //"// Diagnostic for the 60fps work: replay-dry-run. On the off-pass of each\n"
        //"// frame, rebuild the render list from the captured sprite/entry calls\n"
        //"// (phase 0, no interpolation) and re-render it; the result must match the\n"
        //"// normal frame. Leave False unless validating the 60fps capture/replay.\n"
     << "FpsReplayDryRun = " << BoolStr(c.fps_replay_dryrun) << "\n"
     << "\n"
        "// 60fps interpolation. Leave False for stock.\n"
        //"// The full 60fps interpolation path: on the off-pass, re-emit sprites and\n"
        //"// page backgrounds at the inter-tick midpoint, with the guest buffer-B\n"
        //"// redirect and camera sync enabled automatically. Leave False for stock.\n"
     << "FpsInterpolate = " << BoolStr(c.fps_interpolate) << "\n"
     << "\n"
        "[Graphics]\n"
        "\n"
        "// Texture filter for the in-game 320x240 playfield path:\n"
        "//   Default = the game's own filtering (smoothed, as on hardware)\n"
        "//   Linear  = force smooth bilinear filtering on the playfield\n"
        "//   Point   = force crisp nearest-neighbour on the playfield (also disables\n"
        "//             the game's built-in playfield smoothing upscaler)\n"
     << "GameUpscaleFilter = " << FilterDisplay(c.game_upscale_filter) << "\n"
     << "\n"
        //"// Diagnostic: shift in-game texture sampling by this many texels to tune the\n" /*Keep comments about this minimal as user isn't meant to change this*/
        //"// upscale sample phase (sweep e.g. -0.5..0.5). 0 = stock. Leave at 0 unless\n"
        //"// chasing the in-game \"shuffling\" artifact.\n"
     << "SampleTexelBias = " << NumStr(c.sample_texel_bias) << "\n"
     << "\n"
        //"// Fix for the in-game ~2px up-left offset: shift ONLY the low-res playfield\n" /*Keep comments about this minimal as user isn't meant to change this*/
        //"// sampling by this many texels (survives the ~4x upscale, so -0.5 texel =\n"
        //"// ~2px down-right). -0.5 is the confirmed value that aligns it with the Xbox\n"
        //"// 360 reference. Set 0 to disable (stock).\n"
     << "LowresTiledBias = " << NumStr(c.lowres_tiled_bias) << "\n"
     << "\n"
        "// If true, the border around the game screen is solid black instead of\n"
        "// the game's decorative background\n"
     << "BlackBorder = " << BoolStr(c.black_border) << "\n"
     << "\n"
        "[Keyboard1]\n"
        "\n"
        "// Basic keyboard-as-controller support. [Keyboard1] drives player 1 and\n"
        "// [Keyboard2] (below) a second player; both read the same physical\n"
        "// keyboard, so for co-op their keys must not overlap. Accepted names:\n"
        "// A-Z, 0-9, F1-F24, and Up, Down, Left, Right, Return, Escape, Space, Tab,\n"
        "// Backspace, Delete, Insert, Home, End, PageUp, PageDown, Shift, Control,\n"
        "// Alt, Numpad0-9, NumpadEnter, NumpadPlus, NumpadMinus, NumpadStar,\n"
        "// NumpadSlash. Leave a value blank to unbind that control.\n"
        "\n"
        "// If false, ignore keyboard 1 for gameplay input.\n"
     << "Enabled = " << BoolStr(c.keyboard_enabled) << "\n"
     << "\n"
        "// D-pad (movement).\n"
     << "DpadUp = " << c.kb_dpad_up << "\n"
     << "DpadDown = " << c.kb_dpad_down << "\n"
     << "DpadLeft = " << c.kb_dpad_left << "\n"
     << "DpadRight = " << c.kb_dpad_right << "\n"
     << "\n"
        "// Face buttons.\n"
     << "ButtonX = " << c.kb_button_x << "\n"
     << "ButtonA = " << c.kb_button_a << "\n"
     << "ButtonB = " << c.kb_button_b << "\n"
     << "ButtonY = " << c.kb_button_y << "\n"
     << "\n"
        "// Shoulder buttons (LB/RB) and triggers (LT/RT).\n"
     << "LeftShoulder = " << c.kb_lshoulder << "\n"
     << "RightShoulder = " << c.kb_rshoulder << "\n"
     << "LeftTrigger = " << c.kb_ltrigger << "\n"
     << "RightTrigger = " << c.kb_rtrigger << "\n"
     << "\n"
        "// Start / Back (Select).\n"
     << "Start = " << c.kb_start << "\n"
     << "Back = " << c.kb_back << "\n"
     << "\n"
        "[Keyboard2]\n"
     << "Enabled = " << BoolStr(c.keyboard2_enabled) << "\n"
     << "\n"
        "// D-pad (movement).\n"
     << "DpadUp = " << c.kb2_dpad_up << "\n"
     << "DpadDown = " << c.kb2_dpad_down << "\n"
     << "DpadLeft = " << c.kb2_dpad_left << "\n"
     << "DpadRight = " << c.kb2_dpad_right << "\n"
     << "\n"
        "// Face buttons.\n"
     << "ButtonX = " << c.kb2_button_x << "\n"
     << "ButtonA = " << c.kb2_button_a << "\n"
     << "ButtonB = " << c.kb2_button_b << "\n"
     << "ButtonY = " << c.kb2_button_y << "\n"
     << "\n"
        "// Shoulder buttons (LB/RB) and triggers (LT/RT).\n"
     << "LeftShoulder = " << c.kb2_lshoulder << "\n"
     << "RightShoulder = " << c.kb2_rshoulder << "\n"
     << "LeftTrigger = " << c.kb2_ltrigger << "\n"
     << "RightTrigger = " << c.kb2_rtrigger << "\n"
     << "\n"
        "// Start / Back (Select).\n"
     << "Start = " << c.kb2_start << "\n"
     << "Back = " << c.kb2_back << "\n";
  return os.str();
}

}  // namespace

std::filesystem::path ConfigPath() {
  if (g_config_path.empty()) {
    // The INI lives in the working directory (`./`), not the executable folder:
    // it is the bootstrap file carrying [System] Portable, which then decides
    // where the rest of the writable data goes. current_path() is absolute, so
    // the resolved path is stable even if the CWD changes later.
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) cwd = rex::filesystem::GetExecutableFolder();  // unusual; fall back
    g_config_path = cwd / "metalslugxx.ini";
  }
  return g_config_path;
}

const Config& config() { return g_config; }

const Config& LoadConfig() {
  if (g_loaded) return g_config;
  g_loaded = true;

  const std::filesystem::path path = ConfigPath();

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // First run: write a commented default and keep the compiled-in defaults.
    std::ofstream out(path, std::ios::binary);
    if (out) {
      out << BuildIniText(g_config);
      REXLOG_INFO("config: no INI found, wrote defaults to {}", path.string());
    } else {
      REXLOG_WARN("config: no INI found and could not write {} (using defaults)", path.string());
    }
    return g_config;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    REXLOG_WARN("config: could not open {} (using defaults)", path.string());
    return g_config;
  }

  KeyMap map = Parse(in);

  if (auto it = map.find("system.sleep"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.sleep = *b;
    } else {
      REXLOG_WARN("config: [System] Sleep = '{}' is not a bool; using default ({})", it->second,
                  g_config.sleep);
    }
  }

  if (auto it = map.find("system.portable"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.portable = *b;
    } else {
      REXLOG_WARN("config: [System] Portable = '{}' is not a bool; using default ({})", it->second,
                  g_config.portable);
    }
  }

  if (auto it = map.find("game.trialmode"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.trial_mode = *b;
    } else {
      REXLOG_WARN("config: [Game] TrialMode = '{}' is not a bool; using default ({})", it->second,
                  g_config.trial_mode);
    }
  }

  if (auto it = map.find("game.skiplogos"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.skip_logos = *b;
    } else {
      REXLOG_WARN("config: [Game] SkipLogos = '{}' is not a bool; using default ({})", it->second,
                  g_config.skip_logos);
    }
  }

  if (auto it = map.find("game.disablegopopups"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.disable_go_popups = *b;
    } else {
      REXLOG_WARN("config: [Game] DisableGoPopups = '{}' is not a bool; using default ({})",
                  it->second, g_config.disable_go_popups);
    }
  }

  if (auto it = map.find("game.unlockleona"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.unlock_leona = *b;
    } else {
      REXLOG_WARN("config: [Game] UnlockLeona = '{}' is not a bool; using default ({})", it->second,
                  g_config.unlock_leona);
    }
  }

  if (auto it = map.find("game.fpsreplaydryrun"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.fps_replay_dryrun = *b;
    } else {
      REXLOG_WARN("config: [Game] FpsReplayDryRun = '{}' is not a bool; using default ({})",
                  it->second, g_config.fps_replay_dryrun);
    }
  }

  if (auto it = map.find("game.fpsinterpolate"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.fps_interpolate = *b;
    } else {
      REXLOG_WARN("config: [Game] FpsInterpolate = '{}' is not a bool; using default ({})",
                  it->second, g_config.fps_interpolate);
    }
  }

  if (auto it = map.find("graphics.gameupscalefilter"); it != map.end()) {
    std::string v = ToLower(it->second);
    if (v == "default" || v == "linear" || v == "point") {
      g_config.game_upscale_filter = v;
    } else {
      REXLOG_WARN("config: [Graphics] GameUpscaleFilter = '{}' is not one of "
                  "Default/Linear/Point; using default ({})",
                  it->second, g_config.game_upscale_filter);
    }
  }

  if (auto it = map.find("graphics.sampletexelbias"); it != map.end()) {
    try {
      g_config.sample_texel_bias = std::stod(it->second);
      g_config.sample_texel_bias_set = true;
    } catch (...) {
      REXLOG_WARN("config: [Graphics] SampleTexelBias = '{}' is not a number; using default ({})",
                  it->second, g_config.sample_texel_bias);
    }
  }

  if (auto it = map.find("graphics.lowrestiledbias"); it != map.end()) {
    try {
      g_config.lowres_tiled_bias = std::stod(it->second);
      g_config.lowres_tiled_bias_set = true;
    } catch (...) {
      REXLOG_WARN("config: [Graphics] LowresTiledBias = '{}' is not a number; using default ({})",
                  it->second, g_config.lowres_tiled_bias);
    }
  }

  if (auto it = map.find("graphics.blackborder"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.black_border = *b;
    } else {
      REXLOG_WARN("config: [Graphics] BlackBorder = '{}' is not a bool; using default ({})",
                  it->second, g_config.black_border);
    }
  }

  if (auto it = map.find("keyboard1.enabled"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.keyboard_enabled = *b;
    } else {
      REXLOG_WARN("config: [Keyboard1] Enabled = '{}' is not a bool; using default ({})", it->second,
                  g_config.keyboard_enabled);
    }
  }
  ApplyKeyBind(map, "keyboard1.dpadup", "Keyboard1", "DpadUp", g_config.kb_dpad_up);
  ApplyKeyBind(map, "keyboard1.dpaddown", "Keyboard1", "DpadDown", g_config.kb_dpad_down);
  ApplyKeyBind(map, "keyboard1.dpadleft", "Keyboard1", "DpadLeft", g_config.kb_dpad_left);
  ApplyKeyBind(map, "keyboard1.dpadright", "Keyboard1", "DpadRight", g_config.kb_dpad_right);
  ApplyKeyBind(map, "keyboard1.buttonx", "Keyboard1", "ButtonX", g_config.kb_button_x);
  ApplyKeyBind(map, "keyboard1.buttona", "Keyboard1", "ButtonA", g_config.kb_button_a);
  ApplyKeyBind(map, "keyboard1.buttonb", "Keyboard1", "ButtonB", g_config.kb_button_b);
  ApplyKeyBind(map, "keyboard1.buttony", "Keyboard1", "ButtonY", g_config.kb_button_y);
  ApplyKeyBind(map, "keyboard1.leftshoulder", "Keyboard1", "LeftShoulder", g_config.kb_lshoulder);
  ApplyKeyBind(map, "keyboard1.rightshoulder", "Keyboard1", "RightShoulder", g_config.kb_rshoulder);
  ApplyKeyBind(map, "keyboard1.lefttrigger", "Keyboard1", "LeftTrigger", g_config.kb_ltrigger);
  ApplyKeyBind(map, "keyboard1.righttrigger", "Keyboard1", "RightTrigger", g_config.kb_rtrigger);
  ApplyKeyBind(map, "keyboard1.start", "Keyboard1", "Start", g_config.kb_start);
  ApplyKeyBind(map, "keyboard1.back", "Keyboard1", "Back", g_config.kb_back);

  if (auto it = map.find("keyboard2.enabled"); it != map.end()) {
    if (auto b = AsBool(it->second)) {
      g_config.keyboard2_enabled = *b;
    } else {
      REXLOG_WARN("config: [Keyboard2] Enabled = '{}' is not a bool; using default ({})", it->second,
                  g_config.keyboard2_enabled);
    }
  }
  ApplyKeyBind(map, "keyboard2.dpadup", "Keyboard2", "DpadUp", g_config.kb2_dpad_up);
  ApplyKeyBind(map, "keyboard2.dpaddown", "Keyboard2", "DpadDown", g_config.kb2_dpad_down);
  ApplyKeyBind(map, "keyboard2.dpadleft", "Keyboard2", "DpadLeft", g_config.kb2_dpad_left);
  ApplyKeyBind(map, "keyboard2.dpadright", "Keyboard2", "DpadRight", g_config.kb2_dpad_right);
  ApplyKeyBind(map, "keyboard2.buttonx", "Keyboard2", "ButtonX", g_config.kb2_button_x);
  ApplyKeyBind(map, "keyboard2.buttona", "Keyboard2", "ButtonA", g_config.kb2_button_a);
  ApplyKeyBind(map, "keyboard2.buttonb", "Keyboard2", "ButtonB", g_config.kb2_button_b);
  ApplyKeyBind(map, "keyboard2.buttony", "Keyboard2", "ButtonY", g_config.kb2_button_y);
  ApplyKeyBind(map, "keyboard2.leftshoulder", "Keyboard2", "LeftShoulder", g_config.kb2_lshoulder);
  ApplyKeyBind(map, "keyboard2.rightshoulder", "Keyboard2", "RightShoulder", g_config.kb2_rshoulder);
  ApplyKeyBind(map, "keyboard2.lefttrigger", "Keyboard2", "LeftTrigger", g_config.kb2_ltrigger);
  ApplyKeyBind(map, "keyboard2.righttrigger", "Keyboard2", "RightTrigger", g_config.kb2_rtrigger);
  ApplyKeyBind(map, "keyboard2.start", "Keyboard2", "Start", g_config.kb2_start);
  ApplyKeyBind(map, "keyboard2.back", "Keyboard2", "Back", g_config.kb2_back);

  REXLOG_INFO("config: loaded {} -> [System] Sleep = {}, [System] Portable = {}, "
              "[Game] TrialMode = {}, [Game] SkipLogos = {}, [Game] DisableGoPopups = {}, "
              "[Game] UnlockLeona = {}, "
              "[Graphics] GameUpscaleFilter = {}, [Graphics] SampleTexelBias = {}, "
              "[Graphics] LowresTiledBias = {}, [Graphics] BlackBorder = {}",
              path.string(), g_config.sleep ? "True" : "False",
              g_config.portable ? "True" : "False",
              g_config.trial_mode ? "True" : "False", g_config.skip_logos ? "True" : "False",
              g_config.disable_go_popups ? "True" : "False",
              g_config.unlock_leona ? "True" : "False", g_config.game_upscale_filter,
              g_config.sample_texel_bias, g_config.lowres_tiled_bias,
              g_config.black_border ? "True" : "False");
  return g_config;
}

void SaveConfig() {
  const std::filesystem::path path = ConfigPath();

  // Re-write the file from the live config so any keys missing from an older INI
  // get populated (and comments refreshed) while the user's values are kept.
  // Written through a temp file + rename so a crash mid-write can't truncate a
  // good INI to nothing.
  std::filesystem::path tmp = path;
  tmp += ".tmp";

  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXLOG_WARN("config: could not write {} (config not re-saved)", tmp.string());
      return;
    }
    out << BuildIniText(g_config);
    if (!out) {
      REXLOG_WARN("config: error writing {} (config not re-saved)", tmp.string());
      return;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    REXLOG_WARN("config: could not replace {} ({}); config not re-saved", path.string(),
                ec.message());
    std::filesystem::remove(tmp, ec);
    return;
  }

  REXLOG_INFO("config: re-saved {} (populated any missing settings)", path.string());
}

}  // namespace metalslugxx
