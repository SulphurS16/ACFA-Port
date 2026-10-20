#pragma once
#include <cstddef>
#include <cstdint>
struct ID3D11Device;
namespace acfa::metal {
// Per-device native library/function retention; not the on-disk MSL text cache.
inline constexpr size_t shader_cache_max_entries=512;
inline constexpr size_t shader_cache_max_source_bytes=16*1024*1024;
struct ShaderCacheStats {
 uint64_t compile_attempts=0,compilations=0,hits=0,evictions=0;
 size_t entries=0,source_bytes=0;
};
// Read-only cumulative counters. Never submits or waits for GPU work.
ShaderCacheStats shader_cache_stats(ID3D11Device*);
}
