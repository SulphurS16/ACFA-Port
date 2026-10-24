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
#include "runtime/game/key_assign.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/memory.h"
#include "runtime/rsx/rsx.h"

void f_006fe7c0(ppu::Ctx&);  // an AC's turn step (r3 turn object, f1 command): the full-speed probe
void f_005ef328(ppu::Ctx&);  // the game parameters' singleton (a stub that derives its TOC from the caller's)

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
// Mouse mode "camera" (user decision 2026-10-16; an option since 2026-10-20, the default being the
// stick mode further down): the mouse moves the camera: raw counts become angles (kRadPerCount per
// count times the sensitivity; no smoothing, acceleration or dead zone) and turn and pitch the
// camera at once, at any speed. The AC chases the camera's heading with its own turning: the turn
// command, as the stick would give it, set at the turn setter (full while the camera is far off,
// less near it: the AC closes a 1/kChaseSteps share of the angle per 1/60 s step, less the angle
// its present yaw rate still covers as the drag slows it, so it stops on the heading).
//   Only the camera turns freely (user, 2026-10-24: "the only thing that should be free movement
// is the camera itself but the fcs and moving should behave like the game normally does"). The
// look control's yaw angle (+4, the state that 0x7a2940 runs through the pitch state's integrator
// 0x7a22d0; its limits are 0 in this game's camera, so it never turns by itself) places the camera
// but also aims the FCS: lock-on and fire follow it. Until 0.7.2 the mouse camera set that angle to
// its lead on the AC, and a view far ahead of a slowly turning AC locked on and fired at targets
// beside and behind it (a player's clip, 2026-10-19; reproduced, run 20261019-152913); 0.7.3 held
// the lead within 15 degrees, which held the camera too. Now the angle stays the game's own and
// the follow camera takes the lead instead: its wanted eye and look-at point turn about the AC's
// vertical axis by the camera's heading less the game's view heading (mouse_camera_follow), so
// the AC, the FCS and the radar stay as in the game. Input on the right stick (keys bound to it,
// the quick turn keys, a controller) turns the AC as the game does, and the camera turns with the
// AC, keeping its lead: the chase would cut quick turns short (run 20261020-120539). The game's
// follow camera would still swing to a new heading or pitch over half a second (its spring;
// mouse_camera_follow): the turn reaches it at once, the spring keeps smoothing the AC's movement.
// Once the AC points where the camera does (within kSettled) and the mouse rests, the camera rides
// with the AC again, so whatever else turns the AC (auto-sighting in order matches, scripts) turns
// the camera as in the game, until the mouse turns it.
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
  bool ride = false;                     // the right stick turns the AC this frame: the camera turns with it
  double yaw = 0;                        // the camera's heading (radians, the body's heading convention)
  double pitch = 0;                      // mouse pitch still to apply (radians, up)
  double heading = 0;                    // the AC's heading at the last look update
  double view = 0;                       // the game's own view yaw on the AC then (the look control's, normally 0)
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
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  g_cam.moved = false;
  if (m.mode != rt::bindings::MouseMode::Camera) {  // the stick mode takes the mouse at the pad (mouse_stick_frame)
    g_cam.on = false;
    g_cam.pitch = 0;
    g_cam.turned = g_cam.pitched = false;
    return;
  }
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  // A controller used after the mouse has the camera (the game's own), and so does the game while
  // the view takes no turn input.
  g_cam.on = !rt::game::controller_used_last() && view_allows(view, 2);
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

// The turn command of the frame while the mouse has the camera: the AC turns toward the camera's
// heading, and once it points there with the mouse at rest the camera rides with it (no command of
// its own). Input on the right stick turns the AC as the game does (the camera turns with the AC,
// mouse_camera_step).
double chase_command(double stick) {
  u32 body = player_body();
  g_cam.ride = false;
  if (!g_cam.on || !g_cam.known || !body || body != g_cam.body) return stick;
  if (stick != 0) {
    g_cam.ride = true;
    return stick;
  }
  double full_r = g_full[1] > 0 ? g_full[1] : 0.02, full_l = g_full[0] > 0 ? g_full[0] : 0.02;
  double off = wrap(g_cam.yaw - g_cam.view - heading_of(body));
  if (g_cam.moved) g_cam.attached = false;
  else if (std::fabs(off) < kSettled) g_cam.attached = true;
  if (g_cam.attached) return 0;
  // With no command the yaw rate falls by the drag's share per step: the AC still turns rate / drag.
  double coast = g_turn_drag > 0 ? double(rf(body + kYawRate)) / g_turn_drag : 0;
  double d = off - coast;
  if (std::fabs(d) < 1e-5) return 0;
  // A long frame (several steps) closes at most two thirds of the angle.
  return std::clamp(d / ((d > 0 ? full_r : full_l) * std::max(kChaseSteps, 1.5 * g_cam.steps)), -1.0, 1.0);
}

// After the player's look control updated the view from the stick: the camera's heading (which the
// follow camera takes, mouse_camera_follow; the look control's yaw, which also aims the FCS, stays
// the game's), and the mouse's pitch, at once, within the limits.
void mouse_camera_step(u32 look) {
  u32 body = player_body(), y = look + kLookYaw, p = look + kPitchState;
  if (!body || !rt::vm::committed(y, 40) || !rt::vm::committed(p, 40)) return;
  double heading = heading_of(body);
  double turned = wrap(heading - g_cam.heading);  // the AC's turn since the previous look update
  g_cam.heading = heading;
  g_cam.view = rf(y);
  if (body != g_cam.body) {  // another AC (a new mission): the camera starts where the game has it
    g_cam.body = body;
    g_cam.known = false;
  }
  bool reset = g_cam.reset = r32(p + 20) == 1;  // a view reset recentres the view: the camera goes back behind the AC
  if (!g_cam.on || !g_cam.known || reset) {
    g_cam.yaw = wrap(heading + g_cam.view);  // the game's camera: the mouse continues from where it is
    g_cam.known = true;
    g_cam.pitch = 0;
    return;
  }
  if (g_cam.attached) g_cam.yaw = wrap(heading + g_cam.view);  // riding with the AC, whatever turned it
  else if (g_cam.ride) g_cam.yaw = wrap(g_cam.yaw + turned);   // the stick turned the AC: the camera with it
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
// While the mouse has the gameplay camera, the wanted points (the game's, for its own view of the
// AC: the camera behind it) first turn about the AC's vertical axis by the camera's lead on that
// view, the mouse camera's heading; the game's are put back after the update (they are its
// caller's). Then their change relative to the AC (the camera's turn and the view's pitch,
// nothing of the AC's own movement) moves the eye, the look-at point and the last wanted points
// along before the update: the view turns at once, the spring still smooths the AC's movement, and
// the game's floor for the eye (0xab2090: the ground height at (body +208) +32 plus TOC -8388,
// while the camera settings' +120 (TOC -8088) is set) still holds. Not during a view reset: the
// camera swings back behind the AC as the game moves it.
constexpr u32 kGameplayCameraFollowSite = 0x00ab2b04;  // the gameplay camera's update 0xab29c0 calls 0xab2090
constexpr u32 kBodyPosition = 480;                     // an AC body's position (x, y, z)
struct CameraFollow {
  u32 cam = 0;                      // the camera of the last frame's record (0: none)
  float eye[3] = {}, look[3] = {};  // its wanted points then, less the AC's position
};
CameraFollow g_follow;

struct WantedTurn {
  u32 want = 0;      // the wanted points turned for the follow step (0: none)
  u32 saved[4] = {};  // the game's: the eye's x and z, the look-at point's x and z
};

WantedTurn mouse_camera_follow(Ctx& ctx) {
  WantedTurn turn;
  u32 cam = u32(ctx.r[3]), want = u32(ctx.r[5]);
  u32 body = u32(ctx.lr) == kGameplayCameraFollowSite && cam && rt::vm::committed(cam, 92) && rt::vm::committed(want, 28) ? r32(cam + 8) : 0;
  if (!body || body != g_cam.body || !rt::vm::committed(body + kBodyPosition, 12) || !rt::vm::committed(body + kBodyRow2, 12)) {
    g_follow.cam = 0;
    return turn;
  }
  float at[3];
  for (u32 i = 0; i < 3; ++i) at[i] = rf(body + kBodyPosition + 4 * i);
  double lead = g_cam.on && g_cam.known && !g_cam.reset ? wrap(g_cam.yaw - g_cam.view - heading_of(body)) : 0;
  if (std::fabs(lead) > 1e-6) {  // a heading grows from +z toward +x (heading_of)
    double c = std::cos(lead), s = std::sin(lead);
    turn.want = want;
    for (u32 k = 0; k < 2; ++k) {
      u32 pt = want + 16 * k;
      turn.saved[2 * k] = r32(pt);
      turn.saved[2 * k + 1] = r32(pt + 8);
      double x = rf(pt) - at[0], z = rf(pt + 8) - at[2];
      wf(pt, float(at[0] + x * c + z * s));
      wf(pt + 8, float(at[2] - x * s + z * c));
    }
  }
  float eye[3], look[3];
  for (u32 i = 0; i < 3; ++i) {
    eye[i] = rf(want + 4 * i) - at[i];
    look[i] = rf(want + 16 + 4 * i) - at[i];
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
  return turn;
}

// ---- The mouse as the right stick (mouse mode "stick", the default since 2026-10-20) ----
// The mouse deflects the controller's right stick where the game reads it: the pad object's
// normalized stick floats after each update (0xe4e860: right X at +1160, right Y at +1164, up
// positive), so everything the game does with the stick follows: the AC's turn and the look up
// and down with the game's own smoothing, quick turns (the stick sideways with a quick boost), the
// view, the FCS. A frame's mouse movement asks for an angle (kRadPerCount per count times the
// sensitivity, as in camera mode); the command is the one whose steady rate covers that angle over
// the frame: the angle over the full rate times the frame's 1/60 s steps (turn: the turn step's
// probe, full_turn_rate; look: the look control's pitch state, max speed +24 over drag +28,
// radians per step, as its integrator 0x7a22d0 / 0x706cb0 settles). The game's drag then spreads
// the turn over a few frames and keeps its total. Faster than the stick turns, the command stays
// at full and the rest waits for the next frames (g_stick_pending): the AC turns at its top speed
// until it has turned what the mouse asked, up to a half turn; the look waits only for what its
// limits leave. Until 0.7.3 the rest was lost, as with a stick at its end (user decision
// 2026-10-20: option (a)), so a quick flick turned a few degrees (user, 2026-10-24: "if i move the
// mouse it barely turns at all"). A turn faster than the stick's (a quick turn: the stick sideways
// with a quick boost) ends what waits, so a flick with a quick boost turns as the quick turn does.
//   The stick reaches the player's operator through two maps of each axis: the engine's input layer
// (0xd84038 / 0xd937d0, through the pad accessor 0xe4da88) takes kInputLayerDeadZone off and
// stretches the rest to the full range ((d - 0.36) / 0.64; per axis, not on the stick's length:
// the turn command was the same with the stick also pushed up, run 20261020-125729), then 0x6eb7e0
// (input code -> value; GAME_NOTES "Input mechanics measured") a dead zone and two straight pieces
// given by the game's parameters (0x5ef328 -> +4 -> slot 7: +676 the dead zone's edge, +680 the
// value giving 0.5, +684 the one giving 1.0; 0.1, 0.65, 0.99 in the completed-game save's
// regulation). The mouse's deflection is the inverse of both, so the command is linear in the
// mouse's speed (the three measured points, 0.443 -> 0.027, 0.506 -> 0.116, 0.631 -> 0.295, fit
// the two maps to the third digit). The game's vertical inversion option
// (options block +52) inverts the stick's look; the mouse's look is turned against it, so [mouse]
// invert_y alone decides it. The stick's own deflection (keys bound to it, quick turn keys, a
// controller) adds its command.
constexpr u32 kGameToc = 0x01309be0;              // the operator code's TOC (OPDs of 0x6eb7e0, 0x6eee98)
constexpr u32 kPadPort = 372, kPadSticks = 1160;  // pad class 0xe4e860: the port; the stick floats
constexpr u32 kParamStickCurve = 676;             // game parameters: the stick's dead zone edge, half and full deflection
constexpr u32 kOptionsInvertY = 52;               // the options block (r32(TOC - 5000)): vertical inversion
constexpr double kFallbackFullTurn = 0.02;        // radians per step, before the probe knows the AC's (about 70 deg/s)
constexpr double kFallbackFullPitch = 0.02;
constexpr int kProbeFrames = 30;                  // the turn rate probe runs while the mouse moved this recently
constexpr double kInputLayerDeadZone = 0.36;      // the engine input layer's dead zone on each stick axis (measured)
constexpr double kNearFull = 0.995;               // a command just short of full (the mean turn speed still applies)

struct StickCurve {
  double dead = 0, half = 0, full = 0;  // input layer values that give the commands 0, 0.5 and 1 (0x6eb7e0)
  bool known = false;
  bool shaped() const { return dead < half && half < full; }
  double value(double d) const {  // the command for a stick deflection (signed)
    double a = std::max(0.0, (std::fabs(d) - kInputLayerDeadZone) / (1 - kInputLayerDeadZone)), v;
    if (!shaped()) v = std::min(a, 1.0);
    else if (a <= dead) v = 0;
    else if (a <= half) v = (a - dead) / (half - dead) * 0.5;
    else if (a <= full) v = 0.5 + (a - half) / (full - half) * 0.5;
    else v = 1.0;
    return d < 0 ? -v : v;
  }
  double deflection(double c) const {  // the stick deflection for a command (signed; the inverse of value)
    double a = std::min(std::fabs(c), 1.0), d;
    if (a <= 0) return 0;
    if (!shaped()) d = a;
    else if (a <= 0.5) d = dead + a / 0.5 * (half - dead);
    else d = half + (a - 0.5) / 0.5 * (full - half);
    d = std::min(kInputLayerDeadZone + d * (1 - kInputLayerDeadZone), 1.0);
    return c < 0 ? -d : d;
  }
};
StickCurve g_curve;  // main thread
bool g_look_inverted = false;
LARGE_INTEGER g_stick_last{};        // the previous port 0 pad update
int g_stick_recent = 0;              // frames left in which the turn rate probe runs
double g_stick_pending[2] = {0, 0};  // the mouse's turn (right +) and look (up +) the stick has not given yet (radians)
double g_stick_heading = 0;          // the player AC's heading at the previous pad update
u32 g_stick_body = 0;                // and its body
// A turn this much faster than the stick's own top speed is not the stick's (a quick turn).
constexpr double kQuickTurnRate = 1.5;

// The player's action table (operator +80; rt::game::key_assignment): at the operator's turn call
// it is in r28 (0x6eee98, return site 0x6ef030) or r27 (0x6e2198, return site 0x6e22ec).
constexpr u32 kOperatorTurnSiteB = 0x006e22ec;
std::mutex g_keys_mu;
u8 g_keys[rt::game::kSlotCount];
bool g_keys_known = false;

void note_key_table(Ctx& ctx) {
  u32 lr = u32(ctx.lr), table = lr == kPlayerTurnSetSite ? u32(ctx.r[28]) : lr == kOperatorTurnSiteB ? u32(ctx.r[27]) : 0;
  if (!table || !rt::vm::committed(table, rt::game::kSlotCount)) return;
  u8 codes[rt::game::kSlotCount];
  for (u32 i = 0; i < rt::game::kSlotCount; ++i) {
    codes[i] = r8(table + i);
    if (codes[i] > 0x17 && codes[i] < 0xfe) return;  // not an action table
  }
  std::lock_guard lk(g_keys_mu);
  if (!g_keys_known || std::memcmp(codes, g_keys, sizeof codes) != 0) {
    std::string s;
    for (u8 c : codes) s += acfa::strprintf(" %02x", c);
    LOG_I("input", "the game's button assignment (action table 0x%08x):%s", table, s.c_str());
  }
  std::memcpy(g_keys, codes, sizeof codes);
  g_keys_known = true;
}

// Once per frame from the player's operator: the stick curve from the game's parameters (the call
// chain 0x6eb7e0 makes) and the vertical inversion option.
void read_stick_settings(Ctx& ctx) {
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x400) & ~15u;
  w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = kGameToc;
  f_005ef328(ctx);
  u32 params = 0, single = u32(ctx.r[3]);
  if (single && rt::vm::committed(single + 4, 4)) {
    u32 obj = r32(single + 4), vt = obj && rt::vm::committed(obj, 4) ? r32(obj) : 0;
    u32 opd = vt && rt::vm::committed(vt + 28, 4) ? r32(vt + 28) : 0;
    if (opd && rt::vm::committed(opd, 8)) {
      ctx.r[3] = obj;
      ctx.r[2] = r32(opd + 4);
      icall(ctx, r32(opd));
      params = u32(ctx.r[3]);
    }
  }
  ctx = saved;
  if (params && rt::vm::committed(params + kParamStickCurve, 12)) {
    StickCurve c;
    c.dead = rf(params + kParamStickCurve);
    c.half = rf(params + kParamStickCurve + 4);
    c.full = rf(params + kParamStickCurve + 8);
    c.known = true;
    if (!g_curve.known || c.dead != g_curve.dead || c.half != g_curve.half || c.full != g_curve.full)
      LOG_I("input", "stick response (game parameters 0x%08x): dead zone to %.4f, half command at %.4f, full at %.4f%s", params, c.dead, c.half,
            c.full, c.shaped() ? "" : " (not shaped: the deflection as it is)");
    g_curve = c;
  }
  u32 options = r32(kGameToc - 5000);
  g_look_inverted = options && rt::vm::committed(options + kOptionsInvertY, 4) && r32(options + kOptionsInvertY) != 0;
}

double pitch_full_rate() {  // the look's steady pitch rate at a full command (radians per step; 0: not known)
  u32 look = g_look.load(std::memory_order_relaxed);
  if (!look || !rt::vm::committed(look + kPitchState + 24, 8)) return 0;
  double speed = rf(look + kPitchState + 24), drag = rf(look + kPitchState + 28);
  return speed > 0 && drag > 0 ? speed / drag : 0;
}

// The command that gives an angle over the frame (radians; rate: what a command of 1 turns per
// 1/60 s step, steps: the frame's), at most cmax, which turns rmax per step: what is left waits.
struct AxisCommand {
  double command = 0, rest = 0;
};
AxisCommand axis_command(double want, double rate, double steps, double cmax, double rmax) {
  AxisCommand a;
  double c = want / (rate * steps);
  if (std::fabs(c) < cmax) {
    a.command = c;
    return a;
  }
  a.command = std::copysign(cmax, want);
  double rest = want - std::copysign(rmax * steps, want);
  a.rest = rest * want > 0 ? rest : 0;  // a full command may turn a little more than asked: then nothing waits
  return a;
}

// After each port 0 pad update: the frame's mouse movement onto the right stick.
void mouse_stick_frame(u32 pad) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_stick_last.QuadPart ? double(now.QuadPart - g_stick_last.QuadPart) / double(freq.QuadPart) : 0;
  g_stick_last = now;
  if (g_stick_recent > 0) --g_stick_recent;
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  if (m.mode != rt::bindings::MouseMode::Stick) {  // camera mode takes the mouse in the operator
    g_stick_pending[0] = g_stick_pending[1] = 0;
    return;
  }
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  // The AC turned faster than the stick turns it (a quick turn): the turn that waited is done.
  u32 body = player_body();
  double heading = body ? heading_of(body) : 0;
  double top = std::max({g_full[0], g_full[1], kFallbackFullTurn});
  if (body && body == g_stick_body && g_stick_pending[0] != 0 && dt > 0 &&
      std::fabs(wrap(heading - g_stick_heading)) > kQuickTurnRate * top * dt * 60) {
    static int logged = 0;
    if (logged++ < 20)
      LOG_I("input", "mouse stick: the AC turned %.1f deg in %.1f ms (not the stick's turn): %.1f deg left to turn dropped",
            wrap(heading - g_stick_heading) * 180 / kPi, dt * 1000, g_stick_pending[0] * 180 / kPi);
    g_stick_pending[0] = 0;
  }
  g_stick_body = body;
  g_stick_heading = heading;
  if (!dx && !dy && g_stick_pending[0] == 0 && g_stick_pending[1] == 0) return;
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  bool gameplay = rt::game::input_context() == rt::game::InputContext::Gameplay;
  if (dt <= 0 || dt > 0.25 || !gameplay || !g_curve.known || !rt::vm::committed(pad + kPadSticks, 8)) {
    if ((dx || dy) && traced++ < 40)
      LOG_I("input", "mouse stick: %lld, %lld counts dropped (%.0f ms since the previous frame%s%s)", (long long)dx, (long long)dy, dt * 1000,
            gameplay ? "" : ", not in gameplay", g_curve.known ? "" : ", stick response not known yet");
    g_stick_pending[0] = g_stick_pending[1] = 0;  // after a pause or a menu nothing from before turns the AC
    return;
  }
  g_stick_recent = kProbeFrames;
  double steps = dt * 60;
  double yaw = g_stick_pending[0] + double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;  // right +
  double pitch = g_stick_pending[1] + double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;  // up +
  yaw = std::clamp(yaw, -kPi, kPi);  // at most a half turn waits
  u32 look = g_look.load(std::memory_order_relaxed);
  if (look && rt::vm::committed(look + kPitchState, 40)) {  // the look waits only for what its limits leave
    double angle = rf(look + kPitchState), hi = rf(look + kPitchState + 32), lo = rf(look + kPitchState + 36);
    if (lo < hi) pitch = std::clamp(pitch, std::min(0.0, lo - angle), std::max(0.0, hi - angle));
  }
  // The turn step takes an AC's turn speed for a direction only at a full command (+1 or -1) and
  // the mean of both otherwise (0x760490): below full the command scales the mean (mouse turns of
  // 500 counts gave +49.0 and -41.6 degrees when scaled by each side's own rate, as the mean
  // predicts; run 20261020-130211). Where a full command turns slower than the mean (one side of
  // most ACs), the mouse's command stops just short of full, so moving faster never turns slower.
  bool rates = g_full[0] > 0 && g_full[1] > 0;
  double partial = rates ? (g_full[0] + g_full[1]) / 2 : kFallbackFullTurn;
  double full_pitch = pitch_full_rate();
  if (full_pitch <= 0) full_pitch = kFallbackFullPitch;
  int side = yaw < 0 ? 0 : 1;
  bool slow = rates && g_full[side] < partial * kNearFull;
  AxisCommand turn = axis_command(yaw, partial, steps, slow ? kNearFull : 1.0, slow ? partial * kNearFull : rates ? g_full[side] : partial);
  AxisCommand lift = axis_command(pitch, full_pitch, steps, 1.0, full_pitch);
  g_stick_pending[0] = turn.rest;
  g_stick_pending[1] = lift.rest;
  double cx = turn.command, cy = g_look_inverted ? -lift.command : lift.command;
  float bx = rf(pad + kPadSticks), by = rf(pad + kPadSticks + 4);
  double tx = std::clamp(g_curve.value(bx) + cx, -1.0, 1.0), ty = std::clamp(g_curve.value(by) + cy, -1.0, 1.0);
  if (cx != 0) wf(pad + kPadSticks, float(g_curve.deflection(tx)));
  if (cy != 0) wf(pad + kPadSticks + 4, float(g_curve.deflection(ty)));
  if (traced++ < 40)
    LOG_I("input", "mouse stick: %lld, %lld counts over %.1f ms -> commands %.4f, %.4f (rates %.5f, %.5f rad per step; %.4f, %.4f rad wait) -> stick %.4f, %.4f",
          (long long)dx, (long long)dy, dt * 1000, tx, ty, partial, full_pitch, turn.rest, lift.rest, double(rf(pad + kPadSticks)),
          double(rf(pad + kPadSticks + 4)));
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
    note_key_table(ctx);
    read_stick_settings(ctx);
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
  if (g_player_turn.exchange(obj) != obj) {
    LOG_I("input", "mouse: player turn object 0x%08x", obj);
    g_full[0] = g_full[1] = 0;  // another AC: its own turn rate
  }
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
// player's, while the AC chases the mouse camera or the mouse moves the stick (and once before):
// the stick's full turning speed each way (the scale of the chase and of the mouse stick; kept in
// between).
void hook_ac_turn_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]);
  orig(ctx);
  bool stick = rt::bindings::mouse().mode == rt::bindings::MouseMode::Stick && (g_stick_recent > 0 || g_full[1] <= 0);
  if (!((g_cam.on && !g_cam.attached) || stick)) return;
  if (!obj || obj != g_player_turn.load(std::memory_order_relaxed) || !rt::vm::committed(obj, 192)) return;
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
// mouse_camera_follow): the mouse camera's heading and turn reach it at once.
void hook_camera_follow(Ctx& ctx, void (*orig)(Ctx&)) {
  WantedTurn turn = mouse_camera_follow(ctx);
  orig(ctx);
  for (u32 k = 0; turn.want && k < 2; ++k) {
    w32(turn.want + 16 * k, turn.saved[2 * k]);
    w32(turn.want + 16 * k + 8, turn.saved[2 * k + 1]);
  }
}

// The pad class the game uses on port 0 (0xe4e860; r3 1 when it read the pad): the mouse as the
// right stick (mouse_stick_frame).
void hook_pad_update_b(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  if (u32(ctx.r[3]) == 1 && r32(pad + kPadPort) == 0) mouse_stick_frame(pad);
}
}  // namespace ppu

namespace rt::game {
bool key_assignment(u8 codes[kSlotCount]) {
  std::lock_guard lk(ppu::g_keys_mu);
  if (!ppu::g_keys_known) return false;
  std::memcpy(codes, ppu::g_keys, kSlotCount);
  return true;
}
}  // namespace rt::game

