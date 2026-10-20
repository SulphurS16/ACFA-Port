// macOS emulated vblank: a monotonic clock, not a fabricated display-vblank API.
#include "runtime/gpu/display.h"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "platform/macos/display_clock.h"
#include "platform/macos/host_thread.h"
#include "platform/macos/metal_surface.h"
#include "runtime/config.h"
#include "runtime/log.h"

namespace rt::gpu::display {
namespace {

std::atomic<int> g_movies{0}, g_gameplay{0};
std::atomic<u32> g_refresh{0};
std::mutex g_output_mutex;
std::vector<Mode> g_modes;
CGDirectDisplayID g_display = 0;
u64 g_presents_since_output_check = 0;
mac_detail::ClockSchedule g_schedule;  // the single emulated-vblank thread owns this

struct Stats {
  u64 presents = 0, intervals = 0;
  double interval_sum_ms = 0, interval_min_ms = 0, interval_max_ms = 0;
};
std::mutex g_stats_mutex;
Stats g_stats;
u64 g_last_present_ns = 0;

int release(std::atomic<int>& count) {
  int before = count.load(std::memory_order_relaxed);
  while (before > 0 && !count.compare_exchange_weak(before, before - 1, std::memory_order_relaxed)) {}
  return before;
}

double target_fps() {
  return mac_detail::target_fps(cfg().fps.load(std::memory_order_relaxed), g_refresh.load(std::memory_order_relaxed),
                                g_movies.load(std::memory_order_relaxed) > 0, g_gameplay.load(std::memory_order_relaxed) > 0);
}

u32 refresh_of(CGDisplayModeRef mode) {
  if (!mode) return 0;
  const double hz = CGDisplayModeGetRefreshRate(mode);
  // Built-in/variable-refresh displays may return zero. Keep it unknown rather than
  // pretending 60 Hz was measured; --fps display uses its explicit clock fallback.
  return std::isfinite(hz) && hz >= 30 && hz <= 1000 ? u32(std::lround(hz)) : 0;
}

void update_output(bool force) {
  // AppKit publishes this ID into its window-geometry cache on the main thread.
  // This query neither calls AppKit nor constructs NSApplication or a window.
  const auto display = CGDirectDisplayID(mac::display_id());
  std::lock_guard lock(g_output_mutex);
  if (!force && display == g_display) return;
  g_display = display;
  g_modes.clear();
  if (!display || !CGDisplayIsActive(display)) {
    g_refresh.store(0, std::memory_order_relaxed);
    return;
  }
  u32 refresh = 0;
  if (const auto current = CGDisplayCopyDisplayMode(display)) {
    refresh = refresh_of(current);
    CGDisplayModeRelease(current);
  }
  // Publish once: a routine mode check must not briefly change --fps display
  // to its unknown-refresh fallback while the CG query is in progress.
  g_refresh.store(refresh, std::memory_order_relaxed);
  const auto list = CGDisplayCopyAllDisplayModes(display, nullptr);
  if (!list) return;
  const auto count = CFArrayGetCount(list);
  for (CFIndex i = 0; i < count; ++i) {
    const auto mode = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(list, i)));
    if (!CGDisplayModeIsUsableForDesktopGUI(mode)) continue;
    // Match the window layer's backing-pixel dimensions, including scaled Retina modes.
    const auto width = CGDisplayModeGetPixelWidth(mode), height = CGDisplayModeGetPixelHeight(mode);
    const auto hz = refresh_of(mode);
    if (width < 640 || height < 360 || width > UINT32_MAX || height > UINT32_MAX || !hz) continue;
    if (std::none_of(g_modes.begin(), g_modes.end(), [&](const Mode& m) { return m.w == width && m.h == height && m.hz == hz; }))
      g_modes.push_back({u32(width), u32(height), hz});
  }
  CFRelease(list);
  std::sort(g_modes.begin(), g_modes.end(), [](const Mode& a, const Mode& b) {
    return a.w != b.w ? a.w > b.w : a.h != b.h ? a.h > b.h : a.hz > b.hz;
  });
}

}  // namespace

void movie_started() {
  if (g_movies.fetch_add(1, std::memory_order_relaxed) == 0 && cfg().fps != 60)
    LOG_I("display", "video playback: macOS emulated vblank uses a 60 Hz clock until it ends");
}
void movie_stopped() {
  if (release(g_movies) == 1 && cfg().fps != 60) LOG_I("display", "video playback ended");
}
bool movie_playing() { return g_movies.load(std::memory_order_relaxed) > 0; }
void gameplay_started() {
  if (g_gameplay.fetch_add(1, std::memory_order_relaxed) == 0 && cfg().fps != 60)
    LOG_I("display", "mission started: macOS clock target %.2f Hz", target_fps());
}
void gameplay_stopped() {
  if (release(g_gameplay) == 1 && cfg().fps != 60) LOG_I("display", "mission ended: 60 Hz clock outside missions");
}

void wait_next_vblank() {
  const auto deadline = g_schedule.next_deadline(acfa::host::monotonic_time_ns(), target_fps());
  for (;;) {
    const auto now = acfa::host::monotonic_time_ns();
    if (now >= deadline) return;
    const auto left = deadline - now;
    if (left > 250000) acfa::host::sleep_us((left - 150000) / 1000);
    else std::this_thread::yield();
  }
}

u32 present_flags() { return 0; }
void set_exclusive(bool) {}  // no DXGI exclusive mode or DXGI presentation flags on macOS
u32 refresh_hz() { return g_refresh.load(std::memory_order_relaxed); }
std::vector<Mode> modes() { std::lock_guard lock(g_output_mutex); return g_modes; }

void swapchain_created(IDXGISwapChain1*, bool) {
  // Compatibility entry point only: no IDXGI object is dereferenced on this backend.
  update_output(true);
  LOG_I("display", "macOS pacing '%s': using monotonic clock; display-vblank waits and displayed-frame statistics unavailable",
        cfg().pacing.c_str());
}

void after_present(IDXGISwapChain1*) {
  // The presenter reports a completed present call, not proof of the panel's scanout time.
  const auto now = acfa::host::monotonic_time_ns();
  {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.presents;
    if (g_last_present_ns) {
      const double ms = double(now - g_last_present_ns) / 1e6;
      if (!g_stats.intervals || ms < g_stats.interval_min_ms) g_stats.interval_min_ms = ms;
      if (ms > g_stats.interval_max_ms) g_stats.interval_max_ms = ms;
      g_stats.interval_sum_ms += ms;
      ++g_stats.intervals;
    }
    g_last_present_ns = now;
  }
  const bool refresh = ++g_presents_since_output_check >= 120;
  if (refresh) g_presents_since_output_check = 0;
  update_output(refresh);  // a changed cached screen ID is observed on the very next present
}

std::string report() {
  Stats stats;
  {
    std::lock_guard lock(g_stats_mutex);
    stats = g_stats;
    g_stats = {};
  }
  const auto hz = refresh_hz();
  const auto rate = target_fps();
  const auto output = hz ? acfa::strprintf("%u Hz", hz) : std::string("unknown refresh");
  std::string report = acfa::strprintf("display: %s; %.2f Hz monotonic clock%s; %llu present calls; displayed-frame timing unavailable",
                                     output.c_str(), rate, rate >= mac_detail::kUncappedHz ? " (uncapped mission target)" : "",
                                     static_cast<unsigned long long>(stats.presents));
  if (stats.intervals)
    report += acfa::strprintf("; present-call intervals (%llu): mean %.2f ms, min %.2f, max %.2f",
                             static_cast<unsigned long long>(stats.intervals), stats.interval_sum_ms / stats.intervals,
                             stats.interval_min_ms, stats.interval_max_ms);
  return report;
}

}  // namespace rt::gpu::display
