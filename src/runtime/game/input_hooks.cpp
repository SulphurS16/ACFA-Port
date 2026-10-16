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
#include "runtime/game/menu_mouse.h"
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
// ---- Mouse camera ----
// The player's operator hands the AC's commands to its view object through virtual setters: slot 6
// turn (0x70eb30; the tutorial's view class has its own, 0x70ed88), slot 9 look pitch (0x70b548, a
// thunk to slot 8: 0x70e960, the tutorial's 0x70ebd0), the latter negated first when the game's
// vertical inversion option is set. A setter stores the value only while the view's flags allow it
// (view +16 -> word & 2 for turn, & 4 for pitch). The game then smooths the stick: the AC's yaw rate
// and the look control's pitch velocity follow the command through drag.
//
// The mouse moves the camera (user decision 2026-10-16): raw counts become angles (kRadPerCount per
// count times the sensitivity; no smoothing, acceleration or dead zone) and turn and pitch the
// camera at once, at any speed. The AC chases the camera's heading with its own turning: the turn
// command, as the stick would give it, set at the turn setter (full while the camera is far off,
// less near it: the AC closes a 1/kChaseSteps share of the angle per 1/60 s step, less the angle
// its present yaw rate still covers as the drag slows it, so it stops on the heading).
// The game still places the camera (collision, distance, framing): the camera's heading is the AC's
// heading plus the look control's yaw angle (+4, the state that 0x7a2940 runs through the pitch
// state's integrator 0x7a22d0; its limits are 0 in this game's camera, so it never turns by
// itself). After each look update the mouse camera sets that angle to the camera heading less the
// AC's (limits widened to a half turn each way); the radar turns with it (run 20261016-102109). The
// game's follow camera would still swing to a new heading or pitch over half a second (its spring;
// mouse_camera_follow): the turn reaches it at once, the spring keeps smoothing the AC's movement.
// Keys bound to the right stick turn the camera at the stick's full turning speed. Once the AC
// points where the camera does (within kSettled) and the mouse rests, the camera rides with the AC
// again, so whatever else turns the AC (auto-sighting in order matches, scripts) turns the camera
// as in the game, until the mouse turns it.
//   Turn: an AC body turns by its yaw rate (body +516, radians per 1/60 s step), which the game
// updates once per frame from the frame's turn input (0x708770: the turn object's accumulator +28
// through the drag +36; the turn object is r32(r32(r32(controller +4) +156) +268), given to the turn
// step 0x6fe7c0 by the turn inputs 0x753170 / 0x7535d0). The stick fully pushed settles at a rate
// the game's own turn step gives when run as a probe with a full command (the accumulator +28 and
// the drag +36 restored after it): its addition (rate factor +128 x the AC's turn speed for the
// direction / scale, times its tuning when +188 is set) times the gain +44, over the drag.
//   Pitch: the look control's pitch angle (+48) takes the mouse's angle after its update, within
// its limits +80 (up) / +84 (down); mode +68 is 1 while a view reset recentres the view.
//   The game keeps its own camera (the mouse camera follows it, so the mouse continues from where
// the camera is) while a controller was used after the mouse, while the view takes no turn input,
// and during a view reset (R3: the camera goes back behind the AC).
//   Tutorial: its view's turn setter marks a turn input on a non-zero command (the AC's chase, or
// 1/10000 of a command while the mouse moves); its pitch setter marks a look input on r5, the
// operator's "pitch input" flag, which a mouse pitch raises (the other views' setter does not read
// r5). A step counts an input held without a break for half a second (the records at view +24:
// 0x70bd08 starts the 0.5 s on the mark, 0x70d4f8 counts it or starts over on a frame without it;
// run 20261016-023636): the mouse's input lasts over the gaps between its reports (kInputBridge).
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
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadPerCount = kPi / 2000;  // 0.09 degrees: 1000 counts turn 90
// Sign of the pitch that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
constexpr u32 kLookYaw = 4;      // the look control's yaw state (angle +0, velocity +4, limits +32 high / +36 low)
constexpr u32 kPitchState = 48;  // the look control's pitch state (0x7a22d0; the same layout, mode +20)
constexpr u32 kBodyRow2 = 464;   // an AC body's rotation row 2 (x +464, z +472): heading atan2(x, z)
constexpr u32 kYawRate = 516;    // an AC body's yaw rate (radians per 1/60 s step; positive: the heading grows)
// How long a mouse input still counts as held after the mouse's last movement (the tutorial's steps):
// longer than the gaps between a slowly moved mouse's reports, short enough that a pause ends it.
constexpr double kInputBridge = 0.12;
// The AC's chase: the turn command that closes a 1/kChaseSteps share of the angle to the camera's
// heading per 1/60 s step, at most the stick fully pushed.
constexpr double kChaseSteps = 4;
constexpr double kSettled = 0.2 * kPi / 180;  // the AC this near the camera's heading: the camera rides with it again

// The mouse camera (the game's main thread: the operator, the turn step, the AC update and the look
// control all run there, in that order).
struct MouseCam {
  bool on = false;                       // the mouse has the camera this frame
  bool known = false;                    // yaw holds the camera's heading
  bool reset = false;                    // a view reset recentres the view (the game moves the camera)
  bool attached = true;                  // the camera rides with the AC (it points where the camera does)
  bool moved = false;                    // the mouse turned the camera this frame
  double yaw = 0;                        // the camera's heading (radians, the body's heading convention)
  double pitch = 0;                      // mouse pitch still to apply (radians, up)
  u32 body = 0;                          // the AC body whose camera this is
  double steps = 1;                      // this frame's time in 1/60 s steps
  bool turned = false, pitched = false;  // the mouse turns / looks now (the tutorial's steps)
  int turn_dir = 0;                      // the direction of the last mouse turn (sign)
  double turn_at = -1, pitch_at = -1;    // when the mouse last moved that way (seconds)
};
MouseCam g_cam;
double g_full[2] = {0, 0};              // the stick's full turn rate for -1 / +1 (radians per step; 0: not known)
double g_turn_drag = 0;                 // the share of the yaw rate the drag takes off per step (0: not known)
LARGE_INTEGER g_last_frame{};           // the previous mouse camera frame
std::atomic<u32> g_turn_input{0};       // last player turn view seen (diagnostics)
std::atomic<u32> g_player_state{0};     // the player's virtual pad state (written through the view)
std::atomic<u32> g_look{0};             // the player AC's look control object
std::atomic<u32> g_player_turn{0};      // the player AC's turn object
std::atomic<u32> g_turn_toc{0};         // r2 of the turn inputs (the turn step's TOC, for the probe)

float rf(u32 ea) { return f32_of_bits(r32(ea)); }
void wf(u32 ea, float v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  w32(ea, bits);
}

double wrap(double a) {
  a = std::fmod(a + kPi, 2 * kPi);
  return a < 0 ? a + kPi : a - kPi;
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

// The player AC's body (turn object +4) and its heading.
u32 player_body() {
  u32 obj = g_player_turn.load(std::memory_order_relaxed);
  if (!obj || !rt::vm::committed(obj + 4, 4)) return 0;
  u32 body = r32(obj + 4);
  return body && rt::vm::committed(body + kBodyRow2, 12) ? body : 0;
}
double heading_of(u32 body) { return std::atan2(double(rf(body + kBodyRow2)), double(rf(body + kBodyRow2 + 8))); }

// Once per game frame, from the operator's turn call (before the pitch call, the turn step, the AC's
// update and the look control): the mouse movement since the previous frame turns and pitches the
// camera. After a longer gap (menus, a pause, a hitch) nothing gathered meanwhile moves it.
void mouse_look_frame(u32 view) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  // A controller used after the mouse has the camera (the game's own), and so does the game while
  // the view takes no turn input.
  g_cam.on = !rt::game::controller_used_last() && view_allows(view, 2);
  g_cam.moved = false;
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25) {
    if ((dx || dy) && traced++ < 60)
      LOG_I("input", "mouse camera: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx, (long long)dy, dt * 1000);
    g_cam.pitch = 0;
    g_cam.steps = 1;
    return;
  }
  g_cam.steps = dt * 60;
  double t = double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;
  double p = double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;
  double at = double(now.QuadPart) / double(freq.QuadPart);
  if (t != 0) {
    g_cam.turn_at = at;
    g_cam.turn_dir = t > 0 ? 1 : -1;
  }
  if (p != 0) g_cam.pitch_at = at;
  if (g_cam.on && g_cam.known && t != 0) {
    g_cam.yaw = wrap(g_cam.yaw + t);
    g_cam.moved = true;
  }
  g_cam.pitch = g_cam.on && view_allows(view, 4) ? g_cam.pitch + p : 0;
  g_cam.turned = t != 0 || (g_cam.turn_at >= 0 && at - g_cam.turn_at < kInputBridge);
  g_cam.pitched = p != 0 || (g_cam.pitch_at >= 0 && at - g_cam.pitch_at < kInputBridge);
  if ((dx || dy) && traced++ < 60)
    LOG_I("input", "mouse camera: %lld, %lld counts over %.1f ms -> heading %.4f, pitch %.4f rad to apply", (long long)dx, (long long)dy,
          dt * 1000, g_cam.yaw, g_cam.pitch);
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
  if (turn_drag > 0 && turn_drag < 1) g_turn_drag = turn_drag;
  if (logged++ < 4) LOG_I("input", "mouse camera: full stick turn step %g x %g, drag %g -> %g rad per step", double(add), double(gain), double(turn_drag), full);
  return full;
}

// The operator's turn call of the player: the player's view state, then the frame's mouse movement.
void player_turn_call(Ctx& ctx) {
  u32 view = u32(ctx.r[3]);
  if (!view || !rt::vm::committed(view + 12, 4)) return;
  static int logged = 0;
  if (g_turn_input.exchange(view) != view && logged++ < 4)
    LOG_I("input", "mouse camera: player turn command set through 0x%08x (vtable %08x), pad state 0x%08x", view, r32(view), r32(view + 12));
  g_player_state.store(r32(view + 12), std::memory_order_relaxed);
  mouse_look_frame(view);
}

// The turn command of the frame while the mouse has the camera: a stick-like turn (keys bound to the
// right stick) turns the camera at the stick's speed; the AC turns toward the camera's heading, and
// once it points there with the mouse at rest the camera rides with it (no command of its own).
double chase_command(double stick) {
  u32 body = player_body();
  if (!g_cam.on || !g_cam.known || !body || body != g_cam.body) return stick;
  double full_r = g_full[1] > 0 ? g_full[1] : 0.02, full_l = g_full[0] > 0 ? g_full[0] : 0.02;
  if (stick != 0) g_cam.yaw = wrap(g_cam.yaw + stick * (stick > 0 ? full_r : full_l) * g_cam.steps);
  double off = wrap(g_cam.yaw - heading_of(body));
  if (stick != 0 || g_cam.moved) g_cam.attached = false;
  else if (std::fabs(off) < kSettled) g_cam.attached = true;
  if (g_cam.attached) return 0;
  // With no command the yaw rate falls by the drag's share per step: the AC still turns rate / drag.
  double coast = g_turn_drag > 0 ? double(rf(body + kYawRate)) / g_turn_drag : 0;
  double d = off - coast;
  if (std::fabs(d) < 1e-5) return 0;
  // A long frame (several steps) closes at most two thirds of the angle.
  return std::clamp(d / ((d > 0 ? full_r : full_l) * std::max(kChaseSteps, 1.5 * g_cam.steps)), -1.0, 1.0);
}

// After the player's look control updated the view from the stick: the mouse camera's heading as
// the yaw angle from the AC's heading, and the mouse's pitch, at once, within the limits.
void mouse_camera_step(u32 look) {
  u32 body = player_body(), y = look + kLookYaw, p = look + kPitchState;
  if (!body || !rt::vm::committed(y, 40) || !rt::vm::committed(p, 40)) return;
  double heading = heading_of(body);
  if (body != g_cam.body) {  // another AC (a new mission): the camera starts where the game has it
    g_cam.body = body;
    g_cam.known = false;
  }
  bool reset = g_cam.reset = r32(p + 20) == 1;  // a view reset recentres the view: the camera goes back behind the AC
  if (!g_cam.on || !g_cam.known || reset) {
    g_cam.yaw = wrap(heading + rf(y));  // the game's camera: the mouse continues from where it is
    g_cam.known = true;
    g_cam.pitch = 0;
    return;
  }
  if (g_cam.attached) g_cam.yaw = heading;  // riding with the AC, whatever turned it
  wf(y + 32, float(kPi));
  wf(y + 36, float(-kPi));
  wf(y, float(wrap(g_cam.yaw - heading)));
  wf(y + 4, 0);
  if (g_cam.pitch != 0) {
    double hi = rf(p + 32), lo = rf(p + 36);
    if (lo < hi) wf(p, float(std::clamp(double(rf(p)) + g_cam.pitch, lo, hi)));
    g_cam.pitch = 0;
  }
}

// The follow camera (0xab2090: r3 the camera, r5 the frame's wanted eye (+0) and look-at point
// (+16), f1 the frame time; the AC body it follows at +8). It does not take the wanted points: its
// eye (+24) and look-at point (+40) follow them through a spring in the AC's frame (0xab14b8, in
// 1/60 s steps) fed by how far the wanted points moved since the last frame (+56, +72; +88 set once
// they hold a frame's), so a turn of the view reaches the screen over about half a second (run
// 20261016-103853: a 90 degree turn 16 degrees short after 0.18 s, 2 after 0.4 s; the pitch alike).
// While the mouse has the gameplay camera, the wanted points' change relative to the AC (the view's
// turn and pitch, nothing of the AC's own movement) moves the eye, the look-at point and the last
// wanted points along before the update: the view turns at once, the spring still smooths the AC's
// movement, and the game's floor for the eye (0xab2090: the ground height at (body +208) +32 plus
// TOC -8388, while the camera settings' +120 (TOC -8088) is set) still holds. Not during a view
// reset: the camera swings back behind the AC as the game moves it.
constexpr u32 kGameplayCameraFollowSite = 0x00ab2b04;  // the gameplay camera's update 0xab29c0 calls 0xab2090
constexpr u32 kBodyPosition = 480;                     // an AC body's position (x, y, z)
struct CameraFollow {
  u32 cam = 0;                      // the camera of the last frame's record (0: none)
  float eye[3] = {}, look[3] = {};  // its wanted points then, less the AC's position
};
CameraFollow g_follow;

void mouse_camera_follow(Ctx& ctx) {
  u32 cam = u32(ctx.r[3]), want = u32(ctx.r[5]);
  u32 body = u32(ctx.lr) == kGameplayCameraFollowSite && cam && rt::vm::committed(cam, 92) && rt::vm::committed(want, 28) ? r32(cam + 8) : 0;
  if (!body || body != g_cam.body || !rt::vm::committed(body + kBodyPosition, 12)) {
    g_follow.cam = 0;
    return;
  }
  float eye[3], look[3];
  for (u32 i = 0; i < 3; ++i) {
    float at = rf(body + kBodyPosition + 4 * i);
    eye[i] = rf(want + 4 * i) - at;
    look[i] = rf(want + 16 + 4 * i) - at;
  }
  if (g_cam.on && !g_cam.reset && g_follow.cam == cam && r32(cam + 88) != 0) {
    for (u32 i = 0; i < 3; ++i) {
      float de = eye[i] - g_follow.eye[i], dl = look[i] - g_follow.look[i];
      wf(cam + 24 + 4 * i, rf(cam + 24 + 4 * i) + de);
      wf(cam + 56 + 4 * i, rf(cam + 56 + 4 * i) + de);
      wf(cam + 40 + 4 * i, rf(cam + 40 + 4 * i) + dl);
      wf(cam + 72 + 4 * i, rf(cam + 72 + 4 * i) + dl);
    }
    u32 toc = u32(ctx.r[2]), settings = r32(toc - 8088);
    if (settings && rt::vm::committed(settings + 120, 4) && r32(settings + 120) != 0) {
      u32 ground = rt::vm::committed(body + 208, 4) ? r32(body + 208) : 0;
      if (ground && rt::vm::committed(ground + 32, 4)) {
        float floor = rf(ground + 32) + rf(toc - 8388);
        if (rf(cam + 28) < floor) wf(cam + 28, floor);
        if (rf(cam + 60) < floor) wf(cam + 60, floor);
      }
    }
  }
  g_follow.cam = cam;
  std::memcpy(g_follow.eye, eye, sizeof eye);
  std::memcpy(g_follow.look, look, sizeof look);
}

}  // namespace

// Turn command setters (0x70eb30, the tutorial's 0x70ed88: r3 view, f1 value): from the player's
// operator the frame's mouse movement turns the camera, and while the mouse has the camera the
// command becomes the AC's chase of the camera's heading. The tutorial's view counts its "turn"
// steps on a non-zero command: while the mouse moves and the AC has nothing to chase, the command
// is 1/10000 of the stick's (too small to turn the AC), so the step counts it too.
void hook_input_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 view = u32(ctx.r[3]);
  if (u32(ctx.lr) == kPlayerTurnSetSite || is_player_view(view)) {
    player_turn_call(ctx);
    ctx.f[1] = chase_command(ctx.f[1]);
    if (view && r32(view) == kTutorialViewVtable && g_cam.turned && ctx.f[1] == 0) ctx.f[1] = g_cam.turn_dir > 0 ? 1e-4 : -1e-4;
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
    if (g_cam.pitched) ctx.r[5] = 1;
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
  if (g_player_turn.exchange(obj) != obj) LOG_I("input", "mouse camera: player turn object 0x%08x", obj);
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
// player's, while the AC chases the mouse camera: the stick's full turning speed each way (the
// chase's scale; kept from the last chase in between).
void hook_ac_turn_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]);
  orig(ctx);
  if (!g_cam.on || g_cam.attached || !obj || obj != g_player_turn.load(std::memory_order_relaxed) || !rt::vm::committed(obj, 192)) return;
  if (double r = full_turn_rate(ctx, obj, 1); r > 0) g_full[1] = r;
  if (double l = full_turn_rate(ctx, obj, -1); l > 0) g_full[0] = l;
}

// An AC's look control update (0x7a2fb8: r3 the look control, r4 its input, f1 the frame time). For
// the player's (its input holds the player's view state at +12): the mouse camera after the update.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  bool player = is_player_input(input);
  if (player && g_look.exchange(look) != look) {
    static int logged = 0;
    if (logged++ < 400) LOG_I("input", "player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + 48);
  }
  orig(ctx);
  if (player) mouse_camera_step(look);
}

// The follow camera's step toward the frame's wanted eye and look-at point (0xab2090; see
// mouse_camera_follow): the mouse camera's turn reaches it at once.
void hook_camera_follow(Ctx& ctx, void (*orig)(Ctx&)) {
  mouse_camera_follow(ctx);
  orig(ctx);
}

}  // namespace ppu

