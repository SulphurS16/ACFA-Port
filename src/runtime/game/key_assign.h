// The game's live button assignment for the player (the operator's action table; GAME_NOTES
// "Input mechanics measured"): one input code per action, as the control type (SIMPLE / REGULAR-A /
// REGULAR-B, ACSIS > OPTION > CONTROL OPTIONS) and its KEY ASSIGN set it. Keyboard actions are
// translated through it in missions (bindings.cpp), so a key keeps its action under any control type.
#pragma once
#include "common/bytes.h"

namespace rt::game {

// Action slots of the table (operator +80).
enum KeySlot : u32 {
  kSlotMoveForward = 0, kSlotMoveBack, kSlotStrafeRight, kSlotStrafeLeft, kSlotRotateRight, kSlotRotateLeft, kSlotLookUp, kSlotLookDown,
  kSlotResetView, kSlotLock, kSlotBoost, kSlotOveredBoost, kSlotQuickBoost, kSlotRightWeapon, kSlotLeftWeapon, kSlotShoulder,
  kSlotSwitchRight, kSlotSwitchLeft, kSlotPause, kSlotCount
};
// Input codes in it: 00-03 left stick up / down / right / left, 04 / 05 right stick up / down,
// 06 / 07 right stick right / left, 08 Circle, 09 Triangle, 0a Square, 0b Cross, 0c L1, 0d L2
// (pressure), 0e R1, 0f R2 (pressure), 10 L3, 11 R3, 13 START; 0xfe: none.
constexpr u8 kCodeNone = 0xfe;

// Copies the latest assignment seen in a mission (false before the first one). Thread-safe.
bool key_assignment(u8 codes[kSlotCount]);

}  // namespace rt::game
