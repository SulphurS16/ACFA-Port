#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace acfa::metal {

struct ShaderCacheRecord {
  std::filesystem::path path;
  std::string key;
  std::string payload;
  bool has_vertex = false;
  bool has_fragment = false;
};

struct ShaderCacheScanOptions {
  std::size_t max_records = 256;
  std::size_t max_file_bytes = 2 * 1024 * 1024;
  std::size_t max_total_bytes = 32 * 1024 * 1024;
};

struct ShaderCacheScanStats {
  // Matching direct children, including files later rejected or skipped.
  std::size_t discovered = 0;
  // Verified records delivered to the callback, including the stopping record.
  std::size_t accepted = 0;
  std::size_t invalid = 0;
  std::size_t skipped = 0;
  std::size_t bytes_read = 0;
  bool stopped = false;
};

// Read newest verified cache files first, with path order breaking time ties.
// Only one payload is retained at a time; the record is valid during the callback.
// Return false from the callback to stop (for example at a startup wall deadline).
// Entry flags are token hints, not a replacement for Metal's function/type checks.
// Missing/unreadable directories and unstable/malformed files are nonfatal.
ShaderCacheScanStats scan_shader_cache(
    const std::filesystem::path& directory,
    const ShaderCacheScanOptions& options,
    const std::function<bool(const ShaderCacheRecord&)>& callback);

}  // namespace acfa::metal
