// Optional headless benchmark for the player's local, verified MSL cache.
// No game/guest, window, audio, input, command submission or cache writes.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat.h"
#include "platform/macos/metal_shader_cache.h"
#include "platform/macos/shader_prewarm.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
template <class T> struct Ref {
  T* p = nullptr;
  ~Ref() { if (p) p->Release(); }
  T** put() { return &p; }
  operator T*() const { return p; }
};
struct WarmedRecord {
  acfa::metal::ShaderCacheRecord record;
  bool vertex = false, fragment = false;
};
bool create_stage(ID3D11Device* device, const std::string& payload, bool vertex) {
  if (vertex) {
    Ref<ID3D11VertexShader> shader;
    return SUCCEEDED(device->CreateVertexShader(payload.data(), payload.size(), nullptr, shader.put()));
  }
  Ref<ID3D11PixelShader> shader;
  return SUCCEEDED(device->CreatePixelShader(payload.data(), payload.size(), nullptr, shader.put()));
}
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: acfa-native-shader-prewarm-gpu /path/to/shader_cache\n");
    return 2;
  }
  // Independent total wall limit; a compiler stall must not run indefinitely.
  alarm(180);
  @autoreleasepool {
    try {
      Ref<ID3D11Device> device;
      Ref<ID3D11DeviceContext> context;
      if (FAILED(acfa::metal::create_device(device.put(), context.put()))) {
        std::fprintf(stderr, "native shader warm: cannot create headless Metal device\n");
        return 1;
      }
      acfa::metal::ShaderCacheScanOptions options;
      options.max_records = 192;
      // Cache source-key storage is capped at 16 MiB. Keep this bench below it
      // so the second pass tests retention, not deliberate capacity eviction.
      options.max_total_bytes = 8 * 1024 * 1024;
      std::vector<WarmedRecord> warmed;
      std::size_t first_stages = 0, first_failures = 0;
      const auto before = acfa::metal::shader_cache_stats(device);
      const auto first_start = Clock::now();
      const auto scan = acfa::metal::scan_shader_cache(argv[1], options, [&](const auto& record) {
        @autoreleasepool {
          WarmedRecord item{record};
          if (record.has_vertex) {
            item.vertex = create_stage(device, record.payload, true);
            item.vertex ? ++first_stages : ++first_failures;
          }
          if (record.has_fragment) {
            item.fragment = create_stage(device, record.payload, false);
            item.fragment ? ++first_stages : ++first_failures;
          }
          if (item.vertex || item.fragment) warmed.push_back(std::move(item));
        }
        return true;
      });
      const auto first_seconds = elapsed(first_start);
      const auto first = acfa::metal::shader_cache_stats(device);
      std::size_t second_stages = 0, second_failures = 0;
      const auto second_start = Clock::now();
      for (const auto& item : warmed) {
        @autoreleasepool {
          if (item.vertex) create_stage(device, item.record.payload, true) ? ++second_stages : ++second_failures;
          if (item.fragment) create_stage(device, item.record.payload, false) ? ++second_stages : ++second_failures;
        }
      }
      const auto second_seconds = elapsed(second_start);
      const auto second = acfa::metal::shader_cache_stats(device);
      const bool success = first_stages > 0 && first_failures == 0 && second_failures == 0 &&
          second_stages == first_stages && second.compile_attempts == first.compile_attempts &&
          second.compilations == first.compilations &&
          second.hits - first.hits == second_stages && first.evictions == before.evictions;
      std::printf("{\"success\":%s,\"discovered\":%zu,\"verified\":%zu,\"invalid\":%zu,"
                  "\"skipped\":%zu,\"first_stage_creations\":%zu,\"first_stage_failures\":%zu,"
                  "\"first_native_compilations\":%llu,\"first_seconds\":%.6f,"
                  "\"second_stage_creations\":%zu,\"second_stage_failures\":%zu,"
                  "\"second_native_compilations\":%llu,\"second_cache_hits\":%llu,"
                  "\"second_seconds\":%.6f,\"retained_entries\":%zu,\"retained_source_bytes\":%zu}\n",
                  success ? "true" : "false", scan.discovered, scan.accepted, scan.invalid, scan.skipped,
                  first_stages, first_failures,
                  static_cast<unsigned long long>(first.compilations - before.compilations), first_seconds,
                  second_stages, second_failures,
                  static_cast<unsigned long long>(second.compilations - first.compilations),
                  static_cast<unsigned long long>(second.hits - first.hits), second_seconds,
                  second.entries, second.source_bytes);
      alarm(0);
      return success ? 0 : 1;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "native shader warm: %s\n", error.what());
      return 1;
    }
  }
}
