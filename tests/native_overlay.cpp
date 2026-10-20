// Offline exercise of the actual overlay model, CoreText raster and Metal compositor.
// Including the implementation exposes its private model only in this test translation unit;
// do not also compile overlay.cpp into this test target. All host/window/settings seams below
// are fixtures: they cannot open UI, modify a settings file, or poll physical controllers.
#include "../src/runtime/ui/overlay.cpp"
#include <ImageIO/ImageIO.h>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {
std::atomic<WORD> test_buttons{0};
acfa::settings::Display test_display;
unsigned binding_writes = 0;
void require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void ok(HRESULT result, ID3D11Device* device, const char* what) {
  if (FAILED(result)) { std::fprintf(stderr, "FAIL: %s: %s\n", what, acfa::metal::last_error(device).c_str()); std::exit(1); }
}
void tap(WORD buttons) {
  test_buttons = 0; rt::ui::tick();
  test_buttons = buttons; rt::ui::tick();
}
void png(const std::vector<u8>& rgba, const char* path) {
  const auto space = CGColorSpaceCreateDeviceRGB();
  const auto data = CFDataCreate(kCFAllocatorDefault, rgba.data(), CFIndex(rgba.size()));
  const auto provider = CGDataProviderCreateWithCFData(data);
  const auto image = CGImageCreate(1280, 720, 8, 32, 1280 * 4, space,
                                   CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big),
                                   provider, nullptr, false, kCGRenderingIntentDefault);
  const auto url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path), std::strlen(path), false);
  const auto destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  require(destination != nullptr, "PNG artifact destination");
  CGImageDestinationAddImage(destination, image, nullptr);
  require(CGImageDestinationFinalize(destination), "PNG artifact written");
  CFRelease(destination); CFRelease(url); CGImageRelease(image); CGDataProviderRelease(provider); CFRelease(data); CGColorSpaceRelease(space);
}
}  // namespace

namespace rt {
Config& cfg() { static Config config; return config; }
Level log_level() { return Level::Fatal; }
void log_write(Level, const char*, const std::string&) {}
bool input_isolated() { return false; }
namespace pads {
bool get(int slot, XINPUT_STATE* out) {
  if (!out || slot < 0 || slot >= kSlots) return false;
  *out = {};
  if (slot == 0) out->Gamepad.wButtons = test_buttons.load();
  return true;
}
}
namespace settings {
acfa::settings::Display display() { return test_display; }
void set_fps(double value) { cfg().fps = value; test_display.fps = value; }
void set_mode(acfa::settings::Mode value) { test_display.mode = value; }
void set_window_size(int w, int h) { test_display.width = w; test_display.height = h; }
void set_fullscreen_mode(int w, int h, int hz) { test_display.fs_width = w; test_display.fs_height = h; test_display.fs_hz = hz; }
void set_render_scale(int value) { test_display.render_scale = value; }
void set_monitor(const std::string& value) { test_display.monitor = value; }
void save_bindings() { ++binding_writes; }
}
namespace gpu {
void* window_handle() { return reinterpret_cast<void*>(std::uintptr_t(1)); }
void window_refresh_cursor() {}
bool window_mouse_captured() { return false; }
void window_set_mouse_capture(bool) {}
void window_client_size(int* w, int* h) { *w = 1280; *h = 720; }
void window_monitor_size(int* w, int* h) { *w = 2560; *h = 1440; }
void window_work_area(int* w, int* h) { *w = 2560; *h = 1400; }
std::vector<Monitor> window_monitors() { return {{"display:test", 0, 0, 2560, 1440, true}}; }
namespace display {
std::vector<Mode> modes() { return {}; }
u32 refresh_hz() { return 60; }
}
}
}  // namespace rt

int main(int argc, char** argv) {
  alarm(30);
  test_display.mode = acfa::settings::Mode::Windowed;
  rt::ui::MacCanvas marker;
  require(marker.cg != nullptr, "real CoreGraphics bitmap allocated without AppKit");
  marker.fill({20, 30, 40, 50}, 1, 0, 0);
  CGContextFlush(marker.cg);
  const auto marker_at = [&](unsigned x, unsigned y) { return marker.pixels[(y * 1280 + x) * 4]; };
  require(marker_at(25, 35) == 255 && marker_at(25, 720 - 35) == 0, "top-left model coordinates match upload rows");

  ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr;
  ok(acfa::metal::create_device(&device, &context), device, "headless Metal device");
  D3D11_TEXTURE2D_DESC description{};
  description.Width = 1280; description.Height = 720; description.MipLevels = description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* backbuffer = nullptr; ID3D11RenderTargetView* target = nullptr;
  ok(device->CreateTexture2D(&description, nullptr, &backbuffer), device, "headless backbuffer");
  ok(device->CreateRenderTargetView(backbuffer, nullptr, &target), device, "headless target");
  context->OMSetRenderTargets(1, &target, nullptr);
  const D3D11_VIEWPORT previous_viewport{7, 11, 400, 200, 0, 1};
  context->RSSetViewports(1, &previous_viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  const FLOAT background[] = {0.5f, 0.25f, 0.125f, 1};
  context->ClearRenderTargetView(target, background);

  // Draw the real blocking system-dialog model over a known colour.
  auto question = std::async(std::launch::async, [] { return rt::sysdlg::confirm("中文保存数据\n确认保存到本地测试区域？"); });
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!rt::sysdlg::shown() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(rt::sysdlg::shown(), "real system-dialog model became visible to renderer");
  rt::ui::render(device, context, backbuffer, 1280, 720);
  std::vector<u8> pixels; uint32_t width = 0, height = 0;
  require(acfa::metal::readback_texture(context, backbuffer, pixels, width, height), "actual UI composite GPU readback");
  require(width == 1280 && height == 720, "overlay target dimensions");
  const auto at = [&](unsigned x, unsigned y, unsigned channel) { return pixels[(y * width + x) * 4 + channel]; };
  require(at(20, 20, 0) >= 49 && at(20, 20, 0) <= 53 && at(20, 20, 1) >= 24 && at(20, 20, 1) <= 27,
          "premultiplied dialog dimming blends the existing backbuffer");
  require(at(300, 230, 0) < 20 && at(300, 230, 3) == 255, "opaque dialog panel is composited at expected top-left position");
  unsigned glyphs = 0;
  for (unsigned y = 307; y < 410; ++y) for (unsigned x = 330; x < 940; ++x)
    if (at(x, y, 0) > 100) ++glyphs;
  require(glyphs > 100, "actual CoreText UTF-8 body produces visible glyph pixels");
  if (argc > 1) png(pixels, argv[1]);
  D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
  UINT count = 1; D3D11_VIEWPORT restored{}; context->RSGetViewports(&count, &restored);
  ID3D11RenderTargetView* restored_target = nullptr; context->OMGetRenderTargets(1, &restored_target, nullptr);
  require(topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && restored.TopLeftX == 7 && restored.Width == 400 && restored_target == target,
          "overlay restores real renderer target, viewport and primitive topology");
  restored_target->Release();
  rt::sysdlg::abort();
  require(question.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !question.get(), "dialog can close after drawing");

  // Exercise the actual one-page settings model and its controller actions.
  test_buttons = XINPUT_GAMEPAD_A;
  rt::ui::open_from_menu(); rt::ui::tick();
  require(rt::cfg().fps == 60, "held opening button cannot change a setting");
  require(rt::ui::is_open() && rt::ui::g_page == 0, "controller settings opens actual DISPLAY page");
  for (const auto& row : rt::ui::g_rows) require(row.kind != rt::ui::Kind::Binding, "Mac model has no keyboard/mouse binding rows");
  tap(XINPUT_GAMEPAD_RIGHT_SHOULDER);
  require(rt::ui::g_page == 0 && binding_writes == 0, "shoulder navigation cannot expose bindings pages");
  tap(XINPUT_GAMEPAD_A);
  require(rt::cfg().fps == 120, "controller changes actual frame-rate setting");
  rt::ui::on_key(0x1b, true, false); rt::ui::on_mouse_button(1, true, 100, 100);
  require(rt::ui::g_state == rt::ui::State::Open, "Mac keyboard/mouse callbacks do not operate settings");
  context->ClearRenderTargetView(target, background);
  rt::ui::render(device, context, backbuffer, 1280, 720);
  require(acfa::metal::readback_texture(context, backbuffer, pixels, width, height), "settings page GPU readback");
  require(at(20, 20, 0) == 0 && at(20, 20, 3) == 255, "settings draws an opaque black page");
  unsigned title_pixels = 0;
  for (unsigned y = 34; y < 101; ++y) for (unsigned x = 0; x < 330; ++x)
    if (at(x, y, 0) > 100) ++title_pixels;
  require(title_pixels > 200, "CoreText settings heading is present at the top");
  tap(XINPUT_GAMEPAD_DPAD_DOWN); tap(XINPUT_GAMEPAD_A);
  require(test_display.mode == acfa::settings::Mode::Borderless, "DISPLAY MODE maps controller choice to supported borderless mode");
  rt::ui::g_mac_gfx.reset();
  context->ClearState(); target->Release(); backbuffer->Release(); context->Release(); device->Release();
  alarm(0);
  std::puts("native-overlay: real controller model, UTF-8 CoreText raster, Metal premultiplied composite and state restore passed (headless)");
}
