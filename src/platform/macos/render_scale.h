// Native Mac default: retain console-resolution work when a Retina window grows.
// Explicit percentages still allow 150%/200% supersampling; presentation fits the window.
#pragma once
#include <algorithm>
namespace acfa::metal {
inline float scene_scale(int width, int height, int percent) {
  if (width <= 0 || height <= 0) return 1.0f;
  const float fit_height = std::min(float(height), float(width) * 9.0f / 16.0f);
  const float base = std::min(fit_height / 720.0f, 1.0f);
  return std::clamp(base * float(percent) / 100.0f, 0.5f, 4.0f);
}
}
