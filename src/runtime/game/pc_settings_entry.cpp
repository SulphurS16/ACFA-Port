#include "runtime/game/pc_settings_entry.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ui/overlay.h"

namespace rt::game {
namespace {

// The game's UI text table holds "... IMPORT CONVERT DATA", "HDD INSTALL", "IMPORT DATA" in a
// row (the SYSTEM menu's entries); the help texts are a separate table.
const char* const kLabelBefore = "IMPORT CONVERT DATA";
const char* const kLabelOld = "HDD INSTALL";
const char* const kLabelNew = "PC SETTINGS";  // same length
const char* const kHelpOld = "Install ARMORED CORE for Answer to the HDD.";
const char* const kHelpNew = "Configure PC settings.";  // shorter: the rest is cleared

std::mutex g_mu;
u32 g_label_ea = 0, g_help_ea = 0;

std::string utf16be(const char* s, bool terminated) {
  std::string out;
  for (; *s; ++s) {
    out += '\0';
    out += *s;
  }
  if (terminated) out.append(2, '\0');
  return out;
}

bool holds(u32 ea, const std::string& bytes) {
  return ea && vm::committed(ea, u32(bytes.size())) && std::memcmp(::ppu::mem() + ea, bytes.data(), bytes.size()) == 0;
}

// The first place in the game's heaps holding `pat` (2-byte aligned), or 0.
u32 find(const std::string& pat) {
  u32 found = 0;
  for (vm::Area a : {vm::Area::Main, vm::Area::User64k, vm::Area::User1m}) {
    vm::for_each_block(a, [&](u32 begin, u32 size) {
      if (found) return;
      const char* base = reinterpret_cast<const char*>(::ppu::mem() + begin);
      for (const char* p = base; (p = std::search(p, base + size, pat.begin(), pat.end())) != base + size; ++p)
        if (((begin + u32(p - base)) & 1) == 0) {
          found = begin + u32(p - base);
          return;
        }
    });
    if (found) break;
  }
  return found;
}

void write(u32 ea, const std::string& bytes) { std::memcpy(::ppu::mem() + ea, bytes.data(), bytes.size()); }

// Replaces the SYSTEM menu's HDD INSTALL label and help text (once found, only checked again).
void patch_text() {
  std::lock_guard lk(g_mu);
  std::string label_new = utf16be(kLabelNew, true), help_new = utf16be(kHelpNew, true);
  if (!holds(g_label_ea, label_new)) {
    std::string ctx = utf16be(kLabelBefore, true);
    if (u32 at = find(ctx + utf16be(kLabelOld, true))) {
      g_label_ea = at + u32(ctx.size());
      write(g_label_ea, label_new);
      LOG_I("game", "SYSTEM menu: HDD INSTALL is PC SETTINGS (UI text at 0x%08x)", g_label_ea);
    }
  }
  if (!holds(g_help_ea, help_new)) {
    std::string old = utf16be(kHelpOld, true);
    if (u32 at = find(old)) {
      g_help_ea = at;
      std::string text = help_new;
      text.resize(old.size(), '\0');
      write(at, text);
    }
  }
}

}  // namespace

void pc_settings_thread_started(const std::string& name) {
  if (name == "HDDInstallTask") {  // the SYSTEM menu's entry was chosen
    LOG_I("game", "SYSTEM menu: PC SETTINGS chosen");
    ui::open_from_menu();
  } else if (name == "TitleAfterInitScene" || name == "MenuTop/MenuTopSelectScene" || name == "SystemSetting/MenuSysSelectScene") {
    patch_text();  // the UI text is loaded at boot; checked again before the menus that show it
  }
}

}  // namespace rt::game
