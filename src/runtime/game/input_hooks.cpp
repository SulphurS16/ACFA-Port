// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/rsx/rsx.h"

namespace ppu {
namespace {

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
// ---- Mouse look ----
// Each AC's operator writes its turn command into a virtual pad state with 0x70eb30(input, value)
// (the player's: the stick after the input layer's dead zone (~0.4) and curve, from 0x6eee98); the
// AC control reads it through 0x70b610 and turns at a rate proportional to it (measured: 0.1 ->
// 7.7, 0.25 -> 18.6, 0.5 -> 38.9, 1.0 -> 71 deg/s), and counter-steers rotation that does not come
// from that command. Mouse turning is therefore added to the player's command where the stick's is
// written: a value from raw mouse counts per frame time turns the AC by the same angle per count at
// any frame rate, within the AC's own turn rate limit. Input objects of every class (the player's
// seen with vtables 0x1228610 and 0x1228550, AI-driven ACs' 0x122e3c8) read their virtual pad
// state at +12 (vtable slot 2, 0x70b540); the player's is the one the operator writes.
// The player operator sets the turn and pitch commands through a temporary view object on the
// stack (vtable 0x1226c30); AI-driven units do not use that class. The operator code differs by
// state: the reference saw the calls from 0x6eee98 (return sites 0x6ef030 turn, 0x6ef110 /
// 0x6ef77c pitch); with this project's saves (Line Ark, Mimir) they come from 0x6e2198 (turn
// 0x6e22ec) and 0x6edef4 (pitch). The view class identifies the player in every variant.
constexpr u32 kPlayerViewVtable = 0x01226c30;
constexpr u32 kPlayerTurnSetSite = 0x006ef030;
// Sensitivity 1.0: 1000 counts per second turn the AC at its full rate (value 1.0), i.e. one count
// turns 1/1000 of the AC's full rate times one second (0.075 deg for an AC turning 75 deg/s).
constexpr double kTurnPerCount = 1.0 / 1000.0;
// Movement beyond the AC's turn rate is carried into the next frames, at most this many seconds
// of full-rate turning (no input lost to the limit, no long runaway turns after a flick).
constexpr double kMaxCarrySeconds = 0.25;
// The look pitch command (word 13 of the virtual pad state) is set by the player operator through
// the view's slot 9 (0x70b548, a thunk to the setter 0x70e960), from one of these call sites: the
// first passes the stick's value negated (the game's vertical inversion option), the second as is.
// The AC's look control (0x7a2fb8) reads it (0x70b7e8) and moves the look pitch (0x7a22d0, state
// at look control +48): the pitch velocity follows command x max speed through a first-order
// filter (60 steps per second), the angle is clamped to the AC's limits.
constexpr u32 kPlayerPitchSetSites[] = {0x006ef110, 0x006ef77c};
// Pitch per count relative to turning (same scale until the two rates are compared in degrees).
constexpr double kPitchPerCount = kTurnPerCount;
// Sign of the pitch command that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;

struct MouseAxis {
  float value = 0;     // this frame's command from the mouse
  double backlog = 0;  // counts beyond the AC's rate, still to apply
};
MouseAxis g_mouse_turn, g_mouse_pitch;
std::atomic<u32> g_turn_input{0};    // last player turn input seen (diagnostics)
std::atomic<u32> g_player_state{0};  // the player's virtual pad state (written through the view)
std::atomic<u32> g_look{0};          // the player AC's look control object (diagnostics)
LARGE_INTEGER g_last_turn{};       // time of the last mouse look computation

bool is_player_input(u32 input) {
  u32 state = g_player_state.load(std::memory_order_relaxed);
  // input is the game's own call argument (no VirtualQuery here: it costs milliseconds per call in this process)
  return state && input && r32(input + 12) == state;
}

void mouse_axis(MouseAxis& a, s64 counts, double sens, double dt) {
  double c = a.backlog + double(counts);
  double value = std::clamp(c * sens / dt, -1.0, 1.0);
  double rest = c - value * dt / sens, max_rest = kMaxCarrySeconds / std::abs(sens);
  a.backlog = std::clamp(rest, -max_rest, max_rest);
  a.value = float(value);
}

// Once per game frame (the operator sets the player's turn command once per frame, before the
// pitch command): the mouse movement since the previous frame becomes this frame's turn and pitch
// values. The interval between these calls is the game's frame time; after a longer gap (menus,
// pause, a hitch) the movement gathered meanwhile is dropped instead of turning the AC all at once.
void mouse_look_frame() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_turn.QuadPart ? double(now.QuadPart - g_last_turn.QuadPart) / double(freq.QuadPart) : 0;
  g_last_turn = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25 || m.sensitivity_x <= 0 || m.sensitivity_y <= 0) {
    if ((dx || dy || g_mouse_turn.backlog || g_mouse_pitch.backlog) && traced++ < 60)
      LOG_I("input", "mouse look: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx,
            (long long)dy, dt * 1000);
    g_mouse_turn = g_mouse_pitch = MouseAxis{};
    return;
  }
  mouse_axis(g_mouse_turn, m.invert_x ? -dx : dx, m.sensitivity_x * kTurnPerCount, dt);
  double pitch_sens = m.sensitivity_y * kPitchPerCount * kPitchDownSign;
  mouse_axis(g_mouse_pitch, m.invert_y ? -dy : dy, pitch_sens, dt);
  // At a pitch limit the game discards motion beyond it; the carried mouse motion toward that limit
  // is dropped too, so pushing against the limit leaves no dead zone for the way back.
  if (u32 look = g_look.load(std::memory_order_relaxed); g_mouse_pitch.backlog != 0 && look) {
    float angle = f32_of_bits(r32(look + 48)), hi = f32_of_bits(r32(look + 48 + 32)), lo = f32_of_bits(r32(look + 48 + 36));
    double toward = g_mouse_pitch.backlog * pitch_sens;  // > 0: up
    if ((toward > 0 && angle >= hi) || (toward < 0 && angle <= lo)) g_mouse_pitch.backlog = 0;
  }
  if ((dx || dy || g_mouse_turn.backlog || g_mouse_pitch.backlog) && traced++ < 60)
    LOG_I("input", "mouse look: %lld, %lld counts + backlog -> turn %.3f, pitch %.3f over %.1f ms, backlog %.0f, %.0f",
          (long long)dx, (long long)dy, double(g_mouse_turn.value), double(g_mouse_pitch.value), dt * 1000,
          g_mouse_turn.backlog, g_mouse_pitch.backlog);
}

}  // namespace

// Turn command setter (0x70eb30: r3 input object, f1 value). For the player's input object the
// mouse turn of this frame is added to the stick's value.
void hook_input_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 input = u32(ctx.r[3]);
  if (u32(ctx.lr) == kPlayerTurnSetSite || (input && r32(input) == kPlayerViewVtable)) {
    static int logged = 0;
    if (g_turn_input.exchange(input) != input && logged++ < 4)
      LOG_I("input", "mouse look: player turn command set through 0x%08x (vtable %08x), pad state 0x%08x", input,
            r32(input), r32(input + 12));
    g_player_state.store(r32(input + 12), std::memory_order_relaxed);
    mouse_look_frame();
    float stick = float(ctx.f[1]);
    if (g_mouse_turn.value != 0) ctx.f[1] = double(std::clamp(stick + g_mouse_turn.value, -1.0f, 1.0f));
  }
  orig(ctx);
}

// Look (pitch) command setter thunk (0x70b548: r3 view, f1 value). From the player operator, the
// mouse pitch of this frame is added to the stick's value (after the game's own inversion option).
void hook_input_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  u32 view = u32(ctx.r[3]);
  if (lr == kPlayerPitchSetSites[0] || lr == kPlayerPitchSetSites[1] ||
      (view && r32(view) == kPlayerViewVtable)) {
    float stick = float(ctx.f[1]);
    if (g_mouse_pitch.value != 0) ctx.f[1] = double(std::clamp(stick + g_mouse_pitch.value, -1.0f, 1.0f));
  }
  orig(ctx);
}

// An AC's look control update (0x7a2fb8: r3 the look control, r4 its input, f1 the frame time).
// Records the player's for diagnostics (axes command): every input class reads its virtual pad
// state at +12 (vtable slot 2, 0x70b540), the player's is the one the operator writes.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  if (is_player_input(input) && g_look.exchange(look) != look) {
    static int logged = 0;
    if (logged++ < 4) LOG_I("input", "player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + 48);
  }
  orig(ctx);
}

}  // namespace ppu

