#include "platform/macos/shader_prewarm.h"
#include "common/crypto.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
std::string hash(const std::string& source) {
  const auto bytes = acfa::crypto::sha256(source.data(), source.size());
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : bytes) { out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
std::string key(char c) { return std::string(64, c); }
std::string cache(const std::string& k, const std::string& payload) {
  return "// ACFA Metal cache v1 " + k + "\n// payload-sha256 " + hash(payload) + "\n" + payload;
}
void write(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  require(bool(out), "write test fixture");
}
struct TemporaryDirectory {
  fs::path path;
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
}  // namespace

int main() {
  alarm(10);
  char pattern[] = "/tmp/acfa-native-shader-prewarm-XXXXXX";
  require(mkdtemp(pattern) != nullptr, "create unique temporary directory");
  TemporaryDirectory temp{pattern};
  using namespace acfa::metal;
  ShaderCacheScanOptions options;
  const auto accept = [](const ShaderCacheRecord&) { return true; };
  require(scan_shader_cache(temp.path / "missing", options, accept).discovered == 0,
          "missing cache is nonfatal");

  const std::string vs = "vertex float4 acfa_vs() { return 0; }\n";
  const std::string ps = "fragment float4 acfa_ps() { return 1; }\n";
  const std::string both = vs + ps;
  write(temp.path / (key('a') + ".mslcache"), cache(key('a'), vs));
  write(temp.path / (key('b') + ".mslcache"), cache(key('b'), ps));
  write(temp.path / (key('c') + ".mslcache"), cache(key('c'), both));
  const auto time = fs::file_time_type::clock::now();
  for (const char c : {'a', 'b', 'c'})
    fs::last_write_time(temp.path / (key(c) + ".mslcache"), time);
  fs::last_write_time(temp.path / (key('c') + ".mslcache"), time + std::chrono::seconds(1));
  std::vector<std::string> order;
  auto stats = scan_shader_cache(temp.path, options, [&](const ShaderCacheRecord& record) {
    order.push_back(record.key);
    require(hash(record.payload) == hash(record.key == key('a') ? vs : record.key == key('b') ? ps : both),
            "payload is unchanged");
    require(record.has_vertex == (record.key != key('b')) && record.has_fragment == (record.key != key('a')),
            "vertex and fragment tokens detected");
    return true;
  });
  require(order == std::vector<std::string>{key('c'), key('a'), key('b')},
          "newest first and deterministic path tie order");
  require(stats.discovered == 3 && stats.accepted == 3 && stats.invalid == 0 && stats.skipped == 0,
          "valid cache stats");
  options.max_records = 1;
  stats = scan_shader_cache(temp.path, options, accept);
  require(stats.accepted == 1 && stats.skipped == 2 && !stats.stopped, "record cap enforced");
  options.max_records = 256;
  stats = scan_shader_cache(temp.path, options, [](const ShaderCacheRecord&) { return false; });
  require(stats.accepted == 1 && stats.skipped == 2 && stats.stopped, "callback can stop immediately");

  write(temp.path / (key('d') + ".mslcache"), cache(key('e'), vs));
  auto corrupted = cache(key('e'), ps); corrupted.back() = '!';
  write(temp.path / (key('e') + ".mslcache"), corrupted);
  write(temp.path / (key('f') + ".mslcache"), "bad header\n");
  write(temp.path / "bad-key.mslcache", cache(key('a'), vs));
  write(temp.path / (key('1') + ".mslcache"), cache(key('1'), "acfa_vs_suffix prefix_acfa_ps"));
  write(temp.path / "ignored.txt", "unrelated");
  fs::create_symlink(temp.path / (key('a') + ".mslcache"), temp.path / (key('2') + ".mslcache"));
  fs::create_directory(temp.path / (key('3') + ".mslcache"));
  stats = scan_shader_cache(temp.path, options, accept);
  require(stats.discovered == 10 && stats.accepted == 3 && stats.invalid == 5 && stats.skipped == 2,
          "bad key/header/hash/entry and symlinks rejected without aborting scan");

  const auto limits = temp.path / "limits"; fs::create_directory(limits);
  const auto fixture = cache(key('a'), vs);
  write(limits / (key('a') + ".mslcache"), fixture);
  options.max_file_bytes = fixture.size() - 1;
  stats = scan_shader_cache(limits, options, accept);
  require(stats.accepted == 0 && stats.skipped == 1 && stats.bytes_read == 0,
          "oversized file skipped before allocation/read");
  options.max_file_bytes = 2 * 1024 * 1024;
  options.max_total_bytes = fixture.size() - 1;
  stats = scan_shader_cache(limits, options, accept);
  require(stats.accepted == 0 && stats.skipped == 1 && stats.bytes_read == 0,
          "total byte cap enforced before read");
  options.max_total_bytes = fixture.size();
  stats = scan_shader_cache(limits, options, accept);
  require(stats.accepted == 1 && stats.bytes_read == fixture.size(), "exact byte budget accepted");
  alarm(0);
  std::puts("native-shader-prewarm: verified cache scan, stage hints, ordering, limits and cancellation passed");
}
