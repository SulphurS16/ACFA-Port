// A bounded, allocation-free SPSC PCM FIFO. The cellAudio mixer is the one producer;
// the host's render callback is the one consumer. Neither side waits on the other.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rt::audio {

template <std::size_t CapacityFrames>
class StereoFifo {
 public:
  static_assert(CapacityFrames > 0);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "The real-time audio FIFO requires lock-free counters");

  // Like WASAPI out_write: either accept the complete block or leave the queue
  // unchanged. A full device must not stall the guest audio/event clock.
  bool write(const float* stereo, std::size_t frames) noexcept {
    if (frames == 0) return true;
    if (!stereo || frames > CapacityFrames) return false;
    const auto write = write_.load(std::memory_order_relaxed);
    const auto read = read_.load(std::memory_order_acquire);
    if (frames > CapacityFrames - (write - read)) return false;
    const std::size_t at = write % CapacityFrames;
    const std::size_t first = std::min(frames, CapacityFrames - at);
    std::memcpy(pcm_.data() + at * 2, stereo, first * 2 * sizeof(float));
    if (frames > first) std::memcpy(pcm_.data(), stereo + first * 2, (frames - first) * 2 * sizeof(float));
    write_.store(write + frames, std::memory_order_release);
    return true;
  }

  // Always initializes every requested output sample. Underflow plays silence,
  // rather than the stale PCM from a previous trip through the ring.
  std::size_t read(float* stereo, std::size_t frames) noexcept {
    if (!stereo || frames == 0) return 0;
    const auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    const std::size_t available = std::min<std::uint64_t>(frames, write - read);
    const std::size_t at = read % CapacityFrames;
    const std::size_t first = std::min(available, CapacityFrames - at);
    std::memcpy(stereo, pcm_.data() + at * 2, first * 2 * sizeof(float));
    if (available > first) std::memcpy(stereo + first * 2, pcm_.data(), (available - first) * 2 * sizeof(float));
    std::fill_n(stereo + available * 2, (frames - available) * 2, 0.0f);
    read_.store(read + available, std::memory_order_release);
    return available;
  }

  std::size_t queued() const noexcept {
    // An observer can see an old read position and a newer write position. Clamp
    // that harmless conservative snapshot; the producer/consumer use own counters.
    const auto read = read_.load(std::memory_order_acquire);
    const auto write = write_.load(std::memory_order_acquire);
    return std::min<std::uint64_t>(write - read, CapacityFrames);
  }
  static constexpr std::size_t capacity() noexcept { return CapacityFrames; }

 private:
  std::array<float, CapacityFrames * 2> pcm_{};
  alignas(64) std::atomic<std::uint64_t> write_{0};
  alignas(64) std::atomic<std::uint64_t> read_{0};
};

}  // namespace rt::audio
