// The ORDER MATCH AI fixes (script_fixes.h). Every script goes through Lua 5.0.2's lua_load
// (0xea7fa8: L, reader, data, chunk name) with its whole source in a {text, size} block at `data`
// (lua_dobuffer 0xeab4f8, lua_dostring 0xeab588 and the game's own loader 0xc9cdc8). With the fixes
// on (the default), a script of an order match opponent that holds one of the slips is handed to
// the parser corrected, from a copy in guest memory freed when lua_load returns; the game's buffer
// stays as it was. Off, scripts load untouched.
#include "runtime/game/script_fixes.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <regex>

#include "common/bytes.h"
#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/settings.h"

namespace rt::game {
namespace {

// Replaces every `from` with `to`; returns how many.
int replace_all(std::string& s, const std::string& from, const std::string& to) {
  int n = 0;
  for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
    s.replace(p, from.size(), to);
    ++n;
  }
  return n;
}

}  // namespace

std::string fix_arena_script(const std::string& text, std::vector<std::string>* applied) {
  // Only the order match opponents' scripts ("@brief <arena> - COLLARED_11 - <name>", "- ORCA_02 -").
  std::string head = text.substr(0, 512);
  size_t id = head.find(" - COLLARED_");
  if (id == std::string::npos) id = head.find(" - ORCA_");
  if (id == std::string::npos) return text;
  size_t id_end = head.find(" - ", id + 3);
  const std::string who = head.substr(id + 3, id_end == std::string::npos ? 11 : id_end - id - 3);
  std::string s = text;
  auto note = [&](int n, const char* what) {
    if (n > 0 && applied) applied->push_back(acfa::strprintf("%s: %s (%d)", who.c_str(), what, n));
  };
  // Misspelled functions: the call fails with a script error, which ends that AI layer for the
  // frame. Collared 11 Dario Empio (once his Primal Armor is below 40 %: no assault cannon), Collared
  // 2 Lilium Wolcott (when her energy is below a quarter).
  note(replace_all(s, "GetEntityAPStability(", "GetEntityPAStability("), "GetEntityAPStability -> GetEntityPAStability");
  note(replace_all(s, "SetTimerLengthJump(", "SetTimeLengthJump("), "SetTimerLengthJump -> SetTimeLengthJump");
  // Misspelled weapon ids (nil, not a weapon): Collared 6's arm weapons, ORCA 02's shoulder unit.
  note(replace_all(s, "WEAPON_ID__WEPARM_", "WEAPON_ID__ARMWEP_"), "WEAPON_ID__WEPARM_ -> WEAPON_ID__ARMWEP_");
  note(replace_all(s, "WEAPON_ID__SHOULDAR", "WEAPON_ID__SHOULDER"), "WEAPON_ID__SHOULDAR -> WEAPON_ID__SHOULDER");
  // Collared 17 Fragile: the back weapons' fire operation (operation 2) takes the arm weapon's
  // weight shoot_w_1 instead of its own shoot_w_2, so the back weapon fires when the arm weapon
  // was chosen and hardly ever when it was.
  if (s.find("local shoot_w_2") != std::string::npos) {
    int n = 0;
    const std::string op = "OperationIdentifier( 2 )";
    for (size_t p = s.find(op); p != std::string::npos; p = s.find(op, p + op.size())) {
      size_t end = s.find(';', p), w = s.find("shoot_w_1", p), back = s.find("WEAPON_ID__BACK_", p);
      if (end == std::string::npos || w > end || back > end) continue;
      s.replace(w, 9, "shoot_w_2");
      ++n;
    }
    note(n, "back weapon operation weighted with shoot_w_2");
  }
  // Collared 21 Kamisori Johnny: the shoulder attack compares `dist`, which only the other attack
  // functions define (nil < 150 is a script error: the attack never runs).
  if (size_t f = s.find("function S_Attack( context )"); f != std::string::npos) {
    size_t body = s.find('\n', f), end = body == std::string::npos ? body : s.find("\nend", body);
    if (end != std::string::npos) {
      std::string b = s.substr(body, end - body);
      if (std::regex_search(b, std::regex("\\bdist\\b")) && b.find("local dist") == std::string::npos) {
        s.insert(body + 1, "\tlocal dist = GetToTargetDistance( context );\r\n");
        note(1, "S_Attack: local dist = GetToTargetDistance( context )");
      }
    }
  }
  return s;
}

}  // namespace rt::game

namespace ppu {

void hook_lua_load(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 data = u32(ctx.r[5]);
  if (!rt::settings::game().arena_ai_fixes || !data || !rt::vm::committed(data, 8)) {
    orig(ctx);
    return;
  }
  u32 text = r32(data), size = r32(data + 4);
  if (!text || size < 64 || size > (4u << 20) || !rt::vm::committed(text, size)) {
    orig(ctx);
    return;
  }
  std::string src(size, '\0');
  for (u32 i = 0; i < size; ++i) src[i] = char(r8(text + i));
  std::vector<std::string> applied;
  std::string fixed = rt::game::fix_arena_script(src, &applied);
  if (applied.empty()) {
    orig(ctx);
    return;
  }
  u32 copy = rt::vm::alloc(rt::vm::Area::Main, u32(fixed.size()) + 1, 0x10);
  if (!copy) {
    LOG_W("script", "ORDER MATCH AI fixes: no guest memory for the corrected script; loaded as it is");
    orig(ctx);
    return;
  }
  for (size_t i = 0; i < fixed.size(); ++i) w8(copy + u32(i), u8(fixed[i]));
  w8(copy + u32(fixed.size()), 0);
  for (const std::string& a : applied) LOG_I("script", "ORDER MATCH AI fix: %s", a.c_str());
  // The reader hands the corrected copy to the parser; afterwards the block points at the game's
  // text again (a script error unwinding through here frees the copy as well).
  struct Restore {
    u32 data, text, copy;
    ~Restore() {
      w32(data, text);
      rt::vm::free(rt::vm::Area::Main, copy);
    }
  } restore{data, text, copy};
  w32(data, copy);
  w32(data + 4, u32(fixed.size()));
  orig(ctx);
}

}  // namespace ppu
