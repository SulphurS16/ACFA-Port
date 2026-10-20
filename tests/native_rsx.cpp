// End-to-end synthetic RSX -> renderer -> Recorder thread -> native Metal -> CPU pixels.
// Only environment services are fixtures. No ELF, firmware, game assets, window, input or audio.
#include "platform/macos/metal_compat.h"
#include "platform/macos/metal_surface.h"
#include "platform/macos/guest_memory.h"
#include "runtime/config.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/submit.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_thread.h"
#include "runtime/rsx/methods.h"
#include "runtime/rsx/rsx_internal.h"
#include "runtime/settings.h"
#include "runtime/ui/overlay.h"
#include "ppu/ppu_recomp.h"
#include "rsx_shader_fixture.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
constexpr u32 side=64,local_bytes=0x100000,frame_offset=0x20000,fp_offset=0x1000;
std::atomic<unsigned> environment_errors{0},cache_recovery_warnings{0},write_fallback_warnings{0};
const auto started=std::chrono::steady_clock::now();
void check(bool condition,const std::string& why){if(!condition)throw std::runtime_error(why);}
}
namespace rt {
Config& cfg(){static Config config;return config;}
PpuThread* cur_thread(){return nullptr;}
std::string guest_backtrace(const ppu::Ctx&){return "<synthetic native RSX test>";}
double log_time(){return std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();}
Level log_level(){return Level::Warn;}
void log_write(Level level,const char* channel,const std::string& text){
 if(text.find("Metal shader cache failed integrity check")!=std::string::npos)++cache_recovery_warnings;
 else if(text=="guest write tracking unavailable: using bounded exact-content caches with hash/ring fallback")++write_fallback_warnings;
 else if(level>=Level::Warn){++environment_errors;std::cerr<<channel<<": "<<text<<'\n';}
}
[[noreturn]] void fatal(const std::string& why){throw std::runtime_error(why);}
}
namespace rt::settings {acfa::settings::Display display(){return {};}}
namespace rt::ui {
bool screen_up(){return false;}
void render(ID3D11Device*,ID3D11DeviceContext*,ID3D11Texture2D*,u32,u32){throw std::runtime_error("test unexpectedly requested a UI overlay");}
}
namespace rt::gpu {
// renderer_thread_init polls this handle even for a native device. The adapter ignores it.
// A sentinel avoids a five-second window wait; there is no OS window behind this pointer.
void* window_handle(){return reinterpret_cast<void*>(uintptr_t(1));}
void window_client_size(int* width,int* height){if(width)*width=0;if(height)*height=0;}
void window_set_fullscreen(bool){throw std::runtime_error("test unexpectedly requested fullscreen");}
namespace mac {
void* metal_layer(){return nullptr;}
void drawable_size(int* width,int* height){if(width)*width=0;if(height)*height=0;}
double backing_scale(){return 1;}
std::uint32_t display_id(){return 0;}
}
}
namespace rt::rsx {
State g;
std::atomic<u64> g_fifo_idle_us{0};
// Deliberately bounded synthetic local-memory mapping, with the real guest VM underneath.
u32 offset_to_ea(u32 offset,u32 location){
 if(location!=0||offset>=local_bytes)return 0;
 const u32 ea=kLocalBase+offset;return vm::committed(ea)?ea:0;
}
}
namespace {
namespace m=rt::rsx::m;
void method(u32 address,u32 value,u32 count=1,u32 index=0){
 check(address<0x10000&&address%4==0,"invalid synthetic method register");
 rt::rsx::g.regs[address>>2]=value;
 rt::rsx::renderer_method(address>>2,value,count,index);
}
void real_float(u32 address,float value){u32 bits;std::memcpy(&bits,&value,4);method(address,bits);}
void guest_float(u32 offset,float value){u32 bits;std::memcpy(&bits,&value,4);ppu::w32(rt::rsx::kLocalBase+offset,bits);}
void full_scissor(){method(m::SET_SCISSOR_HORIZONTAL,side<<16);method(m::SET_SCISSOR_VERTICAL,side<<16);}
void configure_surface(){
 method(m::SET_CONTEXT_DMA_COLOR_A,0);method(m::SET_CONTEXT_DMA_ZETA,0);
 method(m::SET_SURFACE_CLIP_HORIZONTAL,side<<16);method(m::SET_SURFACE_CLIP_VERTICAL,side<<16);
 // bind_targets reads color format [4:0], depth format [7:5]; format 1 is D16.
 method(m::SET_SURFACE_FORMAT,8u|(1u<<5));method(m::SET_SURFACE_COLOR_TARGET,1);
 method(m::SET_SURFACE_COLOR_AOFFSET,frame_offset);method(m::SET_SURFACE_PITCH_A,side*4);
 method(m::SET_SURFACE_ZETA_OFFSET,0x30000);method(m::SET_SURFACE_PITCH_Z,side*2);
 full_scissor();method(m::SET_VIEWPORT_HORIZONTAL,side<<16);method(m::SET_VIEWPORT_VERTICAL,side<<16);
 real_float(m::SET_VIEWPORT_SCALE,32);real_float(m::SET_VIEWPORT_SCALE+4,-32);real_float(m::SET_VIEWPORT_SCALE+8,1);
 real_float(m::SET_VIEWPORT_OFFSET,32);real_float(m::SET_VIEWPORT_OFFSET+4,32);real_float(m::SET_VIEWPORT_OFFSET+8,0);
 real_float(m::SET_CLIP_MIN,0);real_float(m::SET_CLIP_MAX,1);
 method(m::SET_ZMIN_MAX_CONTROL,1);method(m::SET_COLOR_MASK,0x01010101);method(m::SET_DEPTH_TEST_ENABLE,0);
 method(m::SET_STENCIL_TEST_ENABLE,0);method(m::SET_CULL_FACE_ENABLE,0);method(m::SET_BLEND_ENABLE,0);
}
std::vector<u8> present(u32 offset=frame_offset,u32 pitch=side*4){
 rt::rsx::present_buffer(0,{offset,pitch,side,side});
 rt::gpu::rec().finish();
 std::vector<u8> pixels;u32 width=0,height=0;
 check(rt::gpu::read_back_last_frame(pixels,width,height),"real device last-frame readback failed");
 check(width==side&&height==side&&pixels.size()==size_t(side)*side*4,"last-frame dimensions differ");return pixels;
}
template<class Expected>void verify(const char* label,const std::vector<u8>& pixels,Expected expected,int tolerance=0){
 for(u32 y=0;y<side;++y)for(u32 x=0;x<side;++x){auto color=expected(x,y);for(u32 c=0;c<4;++c){int got=pixels[(size_t(y)*side+x)*4+c];if(std::abs(got-int(color[c]))>tolerance)throw std::runtime_error(std::string(label)+" pixel ("+std::to_string(x)+","+std::to_string(y)+") channel "+std::to_string(c)+" got "+std::to_string(got)+" expected "+std::to_string(color[c]));}}
}
void test_clears(){
 method(m::SET_COLOR_CLEAR_VALUE,0xff0c2238);method(m::SET_ZSTENCIL_CLEAR_VALUE,0xffff);
 method(m::CLEAR_SURFACE,0xf3); // Full color plus depth; D16 has no stencil aspect.
 verify("full clear",present(),[](u32,u32){return std::array<u8,4>{12,34,56,255};});
 method(m::SET_SCISSOR_HORIZONTAL,(40u<<16)|8);method(m::SET_SCISSOR_VERTICAL,(24u<<16)|12);
 method(m::SET_COLOR_CLEAR_VALUE,0xff46505a);method(m::CLEAR_SURFACE,0xf0);
 verify("scissored clear",present(),[](u32 x,u32 y){return x>=8&&x<48&&y>=12&&y<36?std::array<u8,4>{70,80,90,255}:std::array<u8,4>{12,34,56,255};});full_scissor();
}
void install_programs(){
 const auto vp=acfa::test::vertex_program();method(m::SET_TRANSFORM_PROGRAM_LOAD,0);
 // Actual renderer program bursts advance their load cursor only at the final word.
 for(u32 i=0;i<8;++i)method(m::SET_TRANSFORM_PROGRAM+4*i,vp[i],8,i);
 method(m::SET_TRANSFORM_PROGRAM_START,0);
 const auto fp=acfa::test::fragment_program();std::memcpy(ppu::mem()+rt::rsx::kLocalBase+fp_offset,fp.data(),fp.size());
 method(m::SET_SHADER_PROGRAM,fp_offset);method(m::SET_SHADER_CONTROL,1u<<6); // fixture writes FP32 R0, not H0
 const std::array<std::array<float,4>,3> positions={{{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}}};
 for(u32 v=0;v<3;++v)for(u32 c=0;c<4;++c)guest_float(0x4000+v*16+c*4,positions[v][c]);
 for(u32 v=0;v<3;++v)for(u32 c=0;c<4;++c)guest_float(0x5000+v*16+c*4,std::array<float,4>{.2f,.6f,.9f,1}[c]);
 const u32 float4=(1u<<16)|(16u<<8)|(4u<<4)|2u;
 method(m::SET_VERTEX_DATA_ARRAY_OFFSET,0x4000);method(m::SET_VERTEX_DATA_ARRAY_FORMAT,float4);
 method(m::SET_VERTEX_DATA_ARRAY_OFFSET+3*4,0x5000);method(m::SET_VERTEX_DATA_ARRAY_FORMAT+3*4,float4);
}
void test_draw(){
 install_programs();const auto before=rt::rsx::g.draws.load();
 method(m::SET_BEGIN_END,5);method(m::DRAW_ARRAYS,2u<<24);method(m::SET_BEGIN_END,0);
 verify("RSX triangle",present(),[](u32,u32){return std::array<u8,4>{51,153,230,255};},1);
 check(rt::rsx::g.draws.load()==before+1,"real renderer did not execute synthetic draw");
}
void test_color_guest_readback(){
 method(m::SET_COLOR_CLEAR_VALUE,0x7f123456);method(m::CLEAR_SURFACE,0xf0);
 const u32 base=rt::rsx::kLocalBase+frame_offset;
 std::memset(ppu::mem()+base,0xa9,side*side*4);
 check(rt::rsx::renderer_memory_read(base,side*side*4),"dirty rendered source must read back");
 for(u32 i=0;i<side*side;++i){const u8* p=ppu::mem()+base+i*4;check(p[0]==0x7f&&p[1]==0x12&&p[2]==0x34&&p[3]==0x56,"readback guest ARGB byte order/pitch");}
 check(!rt::rsx::renderer_memory_read(base,side*side*4),"unchanged source must not wait/read twice");
 method(m::SET_COLOR_CLEAR_VALUE,0xffabcdef);method(m::CLEAR_SURFACE,0xf0);
 check(rt::rsx::renderer_memory_read(base+12,4),"subrange read must see a newer GPU draw");
 check(ppu::mem()[base+13]==0xab&&ppu::mem()[base+14]==0xcd&&ppu::mem()[base+15]==0xef,"new rendered contents replace old guest bytes");
}
void test_textured_draw(){
 // The synthetic texture program samples tc0; its default (0,0) is sufficient because
 // all four texels intentionally carry the same color. Texture decode/upload/sampler are real.
 const auto fp=acfa::test::fragment_program(true);std::memcpy(ppu::mem()+rt::rsx::kLocalBase+fp_offset,fp.data(),fp.size());
 rt::rsx::renderer_guest_sync();rt::rsx::renderer_memory_written(rt::rsx::kLocalBase+fp_offset,u32(fp.size()));
 constexpr u32 tex_offset=0x10000,tex0=0x1a00;
 for(u32 i=0;i<4;++i){auto p=ppu::mem()+rt::rsx::kLocalBase+tex_offset+i*4;p[0]=255;p[1]=129;p[2]=64;p[3]=200;}
 method(tex0,tex_offset);
 // decode_texture: one mip, linear A8R8G8B8, 2D, border-color storage (no border texels), local.
 method(tex0+4,(1u<<16)|(0xa5u<<8)|(2u<<4)|(1u<<3)|1u);
 method(tex0+8,3u|(3u<<8)|(3u<<16));method(tex0+12,1u<<31);
 method(tex0+16,0xaae4);method(tex0+20,(1u<<16)|(1u<<24));
 method(tex0+24,(2u<<16)|2u);method(0x1840,8);
 method(m::SET_BEGIN_END,5);method(m::DRAW_ARRAYS,2u<<24);method(m::SET_BEGIN_END,0);
 verify("RSX textured triangle",present(),[](u32,u32){return std::array<u8,4>{129,64,200,255};},1);
 // A second frame must reuse the texture; the next CPU rewrite must invalidate it.
 method(m::SET_BEGIN_END,5);method(m::DRAW_ARRAYS,2u<<24);method(m::SET_BEGIN_END,0);
 verify("RSX unchanged texture reuse",present(),[](u32,u32){return std::array<u8,4>{129,64,200,255};},1);
 for(u32 i=0;i<4;++i){auto p=ppu::mem()+rt::rsx::kLocalBase+tex_offset+i*4;p[1]=7;p[2]=201;p[3]=55;}
 method(m::SET_BEGIN_END,5);method(m::DRAW_ARRAYS,2u<<24);method(m::SET_BEGIN_END,0);
 verify("RSX CPU rewritten texture invalidates exact snapshot",present(),[](u32,u32){return std::array<u8,4>{7,201,55,255};},1);

}
void test_depth_as_color(){
 // Create an actual D24S8 RSX surface, clear packed Z/S, then alias it as A8R8G8B8.
 // texture_view invokes renderer.cpp's own depth-to-color fullscreen pass and saved state.
 method(m::SET_SURFACE_FORMAT,8u|(2u<<5));method(m::SET_SURFACE_PITCH_Z,side*4);
 method(m::SET_ZSTENCIL_CLEAR_VALUE,0x1234565d);method(m::CLEAR_SURFACE,3);
 constexpr u32 tex0=0x1a00;
 method(tex0,0x30000);method(tex0+4,(1u<<16)|(0xa5u<<8)|(2u<<4)|(1u<<3)|1u);
 method(tex0+24,(side<<16)|side);method(0x1840,side*4);
 method(m::SET_BEGIN_END,5);method(m::DRAW_ARRAYS,2u<<24);method(m::SET_BEGIN_END,0);
 verify("RSX depth-as-color",present(),[](u32,u32){return std::array<u8,4>{0x34,0x56,0x5d,255};},1);
}
void test_memory_flip(){
 constexpr u32 offset=0x50000,pitch=side*4+16;
 for(u32 y=0;y<side;++y)for(u32 x=0;x<side;++x){u8* p=ppu::mem()+rt::rsx::kLocalBase+offset+y*pitch+x*4;p[0]=17;p[1]=u8(x*3);p[2]=u8(y*3);p[3]=u8((x+y)*2);}
 verify("guest-memory flip",present(offset,pitch),[](u32 x,u32 y){return std::array<u8,4>{u8(x*3),u8(y*3),u8((x+y)*2),255};},1);
}
void test_shader_cache(const std::filesystem::path& cache){
 const std::string source="float4 main(uint i:SV_VertexID):SV_Position { return float4(i,0,0,1); }";
 u32 compiled0=0,cached0=0;rt::gpu::shader_stats(&compiled0,&cached0,nullptr);
 ID3DBlob* first=rt::gpu::compile_hlsl(source,"main","vs_5_0");check(first!=nullptr,"cache first compile failed");
 std::string expected(static_cast<const char*>(first->GetBufferPointer()),first->GetBufferSize());first->Release();
 u32 compiled1=0,cached1=0;rt::gpu::shader_stats(&compiled1,&cached1,nullptr);check(compiled1==compiled0+1&&cached1==cached0,"first cache compile counters");
 ID3DBlob* second=rt::gpu::compile_hlsl(source,"main","vs_5_0");check(second!=nullptr,"cache hit failed");check(second->GetBufferSize()==expected.size()&&!std::memcmp(second->GetBufferPointer(),expected.data(),expected.size()),"cache hit altered MSL");second->Release();
 u32 compiled2=0,cached2=0;rt::gpu::shader_stats(&compiled2,&cached2,nullptr);check(compiled2==compiled1&&cached2==cached1+1,"cache did not avoid compilation");
 std::vector<std::filesystem::path> files;for(const auto& f:std::filesystem::directory_iterator(cache))if(f.is_regular_file())files.push_back(f.path());check(files.size()==1,"synthetic cache must contain exactly one file");
 {std::fstream broken(files[0],std::ios::in|std::ios::out|std::ios::binary);check(bool(broken),"cannot open synthetic cache to corrupt");broken.seekp(-1,std::ios::end);broken.put('!');}
 ID3DBlob* recovered=rt::gpu::compile_hlsl(source,"main","vs_5_0");check(recovered!=nullptr,"damaged cache was not recovered");check(recovered->GetBufferSize()==expected.size()&&!std::memcmp(recovered->GetBufferPointer(),expected.data(),expected.size()),"damaged cache recovery changed MSL");recovered->Release();
 u32 compiled3=0,cached3=0;rt::gpu::shader_stats(&compiled3,&cached3,nullptr);check(compiled3==compiled2+1&&cached3==cached2&&cache_recovery_warnings==1,"damaged cache must recompile and report integrity failure");
}
}
int main(){
 alarm(45);
 try{
  char cache_name[]="/private/tmp/acfa-native-rsx-XXXXXX";char* cache=mkdtemp(cache_name);check(cache!=nullptr,"synthetic cache directory creation");
  rt::cfg().shader_cache=cache;rt::cfg().render_scale=100;rt::cfg().fps=-1;
  test_shader_cache(cache);
  check(rt::vm::init(),"real guest VM reservation failed");rt::vm::commit(rt::rsx::kLocalBase,local_bytes);rt::vm::commit(rt::rsx::kControlAddr,0x1000);
  rt::rsx::renderer_thread_init();check(rt::gpu::device().dev&&rt::gpu::device().ctx,"real native renderer device initialization failed");check(rt::gpu::rec().async(),"test must cover the real asynchronous Recorder");
  configure_surface();test_clears();test_draw();test_color_guest_readback();test_textured_draw();test_depth_as_color();test_memory_flip();rt::gpu::rec().finish();acfa::metal::flush_context(rt::gpu::device().ctx,true);
  check(write_fallback_warnings==1,"must exercise native no-write-tracking content validation");
  check(environment_errors==0,"unexpected renderer warnings/errors occurred");std::filesystem::remove_all(cache);
  std::cout<<"native-rsx: PASS (actual renderer + asynchronous Recorder + native device; six 64x64 full-pixel scenes; shader cache hit and corruption recovery; no window/game/audio/input)\n"<<std::flush;
  // Recorder's production submission thread is intentionally process-long. All work is finished;
  // avoid running global destructors concurrently with that idle detached thread in this test.
  std::_Exit(0);
 }catch(const std::exception& e){std::cerr<<"native-rsx: FAIL: "<<e.what()<<'\n'<<std::flush;std::_Exit(1);}
}
