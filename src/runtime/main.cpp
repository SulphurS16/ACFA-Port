// acfa: runtime host for the recompiled ARMORED CORE for Answer executable.
//   acfa [--disc <dir>] [--prepared <dir>] [--hdd0 <dir>] [--seconds N] [--trace-hle] [--log-level trace|debug|info]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include "common/file.h"
#include "runtime/bindings.h"
#include "runtime/config.h"
#include "runtime/datawatch.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/shader_warm.h"
#include "runtime/gpu/window.h"
#include "runtime/rsx/rsx.h"
#include "runtime/settings.h"
#include "runtime/sysutil.h"
#include "runtime/hle.h"
#include "runtime/input.h"
#include "runtime/loader.h"
#include "runtime/lv2/lv2.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/ppu_thread.h"
#include "runtime/profiler.h"
#include "runtime/reservation.h"

namespace rt {
Config& cfg() {
  static Config c;
  return c;
}
void vdec_dump();
std::string audio_report();  // cellAudio.cpp: output statistics since the last call
void lv2_init();  // lv2 subsystems that need setup after memory/loader
namespace lv2 {
void spu_dump();
void sync_dump();
}
void lwmutex_dump();
namespace gpu {
extern std::atomic<int> g_capture_requests;
}
}  // namespace rt

using namespace rt;
namespace fs = std::filesystem;

namespace {

// Run directories are named after their UTC start time (YYYYMMDD-HHMMSS); with --keep-runs N only
// the newest N stay (the launcher's runs keep their logs without filling the disk).
void prune_runs(const fs::path& runs, unsigned keep) {
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(runs, ec)) {
    std::string n = e.path().filename().string();
    if (e.is_directory(ec) && n.size() == 15 && n[8] == '-' &&
        std::all_of(n.begin(), n.end(), [](char ch) { return ch == '-' || (ch >= '0' && ch <= '9'); }))
      dirs.push_back(e.path());
  }
  if (dirs.size() <= keep) return;
  std::sort(dirs.begin(), dirs.end());
  for (size_t i = 0; i + keep < dirs.size(); ++i) {
    acfa::guard_write(dirs[i]);
    fs::remove_all(dirs[i], ec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config& c = cfg();
  fs::path root = fs::current_path();
  // Where everything machine-local lives: --data DIR (the launcher's data folder), by default
  // local\ in the current directory (the development layout). --settings FILE replaces the
  // shared settings file for a run (tests).
  fs::path data = root / "local";
  std::string settings_override;
  for (int i = 1; i + 1 < argc; ++i) {
    std::string a = argv[i];
    if (a == "--data") data = fs::absolute(argv[i + 1]);
    else if (a == "--settings") settings_override = argv[i + 1];
    else if (a == "--keep-runs") c.keep_runs = unsigned(std::strtoul(argv[i + 1], nullptr, 10));
  }
  c.data_dir = data.string();
  // The user's game folder: by default the disc folder inside the current directory; the first
  // line of <data>\config\disc_root.txt names another one; --disc overrides both.
  c.disc_root = (root / "Armored Core - For Answer (USA)").string();
  if (std::ifstream f(data / "config" / "disc_root.txt"); f) {
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '"')) line.pop_back();
    size_t b = line.find_first_not_of(" \"");
    if (b != std::string::npos && !line.empty()) c.disc_root = line.substr(b);
  }
  c.prepared = (data / "game" / "BLUS30187").string();
  c.hdd0 = (data / "hdd0").string();
  c.hdd1 = (data / "hdd1").string();
  c.flash = (data / "game" / "firmware241" / "dev_flash").string();
  c.spu_cache = (data / "spu_cache").string();
  c.shader_cache = (data / "shader_cache").string();
  c.shader_records = (data / "shader_records").string();
  c.settings_file = settings_override.empty() ? acfa::settings::file_in(data).string() : settings_override;
  std::string shader_manifest = (root / "data" / "shader_manifest.txt").string();
  // Run directory: <data>\runs\<UTC timestamp>
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &now);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  fs::path run = data / "runs" / stamp;
  try {
    acfa::guard_write(run);
    fs::create_directories(run);
    if (c.keep_runs) prune_runs(data / "runs", c.keep_runs);
    c.run_dir = run.string();
    log_open((run / "acfa.log").string());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cannot create the run directory %s: %s\n", run.string().c_str(), e.what());
    return 1;
  }
  // Shared settings (launcher and PC SETTINGS), before the command line overrides them.
  std::vector<std::string> settings_notes;
  settings::init(c.settings_file, &settings_notes);
  {
    acfa::settings::Display d = settings::display();
    c.fps = d.fps;
    c.pacing = d.pacing;
    c.fullscreen = d.mode == acfa::settings::Mode::Borderless;
    c.window_w = d.width;
    c.window_h = d.height;
  }
  std::string level = "info";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--data" || a == "--settings" || a == "--keep-runs") next();  // read above
    else if (a == "--disc") c.disc_root = next();
    else if (a == "--prepared") c.prepared = next();
    else if (a == "--hdd0") c.hdd0 = next();
    else if (a == "--flash") c.flash = next();
    else if (a == "--seconds") c.seconds = std::atof(next().c_str());
    else if (a == "--trace-hle") c.trace_hle = true;
    else if (a == "--trace-lle") c.trace_lle = true;
    else if (a == "--log-level") level = next();
    else if (a == "--headless") c.headless = true;
    else if (a == "--lenient") c.lenient = true;
    else if (a == "--trace-spu" && i + 1 < argc) c.trace_spu = argv[++i];
    else if (a == "--trace-spu-limit" && i + 1 < argc) c.trace_spu_limit = unsigned(std::stoul(argv[++i]));
    else if (a == "--trace-spu-cats" && i + 1 < argc) c.trace_spu_cats = unsigned(std::stoul(argv[++i]));
    else if (a == "--profile") c.profile = next();
    else if (a == "--spu-collect") c.spu_collect = true;
    else if (a == "--shader-collect") c.shader_collect = true;
    else if (a == "--warm-shaders") c.warm_shaders = true;
    else if (a == "--spu-interp") c.spu_interp = true;
    else if (a == "--trace-rsx") c.trace_rsx = unsigned(std::stoul(next()));
    else if (a == "--trace-rsx-at") c.trace_rsx_at = std::atof(next().c_str());
    else if (a == "--dump-shaders") c.dump_shaders = true;
    else if (a == "--shot-surface") c.shot_surface = std::stol(next(), nullptr, 0);
    else if (a == "--watch-lines") c.watch_lines = next();
    else if (a == "--watch-access") c.watch_access = next();
    else if (a == "--dialog") c.dialog_script = next();
    else if (a == "--profile-at") c.profile_at = std::atof(next().c_str());
    else if (a == "--profile-split") c.profile_split = std::atof(next().c_str());
    else if (a == "--frame-report") c.frame_report = std::atof(next().c_str());
    else if (a == "--skip-fp") c.skip_fp = unsigned(std::stoul(next(), nullptr, 0));
    else if (a == "--input") c.input_script = next();
    else if (a == "--control") c.control_file = next();
    else if (a == "--isolate-input") c.isolate_input = true;
    else if (a == "--gpu-submit") c.gpu_submit_thread = next() != "off";
    else if (a == "--priority") c.priority = next();
    else if (a == "--gpu-timing") c.gpu_timing = 1;
    else if (a == "--gpu-timing-draws") c.gpu_timing = 2;
    else if (a == "--spu-cache") c.spu_cache = next();
    else if (a == "--shader-cache") c.shader_cache = next();
    else if (a == "--audio-capture") c.audio_capture = true;
    else if (a == "--gcm-wait") c.gcm_wait_spin = next() == "spin";
    else if (a == "--pacing") c.pacing = next();
    else if (a == "--fps") { std::string v = next(); c.fps = v == "display" ? 0 : std::max(30.0, std::atof(v.c_str())); }
    else if (a == "--windowed") c.fullscreen = false, c.windowed = true;
    else if (a == "--shader-manifest") shader_manifest = next();
    else if (a == "--fullscreen") c.fullscreen = true;
    else if (a == "--affinity") c.affinity = std::strtoull(next().c_str(), nullptr, 16);
    else if (a == "--shot-interval") c.shot_interval = std::atof(next().c_str());
    else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  acfa::add_protected_root(c.disc_root);
  log_set_level(level == "trace" ? Level::Trace : level == "debug" ? Level::Debug : Level::Info);
  hle::g_trace = c.trace_hle;
  for (size_t p = 0; p < c.watch_lines.size();) {  // --watch-lines ea:size,...
    size_t q = c.watch_lines.find(',', p);
    std::string item = c.watch_lines.substr(p, q == std::string::npos ? std::string::npos : q - p);
    size_t colon = item.find(':');
    if (colon != std::string::npos)
      res::watch_add(u32(std::stoul(item.substr(0, colon), nullptr, 0)), u32(std::stoul(item.substr(colon + 1), nullptr, 0)));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  prof::start(c.profile);
  LOG_I("main", "run dir %s", c.run_dir.c_str());
  if (c.affinity) {  // --affinity MASK (hex): fewer logical CPUs, to estimate smaller machines
    if (SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(c.affinity)))
      LOG_I("main", "--affinity: limited to logical CPUs 0x%llx (%d of them)", c.affinity, __builtin_popcountll(c.affinity));
    else
      LOG_W("main", "--affinity 0x%llx not applied (error %lu)", c.affinity, GetLastError());
  }
  if (c.warm_shaders) {  // pipeline step: shader cache from the manifest, no game run
    gpu::WarmResult w = gpu::warm_shaders(shader_manifest, c.disc_root, c.prepared);
    if (!w.error.empty()) {
      LOG_E("warm", "%s", w.error.c_str());
      std::fprintf(stderr, "%s\n", w.error.c_str());
      return 1;
    }
    std::string s = acfa::strprintf("shader warm-up: %u programs, %u compiled, %u already cached, %u skipped, %u failed; %.1f s on %u threads",
                                    w.programs, w.compiled, w.cached, w.skipped, w.failed, w.seconds, w.threads);
    LOG_I("warm", "%s", s.c_str());
    std::printf("%s\n", s.c_str());
    return w.failed ? 1 : 0;
  }
  for (const auto& n : settings_notes) LOG_I("main", "%s", n.c_str());
  if (!c.watch_access.empty() && !datawatch::configure(c.watch_access)) return 2;
  set_fatal_hook([] {
    prof::write_report(cfg().run_dir + "/profile.txt");
    datawatch::write_report(cfg().run_dir + "/access.txt");
  });
  // Optional process priority: with other programs keeping every core busy, each handoff between
  // the game's threads otherwise waits for a time slice (the frame rate collapses); above normal,
  // the game's threads run when they become ready. Off by default: it takes CPU time from other
  // programs while the game runs.
  if (!c.priority.empty()) {
    DWORD cls = c.priority == "high" ? HIGH_PRIORITY_CLASS : c.priority == "above-normal" ? ABOVE_NORMAL_PRIORITY_CLASS : 0;
    if (cls && SetPriorityClass(GetCurrentProcess(), cls))
      LOG_I("main", "process priority: %s", c.priority.c_str());
    else
      LOG_W("main", "--priority %s not applied (use above-normal or high)", c.priority.c_str());
  }
  if (c.isolate_input) LOG_I("input", "--isolate-input: the real keyboard, mouse and pads are ignored (scripted input only)");
  if (c.lenient) LOG_W("main", "SURVEY MODE (--lenient): unimplemented calls return 0; results are not evidence of working behaviour");
  LOG_I("main", "disc %s, prepared %s, hdd0 %s", c.disc_root.c_str(), c.prepared.c_str(), c.hdd0.c_str());

  if (!vm::init()) return 1;
  install_fault_handler();
  ppu_runtime_init();
  ExecInfo ex;
  try {
    // Firmware modules relocated by acfa-prep (LLE) load first so the game binds to them.
    fs::path lle = fs::path(c.prepared) / "lle";
    if (fs::exists(lle))
      for (const auto& e : fs::directory_iterator(lle))
        if (e.path().extension() == ".elf") load_lle_module(acfa::read_file(e.path()));
    bind_lle_imports();
    ex = load_exec(acfa::read_file(fs::path(c.prepared) / "eboot.elf"));
    lv2::g_sdk_version = ex.sdk_version;
    fs::path self = fs::path(c.disc_root) / "PS3_GAME" / "USRDIR" / "EBOOT.BIN";
    if (lv2::read_self_identity(self.string()))
      LOG_I("main", "SELF auth id 0x%016llx, control flags 0x%08x", (unsigned long long)lv2::g_auth_id, lv2::g_ctrl_flags1);
    else
      LOG_W("main", "cannot read the SELF header of %s; program authority ID unknown", self.string().c_str());
  } catch (const std::exception& e) {
    LOG_E("main", "load failed: %s", e.what());
    return 1;
  }
  lv2_init();
  if (!c.headless) gpu::window_dpi_aware();
  if (!c.headless && !gpu::window_create("ARMORED CORE for Answer - native port", c.window_w, c.window_h, true))
    LOG_E("main", "window creation failed; running without presentation");
  else if (!c.headless && c.fullscreen)
    gpu::window_set_fullscreen(true);
  else if (!c.headless && !c.windowed && settings::display().mode == acfa::settings::Mode::Fullscreen) {
    acfa::settings::Display d = settings::display();  // exclusive fullscreen from the first frame
    gpu::request_exclusive(true, u32(d.fs_width), u32(d.fs_height), u32(d.fs_hz));
  }
  start_main_thread(ex.entry_opd, ex.primary_stack, ex.primary_prio, ex.tls_vaddr, ex.tls_filesz, ex.tls_memsz,
                    ex.malloc_pagesize, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
  auto t0 = std::chrono::steady_clock::now();
  double close_at = -1, next_shot = c.shot_interval > 0 ? c.shot_interval : -1, next_title = 1;
  static std::mutex stop_mu;  // the watchdog and the main loop may both decide to stop
  auto stop = [&](const char* why) {
    stop_mu.lock();  // never released: the process ends here
    LOG_I("main", "%s; stopping", why);
    LOG_I("main", "%s", rsx::stats().c_str());
    {
      u32 compiled = 0, cached = 0;
      double secs = 0;
      gpu::shader_stats(&compiled, &cached, &secs);
      LOG_I("main", "shaders: %u compiled (%.1f s), %u from the shader cache", compiled, secs, cached);
    }
    prof::write_report(c.run_dir + "/profile.txt");
    prof::cpu_report();
    thread_dump();
    lwmutex_dump();
    lv2::sync_dump();
    lv2::spu_dump();
    vdec_dump();
    res::watch_dump(c.run_dir + "/watch.txt");
    datawatch::write_report(c.run_dir + "/access.txt");
    LOG_I("dump", "PPU conditional stores refused by a change elsewhere in the 128-byte granule: %llu",
          (unsigned long long)ppu::granule_conflicts());
    rsx::dump();
    dump_hle_stats();
    log_flush();
    ExitProcess(0);
  };
  // Watchdog: the time limit and log flushing keep working while the main thread is busy in a
  // modal system dialog (or stuck), so bounded runs stay bounded and keep their logs.
  std::thread([&] {
    u64 last_flips = 0;
    double last_fps_at = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (now - last_fps_at >= 5) {  // displayed frame rate, for performance tracking
        u64 f = rsx::flips();
        std::string busy = prof::cpu_interval(4);
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        static u64 last_faults = 0;
        // Machine-wide CPU use next to ours: load from other programs shows up in measurements.
        auto ft = [](const FILETIME& t) { return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        FILETIME si, sk, su, pc, pe, pk, pu;
        GetSystemTimes(&si, &sk, &su);
        GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
        static u64 last_idle = 0, last_total = 0, last_ours = 0;
        u64 idle = ft(si), total = ft(sk) + ft(su), ours = ft(pk) + ft(pu);  // kernel time includes idle
        double sys_busy = total > last_total ? 100.0 * (1.0 - double(idle - last_idle) / double(total - last_total)) : 0;
        double our_share = total > last_total ? 100.0 * double(ours - last_ours) / double(total - last_total) : 0;
        if (last_fps_at > 0)
          LOG_I("perf", "%.1f frames/s; CPU: machine %.0f%%, this process %.0f%%; %.0f page faults/s, working set %llu MiB; "
                "busiest threads: %s",
                double(f - last_flips) / (now - last_fps_at), sys_busy, our_share,
                double(pmc.PageFaultCount - last_faults) / (now - last_fps_at), (unsigned long long)(pmc.WorkingSetSize >> 20),
                busy.c_str());
        if (last_fps_at > 0) LOG_I("perf", "%s", audio_report().c_str());
        if (last_fps_at > 0 && !c.headless) LOG_I("perf", "%s", gpu::display::report().c_str());
        last_faults = pmc.PageFaultCount;
        last_idle = idle;
        last_total = total;
        last_ours = ours;
        last_flips = f;
        last_fps_at = now;
      }
      log_flush();
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (c.seconds > 0 && el >= c.seconds + 2) stop("--seconds limit reached (watchdog)");
      if (input_quit_requested()) stop("quit requested in the control file");
    }
  }).detach();
  for (;;) {
    if (!c.headless) gpu::window_pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (gpu::window_close_requested() && close_at < 0) {
      LOG_I("main", "window closed: sending CELL_SYSUTIL_REQUEST_EXITGAME to the game");
      sysutil::post_event(0x0101, 0);
      close_at = el;
    }
    if (close_at >= 0 && el - close_at > 5.0) stop("game did not exit within 5 s of the exit request");
    if (next_shot > 0 && el >= next_shot) {
      ++gpu::g_capture_requests;
      next_shot += c.shot_interval;
    }
    if (el >= next_title) {
      next_title = el + 1;
      if (!c.headless) gpu::window_set_title("ARMORED CORE for Answer - native port | " + rsx::stats());
    }
    if (c.seconds > 0 && el >= c.seconds) stop("--seconds limit reached");
  }
}
