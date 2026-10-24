// The ORDER MATCH AI fixes (settings [game] arena_ai_fixes, on by default; GAME_NOTES "Regulations,
// arena scripts and community patches"): slips in the order match opponents' AI scripts corrected
// when the game loads them, so those opponents do what their scripts were written to do.
#pragma once
#include <string>
#include <vector>

namespace rt::game {

// `text` (a script's Lua source) with the slips corrected; `applied` gets one line per correction
// made (empty, and the text unchanged, for a script without any).
std::string fix_arena_script(const std::string& text, std::vector<std::string>* applied);

}  // namespace rt::game
