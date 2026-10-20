// Native Mac window surface for the future Metal presenter. Does not import AppKit
// into portable C++ headers and does not create a window/device when queried.
#pragma once
#include <cstdint>

namespace rt::gpu::mac {

// Borrowed CAMetalLayer* (cast with __bridge in Objective-C++), or nullptr when no
// live window exists. The window retains its layer for its process lifetime; the
// renderer must not transfer ownership. Set its Metal device before nextDrawable.
void* metal_layer();
// Drawable dimensions are backing-store pixels, including Retina scaled modes;
// this is intentionally not a claim about the panel's native physical resolution.
void drawable_size(int* width, int* height);
double backing_scale();
// Cached NSScreenNumber; never queries AppKit from the renderer. Zero without a live window.
std::uint32_t display_id();

}  // namespace rt::gpu::mac
