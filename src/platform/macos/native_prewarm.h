// Startup-only warm-up: runs before guest execution, never changes game resources.
#pragma once
#include <functional>
#include "common/bytes.h"
namespace rt::gpu {
struct NativeShaderWarmResult {
  u32 warmed=0, failed=0, invalid=0, skipped=0;
  bool cancelled=false, budget_reached=false;
  double seconds=0;
};
// Callback pumps the main-thread window and returns false on user close.
// The finite 60-second budget is checked between synchronous driver compilations.
NativeShaderWarmResult prewarm_native_shaders(const std::function<bool(u32)>& progress);
}
