#include "runtime/bindings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "common/ini.h"
#include "common/settings_file.h"
#include "runtime/game/key_assign.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/input.h"
#include "runtime/log.h"

namespace rt::bindings {
namespace {

// cellPad digital words.
constexpr u16 SELECT = 0x1, L3 = 0x2, R3 = 0x4, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80;
constexpr u16 L2 = 0x1, R2 = 0x2, L1 = 0x4, R1 = 0x8, TRIANGLE = 0x10, CIRCLE = 0x20, CROSS = 0x40, SQUARE = 0x80;

enum class Kind { D1, D2, Axis, Macro };
// Sequences of the key-only controls (macros): quick turns, their two-stage variants, the
// two-stage quick boost, the quick boost held half pressed, assault armor.
enum Macro { kQuickTurnLeft, kQuickTurnRight, kTwoStageTurnLeft, kTwoStageTurnRight, kTwoStageBoost, kHalfBoost, kAssaultArmor };
struct Control {
  const char* name;
  Kind kind;
  u16 bit;
  int axis;       // 0 lx, 1 ly, 2 rx, 3 ry
  bool positive;
  int slot;       // the action it stands for in missions (rt::game::KeySlot, the SIMPLE / REGULAR-A
                  // meaning of the button; -1: the button itself); Kind::Macro: the Macro
};
using namespace rt::game;
const Control kControls[] = {
    {"lstick_up", Kind::Axis, 0, 1, false, -1},     {"lstick_down", Kind::Axis, 0, 1, true, -1},
    {"lstick_left", Kind::Axis, 0, 0, false, -1},   {"lstick_right", Kind::Axis, 0, 0, true, -1},
    {"rstick_up", Kind::Axis, 0, 3, false, -1},     {"rstick_down", Kind::Axis, 0, 3, true, -1},
    {"rstick_left", Kind::Axis, 0, 2, false, -1},   {"rstick_right", Kind::Axis, 0, 2, true, -1},
    {"cross", Kind::D2, CROSS, 0, false, kSlotRightWeapon},       {"square", Kind::D2, SQUARE, 0, false, kSlotLeftWeapon},
    {"r1", Kind::D2, R1, 0, false, kSlotShoulder},                {"circle", Kind::D2, CIRCLE, 0, false, kSlotSwitchRight},
    {"triangle", Kind::D2, TRIANGLE, 0, false, kSlotSwitchLeft},  {"l2", Kind::D2, L2, 0, false, kSlotBoost},
    {"r2", Kind::D2, R2, 0, false, kSlotQuickBoost},              {"l1", Kind::D2, L1, 0, false, kSlotOveredBoost},
    {"r3", Kind::D1, R3, 0, false, kSlotResetView},               {"l3", Kind::D1, L3, 0, false, kSlotLock},
    {"start", Kind::D1, START, 0, false, kSlotPause},             {"select", Kind::D1, SELECT, 0, false, -1},
    {"up", Kind::D1, UP, 0, false, -1},             {"down", Kind::D1, DOWN, 0, false, -1},
    {"left", Kind::D1, LEFT, 0, false, -1},         {"right", Kind::D1, RIGHT, 0, false, -1},
    {"quick_turn_left", Kind::Macro, 0, 0, false, kQuickTurnLeft},
    {"quick_turn_right", Kind::Macro, 0, 0, false, kQuickTurnRight},
    {"two_stage_turn_left", Kind::Macro, 0, 0, false, kTwoStageTurnLeft},
    {"two_stage_turn_right", Kind::Macro, 0, 0, false, kTwoStageTurnRight},
    {"two_stage_boost", Kind::Macro, 0, 0, false, kTwoStageBoost},
    {"half_boost", Kind::Macro, 0, 0, false, kHalfBoost},
    {"assault_armor", Kind::Macro, 0, 0, false, kAssaultArmor},
};
constexpr size_t kControlCount = sizeof(kControls) / sizeof(kControls[0]);

// What an input code of the game's assignment (key_assign.h) presses on the pad.
struct PadElem {
  bool valid = false;
  Kind kind = Kind::D1;
  u16 bit = 0;
  int axis = 0;
  bool positive = false;
};
PadElem elem_of_code(u8 code) {
  static const PadElem kSticks[8] = {
      {true, Kind::Axis, 0, 1, false}, {true, Kind::Axis, 0, 1, true}, {true, Kind::Axis, 0, 0, true}, {true, Kind::Axis, 0, 0, false},
      {true, Kind::Axis, 0, 3, false}, {true, Kind::Axis, 0, 3, true}, {true, Kind::Axis, 0, 2, true}, {true, Kind::Axis, 0, 2, false},
  };
  if (code < 8) return kSticks[code];
  switch (code) {
    case 0x08: return {true, Kind::D2, CIRCLE};
    case 0x09: return {true, Kind::D2, TRIANGLE};
    case 0x0a: return {true, Kind::D2, SQUARE};
    case 0x0b: return {true, Kind::D2, CROSS};
    case 0x0c: return {true, Kind::D2, L1};
    case 0x0d: return {true, Kind::D2, L2};
    case 0x0e: return {true, Kind::D2, R1};
    case 0x0f: return {true, Kind::D2, R2};
    case 0x10: return {true, Kind::D1, L3};
    case 0x11: return {true, Kind::D1, R3};
    case 0x12: return {true, Kind::D1, SELECT};
    case 0x13: return {true, Kind::D1, START};
    // The D-pad in the stick codes' order (the operator's button bits 0x1, 0x2, 0x8, 0x4: the
    // Xbox 360 layout of up, down, right, left; GAME_NOTES "Input mechanics measured").
    case 0x14: return {true, Kind::D1, UP};
    case 0x15: return {true, Kind::D1, DOWN};
    case 0x16: return {true, Kind::D1, RIGHT};
    case 0x17: return {true, Kind::D1, LEFT};
    default: return {};  // none (0xfe; SIMPLE's strafe slots hold 0xff)
  }
}
// The REGULAR-A assignment: the meaning of the buttons when the game's is not known yet.
constexpr u8 kDefaultCodes[kSlotCount] = {0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x04, 0x05, 0x11, 0x10,
                                          0x0d, 0x0c, 0x0f, 0x0b, 0x0a, 0x0e, 0x08, 0x09, 0x13};

struct KeyName {
  const char* name;
  u32 code;
};
const KeyName kKeys[] = {
    {"Space", VK_SPACE},        {"Enter", VK_RETURN},       {"Escape", VK_ESCAPE},      {"Tab", VK_TAB},
    {"Backspace", VK_BACK},     {"Shift", VK_SHIFT},        {"Ctrl", VK_CONTROL},       {"Alt", VK_MENU},
    {"Up", VK_UP},              {"Down", VK_DOWN},          {"Left", VK_LEFT},          {"Right", VK_RIGHT},
    {"Home", VK_HOME},          {"End", VK_END},            {"PageUp", VK_PRIOR},       {"PageDown", VK_NEXT},
    {"Insert", VK_INSERT},      {"Delete", VK_DELETE},      {"CapsLock", VK_CAPITAL},   {"Semicolon", VK_OEM_1},
    {"Equals", VK_OEM_PLUS},    {"Comma", VK_OEM_COMMA},    {"Minus", VK_OEM_MINUS},    {"Period", VK_OEM_PERIOD},
    {"Slash", VK_OEM_2},        {"Backquote", VK_OEM_3},    {"LBracket", VK_OEM_4},     {"Backslash", VK_OEM_5},
    {"RBracket", VK_OEM_6},     {"Quote", VK_OEM_7},        {"NumAdd", VK_ADD},         {"NumSubtract", VK_SUBTRACT},
    {"NumMultiply", VK_MULTIPLY}, {"NumDivide", VK_DIVIDE}, {"NumDecimal", VK_DECIMAL}, {"Mouse1", kMouse + 0},
    {"Mouse2", kMouse + 1},     {"Mouse3", kMouse + 2},     {"Mouse4", kMouse + 3},     {"Mouse5", kMouse + 4},
    {"WheelUp", kWheelUp},      {"WheelDown", kWheelDown},
};

std::mutex g_mu;  // bindings change on the window thread (PC SETTINGS) while pads are read
std::vector<u32> g_bind[kControlCount];
std::vector<u32> g_menu[kControlCount];  // [menu]: in the game's menus first
std::atomic<double> g_sensitivity_x{1.0}, g_sensitivity_y{1.0};
std::atomic<bool> g_invert_x{false}, g_invert_y{false};
std::atomic<MouseMode> g_mouse_mode{MouseMode::Stick};
std::chrono::steady_clock::time_point g_wheel_up_until, g_wheel_down_until;

// Key sequences (sample(), on the thread reading the pad). Times in seconds from the first data the
// game got with the key down. Quick turn: the quick boost pressed with the right stick sideways
// and the left stick at rest (with it pushed the boost goes that way instead), the stick held as
// long as the key (the turn is the boost's ~60 degrees plus the stick's own; run 20261024-174951).
// Two-stage: the quick boost half pressed for kStage, then fully: the boost's second stage (113
// units instead of ~50 alone; a quick turn ~118 degrees instead of ~75) needs the half press
// held between about 0.37 and 0.40 s, as measured at 60 fps (GAME_NOTES "Input mechanics
// measured"); switching at the first data at least kStage in lands on 0.367 s at 30, 60 and 120
// frames per second.
constexpr double kQbPress = 0.1, kStage = 0.36, kStickLead = 0.035, kMinHold = 0.15;
constexpr int kHalfPressure = 180;  // QB held half: below its firing point (208-230), in the charge range (160-208)
struct MacroState {
  bool down = false;  // a bound input is down (as of the last delivered sample)
  double t0 = -1;     // the sequence's start (-1: idle)
};
MacroState g_macro[7];
double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(u8(s.back()))) s.pop_back();
  size_t b = 0;
  while (b < s.size() && std::isspace(u8(s[b]))) ++b;
  return s.substr(b);
}

bool equal_ci(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(u8(a[i])) != std::tolower(u8(b[i]))) return false;
  return true;
}

std::vector<u32> parse_list(const std::string& value, const char* control) {
  std::vector<u32> out;
  std::istringstream items(value);
  for (std::string item; std::getline(items, item, ',');) {
    item = trim(item);
    if (item.empty()) continue;
    u32 code = key_code(item);
    if (!code) LOG_W("input", "settings [keyboard] %s: unknown input '%s'", control, item.c_str());
    else if (!bindable(code)) LOG_W("input", "settings [keyboard] %s: '%s' is reserved by the window", control, item.c_str());
    else if (std::find(out.begin(), out.end(), code) == out.end()) out.push_back(code);
  }
  return out;
}

void parse_section(const acfa::Ini& ini, const char* section, std::vector<u32>* bind) {
  std::lock_guard lk(g_mu);
  for (size_t c = 0; c < kControlCount; ++c) bind[c].clear();
  for (const auto& [name, value] : ini.entries(section)) {
    size_t c = 0;
    while (c < kControlCount && !equal_ci(name, kControls[c].name)) ++c;
    if (c == kControlCount) {
      LOG_W("input", "settings [%s]: unknown pad control '%s'", section, name.c_str());
      continue;
    }
    bind[c] = parse_list(value, kControls[c].name);
  }
}
void parse_keyboard(const acfa::Ini& ini) {
  parse_section(ini, "keyboard", g_bind);
  parse_section(ini, "menu", g_menu);
}

}  // namespace

size_t control_count() { return kControlCount; }
const char* control_name(size_t c) { return c < kControlCount ? kControls[c].name : ""; }

u32 key_code(const std::string& n) {
  for (const KeyName& k : kKeys)
    if (equal_ci(n, k.name)) return k.code;
  if (n.size() == 1 && std::isalnum(u8(n[0]))) return u32(std::toupper(u8(n[0])));  // 'A'-'Z', '0'-'9'
  if (n.size() >= 2 && (n[0] == 'F' || n[0] == 'f') && std::isdigit(u8(n[1]))) {
    int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return u32(VK_F1 + f - 1);
  }
  if (n.size() == 4 && equal_ci(n.substr(0, 3), "Num") && std::isdigit(u8(n[3]))) return u32(VK_NUMPAD0 + (n[3] - '0'));
  return 0;
}

std::string input_name(u32 code) {
  for (const KeyName& k : kKeys)
    if (k.code == code) return k.name;
  if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, char(code));
  if (code >= VK_F1 && code <= VK_F24) return "F" + std::to_string(code - VK_F1 + 1);
  if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) return "Num" + std::to_string(code - VK_NUMPAD0);
  return "";
}

bool bindable(u32 code) { return code != VK_F1 && code != VK_F11 && !input_name(code).empty(); }

void load(const acfa::Ini& ini) {
  parse_keyboard(ini);
  MouseSettings m;
  auto sens = [&](const char* key, double* out) {
    if (auto v = ini.get("mouse", key)) {
      double s = std::atof(v->c_str());
      if (s > 0 && s <= 100) *out = s;
      else LOG_W("input", "settings [mouse] %s '%s' ignored (use a number above 0)", key, v->c_str());
    }
  };
  sens("sensitivity", &m.sensitivity_x);  // the earlier single setting: both axes
  m.sensitivity_y = m.sensitivity_x;
  sens("sensitivity_x", &m.sensitivity_x);
  sens("sensitivity_y", &m.sensitivity_y);
  if (auto v = ini.get("mouse", "invert_x")) m.invert_x = equal_ci(*v, "true") || *v == "1";
  if (auto v = ini.get("mouse", "invert_y")) m.invert_y = equal_ci(*v, "true") || *v == "1";
  if (auto v = ini.get("mouse", "mode")) {
    if (equal_ci(trim(*v), "camera")) m.mode = MouseMode::Camera;
    else if (!equal_ci(trim(*v), "stick")) LOG_W("input", "settings [mouse] mode '%s' ignored (use stick or camera)", v->c_str());
  }
  set_mouse(m);
  size_t bound = 0;
  {
    std::lock_guard lk(g_mu);
    for (const auto& b : g_bind) bound += !b.empty();
  }
  LOG_I("input", "keyboard/mouse: %zu of %zu controls bound; mouse as the %s, sensitivity %.3f x, %.3f y%s%s", bound, kControlCount,
        m.mode == MouseMode::Camera ? "camera" : "right stick", m.sensitivity_x, m.sensitivity_y, m.invert_x ? ", X inverted" : "",
        m.invert_y ? ", Y inverted" : "");
}

const char* mouse_mode_name(MouseMode m) { return m == MouseMode::Camera ? "camera" : "stick"; }

void store(acfa::Ini& ini) {
  std::lock_guard lk(g_mu);
  for (size_t c = 0; c < kControlCount; ++c) {
    std::string v;
    for (u32 code : g_bind[c]) v += (v.empty() ? "" : ", ") + input_name(code);
    ini.set("keyboard", kControls[c].name, v);
    std::string m;
    for (u32 code : g_menu[c]) m += (m.empty() ? "" : ", ") + input_name(code);
    if (!m.empty() || ini.get("menu", kControls[c].name)) ini.set("menu", kControls[c].name, m);
  }
  ini.remove("mouse", "sensitivity");  // the earlier single setting
  ini.set("mouse", "sensitivity_x", acfa::strprintf("%.2f", g_sensitivity_x.load()));
  ini.set("mouse", "sensitivity_y", acfa::strprintf("%.2f", g_sensitivity_y.load()));
  ini.set("mouse", "invert_x", g_invert_x.load() ? "true" : "false");
  ini.set("mouse", "invert_y", g_invert_y.load() ? "true" : "false");
  ini.set("mouse", "mode", mouse_mode_name(g_mouse_mode.load()));
}

std::vector<u32> get(size_t c, Layer l) {
  std::lock_guard lk(g_mu);
  return c < kControlCount ? (l == Layer::Menu ? g_menu : g_bind)[c] : std::vector<u32>{};
}

void set(size_t c, const std::vector<u32>& codes, Layer l) {
  if (c >= kControlCount) return;
  std::lock_guard lk(g_mu);
  (l == Layer::Menu ? g_menu : g_bind)[c] = codes;
}

void reset_defaults(Layer l) {
  if (l == Layer::Menu)
    parse_section(acfa::Ini::parse(std::string("[menu]\n") + acfa::settings::default_menu_text()), "menu", g_menu);
  else
    parse_section(acfa::Ini::parse(std::string("[keyboard]\n") + acfa::settings::default_keyboard_text()), "keyboard", g_bind);
}

PadOut sample(bool deliver) {
  PadOut o;
  if (!input_has_focus()) return o;
  auto now = std::chrono::steady_clock::now();
  if (s32 w = input_mouse_wheel_take()) {  // one notch: a press of 0.1 s
    (w > 0 ? g_wheel_up_until : g_wheel_down_until) = now + std::chrono::milliseconds(100);
  }
  // Presses since the game's last data count as down once, however short (a tap between two reads).
  std::array<bool, 256> tap{};
  u32 mouse_tap = 0;
  if (deliver) input_taps_take(&tap, &mouse_tap);
  auto down = [&](u32 code) {
    if (code < kMouse) return input_key_down(code) || tap[code];
    if (code < kWheelUp) return input_mouse_button(int(code - kMouse)) || ((mouse_tap >> (code - kMouse)) & 1) != 0;
    return now < (code == kWheelUp ? g_wheel_up_until : g_wheel_down_until);
  };
  bool neg[4] = {}, pos[4] = {};
  std::lock_guard lk(g_mu);
  // In the game's menus an input listed in [menu] drives its menu control only; on other screens
  // than gameplay (title, movies) the menu's confirm and back keys press Start as well. An input
  // keeps the meaning it had when it went down until it is released: Escape closing the pause menu
  // (Circle there) does not become Start once gameplay resumes under the still pressed key.
  const game::InputContext where = game::input_context();
  static std::vector<std::pair<u32, game::InputContext>> held;  // inputs down, with their context at the press
  auto context_of = [&](u32 code) {
    for (auto& [c, at] : held)
      if (c == code) return at;
    held.push_back({code, where});
    if (where == game::InputContext::Menu && code < kMouse) game::note_menu_key();  // keys, not the mouse's buttons
    return where;
  };
  held.erase(std::remove_if(held.begin(), held.end(), [&](const auto& h) { return !down(h.first); }), held.end());
  auto in_menu_layer = [&](u32 code) {
    for (const auto& m : g_menu)
      if (std::find(m.begin(), m.end(), code) != m.end()) return true;
    return false;
  };
  // Whether input `code` (down) drives control c in the context it was pressed in.
  auto drives = [&](u32 code, size_t c) {
    game::InputContext at = context_of(code);
    bool game = std::find(g_bind[c].begin(), g_bind[c].end(), code) != g_bind[c].end();
    if (at != game::InputContext::Menu) return game;
    return std::find(g_menu[c].begin(), g_menu[c].end(), code) != g_menu[c].end() || (game && !in_menu_layer(code));
  };
  // In missions a button control stands for its action (Control::slot) and presses the button the
  // game's assignment gives that action (SIMPLE / REGULAR-A / REGULAR-B / a custom KEY ASSIGN);
  // elsewhere the buttons themselves.
  u8 codes[kSlotCount];
  if (!game::key_assignment(codes)) std::memcpy(codes, kDefaultCodes, sizeof codes);
  auto apply = [&](const PadElem& e) {
    if (!e.valid) return;
    if (e.kind == Kind::D1) o.d1 |= e.bit;
    else if (e.kind == Kind::D2) o.d2 |= e.bit;
    else (e.positive ? pos : neg)[e.axis] = true;
  };
  auto action = [&](u32 slot) { return elem_of_code(codes[slot]); };
  bool r2_full = false, l2_full = false;
  for (size_t c = 0; c < kControlCount; ++c) {
    const Control& k = kControls[c];
    if (k.kind == Kind::Macro) continue;
    bool on = false, mission = false;
    auto check = [&](u32 code) {
      if (!down(code) || !drives(code, c)) return;
      on = true;
      mission = mission || context_of(code) == game::InputContext::Gameplay;
    };
    for (u32 code : g_bind[c]) check(code);
    for (u32 code : g_menu[c]) check(code);
    // Screens without a menu (title, movies): the menu's confirm and back keys press Start.
    if (k.kind == Kind::D2 && (k.bit == CROSS || k.bit == CIRCLE))
      for (u32 code : g_menu[c])
        if (down(code) && context_of(code) == game::InputContext::Screen) o.d1 |= START;
    if (!on) continue;
    PadElem e = mission && k.slot >= 0 ? action(u32(k.slot)) : PadElem{true, k.kind, k.bit, k.axis, k.positive};
    apply(e);
    r2_full = r2_full || (e.valid && e.kind == Kind::D2 && e.bit == R2);
    l2_full = l2_full || (e.valid && e.kind == Kind::D2 && e.bit == L2);
  }
  // Key sequences, in missions only.
  const double t = now_s();
  int boost_pressure = -1;  // the quick boost's pressure a sequence asks for (-1: none)
  bool boost_press = false, lstick_rest = false;
  for (size_t c = 0; c < kControlCount; ++c) {
    const Control& k = kControls[c];
    if (k.kind != Kind::Macro) continue;
    MacroState& m = g_macro[k.slot];
    bool held = false;
    for (u32 code : g_bind[c])
      if (down(code) && context_of(code) == game::InputContext::Gameplay) held = true;
    if (deliver) {
      if (held && !m.down && m.t0 < 0) m.t0 = t;  // a new press starts the sequence
      m.down = held;
    }
    if (m.t0 < 0) continue;
    const double e = t - m.t0;
    const bool turn = k.slot <= kTwoStageTurnRight;
    const bool two = k.slot == kTwoStageTurnLeft || k.slot == kTwoStageTurnRight || k.slot == kTwoStageBoost;
    const bool right = k.slot == kQuickTurnRight || k.slot == kTwoStageTurnRight;
    const double fire = two ? kStage : 0;  // when the quick boost fires
    bool done;
    switch (k.slot) {
      case kHalfBoost:
        if (held) boost_pressure = std::max(boost_pressure, kHalfPressure);
        done = !held;
        break;
      case kAssaultArmor:
        if (held || e < kMinHold) {
          apply(action(kSlotSwitchRight));
          apply(action(kSlotSwitchLeft));
          apply(action(kSlotOveredBoost));
        }
        done = !held && e >= kMinHold;
        break;
      default:
        if (e < fire) boost_pressure = std::max(boost_pressure, kHalfPressure);
        else if (e < fire + kQbPress) boost_press = true;
        if (turn && e >= fire - kStickLead && (held || e < fire + kMinHold)) apply(action(right ? kSlotRotateRight : kSlotRotateLeft));
        if (turn && e >= fire - kStickLead && e < fire + kQbPress) lstick_rest = true;
        done = e >= fire + (turn ? kMinHold : kQbPress) && !held;
        break;
    }
    if (done && deliver) m.t0 = -1;
  }
  // The quick boost's button: its pressure where it has one (L2 / R2), else a press at full.
  const PadElem qb = action(kSlotQuickBoost);
  if (boost_press) apply(qb);
  const bool qb_r2 = qb.valid && qb.kind == Kind::D2 && qb.bit == R2, qb_l2 = qb.valid && qb.kind == Kind::D2 && qb.bit == L2;
  if (boost_press || boost_pressure >= 0) {
    const int p = boost_press ? 255 : boost_pressure;
    if (qb_r2) o.r2p = r2_full ? 255 : std::max(o.r2p, p);
    else if (qb_l2) o.l2p = l2_full ? 255 : std::max(o.l2p, p);
  }
  o.lstick_rest = lstick_rest;
  int* axes[4] = {&o.lx, &o.ly, &o.rx, &o.ry};
  for (int a = 0; a < 4; ++a)
    if (neg[a] || pos[a]) *axes[a] = neg[a] && pos[a] ? 128 : neg[a] ? 0 : 255;
  return o;
}

std::string mission_button(size_t c) {
  if (c >= kControlCount || kControls[c].kind == Kind::Macro || kControls[c].slot < 0) return "";
  u8 codes[kSlotCount];
  if (!game::key_assignment(codes)) return "";
  PadElem e = elem_of_code(codes[kControls[c].slot]);
  if (!e.valid) return "NONE";
  if (e.kind == Kind::Axis) {
    static const char* const kAxis[4][2] = {{"L STICK LEFT", "L STICK RIGHT"}, {"L STICK UP", "L STICK DOWN"},
                                            {"R STICK LEFT", "R STICK RIGHT"}, {"R STICK UP", "R STICK DOWN"}};
    return kAxis[e.axis][e.positive ? 1 : 0];
  }
  struct Name {
    Kind kind;
    u16 bit;
    const char* name;
  };
  static const Name kNames[] = {{Kind::D2, CROSS, "CROSS"}, {Kind::D2, CIRCLE, "CIRCLE"}, {Kind::D2, SQUARE, "SQUARE"},
                                {Kind::D2, TRIANGLE, "TRIANGLE"}, {Kind::D2, L1, "L1"}, {Kind::D2, L2, "L2"}, {Kind::D2, R1, "R1"},
                                {Kind::D2, R2, "R2"}, {Kind::D1, L3, "L3"}, {Kind::D1, R3, "R3"}, {Kind::D1, START, "START"},
                                {Kind::D1, SELECT, "SELECT"}, {Kind::D1, UP, "D-PAD UP"}, {Kind::D1, DOWN, "D-PAD DOWN"},
                                {Kind::D1, LEFT, "D-PAD LEFT"}, {Kind::D1, RIGHT, "D-PAD RIGHT"}};
  for (const Name& n : kNames)
    if (n.kind == e.kind && n.bit == e.bit) return n.name;
  return "";
}

MouseSettings mouse() {
  return {g_sensitivity_x.load(std::memory_order_relaxed), g_sensitivity_y.load(std::memory_order_relaxed),
          g_invert_x.load(std::memory_order_relaxed), g_invert_y.load(std::memory_order_relaxed),
          g_mouse_mode.load(std::memory_order_relaxed)};
}

void set_mouse(const MouseSettings& m) {
  g_sensitivity_x = m.sensitivity_x;
  g_sensitivity_y = m.sensitivity_y;
  g_invert_x = m.invert_x;
  g_invert_y = m.invert_y;
  g_mouse_mode = m.mode;
}

}  // namespace rt::bindings
