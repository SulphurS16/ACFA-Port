// ACFA-Launcher: the player's way in. Chooses the game files (disc folder, update package, system
// software, key file) and the install folder, runs the first-time preparation and build
// (setup.h), sets the frame rate, display mode and resolution (the shared settings file, also
// changed in game under SYSTEM > PC SETTINGS), backs up and restores saves, and starts the game.
// Contains no game data or game-derived code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "common/bytes.h"
#include "common/ini.h"
#include "common/settings_file.h"
#include "launcher/inputs.h"
#include "launcher/process.h"
#include "launcher/setup.h"

namespace fs = std::filesystem;
using namespace launcher;

namespace {

constexpr UINT WM_SETUP = WM_APP + 1, WM_CHECKED = WM_APP + 2, WM_GAME_EXIT = WM_APP + 3;
enum Id {
  kDiscEdit = 100, kUpdateEdit, kPupEdit, kKeysEdit,
  kDiscBrowse = 110, kUpdateBrowse, kPupBrowse, kKeysBrowse,
  kDiscState = 120, kUpdateState, kPupState, kKeysState,
  kDataEdit = 130, kDataBrowse, kDataState,
  kPrepare = 140, kCancel, kOpenLog, kProgress, kStatus, kLog,
  kFps = 150, kMode, kResolution,
  kSavesPath = 160, kSavesOpen, kSavesBackup, kSavesRestore,
  kPlay = 170, kPlayState,
  kLabel = 200,
};

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font = nullptr, g_big = nullptr;
UINT g_dpi = 96;
int S(int v) { return MulDiv(v, int(g_dpi), 96); }

fs::path g_src, g_ini_path;
SetupConfig g_cfg;
Setup g_setup;
HANDLE g_game = nullptr;
bool g_ready = false;

std::mutex g_check_mu;
Status g_checks[4];
std::atomic<int> g_check_gen{0};

HWND item(int id) { return GetDlgItem(g_wnd, id); }

std::wstring get_text(int id) {
  int n = GetWindowTextLengthW(item(id));
  std::wstring s(size_t(n) + 1, L'\0');
  GetWindowTextW(item(id), s.data(), n + 1);
  s.resize(size_t(n));
  return s;
}
void set_text(int id, const std::wstring& s) { SetWindowTextW(item(id), s.c_str()); }

// ---- launcher.ini: where the player's files and the install folder are ----
void load_paths() {
  acfa::Ini ini = acfa::Ini::load(g_ini_path);
  auto p = [&](const char* k) { return fs::path(widen(ini.get_or("paths", k, ""))); };
  g_cfg.disc = p("disc");
  g_cfg.update = p("update");
  g_cfg.pup = p("system_software");
  g_cfg.keys = p("keys");
  g_cfg.data = p("install");
}

void save_paths() {
  try {
    acfa::Ini ini = acfa::Ini::load(g_ini_path);
    if (ini.empty()) ini = acfa::Ini::parse("# ARMORED CORE for Answer launcher: the locations chosen in the launcher\n");
    ini.set("paths", "disc", narrow(g_cfg.disc.wstring()));
    ini.set("paths", "update", narrow(g_cfg.update.wstring()));
    ini.set("paths", "system_software", narrow(g_cfg.pup.wstring()));
    ini.set("paths", "keys", narrow(g_cfg.keys.wstring()));
    ini.set("paths", "install", narrow(g_cfg.data.wstring()));
    std::error_code ec;
    fs::create_directories(g_ini_path.parent_path(), ec);
    ini.save(g_ini_path);
  } catch (const std::exception&) {
  }
}

bool writable(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path probe = dir / L".write-test";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

fs::path local_appdata() {
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r / L"ACFA Port";
}

// The port's sources: the launcher sits in their folder (a release) or in a build folder below.
fs::path find_sources() {
  wchar_t buf[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  fs::path dir = fs::path(buf).parent_path();
  for (int i = 0; i < 5 && !dir.empty(); ++i, dir = dir.parent_path()) {
    std::error_code ec;
    if (fs::exists(dir / "CMakeLists.txt", ec) && fs::exists(dir / "data" / "toolchain.txt", ec) && fs::exists(dir / "src" / "runtime", ec))
      return dir;
    if (dir == dir.parent_path()) break;
  }
  return {};
}

// ---- settings (the shared file in the install folder) ----
fs::path settings_path() { return acfa::settings::file_in(g_cfg.data); }

struct ModeItem {
  int w, h, hz;
};
std::vector<ModeItem> g_res_items;  // the resolution combo's entries for the current mode

void monitor_size(int* w, int* h, int* hz) {
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
  *w = int(dm.dmPelsWidth);
  *h = int(dm.dmPelsHeight);
  *hz = int(dm.dmDisplayFrequency);
}

void fill_resolutions(const acfa::settings::Display& d) {
  HWND cb = item(kResolution);
  SendMessageW(cb, CB_RESETCONTENT, 0, 0);
  g_res_items.clear();
  int mw, mh, mhz;
  monitor_size(&mw, &mh, &mhz);
  int sel = 0;
  auto add = [&](const std::wstring& text, ModeItem m) {
    SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(text.c_str()));
    g_res_items.push_back(m);
  };
  if (d.mode == acfa::settings::Mode::Windowed) {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    for (const auto& s : acfa::settings::window_sizes()) {
      if ((s.w > wa.right - wa.left || s.h > wa.bottom - wa.top) && !(s.w == d.width && s.h == d.height)) continue;
      if (s.w == d.width && s.h == d.height) sel = int(g_res_items.size());
      add(std::to_wstring(s.w) + L" x " + std::to_wstring(s.h), {s.w, s.h, 0});
    }
    if (std::none_of(g_res_items.begin(), g_res_items.end(), [&](const ModeItem& m) { return m.w == d.width && m.h == d.height; })) {
      sel = int(g_res_items.size());
      add(std::to_wstring(d.width) + L" x " + std::to_wstring(d.height), {d.width, d.height, 0});
    }
    EnableWindow(cb, TRUE);
  } else if (d.mode == acfa::settings::Mode::Fullscreen) {
    add(L"Desktop (" + std::to_wstring(mw) + L" x " + std::to_wstring(mh) + L", " + std::to_wstring(mhz) + L" Hz)", {0, 0, 0});
    std::set<std::tuple<int, int, int>> seen;
    std::vector<ModeItem> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i)
      if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 640 && seen.insert({int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)}).second)
        modes.push_back({int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)});
    std::sort(modes.begin(), modes.end(), [](const ModeItem& a, const ModeItem& b) {
      return a.w != b.w ? a.w > b.w : a.h != b.h ? a.h > b.h : a.hz > b.hz;
    });
    for (const ModeItem& m : modes) {
      if (m.w == d.fs_width && m.h == d.fs_height && (m.hz == d.fs_hz || !d.fs_hz)) sel = int(g_res_items.size());
      add(std::to_wstring(m.w) + L" x " + std::to_wstring(m.h) + L", " + std::to_wstring(m.hz) + L" Hz", m);
    }
    EnableWindow(cb, TRUE);
  } else {
    add(std::to_wstring(mw) + L" x " + std::to_wstring(mh), {mw, mh, 0});
    EnableWindow(cb, FALSE);
  }
  SendMessageW(cb, CB_SETCURSEL, WPARAM(sel), 0);
}

void load_settings_controls() {
  if (g_cfg.data.empty()) return;
  acfa::settings::Display d;
  try {
    d = acfa::settings::read_display(acfa::settings::load_or_create(settings_path(), nullptr));
  } catch (const std::exception&) {
  }
  HWND fps = item(kFps);
  SendMessageW(fps, CB_RESETCONTENT, 0, 0);
  int sel = -1;
  const auto& choices = acfa::settings::fps_choices();
  for (size_t i = 0; i < choices.size(); ++i) {
    std::wstring t = widen(choices[i].label) + (choices[i].validated ? L"" : L" (untested)");
    SendMessageW(fps, CB_ADDSTRING, 0, LPARAM(t.c_str()));
    if (std::abs(choices[i].fps - d.fps) < 0.01) sel = int(i);
  }
  if (sel < 0) {
    std::wstring t = widen(acfa::settings::fps_text(d.fps)) + L" (untested)";
    SendMessageW(fps, CB_ADDSTRING, 0, LPARAM(t.c_str()));
    sel = int(choices.size());
  }
  SendMessageW(fps, CB_SETCURSEL, WPARAM(sel), 0);
  HWND mode = item(kMode);
  SendMessageW(mode, CB_RESETCONTENT, 0, 0);
  for (const wchar_t* m : {L"Windowed", L"Fullscreen", L"Borderless fullscreen"}) SendMessageW(mode, CB_ADDSTRING, 0, LPARAM(m));
  SendMessageW(mode, CB_SETCURSEL, d.mode == acfa::settings::Mode::Fullscreen ? 1 : d.mode == acfa::settings::Mode::Borderless ? 2 : 0, 0);
  fill_resolutions(d);
}

void save_setting(int id) {
  if (g_cfg.data.empty()) return;
  try {
    acfa::Ini ini = acfa::settings::load_or_create(settings_path(), nullptr);
    acfa::settings::Display d = acfa::settings::read_display(ini);
    int sel = int(SendMessageW(item(id), CB_GETCURSEL, 0, 0));
    if (id == kFps) {
      const auto& choices = acfa::settings::fps_choices();
      if (sel >= 0 && sel < int(choices.size())) ini.set("display", "fps", acfa::settings::fps_text(choices[size_t(sel)].fps));
    } else if (id == kMode) {
      d.mode = sel == 1 ? acfa::settings::Mode::Fullscreen : sel == 2 ? acfa::settings::Mode::Borderless : acfa::settings::Mode::Windowed;
      ini.set("display", "mode", acfa::settings::mode_name(d.mode));
      fill_resolutions(d);
    } else if (id == kResolution && sel >= 0 && sel < int(g_res_items.size())) {
      const ModeItem& m = g_res_items[size_t(sel)];
      if (d.mode == acfa::settings::Mode::Windowed) {
        ini.set("display", "window", acfa::strprintf("%dx%d", m.w, m.h));
      } else if (d.mode == acfa::settings::Mode::Fullscreen) {
        d.fs_width = m.w;
        d.fs_height = m.h;
        d.fs_hz = m.hz;
        ini.set("display", "fullscreen_mode", acfa::settings::fullscreen_text(d));
      }
    }
    ini.save(settings_path());
  } catch (const std::exception& e) {
    MessageBoxW(g_wnd, widen(std::string("Settings not saved: ") + e.what()).c_str(), L"ARMORED CORE for Answer", MB_ICONWARNING);
  }
}

// ---- saves ----
fs::path saves_dir() { return g_cfg.data / "hdd0" / "home" / "00000001" / "savedata"; }
fs::path backups_dir() { return g_cfg.data / "backups"; }

std::wstring stamp_now() {
  std::time_t t = std::time(nullptr);
  wchar_t b[32];
  std::wcsftime(b, std::size(b), L"%Y%m%d-%H%M%S", std::localtime(&t));
  return b;
}

// Copies the game's save folders (BLUS30187...) from one savedata folder to another.
size_t copy_saves(const fs::path& from, const fs::path& to) {
  size_t n = 0;
  std::error_code ec;
  fs::create_directories(to, ec);
  for (const auto& e : fs::directory_iterator(from, ec)) {
    if (!e.is_directory() || e.path().filename().wstring().rfind(L"BLUS30187", 0) != 0) continue;
    fs::copy(e.path(), to / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec) ++n;
  }
  return n;
}

// ---- checks of the chosen files (background: hashing takes a moment) ----
void start_checks() {
  int gen = ++g_check_gen;
  SetupConfig c = g_cfg;
  std::thread([c, gen] {
    Status s[4] = {check_disc(c.disc), check_update(c.update), check_pup(c.pup), check_keys(c.keys)};
    {
      std::lock_guard lk(g_check_mu);
      if (gen != g_check_gen) return;
      for (int i = 0; i < 4; ++i) g_checks[i] = s[i];
    }
    PostMessageW(g_wnd, WM_CHECKED, WPARAM(gen), 0);
  }).detach();
}

// The install folder must not lie inside a folder holding the game files (nothing is written there).
bool inside(const fs::path& child, const fs::path& parent) {
  if (child.empty() || parent.empty()) return false;
  std::error_code ec;
  std::wstring c = fs::weakly_canonical(fs::absolute(child, ec), ec).wstring(), p = fs::weakly_canonical(fs::absolute(parent, ec), ec).wstring();
  for (auto* s : {&c, &p}) {
    for (auto& ch : *s) ch = wchar_t(towlower(ch));
    while (s->size() > 3 && (s->back() == L'\\' || s->back() == L'/')) s->pop_back();
  }
  return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && (c[p.size()] == L'\\' || c[p.size()] == L'/'));
}
bool data_ok() {
  if (g_cfg.data.empty()) return false;
  for (const fs::path& in : {g_cfg.disc, g_cfg.update.parent_path(), g_cfg.pup.parent_path(), g_cfg.keys.parent_path()})
    if (inside(g_cfg.data, in)) return false;
  return true;
}

bool inputs_ok() {
  std::lock_guard lk(g_check_mu);
  return g_checks[0].verdict == Verdict::Ok && g_checks[1].verdict != Verdict::Bad && g_checks[2].verdict == Verdict::Ok &&
         g_checks[3].verdict == Verdict::Ok;
}

void update_space() {
  if (g_cfg.data.empty()) {
    set_text(kDataState, L"Choose where the port keeps its files.");
    return;
  }
  if (!data_ok()) {
    set_text(kDataState, L"Choose a folder outside the game files' folders.");
    return;
  }
  fs::path p = g_cfg.data;
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  ULARGE_INTEGER avail{};
  GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr);
  unsigned long long need = Setup::space_needed(g_cfg);
  set_text(kDataState, widen(acfa::strprintf("Needs %.1f GB more; %.0f GB free.", double(need) / (1ull << 30), double(avail.QuadPart) / (1ull << 30))));
}

void update_buttons() {
  bool busy = g_setup.running(), playing = g_game != nullptr;
  g_ready = !g_cfg.data.empty() && Setup::ready(g_cfg);
  EnableWindow(item(kPrepare), !busy && !playing && inputs_ok() && data_ok());
  EnableWindow(item(kCancel), busy);
  EnableWindow(item(kPlay), !busy && !playing && g_ready && inputs_ok());
  for (int id : {kDiscBrowse, kUpdateBrowse, kPupBrowse, kKeysBrowse, kDataBrowse, kDiscEdit, kUpdateEdit, kPupEdit, kKeysEdit, kDataEdit})
    EnableWindow(item(id), !busy && !playing);
  for (int id : {kSavesBackup, kSavesRestore, kFps, kMode}) EnableWindow(item(id), !playing && !g_cfg.data.empty());
  set_text(kPrepare, g_ready ? L"Rebuild" : L"Prepare and build");
  set_text(kPlayState, playing ? L"Playing." : busy ? L"" : g_ready ? L"Ready." : L"Prepare and build first.");
}

void refresh_paths_from_ui() {
  g_cfg.disc = get_text(kDiscEdit);
  g_cfg.update = get_text(kUpdateEdit);
  g_cfg.pup = get_text(kPupEdit);
  g_cfg.keys = get_text(kKeysEdit);
  g_cfg.data = get_text(kDataEdit);
  save_paths();
  update_space();
  set_text(kSavesPath, g_cfg.data.empty() ? L"" : saves_dir().wstring());
  start_checks();
  update_buttons();
}

// ---- file and folder pickers ----
fs::path pick(bool folder, const wchar_t* title, const fs::path& start, const wchar_t* filter_name, const wchar_t* filter) {
  IFileOpenDialog* dlg = nullptr;
  fs::path result;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  dlg->SetTitle(title);
  if (!folder && filter) {
    COMDLG_FILTERSPEC spec[2] = {{filter_name, filter}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
  }
  std::error_code ec;
  fs::path dir = start;
  while (!dir.empty() && !fs::is_directory(dir, ec)) dir = dir.parent_path();
  IShellItem* si = nullptr;
  if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
    dlg->SetFolder(si);
    si->Release();
  }
  if (SUCCEEDED(dlg->Show(g_wnd))) {
    IShellItem* r = nullptr;
    if (SUCCEEDED(dlg->GetResult(&r))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) result = p;
      CoTaskMemFree(p);
      r->Release();
    }
  }
  dlg->Release();
  return result;
}

// ---- the game ----
void play() {
  fs::path exe = Setup::game_exe(g_cfg);
  std::wstring cmd = quote_arg(exe.wstring()) + L" --data " + quote_arg(g_cfg.data.wstring()) + L" --disc " + quote_arg(g_cfg.disc.wstring()) +
                     L" --keep-runs 10";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_cfg.data.c_str(), &si, &pi)) {
    MessageBoxW(g_wnd, L"The game could not be started.", L"ARMORED CORE for Answer", MB_ICONERROR);
    return;
  }
  CloseHandle(pi.hThread);
  g_game = pi.hProcess;
  update_buttons();
  ShowWindow(g_wnd, SW_MINIMIZE);
  std::thread([h = pi.hProcess] {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    PostMessageW(g_wnd, WM_GAME_EXIT, WPARAM(code), 0);
  }).detach();
}

fs::path newest_run_log() {
  fs::path best;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(g_cfg.data / "runs", ec))
    if (e.is_directory() && (best.empty() || e.path().filename() > best.filename())) best = e.path();
  return best.empty() ? best : best / "acfa.log";
}

// ---- layout (96-DPI units) ----
struct Place {
  int id;
  int x, y, w, h;
};
const Place kPlaces[] = {
    {kDiscEdit, 132, 30, 384, 24},   {kDiscBrowse, 522, 29, 82, 26},   {kDiscState, 132, 56, 472, 18},
    {kUpdateEdit, 132, 80, 384, 24}, {kUpdateBrowse, 522, 79, 82, 26}, {kUpdateState, 132, 106, 472, 18},
    {kPupEdit, 132, 130, 384, 24},   {kPupBrowse, 522, 129, 82, 26},   {kPupState, 132, 156, 472, 18},
    {kKeysEdit, 132, 180, 384, 24},  {kKeysBrowse, 522, 179, 82, 26},  {kKeysState, 132, 206, 472, 18},
    {kDataEdit, 24, 266, 492, 24},   {kDataBrowse, 522, 265, 82, 26},  {kDataState, 24, 293, 580, 18},
    {kPrepare, 24, 344, 160, 30},    {kCancel, 192, 344, 90, 30},      {kOpenLog, 290, 344, 90, 30},
    {kProgress, 24, 384, 580, 16},   {kStatus, 24, 404, 580, 18},      {kLog, 24, 426, 580, 176},
    {kFps, 748, 30, 190, 300},       {kMode, 748, 64, 190, 300},       {kResolution, 748, 98, 190, 300},
    {kSavesPath, 636, 166, 304, 34}, {kSavesOpen, 636, 204, 96, 28},   {kSavesBackup, 740, 204, 96, 28},
    {kSavesRestore, 844, 204, 96, 28}, {kPlayState, 624, 496, 324, 20}, {kPlay, 624, 520, 324, 82},
};
struct Label {
  int x, y, w, h;
  const wchar_t* text;
  bool group;
};
const Label kLabels[] = {
    {12, 6, 604, 232, L"Game files", true},    {24, 33, 104, 20, L"Disc folder", false},     {24, 83, 104, 20, L"Update 01.35", false},
    {24, 133, 104, 20, L"System software", false}, {24, 183, 104, 20, L"Key file", false},   {12, 244, 604, 76, L"Install folder", true},
    {12, 324, 604, 286, L"Setup", true},          {624, 6, 324, 128, L"Settings", true},      {636, 33, 108, 20, L"Frame rate", false},
    {636, 67, 108, 20, L"Display mode", false},   {636, 101, 108, 20, L"Resolution", false}, {624, 142, 324, 100, L"Saves", true},
};
constexpr int kClientW = 960, kClientH = 616;

void layout() {
  for (const Place& p : kPlaces) SetWindowPos(item(p.id), nullptr, S(p.x), S(p.y), S(p.w), S(p.h), SWP_NOZORDER | SWP_NOACTIVATE);
  for (size_t i = 0; i < std::size(kLabels); ++i) {
    const Label& l = kLabels[i];
    SetWindowPos(item(kLabel + int(i)), nullptr, S(l.x), S(l.y), S(l.w), S(l.h), SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void make_fonts() {
  if (g_font) DeleteObject(g_font);
  if (g_big) DeleteObject(g_big);
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof ncm;
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
  g_font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW big = ncm.lfMessageFont;
  big.lfHeight = -MulDiv(16, int(g_dpi), 72);
  big.lfWeight = FW_SEMIBOLD;
  g_big = CreateFontIndirectW(&big);
  EnumChildWindows(g_wnd, [](HWND h, LPARAM) -> BOOL {
    SendMessageW(h, WM_SETFONT, WPARAM(GetDlgCtrlID(h) == kPlay ? g_big : g_font), TRUE);
    return TRUE;
  }, 0);
}

void create_controls() {
  auto make = [](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, HMENU(INT_PTR(id)), g_inst, nullptr);
  };
  for (size_t i = 0; i < std::size(kLabels); ++i)
    make(kLabels[i].group ? L"BUTTON" : L"STATIC", kLabels[i].text, kLabels[i].group ? BS_GROUPBOX : SS_LEFT, kLabel + int(i));
  for (int i = 0; i < 4; ++i) {
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kDiscEdit + i, WS_EX_CLIENTEDGE);
    make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, kDiscBrowse + i);
    make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kDiscState + i);
  }
  make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kDataEdit, WS_EX_CLIENTEDGE);
  make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, kDataBrowse);
  make(L"STATIC", L"", SS_LEFT, kDataState);
  make(L"BUTTON", L"Prepare and build", WS_TABSTOP | BS_PUSHBUTTON, kPrepare);
  make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, kCancel);
  make(L"BUTTON", L"Open log", WS_TABSTOP | BS_PUSHBUTTON, kOpenLog);
  make(PROGRESS_CLASSW, L"", PBS_SMOOTH, kProgress);
  SendMessageW(item(kProgress), PBM_SETRANGE32, 0, 1000);
  make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kStatus);
  make(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, kLog, WS_EX_CLIENTEDGE);
  for (int id : {kFps, kMode, kResolution}) make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
  make(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, kSavesPath);
  make(L"BUTTON", L"Open", WS_TABSTOP | BS_PUSHBUTTON, kSavesOpen);
  make(L"BUTTON", L"Back up", WS_TABSTOP | BS_PUSHBUTTON, kSavesBackup);
  make(L"BUTTON", L"Restore...", WS_TABSTOP | BS_PUSHBUTTON, kSavesRestore);
  make(L"STATIC", L"", SS_CENTER, kPlayState);
  make(L"BUTTON", L"PLAY", WS_TABSTOP | BS_DEFPUSHBUTTON, kPlay);
}

void add_log_line(const std::string& s) {
  HWND lb = item(kLog);
  SendMessageW(lb, LB_ADDSTRING, 0, LPARAM(widen(s).c_str()));
  LRESULT n = SendMessageW(lb, LB_GETCOUNT, 0, 0);
  while (n > 500) n = SendMessageW(lb, LB_DELETESTRING, 0, 0);
  SendMessageW(lb, LB_SETTOPINDEX, WPARAM(std::max<LRESULT>(0, n - 1)), 0);
}

void on_setup_update() {
  SetupView v = g_setup.view();
  for (const auto& l : v.lines) add_log_line(l);
  SendMessageW(item(kProgress), PBM_SETPOS, WPARAM(int(v.fraction * 1000)), 0);
  std::string status = v.status;
  size_t done = 0;
  for (const auto& s : v.steps) done += s.state == StepState::Done || s.state == StepState::Skipped;
  if (v.running) status = acfa::strprintf("Step %zu of %zu: ", std::min(done + 1, v.steps.size()), v.steps.size()) + status;
  if (v.finished && !v.ok) status = v.cancelled ? "Cancelled. Prepare and build continues where it stopped." : v.error;
  if (v.finished && v.ok) status = "Ready to play.";
  set_text(kStatus, widen(status));
  if (v.finished) {
    update_space();
    update_buttons();
    if (v.ok) FlashWindow(g_wnd, TRUE);
  }
}

Verdict g_verdicts[4];
COLORREF verdict_color(Verdict v) {
  return v == Verdict::Ok ? RGB(0, 120, 40) : v == Verdict::Bad ? RGB(190, 30, 30) : GetSysColor(COLOR_GRAYTEXT);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (code == EN_KILLFOCUS && ((id >= kDiscEdit && id <= kKeysEdit) || id == kDataEdit)) {
        refresh_paths_from_ui();
        if (id == kDataEdit) load_settings_controls();
        return 0;
      }
      if (code == CBN_SELCHANGE && (id == kFps || id == kMode || id == kResolution)) {
        save_setting(id);
        return 0;
      }
      if (code != BN_CLICKED) break;
      switch (id) {
        case kDiscBrowse: {
          fs::path p = pick(true, L"Disc folder of ARMORED CORE for Answer (contains PS3_GAME)", g_cfg.disc, nullptr, nullptr);
          if (p.empty()) break;
          set_text(kDiscEdit, p.wstring());
          if (get_text(kPupEdit).empty()) set_text(kPupEdit, find_pup(p).wstring());
          if (get_text(kUpdateEdit).empty()) set_text(kUpdateEdit, find_update(p).wstring());
          refresh_paths_from_ui();
          break;
        }
        case kUpdateBrowse: {
          fs::path p = pick(false, L"Update package 01.35 (UP0001-BLUS30187_00-...-A0135-...pkg)", g_cfg.update.empty() ? g_cfg.disc : g_cfg.update,
                            L"PS3 package", L"*.pkg");
          if (!p.empty()) set_text(kUpdateEdit, p.wstring()), refresh_paths_from_ui();
          break;
        }
        case kPupBrowse: {
          fs::path p = pick(false, L"PS3 system software 2.41 (PS3UPDAT.PUP)", g_cfg.pup.empty() ? g_cfg.disc : g_cfg.pup, L"PS3 system software",
                            L"*.PUP");
          if (!p.empty()) set_text(kPupEdit, p.wstring()), refresh_paths_from_ui();
          break;
        }
        case kKeysBrowse: {
          fs::path p = pick(false, L"Key file", g_cfg.keys, L"Key file", L"*.txt");
          if (!p.empty()) set_text(kKeysEdit, p.wstring()), refresh_paths_from_ui();
          break;
        }
        case kDataBrowse: {
          fs::path p = pick(true, L"Install folder (tools, prepared files, build, saves)", g_cfg.data, nullptr, nullptr);
          if (p.empty()) break;
          if (!writable(p)) {
            MessageBoxW(g_wnd, L"This folder cannot be written. Choose another one.", L"ARMORED CORE for Answer", MB_ICONWARNING);
            break;
          }
          set_text(kDataEdit, p.wstring());
          refresh_paths_from_ui();
          load_settings_controls();
          break;
        }
        case kPrepare:
          refresh_paths_from_ui();
          SendMessageW(item(kLog), LB_RESETCONTENT, 0, 0);
          g_setup.start(g_cfg, [] { PostMessageW(g_wnd, WM_SETUP, 0, 0); });
          update_buttons();
          break;
        case kCancel:
          g_setup.cancel();
          break;
        case kOpenLog: {
          fs::path log = g_cfg.data / "launcher.log";
          if (fs::exists(log)) ShellExecuteW(g_wnd, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case kSavesOpen: {
          std::error_code ec;
          fs::create_directories(saves_dir(), ec);
          ShellExecuteW(g_wnd, L"explore", saves_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case kSavesBackup: {
          fs::path to = backups_dir() / (L"saves-" + stamp_now());
          size_t n = copy_saves(saves_dir(), to);
          std::wstring msg = n ? L"Saves backed up to\n" + to.wstring() : L"There are no saves yet.";
          MessageBoxW(g_wnd, msg.c_str(), L"ARMORED CORE for Answer", MB_ICONINFORMATION);
          break;
        }
        case kSavesRestore: {
          fs::path from = pick(true, L"Backup to restore (a saves-... folder)", backups_dir(), nullptr, nullptr);
          if (from.empty()) break;
          std::error_code ec;
          bool any = false;
          for (const auto& e : fs::directory_iterator(from, ec))
            any |= e.is_directory() && e.path().filename().wstring().rfind(L"BLUS30187", 0) == 0;
          if (!any) {
            MessageBoxW(g_wnd, L"No saves of this game in that folder.", L"ARMORED CORE for Answer", MB_ICONWARNING);
            break;
          }
          if (MessageBoxW(g_wnd, L"Replace the current saves with this backup?\nThe current saves are backed up first.", L"ARMORED CORE for Answer",
                          MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
          copy_saves(saves_dir(), backups_dir() / (L"saves-before-restore-" + stamp_now()));
          for (const auto& e : fs::directory_iterator(saves_dir(), ec))
            if (e.is_directory() && e.path().filename().wstring().rfind(L"BLUS30187", 0) == 0) fs::remove_all(e.path(), ec);
          size_t n = copy_saves(from, saves_dir());
          MessageBoxW(g_wnd, (std::to_wstring(n) + L" save(s) restored.").c_str(), L"ARMORED CORE for Answer", MB_ICONINFORMATION);
          break;
        }
        case kPlay:
          if (IsWindowEnabled(item(kPlay))) play();
          break;
      }
      return 0;
    }
    case WM_SETUP:
      on_setup_update();
      return 0;
    case WM_CHECKED: {
      if (int(wp) != g_check_gen) return 0;
      Status checks[4];
      {
        std::lock_guard lk(g_check_mu);
        std::copy(std::begin(g_checks), std::end(g_checks), checks);
      }
      for (int i = 0; i < 4; ++i) {
        g_verdicts[i] = checks[i].verdict;
        set_text(kDiscState + i, widen(checks[i].text));
        InvalidateRect(item(kDiscState + i), nullptr, TRUE);
      }
      update_buttons();
      return 0;
    }
    case WM_GAME_EXIT: {
      if (g_game) CloseHandle(g_game);
      g_game = nullptr;
      ShowWindow(g_wnd, SW_RESTORE);
      load_settings_controls();  // PC SETTINGS may have changed them
      update_buttons();
      if (wp != 0) {
        fs::path log = newest_run_log();
        set_text(kStatus, L"The game stopped with an error. Log: " + log.wstring());
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      int id = GetDlgCtrlID(HWND(lp));
      HDC dc = HDC(wp);
      SetBkMode(dc, TRANSPARENT);
      if (id >= kDiscState && id <= kKeysState) SetTextColor(dc, verdict_color(g_verdicts[id - kDiscState]));
      return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DPICHANGED: {
      g_dpi = HIWORD(wp);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      make_fonts();
      layout();
      return 0;
    }
    case WM_CLOSE:
      if (g_setup.running() &&
          MessageBoxW(h, L"Setup is running. Stop it and close?", L"ARMORED CORE for Answer", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
      g_setup.cancel();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  g_inst = inst;
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ACFA-Port-Launcher");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"AcfaLauncher", nullptr)) SetForegroundWindow(other);
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  g_src = find_sources();
  if (g_src.empty()) {
    MessageBoxW(nullptr, L"Put ACFA-Launcher.exe in the port's folder (the one with CMakeLists.txt, data and src).", L"ARMORED CORE for Answer",
                MB_ICONERROR);
    return 1;
  }
  // The launcher's own record of the chosen locations: beside it when that folder is writable.
  wchar_t self[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  fs::path exe_dir = fs::path(self).parent_path();
  g_ini_path = (writable(exe_dir) ? exe_dir : local_appdata()) / "launcher.ini";
  load_paths();
  g_cfg.src = g_src;
  if (g_cfg.data.empty()) g_cfg.data = writable(g_src / "local") ? g_src / "local" : local_appdata();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"AcfaLauncher";
  RegisterClassExW(&wc);
  g_dpi = GetDpiForSystem();
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, S(kClientW), S(kClientH)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
  g_wnd = CreateWindowExW(0, wc.lpszClassName, L"ARMORED CORE for Answer - PC port", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  UINT dpi = GetDpiForWindow(g_wnd);
  if (dpi != g_dpi) {  // the window opened on a monitor with another scale
    g_dpi = dpi;
    r = RECT{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
  }
  create_controls();
  make_fonts();
  layout();
  set_text(kDiscEdit, g_cfg.disc.wstring());
  set_text(kUpdateEdit, g_cfg.update.wstring());
  set_text(kPupEdit, g_cfg.pup.wstring());
  set_text(kKeysEdit, g_cfg.keys.wstring());
  set_text(kDataEdit, g_cfg.data.wstring());
  refresh_paths_from_ui();
  load_settings_controls();
  ShowWindow(g_wnd, show);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(g_wnd, &m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  CoUninitialize();
  CloseHandle(single);
  return 0;
}
