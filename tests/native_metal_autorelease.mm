// Real draws on an ordinary pthread without an enclosing autorelease pool.
// Reusing shaders/state exercises the pipeline-cache hit on every subsequent
// draw, as well as render-pass, command-buffer and encoder lifetime.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat.h"
#include <mach/mach.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr unsigned side=64,warm_frames=128,measured_frames=2048,draws_per_frame=8;
constexpr const char* shader=R"(#include <metal_stdlib>
using namespace metal;
struct V {float4 position [[position]];};
vertex V acfa_vs(uint i [[vertex_id]]) {
  float2 p=float2((i<<1)&2,i&2);V v;v.position=float4(p*2.0-1.0,0,1);return v;
}
fragment float4 acfa_ps() {return float4(.25,.5,.75,1);}
)";
template<class T> struct Owned {
  T* p=nullptr;~Owned(){if(p)p->Release();}
  T** put(){return &p;}operator T*()const{return p;}T* operator->()const{return p;}
};
uint64_t resident_bytes() {
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t n=MACH_TASK_BASIC_INFO_COUNT;
  if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&info),&n)!=KERN_SUCCESS)
    throw std::runtime_error("task_info failed");
  return info.resident_size;
}
void require(bool yes,const char* what){if(!yes)throw std::runtime_error(what);}
}
int main() {
  alarm(15);
  try {
    Owned<ID3D11Device> device;Owned<ID3D11DeviceContext> context;
    Owned<ID3D11Texture2D> texture;Owned<ID3D11RenderTargetView> target;
    Owned<ID3D11VertexShader> vs;Owned<ID3D11PixelShader> ps;
    Owned<ID3D11RasterizerState> raster;
    require(SUCCEEDED(acfa::metal::create_device(device.put(),context.put())),"Metal device creation failed");
    @autoreleasepool {
      D3D11_TEXTURE2D_DESC d{};
      d.Width=d.Height=side;d.MipLevels=d.ArraySize=1;
      d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
      d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
      require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,texture.put())),"texture creation failed");
      require(SUCCEEDED(device->CreateRenderTargetView(texture,nullptr,target.put())),"render target creation failed");
      require(SUCCEEDED(device->CreateVertexShader(shader,std::strlen(shader),nullptr,vs.put())),"vertex shader creation failed");
      require(SUCCEEDED(device->CreatePixelShader(shader,std::strlen(shader),nullptr,ps.put())),"pixel shader creation failed");
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
      require(SUCCEEDED(device->CreateRasterizerState(&rd,raster.put())),"raster state creation failed");
      ID3D11RenderTargetView* rt=target;
      context->OMSetRenderTargets(1,&rt,nullptr);
      D3D11_VIEWPORT vp{0,0,side,side,0,1};context->RSSetViewports(1,&vp);
      context->RSSetState(raster);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);
    }
    std::exception_ptr failure;
    uint64_t before=0,peak=0,after=0;unsigned completed=0;
    std::thread worker([&] {
      // Deliberately no outer @autoreleasepool: API boundaries must drain.
      try {
        auto frame=[&]{
          const FLOAT black[]={0,0,0,1};context->ClearRenderTargetView(target,black);
          for(unsigned j=0;j<draws_per_frame;++j)context->Draw(3,0);
          acfa::metal::flush_context(context,true);
        };
        for(unsigned i=0;i<warm_frames;++i)frame();
        before=peak=resident_bytes();
        for(unsigned i=0;i<measured_frames;++i){
          frame();completed=i+1;
          if((i&63)==63){
            after=resident_bytes();peak=std::max(peak,after);
            require(peak<=before+128ull*1024*1024,"early stop: worker RSS growth exceeded 128 MiB");
          }
        }
        after=resident_bytes();
      }catch(...){failure=std::current_exception();}
    });
    worker.join();
    std::printf("Metal cached-draw stress: %u/%u frames x %u draws, warm RSS %.2f MiB, peak %.2f MiB, end %.2f MiB, growth %.2f MiB\n",
      completed,measured_frames,draws_per_frame,before/1048576.0,peak/1048576.0,after/1048576.0,(peak-before)/1048576.0);
    if(failure)std::rethrow_exception(failure);
    std::vector<uint8_t> pixels;uint32_t w=0,h=0;
    require(acfa::metal::readback_texture(context,texture,pixels,w,h),"draw readback failed");
    require(w==side&&h==side&&pixels.size()==side*side*4,"readback dimensions mismatch");
    constexpr int wanted[]={64,128,191,255};
    for(size_t i=0;i<pixels.size();++i)require(std::abs(int(pixels[i])-wanted[i%4])<=1,"cached-draw pixel mismatch");
    // Warmup separates initial shader/driver allocations from ongoing work.
    require(peak<=before+48ull*1024*1024,"worker RSS grew more than 48 MiB after warmup");
    std::puts("native-metal-autorelease: PASS (all pixels checked; no window, audio or game input)");
    return 0;
  }catch(const std::exception& e){
    std::fprintf(stderr,"native-metal-autorelease: FAIL: %s\n",e.what());return 1;
  }
}
