#include "launcher/setup.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "common/crypto.h"
#include "launcher/download.h"

namespace launcher {
namespace {

// Firmware modules run natively (order defines their load slots; libsre first), as tools\pipeline.ps1.
const char* const kModules = "libsre,libsail,libpamf,libdmux,libdmuxpamf,libadec,libvpost,libatrac3plus,libatxdec,libat3dec";

struct StepDef {
  const char* name;
  double weight;  // share of the whole setup's time (rough)
};
enum : size_t { kToolchain, kTools, kPrepare, kFirmware, kLle, kRecompile, kSpu, kBuild, kShaders, kStepCount };
const StepDef kSteps[kStepCount] = {
    {"Download build tools", 12}, {"Build preparation tools", 6}, {"Prepare game files", 3},   {"Extract system software", 4},
    {"Prepare system modules", 1}, {"Translate game code", 8},    {"Translate SPU code", 5}, {"Build the game", 56},
    {"Compile shaders", 5},
};

struct Tool {
  std::string name, sha, into, check, path, use, url;
};

std::vector<Tool> read_toolchain(const fs::path& src) {
  std::vector<Tool> out;
  std::ifstream f(src / "data" / "toolchain.txt");
  for (std::string line; std::getline(f, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    Tool t;
    if (in >> t.name >> t.sha >> t.into >> t.check >> t.path >> t.use >> t.url) out.push_back(t);
  }
  return out;
}

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

// What a file is, cheaply: size and modification time.
std::string file_stamp(const fs::path& p) {
  std::error_code ec;
  auto size = fs::file_size(p, ec);
  if (ec) return "none";
  auto t = fs::last_write_time(p, ec);
  return std::to_string((unsigned long long)size) + "@" + std::to_string((long long)t.time_since_epoch().count());
}

std::string content_hash(const fs::path& p) {
  if (!is_file(p)) return "none";
  try {
    return acfa::crypto::sha256_file_hex(narrow(p.wstring()));
  } catch (const std::exception&) {
    return "unreadable";
  }
}

fs::path prepared(const SetupConfig& c) { return c.data / "game" / "BLUS30187"; }
fs::path firmware(const SetupConfig& c) { return c.data / "game" / "firmware241"; }
fs::path build_dir(const SetupConfig& c) { return c.data / "build"; }
fs::path tools_dir(const SetupConfig& c) { return c.data / "tools"; }

std::vector<std::string> module_list() {
  std::vector<std::string> v;
  std::string s = kModules;
  for (size_t p = 0; p <= s.size();) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    if (q > p) v.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  return v;
}

// Each step's record: what it was made from. A step is redone when its record differs.
std::string stamp(const SetupConfig& c, size_t step) {
  switch (step) {
    case kPrepare:
      return "eboot " + file_stamp(c.disc / "PS3_GAME" / "USRDIR" / "EBOOT.BIN") + "\nupdate " +
             (c.update.empty() ? std::string("none") : file_stamp(c.update)) + "\nkeys " + content_hash(c.keys);
    case kFirmware:
      return "pup " + file_stamp(c.pup) + "\nkeys " + content_hash(c.keys);
    case kLle:
      return "firmware " + content_hash(c.data / "state" / "firmware.txt") + "\nmodules " + kModules;
    case kRecompile:
      return "prepared " + content_hash(prepared(c) / "manifest.txt") + "\nlle " + content_hash(prepared(c) / "lle" / "manifest.txt") +
             "\nhooks " + content_hash(c.src / "src" / "runtime" / "game" / "hooks.txt") + "\nrecompiler " +
             file_stamp(build_dir(c) / "acfa-recomp.exe");
    case kSpu:
      return "prepared " + content_hash(prepared(c) / "manifest.txt") + "\nmanifest " + content_hash(c.src / "data" / "spu_manifest.txt") +
             "\nspurecomp " + file_stamp(build_dir(c) / "acfa-spurecomp.exe");
    default:
      return "";
  }
}

const char* state_name(size_t step) {
  switch (step) {
    case kPrepare: return "prepare";
    case kFirmware: return "firmware";
    case kLle: return "lle";
    case kRecompile: return "recompile";
    case kSpu: return "spu";
    default: return "";
  }
}

std::string read_state(const SetupConfig& c, size_t step) {
  std::ifstream f(c.data / "state" / (std::string(state_name(step)) + ".txt"), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void write_state(const SetupConfig& c, size_t step, const std::string& s) {
  std::error_code ec;
  fs::create_directories(c.data / "state", ec);
  std::ofstream(c.data / "state" / (std::string(state_name(step)) + ".txt"), std::ios::binary) << s;
}

bool outputs_present(const SetupConfig& c, size_t step) {
  switch (step) {
    case kPrepare: return is_file(prepared(c) / "eboot.elf") && is_file(prepared(c) / "manifest.txt");
    case kFirmware: return is_dir(firmware(c) / "dev_flash" / "sys" / "external");
    case kLle:
      for (const auto& m : module_list())
        if (!is_file(prepared(c) / "lle" / (m + ".elf"))) return false;
      return is_file(prepared(c) / "lle" / "manifest.txt");
    case kRecompile: return is_file(c.data / "gen" / "ppu" / "ppu_functab.cpp");
    case kSpu: return is_dir(c.data / "gen" / "spu");
    default: return false;
  }
}

bool step_current(const SetupConfig& c, size_t step) { return outputs_present(c, step) && read_state(c, step) == stamp(c, step); }

std::wstring tool_path_prefix(const SetupConfig& c) {
  std::wstring p;
  for (const Tool& t : read_toolchain(c.src))
    if (t.use == "build") p += (p.empty() ? L"" : L";") + (tools_dir(c) / widen(t.path)).wstring();
  return p;
}

fs::path cmake_exe(const SetupConfig& c) {
  for (const Tool& t : read_toolchain(c.src))
    if (t.name == "cmake") return tools_dir(c) / widen(t.path) / "cmake.exe";
  return "cmake.exe";
}

std::wstring w(const fs::path& p) { return p.wstring(); }
std::wstring fwd(const fs::path& p) { return p.generic_wstring(); }  // CMake prefers forward slashes

bool x86_64_v3() {
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx") && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") &&
         __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("sse4.2") &&
         __builtin_cpu_supports("popcnt");
}

unsigned long long free_space(fs::path p) {
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  ULARGE_INTEGER avail{};
  if (p.empty() || !GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) return 0;
  return avail.QuadPart;
}

}  // namespace

Setup::Setup() {
  for (const StepDef& s : kSteps) view_.steps.push_back({s.name, StepState::Waiting});
}

Setup::~Setup() {
  cancel();
  if (worker_.joinable()) worker_.join();
  if (log_file_) std::fclose(log_file_);
}

fs::path Setup::game_exe(const SetupConfig& c) { return build_dir(c) / "acfa.exe"; }

bool Setup::ready(const SetupConfig& c) {
  if (!is_file(game_exe(c))) return false;
  for (size_t s : {kPrepare, kFirmware, kLle, kRecompile, kSpu})
    if (!step_current(c, s)) return false;
  std::ifstream f(c.data / "state" / "built.txt", std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str() == stamp(c, kRecompile) + "\n" + stamp(c, kSpu);
}

unsigned long long Setup::space_needed(const SetupConfig& c) {
  // Measured on the development PC: tools 0.92 GB (+0.25 GB of archives while unpacking), prepared
  // files 0.16, generated code 0.16, build 0.45, the game's system cache 1.2, shaders and SPU code
  // 0.03, saves under 0.01.
  const double gb = 1024.0 * 1024 * 1024;
  double need = 0;
  if (!is_dir(tools_dir(c) / "cmake-4.4.3-windows-x86_64")) need += 1.2;
  if (!is_file(prepared(c) / "eboot.elf")) need += 0.2;
  if (!is_file(c.data / "gen" / "ppu" / "ppu_functab.cpp")) need += 0.2;
  if (!is_file(game_exe(c))) need += 0.5;
  if (!is_dir(c.data / "hdd1" / "model")) need += 1.3;
  return (unsigned long long)(need * gb);
}

void Setup::log(const std::string& line) {
  std::lock_guard lk(mu_);
  view_.lines.push_back(line);
  if (log_file_) {
    std::fputs(line.c_str(), log_file_);
    std::fputc('\n', log_file_);
    std::fflush(log_file_);
  }
}

void Setup::set_step(size_t i, StepState s) {
  {
    std::lock_guard lk(mu_);
    view_.steps[i].state = s;
    if (s == StepState::Running) {
      step_ = i;
      view_.status = kSteps[i].name;
    }
  }
  if (s == StepState::Running) set_fraction(0);
}

void Setup::set_fraction(double f) {
  double total = 0, done = 0;
  for (size_t i = 0; i < kStepCount; ++i) {
    total += kSteps[i].weight;
    if (i < step_) done += kSteps[i].weight;
  }
  std::lock_guard lk(mu_);
  view_.fraction = (done + kSteps[step_].weight * std::min(1.0, std::max(0.0, f))) / total;
}

void Setup::set_status(const std::string& s) {
  std::lock_guard lk(mu_);
  view_.status = s;
}

SetupView Setup::view() {
  std::lock_guard lk(mu_);
  SetupView v = view_;
  view_.lines.clear();
  v.running = running_.load();
  return v;
}

void Setup::cancel() {
  cancel_ = true;
  runner_.cancel();
}

void Setup::start(const SetupConfig& c, std::function<void()> notify) {
  if (running_) return;
  if (worker_.joinable()) worker_.join();
  notify_ = std::move(notify);
  cancel_ = false;
  runner_.reset();
  {
    std::lock_guard lk(mu_);
    for (auto& s : view_.steps) s.state = StepState::Waiting;
    view_.fraction = 0;
    view_.status.clear();
    view_.lines.clear();
    view_.finished = view_.ok = view_.cancelled = false;
    view_.error.clear();
  }
  running_ = true;
  worker_ = std::thread([this, c] { run(c); });
}

void Setup::run(SetupConfig c) {
  std::error_code ec;
  fs::create_directories(c.data, ec);
  if (log_file_) std::fclose(log_file_);
  log_file_ = _wfopen((c.data / "launcher.log").c_str(), L"ab");
  {
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log(std::string("==== setup ") + when + " ====");
  }
  auto fail = [&](const std::string& why) {
    log("ERROR: " + why);
    std::lock_guard lk(mu_);
    view_.steps[step_].state = StepState::Failed;
    view_.error = why;
    view_.finished = true;
    view_.cancelled = cancel_.load();
  };
  auto notify = [&] {
    if (notify_) notify_();
  };
  std::wstring path = tool_path_prefix(c);
  // Runs a tool; ninja's "[n/m]" lines move the step's progress.
  std::string last_error;
  auto exec = [&](const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd) -> bool {
    std::string line0 = narrow(exe.filename().wstring());
    for (const auto& a : args) line0 += " " + narrow(a);
    log("> " + line0);
    last_error.clear();
    RunResult r = runner_.run(exe, args, cwd, path, [&](const std::string& line) {
      log(line);
      if (line.size() > 3 && line[0] == '[') {
        unsigned a = 0, b = 0;
        if (std::sscanf(line.c_str(), "[%u/%u]", &a, &b) == 2 && b) set_fraction(double(a) / b);
      }
      if (line.rfind("error", 0) == 0 || line.find(": error:") != std::string::npos || line.rfind("FAILED", 0) == 0) last_error = line;
      notify();
    });
    if (r.cancelled || cancel_) return false;
    if (!r.started) {
      last_error = r.error;
      return false;
    }
    if (r.exit_code != 0) {
      if (last_error.empty()) last_error = line0.substr(0, line0.find(' ')) + " ended with code " + std::to_string(r.exit_code);
      return false;
    }
    return true;
  };
  auto finish_step = [&](size_t i, bool skipped) {
    set_step(i, skipped ? StepState::Skipped : StepState::Done);
    notify();
  };
  auto run_steps = [&]() -> bool {
    // Checks first: CPU, disk space.
    if (!x86_64_v3()) {
      step_ = kToolchain;
      fail("This CPU lacks AVX2/FMA3/BMI2 (x86-64-v3). The port needs an Intel Haswell (2013) or AMD Excavator/Zen CPU or newer.");
      return false;
    }
    unsigned long long need = space_needed(c), have = free_space(c.data);
    if (have && have < need) {
      step_ = kToolchain;
      fail(acfa::strprintf("Not enough disk space in %s: %.1f GB needed, %.1f GB free.", narrow(c.data.wstring()).c_str(),
                           double(need) / (1 << 30), double(have) / (1 << 30)));
      return false;
    }
    // 1. Toolchain.
    set_step(kToolchain, StepState::Running);
    notify();
    std::vector<Tool> tools = read_toolchain(c.src);
    if (tools.empty()) {
      fail("data\\toolchain.txt is missing from the port's folder.");
      return false;
    }
    bool fetched = false;
    size_t n_build = 0, i_build = 0;
    for (const Tool& t : tools) n_build += t.use == "build";
    for (const Tool& t : tools) {
      if (t.use != "build") continue;
      ++i_build;
      if (is_dir(tools_dir(c) / widen(t.check))) continue;
      fetched = true;
      fs::path zip = c.data / "downloads" / widen(t.url.substr(t.url.rfind('/') + 1));
      if (!is_file(zip) || content_hash(zip) != t.sha) {
        set_status("Downloading " + t.name);
        log("downloading " + t.url);
        notify();
        std::string err = download(widen(t.url), zip, [&](unsigned long long got, unsigned long long total) {
          if (!total) return;
          set_fraction((double(i_build - 1) + double(got) / double(total)) / double(n_build));
          set_status(acfa::strprintf("Downloading %s: %.0f / %.0f MB", t.name.c_str(), got / 1048576.0, total / 1048576.0));
          notify();
        }, cancel_);
        if (cancel_) return false;
        if (!err.empty()) {
          fail("Download of " + t.name + " failed: " + err);
          return false;
        }
        if (content_hash(zip) != t.sha) {
          fs::remove(zip, ec);
          fail("The downloaded " + t.name + " archive does not match its pinned SHA-256 (data\\toolchain.txt).");
          return false;
        }
      }
      set_status("Unpacking " + t.name);
      notify();
      fs::path into = t.into == "." ? tools_dir(c) : tools_dir(c) / widen(t.into);
      fs::create_directories(into, ec);
      wchar_t sys[MAX_PATH];
      GetSystemDirectoryW(sys, MAX_PATH);
      if (!exec(fs::path(sys) / "tar.exe", {L"-xf", w(zip), L"-C", w(into)}, c.data)) {
        fail("Unpacking " + t.name + " failed: " + last_error);
        return false;
      }
      fs::remove(zip, ec);  // the unpacked tools stay; the archive is not needed again
    }
    finish_step(kToolchain, !fetched);
    // 2. Preparation tools (configure: the generated code is picked up when it exists).
    auto configure = [&] {
      return exec(cmake_exe(c),
                  {L"-S", fwd(c.src), L"-B", fwd(build_dir(c)), L"-G", L"Ninja", L"-DCMAKE_BUILD_TYPE=Release", L"-DCMAKE_C_COMPILER=clang",
                   L"-DCMAKE_CXX_COMPILER=clang++", L"-DACFA_GEN_DIR=" + fwd(c.data / "gen" / "ppu"),
                   L"-DACFA_SPU_GEN_DIR=" + fwd(c.data / "gen" / "spu")},
                  c.data);
    };
    set_step(kTools, StepState::Running);
    notify();
    if (!configure() || !exec(cmake_exe(c), {L"--build", fwd(build_dir(c)), L"--target", L"acfa-prep", L"acfa-recomp", L"acfa-spurecomp"}, c.data)) {
      fail("Building the preparation tools failed: " + last_error);
      return false;
    }
    finish_step(kTools, false);
    fs::path prep = build_dir(c) / "acfa-prep.exe";
    // 3-7: the game files, each only when its inputs changed.
    struct Work {
      size_t step;
      std::function<bool()> run;
      const char* what;
    };
    std::vector<Work> work = {
        {kPrepare,
         [&] {
           return exec(prep, {L"prepare", L"--game", w(c.disc), L"--out", w(prepared(c)), L"--keys", w(c.keys), L"--update",
                              c.update.empty() ? std::wstring(L"none") : w(c.update)},
                       c.data);
         },
         "Preparing the game files failed (decryption: check the key file)"},
        {kFirmware, [&] { return exec(prep, {L"firmware", w(c.pup), w(firmware(c)), L"--keys", w(c.keys)}, c.data); },
         "Extracting the system software failed"},
        {kLle,
         [&] {
           return exec(prep, {L"lle", L"--firmware", w(firmware(c)), L"--out", w(prepared(c)), L"--modules", widen(kModules), L"--keys", w(c.keys)},
                       c.data);
         },
         "Preparing the system modules failed"},
        {kRecompile,
         [&] {
           fs::remove_all(c.data / "gen" / "ppu", ec);
           std::vector<std::wstring> a = {L"emit", w(prepared(c) / "eboot.elf"), w(c.data / "gen" / "ppu")};
           for (const auto& m : module_list()) a.insert(a.end(), {L"--module", w(prepared(c) / "lle" / widen(m + ".elf"))});
           a.insert(a.end(), {L"--hooks", w(c.src / "src" / "runtime" / "game" / "hooks.txt")});
           return exec(build_dir(c) / "acfa-recomp.exe", a, c.data);
         },
         "Translating the game code failed"},
        {kSpu,
         [&] {
           fs::remove_all(c.data / "gen" / "spu", ec);
           fs::create_directories(c.data / "gen" / "spu", ec);
           return exec(prep, {L"spu-records", L"--manifest", w(c.src / "data" / "spu_manifest.txt"), L"--out", w(c.data / "spu_cache"), w(prepared(c))},
                       c.data) &&
                  exec(build_dir(c) / "acfa-spurecomp.exe", {w(c.data / "gen" / "spu"), w(c.data / "spu_cache")}, c.data);
         },
         "Translating the SPU code failed"},
    };
    for (Work& wk : work) {
      if (step_current(c, wk.step)) {
        finish_step(wk.step, true);
        continue;
      }
      set_step(wk.step, StepState::Running);
      notify();
      std::string s = stamp(c, wk.step);
      if (!wk.run()) {
        if (!cancel_) fail(std::string(wk.what) + ": " + last_error);
        return false;
      }
      write_state(c, wk.step, wk.step == kRecompile || wk.step == kSpu ? stamp(c, wk.step) : s);
      finish_step(wk.step, false);
    }
    // 8. The game.
    set_step(kBuild, StepState::Running);
    notify();
    if (!configure() || !exec(cmake_exe(c), {L"--build", fwd(build_dir(c)), L"--target", L"acfa"}, c.data)) {
      if (!cancel_) fail("Building the game failed: " + last_error);
      return false;
    }
    {
      std::ofstream(c.data / "state" / "built.txt", std::ios::binary) << stamp(c, kRecompile) + "\n" + stamp(c, kSpu);
    }
    finish_step(kBuild, false);
    // 9. Shaders known from the manifest, compiled before the first run.
    set_step(kShaders, StepState::Running);
    notify();
    if (!exec(game_exe(c),
              {L"--data", w(c.data), L"--disc", w(c.disc), L"--warm-shaders", L"--shader-manifest", w(c.src / "data" / "shader_manifest.txt"),
               L"--keep-runs", L"10"},
              c.data)) {
      if (!cancel_) fail("Compiling the shaders failed: " + last_error);
      return false;
    }
    finish_step(kShaders, false);
    return true;
  };
  bool ok = run_steps();
  {
    std::lock_guard lk(mu_);
    view_.finished = true;
    view_.ok = ok;
    view_.cancelled = cancel_.load();
    if (ok) {
      view_.fraction = 1;
      view_.status = "Ready";
    } else if (cancel_) {
      view_.status = "Cancelled";
      view_.steps[step_].state = StepState::Failed;
    }
  }
  log(ok ? "setup finished" : cancel_ ? "setup cancelled" : "setup failed");
  if (log_file_) {
    std::fclose(log_file_);
    log_file_ = nullptr;
  }
  running_ = false;
  if (notify_) notify_();
}

}  // namespace launcher
