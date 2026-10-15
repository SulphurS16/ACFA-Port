// Mouse in the game's own menus (GAME_NOTES "Menu dialogs"). The game's menus are dialogs of
// objects (buttons, list rows, sliders) laid out in the console's 1280x720 picture. Each scene's
// dialog managers hold a stack of dialogs; a key message (type 384, codes 0x5810-0x5823) goes to
// the top dialog of the first manager whose top dialog takes input, then to that dialog's focused
// object. Every frame, before the game turns pad presses into key messages (0x8ad48), a probe
// message with a code the game does not use goes through the game's own routing; the manager
// dispatcher hooks (0x2d9248 for scenes, 0x2d9008 for system dialogs) stop it at the manager
// that would take a key and note that manager's top dialog. The cursor then focuses the item
// under it through the dialog's own SetFocus (the D-pad's path, with its highlight, help line and
// notifications); a click presses Cross after focusing what was clicked, a right click Circle,
// the wheel the D-pad. A click or drag on a slider's bar moves the slider with the D-pad (held
// while far, single steps near) until its value matches the cursor. In a list the row under the
// cursor is selected with the list's own move routine (and its notification), as far as that
// does not scroll the list; a click on a row selects and chooses it. Nothing here changes the
// game's state other than through those calls and presses.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/game/menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/game/frame_policy.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ui/overlay.h"

void f_003e71a0(ppu::Ctx&);  // menu object: tell its parent dialog (r3 object, r4 code)

namespace rt::game {
namespace {

using ppu::Ctx;

constexpr u32 kKeyMessage = 384;
constexpr u32 kProbeCode = 0x58ff;  // no key of the game (its codes: 0x5810-0x5823 and a few more)

// Guest layout (GAME_NOTES "Menu dialogs").
constexpr u32 kFrameworkDispatch = 24;  // framework vtable: dispatch(framework, message)
constexpr u32 kMgrStack = 4, kMgrDepth = 132, kMgrTable = 268, kMgrTableCount = 524;
constexpr u32 kDlgBusy = 72;      // dialog vtable: nonzero while it takes no input
constexpr u32 kDlgSetFocus = 48;  // dialog vtable: SetFocus(dialog, object)
constexpr u32 kDlgChildren = 24, kDlgChildCount = 28, kDlgFocus = 128;
constexpr u32 kObjId = 4;          // -1: an unnamed part
constexpr u32 kObjRect = 52;       // object vtable: rect (left, top, right, bottom) by hidden pointer
constexpr u32 kObjFocusable = 56;  // object vtable: focusable
// CMenuSliderCtrl (vtable 0x120dff8): min, max, value, step (ints), the bar (floats l, t, r, b).
constexpr u32 kSliderVtable = 0x0120dff8;
constexpr u32 kSliderMin = 0x34, kSliderMax = 0x38, kSliderValue = 0x3c, kSliderStep = 0x40, kSliderBar = 0x44;
// The list control (vtable 0x120dd08, FREE PLAY's missions and others): column +68, selected row
// +104, rows on a page +108, rows +112, first row shown +116; vtable slot 21 moves the selection
// (out, list, current {column, row}, delta), then the list's parent is told (0x3e71a0, code 5).
constexpr u32 kListVtable = 0x0120dd08;
constexpr u32 kListCol = 68, kListRow = 104, kListPage = 108, kListCount = 112, kListTop = 116, kListMove = 84;

// Pad bits (cellPad digital words).
constexpr u16 kStart = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u16 kCircle = 0x20, kCross = 0x40;

s64 now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<int> g_context{int(InputContext::Screen)};
std::atomic<s64> g_mouse_at{0}, g_pad_at{0};  // last use of each (ms)
std::atomic<bool> g_cursor{false};             // menu cursor shown (window thread reads)

// Mouse events from the window thread, handled on the game's main thread at its next frame.
struct Event {
  enum Kind { LeftDown, LeftUp, Right, Wheel } kind;
  int value;  // wheel notches
};
std::mutex g_mu;
std::deque<Event> g_events;
// Presses made for the mouse: active from `at` for 70 ms (two game frames at least).
struct Press {
  u16 d1, d2;
  s64 at;
};
std::deque<Press> g_presses;
std::atomic<u16> g_hold{0};  // a direction held for a slider far from the cursor's value

// Main thread only.
bool g_probing = false;
u32 g_probe_dialog = 0;
u32 g_last_dialog = 0;
int g_last_x = -1, g_last_y = -1;
struct Drag {
  u32 slider = 0;  // dragged slider (0: none)
  bool button = false;
  float target = 0;  // the value at the cursor
  s64 next_tap = 0;
};
Drag g_drag;

bool valid(u32 a, u32 size = 4) { return a >= 0x10000 && !(a & 3) && vm::committed(a, size); }

// Calls the guest function behind descriptor `opd` with r3, r4 on a frame below the current stack
// pointer (leaving the 0x100 bytes under it to the caller); every register is restored after.
u64 call(Ctx& ctx, u32 opd, u64 r3, u64 r4 = 0, u64 r5 = 0, u64 r6 = 0) {
  if (!valid(opd, 8)) return 0;
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x300) & ~15u;
  ppu::w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = ppu::r32(opd + 4);
  ctx.r[3] = r3;
  ctx.r[4] = r4;
  ctx.r[5] = r5;
  ctx.r[6] = r6;
  ppu::icall(ctx, ppu::r32(opd));
  u64 ret = ctx.r[3];
  ctx = saved;
  return ret;
}
// The same for a recompiled function called directly (its own TOC handling).
void call_fn(Ctx& ctx, void (*fn)(Ctx&), u64 r3, u64 r4) {
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x300) & ~15u;
  ppu::w64(sp, ctx.r[1]);
  ctx.r[1] = sp;
  ctx.r[3] = r3;
  ctx.r[4] = r4;
  fn(ctx);
  ctx = saved;
}
u32 vfunc(u32 obj, u32 slot) {
  if (!valid(obj)) return 0;
  u32 vt = ppu::r32(obj);
  return valid(vt + slot) ? ppu::r32(vt + slot) : 0;
}

// The probe reached a manager: the dialog that a key would reach, as 0x2d9248 decides it.
bool probe_manager(Ctx& ctx, u32 mgr) {
  if (!valid(mgr) || !valid(mgr + kMgrTableCount)) return false;
  u32 depth = ppu::r32(mgr + kMgrDepth);
  if (depth == 0 || depth > 32) return false;
  u32 top = ppu::r32(mgr + kMgrStack + 4 * (depth - 1));
  u32 n = ppu::r32(mgr + kMgrTableCount);
  bool enabled = false, listed = false;
  for (u32 i = 0; i < n && i < 32 && !listed; ++i) {
    u32 e = mgr + kMgrTable + 8 * i;
    if (ppu::r32(e + 4) == top) {
      listed = true;
      enabled = ppu::r32(e) == 1;
    }
  }
  if (!listed || !enabled) return false;
  if (call(ctx, vfunc(top, kDlgBusy), top) != 0) return false;
  g_probe_dialog = top;
  return true;
}

void dispatch_hook(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 msg = u32(ctx.r[4]);
  if (!g_probing || !valid(msg, 8) || ppu::r32(msg) != kKeyMessage || ppu::r32(msg + 4) != kProbeCode) {
    orig(ctx);
    return;
  }
  ctx.r[3] = probe_manager(ctx, u32(ctx.r[3])) ? 1 : 0;  // 1: the routing stops here
}

// Client pixels -> the game's 1280x720 picture (the frame is fitted to the window, centred).
bool to_game(int cx, int cy, float* gx, float* gy) {
  int w = 0, h = 0;
  gpu::window_client_size(&w, &h);
  if (w <= 0 || h <= 0) return false;
  float s = std::min(float(w) / 1280.0f, float(h) / 720.0f);
  float x0 = (float(w) - 1280.0f * s) / 2, y0 = (float(h) - 720.0f * s) / 2;
  *gx = (float(cx) - x0) / s;
  *gy = (float(cy) - y0) / s;
  return true;
}

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
  bool contains(float x, float y) const { return r > l && b > t && x >= l && x < r && y >= t && y < b; }
};
Rect rect_of(Ctx& ctx, u32 obj) {
  u32 out = (u32(ctx.r[1]) - 0x40) & ~15u;  // inside the 0x100 bytes call() leaves free
  for (int k = 0; k < 4; ++k) ppu::w32(out + 4 * k, 0);
  call(ctx, vfunc(obj, kObjRect), out, obj);
  return {float(ppu::rf32(out)), float(ppu::rf32(out + 4)), float(ppu::rf32(out + 8)), float(ppu::rf32(out + 12))};
}
bool is_slider(u32 obj) { return valid(obj + kSliderBar + 16) && ppu::r32(obj) == kSliderVtable; }
Rect slider_bar(u32 s) {
  Rect r{float(ppu::rf32(s + kSliderBar)), float(ppu::rf32(s + kSliderBar + 4)), float(ppu::rf32(s + kSliderBar + 8)),
         float(ppu::rf32(s + kSliderBar + 12))};
  bool sane = r.r > r.l && r.b >= r.t && r.l >= -1 && r.r <= 1281 && r.t >= -1 && r.b <= 721;
  return sane ? r : Rect{};
}

// The item of `dialog` under (gx, gy): named items (an ID: what the D-pad moves between) before
// unnamed parts, items before slider bars (a row's slider moves with the row), then the smallest.
// `slider`: a slider whose bar is under the point, if any (the item's own or a part of its row).
u32 object_at(Ctx& ctx, u32 dialog, float gx, float gy, u32* slider) {
  *slider = 0;
  u32 arr = ppu::r32(dialog + kDlgChildren), n = ppu::r32(dialog + kDlgChildCount);
  if (n == 0 || n > 1024 || !valid(arr, 4 * n)) return 0;
  u32 best = 0;
  int best_rank = -1;
  float best_area = 0;
  for (u32 i = 0; i < n; ++i) {
    u32 obj = ppu::r32(arr + 4 * i);
    if (!valid(obj, 16)) continue;
    bool slider_obj = is_slider(obj);
    if (slider_obj) {  // the whole bar (its rect is only the filled part), with a margin for the cursor
      Rect bar = slider_bar(obj);
      if (bar.r > bar.l && gx >= bar.l - 8 && gx < bar.r + 8 && gy >= bar.t - 12 && gy < bar.b + 12) *slider = obj;
    }
    if (call(ctx, vfunc(obj, kObjFocusable), obj) == 0) continue;
    Rect r = rect_of(ctx, obj);
    if (!r.contains(gx, gy)) continue;
    int rank = (ppu::r32(obj + kObjId) != 0xffffffffu ? 2 : 0) + (slider_obj ? 0 : 1);
    float area = (r.r - r.l) * (r.b - r.t);
    if (rank > best_rank || (rank == best_rank && area < best_area)) {
      best = obj;
      best_rank = rank;
      best_area = area;
    }
  }
  return best;
}

bool is_list(u32 obj) { return valid(obj + kListTop + 4) && ppu::r32(obj) == kListVtable; }

// The list row under (gx, gy) of `list` with rect `r`, or -1; *safe: selecting it does not scroll
// the list. Wherever it can, the game keeps the selection on the middle row of the page (first
// row shown = selection - page / 2, within the list: FREE PLAY, run 20261015-100154).
s32 list_row_at(u32 list, const Rect& r, float gy, bool* safe) {
  s32 page = s32(ppu::r32(list + kListPage)), count = s32(ppu::r32(list + kListCount)), top = s32(ppu::r32(list + kListTop));
  if (page <= 0 || page > 64 || count <= 0 || top < 0 || !(r.b > r.t)) return -1;
  s32 visible = s32((gy - r.t) / ((r.b - r.t) / float(page)));
  if (visible < 0 || visible >= page || top + visible >= count) return -1;
  s32 target = top + visible;
  *safe = std::clamp(target - page / 2, 0, std::max(0, count - page)) == top;
  return target;
}

// Selects row `target` of `list` as its key handler does (the list's own move, then the parent's
// notification). Returns whether the list scrolled.
bool list_select(Ctx& ctx, u32 list, s32 target) {
  s32 row = s32(ppu::r32(list + kListRow)), col = s32(ppu::r32(list + kListCol)), top = s32(ppu::r32(list + kListTop));
  if (target == row) return false;
  u32 m = (u32(ctx.r[1]) - 0x60) & ~15u;  // {current}, {delta}, {out}: inside the 0x100 bytes call() leaves free
  ppu::w32(m, u32(col));
  ppu::w32(m + 4, u32(row));
  ppu::w32(m + 8, 0);
  ppu::w32(m + 12, u32(target - row));
  ppu::w32(m + 16, u32(col));
  ppu::w32(m + 20, u32(row));
  call(ctx, vfunc(list, kListMove), m + 16, list, m, m + 8);
  if (s32(ppu::r32(m + 16)) != col || s32(ppu::r32(m + 20)) != row) call_fn(ctx, f_003e71a0, list, 5);
  return s32(ppu::r32(list + kListTop)) != top;
}

void press(u16 d1, u16 d2, s64 at) {
  std::lock_guard lk(g_mu);
  g_presses.push_back({d1, d2, at});
}

// The dragged slider follows the cursor: the D-pad held toward the target while it is ten steps
// away or more (the game speeds up a held key), single presses nearer; done within half a step.
void drive_slider(float gx) {
  u32 s = g_drag.slider;
  Rect bar = slider_bar(s);
  s32 lo = s32(ppu::r32(s + kSliderMin)), hi = s32(ppu::r32(s + kSliderMax)), v = s32(ppu::r32(s + kSliderValue));
  s32 step = std::max<s32>(1, s32(ppu::r32(s + kSliderStep)));
  if (!(bar.r > bar.l) || hi <= lo) {
    g_drag = {};
    g_hold = 0;
    return;
  }
  if (g_drag.button) g_drag.target = float(lo) + std::clamp((gx - bar.l) / (bar.r - bar.l), 0.0f, 1.0f) * float(hi - lo);
  float delta = g_drag.target - float(v);
  u16 dir = delta > 0 ? kRight : kLeft;
  if (std::fabs(delta) <= float(step) / 2) {
    g_hold = 0;
    if (!g_drag.button) g_drag = {};  // released and there
    return;
  }
  if (std::fabs(delta) >= 10.0f * float(step)) {
    g_hold = dir;
    return;
  }
  g_hold = 0;
  s64 t = now_ms();
  if (t >= g_drag.next_tap) {
    press(dir, 0, t);
    g_drag.next_tap = t + 110;  // a press and a release frame or two
  }
}

// One frame of the menu mouse on the game's main thread (before the pads become key messages).
void frame(Ctx& ctx) {
  u32 framework = u32(ctx.r[3]);
  if (ui::is_open() || !valid(framework)) return;
  // Probe: which dialog takes keys now.
  u32 msg = (u32(ctx.r[1]) - 0x80) & ~15u;
  ppu::w32(msg, kKeyMessage);
  ppu::w32(msg + 4, kProbeCode);
  g_probe_dialog = 0;
  g_probing = true;
  call(ctx, vfunc(framework, kFrameworkDispatch), framework, msg);
  g_probing = false;
  u32 dialog = g_probe_dialog;
  bool menu = dialog && valid(dialog + kDlgFocus) && ppu::r32(dialog + kDlgFocus) != 0;
  InputContext c = menu ? InputContext::Menu : mission_running() ? InputContext::Gameplay : InputContext::Screen;
  InputContext was = InputContext(g_context.exchange(int(c)));
  bool mouse = g_mouse_at.load() > g_pad_at.load();
  bool focus = input_has_focus();
  bool show = c == InputContext::Menu && mouse && focus;
  if (c != was || show != g_cursor.load()) {
    g_cursor = show;
    if (c != was) LOG_I("input", "input context %s", c == InputContext::Menu ? "menu" : c == InputContext::Gameplay ? "gameplay" : "screen");
    // Mouse look takes the mouse in gameplay; menus give it back.
    if (c == InputContext::Menu && was == InputContext::Gameplay) gpu::window_request_mouse_capture(false);
    else if (c == InputContext::Gameplay && mouse && !input_isolated()) gpu::window_request_mouse_capture(true);
    gpu::window_refresh_cursor();
  }
  std::deque<Event> events;
  {
    std::lock_guard lk(g_mu);
    events.swap(g_events);
  }
  if (c != InputContext::Menu || dialog != g_last_dialog) {  // the dragged slider left with its dialog
    g_drag = {};
    g_hold = 0;
  }
  if (c == InputContext::Screen) {  // title, movies: a click is Start
    for (const Event& e : events)
      if (e.kind == Event::LeftDown) press(kStart, 0, now_ms());
    g_last_dialog = dialog;
    return;
  }
  if (c != InputContext::Menu) {
    g_last_dialog = dialog;
    return;
  }
  // Hover: the item under the cursor takes the focus when the cursor moved or was clicked.
  POINT p{};
  float gx = 0, gy = 0;
  bool have = false;
  HWND hwnd = static_cast<HWND>(gpu::window_handle());
  if (!have && (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !to_game(p.x, p.y, &gx, &gy))) return;
  bool moved = p.x != g_last_x || p.y != g_last_y || dialog != g_last_dialog;
  g_last_x = p.x;
  g_last_y = p.y;
  g_last_dialog = dialog;
  bool down = std::any_of(events.begin(), events.end(), [](const Event& e) { return e.kind == Event::LeftDown; });
  u32 hit = 0, slider = 0;
  s32 row = -1;  // a list's row under the cursor
  bool row_safe = false;
  if (show && (moved || down) && !(g_drag.slider && g_drag.button)) {
    hit = object_at(ctx, dialog, gx, gy, &slider);
    if (hit && hit != ppu::r32(dialog + kDlgFocus)) {
      call(ctx, vfunc(dialog, kDlgSetFocus), dialog, hit);
    }
    if (hit && is_list(hit)) {
      row = list_row_at(hit, rect_of(ctx, hit), gy, &row_safe);
      // Hover selects rows that do not scroll the list; after a scroll the cursor has to move a
      // row's height before hover selects again (no runaway scrolling under a resting cursor).
      static float scrolled_y = -1e9f;
      if (row >= 0 && row_safe && !down && std::fabs(gy - scrolled_y) > 40 && list_select(ctx, hit, row)) scrolled_y = gy;
    }
  }
  s64 t = now_ms();
  for (const Event& e : events) {
    if (e.kind == Event::LeftDown) {
      if (slider) {  // on a slider's bar: the slider follows the cursor while the button is down
        g_drag = {slider, true, 0, 0};
      } else if (hit) {
        if (row >= 0) list_select(ctx, hit, row);  // a list's row: selected, then chosen
        press(0, kCross, t + (row >= 0 ? 50 : 0));  // a click on an item chooses it; elsewhere nothing
      }
    } else if (e.kind == Event::LeftUp) {
      g_drag.button = false;
    } else if (e.kind == Event::Right) {
      press(0, kCircle, t);
    } else {
      for (int i = 0; i < std::abs(e.value); ++i, t += 90) press(e.value > 0 ? kUp : kDown, 0, t);
    }
  }
  if (g_drag.slider) drive_slider(gx);
}

}  // namespace

InputContext input_context() { return InputContext(g_context.load(std::memory_order_relaxed)); }

void menu_mouse_moved() { g_mouse_at = now_ms(); }

void menu_mouse_button(int button, bool down) {
  g_mouse_at = now_ms();
  if (button != 0 && button != 1) return;
  if (button == 1 && !down) return;
  std::lock_guard lk(g_mu);
  g_events.push_back({button == 1 ? Event::Right : down ? Event::LeftDown : Event::LeftUp, 0});
}

void menu_mouse_wheel(int notches) {
  g_mouse_at = now_ms();
  if (!notches) return;
  std::lock_guard lk(g_mu);
  g_events.push_back({Event::Wheel, notches});
}

bool menu_cursor_visible() { return g_cursor.load(std::memory_order_relaxed); }

void note_pad_input() {
  s64 t = now_ms();
  if (g_pad_at.exchange(t) < g_mouse_at.load() && g_cursor.load()) {
    g_cursor = false;  // the controller took over: the cursor hides at once
    gpu::window_refresh_cursor();
  }
}

void menu_pad_bits(u16* d1, u16* d2) {
  s64 t = now_ms();
  *d1 |= g_hold.load(std::memory_order_relaxed);
  std::lock_guard lk(g_mu);
  while (!g_presses.empty() && g_presses.front().at + 70 <= t) g_presses.pop_front();
  for (const Press& p : g_presses) {
    if (p.at > t) continue;
    *d1 |= p.d1;
    *d2 |= p.d2;
  }
}

}  // namespace rt::game

namespace ppu {

// 0x8ad48, every frame on the main thread: the menu mouse first, then the pads become messages.
void hook_menu_frame(Ctx& ctx, void (*orig)(Ctx&)) {
  rt::game::frame(ctx);
  orig(ctx);
}
// 0x2d9248 / 0x2d9008: a key message to a dialog manager (scenes' / system dialogs').
void hook_menu_dispatch(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::dispatch_hook(ctx, orig); }
void hook_menu_dispatch_sys(Ctx& ctx, void (*orig)(Ctx&)) { rt::game::dispatch_hook(ctx, orig); }

}  // namespace ppu
