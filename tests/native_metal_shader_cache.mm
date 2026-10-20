// Synthetic, headless real-GPU tests. No game window, input, save or audio.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat_internal.h"
#include "platform/macos/metal_shader_cache.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool b,const std::string& text){if(!b)throw std::runtime_error(text);}
void ok(HRESULT hr,ID3D11Device* d,const char* s){check(SUCCEEDED(hr),std::string(s)+": "+acfa::metal::last_error(d));}
template<class T>struct Ref {T* p=nullptr;~Ref(){reset();}void reset(){if(p)p->Release();p=nullptr;}T** put(){reset();return &p;}T* operator->()const{return p;}operator T*()const{return p;}};
std::string module(const std::string& color){return R"(#include <metal_stdlib>
using namespace metal;
vertex float4 acfa_vs(uint i [[vertex_id]]) {
 const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
 return float4(p[i],0,1);
}
fragment float4 acfa_ps(){return float4()"+color+R"();}
)";}
void pixels(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11VertexShader* vs,ID3D11PixelShader* ps,bool green){
 Ref<ID3D11Texture2D> target;Ref<ID3D11RenderTargetView> rt;Ref<ID3D11RasterizerState> raster;
 D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
 ok(d->CreateTexture2D(&td,nullptr,target.put()),d,"pixel target");ok(d->CreateRenderTargetView(target,nullptr,rt.put()),d,"pixel RTV");
 D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
 ok(d->CreateRasterizerState(&rd,raster.put()),d,"pixel raster");
 ID3D11RenderTargetView* r=rt;c->OMSetRenderTargets(1,&r,nullptr);
 D3D11_VIEWPORT v{0,0,8,8,0,1};c->RSSetViewports(1,&v);c->RSSetState(raster);
 c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);
 const float zero[4]={};c->ClearRenderTargetView(rt,zero);c->Draw(3,0);
 std::vector<uint8_t> p;uint32_t w=0,h=0;
 check(acfa::metal::readback_texture(c,target,p,w,h),"GPU readback");check(w==8&&h==8,"readback dimensions");
 for(size_t k=0;k<p.size();k+=4)check(p[k]==(green?0:255)&&p[k+1]==(green?255:0)&&p[k+2]==0&&p[k+3]==255,"reused/changed shader pixel mismatch");
 c->VSSetShader(nullptr,nullptr,0);c->PSSetShader(nullptr,nullptr,0);c->OMSetRenderTargets(0,nullptr,nullptr);c->RSSetState(nullptr);
}
void retention_and_pixels(){
 Ref<ID3D11Device> d;Ref<ID3D11DeviceContext> c;ok(acfa::metal::create_device(d.put(),c.put()),d,"device");
 const auto red=module("1,0,0,1"),green=module("0,1,0,1");
 Ref<ID3D11VertexShader> vs;Ref<ID3D11PixelShader> ps;
 ok(d->CreateVertexShader(red.data(),red.size(),nullptr,vs.put()),d,"warm VS");
 ok(d->CreatePixelShader(red.data(),red.size(),nullptr,ps.put()),d,"warm PS");
 __weak id<MTLLibrary> library=vs->native->library;
 __weak id<MTLFunction> vertex=vs->native->function,fragment=ps->native->function;
 auto s=acfa::metal::shader_cache_stats(d);check(s.compilations==1&&s.hits==1&&s.entries==1,"one shared compiled module serves both stages");
 vs.reset();ps.reset();check(library&&vertex&&fragment,"prewarmed native objects survive wrapper release");
 ok(d->CreateVertexShader(red.data(),red.size(),nullptr,vs.put()),d,"retained VS");
 ok(d->CreatePixelShader(red.data(),red.size(),nullptr,ps.put()),d,"retained PS");
 s=acfa::metal::shader_cache_stats(d);check(s.compilations==1&&s.hits==3,"recreation must not compile again");
 check(vs->native->library==library&&vs->native->function==vertex&&ps->native->function==fragment,"recreation reuses identical retained native objects");
 pixels(d,c,vs,ps,false);
 ok(d->CreateVertexShader(green.data(),green.size(),nullptr,vs.put()),d,"changed VS");
 ok(d->CreatePixelShader(green.data(),green.size(),nullptr,ps.put()),d,"changed PS");
 s=acfa::metal::shader_cache_stats(d);check(s.compilations==2&&s.entries==2,"changed source must miss cache");
 pixels(d,c,vs,ps,true);
 std::weak_ptr<acfa::metal::DeviceNative> weak=d->native;
 vs.reset();ps.reset();c.reset();d.reset();check(weak.expired(),"device-owned cache must not form a shared_ptr cycle");
 std::cout<<"shader-cache: released warm wrappers reused with zero new compiles; changed source renders new pixels; lifetime passes\n";
}
void failures_and_concurrency(){
 Ref<ID3D11Device> d;Ref<ID3D11DeviceContext> c;ok(acfa::metal::create_device(d.put(),c.put()),d,"device");
 Ref<ID3D11PixelShader> ps;const std::string invalid="not a Metal program";
 auto start=acfa::metal::shader_cache_stats(d);
 for(int i=0;i<2;++i)check(FAILED(d->CreatePixelShader(invalid.data(),invalid.size(),nullptr,ps.put()))&&!ps.p,"invalid source must fail each time");
 auto s=acfa::metal::shader_cache_stats(d);check(s.compile_attempts==start.compile_attempts+2&&s.compilations==0&&s.entries==0,"compile failures must not be retained");
 const std::string wrong="#include <metal_stdlib>\nusing namespace metal; vertex float4 acfa_ps(){return float4(0,0,0,1);}";
 for(int i=0;i<2;++i)check(FAILED(d->CreatePixelShader(wrong.data(),wrong.size(),nullptr,ps.put()))&&!ps.p,"wrong function stage must fail");
 s=acfa::metal::shader_cache_stats(d);check(s.compile_attempts==4&&s.compilations==2&&s.entries==0,"wrong-stage source must not be retained as valid shader");
 const char bad_utf8[]={char(0xff),char(0xfe)};
 check(FAILED(d->CreatePixelShader(bad_utf8,sizeof bad_utf8,nullptr,ps.put()))&&!ps.p,"invalid UTF-8 must fail");
 check(acfa::metal::shader_cache_stats(d).compile_attempts==4,"invalid UTF-8 must not reach Metal compiler");
 const auto source=module("0,0,1,1");std::atomic<int> failures=0;
 std::vector<std::thread> threads;
 for(int t=0;t<8;++t)threads.emplace_back([&]{@autoreleasepool{Ref<ID3D11VertexShader> local;if(FAILED(d->CreateVertexShader(source.data(),source.size(),nullptr,local.put())))++failures;}});
 for(auto& thread:threads)thread.join();s=acfa::metal::shader_cache_stats(d);
 check(failures==0&&s.compile_attempts==5&&s.compilations==3&&s.hits==7&&s.entries==1,"concurrent first users must compile exactly once");
 std::cout<<"shader-cache: invalid source/UTF-8/stage rejected; eight concurrent first users compile once\n";
}
void bounded_lru(){
 Ref<ID3D11Device> d;Ref<ID3D11DeviceContext> c;ok(acfa::metal::create_device(d.put(),c.put()),d,"device");
 const auto source=module("1,0,0,1");Ref<ID3D11VertexShader> vs;
 const auto count=acfa::metal::shader_cache_max_entries+1;
 for(size_t i=0;i<count;++i){const auto unique=source+"// cache-bound fixture "+std::to_string(i)+"\n";ok(d->CreateVertexShader(unique.data(),unique.size(),nullptr,vs.put()),d,"bounded cache shader");}
 auto s=acfa::metal::shader_cache_stats(d);
 check(s.entries==acfa::metal::shader_cache_max_entries&&s.evictions==1&&s.compilations==count,"native module retention must be bounded");
 check(s.source_bytes<=acfa::metal::shader_cache_max_source_bytes,"retained source bytes stay bounded");
 const auto recent=source+"// cache-bound fixture "+std::to_string(count-1)+"\n";
 ok(d->CreateVertexShader(recent.data(),recent.size(),nullptr,vs.put()),d,"recent cache hit");
 check(acfa::metal::shader_cache_stats(d).compilations==count,"recent module remains warm");
 const auto oldest=source+"// cache-bound fixture 0\n";
 ok(d->CreateVertexShader(oldest.data(),oldest.size(),nullptr,vs.put()),d,"evicted shader recompiles");
 s=acfa::metal::shader_cache_stats(d);check(s.compilations==count+1&&s.entries==acfa::metal::shader_cache_max_entries&&s.evictions==2,"evicted module compiles lazily and remains bounded");
 std::cout<<"shader-cache: "<<count<<" real native modules respect LRU limit; hot entry retained and evicted entry recompiles\n";
}
}
int main(){@autoreleasepool{try{retention_and_pixels();failures_and_concurrency();bounded_lru();std::cout<<"native-metal-shader-cache: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"native-metal-shader-cache: FAIL: "<<e.what()<<"\n";return 1;}}}
