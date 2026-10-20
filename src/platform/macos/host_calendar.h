// macOS wall-clock/time-zone adapters for guest RTC/system utilities. These are
// independent of the monotonic execution clock and never set the system clock/TZ.
#pragma once
#ifndef __APPLE__
#error "The macOS calendar adapter requires Apple CoreFoundation"
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <chrono>
#include <cstdint>

namespace acfa::host {

inline std::int64_t unix_wall_time_us() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

struct TimeZoneOffset {
  std::int32_t current_seconds = 0;   // east of UTC, including daylight adjustment
  std::int32_t standard_minutes = 0;  // PS3 TIMEZONE parameter excludes daylight time
  bool daylight = false;
};

inline TimeZoneOffset timezone_at(CFTimeZoneRef zone, std::int64_t unix_us) noexcept {
  if (!zone) return {};
  const CFAbsoluteTime at = double(unix_us) / 1000000.0 - kCFAbsoluteTimeIntervalSince1970;
  const auto offset = CFTimeZoneGetSecondsFromGMT(zone, at);
  const auto daylight_offset = CFTimeZoneGetDaylightSavingTimeOffset(zone, at);
  return {static_cast<std::int32_t>(offset), static_cast<std::int32_t>((offset - daylight_offset) / 60),
          bool(CFTimeZoneIsDaylightSavingTime(zone, at))};
}

inline TimeZoneOffset system_timezone_now() noexcept {
  const auto zone = CFTimeZoneCopySystem();
  const auto result = timezone_at(zone, unix_wall_time_us());
  if (zone) CFRelease(zone);
  return result;
}

}  // namespace acfa::host
