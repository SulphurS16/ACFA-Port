// Clock policy shared by the actual macOS vblank backend and its offline test.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rt::gpu::display::mac_detail {

inline constexpr double kUncappedHz = 1000;

inline double target_fps(double requested, std::uint32_t refresh, bool movie, bool gameplay) {
  // Menus count frames and the firmware movie player requires a 60 Hz vblank.
  if (movie || !gameplay || !std::isfinite(requested)) return 60;
  if (requested < 0) return kUncappedHz;
  if (requested == 0) return refresh >= 30 ? double(refresh) : 60;
  // These are the settings file's supported limits; guard malformed direct callers too.
  return std::clamp(requested, 30.0, kUncappedHz);
}

class ClockSchedule {
 public:
  std::uint64_t next_deadline(std::uint64_t now, double fps) {
    if (!std::isfinite(fps) || fps <= 0) fps = 60;
    fps = std::clamp(fps, 30.0, kUncappedHz);
    const long double period = 1.0e9L / fps;
    // Reset after a rate change or a long stall. Fractional periods otherwise accumulate
    // so 60/59.94 Hz do not drift by one rounded nanosecond per emulated vblank.
    if (!started_ || fps != fps_ || static_cast<long double>(now) - next_ > 4 * period) {
      next_ = now;
      started_ = true;
    }
    fps_ = fps;
    next_ += period;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return next_ >= static_cast<long double>(maximum) ? maximum : std::uint64_t(std::ceil(next_));
  }

 private:
  bool started_ = false;
  double fps_ = 0;
  long double next_ = 0;
};

}  // namespace rt::gpu::display::mac_detail
