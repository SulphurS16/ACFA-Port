// Native Metal adapter for the existing RSX renderer's device/presentation API.
// The D3D-shaped objects here are a project-local C++ call surface, not Windows COM.
#include "runtime/gpu/d3d11_device.h"
#include "platform/macos/metal_compat.h"
#include "platform/macos/metal_shader.h"
#include "platform/macos/metal_surface.h"
#include "platform/macos/shader_prewarm.h"
#include "platform/macos/native_prewarm.h"
#include "platform/macos/metal_shader_cache.h"
#include "platform/macos/host_thread.h"
#include "common/crypto.h"
#include "common/file.h"
#include "runtime/config.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/window.h"
#include "runtime/ui/overlay.h"
#include "runtime/log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace rt::gpu {
namespace {
Device native_device;
std::mutex init_mutex,cache_mutex;
std::atomic<u64> compile_us{0};
std::atomic<u32> compiles{0},cache_hits{0};
ID3D11VertexShader* blit_vs=nullptr;
ID3D11PixelShader* blit_ps=nullptr;
ID3D11SamplerState* linear_sampler=nullptr;
ID3D11Texture2D* last_frame=nullptr;
ID3D11Texture2D* composed_frame=nullptr;
u32 last_w=0,last_h=0,composed_w=0,composed_h=0;
const char* blit_hlsl=R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
V vs_main(uint i:SV_VertexID){ V o; o.uv=float2((i<<1)&2,i&2); o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
float4 ps_main(V i):SV_Target { return float4(t0.Sample(s0,i.uv).rgb,1); }
)";
std::string hash_hex(const void* p,size_t n){
 auto h=acfa::crypto::sha256(p,n);static constexpr char digits[]="0123456789abcdef";
 std::string out;out.reserve(64);for(u8 v:h){out+=digits[v>>4];out+=digits[v&15];}return out;
}
std::vector<u8> blob_bytes(const std::string& s){return {s.begin(),s.end()};}
void require_ok(HRESULT hr,const char* where){
 if(FAILED(hr))throw std::runtime_error(std::string(where)+": "+acfa::metal::last_error(native_device.dev));
}
bool fit_texture(ID3D11Texture2D*& texture,u32& have_w,u32& have_h,u32 w,u32 h){
 if(texture && have_w==w && have_h==h)return true;
 if(texture){texture->Release();texture=nullptr;}
 D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
 desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
 desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 if(FAILED(native_device.dev->CreateTexture2D(&desc,nullptr,&texture)))return false;
 have_w=w;have_h=h;return true;
}
void blit_to(ID3D11ShaderResourceView* source,ID3D11Texture2D* target,u32 w,u32 h,const D3D11_VIEWPORT& viewport){
 ID3D11RenderTargetView* rtv=nullptr;
 require_ok(native_device.dev->CreateRenderTargetView(target,nullptr,&rtv),"presentation render target");
 auto* c=native_device.ctx;
 c->OMSetRenderTargets(1,&rtv,nullptr);rtv->Release();
 c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);c->RSSetState(nullptr);
 c->RSSetViewports(1,&viewport);D3D11_RECT rect{0,0,LONG(w),LONG(h)};c->RSSetScissorRects(1,&rect);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->VSSetShader(blit_vs,nullptr,0);c->PSSetShader(blit_ps,nullptr,0);
 c->PSSetShaderResources(0,1,&source);c->PSSetSamplers(0,1,&linear_sampler);c->Draw(3,0);
 ID3D11ShaderResourceView* none=nullptr;c->PSSetShaderResources(0,1,&none);
}
struct Capture { ID3D11Texture2D* texture;u32 index,w,h; };
std::vector<Capture> captures;
}
Device& device(){return native_device;}
ID3DBlob* compile_hlsl(const std::string& source,const char* entry,const char* target){
 if(!entry || !target)return nullptr;
 acfa::metal::ShaderStage stage;
 if(std::string(target).starts_with("vs_"))stage=acfa::metal::ShaderStage::Vertex;
 else if(std::string(target).starts_with("ps_"))stage=acfa::metal::ShaderStage::Fragment;
 else {LOG_E("gpu","unsupported Metal stage %s",target);return nullptr;}
 std::string identity=std::string(acfa::metal::shader_compiler_version())+'|'+target+'|'+entry+'|'+source;
 std::string key=hash_hex(identity.data(),identity.size());
 std::filesystem::path cache;
 if(!cfg().shader_cache.empty()){
  cache=std::filesystem::path(cfg().shader_cache)/(key+".mslcache");
  std::lock_guard lock(cache_mutex);
  std::error_code ec;
  if(std::filesystem::is_regular_file(cache,ec))try{
   auto bytes=acfa::read_file(cache);std::string contents(bytes.begin(),bytes.end());
   std::string prefix="// ACFA Metal cache v1 "+key+"\n// payload-sha256 ";
   if(contents.starts_with(prefix) && contents.size()>prefix.size()+65){
    auto expected=contents.substr(prefix.size(),64);auto payload=contents.substr(prefix.size()+65);
    if(contents[prefix.size()+64]=='\n' && hash_hex(payload.data(),payload.size())==expected){
     ++cache_hits;return new ID3DBlob(blob_bytes(payload));
    }
   }
   LOG_W("gpu","Metal shader cache failed integrity check; recompiling");
  }catch(const std::exception& e){LOG_W("gpu","Metal shader cache read: %s",e.what());}
 }
 auto start=acfa::host::monotonic_time_us();acfa::metal::ShaderSource result;
 bool ok=acfa::metal::hlsl_to_msl(source,entry,stage,&result);
 auto elapsed=acfa::host::monotonic_time_us()-start;compile_us.fetch_add(elapsed);++compiles;
 if(!ok){LOG_E("gpu","HLSL to Metal failed (%s/%s): %s",entry,target,result.diagnostics.c_str());return nullptr;}
 if(!cache.empty())try{
  std::string payload="// ACFA Metal cache v1 "+key+"\n// payload-sha256 "+hash_hex(result.source.data(),result.source.size())+"\n"+result.source;
  std::lock_guard lock(cache_mutex);acfa::write_text(cache,payload);
 }catch(const std::exception& e){LOG_W("gpu","Metal shader cache write: %s",e.what());}
 return new ID3DBlob(blob_bytes(result.source));
}
void shader_stats(u32* compiled,u32* cached,double* seconds){
 if(compiled)*compiled=compiles.load();if(cached)*cached=cache_hits.load();if(seconds)*seconds=compile_us.load()/1e6;
}
bool device_init(void*){
 std::lock_guard lock(init_mutex);if(native_device.dev)return true;
 try{
  require_ok(acfa::metal::create_device(&native_device.dev,&native_device.ctx),"create Metal device");
  native_device.no_overwrite_srv=true;
  ID3DBlob* vs=compile_hlsl(blit_hlsl,"vs_main","vs_5_0");
  ID3DBlob* ps=compile_hlsl(blit_hlsl,"ps_main","ps_5_0");
  if(!vs || !ps){if(vs)vs->Release();if(ps)ps->Release();throw std::runtime_error("Metal presentation shader translation failed");}
  auto vhr=native_device.dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&blit_vs);
  auto phr=native_device.dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&blit_ps);
  vs->Release();ps->Release();require_ok(vhr,"Metal presentation vertex shader");require_ok(phr,"Metal presentation fragment shader");
  D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
  require_ok(native_device.dev->CreateSamplerState(&sampler,&linear_sampler),"Metal presentation sampler");
  LOG_I("gpu","native Metal RSX device initialized; %s",acfa::metal::shader_compiler_version());
  display::swapchain_created(nullptr,false);return true;
 }catch(const std::exception& e){
  LOG_E("gpu","Metal device initialization: %s",e.what());
  if(blit_vs){blit_vs->Release();blit_vs=nullptr;}if(blit_ps){blit_ps->Release();blit_ps=nullptr;}
  if(linear_sampler){linear_sampler->Release();linear_sampler=nullptr;}
  if(native_device.ctx){native_device.ctx->Release();native_device.ctx=nullptr;}
  if(native_device.dev){native_device.dev->Release();native_device.dev=nullptr;}
  return false;
 }
}
NativeShaderWarmResult prewarm_native_shaders(const std::function<bool(u32)>& progress){
 NativeShaderWarmResult result;
 const auto started=acfa::host::monotonic_time_us();
 if(!native_device.dev || cfg().shader_cache.empty())return result;
 const auto before=acfa::metal::shader_cache_stats(native_device.dev);
 acfa::metal::ShaderCacheScanOptions options;options.max_records=acfa::metal::shader_cache_max_entries;
 auto scanned=acfa::metal::scan_shader_cache(cfg().shader_cache,options,[&](const acfa::metal::ShaderCacheRecord& record){
  if(progress && !progress(result.warmed)){result.cancelled=true;return false;}
  if(acfa::host::monotonic_time_us()-started>=60000000){result.budget_reached=true;return false;}
  bool ok=true;
  if(record.has_vertex){
   ID3D11VertexShader* shader=nullptr;
   const auto hr=native_device.dev->CreateVertexShader(record.payload.data(),record.payload.size(),nullptr,&shader);
   if(shader)shader->Release();
   ok=SUCCEEDED(hr);
  }
  if(record.has_fragment){
   ID3D11PixelShader* shader=nullptr;
   const auto hr=native_device.dev->CreatePixelShader(record.payload.data(),record.payload.size(),nullptr,&shader);
   if(shader)shader->Release();
   ok=SUCCEEDED(hr) && ok;
  }
  if(!record.has_vertex && !record.has_fragment)ok=false;
  if(ok)++result.warmed;else ++result.failed;
  return true;
 });
 result.invalid=u32(scanned.invalid);result.skipped=u32(scanned.skipped);
 result.seconds=(acfa::host::monotonic_time_us()-started)/1e6;
 const auto after=acfa::metal::shader_cache_stats(native_device.dev);
 LOG_I("warm","native Metal startup: %u modules warmed, %u failed, %u invalid, %u skipped in %.2f s; %llu libraries compiled, %llu retained-cache hits%s%s",
  result.warmed,result.failed,result.invalid,result.skipped,result.seconds,
  (unsigned long long)(after.compilations-before.compilations),(unsigned long long)(after.hits-before.hits),
  result.budget_reached?"; startup budget reached":"",result.cancelled?"; user closed window":"");
 return result;
}
void request_exclusive(bool on,u32,u32,u32){
 // macOS uses the native borderless mode. No display resolution/refresh changes are attempted.
 if(on)LOG_I("display","saved exclusive-fullscreen choice maps to borderless fullscreen on macOS");
 window_set_fullscreen(on);display::set_exclusive(false);
}
bool exclusive_active(){return false;}
void present_texture(ID3D11ShaderResourceView* source,u32 src_w,u32 src_h){
 if(!source || !native_device.ctx || !src_w || !src_h)return;
 auto* ctx=native_device.ctx;
 try{
  if(!fit_texture(last_frame,last_w,last_h,src_w,src_h))throw std::runtime_error("Metal scene copy allocation failed");
  D3D11_VIEWPORT scene{0,0,float(src_w),float(src_h),0,1};blit_to(source,last_frame,src_w,src_h,scene);
  auto* layer=mac::metal_layer();
  if(!layer){acfa::metal::flush_context(ctx,false);return;}
  int width=0,height=0;mac::drawable_size(&width,&height);if(width<=0 || height<=0){acfa::metal::flush_context(ctx,false);return;}
  u32 w=u32(width),h=u32(height);native_device.width=w;native_device.height=h;
  if(!fit_texture(composed_frame,composed_w,composed_h,w,h))throw std::runtime_error("Metal window composition allocation failed");
  ID3D11RenderTargetView* target=nullptr;require_ok(native_device.dev->CreateRenderTargetView(composed_frame,nullptr,&target),"Metal composition RTV");
  float black[4]={0,0,0,1};ctx->ClearRenderTargetView(target,black);target->Release();
  float scale=std::min(float(w)/src_w,float(h)/src_h);
  float fw=std::floor(src_w*scale+.5f),fh=std::floor(src_h*scale+.5f);
  D3D11_VIEWPORT fit{std::floor((w-fw)/2),std::floor((h-fh)/2),fw,fh,0,1};
  blit_to(source,composed_frame,w,h,fit);
  if(ui::screen_up()){ctx->OMSetRenderTargets(0,nullptr,nullptr);ui::render(native_device.dev,ctx,composed_frame,w,h);}
  ID3D11ShaderResourceView* view=nullptr;require_ok(native_device.dev->CreateShaderResourceView(composed_frame,nullptr,&view),"Metal composition SRV");
  bool presented=acfa::metal::present_to_layer(ctx,view,layer,false);view->Release();
  if(presented)display::after_present(nullptr);
  else acfa::metal::flush_context(ctx,false); // Hidden/minimized/unavailable drawable still submits guest GPU work.
 }catch(const std::exception& e){LOG_E("gpu","Metal presentation: %s",e.what());throw;}
}
bool read_back_texture(ID3D11Texture2D* texture,std::vector<u8>& rgba,u32& w,u32& h){
 if(!texture || !native_device.ctx)return false;
 return acfa::metal::readback_texture(native_device.ctx,texture,rgba,w,h);
}
bool read_back_last_frame(std::vector<u8>& rgba,u32& w,u32& h){return read_back_texture(last_frame,rgba,w,h);}
void begin_capture(u32 index){
 ID3D11Texture2D* source=ui::screen_up() && composed_frame?composed_frame:last_frame;
 if(!source || captures.size()>=8)return;
 D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
 ID3D11Texture2D* staging=nullptr;
 require_ok(native_device.dev->CreateTexture2D(&desc,nullptr,&staging),"Metal capture staging");
 native_device.ctx->CopyResource(staging,source);captures.push_back({staging,index,desc.Width,desc.Height});
 acfa::metal::flush_context(native_device.ctx,false);
}
void poll_captures(const std::function<void(u32,std::vector<u8>&&,u32,u32)>& done){
 auto* ctx=native_device.ctx;if(!ctx)return;
 for(auto it=captures.begin();it!=captures.end();){
  D3D11_MAPPED_SUBRESOURCE mapped{};auto hr=ctx->Map(it->texture,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
  if(hr==DXGI_ERROR_WAS_STILL_DRAWING){++it;continue;}
  if(FAILED(hr))throw std::runtime_error("Metal capture staging map failed");
  std::vector<u8> rgba(size_t(it->w)*it->h*4);
  for(u32 y=0;y<it->h;++y)std::memcpy(rgba.data()+size_t(y)*it->w*4,static_cast<u8*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(it->w)*4);
  ctx->Unmap(it->texture,0);auto index=it->index,w=it->w,h=it->h;it->texture->Release();it=captures.erase(it);done(index,std::move(rgba),w,h);
 }
}
} // namespace rt::gpu
