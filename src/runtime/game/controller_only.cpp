// No guest camera writes or keyboard/mouse injection in the controller-only build.
#include "runtime/game/menu_mouse.h"
namespace rt::game {
InputContext input_context() { return InputContext::Screen; }
void menu_mouse_moved() {}
void menu_mouse_button(int, bool) {}
void menu_mouse_wheel(int) {}
bool menu_cursor_visible() { return false; }
void note_pad_input() {}
void note_menu_key() {}
bool controller_used_last() { return true; }
void menu_pad_bits(u16*, u16*) {}
}
