#include "runtime/gpu/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/settings.h"
#include "runtime/ui/overlay.h"

namespace rt::gpu {
namespace {

HWND g_hwnd = nullptr;
std::atomic<bool> g_close{false};
bool g_alive = false;
DWORD g_thread = 0;
std::atomic<bool> g_mouse_captured{false};

void clip_cursor() {
  RECT r;
  GetClientRect(g_hwnd, &r);
  MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
  ClipCursor(&r);
}

// Mouse capture for mouse look: the cursor is hidden and confined to the client area.
void set_mouse_capture(bool on) {
  if (on == g_mouse_captured) return;
  g_mouse_captured = on;
  if (on) {
    clip_cursor();
    while (ShowCursor(FALSE) >= 0) {
    }
  } else {
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {
    }
  }
  input_mouse_capture(on);
}

// Borderless fullscreen: the window without frame covers its monitor (the swap chain follows the
// client size); toggled with F11 or Alt+Enter, restored to the previous window placement.
std::atomic<bool> g_fullscreen{false};
WINDOWPLACEMENT g_windowed_placement = [] {
  WINDOWPLACEMENT p{};
  p.length = sizeof p;
  return p;
}();
void set_fullscreen(bool on) {
  if (on == g_fullscreen || !g_hwnd) return;
  DWORD style = DWORD(GetWindowLongW(g_hwnd, GWL_STYLE));
  if (on) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetWindowPlacement(g_hwnd, &g_windowed_placement) || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
      return;
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongW(g_hwnd, GWL_STYLE, LONG((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
    SetWindowPlacement(g_hwnd, &g_windowed_placement);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g_fullscreen = on;
  if (g_mouse_captured) clip_cursor();
  LOG_I("gpu", "window: %s", on ? "borderless fullscreen" : "windowed");
}

void work_area(int* w, int* h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  *w = mi.rcWork.right - mi.rcWork.left;
  *h = mi.rcWork.bottom - mi.rcWork.top;
}

// Windowed: the client area at w x h (or the largest size with that aspect ratio whose frame
// fits the work area), centred on the monitor.
void set_client_size(int w, int h) {
  if (!g_hwnd || exclusive_active()) return;  // applied when the window is a window again
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  int aw = mi.rcWork.right - mi.rcWork.left - fw, ah = mi.rcWork.bottom - mi.rcWork.top - fh;
  if (w > aw || h > ah) {
    double s = std::min(double(aw) / w, double(ah) / h);
    w = int(w * s);
    h = int(h * s);
  }
  int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - (w + fw)) / 2;
  int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - (h + fh)) / 2;
  if (g_fullscreen) {  // applies when leaving fullscreen
    g_windowed_placement.rcNormalPosition = RECT{x, y, x + w + fw, y + h + fh};
    g_windowed_placement.showCmd = SW_SHOWNORMAL;
    return;
  }
  ShowWindow(g_hwnd, SW_SHOWNORMAL);
  SetWindowPos(g_hwnd, nullptr, x, y, w + fw, h + fh, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
  if (g_mouse_captured) clip_cursor();
}

struct Call {
  const std::function<void()>* fn;
  bool done = false;
};
std::mutex g_call_mu;
std::condition_variable g_call_cv;
std::deque<Call*> g_calls;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  const bool overlay = ui::is_open();
  switch (msg) {
    case WM_CLOSE:
      g_close = true;
      return 0;  // the runtime asks the game to exit first
    case WM_DESTROY:
      g_alive = false;
      return 0;
    case WM_INPUT: {
      RAWINPUT ri;
      UINT size = sizeof ri;
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
          ri.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = ri.data.mouse;
        input_mouse_raw(m.usFlags, m.lLastX, m.lLastY, m.usButtonFlags, m.usButtonData);
      }
      break;  // DefWindowProc cleans up the raw input buffer
    }
    case WM_MOUSEMOVE:
      if (overlay) ui::on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
      int button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP   ? 0
                   : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? 1
                   : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ? 2
                   : GET_XBUTTON_WPARAM(wp) == XBUTTON1          ? 3
                                                                 : 4;
      if (overlay) {
        ui::on_mouse_button(button, down, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (down) SetCapture(h);
        else ReleaseCapture();
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
      }
      if (msg == WM_LBUTTONDOWN && !g_mouse_captured && !input_isolated()) {  // isolated test runs never take the cursor
        set_mouse_capture(true);
        return 0;
      }
      break;
    }
    case WM_MOUSEWHEEL:
      if (overlay) {
        ui::on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
      }
      break;
    case WM_SIZE:
    case WM_MOVE:
      if (g_mouse_captured) clip_cursor();
      break;
    case WM_APP + 1:  // exclusive fullscreen was left: the window takes the configured mode
      settings::apply_window_mode();
      return 0;
    case WM_EXITSIZEMOVE:  // the user resized the window: kept for the next start
      if (!g_fullscreen && !exclusive_active() && !IsIconic(h) && !IsZoomed(h)) {
        RECT rc{};
        GetClientRect(h, &rc);
        if (rc.right >= 320 && rc.bottom >= 180) settings::note_window_size(rc.right, rc.bottom);
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      bool repeat = lp & (1 << 30);
      if (!repeat && (wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN && (lp & (1 << 29))))) {
        settings::toggle_fullscreen();
        return 0;  // not passed to the game's key bindings
      }
      if (wp == VK_F1 && !input_isolated()) {  // releases (or takes) the mouse
        if (!repeat && !overlay) set_mouse_capture(!g_mouse_captured);
        return 0;
      }
      if (overlay) {
        ui::on_key(u32(wp), true, repeat);
        return 0;  // keys pressed in PC SETTINGS never reach the game
      }
      input_key(u32(wp), true);
      if (msg == WM_SYSKEYDOWN && wp != VK_F10 && wp != VK_MENU) break;  // Alt+F4 and the like
      return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
      input_key(u32(wp), false);  // releases always reach the key state (no stuck keys)
      if (overlay) ui::on_key(u32(wp), false, false);
      if (msg == WM_SYSKEYUP && wp != VK_F10 && wp != VK_MENU) break;
      return 0;
    case WM_SYSCHAR:
      return 0;  // no menu beep for Alt+key
    case WM_KILLFOCUS:
      set_mouse_capture(false);
      input_focus(false);
      return 0;
    case WM_SETFOCUS:
      input_focus(true);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void window_dpi_aware() {
  using SetCtx = BOOL(WINAPI*)(HANDLE);
  if (auto fn = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
    fn(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

bool window_create(const std::string& title, int w, int h, bool visible) {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"acfa_window";
  RegisterClassExW(&wc);
  RECT r{0, 0, w, h};
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&r, style, FALSE);
  std::wstring wt(title.begin(), title.end());
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return false;
  g_alive = true;
  g_thread = GetCurrentThreadId();
  set_client_size(w, h);  // fitted to the monitor, centred
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.hwndTarget = g_hwnd;
  if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
    LOG_W("input", "raw mouse input unavailable (RegisterRawInputDevices failed, error %lu)", GetLastError());
  if (visible) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  return true;
}

void window_set_fullscreen(bool on) {
  run_on_window_thread([on] { set_fullscreen(on); });
}
bool window_is_fullscreen() { return g_fullscreen.load(); }

void window_set_client_size(int w, int h) {
  run_on_window_thread([w, h] { set_client_size(w, h); });
}
void window_client_size(int* w, int* h) {
  RECT rc{};
  if (g_hwnd) GetClientRect(g_hwnd, &rc);
  *w = rc.right;
  *h = rc.bottom;
}
void window_work_area(int* w, int* h) {
  *w = *h = 0;
  if (g_hwnd) work_area(w, h);
}
void window_monitor_size(int* w, int* h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  *w = *h = 0;
  if (g_hwnd && GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  }
}

void window_set_mouse_capture(bool on) {
  run_on_window_thread([on] {
    if (!on || !input_isolated()) set_mouse_capture(on);
  });
}
bool window_mouse_captured() { return g_mouse_captured.load(); }

void* window_handle() { return g_hwnd; }

bool window_pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (;;) {
    Call* c;
    {
      std::lock_guard lk(g_call_mu);
      if (g_calls.empty()) break;
      c = g_calls.front();
      g_calls.pop_front();
    }
    (*c->fn)();
    {
      std::lock_guard lk(g_call_mu);
      c->done = true;
    }
    g_call_cv.notify_all();
  }
  ui::tick();
  return g_alive;
}

bool run_on_window_thread(const std::function<void()>& fn) {
  if (!g_hwnd || !g_alive) return false;
  if (GetCurrentThreadId() == g_thread) {
    fn();
    return true;
  }
  Call c{&fn};
  std::unique_lock lk(g_call_mu);
  g_calls.push_back(&c);
  g_call_cv.wait(lk, [&] { return c.done; });
  return true;
}

bool window_close_requested() { return g_close.load(); }

void window_set_title(const std::string& title) {
  std::wstring wt(title.begin(), title.end());
  if (g_hwnd) SetWindowTextW(g_hwnd, wt.c_str());
}

}  // namespace rt::gpu
