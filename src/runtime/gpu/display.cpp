#include "runtime/gpu/display.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_5.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "runtime/config.h"
#include "runtime/log.h"

namespace rt::lv2 {
u64 system_time_us();
void sleep_us(u64 us);
}  // namespace rt::lv2

namespace rt::gpu::display {
namespace {

std::mutex g_mu;
IDXGIOutput* g_output = nullptr;  // output containing the window (held reference), set by the presenting thread
std::wstring g_output_name;
u32 g_refresh = 0;  // Hz of the output's current display mode (integer, as Windows reports it)
u32 g_divisor = 0;  // display-locked pacing: an emulated vblank every N display vblanks (0: clock)
bool g_tearing = false;
std::atomic<bool> g_locked{false};  // the last vblank came from the display (else the clock)

u32 divisor_for(u32 hz) {
  if (hz < 50) return 0;
  u32 n = (hz + 30) / 60;
  return n && std::fabs(double(hz) / n - 60.0) <= 1.5 ? n : 0;
}

// ---- clock pacing (60 Hz from the performance counter) ----
double g_next_tick_us = 0;
void wait_clock() {
  const double period = 1e6 / 60.0;
  double now = double(lv2::system_time_us());
  if (g_next_tick_us == 0 || now - g_next_tick_us > 4 * period) g_next_tick_us = now;  // start or resynchronise
  g_next_tick_us += period;
  for (;;) {
    now = double(lv2::system_time_us());
    double left = g_next_tick_us - now;
    if (left <= 0) return;
    if (left > 2000) lv2::sleep_us(u64(left - 1000));
    else _mm_pause();
  }
}

// ---- presentation statistics ----
struct Stats {
  u64 displayed = 0, unresolved = 0, presents = 0;
  u64 by_refreshes[5] = {};  // frames shown for 1, 2, 3, 4, 5+ refreshes (index n-1)
  std::vector<float> interval_ms;
  u64 unavailable = 0;
};
Stats g_stats;  // written by the presenting thread, read by report() (perf thread)
std::mutex g_stats_mu;
UINT g_last_count = 0, g_last_refresh = 0;
LARGE_INTEGER g_last_qpc{};
bool g_have_last = false;
double g_qpc_ms = 0;
u64 g_presents_since_output_check = 0;

void update_output(IDXGISwapChain1* swap) {
  IDXGIOutput* out = nullptr;
  if (FAILED(swap->GetContainingOutput(&out)) || !out) return;
  DXGI_OUTPUT_DESC od{};
  out->GetDesc(&od);
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  u32 hz = EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  std::lock_guard lk(g_mu);
  if (g_output && g_output_name == od.DeviceName && g_refresh == hz) {
    out->Release();
    return;
  }
  if (g_output) g_output->Release();
  g_output = out;
  g_output_name = od.DeviceName;
  g_refresh = hz;
  g_divisor = divisor_for(hz);
  const std::string& want = cfg().pacing;
  if (want == "clock") g_divisor = 0;
  else if (want == "display" && !g_divisor && hz) g_divisor = std::max<u32>(1, (hz + 30) / 60);
  char name[64];
  WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, name, sizeof name, nullptr, nullptr);
  if (g_divisor)
    LOG_I("display", "output %s at %u Hz: the game's vblank follows every %u%s display vblank (%.2f Hz)", name, hz, g_divisor,
          g_divisor == 1 ? "" : g_divisor == 2 ? "nd" : g_divisor == 3 ? "rd" : "th", double(hz) / g_divisor);
  else
    LOG_I("display", "output %s at %u Hz: not a multiple of ~60 Hz; the game's vblank runs from a 60 Hz clock%s", name, hz,
          g_tearing ? ", frames presented with tearing allowed (even pacing on variable-refresh displays)" : "");
}

}  // namespace

void wait_next_vblank() {
  IDXGIOutput* out = nullptr;
  u32 div = 0;
  {
    std::lock_guard lk(g_mu);
    if (g_output && g_divisor) {
      out = g_output;
      out->AddRef();
      div = g_divisor;
    }
  }
  if (out) {
    bool ok = true;
    for (u32 i = 0; i < div && ok; ++i) ok = SUCCEEDED(out->WaitForVBlank());
    out->Release();
    if (ok) {
      g_locked = true;
      g_next_tick_us = 0;
      return;
    }
  }
  g_locked = false;
  wait_clock();
}

u32 present_flags() {
  std::lock_guard lk(g_mu);
  return g_tearing && !g_divisor ? DXGI_PRESENT_ALLOW_TEARING : 0;
}

void swapchain_created(IDXGISwapChain1* swap, bool tearing) {
  {
    std::lock_guard lk(g_mu);
    g_tearing = tearing;
  }
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpc_ms = 1000.0 / double(f.QuadPart);
  update_output(swap);
}

void after_present(IDXGISwapChain1* swap) {
  std::lock_guard slk(g_stats_mu);
  ++g_stats.presents;
  if (++g_presents_since_output_check >= 120) {  // the window may have moved to another output
    g_presents_since_output_check = 0;
    update_output(swap);
  }
  DXGI_FRAME_STATISTICS st{};
  HRESULT hr = swap->GetFrameStatistics(&st);
  if (FAILED(hr)) {
    ++g_stats.unavailable;
    g_have_last = false;
    return;
  }
  if (!st.PresentCount) return;
  if (g_have_last && st.PresentCount != g_last_count) {
    UINT np = st.PresentCount - g_last_count, nr = st.PresentRefreshCount - g_last_refresh;
    if (np == 1) {
      ++g_stats.displayed;
      if (nr >= 1) ++g_stats.by_refreshes[std::min<UINT>(nr, 5) - 1];
      // Display time of this frame is known when it was displayed at the latest vblank.
      if (st.SyncRefreshCount == st.PresentRefreshCount && g_last_qpc.QuadPart)
        g_stats.interval_ms.push_back(float(double(st.SyncQPCTime.QuadPart - g_last_qpc.QuadPart) * g_qpc_ms));
    } else {
      g_stats.unresolved += np;
    }
  }
  g_last_qpc = st.SyncRefreshCount == st.PresentRefreshCount ? st.SyncQPCTime : LARGE_INTEGER{};
  g_last_count = st.PresentCount;
  g_last_refresh = st.PresentRefreshCount;
  g_have_last = true;
}

std::string report() {
  Stats s;
  {
    std::lock_guard slk(g_stats_mu);
    s = std::move(g_stats);
    g_stats = Stats{};
  }
  u32 hz, div;
  bool tearing;
  {
    std::lock_guard lk(g_mu);
    hz = g_refresh;
    div = g_divisor;
    tearing = g_tearing;
  }
  std::string pacing = div && g_locked.load() ? acfa::strprintf("display-locked every %u vblank%s", div, div > 1 ? "s" : "")
                           : acfa::strprintf("60 Hz clock%s", tearing ? ", tearing allowed" : "");
  if (!s.presents) return acfa::strprintf("display: %u Hz, %s; no frames presented", hz, pacing.c_str());
  std::string shown;
  u64 known = 0;
  for (u64 n : s.by_refreshes) known += n;
  if (known)
    for (int i = 0; i < 5; ++i)
      if (s.by_refreshes[i])
        shown += acfa::strprintf("%s%d%s: %.1f%%", shown.empty() ? "" : ", ", i + 1, i == 4 ? "+" : "", 100.0 * double(s.by_refreshes[i]) / double(known));
  std::string iv;
  if (!s.interval_ms.empty()) {
    std::sort(s.interval_ms.begin(), s.interval_ms.end());
    auto pct = [&](double p) { return s.interval_ms[std::min(s.interval_ms.size() - 1, size_t(p * double(s.interval_ms.size())))]; };
    iv = acfa::strprintf("; displayed-frame intervals (%zu): median %.2f ms, p1 %.2f, p99 %.2f, max %.2f", s.interval_ms.size(), pct(0.5),
                         pct(0.01), pct(0.99), s.interval_ms.back());
  }
  return acfa::strprintf("display: %u Hz, %s; %llu presented, %llu displayed (%llu unresolved%s); refreshes per frame %s%s", hz,
                         pacing.c_str(), (unsigned long long)s.presents, (unsigned long long)s.displayed,
                         (unsigned long long)s.unresolved,
                         s.unavailable ? acfa::strprintf(", statistics unavailable %llu", (unsigned long long)s.unavailable).c_str() : "",
                         shown.empty() ? "-" : shown.c_str(), iv.c_str());
}

}  // namespace rt::gpu::display
