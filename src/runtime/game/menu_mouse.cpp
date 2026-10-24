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
// while far, single steps near) until its value matches the cursor. In a list the item under the
// cursor is reached with D-pad presses, as a player reaches it (hover: only items whose selection
// does not scroll the list); a click on an item steps to it and chooses it, on a strip of slots
// or categories (ASSEMBLE, TUNE) only selects it. A click on an arrow (◀ ▶ of a choice row or a
// strip, ▲ ▼ of a list) presses its direction. Dialogs whose scene moves their lists itself keep
// no focused object (TUNE): the cursor leaves their focus alone. Nothing here changes the game's
// state other than through those calls and presses.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/game/menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/game/frame_policy.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

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
// The dialog's rect {left, top, right, bottom} in the 1280x720 picture (floats; SetRect 0x3dd0b0,
// MoveTo 0x3de918). Its objects' rects are relative to its top left: full-screen dialogs sit at
// 0, 0; message boxes (vtable 0x1211410: "Restarting this tutorial.", "Enter pilot name.") are
// centred (run 20261016-025405).
constexpr u32 kDlgRect = 136;
constexpr u32 kObjId = 4;          // -1: an unnamed part
constexpr u32 kObjRect = 52;       // object vtable: rect (left, top, right, bottom) by hidden pointer
constexpr u32 kObjFocusable = 56;  // object vtable: focusable
// CMenuSliderCtrl (vtable 0x120dff8): min, max, value, step (ints), the bar (floats l, t, r, b).
constexpr u32 kSliderVtable = 0x0120dff8;
constexpr u32 kSliderMin = 0x34, kSliderMax = 0x38, kSliderValue = 0x3c, kSliderStep = 0x40, kSliderBar = 0x44;
// The list control (vtable 0x120dd08, FREE PLAY's missions, ASSEMBLE's parts and others):
// selected column +68, columns on a page +72, columns +76, first column shown +80; selected row
// +104, rows on a page +108, rows +112, first row shown +116.
constexpr u32 kListVtable = 0x0120dd08;
constexpr u32 kListCol = 68, kListColPage = 72, kListColCount = 76, kListColTop = 80;
constexpr u32 kListRow = 104, kListPage = 108, kListCount = 112, kListTop = 116;

// Pad bits (cellPad digital words).
constexpr u16 kStart = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u16 kCircle = 0x20, kCross = 0x40;

s64 now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<int> g_context{int(InputContext::Screen)};
std::atomic<s64> g_mouse_at{0}, g_pad_at{0};  // last use of each (ms)
std::atomic<s64> g_key_at{0};                 // last key pressed in a menu (hides the cursor, not mouse look)
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
  bool screen_only;  // dropped when due unless the screen still has no menu and is not gameplay
};
std::deque<Press> g_presses;
std::atomic<u16> g_hold{0};  // a direction held for a slider far from the cursor's value

// Main thread only.
bool g_probing = false;
u32 g_probe_dialog = 0;
u32 g_last_dialog = 0;
// A menu that takes no keys for a moment stays a menu: CLIENT SELECT routes keys to no dialog for
// two quick frames (about 7 ms) after each focus change (run 20261016-035810), which hid the
// cursor for that moment. For kMenuGraceMs after a menu last took keys the context stays a menu
// (the cursor stays, capture unchanged) and clicks wait for the menu; when no menu comes back
// they are dropped (not turned into START presses as on screens without a menu).
constexpr s64 kMenuGraceMs = 150;
s64 g_menu_seen = 0;
std::deque<Event> g_waiting;
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
// The top left of `dialog` in the picture (0, 0 when it is not a sane rect).
void dialog_origin(u32 dialog, float* x, float* y) {
  *x = *y = 0;
  if (!valid(dialog + kDlgRect, 16)) return;
  float l = ppu::rf32(dialog + kDlgRect), t = ppu::rf32(dialog + kDlgRect + 4);
  float r = ppu::rf32(dialog + kDlgRect + 8), b = ppu::rf32(dialog + kDlgRect + 12);
  if (!(l >= 0 && t >= 0 && r > l && b > t && r <= 1281 && b <= 721)) return;
  *x = l;
  *y = t;
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

// The list control or a class derived from it (DESIGN's schematics and tabs: their own vtables
// with the list's key handler, slot 10, and move routine, slot 21; run 20261024-185045).
bool is_list(u32 obj) {
  if (!valid(obj + kListTop + 4)) return false;
  u32 vt = ppu::r32(obj);
  if (vt == kListVtable) return true;
  return valid(vt + 88) && ppu::r32(vt + 40) == ppu::r32(kListVtable + 40) && ppu::r32(vt + 84) == ppu::r32(kListVtable + 84);
}
// A derived strip whose selection is kept elsewhere: DESIGN's tabs did not change their column
// when the D-pad changed the tab (run 20261024-185045), so the cursor cannot tell how far to step
// and leaves them alone (a click there pressed Cross on the selected schematic).
bool untracked_strip(u32 obj) {
  return is_list(obj) && ppu::r32(obj) != kListVtable && s32(ppu::r32(obj + kListColPage)) > 1 && s32(ppu::r32(obj + kListPage)) <= 1;
}

// Whether `dialog` holds a list the player can move (checked again every half second). A dialog
// that takes keys but keeps no focused object is still a menu then: TUNE's scene moves its lists
// itself (run 20261024-182618).
bool has_list(Ctx& ctx, u32 dialog) {
  static u32 last = 0;
  static bool result = false;
  static s64 at = 0;
  s64 t = now_ms();
  if (dialog == last && t - at < 500) return result;
  last = dialog;
  at = t;
  result = false;
  u32 arr = ppu::r32(dialog + kDlgChildren), n = ppu::r32(dialog + kDlgChildCount);
  if (n == 0 || n > 1024 || !valid(arr, 4 * n)) return false;
  for (u32 i = 0; i < n && !result; ++i) {
    u32 obj = ppu::r32(arr + 4 * i);
    result = is_list(obj) && call(ctx, vfunc(obj, kObjFocusable), obj) != 0;
  }
  return result;
}

// A list's item under the cursor: the D-pad steps from the selection to it.
struct ListItem {
  bool strip = false;      // one row of several columns (ASSEMBLE's slots, TUNE's categories)
  s32 dcol = 0, drow = 0;  // steps right / down (negative: left / up)
  bool safe = false;       // selecting it does not scroll the list
};

// The item of `list` (rect `r`) under (x, y), if any. Wherever it can, a list keeps the selection
// on the middle row of the page (first row shown = selection - page / 2, within the list: FREE
// PLAY, run 20261015-100154). The strips keep the selection first on the page and wrap around
// (ASSEMBLE, TUNE: run 20261024-182618), so every move scrolls them.
bool list_item_at(u32 list, const Rect& r, float x, float y, ListItem* it) {
  struct Axis {
    s32 sel, page, count, top;
  };
  const Axis axes[2] = {
      {s32(ppu::r32(list + kListCol)), s32(ppu::r32(list + kListColPage)), s32(ppu::r32(list + kListColCount)), s32(ppu::r32(list + kListColTop))},
      {s32(ppu::r32(list + kListRow)), s32(ppu::r32(list + kListPage)), s32(ppu::r32(list + kListCount)), s32(ppu::r32(list + kListTop))}};
  if (!(r.r > r.l && r.b > r.t)) return false;
  it->strip = axes[0].page > 1 && axes[1].page <= 1;
  const float pos[2] = {(x - r.l) / (r.r - r.l), (y - r.t) / (r.b - r.t)};  // 0-1 across the list
  s32 steps[2] = {0, 0};
  bool scrolls = false;
  for (int k = 0; k < 2; ++k) {
    const Axis& a = axes[k];
    if (a.page <= 1) continue;  // the list has one column (or row) on a page
    if (a.page > 64 || a.count <= 0 || a.count > 4096 || a.top < 0 || a.top >= a.count) return false;
    s32 visible = s32(std::floor(pos[k] * float(a.page)));
    if (visible < 0 || visible >= a.page) return false;
    s32 target = a.top + visible;
    if (target >= a.count) {
      if (!it->strip || a.top + a.page <= a.count) return false;  // past the last item
      target -= a.count;
    }
    steps[k] = target - a.sel;
    if (it->strip) {  // the shorter way around
      if (steps[k] > a.count / 2) steps[k] -= a.count;
      if (steps[k] < -(a.count / 2)) steps[k] += a.count;
    }
    scrolls = scrolls || std::clamp(target - a.page / 2, 0, std::max(0, a.count - a.page)) != a.top;
  }
  it->dcol = steps[0];
  it->drow = steps[1];
  it->safe = !it->strip && !scrolls;
  return true;
}

void press(u16 d1, u16 d2, s64 at, bool screen_only = false) {
  std::lock_guard lk(g_mu);
  g_presses.push_back({d1, d2, at, screen_only});
}

// The arrow under (x, y), if any: a small unnamed part (6-16 x 8-24) that is one of a pair, the
// ◀ ▶ of a choice row (COCKPIT COLOR, RADAR TYPE) or of a strip (ASSEMBLE, TUNE) or the ▲ ▼ of a
// list. Returns the D-pad direction it stands for (0: none) and its centre. Choice rows' arrows
// also line up with the next row's: a partner side by side wins.
u16 arrow_at(Ctx& ctx, u32 dialog, float x, float y, float* cx, float* cy) {
  u32 arr = ppu::r32(dialog + kDlgChildren), n = ppu::r32(dialog + kDlgChildCount);
  if (n == 0 || n > 1024 || !valid(arr, 4 * n)) return 0;
  std::vector<Rect> parts;
  int at = -1;
  for (u32 i = 0; i < n; ++i) {
    u32 obj = ppu::r32(arr + 4 * i);
    if (!valid(obj, 16) || is_list(obj) || is_slider(obj)) continue;
    s32 id = s32(ppu::r32(obj + kObjId));
    if ((id != 0 && id != -1) || call(ctx, vfunc(obj, kObjFocusable), obj) != 0) continue;
    Rect r = rect_of(ctx, obj);
    float w = r.r - r.l, h = r.b - r.t;
    if (w < 6 || w > 16 || h < 8 || h > 24) continue;
    if (x >= r.l - 4 && x < r.r + 4 && y >= r.t - 4 && y < r.b + 4) at = int(parts.size());
    parts.push_back(r);
  }
  if (at < 0) return 0;
  const Rect& a = parts[size_t(at)];
  *cx = (a.l + a.r) / 2;
  *cy = (a.t + a.b) / 2;
  float side = 0, stack = 0;  // the nearest partner side by side / above or below (signed distance)
  for (size_t i = 0; i < parts.size(); ++i) {
    float px = (parts[i].l + parts[i].r) / 2 - *cx, py = (parts[i].t + parts[i].b) / 2 - *cy;
    if (int(i) == at) continue;
    if (std::fabs(py) < 3 && std::fabs(px) > 20 && (side == 0 || std::fabs(px) < std::fabs(side))) side = px;
    if (std::fabs(px) < 3 && std::fabs(py) > 20 && (stack == 0 || std::fabs(py) < std::fabs(stack))) stack = py;
  }
  if (side != 0) return side > 0 ? kLeft : kRight;
  if (stack != 0) return stack > 0 ? kUp : kDown;
  return 0;
}

// D-pad steps for a list under way until g_list_busy (ms; main thread). A list moves by D-pad
// presses, as a player moves it: ASSEMBLE's scene keeps its lists' selection itself, so the
// list's own move routine did not hold there (run 20261024-182618).
s64 g_list_busy = 0;
u32 g_step_list = 0;   // the list a hover stepped, and its first row shown before
s32 g_step_top = 0;
float g_scrolled_y = -1e9f;  // where a hover's steps scrolled the list: the cursor moves on first

// Presses the D-pad toward `it` from `t`, 90 ms apart. Returns when the last step has been taken.
s64 list_step(const ListItem& it, s64 t) {
  if (!it.dcol && !it.drow) return t;
  auto run = [&](s32 n, u16 forward, u16 back) {
    for (s32 i = 0; i < std::abs(n); ++i, t += 90) press(n > 0 ? forward : back, 0, t);
  };
  run(it.drow, kDown, kUp);
  run(it.dcol, kRight, kLeft);
  g_list_busy = t + 60;
  return g_list_busy;
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
  if (ui::is_open() || sysdlg::shown() || !valid(framework)) return;
  // Probe: which dialog takes keys now.
  u32 msg = (u32(ctx.r[1]) - 0x80) & ~15u;
  ppu::w32(msg, kKeyMessage);
  ppu::w32(msg + 4, kProbeCode);
  g_probe_dialog = 0;
  g_probing = true;
  call(ctx, vfunc(framework, kFrameworkDispatch), framework, msg);
  g_probing = false;
  u32 dialog = g_probe_dialog;
  // A dialog that takes keys is a menu when an object has its focus or, where its scene moves the
  // selection itself and the dialog keeps none (TUNE), when it holds a list.
  u32 dlg_focus = dialog && valid(dialog + kDlgFocus) ? ppu::r32(dialog + kDlgFocus) : 0;
  bool menu = dialog && valid(dialog + kDlgFocus) && (dlg_focus != 0 || has_list(ctx, dialog));
  s64 now = now_ms();
  if (menu) g_menu_seen = now;
  bool grace = !menu && InputContext(g_context.load()) == InputContext::Menu && now - g_menu_seen < kMenuGraceMs;
  InputContext c = menu || grace ? InputContext::Menu : mission_running() ? InputContext::Gameplay : InputContext::Screen;
  InputContext was = InputContext(g_context.exchange(int(c)));
  bool mouse = g_mouse_at.load() > std::max(g_pad_at.load(), g_key_at.load());
  bool focus = input_has_focus();
  bool show = c == InputContext::Menu && mouse && focus;
  if (bool before = g_cursor.load(); c != was || show != before) {
    g_cursor = show;
    if (c != was) LOG_I("input", "input context %s", c == InputContext::Menu ? "menu" : c == InputContext::Gameplay ? "gameplay" : "screen");
    // Mouse look takes the mouse in gameplay; everything else gives it back (menus, the mission's
    // report, movies, loading).
    if (c == InputContext::Gameplay && was != InputContext::Gameplay) gpu::window_request_mouse_capture(true);
    else if (c != InputContext::Gameplay && was == InputContext::Gameplay) gpu::window_request_mouse_capture(false);
    gpu::window_refresh_cursor();
  }
  std::deque<Event> events;
  {
    std::lock_guard lk(g_mu);
    events.swap(g_events);
  }
  if (grace) {  // no menu takes keys this moment: its clicks wait for it
    g_waiting.insert(g_waiting.end(), events.begin(), events.end());
    return;
  }
  if (c == InputContext::Menu) events.insert(events.begin(), g_waiting.begin(), g_waiting.end());
  g_waiting.clear();
  if (c != InputContext::Menu || dialog != g_last_dialog) {  // the dragged slider left with its dialog
    g_drag = {};
    g_hold = 0;
  }
  if (c == InputContext::Screen) {  // title, movies, the mission report: a click is Start, then Cross
    for (const Event& e : events)
      if (e.kind == Event::LeftDown) {
        s64 t = now_ms();
        press(kStart, 0, t);
        // The mission report waits for Cross (Start does nothing there, run 20261016-041503):
        // Cross 0.2 s after Start, unless a menu or gameplay has the input by then (the title's
        // Start opens a menu) or a movie plays (Start skips one).
        if (!gpu::display::movie_playing()) press(0, kCross, t + 200, true);
      }
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
  float ox = 0, oy = 0;  // the cursor in the dialog's own coordinates (its objects' rects)
  dialog_origin(dialog, &ox, &oy);
  float lx = gx - ox, ly = gy - oy;
  bool down = std::any_of(events.begin(), events.end(), [](const Event& e) { return e.kind == Event::LeftDown; });
  s64 t = now_ms();
  // List steps done: look at the item under the cursor again; a hover's steps that scrolled the
  // list wait for the cursor to move a row's height (no runaway scrolling under a resting cursor).
  bool settled = g_list_busy && t >= g_list_busy;
  if (settled) {
    if (g_step_list && is_list(g_step_list) && s32(ppu::r32(g_step_list + kListTop)) != g_step_top) g_scrolled_y = ly;
    g_list_busy = 0;
    g_step_list = 0;
  }
  u32 hit = 0, slider = 0;
  ListItem item;
  bool on_item = false;    // the cursor is on a list's item
  bool refocused = false;  // this frame's hover moved the focus
  if (show && (moved || down || settled) && !(g_drag.slider && g_drag.button)) {
    hit = object_at(ctx, dialog, lx, ly, &slider);
    if (untracked_strip(hit)) hit = 0;
    // The focus follows the cursor where the dialog keeps one (where it keeps none, its scene
    // takes the keys itself).
    if (hit && dlg_focus && hit != dlg_focus) {
      call(ctx, vfunc(dialog, kDlgSetFocus), dialog, hit);
      refocused = true;
    }
    on_item = hit && is_list(hit) && list_item_at(hit, rect_of(ctx, hit), lx, ly, &item);
    // Hover selects items that do not scroll the list.
    if (on_item && item.safe && !down && !g_list_busy && std::fabs(ly - g_scrolled_y) > 40 && (item.dcol || item.drow)) {
      g_step_list = hit;
      g_step_top = s32(ppu::r32(hit + kListTop));
      list_step(item, t + (refocused ? 150 : 0));
    }
  }
  for (const Event& e : events) {
    if (e.kind == Event::LeftDown) {
      float ax = 0, ay = 0;
      u16 arrow = slider ? 0 : arrow_at(ctx, dialog, lx, ly, &ax, &ay);
      if (slider) {  // on a slider's bar: the slider follows the cursor while the button is down
        g_drag = {slider, true, 0, 0};
      } else if (arrow) {  // an arrow: its row takes the focus (where the dialog keeps one), then the direction
        u32 unused = 0, row = object_at(ctx, dialog, ax, ay, &unused);
        s64 due = t + (refocused ? 150 : 0);
        if (row && dlg_focus && row != ppu::r32(dialog + kDlgFocus)) {
          call(ctx, vfunc(dialog, kDlgSetFocus), dialog, row);
          due = t + 150;
        }
        press(arrow, 0, due);
      } else if (hit) {
        if (on_item && g_list_busy) {  // after the steps under way, from where they end
          g_waiting.push_back(e);
          continue;
        }
        // A click on an item chooses it; elsewhere nothing. When the click also moved the focus,
        // the choice waits until the dialog has taken it (CLIENT SELECT lost a press made in the
        // frame of its focus change, run 20261016-035308; 150 ms later it was taken, -035810).
        s64 due = t + (refocused ? 150 : 0);
        if (on_item) {  // a list's item: stepped to, then chosen; a strip's (a slot, a category) only selected
          due = list_step(item, due);
          if (item.strip) continue;
        }
        press(0, kCross, due);
      }
    } else if (e.kind == Event::LeftUp) {
      g_drag.button = false;
    } else if (e.kind == Event::Right) {
      press(0, kCircle, t);
    } else {
      for (int i = 0; i < std::abs(e.value); ++i, t += 90) press(e.value > 0 ? kUp : kDown, 0, t);
    }
  }
  if (g_drag.slider) drive_slider(lx);
}

}  // namespace

InputContext input_context() { return InputContext(g_context.load(std::memory_order_relaxed)); }

void menu_mouse_moved() {
  g_mouse_at = now_ms();
  if (input_context() == InputContext::Menu && input_has_focus()) g_cursor = true;  // at once; frame() agrees
}

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

bool controller_used_last() { return g_pad_at.load() > g_mouse_at.load(); }

void note_pad_input() {
  s64 t = now_ms();
  if (g_pad_at.exchange(t) < g_mouse_at.load() && g_cursor.load()) {
    g_cursor = false;  // the controller took over: the cursor hides at once
    gpu::window_refresh_cursor();
  }
}

void note_menu_key() {
  g_key_at = now_ms();
  if (g_cursor.exchange(false)) gpu::window_refresh_cursor();  // the keys took over until the mouse moves
}

void menu_pad_bits(u16* d1, u16* d2) {
  s64 t = now_ms();
  *d1 |= g_hold.load(std::memory_order_relaxed);
  bool screen = input_context() == InputContext::Screen;
  std::lock_guard lk(g_mu);
  g_presses.erase(std::remove_if(g_presses.begin(), g_presses.end(),
                                 [&](const Press& p) { return p.at + 70 <= t || (p.screen_only && !screen && p.at <= t); }),
                  g_presses.end());
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
