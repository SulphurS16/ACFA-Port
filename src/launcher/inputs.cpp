#include "launcher/inputs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <vector>

#include "common/crypto.h"
#include "common/file.h"
#include "common/known_inputs.h"
#include "common/sfo.h"
#include "launcher/process.h"

namespace launcher {
namespace {

namespace ki = acfa::inputs;

bool is_file(const fs::path& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

std::string hash_of(const fs::path& p) {
  try {
    return acfa::crypto::sha256_file_hex(narrow(p.wstring()));
  } catch (const std::exception&) {
    return "";
  }
}

std::string join(const std::vector<std::string>& v, size_t max_shown) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < max_shown; ++i) s += (i ? ", " : "") + v[i];
  if (v.size() > max_shown) s += ", ...";
  return s;
}

// "release:04.8900:" -> "4.89"
std::string read_system_software_version(const fs::path& dev_flash) {
  std::ifstream f(dev_flash / "vsh" / "etc" / "version.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("release:", 0) != 0) continue;
    std::string v = line.substr(8, line.find(':', 8) - 8);  // 04.8900
    if (v.size() >= 5) {
      std::string major = v.substr(0, v.find('.'));
      while (major.size() > 1 && major[0] == '0') major.erase(0, 1);
      return major + "." + v.substr(v.find('.') + 1, 2);
    }
  }
  return "";
}

}  // namespace

Found find_files(const fs::path& disc, const fs::path& rpcs3) {
  Found f;
  for (const fs::path& p : {disc / "PS3_GAME" / "USRDIR" / "EBOOT.elf", rpcs3 / "EBOOT.elf"})
    if (!disc.empty() && is_file(p)) {
      f.eboot_elf = p;
      break;
    }
  if (f.eboot_elf.empty() && !rpcs3.empty() && is_file(rpcs3 / "EBOOT.elf")) f.eboot_elf = rpcs3 / "EBOOT.elf";
  if (!rpcs3.empty()) {
    // RPCS3's own folder, its dev_flash, or a folder holding the decrypted files.
    std::vector<fs::path> dirs = {rpcs3 / "dev_flash" / "sys" / "external", rpcs3 / "sys" / "external", rpcs3};
    for (const fs::path& d : dirs)
      if (is_file(d / "libsre.prx")) {
        f.modules_dir = d;
        break;
      }
    if (f.modules_dir.empty())
      for (const fs::path& d : dirs)
        if (is_file(d / "libsre.sprx")) {  // the firmware is there, not decrypted yet
          f.modules_dir = d;
          break;
        }
    for (const fs::path& g : {rpcs3 / "dev_hdd0" / "game" / ki::kTitleId, rpcs3 / "game" / ki::kTitleId, rpcs3 / ki::kTitleId})
      if (is_file(g / "USRDIR" / ki::kRegulation.name)) {
        f.game_data = g;
        break;
      }
    for (const fs::path& df : {rpcs3 / "dev_flash", rpcs3, f.modules_dir.parent_path().parent_path()})
      if (!df.empty() && is_file(df / "vsh" / "etc" / "version.txt")) {
        f.rpcs3_system_software = read_system_software_version(df);
        break;
      }
  }
  return f;
}

Status check_disc(const fs::path& disc, const Found& f) {
  if (disc.empty()) return {Verdict::Empty, "Choose the game's disc folder (it contains PS3_GAME).", ""};
  fs::path sfo = disc / "PS3_GAME" / "PARAM.SFO";
  if (!is_file(sfo)) return {Verdict::Bad, "Not a PS3 disc folder: PS3_GAME\\PARAM.SFO is missing.", ""};
  std::string id, version;
  try {
    acfa::sfo::Map m = acfa::sfo::parse(acfa::read_file(sfo));
    id = acfa::sfo::get_string(m, "TITLE_ID");
    version = acfa::sfo::get_string(m, "VERSION");
  } catch (const std::exception& e) {
    return {Verdict::Bad, std::string("PARAM.SFO unreadable: ") + e.what(), ""};
  }
  if (id != ki::kTitleId) return {Verdict::Bad, "This is " + id + ", not ARMORED CORE for Answer (USA, BLUS30187).", ""};
  if (version != ki::kDiscVersion) return {Verdict::Bad, "BLUS30187 disc " + version + ": the port supports disc 01.01.", ""};
  if (f.eboot_elf.empty())
    return {Verdict::Bad, "EBOOT.elf missing: decrypt PS3_GAME\\USRDIR\\EBOOT.BIN in RPCS3 (How to decrypt..., step 2).",
            "EBOOT.elf not found in " + narrow((disc / "PS3_GAME" / "USRDIR").wstring())};
  std::string h = hash_of(f.eboot_elf);
  if (h != ki::kEbootElf.sha256)
    return {Verdict::Bad, "EBOOT.elf is not the decrypted executable of disc 01.01 (decrypt EBOOT.BIN again).",
            narrow(f.eboot_elf.wstring()) + ": sha256 " + h + ", expected " + ki::kEbootElf.sha256};
  return {Verdict::Ok, "ARMORED CORE for Answer, BLUS30187, disc " + version + "; EBOOT.elf decrypted", ""};
}

Status check_modules(const fs::path& rpcs3, const Found& f) {
  if (rpcs3.empty()) return {Verdict::Empty, "Choose the RPCS3 folder (or a folder with the decrypted .prx files).", ""};
  if (f.modules_dir.empty())
    return {Verdict::Bad, "No system software found here: choose RPCS3's folder (it contains dev_flash).", ""};
  std::vector<std::string> missing, other, wrong;
  std::string other_version, details;
  for (int i = 0; i < ki::kModuleCount; ++i) {
    const ki::Known* k = ki::module(ki::kModules[i]);
    fs::path p = f.modules_dir / k->name;
    if (!is_file(p)) {
      missing.push_back(k->name);
      details += std::string(k->name) + ": not found in " + narrow(f.modules_dir.wstring()) + "\n";
      continue;
    }
    std::string h = hash_of(p);
    if (h == k->sha256) continue;
    std::string v = ki::module_other_version(ki::kModules[i], h);
    if (!v.empty()) {
      other.push_back(k->name);
      other_version = v;
      details += std::string(k->name) + ": from system software " + v + "\n";
    } else {
      wrong.push_back(k->name);
      details += std::string(k->name) + ": sha256 " + h + ", expected " + k->sha256 + "\n";
    }
  }
  fs::path pic = f.modules_dir / ki::kFlashAtrac.name;
  bool pic_ok = is_file(pic) && hash_of(pic) == ki::kFlashAtrac.sha256;
  if (!pic_ok) details += std::string(ki::kFlashAtrac.name) + (is_file(pic) ? ": differs from every known version\n" : ": not found\n");
  std::string sw = f.rpcs3_system_software;
  if (!other.empty() && sw == ki::kSystemSoftware)  // RPCS3 was updated since these were decrypted
    return {Verdict::Bad, "The decrypted modules are from system software " + other_version + ": decrypt them again (step 3).", details};
  if (!other.empty() || (!sw.empty() && sw != ki::kSystemSoftware && missing.size() == size_t(ki::kModuleCount)))
    return {Verdict::Bad,
            "RPCS3 has system software " + (other_version.empty() ? sw : other_version) + "; the port needs " + ki::kSystemSoftware +
                " (How to decrypt..., step 1).",
            details};
  if (missing.size() == size_t(ki::kModuleCount))
    return {Verdict::Bad, "The system modules are not decrypted yet (How to decrypt..., step 3).", details};
  if (!missing.empty())
    return {Verdict::Bad, "Not decrypted yet (step 3): " + join(missing, 3) + " (" + std::to_string(missing.size()) + " of 10).", details};
  if (!wrong.empty()) return {Verdict::Bad, "Not the expected decrypted modules: " + join(wrong, 3) + " (decrypt them again).", details};
  if (!pic_ok) return {Verdict::Bad, "flashATRAC.pic is missing or damaged in " + narrow(f.modules_dir.wstring()) + ".", details};
  return {Verdict::Ok, "10 system modules of system software " + std::string(ki::kSystemSoftware) + ", decrypted", ""};
}

Status check_update(const fs::path& rpcs3, const Found& f) {
  if (rpcs3.empty() || f.game_data.empty())
    return {Verdict::Optional, "Optional: install update 01.35 in RPCS3 for the final balance data (How to decrypt..., step 4).", ""};
  fs::path reg = f.game_data / "USRDIR" / ki::kRegulation.name;
  std::string h = hash_of(reg);
  if (h != ki::kRegulation.sha256)
    return {Verdict::Optional, "The installed update is not 01.35 (regulation.bin differs): the disc's balance data is used.",
            narrow(reg.wstring()) + ": sha256 " + h + ", expected " + ki::kRegulation.sha256};
  return {Verdict::Ok, "Update 01.35 (balance data)", ""};
}

std::wstring decrypt_help(const fs::path& disc, const fs::path& rpcs3, const Found& f) {
  std::wstring eboot = disc.empty() ? L"<disc folder>\\PS3_GAME\\USRDIR\\EBOOT.BIN" : (disc / "PS3_GAME" / "USRDIR" / "EBOOT.BIN").wstring();
  std::wstring ext = !f.modules_dir.empty() ? f.modules_dir.wstring()
                     : rpcs3.empty()        ? L"<RPCS3 folder>\\dev_flash\\sys\\external"
                                            : (rpcs3 / "dev_flash" / "sys" / "external").wstring();
  std::wstring sw = widen(ki::kSystemSoftware);
  std::wstring have = f.rpcs3_system_software.empty() ? L"" : L" (yours has " + widen(f.rpcs3_system_software) + L")";
  std::wstring mods;
  for (int i = 0; i < ki::kModuleCount; ++i) mods += (i ? (i % 5 ? L", " : L",\n      ") : L"") + widen(ki::kModules[i]) + L".sprx";
  return L"The port contains no keys and decrypts nothing: decrypt these files once with RPCS3.\n\n"
         L"1. RPCS3 needs PS3 system software " + sw + have +
         L": File > Install Firmware, with PS3UPDAT.PUP from the PS3 system software update page on playstation.com.\n\n"
         L"2. Utilities > Decrypt PS3 Binaries, and choose\n      " + eboot +
         L"\n   RPCS3 writes EBOOT.elf next to it.\n\n"
         L"3. Utilities > Decrypt PS3 Binaries again. In\n      " + ext +
         L"\n   select these 10 files together (Ctrl+click):\n      " + mods +
         L"\n   RPCS3 writes a .prx file next to each.\n\n"
         L"4. Optional, for the game's final balance data: File > Install Packages, with the update\n"
         L"   UP0001-BLUS30187_00-AC45000004141906-A0135-V0100-PE.pkg\n\n"
         L"5. Here: choose the disc folder and the RPCS3 folder. When their lines are green, Prepare and build.";
}

fs::path find_rpcs3() {
  std::vector<fs::path> roots;
  for (KNOWNFOLDERID id : {FOLDERID_Desktop, FOLDERID_Downloads, FOLDERID_Documents, FOLDERID_Profile, FOLDERID_ProgramFiles}) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) roots.push_back(p);
    CoTaskMemFree(p);
  }
  for (const wchar_t* d : {L"C:\\", L"D:\\", L"E:\\"}) roots.push_back(d);
  for (const fs::path& r : roots) {
    if (is_file(r / "RPCS3" / "rpcs3.exe")) return r / "RPCS3";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(r, fs::directory_options::skip_permission_denied, ec)) {
      std::wstring n = e.path().filename().wstring();
      for (auto& c : n) c = wchar_t(towlower(c));
      if (e.is_directory(ec) && n.find(L"rpcs3") != std::wstring::npos && is_file(e.path() / "rpcs3.exe")) return e.path();
    }
  }
  return {};
}

}  // namespace launcher
