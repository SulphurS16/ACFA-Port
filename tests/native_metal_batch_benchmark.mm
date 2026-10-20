// Synthetic encoder-overhead benchmark: no game, window, audio or input.
// These completed-frame timings are not an estimate of actual gameplay FPS.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT width = 1280, height = 720, draws = 500, warm_frames = 2, sample_frames = 5;
template<class T> struct Ref {
  T* p = nullptr;
  ~Ref() { if (p) p->Release(); }
  T** put() { return &p; }
  T* operator->() const { return p; }
  operator T*() const { return p; }
};
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void ok(HRESULT result, ID3D11Device* device, const char* what) {
  if (FAILED(result)) throw std::runtime_error(std::string(what) + ": " + acfa::metal::last_error(device));
}
double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
constexpr char source[] = R"(#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; };
vertex V acfa_vs(uint id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
  V out; out.position=float4(positions[id],0,1); return out;
}
fragment float4 acfa_ps(V in [[stage_in]]) { return float4(0,1,0,.5); }
)";
}  // namespace

int main(int argc,char** argv) {
  const bool alternating=argc==2&&std::string(argv[1])=="--alternating";
  const bool every=argc==2&&std::string(argv[1])=="--constant-every-draw";
  const bool constants=every||(argc==2&&std::string(argv[1])=="--constant-updates");
  if(argc>2||(argc>1&&!alternating&&!constants))return 2;
  alarm(60);
  @autoreleasepool {
    try {
      Ref<ID3D11Device> device;
      Ref<ID3D11DeviceContext> context;
      ok(acfa::metal::create_device(device.put(), context.put()), device, "create device");
      Ref<ID3D11VertexShader> vertex;
      Ref<ID3D11PixelShader> fragment;
      std::string effective=source;
      if(constants){const std::string old="fragment float4 acfa_ps(V in [[stage_in]]) { return float4(0,1,0,.5); }";const auto pos=effective.find(old);check(pos!=std::string::npos,"constant shader fixture");effective.replace(pos,old.size(),"struct C {float4 color;float4 params;}; fragment float4 acfa_ps(V in [[stage_in]],constant C& c [[buffer(2)]]) { return c.color; }");}
      ok(device->CreateVertexShader(effective.data(), effective.size(), nullptr, vertex.put()), device, "vertex shader");
      ok(device->CreatePixelShader(effective.data(), effective.size(), nullptr, fragment.put()), device, "fragment shader");
      D3D11_TEXTURE2D_DESC desc{width,height,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,0,0};
      Ref<ID3D11Texture2D> target;
      Ref<ID3D11RenderTargetView> view;
      ok(device->CreateTexture2D(&desc, nullptr, target.put()), device, "render target");
      ok(device->CreateRenderTargetView(target, nullptr, view.put()), device, "render target view");
      D3D11_BLEND_DESC blend_desc{};
      auto& blend = blend_desc.RenderTarget[0];
      blend.BlendEnable = TRUE;
      blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
      blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      blend.BlendOp = D3D11_BLEND_OP_ADD;
      blend.SrcBlendAlpha = D3D11_BLEND_ONE;
      blend.DestBlendAlpha = D3D11_BLEND_ZERO;
      blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      Ref<ID3D11BlendState> blend_state,alternate_blend;
      ok(device->CreateBlendState(&blend_desc, blend_state.put()), device, "blend state");
      if(alternating)ok(device->CreateBlendState(&blend_desc,alternate_blend.put()),device,"alternating blend state");
      D3D11_RASTERIZER_DESC raster_desc{};
      raster_desc.FillMode = D3D11_FILL_SOLID;
      raster_desc.CullMode = D3D11_CULL_NONE;
      raster_desc.DepthClipEnable = TRUE;
      Ref<ID3D11RasterizerState> raster;
      ok(device->CreateRasterizerState(&raster_desc, raster.put()), device, "raster state");
      D3D11_DEPTH_STENCIL_DESC depth_desc{};
      Ref<ID3D11DepthStencilState> depth;
      ok(device->CreateDepthStencilState(&depth_desc, depth.put()), device, "depth disabled state");
      ID3D11RenderTargetView* rtv = view;
      context->OMSetRenderTargets(1, &rtv, nullptr);
      context->VSSetShader(vertex, nullptr, 0);
      context->PSSetShader(fragment, nullptr, 0);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->OMSetBlendState(blend_state, nullptr, ~0u);
      context->OMSetDepthStencilState(depth, 0);
      context->RSSetState(raster);
      const D3D11_VIEWPORT viewport{0,0,width,height,0,1};
      context->RSSetViewports(1, &viewport);
      Ref<ID3D11Buffer> cb;
      struct Constants {float color[4];float params[4];};
      Constants values{{0,1,0,.5},{0,0,0,0}};
      if(constants){D3D11_BUFFER_DESC bd{sizeof values,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA init{&values,0,0};ok(device->CreateBuffer(&bd,&init,cb.put()),device,"constant buffer");ID3D11Buffer* b=cb;context->PSSetConstantBuffers(2,1,&b);}
      const auto frame = [&] {
        const float black[4] = {0,0,0,0};
        context->ClearRenderTargetView(view, black);
        for (UINT draw = 0; draw < draws; ++draw) {
          if(alternating)context->OMSetBlendState(draw%2?blend_state:alternate_blend,nullptr,~0u);
          if(constants){values.params[0]=float((draw/(every?1:8))%2);context->UpdateSubresource(cb,0,nullptr,&values,0,0);}
          context->Draw(3,0);
        }
      };
      for (UINT i = 0; i < warm_frames; ++i) {
        frame();
        acfa::metal::flush_context(context, true);
      }
      (void)acfa::metal::performance_report();  // Drop shader/pipeline/warmup counters.
      std::array<double, sample_frames> completed{}, encoded{};
      const auto total_start = Clock::now();
      for (UINT i = 0; i < sample_frames; ++i) {
        const auto start = Clock::now();
        frame();
        encoded[i] = ms_since(start);
        acfa::metal::flush_context(context, true);
        completed[i] = ms_since(start);
      }
      const auto total_ms = ms_since(total_start);
      const auto performance = acfa::metal::performance_report();
      std::vector<uint8_t> pixels;
      uint32_t actual_width = 0, actual_height = 0;
      check(acfa::metal::readback_texture(context, target, pixels, actual_width, actual_height), "GPU readback failed");
      check(actual_width == width && actual_height == height && pixels.size() == size_t(width)*height*4,
            "GPU output dimensions wrong");
      for (size_t i = 0; i < pixels.size(); i += 4)
        check(pixels[i] == 0 && pixels[i+1] >= 254 && pixels[i+2] == 0 &&
                  pixels[i+3] >= 127 && pixels[i+3] <= 128, "GPU blended pixel mismatch");
      std::sort(completed.begin(), completed.end());
      std::sort(encoded.begin(), encoded.end());
      std::printf("{\"success\":true,\"synthetic_only\":true,\"width\":%u,\"height\":%u,"
                  "\"constant_updates\":%s,\"alternating_state\":%s,\"draws_per_frame\":%u,\"warmup_frames\":%u,\"sample_frames\":%u,"
                  "\"completed_median_ms\":%.4f,\"completed_min_ms\":%.4f,\"completed_max_ms\":%.4f,"
                  "\"encode_median_ms\":%.4f,\"completed_total_ms\":%.4f,\"verified_pixels\":%zu}\n",
                  width, height, constants?"true":"false", alternating?"true":"false", draws, warm_frames, sample_frames, completed[sample_frames/2],
                  completed.front(), completed.back(), encoded[sample_frames/2], total_ms, pixels.size()/4);
      std::fprintf(stderr, "%s\n", performance.c_str());
      alarm(0);
      return 0;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "native-metal-batch-benchmark: FAIL: %s\n", error.what());
      return 1;
    }
  }
}
