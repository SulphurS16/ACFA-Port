#pragma once
#include <string_view>
#ifndef ACFA_CONTROLLER_ONLY
#define ACFA_CONTROLLER_ONLY 1
#endif
namespace acfa::input_policy {
inline constexpr bool keyboard_mouse = !ACFA_CONTROLLER_ONLY;
inline constexpr bool allow_hook(std::string_view n) {
  if (keyboard_mouse) return true;
  return n != "input_pitch_set" && n != "look_update" && n != "input_turn_set" &&
         n != "ac_turn_input" && n != "ac_turn_input_b" && n != "ac_turn_update" &&
         n != "camera_follow" && n != "menu_frame" && n != "menu_dispatch" && n != "menu_dispatch_sys";
}
}
