// Display timing: where the emulated vertical blank comes from, and how presented frames
// actually reached the screen.
//
// The game paces its frames by counting vblanks (flip after 1 or 2, GAME_NOTES "Frame pacing").
// On a display whose refresh is a multiple of about 60 Hz (60, 120, 180, 240 Hz) the emulated
// vblank follows the display's own vblank (IDXGIOutput::WaitForVBlank, every N-th one), so each
// game frame stays on screen for the same number of refreshes. On other refresh rates (144, 165
// Hz...) it ticks from a 60 Hz clock and frames are presented with tearing allowed, so displays
// with variable refresh show them evenly (fixed-refresh displays at such rates cannot show 60 fps
// evenly). Presentation statistics (DXGI frame statistics) measure what was displayed: how many
// refreshes each frame stayed up and the intervals between displayed frames.
#pragma once
#include <string>

#include "common/bytes.h"

struct IDXGISwapChain1;

namespace rt::gpu::display {

// Blocks the vblank thread until the next emulated vblank is due.
void wait_next_vblank();
// Present flags for the current pacing (DXGI_PRESENT_ALLOW_TEARING in clock mode when supported).
u32 present_flags();
// The swap chain was created (tearing: created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING). Called on
// the presenting thread.
void swapchain_created(IDXGISwapChain1* swap, bool tearing);
// After each Present (presenting thread): frame statistics and the containing output.
void after_present(IDXGISwapChain1* swap);
// One line for the perf log (since the previous call).
std::string report();

}  // namespace rt::gpu::display
