// Host audio output (WASAPI on Windows; default-output AudioUnit on macOS).
// Both accept 48 kHz float stereo with OS-side conversion. One mixer thread writes PCM.
#pragma once
#include "common/bytes.h"

namespace rt::audio {

bool out_init();
bool out_available();
// Frames waiting for host playback (0 if unavailable); excludes fixed hardware latency.
u32 out_queued_frames();
// Writes interleaved stereo float frames (non-blocking; drops if the device buffer is full).
void out_write(const float* stereo, u32 frames);

}  // namespace rt::audio
