// Offline only: never calls out_init(), AudioUnitInitialize(), or device APIs.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "runtime/audio/audio_out.h"
#include "runtime/audio/audio_out_macos.h"
#include "runtime/log.h"

namespace rt {
Level log_level() { return Level::Error; }
void log_write(Level, const char*, const std::string&) {}
}  // namespace rt

namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

AudioBufferList buffer_list(float* pcm, UInt32 frames) {
  AudioBufferList result{};
  result.mNumberBuffers = 1;
  result.mBuffers[0] = AudioBuffer{2, frames * 2 * UInt32(sizeof(float)), pcm};
  return result;
}

void format_and_callback() {
  using namespace rt::audio;
  const auto format = mac::stream_format();
  check(format.mSampleRate == 48000 && format.mChannelsPerFrame == 2, "PCM rate/channel format");
  check(format.mFormatID == kAudioFormatLinearPCM && format.mBitsPerChannel == 32, "float PCM format");
  check((format.mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked)) ==
        (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked), "PCM float/packed flags");
  check((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0 &&
        format.mBytesPerFrame == 8 && format.mBytesPerPacket == 8 && format.mFramesPerPacket == 1,
        "interleaved frame format");

  mac::OutputFifo fifo;
  std::array<float, 16> pcm;
  pcm.fill(9);
  auto buffers = buffer_list(pcm.data(), 8);
  AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_PreRender;
  check(mac::render(fifo, &flags, 8, &buffers) == noErr, "empty callback status");
  check(std::all_of(pcm.begin(), pcm.end(), [](float n) { return n == 0; }), "underflow not zero-filled");
  check((flags & kAudioUnitRenderAction_OutputIsSilence) && (flags & kAudioUnitRenderAction_PreRender),
        "empty callback silence flag/preserved flags");
  const float source[] = {0.5f, -0.25f, 1.0f, -1.0f};
  check(fifo.write(source, 2), "write callback input");
  pcm.fill(9);
  buffers = buffer_list(pcm.data(), 8);
  check(mac::render(fifo, &flags, 8, &buffers) == noErr, "partial callback status");
  check(std::equal(std::begin(source), std::end(source), pcm.begin()), "callback stereo sample ordering");
  check(std::all_of(pcm.begin() + 4, pcm.end(), [](float n) { return n == 0; }), "partial underflow tail");
  check((flags & kAudioUnitRenderAction_OutputIsSilence) == 0 && fifo.queued() == 0, "partial callback count/flags");
  check(mac::render(fifo, nullptr, 8, &buffers) == noErr, "nullable action flags");
  check(std::all_of(pcm.begin(), pcm.end(), [](float n) { return n == 0; }), "stale PCM replayed");

  check(fifo.write(source, 2), "write before invalid callback");
  pcm.fill(9);
  buffers = buffer_list(pcm.data(), 8);
  buffers.mBuffers[0].mDataByteSize = 4;
  check(mac::render(fifo, &flags, 8, &buffers) == kAudio_ParamError, "undersized buffer accepted");
  check(pcm[0] == 0 && pcm[1] == 9 && fifo.queued() == 2, "invalid callback overwrote beyond buffer/consumed PCM");
  buffers = buffer_list(pcm.data(), 8);
  buffers.mBuffers[0].mNumberChannels = 1;
  check(mac::render(fifo, &flags, 8, &buffers) == kAudio_ParamError, "mono format accepted");
  check(mac::render(fifo, &flags, 8, nullptr) == kAudio_ParamError, "null list accepted");
  buffers = buffer_list(nullptr, 8);
  check(mac::render(fifo, &flags, 8, &buffers) == kAudio_ParamError, "null PCM accepted");
  buffers = buffer_list(nullptr, 0);
  check(mac::render(fifo, &flags, 0, &buffers) == noErr && fifo.queued() == 2, "zero-frame callback");

  struct TwoBuffers { UInt32 count; AudioBuffer buffer[2]; } planar{};
  std::array<float, 8> left, right;
  left.fill(9); right.fill(9);
  planar.count = 2;
  planar.buffer[0] = AudioBuffer{1, UInt32(sizeof(left)), left.data()};
  planar.buffer[1] = AudioBuffer{1, UInt32(sizeof(right)), right.data()};
  check(mac::render(fifo, &flags, 8, reinterpret_cast<AudioBufferList*>(&planar)) == kAudio_ParamError,
        "planar PCM accepted");
  check(std::all_of(left.begin(), left.end(), [](float n) { return n == 0; }) &&
        std::all_of(right.begin(), right.end(), [](float n) { return n == 0; }) && fifo.queued() == 2,
        "invalid planar callback not silenced safely");

  check(!out_available() && out_queued_frames() == 0, "device opened by static initialization");
  out_write(source, 2);
  check(!out_available() && out_queued_frames() == 0, "uninitialized output wrote samples");
}

void full_drop_and_wrap() {
  rt::audio::StereoFifo<7> fifo;
  const std::array<float, 14> first{1,-1,2,-2,3,-3,4,-4,5,-5,6,-6,7,-7};
  check(fifo.write(first.data(), 7) && fifo.queued() == 7, "full FIFO capacity");
  check(!fifo.write(first.data(), 1) && fifo.queued() == 7, "full FIFO did not drop full block");
  std::array<float, 10> out{};
  check(fifo.read(out.data(), 5) == 5 && std::equal(out.begin(), out.end(), first.begin()), "first FIFO samples");
  check(!fifo.write(first.data(), 6) && fifo.queued() == 2, "overflow partially enqueued");
  check(fifo.write(first.data(), 5) && fifo.queued() == 7, "wrapped FIFO write");
  std::array<float, 18> final;
  final.fill(9);
  check(fifo.read(final.data(), 9) == 7 && fifo.queued() == 0, "wrapped read/underflow count");
  check(std::equal(first.begin() + 10, first.end(), final.begin()) &&
        std::equal(first.begin(), first.begin() + 10, final.begin() + 4), "wrapped samples/stereo channels");
  check(std::all_of(final.begin() + 14, final.end(), [](float n) { return n == 0; }), "wrapped underflow tail");
  check(!fifo.write(nullptr, 1) && !fifo.write(first.data(), 8) && fifo.write(nullptr, 0), "FIFO input guards");
}

void concurrent_callback() {
  constexpr std::uint64_t count = 1000000;
  rt::audio::mac::OutputFifo fifo;
  std::atomic<bool> bad{false};
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> read_count{0};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::thread writer([&] {
    std::array<float, 1022> pcm{};
    for (std::uint64_t at = 0; at < count && !bad.load(); ) {
      const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>((at * 17) % 511 + 1, count - at));
      for (std::size_t i = 0; i < frames; ++i) {
        pcm[i * 2] = float(at + i + 1);
        pcm[i * 2 + 1] = -pcm[i * 2];
      }
      if (fifo.write(pcm.data(), frames)) at += frames;
      else std::this_thread::yield();
      if (fifo.queued() > fifo.capacity() || std::chrono::steady_clock::now() > deadline) bad.store(true);
    }
    done.store(true, std::memory_order_release);
  });
  std::thread reader([&] {
    std::array<float, 8192> pcm{};
    std::uint64_t at = 0, callbacks = 0;
    while (at < count && !bad.load()) {
      const auto frames = UInt32((callbacks++ * 37) % 4096 + 1);
      auto buffers = buffer_list(pcm.data(), frames);
      AudioUnitRenderActionFlags flags = 0;
      if (rt::audio::mac::render(fifo, &flags, frames, &buffers) != noErr) { bad.store(true); break; }
      bool zero = false;
      for (UInt32 i = 0; i < frames; ++i) {
        if (pcm[i * 2] == 0) zero = true;
        if (zero) {
          if (pcm[i * 2] != 0 || pcm[i * 2 + 1] != 0) bad.store(true);
        } else {
          if (pcm[i * 2] != float(at + 1) || pcm[i * 2 + 1] != -float(at + 1)) bad.store(true);
          ++at;
        }
      }
      if (fifo.queued() > fifo.capacity() || std::chrono::steady_clock::now() > deadline) bad.store(true);
      if (done.load(std::memory_order_acquire) && fifo.queued() == 0 && at < count) bad.store(true);
      std::this_thread::yield();
    }
    read_count.store(at);
  });
  writer.join();
  reader.join();
  check(!bad.load() && read_count.load() == count && fifo.queued() == 0,
        "concurrent callback samples lost/reordered/replayed or test timed out");
}
}  // namespace

int main() {
  try {
    format_and_callback();
    full_drop_and_wrap();
    concurrent_callback();
    std::cout << "PASS offline audio: native float32 48kHz interleaved stereo, full-block drop, wrap, "
                 "underflow zeroing, invalid callback buffers, 1000000 concurrent ordered frames; no device opened\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
