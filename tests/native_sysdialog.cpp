// Actual sysdialog model/controller source with explicit headless host seams.
// The non-null window token below is a test fixture, never a real window or NSApplication.
#include "runtime/sysdialog.h"
#include "runtime/gamepads.h"
#include "runtime/log.h"
#include "runtime/ui/overlay.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> window_live{false};
std::atomic<WORD> buttons[rt::pads::kSlots]{};
void require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
namespace dlg = rt::sysdlg;
using namespace std::chrono_literals;
void wait_visible() {
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!dlg::shown() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
  require(dlg::shown() && dlg::active(), "dialog publishes a live blocking model");
}
rt::ui::DialogView view() {
  rt::ui::DialogView out;
  require(dlg::view(&out), "renderer can snapshot actual dialog model");
  return out;
}
void release_buttons() {
  for (auto& b : buttons) b = 0;
  dlg::tick();
}
void tap(WORD button, int port = 0) {
  release_buttons();
  buttons[port] = button;
  dlg::tick();
}
template<class T> T answered(std::future<T>& result) {
  require(result.wait_for(2s) == std::future_status::ready, "controller response wakes waiting caller");
  const auto value = result.get();
  require(!dlg::active() && !dlg::shown(), "answered dialog releases controller and model");
  return value;
}
}  // namespace

namespace rt {
bool input_isolated() { return false; }
Level log_level() { return Level::Fatal; }
void log_write(Level, const char*, const std::string&) {}
namespace pads {
bool get(int slot, XINPUT_STATE* out) {
  if (slot < 0 || slot >= kSlots || !out) return false;
  *out = {};
  out->Gamepad.wButtons = buttons[slot].load();
  return true;
}
}
namespace gpu {
void* window_handle() { return window_live.load() ? reinterpret_cast<void*>(std::uintptr_t(1)) : nullptr; }
void window_refresh_cursor() {}
}
}  // namespace rt

int main() {
  alarm(10);  // Bounded even if the model's wait/notify path regresses.
  require(!dlg::confirm("no host window"), "absent host window cancels instead of accepting");
  window_live = true;
  const auto released = dlg::released_count();

  // A button already held when opened must not accidentally accept a confirmation.
  buttons[0] = XINPUT_GAMEPAD_A;
  auto question = std::async(std::launch::async, [] { return dlg::confirm("confirm"); });
  wait_visible();
  dlg::tick();
  require(question.wait_for(0ms) == std::future_status::timeout, "held opening button is suppressed");
  const auto original = view();
  dlg::on_key(0x1b, true, false);
  dlg::on_char('Q');
  dlg::on_mouse_move(900, 600);
  dlg::on_mouse_button(0, true, 900, 600);
  dlg::on_wheel(-120);
  require(question.wait_for(0ms) == std::future_status::timeout && view().button == original.button && !dlg::cursor_wanted(),
          "Mac key/mouse callbacks cannot drive system dialogs");
  tap(XINPUT_GAMEPAD_DPAD_RIGHT);
  require(view().button == 1, "controller can choose NO");
  tap(XINPUT_GAMEPAD_A);
  require(!answered(question), "NO choice reaches caller");

  release_buttons();
  std::vector<dlg::ListItem> items(10);
  for (int i = 0; i < 10; ++i) { items[i].title = "row " + std::to_string(i); items[i].mtime = 1700000000; }
  auto list = std::async(std::launch::async, [&] { return dlg::choose("saved data", items, 7); });
  wait_visible();
  require(view().focus == 7 && view().scroll == 2 && !view().dates[0].empty(), "initial list focus, scroll and POSIX date conversion");
  tap(XINPUT_GAMEPAD_DPAD_DOWN, 3);
  require(view().focus == 8, "every supported pad slot can navigate");
  dlg::tick();
  require(view().focus == 8, "held direction does not repeat immediately");
  std::this_thread::sleep_for(400ms);
  dlg::tick();
  require(view().focus == 9 && view().scroll == 4, "held direction repeats after delay and scrolls");
  tap(XINPUT_GAMEPAD_A, 3);
  require(answered(list) == 9, "list selection is the actual focused row");

  release_buttons();
  auto cancel = std::async(std::launch::async, [&] { return dlg::choose("cancel", items, 0); });
  wait_visible();
  tap(XINPUT_GAMEPAD_B);
  require(answered(cancel) == -1, "Circle/B returns list cancellation");

  release_buttons();
  std::u16string text;
  auto keyboard = std::async(std::launch::async, [&] { return dlg::text_input("name", u"OLD", 2, &text); });
  wait_visible();
  require(view().edit == u"OL" && view().edit_selected, "text entry preserves initial selection and limit");
  tap(XINPUT_GAMEPAD_A);
  require(view().edit == u"A", "first on-screen key replaces initial text");
  tap(XINPUT_GAMEPAD_DPAD_RIGHT);
  tap(XINPUT_GAMEPAD_A);
  tap(XINPUT_GAMEPAD_Y);
  require(view().edit == u"AB", "controller typing obeys max characters");
  tap(XINPUT_GAMEPAD_X);
  tap(XINPUT_GAMEPAD_Y);
  require(view().edit == u"A ", "Square/X erases and Triangle/Y adds space");
  tap(XINPUT_GAMEPAD_START);
  require(answered(keyboard) && text == u"A ", "START accepts actual controller text");

  release_buttons();
  auto aborted = std::async(std::launch::async, [] { return dlg::confirm("abort"); });
  wait_visible();
  dlg::abort();
  require(!answered(aborted), "shutdown abort wakes modal caller as cancellation");

  release_buttons();
  auto yes = std::async(std::launch::async, [] { return dlg::confirm("yes"); });
  wait_visible();
  tap(XINPUT_GAMEPAD_A);
  require(answered(yes), "YES choice returns true rather than a fixed cancellation");

  release_buttons();
  auto message = std::async(std::launch::async, [] { dlg::message("message"); return true; });
  wait_visible();
  require(view().kind == rt::ui::DialogView::Message, "message publishes its actual model");
  tap(XINPUT_GAMEPAD_A);
  require(answered(message), "controller acknowledgement releases a message caller");
  require(dlg::released_count() == released + 7, "all modal completions publish controller release");
  alarm(0);
  std::puts("native-sysdialog: real model/controller selection, text, repeat and abort passed (headless fixtures; no displayed UI)");
}
