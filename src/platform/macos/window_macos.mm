// Native AppKit window only. No game keyboard/mouse handlers or cursor capture.
// All AppKit mutation and event delivery happens on the process main thread.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <pthread.h>

#include "common/input_policy.h"
#include "platform/macos/metal_surface.h"
#include "runtime/config.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/gamepads.h"
#include "runtime/log.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

static_assert(!acfa::input_policy::keyboard_mouse,
              "The native Mac branch implements controller-only input");

namespace rt::gpu::mac_detail {
void update_geometry();
void request_close();
void window_closed();
void focus_changed(bool focused);
bool fullscreen();
void screen_changed();
}  // namespace rt::gpu::mac_detail

@interface ACFAMetalView : NSView
@end
@implementation ACFAMetalView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return NO; }
- (void)keyDown:(NSEvent*)event { (void)event; }
- (void)keyUp:(NSEvent*)event { (void)event; }
- (void)flagsChanged:(NSEvent*)event { (void)event; }
- (void)mouseDown:(NSEvent*)event { (void)event; }
- (void)mouseUp:(NSEvent*)event { (void)event; }
- (void)rightMouseDown:(NSEvent*)event { (void)event; }
- (void)rightMouseUp:(NSEvent*)event { (void)event; }
- (void)otherMouseDown:(NSEvent*)event { (void)event; }
- (void)otherMouseUp:(NSEvent*)event { (void)event; }
- (void)mouseMoved:(NSEvent*)event { (void)event; }
- (void)mouseDragged:(NSEvent*)event { (void)event; }
- (void)rightMouseDragged:(NSEvent*)event { (void)event; }
- (void)otherMouseDragged:(NSEvent*)event { (void)event; }
- (void)scrollWheel:(NSEvent*)event { (void)event; }
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  rt::gpu::mac_detail::update_geometry();
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  rt::gpu::mac_detail::update_geometry();
}
@end

@interface ACFAWindow : NSWindow
@end
@implementation ACFAWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen*)screen {
  // A borderless window covers the complete screen, not just its visibleFrame.
  if (rt::gpu::mac_detail::fullscreen()) return frame;
  return [super constrainFrameRect:frame toScreen:screen];
}
@end

@interface ACFAWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation ACFAWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  rt::gpu::mac_detail::request_close();
  // Match the Windows path: first request the guest's graceful exit. The runtime
  // still owns the window and its presentation surface until the process exits.
  return NO;
}
- (void)windowWillClose:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::window_closed();
}
- (void)windowDidResize:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::update_geometry();
}
- (void)windowDidMove:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::update_geometry();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::update_geometry();
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::screen_changed();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::focus_changed(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::focus_changed(false);
}
@end

@interface ACFAApplicationDelegate : NSObject <NSApplicationDelegate>
@end
@implementation ACFAApplicationDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  (void)sender;
  rt::gpu::mac_detail::request_close();
  return NSTerminateCancel;
}
- (void)applicationDidChangeScreenParameters:(NSNotification*)notification {
  (void)notification;
  rt::gpu::mac_detail::screen_changed();
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return NO;
}
@end

namespace rt::gpu {
namespace {

constexpr NSWindowStyleMask kWindowedStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

// This module is compiled with ARC. The window/surface are deliberately retained
// for process lifetime, matching the runtime's other host resources. Creation is
// allowed once, so a presenting thread can never observe a released old layer.
ACFAWindow* g_window = nil;
ACFAMetalView* g_view = nil;
CAMetalLayer* g_layer = nil;
ACFAWindowDelegate* g_window_delegate = nil;
ACFAApplicationDelegate* g_application_delegate = nil;
bool g_created = false;
std::atomic<void*> g_handle{nullptr};
std::atomic<void*> g_layer_handle{nullptr};
std::atomic<bool> g_alive{false};
std::atomic<bool> g_close{false};
std::atomic<bool> g_fullscreen{false};
NSRect g_windowed_frame{};
NSApplicationPresentationOptions g_windowed_presentation = NSApplicationPresentationDefault;

struct Geometry {
  int width = 0, height = 0;
  int work_width = 0, work_height = 0;
  int monitor_width = 0, monitor_height = 0;
  double scale = 1;
  std::uint32_t display = 0;
  std::vector<Monitor> monitors;
};
std::mutex g_geometry_mutex;
Geometry g_geometry;

struct MainCall {
  std::function<void()> fn;
  bool done = false, executed = false;
  std::exception_ptr error;
};
std::mutex g_call_mutex;
std::condition_variable g_call_cv;
std::deque<std::shared_ptr<MainCall>> g_calls;

bool main_thread() { return pthread_main_np() != 0; }

int pixels(CGFloat value) { return static_cast<int>(std::lround(value)); }

NSScreen* primary_screen() { return [[NSScreen screens] firstObject]; }
NSScreen* current_screen() { return [g_window screen] ?: primary_screen(); }

std::string screen_id(NSScreen* screen) {
  NSNumber* number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
  return "display:" + std::to_string([number unsignedIntValue]);
}

std::vector<Monitor> list_monitors() {
  std::vector<Monitor> result;
  NSScreen* primary = primary_screen();
  const NSRect primary_backing = primary ? [primary convertRectToBacking:[primary frame]] : NSZeroRect;
  for (NSScreen* screen in [NSScreen screens]) {
    // Use AppKit's coordinate conversions, not a guessed Retina multiplier.
    const NSRect backing = [screen convertRectToBacking:[screen frame]];
    Monitor monitor;
    monitor.device = screen_id(screen);
    monitor.x = pixels(NSMinX(backing) - NSMinX(primary_backing));
    monitor.y = pixels(NSMaxY(primary_backing) - NSMaxY(backing));
    monitor.w = pixels(NSWidth(backing));
    monitor.h = pixels(NSHeight(backing));
    monitor.primary = screen == primary;
    result.push_back(std::move(monitor));
  }
  return result;
}

NSScreen* find_screen(const std::string& device) {
  for (NSScreen* screen in [NSScreen screens])
    if (screen_id(screen) == device) return screen;
  return primary_screen();
}

NSRect fitted_frame(NSScreen* screen, int width, int height) {
  if (!screen || width <= 0 || height <= 0) return NSZeroRect;
  const NSRect work = [screen visibleFrame];
  const NSRect content = [screen convertRectFromBacking:NSMakeRect(0, 0, width, height)];
  const NSRect decorated = [NSWindow frameRectForContentRect:content styleMask:kWindowedStyle];
  const CGFloat frame_w = NSWidth(decorated) - NSWidth(content);
  const CGFloat frame_h = NSHeight(decorated) - NSHeight(content);
  if (NSWidth(work) <= frame_w || NSHeight(work) <= frame_h) return NSZeroRect;
  const CGFloat fit = std::min<CGFloat>({1, (NSWidth(work) - frame_w) / NSWidth(content),
                                        (NSHeight(work) - frame_h) / NSHeight(content)});
  const NSSize size = NSMakeSize(NSWidth(content) * fit + frame_w, NSHeight(content) * fit + frame_h);
  return NSMakeRect(NSMidX(work) - size.width / 2, NSMidY(work) - size.height / 2, size.width, size.height);
}

void set_client_size(int width, int height) {
  if (!g_window || width <= 0 || height <= 0) return;
  const NSRect frame = fitted_frame(current_screen(), width, height);
  if (NSIsEmptyRect(frame)) return;
  if (g_fullscreen.load()) g_windowed_frame = frame;
  else [g_window setFrame:frame display:NO];
  mac_detail::update_geometry();
}

void set_fullscreen(bool on) {
  if (!g_window || on == g_fullscreen.load()) return;
  if (on) {
    NSScreen* screen = current_screen();
    if (!screen) return;
    g_windowed_frame = [g_window frame];
    g_windowed_presentation = [NSApp presentationOptions];
    g_fullscreen.store(true);
    [g_window setStyleMask:NSWindowStyleMaskBorderless];
    [g_window setFrame:[screen frame] display:NO];
    [NSApp setPresentationOptions:NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar];
  } else {
    g_fullscreen.store(false);
    [NSApp setPresentationOptions:g_windowed_presentation];
    [g_window setStyleMask:kWindowedStyle];
    [g_window setFrame:g_windowed_frame display:NO];
  }
  mac_detail::update_geometry();
}

void cancel_calls() {
  {
    std::lock_guard lock(g_call_mutex);
    for (const auto& call : g_calls) call->done = true;
    g_calls.clear();
  }
  g_call_cv.notify_all();
}

void drain_calls() {
  for (;;) {
    std::shared_ptr<MainCall> call;
    {
      std::lock_guard lock(g_call_mutex);
      if (g_calls.empty()) return;
      call = g_calls.front();
      g_calls.pop_front();
    }
    std::exception_ptr error;
    const bool execute = g_alive.load();
    if (execute) {
      try { call->fn(); } catch (...) { error = std::current_exception(); }
    }
    {
      std::lock_guard lock(g_call_mutex);
      call->error = error;
      call->executed = execute;
      call->done = true;
    }
    g_call_cv.notify_all();
  }
}

void dimensions(int* width, int* height, int value_w, int value_h) {
  if (width) *width = value_w;
  if (height) *height = value_h;
}

}  // namespace

namespace mac_detail {

bool fullscreen() { return g_fullscreen.load(); }

void update_geometry() {
  if (!main_thread() || !g_window || !g_view || !g_layer) return;
  const NSRect bounds = [g_view bounds];
  const NSRect backing = [g_view convertRectToBacking:bounds];
  const auto width = std::max(0, pixels(NSWidth(backing)));
  const auto height = std::max(0, pixels(NSHeight(backing)));
  const CGFloat scale = [g_window backingScaleFactor];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  g_layer.frame = NSRectToCGRect(bounds);
  g_layer.contentsScale = scale;
  g_layer.drawableSize = CGSizeMake(width, height);
  [CATransaction commit];
  Geometry next;
  next.width = width;
  next.height = height;
  next.scale = scale;
  next.monitors = list_monitors();
  if (NSScreen* screen = current_screen()) {
    next.display = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    const NSRect work = [screen convertRectToBacking:[screen visibleFrame]];
    const NSRect monitor = [screen convertRectToBacking:[screen frame]];
    next.work_width = pixels(NSWidth(work));
    next.work_height = pixels(NSHeight(work));
    next.monitor_width = pixels(NSWidth(monitor));
    next.monitor_height = pixels(NSHeight(monitor));
  }
  std::lock_guard lock(g_geometry_mutex);
  g_geometry = std::move(next);
}

void request_close() {
  g_close.store(true);
  sysdlg::abort();
}

void window_closed() {
  g_close.store(true);
  g_alive.store(false);
  g_handle.store(nullptr);
  g_layer_handle.store(nullptr);
  {
    std::lock_guard lock(g_geometry_mutex);
    g_geometry.width = g_geometry.height = 0;
  }
  if (g_fullscreen.exchange(false)) [NSApp setPresentationOptions:g_windowed_presentation];
  cancel_calls();
}

void focus_changed(bool focused) {
  if (!input_isolated()) input_focus(focused);
}

void screen_changed() {
  if (g_window && g_fullscreen.load()) {
    if (NSScreen* screen = current_screen()) [g_window setFrame:[screen frame] display:NO];
  }
  update_geometry();
}

}  // namespace mac_detail

void window_dpi_aware() { /* Cocoa uses backing-coordinate conversions per window. */ }

bool window_create(const std::string& title, int width, int height, bool visible) {
  if (!main_thread() || width <= 0 || height <= 0 || g_created) {
    LOG_E("gpu", "Mac window creation requires the main thread, positive dimensions and a fresh process");
    return false;
  }
  @autoreleasepool {
    NSApplication* application = [NSApplication sharedApplication];
    if (![application setActivationPolicy:NSApplicationActivationPolicyRegular]) {
      LOG_E("gpu", "macOS declined the game's regular application policy");
      return false;
    }
    g_application_delegate = [[ACFAApplicationDelegate alloc] init];
    [application setDelegate:g_application_delegate];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"ACFA"];
    NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:@"ACFA" action:nil keyEquivalent:@""];
    NSMenu* app_menu = [[NSMenu alloc] initWithTitle:@"ACFA"];
    [app_menu addItemWithTitle:@"Quit ACFA" action:@selector(terminate:) keyEquivalent:@"q"];
    [app_item setSubmenu:app_menu];
    [menu addItem:app_item];
    [application setMainMenu:menu];
    [application finishLaunching];
    // Initialize GameController on the main thread before guest pad polling.
    // Discovery/input delivery is asynchronous and is serviced by window_pump.
    XINPUT_STATE initial_pad{};
    pads::get(0, &initial_pad);
    NSScreen* screen = primary_screen();
    if (!screen) {
      LOG_E("gpu", "no macOS display is available for a window");
      return false;
    }
    g_window = [[ACFAWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
                                            styleMask:kWindowedStyle backing:NSBackingStoreBuffered
                                                defer:NO screen:screen];
    if (!g_window) return false;
    [g_window setReleasedWhenClosed:NO];
    [g_window setTitle:[[NSString alloc] initWithBytes:title.data() length:title.size() encoding:NSUTF8StringEncoding] ?: @"ACFA"];
    g_window_delegate = [[ACFAWindowDelegate alloc] init];
    [g_window setDelegate:g_window_delegate];
    g_view = [[ACFAMetalView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    g_layer = [CAMetalLayer layer];
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = YES;
    g_layer.displaySyncEnabled = YES;
    [g_view setLayer:g_layer];
    [g_view setWantsLayer:YES];
    [g_window setContentView:g_view];
    [g_window makeFirstResponder:g_view];
    g_created = true;
    g_close.store(false);
    g_alive.store(true);
    g_handle.store((__bridge void*)g_window);
    g_layer_handle.store((__bridge void*)g_layer);
    set_client_size(width, height);
    if (visible) {
      // Initial launch never takes the user's keyboard focus. Clicking the
      // window can make it key normally; GameController input remains enabled.
      [g_window orderFront:nil];
      // No activateIgnoringOtherApps: creating the game must not force another
      // foreground application to give up its focus.
    }
    mac_detail::update_geometry();
    return true;
  }
}

void* window_handle() { return g_handle.load(); }
bool window_close_requested() { return g_close.load(); }

bool window_pump() {
  if (!main_thread() || !g_alive.load()) return false;
  @autoreleasepool {
    // An expired nextEvent deadline only polls the AppKit event queue. Give
    // CoreFoundation/GCD sources time too (GameController discovery and reports,
    // plus CoreAnimation transaction delivery) without taking keyboard focus.
    [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                          beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    for (;;) {
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
      if (!event) break;
      [NSApp sendEvent:event];
    }
    drain_calls();
    ui::tick();
    sysdlg::tick();
    [NSApp updateWindows];
  }
  return g_alive.load();
}

bool run_on_window_thread(const std::function<void()>& fn) {
  if (!fn || !g_alive.load()) return false;
  if (main_thread()) { fn(); return true; }
  auto call = std::make_shared<MainCall>();
  call->fn = fn;
  std::unique_lock lock(g_call_mutex);
  if (!g_alive.load()) return false;
  g_calls.push_back(call);
  // The runtime pumps every main-loop iteration. No dispatch_sync or AppKit API
  // is called off-main, and closing the window cancels queued callers safely.
  g_call_cv.wait(lock, [&] { return call->done; });
  const auto error = call->error;
  const bool executed = call->executed;
  lock.unlock();
  if (error) std::rethrow_exception(error);
  return executed;
}

void window_set_title(const std::string& title) {
  run_on_window_thread([title] {
    [g_window setTitle:[[NSString alloc] initWithBytes:title.data() length:title.size() encoding:NSUTF8StringEncoding] ?: @"ACFA"];
  });
}
void window_set_fullscreen(bool on) { run_on_window_thread([on] { set_fullscreen(on); }); }
bool window_is_fullscreen() { return g_fullscreen.load(); }
void window_set_client_size(int width, int height) {
  run_on_window_thread([width, height] { set_client_size(width, height); });
}
void window_client_size(int* width, int* height) {
  std::lock_guard lock(g_geometry_mutex);
  dimensions(width, height, g_geometry.width, g_geometry.height);
}
void window_work_area(int* width, int* height) {
  std::lock_guard lock(g_geometry_mutex);
  dimensions(width, height, g_geometry.work_width, g_geometry.work_height);
}
void window_monitor_size(int* width, int* height) {
  std::lock_guard lock(g_geometry_mutex);
  dimensions(width, height, g_geometry.monitor_width, g_geometry.monitor_height);
}
std::vector<Monitor> window_monitors() {
  if (main_thread()) {
    auto monitors = list_monitors();
    std::lock_guard lock(g_geometry_mutex);
    g_geometry.monitors = monitors;
    return monitors;
  }
  std::lock_guard lock(g_geometry_mutex);
  return g_geometry.monitors;
}
void window_move_to_monitor(const std::string& device) {
  run_on_window_thread([device] {
    NSScreen* screen = find_screen(device);
    if (!screen) return;
    if (g_fullscreen.load()) [g_window setFrame:[screen frame] display:NO];
    else {
      int width, height;
      window_client_size(&width, &height);
      const auto frame = fitted_frame(screen, width, height);
      if (!NSIsEmptyRect(frame)) [g_window setFrame:frame display:NO];
    }
    mac_detail::update_geometry();
  });
}

// The user explicitly requested controller-only operation. These remain no-ops
// even before window creation and never hide, warp, confine or capture the cursor.
void window_set_mouse_capture(bool) {}
bool window_mouse_captured() { return false; }
void window_request_mouse_capture(bool) {}
void window_refresh_cursor() {}

namespace mac {
void* metal_layer() { return g_layer_handle.load(); }
void drawable_size(int* width, int* height) { window_client_size(width, height); }
double backing_scale() { std::lock_guard lock(g_geometry_mutex); return g_geometry.scale; }
std::uint32_t display_id() {
  if (!g_alive.load()) return 0;
  std::lock_guard lock(g_geometry_mutex); return g_geometry.display;
}
}  // namespace mac

}  // namespace rt::gpu
