#include "common/file.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace acfa {
namespace {

std::mutex g_mu;

fs::path norm(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(fs::absolute(p), ec);
  if (ec) throw std::runtime_error("cannot resolve protected path: " + p.string());
#if defined(_WIN32)
  auto s = abs.wstring();
  for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return fs::path(s).lexically_normal();
#else
  return abs.lexically_normal();
#endif
}

bool within(fs::path p, const fs::path& root) {
  for (;;) {
    std::error_code ec;
    if (p == root || fs::equivalent(p, root, ec)) return true;
    auto parent = p.parent_path();
    if (parent.empty() || parent == p) return false;
    p = std::move(parent);
  }
}

// Built-in protected roots: the game folder inside the project root (tools run from the root)
// and, on the development PC, the read/copy-only reference projects and the Manager's records.
// Each tool also adds the game folders it is given at runtime.
std::vector<fs::path> g_protected = {
    norm(fs::current_path() / "Armored Core - For Answer (USA)"),
};

}  // namespace

void add_protected_root(const fs::path& p) {
  std::lock_guard lk(g_mu);
  g_protected.push_back(norm(p));
}

void guard_write(const fs::path& p) {
  fs::path n = norm(p);
  std::lock_guard lk(g_mu);
  for (const auto& r : g_protected) {
    if (within(n, r))
      throw std::runtime_error("refusing to write inside protected read-only tree: " + p.string());
  }
}

std::vector<u8> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<u8> v(static_cast<size_t>(n));
  if (n && !f.read(reinterpret_cast<char*>(v.data()), n)) throw std::runtime_error("read failed: " + p.string());
  return v;
}

std::vector<u8> read_file_range(const fs::path& p, u64 offset, u64 size) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  f.seekg(std::streamoff(offset));
  std::vector<u8> v(static_cast<size_t>(size));
  if (size && !f.read(reinterpret_cast<char*>(v.data()), std::streamsize(size)))
    throw std::runtime_error("short read: " + p.string());
  return v;
}

void write_file(const fs::path& p, const void* data, size_t size) {
  guard_write(p);
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("cannot create " + p.string());
  if (size) f.write(static_cast<const char*>(data), std::streamsize(size));
  if (!f) throw std::runtime_error("write failed: " + p.string());
}

void write_text(const fs::path& p, const std::string& s) { write_file(p, s.data(), s.size()); }

}  // namespace acfa
