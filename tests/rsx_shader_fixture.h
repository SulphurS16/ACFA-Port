#pragma once
#include "runtime/gpu/vp_hlsl.h"
#include "runtime/gpu/fp_hlsl.h"
#include <array>
#include <stdexcept>
namespace acfa::test {
// Synthetic NV40 instructions, not copied from the game. Two VP MOVs write
// position/color; the FP MOV exports interpolated front diffuse color.
inline std::array<u32,rt::gpu::kVpSlots*4> vertex_program(bool constants=false) {
  std::array<u32,rt::gpu::kVpSlots*4> words{};
  auto mov=[&](unsigned pc,unsigned input,unsigned output,bool end) {
    u32 s=2u|(1u<<12)|(2u<<10)|(3u<<8); // input, identity xyzw
    const bool multiply=constants && output==1;
    u32 source1=multiply ? ((s&~3u)|3u) : s; // vc[0] for the constant-buffer case
    words[pc*4]=(7u<<10)|(63u<<15)|(1u<<30);
    words[pc*4+1]=((multiply?2u:1u)<<22)|(input<<8)|((s>>9)&255u);
    words[pc*4+2]=((s&511u)<<23)|(source1<<6)|((s>>11)&63u);
    words[pc*4+3]=(output<<2)|(63u<<7)|(15u<<13)|((s&2047u)<<21)|(end?1u:0u);
  };
  mov(0,0,0,false);mov(1,3,1,true);return words;
}
inline std::array<u8,16> fragment_program(bool texture=false) {
  std::array<u8,16> bytes{};
  u32 swz=(1u<<11)|(2u<<13)|(3u<<15);
  u32 w[4]={(u32(texture?0x17:1)<<24)|1u|(15u<<9)|((texture?4u:1u)<<13),
            swz|1u|(7u<<18),swz,swz};
  for(unsigned i=0;i<4;++i){bytes[i*4]=u8(w[i]>>8);bytes[i*4+1]=u8(w[i]);bytes[i*4+2]=u8(w[i]>>24);bytes[i*4+3]=u8(w[i]>>16);}
  return bytes;
}
inline std::array<u8,32> fragment_constant_program() {
  std::array<u8,32> bytes{};
  u32 swz=(1u<<11)|(2u<<13)|(3u<<15);
  const u32 words[8]={(2u<<24)|1u|(15u<<9)|(1u<<13),swz|1u|(7u<<18),swz|2u,swz,
                       0x3f000000u,0x3f800000u,0x3f800000u,0x3f800000u};
  for(unsigned i=0;i<8;++i){bytes[i*4]=u8(words[i]>>8);bytes[i*4+1]=u8(words[i]);bytes[i*4+2]=u8(words[i]>>24);bytes[i*4+3]=u8(words[i]>>16);}
  return bytes;
}

}
