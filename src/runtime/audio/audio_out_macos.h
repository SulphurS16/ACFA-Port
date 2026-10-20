// The same PCM format and buffer-filling routine are used by the real AudioUnit
// callback and the offline tests. Including this header never opens an audio device.
#pragma once
#include <AudioUnit/AudioUnit.h>

#include "runtime/audio/audio_fifo.h"

namespace rt::audio::mac {

inline constexpr unsigned kSampleRate = 48000;
inline constexpr unsigned kChannels = 2;
inline constexpr unsigned kBytesPerFrame = kChannels * sizeof(float);
inline constexpr unsigned kQueueFrames = kSampleRate / 10;  // 100 ms bounded capacity
using OutputFifo = StereoFifo<kQueueFrames>;

inline AudioStreamBasicDescription stream_format() noexcept {
  AudioStreamBasicDescription format{};
  format.mSampleRate = kSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
  format.mBytesPerPacket = kBytesPerFrame;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = kBytesPerFrame;
  format.mChannelsPerFrame = kChannels;
  format.mBitsPerChannel = sizeof(float) * 8;
  return format;
}

inline OSStatus render(OutputFifo& fifo, AudioUnitRenderActionFlags* flags,
                       UInt32 frames, AudioBufferList* buffers) noexcept {
  if (!buffers) return kAudio_ParamError;
  // Our accepted format is one buffer of interleaved L/R float32. Refuse a
  // mismatched host layout safely; never reinterpret planar/short buffers.
  const std::uint64_t bytes = std::uint64_t(frames) * kBytesPerFrame;
  if (buffers->mNumberBuffers != 1 || buffers->mBuffers[0].mNumberChannels != kChannels ||
      bytes > buffers->mBuffers[0].mDataByteSize ||
      (bytes != 0 && !buffers->mBuffers[0].mData)) {
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
      auto& buffer = buffers->mBuffers[i];
      if (buffer.mData) std::memset(buffer.mData, 0, buffer.mDataByteSize);
    }
    if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return kAudio_ParamError;
  }
  const auto consumed = fifo.read(static_cast<float*>(buffers->mBuffers[0].mData), frames);
  buffers->mBuffers[0].mDataByteSize = static_cast<UInt32>(bytes);
  if (flags) {
    if (consumed == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    else *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
  }
  return noErr;
}

}  // namespace rt::audio::mac
