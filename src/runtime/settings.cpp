#include "runtime/settings.h"

#include <algorithm>
#include <mutex>

#include "common/ini.h"
#include "runtime/bindings.h"
#include "runtime/config.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"

namespace rt::settings {
namespace {

using acfa::settings::Mode;

std::mutex g_mu;  // g_display and the file; never held while calling into the window
std::string g_file;
acfa::settings::Display g_display;
Mode g_last_full = Mode::Borderless;  // what F11 / Alt+Enter switch to from a window

// Re-reads the file (it may have been edited since), changes the given values and saves it.
template <class F>
void update_file(F&& change) {
  if (g_file.empty()) return;
  try {
    acfa::Ini ini = acfa::Ini::load(g_file);
    change(ini);
    ini.save(g_file);
  } catch (const std::exception& e) {
    LOG_W("settings", "not saved: %s", e.what());
  }
}

}  // namespace

void init(const std::string& file, std::vector<std::string>* notes) {
  std::lock_guard lk(g_mu);
  g_file = file;
  acfa::Ini ini = acfa::settings::load_or_create(file, notes);
  g_display = acfa::settings::read_display(ini);
  if (g_display.mode != Mode::Windowed) g_last_full = g_display.mode;
  bindings::load(ini);
}

void use_safe_display() {
  std::lock_guard lk(g_mu);
  g_display = acfa::settings::Display{};
  g_display.mode = Mode::Windowed;
}

const std::string& file() { return g_file; }

acfa::settings::Display display() {
  std::lock_guard lk(g_mu);
  return g_display;
}

void set_fps(double fps) {
  cfg().fps = fps;
  {
    std::lock_guard lk(g_mu);
    g_display.fps = fps;
    update_file([&](acfa::Ini& ini) { ini.set("display", "fps", acfa::settings::fps_text(fps)); });
  }
  LOG_I("settings", "frame rate %s", acfa::settings::fps_text(fps).c_str());
}

void set_mode(Mode m) {
  acfa::settings::Display d;
  {
    std::lock_guard lk(g_mu);
    if (m != Mode::Windowed) g_last_full = m;
    g_display.mode = m;
    d = g_display;
    update_file([&](acfa::Ini& ini) { ini.set("display", "mode", acfa::settings::mode_name(m)); });
  }
  LOG_I("settings", "display mode %s", acfa::settings::mode_name(m));
  if (m == Mode::Fullscreen) {
    gpu::window_set_fullscreen(false);  // DXGI takes the window as it is
    gpu::request_exclusive(true, u32(d.fs_width), u32(d.fs_height), u32(d.fs_hz));
  } else if (gpu::exclusive_active()) {
    gpu::request_exclusive(false);  // the window then applies its mode (apply_window_mode)
  } else {
    gpu::request_exclusive(false);
    gpu::window_set_fullscreen(m == Mode::Borderless);
  }
}

void set_window_size(int w, int h) {
  {
    std::lock_guard lk(g_mu);
    g_display.width = w;
    g_display.height = h;
    update_file([&](acfa::Ini& ini) { ini.set("display", "window", acfa::strprintf("%dx%d", w, h)); });
  }
  gpu::window_set_client_size(w, h);  // a window not shown as one keeps the size for later
  LOG_I("settings", "window size %dx%d", w, h);
}

void set_fullscreen_mode(int w, int h, int hz) {
  acfa::settings::Display d;
  {
    std::lock_guard lk(g_mu);
    g_display.fs_width = w;
    g_display.fs_height = h;
    g_display.fs_hz = hz;
    d = g_display;
    update_file([&](acfa::Ini& ini) { ini.set("display", "fullscreen_mode", acfa::settings::fullscreen_text(d)); });
  }
  if (d.mode == Mode::Fullscreen) gpu::request_exclusive(true, u32(w), u32(h), u32(hz));
  LOG_I("settings", "fullscreen mode %s", acfa::settings::fullscreen_text(d).c_str());
}

void set_render_scale(int percent) {
  percent = std::clamp(percent, acfa::settings::kRenderScaleMin, acfa::settings::kRenderScaleMax);
  {
    std::lock_guard lk(g_mu);
    g_display.render_scale = percent;
    update_file([&](acfa::Ini& ini) { ini.set("display", "render_scale", std::to_string(percent)); });
  }
  LOG_I("settings", "render scale %d%%", percent);
}

void set_monitor(const std::string& device) {
  acfa::settings::Display d;
  {
    std::lock_guard lk(g_mu);
    if (g_display.monitor == device) return;
    g_display.monitor = device;
    g_display.fs_width = g_display.fs_height = g_display.fs_hz = 0;  // the new monitor's desktop mode
    d = g_display;
    update_file([&](acfa::Ini& ini) {
      ini.set("display", "monitor", device);
      ini.set("display", "fullscreen_mode", acfa::settings::fullscreen_text(d));
    });
  }
  LOG_I("settings", "monitor %s", device.empty() ? "primary" : device.c_str());
  if (d.mode == Mode::Fullscreen && gpu::exclusive_active()) {
    gpu::request_exclusive(false);  // leaves exclusive mode; apply_window_mode re-enters it on the new monitor
  } else {
    gpu::window_move_to_monitor(device);
  }
}

void toggle_fullscreen() {
  Mode now, next;
  {
    std::lock_guard lk(g_mu);
    now = g_display.mode;
    next = now == Mode::Windowed ? g_last_full : Mode::Windowed;
  }
  set_mode(next);
}

void apply_window_mode() {
  acfa::settings::Display cur = display();
  Mode m = cur.mode;
  gpu::window_move_to_monitor(cur.monitor);
  if (m == Mode::Fullscreen) {  // exclusive fullscreen left for another monitor: entered again there
    if (!gpu::exclusive_active()) gpu::request_exclusive(true, u32(cur.fs_width), u32(cur.fs_height), u32(cur.fs_hz));
    return;
  }
  gpu::window_set_fullscreen(m == Mode::Borderless);
  if (m == Mode::Windowed) {
    acfa::settings::Display d = display();
    gpu::window_set_client_size(d.width, d.height);
  }
}

void note_window_size(int w, int h) {
  std::lock_guard lk(g_mu);
  if (g_display.width == w && g_display.height == h) return;
  g_display.width = w;
  g_display.height = h;
  update_file([&](acfa::Ini& ini) { ini.set("display", "window", acfa::strprintf("%dx%d", w, h)); });
}

void save_bindings() {
  std::lock_guard lk(g_mu);
  update_file([](acfa::Ini& ini) { bindings::store(ini); });
}

}  // namespace rt::settings
