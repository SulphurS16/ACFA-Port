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

void f_006fe7c0(ppu::Ctx&);  // an AC's turn step (r3 turn object, f1 command): the full-speed probe

namespace ppu {
namespace {

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
// ---- Mouse look ----
// The player's operator hands the AC's commands to its view object through virtual setters: slot 6
// turn (0x70eb30; the tutorial's view class has its own, 0x70ed88), slot 9 look pitch (0x70b548, a
// thunk to slot 8: 0x70e960, the tutorial's 0x70ebd0), the latter negated first when the game's
// vertical inversion option is set. A setter stores the value only while the view's flags allow it
// (view +16 -> word & 2 for turn, & 4 for pitch). The game then smooths the stick: the AC's yaw rate
// and the look control's pitch velocity follow the command through drag.
//
// The mouse does not go that way (user brief 2026-10-16, from the Armored Core 4 port's playtest;
// that port's method, its commit c23c839, with this game's addresses): raw counts become angles
// (kRadPerCount per count times the sensitivity; no smoothing, acceleration or dead zone), and each
// frame the AC turns, and the view pitches, by the mouse's angle directly, at most at the speed of
// the stick fully pushed. Movement beyond that speed waits as a pending angle and follows at full
// speed in the next frames, so the AC turns until it points where the mouse points and no movement
// is lost. The pending angle is dropped only where the stick would be ignored too (the view's
// flags, menus and pauses, a gap of more than 0.25 s), during a view reset, and against a pitch
// limit (no dead zone on the way back).
//   Turn: an AC body turns by its yaw rate (body +516, radians per 1/60 s step), which the game
// updates once per frame from the frame's turn input (0x708770: the turn object's accumulator +28
// through the drag +36; the turn object is r32(r32(r32(controller +4) +156) +268), given to the turn
// step 0x6fe7c0 by the turn inputs 0x753170 / 0x7535d0). After that update the mouse's share of the
// frame is added to the rate, and taken off again before the next update, so the stick's own
// dynamics never see it. Full speed is what the stick fully pushed reaches: the game's own turn step
// (0x6fe7c0) run with a full command as a probe (the accumulator +28 and the drag +36 restored after
// it) gives the frame's addition (rate factor +128 x the AC's turn speed for the direction / scale,
// times its tuning when +188 is set) and the drag; the steady rate is their quotient.
//   Pitch: the look control's pitch angle (+48) gets the mouse's step after its update; full speed
// max speed (+72) over drag (+76) per step; limits +80 (up) / +84 (down); mode +68 is 1 while a view
// reset recentres it.
//   Tutorial: its view's turn setter marks a turn input on a non-zero command (a mouse turn passes
// 1/10000 of a command); its pitch setter marks a look input on r5, the operator's "pitch input"
// flag, which a mouse pitch raises (the other views' setter does not read r5). A step counts an
// input held without a break for half a second (the records at view +24: 0x70bd08 starts the
// 0.5 s on the mark, 0x70d4f8 counts it or starts over on a frame without it; run 20261016-023636):
// the mouse's input lasts while the view still turns from it (a pending angle) and over the gaps
// between the mouse's reports (kInputBridge), so turning or looking with the mouse for half a
// second counts as the stick held as long.
// The player operator sets the turn and pitch commands through a temporary view object on the
// stack (vtable 0x1226c30); AI-driven units do not use that class. The operator code differs by
// state: the reference saw the calls from 0x6eee98 (return sites 0x6ef030 turn, 0x6ef110 /
// 0x6ef77c pitch); with this project's saves (Line Ark, Mimir) they come from 0x6e2198 (turn
// 0x6e22ec) and 0x6edef4 (pitch). The view class identifies the player in every variant. The
// tutorial uses a second view class (vtable 0x1226b70, run 20261015-120215). These two are the
// only classes with the view's pitch thunk (slot 9) in the executable.
constexpr u32 kPlayerViewVtable = 0x01226c30;
constexpr u32 kTutorialViewVtable = 0x01226b70;
constexpr u32 kPlayerTurnSetSite = 0x006ef030;
constexpr u32 kPlayerPitchSetSites[] = {0x006ef110, 0x006ef77c};
constexpr double kRadPerCount = 3.14159265358979323846 / 2000;  // 0.09 degrees: 1000 counts turn 90
// Sign of the pitch that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
constexpr u32 kPitchState = 48;  // the look control's pitch state (0x7a22d0)
constexpr u32 kYawRate = 516;    // an AC body's yaw rate (body = turn object +4)
// How long a mouse input still counts as held after the mouse's last movement (the tutorial's steps):
// longer than the gaps between a slowly moved mouse's reports, short enough that a pause ends it.
constexpr double kInputBridge = 0.12;

// The mouse's pending angles and this frame's facts (the game's main thread: the operator, the turn
// step, the AC update and the look control all run there).
struct MouseLook {
  double turn = 0, pitch = 0;           // radians still to apply (turn: the stick's positive direction; pitch: up)
  bool turned = false, pitched = false;  // the mouse turns / looks now (the tutorial's steps)
  int turn_dir = 0;                     // the direction of the last mouse turn (sign)
  double turn_at = -1, pitch_at = -1;   // when the mouse last moved that way (seconds)
};
MouseLook g_ml;
LARGE_INTEGER g_last_frame{};           // the previous mouse look frame
std::atomic<u32> g_turn_input{0};       // last player turn view seen (diagnostics)
std::atomic<u32> g_player_state{0};     // the player's virtual pad state (written through the view)
std::atomic<u32> g_look{0};             // the player AC's look control object
std::atomic<u32> g_player_turn{0};      // the player AC's turn object
std::atomic<u32> g_turn_toc{0};         // r2 of the turn inputs (the turn step's TOC, for the probe)
float g_turn_added = 0;                 // the yaw rate the mouse added this frame (taken off before the next update)
u32 g_turn_added_body = 0;

float rf(u32 ea) { return f32_of_bits(r32(ea)); }
void wf(u32 ea, float v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  w32(ea, bits);
}

bool is_player_view(u32 view) {
  u32 vt = view ? r32(view) : 0;
  return vt == kPlayerViewVtable || vt == kTutorialViewVtable;
}

bool is_player_input(u32 input) {
  u32 state = g_player_state.load(std::memory_order_relaxed);
  // input is the game's own call argument (no VirtualQuery here: it costs milliseconds per call in this process)
  return state && input && r32(input + 12) == state;
}

bool view_allows(u32 view, u32 mask) {
  u32 flags = view ? r32(view + 16) : 0;
  return flags && rt::vm::committed(flags, 4) && (r32(flags) & mask) != 0;
}

// Once per game frame, from the operator's turn call (before the pitch call, the turn step, the AC's
// update and the look control): the mouse movement since the previous frame joins the pending
// angles. After a longer gap (menus, a pause, a hitch) nothing gathered meanwhile turns the AC.
void mouse_look_frame(u32 view) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25) {
    if ((dx || dy || g_ml.turn || g_ml.pitch) && traced++ < 60)
      LOG_I("input", "mouse look: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx, (long long)dy, dt * 1000);
    g_ml = MouseLook{};
    return;
  }
  double t = double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;
  double p = double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;
  double at = double(now.QuadPart) / double(freq.QuadPart);
  if (t != 0) {
    g_ml.turn_at = at;
    g_ml.turn_dir = t > 0 ? 1 : -1;
  }
  if (p != 0) g_ml.pitch_at = at;
  g_ml.turn = view_allows(view, 2) ? g_ml.turn + t : 0;  // where the view takes no turn the stick turns nothing either
  g_ml.pitch = view_allows(view, 4) ? g_ml.pitch + p : 0;
  g_ml.turned = g_ml.turn != 0 || (g_ml.turn_at >= 0 && at - g_ml.turn_at < kInputBridge);
  g_ml.pitched = g_ml.pitch != 0 || (g_ml.pitch_at >= 0 && at - g_ml.pitch_at < kInputBridge);
  if ((dx || dy) && traced++ < 60)
    LOG_I("input", "mouse look: %lld, %lld counts over %.1f ms -> pending turn %.4f, pitch %.4f rad", (long long)dx, (long long)dy, dt * 1000,
          g_ml.turn, g_ml.pitch);
}

// The steady yaw rate of the stick fully pushed in a direction (radians per step): the game's own
// turn step (0x6fe7c0) run as a probe with a full command on the player's turn object, its
// accumulator (+28) and drag (+36) restored after it; the yaw integrator (0x707840 -> 0x706cb0)
// takes the accumulator times the turn object's +44 (0.94 for the completed-game save's AC) and
// settles where the drag takes it back off (its other terms are 0 and 1). 0 if not known yet.
double full_turn_rate(Ctx& ctx, u32 obj, int dir) {
  u32 toc = g_turn_toc.load(std::memory_order_relaxed);
  if (!toc || !rt::vm::committed(obj, 192)) return 0;
  u32 acc = r32(obj + 28), drag = r32(obj + 36);
  float before = rf(obj + 28);
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x400) & ~15u;
  w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = toc;
  ctx.r[3] = obj;
  ctx.f[1] = dir > 0 ? 1.0 : -1.0;
  f_006fe7c0(ctx);
  float add = rf(obj + 28) - before, turn_drag = rf(obj + 36), gain = rf(obj + 44);
  w32(obj + 28, acc);
  w32(obj + 36, drag);
  ctx = saved;
  static int logged = 0;
  double full = turn_drag > 0 ? std::fabs(double(add)) * double(gain) / double(turn_drag) : 0;
  if (logged++ < 4) LOG_I("input", "mouse look: full stick turn step %g x %g, drag %g -> %g rad per step", double(add), double(gain), double(turn_drag), full);
  return full;
}

// The operator's turn call of the player: the player's view state, then the frame's mouse movement.
void player_turn_call(Ctx& ctx) {
  u32 view = u32(ctx.r[3]);
  if (!view || !rt::vm::committed(view + 12, 4)) return;
  static int logged = 0;
  if (g_turn_input.exchange(view) != view && logged++ < 4)
    LOG_I("input", "mouse look: player turn command set through 0x%08x (vtable %08x), pad state 0x%08x", view, r32(view), r32(view + 12));
  g_player_state.store(r32(view + 12), std::memory_order_relaxed);
  mouse_look_frame(view);
}

// The mouse's pitch step after the player's look control updated the angle from the stick: the
// pending angle, at most at full speed, within the limits (the rest toward a limit is dropped).
void mouse_pitch_step(u32 look, double dt) {
  u32 p = look + kPitchState;
  if (g_ml.pitch == 0 || !rt::vm::committed(p, 40)) return;
  if (r32(p + 20) == 1) {  // a view reset recentres the view: the mouse waits
    g_ml.pitch = 0;
    return;
  }
  double steps = dt * 60, maxs = rf(p + 24), drag = rf(p + 28), hi = rf(p + 32), lo = rf(p + 36);
  if (steps <= 0 || steps > 15 || maxs <= 0 || drag <= 0 || lo >= hi) return;
  double full = maxs / drag * steps;
  double step = std::clamp(g_ml.pitch, -full, full);
  double angle = rf(p), next = std::clamp(angle + step, lo, hi);
  wf(p, float(next));
  g_ml.pitch -= step;
  if ((g_ml.pitch > 0 && next >= hi) || (g_ml.pitch < 0 && next <= lo) || std::fabs(g_ml.pitch) < 1e-7) g_ml.pitch = 0;
}

}  // namespace

// Turn command setters (0x70eb30, the tutorial's 0x70ed88: r3 view, f1 value): the stick's command
// goes on unchanged; from the player's operator the frame's mouse movement is gathered (it turns the
// AC in ac_turn_update). The tutorial's view counts its "turn" steps on a non-zero command: a mouse
// turn passes a command too small to turn the AC (1/10000 of the stick's), so the step counts it too.
void hook_input_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 view = u32(ctx.r[3]);
  if (u32(ctx.lr) == kPlayerTurnSetSite || is_player_view(view)) {
    player_turn_call(ctx);
    if (view && r32(view) == kTutorialViewVtable && g_ml.turned && ctx.f[1] == 0) ctx.f[1] = g_ml.turn_dir > 0 ? 1e-4 : -1e-4;
  }
  orig(ctx);
}

// Look (pitch) command setter thunk (0x70b548: r3 view, f1 value, r5 the operator's "pitch input"
// flag): the stick's command goes on unchanged. The tutorial view's slot 8 (0x70ebd0) marks its
// "look up and down" step on that flag: a mouse pitch raises it (slot 8 of the other views ignores it).
void hook_input_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  u32 view = u32(ctx.r[3]);
  if (lr == kPlayerPitchSetSites[0] || lr == kPlayerPitchSetSites[1] || is_player_view(view)) {
    if (g_ml.pitched) ctx.r[5] = 1;
  }
  orig(ctx);
}

// The turn inputs (0x753170 every frame, 0x7535d0 in the other operator state: r3 an AC's controller,
// r4 its input). The player's (its input holds the player's view state) gives the player's turn
// object, r32(r32(r32(controller +4) +156) +268), and the TOC its turn step runs with.
namespace {
void note_turn_input(Ctx& ctx) {
  u32 ctl = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  if (!state || !input || !rt::vm::committed(input + 12, 4) || r32(input + 12) != state || !rt::vm::committed(ctl + 4, 4)) return;
  u32 a = r32(ctl + 4);
  if (!a || !rt::vm::committed(a + 156, 4)) return;
  u32 b = r32(a + 156);
  if (!b || !rt::vm::committed(b + 268, 4)) return;
  u32 obj = r32(b + 268);
  g_turn_toc.store(u32(ctx.r[2]), std::memory_order_relaxed);
  if (g_player_turn.exchange(obj) != obj) LOG_I("input", "mouse look: player turn object 0x%08x", obj);
}
}  // namespace
void hook_ac_turn_input(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}
void hook_ac_turn_input_b(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}

// 0x708770 (r3 an AC's turn object): the AC body's yaw rate from the frame's turn input. For the
// player's: the mouse's share of the last frame comes off first (the game's update sees the stick's
// rate only), then this frame's share goes on: the pending angle, at most at full speed (with the
// stick, the two together stay within it).
void hook_ac_turn_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]);
  if (!obj || obj != g_player_turn.load(std::memory_order_relaxed) || !rt::vm::committed(obj, 192)) {
    orig(ctx);
    return;
  }
  u32 body = r32(obj + 4);
  if (!body || !rt::vm::committed(body + kYawRate, 4)) {
    orig(ctx);
    return;
  }
  if (g_turn_added != 0 && g_turn_added_body == body) wf(body + kYawRate, rf(body + kYawRate) - g_turn_added);
  g_turn_added = 0;
  orig(ctx);
  double steps = double(rf(obj + 8)) * 60;  // the frame's time in 1/60 s steps
  if (g_ml.turn == 0 || steps <= 0 || steps > 15) return;
  double full = full_turn_rate(ctx, obj, g_ml.turn > 0 ? 1 : -1);
  if (full <= 0) return;
  double rate = rf(body + kYawRate);
  double add = std::clamp(g_ml.turn / steps, std::min(0.0, -full - rate), std::max(0.0, full - rate));
  wf(body + kYawRate, float(rate + add));
  g_turn_added = float(add);
  g_turn_added_body = body;
  g_ml.turn -= add * steps;
  if (std::fabs(g_ml.turn) < 1e-7) g_ml.turn = 0;
}

// An AC's look control update (0x7a2fb8: r3 the look control, r4 its input, f1 the frame time). For
// the player's (its input holds the player's view state at +12): the mouse's pitch step after the
// update.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  double dt = ctx.f[1];
  bool player = is_player_input(input);
  if (player && g_look.exchange(look) != look) {
    static int logged = 0;
    if (logged++ < 400) LOG_I("input", "player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + 48);
  }
  orig(ctx);
  if (player) mouse_pitch_step(look, dt);
}

}  // namespace ppu

