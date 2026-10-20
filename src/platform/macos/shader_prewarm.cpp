#include "platform/macos/shader_prewarm.h"
#include "common/crypto.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace acfa::metal {
namespace {
struct Candidate {
  std::filesystem::path path;
  std::filesystem::file_time_type time;
};

bool hex_key(std::string_view key) {
  return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string hash_hex(std::string_view bytes) {
  const auto hash = acfa::crypto::sha256(bytes.data(), bytes.size());
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const auto byte : hash) {
    out += digits[byte >> 4];
    out += digits[byte & 15];
  }
  return out;
}

bool identifier_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool has_token(std::string_view text, std::string_view token) {
  for (auto at = text.find(token); at != std::string_view::npos;
       at = text.find(token, at + token.size())) {
    if ((at == 0 || !identifier_char(text[at - 1])) &&
        (at + token.size() == text.size() ||
         !identifier_char(text[at + token.size()]))) return true;
  }
  return false;
}

struct FileHandle {
  int fd;
  ~FileHandle() { if (fd >= 0) close(fd); }
};

// The final path must stay a regular non-symlink file. Check size and modification
// time after the read because the running game may still be adding cache files.
bool stable_read(const std::filesystem::path& path, std::size_t limit,
                 std::size_t remaining, std::string& contents,
                 ShaderCacheScanStats& stats) {
  const FileHandle file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat before{}, after{};
  if (file.fd < 0 || fstat(file.fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0) {
    ++stats.invalid;
    return false;
  }
  const auto size = static_cast<std::uintmax_t>(before.st_size);
  if (size > limit || size > remaining) {
    ++stats.skipped;
    return false;
  }
  contents.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto n = read(file.fd, contents.data() + offset, contents.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { stats.bytes_read += offset; ++stats.invalid; return false; }
    offset += static_cast<std::size_t>(n);
  }
  stats.bytes_read += offset;
  if (fstat(file.fd, &after) || before.st_size != after.st_size ||
      before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
      before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec) {
    ++stats.invalid;
    return false;
  }
  return true;
}

bool parse(const Candidate& candidate, std::string& contents, ShaderCacheRecord& record) {
  const auto key = candidate.path.stem().string();
  if (!hex_key(key)) return false;
  const auto prefix = "// ACFA Metal cache v1 " + key + "\n// payload-sha256 ";
  if (!contents.starts_with(prefix) || contents.size() <= prefix.size() + 65 ||
      contents[prefix.size() + 64] != '\n') return false;
  const auto expected = std::string_view(contents).substr(prefix.size(), 64);
  const auto payload = std::string_view(contents).substr(prefix.size() + 65);
  if (!hex_key(expected) || hash_hex(payload) != expected) return false;
  record.has_vertex = has_token(payload, "acfa_vs");
  record.has_fragment = has_token(payload, "acfa_ps");
  if (!record.has_vertex && !record.has_fragment) return false;
  record.path = candidate.path;
  record.key = key;
  // Drop the header in-place rather than retaining all verified payloads.
  contents.erase(0, prefix.size() + 65);
  record.payload = std::move(contents);
  return true;
}
}  // namespace

ShaderCacheScanStats scan_shader_cache(
    const std::filesystem::path& directory,
    const ShaderCacheScanOptions& options,
    const std::function<bool(const ShaderCacheRecord&)>& callback) {
  ShaderCacheScanStats stats;
  std::vector<Candidate> candidates;
  std::error_code error;
  std::filesystem::directory_iterator it(
      directory, std::filesystem::directory_options::skip_permission_denied, error), end;
  while (!error && it != end) {
    const auto& entry = *it;
    if (entry.path().extension() == ".mslcache") {
      ++stats.discovered;
      const auto status = entry.symlink_status(error);
      if (!error && status.type() == std::filesystem::file_type::regular) {
        const auto time = entry.last_write_time(error);
        if (!error) candidates.push_back({entry.path(), time});
        else ++stats.invalid;
      } else ++stats.skipped;
      error.clear();
    }
    it.increment(error);
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return a.time != b.time ? a.time > b.time : a.path < b.path;
  });
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (!callback || stats.accepted >= options.max_records ||
        stats.bytes_read >= options.max_total_bytes) {
      stats.skipped += candidates.size() - i;
      break;
    }
    std::string contents;
    if (!stable_read(candidates[i].path, options.max_file_bytes,
                     options.max_total_bytes - stats.bytes_read, contents, stats)) continue;
    ShaderCacheRecord record;
    if (!parse(candidates[i], contents, record)) { ++stats.invalid; continue; }
    ++stats.accepted;
    if (!callback(record)) {
      stats.stopped = true;
      stats.skipped += candidates.size() - i - 1;
      break;
    }
  }
  return stats;
}
}  // namespace acfa::metal
