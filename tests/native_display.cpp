// Actual macOS display backend, with a headless window-cache seam; no UI/GPU/device startup.
#include "platform/macos/display_clock.h"
#include "platform/macos/render_scale.h"
#include "platform/macos/host_thread.h"
#include "runtime/config.h"
#include "runtime/gpu/display.h"
#include "runtime/log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unistd.h>

namespace rt {
Config& cfg() { static Config config; return config; }
Level log_level() { return Level::Fatal; }
void log_write(Level, const char*, const std::string&) {}
namespace gpu::mac { std::uint32_t display_id() { return 0; } }
}  // namespace rt

namespace {
void require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
namespace display = rt::gpu::display;
namespace detail = display::mac_detail;
void timed_ticks(int count, std::uint64_t minimum_ns, const char* what) {
  const auto begin = acfa::host::monotonic_time_ns();
  for (int i = 0; i < count; ++i) display::wait_next_vblank();
  const auto elapsed = acfa::host::monotonic_time_ns() - begin;
  require(elapsed >= minimum_ns && elapsed < 1000000000, what);
}
}  // namespace

int main() {
  require(acfa::metal::scene_scale(3456, 2168, 100) == 1.0f, "Retina/fullscreen 100 percent remains console 720p");
  require(acfa::metal::scene_scale(5120, 2880, 100) == 1.0f, "5K output does not silently supersample");
  require(acfa::metal::scene_scale(1280, 720, 100) == 1.0f, "720p remains native");
  require(acfa::metal::scene_scale(3456, 2168, 200) == 2.0f, "explicit supersampling remains possible");
  require(acfa::metal::scene_scale(640, 360, 100) == 0.5f, "small windows can render fewer pixels");
  alarm(10);  // Independent total wall limit, including all pacing calls.
  for (const double requested : {30.0, 59.94, 60.0, 60.4, 120.0, 144.0, 0.0, -1.0}) {
    require(detail::target_fps(requested, 144, false, false) == 60, "menus remain exactly 60 Hz");
    require(detail::target_fps(requested, 144, true, true) == 60, "movies remain exactly 60 Hz in missions");
  }
  require(detail::target_fps(120, 144, false, true) == 120, "mission uses configured FPS");
  require(detail::target_fps(59.94, 144, false, true) == 59.94, "fractional mission target retained");
  require(detail::target_fps(0, 144, false, true) == 144, "display target uses known refresh");
  require(detail::target_fps(0, 0, false, true) == 60, "unknown display refresh falls back to 60 Hz clock");
  require(detail::target_fps(-1, 0, false, true) == 1000, "uncapped mission keeps finite 1 ms vblank quantum");
  require(detail::target_fps(std::numeric_limits<double>::infinity(), 0, false, true) == 60, "infinite target is safe");
  require(detail::target_fps(std::numeric_limits<double>::quiet_NaN(), 0, false, true) == 60, "NaN target is safe");

  detail::ClockSchedule schedule;
  std::uint64_t deadline = 0;
  for (int i = 0; i < 240; ++i) deadline = schedule.next_deadline(deadline, 60);
  require(deadline >= 4000000000 && deadline <= 4000000001, "fractional periods do not accumulate truncation drift");
  const auto faster = schedule.next_deadline(deadline, 120);
  require(faster - deadline >= 8333333 && faster - deadline <= 8333334, "target change starts one new period");
  const auto late = faster + 1000000000;
  const auto resynced = schedule.next_deadline(late, 120);
  require(resynced - late >= 8333333 && resynced - late <= 8333334, "long stall resynchronises without a burst");

  display::swapchain_created(nullptr, true);
  require(display::refresh_hz() == 0 && display::modes().empty(), "absent window cache does not invent display modes");
  display::set_exclusive(true);
  require(display::present_flags() == 0, "Mac flags are zero even for DXGI compatibility inputs");
  display::set_exclusive(false);
  require(display::present_flags() == 0, "Mac never requests DXGI tearing");
  display::after_present(nullptr);
  acfa::host::sleep_us(1000);
  display::after_present(nullptr);
  const auto report = display::report();
  require(report.find("2 present calls") != std::string::npos && report.find("monotonic clock") != std::string::npos &&
              report.find("displayed-frame timing unavailable") != std::string::npos,
          "report distinguishes present calls from actual display timing");
  require(display::report().find("0 present calls") != std::string::npos, "statistics reset between reports");

  rt::cfg().fps = 120;
  timed_ticks(2, 32000000, "actual menu clock waits at 60 Hz");
  display::gameplay_started();
  timed_ticks(6, 49000000, "actual mission clock waits at 120 Hz");
  display::movie_started();
  display::movie_started();
  display::movie_stopped();
  require(display::movie_playing(), "nested decoder still holds movie pacing");
  timed_ticks(2, 32000000, "actual movie clock caps mission at 60 Hz");
  display::movie_stopped();
  display::movie_stopped();
  require(!display::movie_playing(), "movie stops do not underflow counter");
  display::gameplay_stopped();
  display::gameplay_stopped();
  rt::cfg().fps = -1;
  display::gameplay_started();
  timed_ticks(3, 2900000, "actual uncapped mission still waits finite 1 ms ticks");
  display::gameplay_stopped();
  timed_ticks(2, 32000000, "mission exit restores menu clock");
  alarm(0);
  std::puts("native-display: policy, actual clock waits and honest present reporting passed (no window)");
}
