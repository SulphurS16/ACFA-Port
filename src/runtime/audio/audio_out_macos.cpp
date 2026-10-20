// Native macOS default-output AudioUnit. Apple's output unit converts this 48 kHz
// client format to the selected device's format/rate; no device settings are changed.
#include "runtime/audio/audio_out.h"
#include "runtime/audio/audio_out_macos.h"

#include <atomic>
#include <mutex>

#include "runtime/log.h"

namespace rt::audio {
namespace {

mac::OutputFifo g_fifo;
std::atomic<bool> g_ok{false};
std::mutex g_init_mutex;
// Process-lifetime, matching the upstream cellAudio mixer thread and WASAPI path.
AudioUnit g_unit = nullptr;

OSStatus render(void*, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                UInt32, UInt32 frames, AudioBufferList* buffers) noexcept {
  // No allocation, mutex, device API or logging is allowed on this thread.
  return mac::render(g_fifo, flags, frames, buffers);
}

}  // namespace

bool out_init() {
  std::lock_guard lock(g_init_mutex);
  if (g_ok.load(std::memory_order_acquire)) return true;
  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_DefaultOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  const auto component = AudioComponentFindNext(nullptr, &description);
  if (!component) {
    LOG_W("audio", "no macOS default output AudioUnit; audio output disabled");
    return false;
  }
  AudioUnit unit = nullptr;
  OSStatus status = AudioComponentInstanceNew(component, &unit);
  if (status != noErr) {
    LOG_W("audio", "AudioComponentInstanceNew failed %d; audio output disabled", int(status));
    return false;
  }
  bool initialized = false;
  auto failed = [&](const char* operation) {
    LOG_W("audio", "%s failed %d; audio output disabled", operation, int(status));
    if (initialized) AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return false;
  };
  const auto format = mac::stream_format();
  status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                &format, sizeof(format));
  if (status != noErr) return failed("AudioUnit client format");
  AURenderCallbackStruct callback{render, nullptr};
  status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                &callback, sizeof(callback));
  if (status != noErr) return failed("AudioUnit render callback");
  status = AudioUnitInitialize(unit);
  if (status != noErr) return failed("AudioUnitInitialize");
  initialized = true;
  status = AudioOutputUnitStart(unit);
  if (status != noErr) return failed("AudioOutputUnitStart");
  g_unit = unit;
  g_ok.store(true, std::memory_order_release);
  LOG_I("audio", "CoreAudio output: 48 kHz float stereo, FIFO capacity %u frames", mac::kQueueFrames);
  return true;
}

bool out_available() { return g_ok.load(std::memory_order_acquire); }

u32 out_queued_frames() {
  // Client PCM waiting for an AudioUnit pull. Already-rendered hardware latency is
  // deliberately not counted; including a fixed device latency would bias the
  // existing cellAudio queue feedback loop. The mixer targets ~36 ms of client PCM.
  return out_available() ? static_cast<u32>(g_fifo.queued()) : 0;
}

void out_write(const float* stereo, u32 frames) {
  if (out_available()) (void)g_fifo.write(stereo, frames);
}

}  // namespace rt::audio
