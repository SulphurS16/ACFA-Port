// Controller-only host input. No raw mouse registration, key bindings or camera state.
#include <atomic>
#include "runtime/input.h"
#include "runtime/config.h"
#include "runtime/gpu/input_hooks.h"
namespace rt {
namespace { std::atomic<bool> focus{true}; }
namespace gpu {
void input_key(u32, bool) {}
void input_focus(bool value) { focus=value; }
void input_mouse_raw(u16,s32,s32,u16,u16) {}
void input_mouse_capture(bool) {}
}
bool input_isolated() { return cfg().isolate_input; }
void input_key_inject(u32,bool) {}
bool input_key_down(u32) { return false; }
bool input_has_focus() { return focus.load(); }
void input_mouse_take(s64* x,s64* y) { *x=0;*y=0; }
bool input_mouse_button(int) { return false; }
s32 input_mouse_wheel_take() { return 0; }
bool input_mouse_captured() { return false; }
void input_mouse_inject(s64,s64) {}
void input_mouse_button_inject(int,bool) {}
}
