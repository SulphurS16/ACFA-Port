// Real host-file tests in a fresh build-directory scratch tree only. No user
// game, save, firmware, mount or emulator state is opened or changed.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "common/file.h"
#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/vfs.h"

namespace rt {
Level log_level() { return Level::Error; }
void log_write(Level, const char*, const std::string&) {}
}  // namespace rt

namespace {
namespace fs = std::filesystem;
namespace vfs = rt::vfs;
using namespace rt;

void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void ok(s32 value, const char* why) { if (value != CELL_OK) throw std::runtime_error(acfa::strprintf("%s: %08x", why, u32(value))); }
void equal(s32 value, s32 expected, const char* why) { if (value != expected) throw std::runtime_error(acfa::strprintf("%s: %08x != %08x", why, u32(value), u32(expected))); }

std::string host_text(const fs::path& path) {
  auto bytes = acfa::read_file(path);
  return std::string(bytes.begin(), bytes.end());
}

struct Scratch {
  fs::path root;
  explicit Scratch(const fs::path& parent) {
    fs::create_directories(parent);
    std::string pattern = (fs::absolute(parent) / "acfa-vfs-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
    auto* result = ::mkdtemp(bytes.data());
    check(result, "fresh scratch creation"); root = result;
  }
  ~Scratch() { std::error_code ec; fs::remove_all(root, ec); }
};

void exercise(const fs::path& root) {
  const fs::path disc = root / "日版只读光盘", flash = root / "固件只读";
  const fs::path hdd0 = root / "硬盘零", hdd1 = root / "缓存一", saves = root / "独立存档测试";
  fs::create_directories(disc / "PS3_GAME/USRDIR"); fs::create_directories(flash);
  acfa::write_text(disc / "PS3_GAME/USRDIR/游戏文本.bin", "disc-data");
  acfa::write_text(flash / "固件.dat", "flash-data");
  acfa::add_protected_root(disc); acfa::add_protected_root(flash);
  vfs::init(disc.string(), hdd0.string(), hdd1.string(), flash.string(), saves.string());
  vfs::Resolved resolved;
  check(vfs::resolve("/DEV_BDVD/PS3_GAME/USRDIR/游戏文本.bin", resolved) &&
        resolved.host == disc / "PS3_GAME/USRDIR/游戏文本.bin" && !resolved.writable, "case-insensitive mount prefix");
  check(vfs::resolve("/dev_hdd0/home/00000001/savedata/中文档案", resolved) &&
        resolved.host == saves / "中文档案" && resolved.writable, "longest save mount prefix");
  check(vfs::resolve("/dev_hdd1/a/.././文本", resolved) && resolved.host == hdd1 / "文本", "guest lexical normalization");
  for (const std::string& guest : {"/dev_hdd0x/file", "/dev_hdd0/../dev_bdvd/file", "/dev_hdd0/C:file", "/dev_hdd0/dir\\file", "/unknown/file"})
    check(!vfs::resolve(guest, resolved), "unsafe/outside guest path accepted");
  check(!vfs::resolve(std::string("/dev_hdd0/a\0b", 13), resolved), "embedded NUL path accepted");

  constexpr s32 RDWR = 2, CREATE = 0x40, EXCL = 0x80, TRUNC = 0x200, APPEND = 0x400;
  u32 fd = 0;
  for (s32 flags : {1, RDWR, CREATE, TRUNC, APPEND})
    equal(vfs::open("/dev_bdvd/PS3_GAME/USRDIR/游戏文本.bin", flags, &fd), CELL_EROFS, "read-only disc write refused");
  equal(vfs::mkdir("/dev_flash/new"), CELL_EROFS, "read-only flash mkdir refused");
  equal(vfs::unlink("/app_home/游戏文本.bin"), CELL_EROFS, "read-only app_home unlink refused");
  ok(vfs::open("/app_home/游戏文本.bin", 0, &fd), "read-only guest open");
  vfs::Stat st{};
  ok(vfs::fstat(fd, &st), "disc fstat"); check(st.readonly && st.size == 9, "disc fd readonly metadata");
  std::array<char, 16> data{}; u64 done = 0, position = 0;
  ok(vfs::read(fd, data.data(), data.size(), &done), "disc read"); check(done == 9 && std::string(data.data(), 9) == "disc-data", "disc bytes");
  ok(vfs::close(fd), "close disc"); equal(vfs::close(fd), CELL_EBADF, "double close rejected");

  ok(vfs::mkdir("/dev_hdd0/汉化目录"), "Chinese directory creation");
  const std::string path = "/dev_hdd0/汉化目录/存档资源.bin";
  ok(vfs::open(path, RDWR | CREATE | EXCL, &fd), "Chinese file creation");
  check(vfs::fd_path(fd) == path, "guest fd path preserved");
  u32 duplicate;
  equal(vfs::open(path, RDWR | CREATE | EXCL, &duplicate), CELL_EEXIST, "exclusive create error");
  ok(vfs::write(fd, "abcdef", 6, &done), "sequential write"); check(done == 6, "write count");
  ok(vfs::lseek(fd, 2, 0, &position), "position before explicit I/O"); check(position == 2, "seek result");
  data.fill(0); ok(vfs::pread(fd, 4, data.data(), 8, &done), "positioned EOF-short read"); check(done == 2 && std::string(data.data(), 2) == "ef", "pread count/bytes");
  ok(vfs::lseek(fd, 0, 1, &position), "position after pread"); check(position == 2, "pread moved file position");
  ok(vfs::pwrite(fd, 0, "XY", 2, &done), "positioned write"); check(done == 2, "pwrite count");
  ok(vfs::lseek(fd, 0, 1, &position), "position after pwrite"); check(position == 2, "pwrite moved file position");
  ok(vfs::read(fd, data.data(), 2, &done), "read resumes from old cursor"); check(done == 2 && std::string(data.data(), 2) == "cd", "cursor semantics after explicit I/O");
  ok(vfs::pread(fd, 0, nullptr, 0, &done), "empty pread"); check(done == 0, "empty pread count");
  ok(vfs::pwrite(fd, 0, nullptr, 0, &done), "empty pwrite"); check(done == 0, "empty pwrite count");
  equal(vfs::pread(fd, std::numeric_limits<u64>::max(), data.data(), 1, &done), CELL_EINVAL, "pread offset overflow");
  equal(vfs::pwrite(fd, u64(std::numeric_limits<s64>::max()), data.data(), 2, &done), CELL_EINVAL, "pwrite length overflow");
  equal(vfs::ftruncate(fd, std::numeric_limits<u64>::max()), CELL_EINVAL, "truncate offset overflow");
  equal(vfs::lseek(fd, -1, 0, &position), CELL_EINVAL, "negative absolute seek");
  equal(vfs::lseek(fd, 0, 3, &position), CELL_EINVAL, "invalid seek origin");

  // A sparse position above 32 bits must not wrap to the beginning of a save.
  constexpr u64 far = (u64(1) << 32) + 17;
  ok(vfs::pwrite(fd, far, "Z", 1, &done), "64-bit sparse pwrite");
  ok(vfs::fstat(fd, &st), "sparse fstat"); check(st.size == far + 1, "64-bit file size");
  ok(vfs::pread(fd, far, data.data(), 1, &done), "64-bit sparse pread"); check(done == 1 && data[0] == 'Z', "64-bit positioned byte");
  ok(vfs::ftruncate(fd, 6), "sparse file shrink");
  ok(vfs::ftruncate(fd, 10), "file extend");
  data.fill(9); ok(vfs::pread(fd, 6, data.data(), 4, &done), "read extended zero range");
  check(done == 4 && std::all_of(data.begin(), data.begin() + 4, [](char x) { return x == 0; }), "extended bytes not zero");

  // Calls sharing a guest fd serialize native access, without changing its cursor.
  ok(vfs::lseek(fd, 3, 0, &position), "concurrency cursor setup");
  std::atomic<bool> good{true};
  std::vector<std::thread> writers;
  for (u64 n = 0; n < 4; ++n) writers.emplace_back([&, n] {
    const char value = char('0' + n);
    for (unsigned i = 0; i < 128; ++i) {
      char got = 0; u64 count = 0;
      if (vfs::pwrite(fd, 16 + n, &value, 1, &count) || count != 1 ||
          vfs::pread(fd, 16 + n, &got, 1, &count) || count != 1 || got != value) good = false;
    }
  });
  for (auto& t : writers) t.join(); check(good, "concurrent explicit I/O");
  ok(vfs::lseek(fd, 0, 1, &position), "cursor after concurrent explicit I/O"); check(position == 3, "concurrent positioned I/O moved cursor");
  ok(vfs::ftruncate(fd, 6), "final test-file shrink"); ok(vfs::close(fd), "close writable file");
  check(host_text(hdd0 / "汉化目录/存档资源.bin") == "XYcdef", "actual Chinese host file bytes");

  ok(vfs::open(path, RDWR | APPEND, &fd), "append open");
  ok(vfs::lseek(fd, 1, 0, &position), "append cursor setup");
  ok(vfs::pwrite(fd, 0, "Q", 1, &done), "pwrite on append descriptor");
  ok(vfs::lseek(fd, 0, 1, &position), "append positioned cursor"); check(position == 1, "append pwrite moved cursor");
  ok(vfs::write(fd, "!", 1, &done), "sequential append"); ok(vfs::close(fd), "append close");
  check(host_text(hdd0 / "汉化目录/存档资源.bin") == "QYcdef!", "append/positioned write semantics");

  ok(vfs::open(path, 0, &fd), "read access open"); equal(vfs::write(fd, "bad", 3, &done), CELL_EBADF, "write through read descriptor refused");
  ok(vfs::close(fd), "read access close");
  ok(vfs::open(path, 1, &fd), "write access open"); equal(vfs::read(fd, data.data(), 1, &done), CELL_EBADF, "read through write descriptor refused");
  ok(vfs::close(fd), "write access close");
  ok(vfs::truncate(path, 3), "path truncate"); check(host_text(hdd0 / "汉化目录/存档资源.bin") == "QYc", "path truncate data");
  ok(vfs::utime(path, 1400000000, 1400000010), "guest timestamp update");
  ok(vfs::stat(path, &st), "stat timestamps"); check(st.atime == 1400000000 && st.mtime == 1400000010 && !st.readonly, "timestamp/readonly metadata");

  ok(vfs::open("/dev_hdd0/home/00000001/savedata/中文存档.dat", RDWR | CREATE, &fd), "save remount file");
  ok(vfs::write(fd, "save", 4, &done), "scratch save write"); ok(vfs::close(fd), "scratch save close");
  check(host_text(saves / "中文存档.dat") == "save" && !fs::exists(hdd0 / "home/00000001/savedata/中文存档.dat"), "save longest mount real target");
  ok(vfs::opendir("/dev_hdd0/汉化目录", &fd), "directory fd open");
  equal(vfs::read(fd, data.data(), 1, &done), CELL_EBADF, "file I/O through directory fd refused");
  equal(vfs::close(fd), CELL_EBADF, "file close through directory fd refused");
  std::set<std::string> names; bool got; u8 type; std::string name;
  while (true) { ok(vfs::readdir(fd, &got, &type, &name), "directory entry"); if (!got) break; names.insert(name); }
  check(names == std::set<std::string>{".", "..", "存档资源.bin"}, "Chinese directory enumeration");
  ok(vfs::closedir(fd), "directory close"); equal(vfs::closedir(fd), CELL_EBADF, "double directory close");
  equal(vfs::rmdir("/dev_hdd0/汉化目录"), CELL_ENOTEMPTY, "nonempty directory error");
  equal(vfs::unlink("/dev_hdd0/汉化目录"), CELL_EISDIR, "directory unlink error");
  equal(vfs::open("/dev_hdd0/汉化目录", 0, &fd), CELL_EISDIR, "file open on directory error");
  equal(vfs::open("/dev_hdd0/缺失", 0, &fd), CELL_ENOENT, "missing file error");

  fs::create_directory_symlink(disc, hdd1 / "只读别名");
  equal(vfs::open("/dev_hdd1/只读别名/PS3_GAME/USRDIR/游戏文本.bin", RDWR | TRUNC, &fd), CELL_EROFS, "protected-root symlink open guard");
  equal(vfs::truncate("/dev_hdd1/只读别名/PS3_GAME/USRDIR/游戏文本.bin", 0), CELL_EROFS, "protected-root symlink truncate guard");
  equal(vfs::unlink("/dev_hdd1/只读别名/PS3_GAME/USRDIR/游戏文本.bin"), CELL_EROFS, "protected-root symlink unlink guard");
  equal(vfs::rename(path, "/dev_hdd1/只读别名/danger"), CELL_EROFS, "protected-root rename destination guard");
  equal(vfs::rename("/dev_hdd1/只读别名/PS3_GAME/USRDIR/游戏文本.bin", "/dev_hdd1/danger"), CELL_EROFS, "protected-root rename source guard");
  check(host_text(disc / "PS3_GAME/USRDIR/游戏文本.bin") == "disc-data", "read-only source changed");

  acfa::write_text(hdd0 / "汉化目录/替换.dat", "old");
  ok(vfs::rename(path, "/dev_hdd0/汉化目录/替换.dat"), "rename replaces owned scratch file");
  check(host_text(hdd0 / "汉化目录/替换.dat") == "QYc" && !fs::exists(hdd0 / "汉化目录/存档资源.bin"), "rename replacement content");
  ok(vfs::unlink("/dev_hdd0/汉化目录/替换.dat"), "scratch unlink");
  ok(vfs::rmdir("/dev_hdd0/汉化目录"), "empty directory removal");
  equal(vfs::read(999999, data.data(), 1, &done), CELL_EBADF, "invalid guest descriptor");
  equal(vfs::mkdir("/dev_hdd0/missing/child"), CELL_ENOENT, "missing parent mkdir");

  // A read that obtained its shared File before close must finish against A,
  // never against a native descriptor reused by the newly opened B file.
  acfa::write_text(hdd1 / "close-A", std::string(4096, 'A'));
  acfa::write_text(hdd1 / "reuse-B", std::string(4096, 'B'));
  for (unsigned round = 0; round < 64; ++round) {
    u32 old_fd, new_fd;
    ok(vfs::open("/dev_hdd1/close-A", 0, &old_fd), "close-race old open");
    std::atomic<bool> started{false}, safe{true};
    std::thread reader([&] {
      std::array<char, 4096> bytes{};
      started = true;
      for (unsigned n = 0; n < 128; ++n) {
        u64 count = 0;
        const s32 result = vfs::pread(old_fd, 0, bytes.data(), bytes.size(), &count);
        if (result == CELL_EBADF) continue;
        if (result || count != bytes.size() || !std::all_of(bytes.begin(), bytes.end(), [](char x) { return x == 'A'; }))
          safe = false;
      }
    });
    while (!started) std::this_thread::yield();
    ok(vfs::close(old_fd), "concurrent guest close");
    ok(vfs::open("/dev_hdd1/reuse-B", 0, &new_fd), "native descriptor reuse candidate");
    reader.join(); check(safe, "concurrent close reused wrong native descriptor");
    ok(vfs::close(new_fd), "close reused descriptor");
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("pass build-directory scratch parent");
    Scratch scratch(argv[1]); exercise(scratch.root);
    std::puts("PASS native VFS: actual UTF-8 files/mounts, readonly+symlink guards, 64-bit+append positioned I/O without cursor changes, 512 concurrent I/O pairs, 64 concurrent close/reuse rounds, stat/truncate/times/directories/rename/errors");
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "FAIL native VFS: %s\n", e.what()); return 1; }
}
