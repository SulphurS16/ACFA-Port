// Real headless GPU tests for the RSX context subset. Synthetic data only.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat_internal.h"
#include "platform/macos/metal_render_batch.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool b,const std::string& text){if(!b)throw std::runtime_error(text);}
template<class T> struct Owned {T* p=nullptr;~Owned(){if(p)p->Release();} T** put(){return &p;} operator T*()const{return p;} T* operator->()const{return p;}};
struct Constants {float color[4];float params[4];};
constexpr UINT side=32;
const char* shader=R"(#include <metal_stdlib>
using namespace metal;
struct C {float4 color;float4 params;};
struct V {float4 position [[position]];float4 color;float2 uv;};
vertex V acfa_vs(uint i [[vertex_id]],constant C& c [[buffer(0)]],device const float4* v0 [[buffer(16)]],device const float4* v1 [[buffer(17)]],device const float4* v2 [[buffer(18)]],device const float4* v3 [[buffer(19)]]) { V v;v.position=float4(v0[i].xy,c.params.x,1);v.color=c.color*(v1[0]+v2[0]+v3[0])/3.0;v.uv=v0[i].xy*float2(.5,-.5)+.5;return v; }
fragment float4 acfa_ps(V v [[stage_in]],constant C& c [[buffer(2)]]) {return v.color*c.color;}
)";
struct Fixture {
 Owned<ID3D11Device> d;Owned<ID3D11DeviceContext> c;Owned<ID3D11Texture2D> target;Owned<ID3D11RenderTargetView> rt;Owned<ID3D11VertexShader> vs;Owned<ID3D11PixelShader> ps;Owned<ID3D11Buffer> cb,fp,pos,ones,ib;Owned<ID3D11ShaderResourceView> positions,unit;Owned<ID3D11RasterizerState> raster,scissored;
 Fixture(){
  check(SUCCEEDED(acfa::metal::create_device(d.put(),c.put())),"create device");
  D3D11_TEXTURE2D_DESC td{side,side,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};ok(d->CreateTexture2D(&td,nullptr,target.put()),"target");ok(d->CreateRenderTargetView(target,nullptr,rt.put()),"rtv");ok(d->CreateVertexShader(shader,std::strlen(shader),nullptr,vs.put()),"vertex shader");ok(d->CreatePixelShader(shader,std::strlen(shader),nullptr,ps.put()),"fragment shader");
  D3D11_BUFFER_DESC bd{sizeof(Constants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};Constants init{{1,0,0,1},{.25f,0,0,0}},fragment{{1,1,1,1},{0,0,0,0}};D3D11_SUBRESOURCE_DATA data{&init,0,0};ok(d->CreateBuffer(&bd,&data,cb.put()),"VS constants");data.pSysMem=&fragment;ok(d->CreateBuffer(&bd,&data,fp.put()),"PS constants");
  const float vertices[12]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};const float one[4]={1,1,1,1};bd={sizeof vertices,D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,D3D11_CPU_ACCESS_WRITE,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};data.pSysMem=vertices;ok(d->CreateBuffer(&bd,&data,pos.put()),"raw positions");bd={sizeof one,D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};data.pSysMem=one;ok(d->CreateBuffer(&bd,&data,ones.put()),"raw factor");
  auto srv=[&](ID3D11Buffer* b,UINT bytes,ID3D11ShaderResourceView** out){D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_TYPELESS;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sd.BufferEx={0,bytes/4,D3D11_BUFFEREX_SRV_FLAG_RAW};ok(d->CreateShaderResourceView(b,&sd,out),"raw view");};srv(pos,sizeof vertices,positions.put());srv(ones,sizeof one,unit.put());
  const UINT indices[]={2,0,1};bd={sizeof indices,D3D11_USAGE_IMMUTABLE,D3D11_BIND_INDEX_BUFFER,0,0,0};data.pSysMem=indices;ok(d->CreateBuffer(&bd,&data,ib.put()),"index buffer");
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ok(d->CreateRasterizerState(&rd,raster.put()),"raster");rd.ScissorEnable=TRUE;ok(d->CreateRasterizerState(&rd,scissored.put()),"scissor raster");bind();
 }
 void ok(HRESULT h,const char* msg){check(SUCCEEDED(h),std::string(msg)+": "+acfa::metal::last_error(d));}
 void bind(){ID3D11RenderTargetView* r=rt;c->OMSetRenderTargets(1,&r,nullptr);D3D11_VIEWPORT v{0,0,side,side,0,1};c->RSSetViewports(1,&v);c->RSSetState(raster);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);ID3D11Buffer* b=cb;c->VSSetConstantBuffers(0,1,&b);b=fp;c->PSSetConstantBuffers(2,1,&b);ID3D11ShaderResourceView* raw[]={positions,unit,unit,unit};c->VSSetShaderResources(0,4,raw);c->IASetIndexBuffer(ib,DXGI_FORMAT_R32_UINT,0);}
 void color(float r,float g,float b,float a=1,float z=.25){Constants v{{r,g,b,a},{z,0,0,0}};c->UpdateSubresource(cb,0,nullptr,&v,0,0);}
 void clear(){const float zero[4]={0,0,0,0};c->ClearRenderTargetView(rt,zero);}
 std::vector<uint8_t> pixels(){std::vector<uint8_t> p;uint32_t w=0,h=0;check(acfa::metal::readback_texture(c,target,p,w,h),"readback: "+acfa::metal::last_error(d));check(w==side&&h==side,"readback dimensions");return p;}
 void uniform(std::array<int,4> want){auto p=pixels();for(size_t i=0;i<p.size();++i)check(std::abs(int(p[i])-want[i%4])<=1,"pixel mismatch at byte "+std::to_string(i)+" got "+std::to_string(p[i])+" expected "+std::to_string(want[i%4]));}
};
void drawing_and_queries(){
 Fixture f;f.clear();f.color(1,0,0);f.c->Draw(3,0);f.uniform({255,0,0,255});
 // Two queued draws use different native constant allocations before any wait.
 f.clear();f.c->RSSetState(f.scissored);D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};f.c->RSSetScissorRects(1,&left);f.color(0,1,0);f.c->Draw(3,0);f.c->RSSetScissorRects(1,&right);f.color(0,0,1);f.c->DrawIndexed(3,0,0);auto p=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){size_t k=(y*side+x)*4;check(p[k]==0&&p[k+1]==(x<side/2?255:0)&&p[k+2]==(x<side/2?0:255)&&p[k+3]==255,"COW indexed/scissor pixels");}
 // Raw buffer discard changes the wrapper's allocation; retained previous draw
 // must preserve the original positions, and current raw SRV must see new bytes.
 f.clear();f.c->RSSetScissorRects(1,&left);f.color(1,0,0);f.c->Draw(3,0);D3D11_MAPPED_SUBRESOURCE map{};f.ok(f.c->Map(f.pos,0,D3D11_MAP_WRITE_DISCARD,0,&map),"discard raw buffer");const float offscreen[12]={4,4,0,1, 6,4,0,1, 4,6,0,1};std::memcpy(map.pData,offscreen,sizeof offscreen);f.c->Unmap(f.pos,0);f.c->RSSetScissorRects(1,&right);f.color(0,1,0);f.c->Draw(3,0);p=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){size_t k=(y*side+x)*4;check(p[k]==(x<side/2?255:0)&&p[k+1]==0,"raw discard allocation lifetime");}
 const float fullscreen[12]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};f.ok(f.c->Map(f.pos,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&map),"no overwrite after completed readback");std::memcpy(map.pData,fullscreen,sizeof fullscreen);f.c->Unmap(f.pos,0);f.c->RSSetState(f.raster);
 Owned<ID3D11Query> q,event;D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION,0};f.ok(f.d->CreateQuery(&qd,q.put()),"occlusion query");qd.Query=D3D11_QUERY_EVENT;f.ok(f.d->CreateQuery(&qd,event.put()),"event query");uint64_t samples=123;check(f.c->GetData(q,&samples,sizeof samples,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_FALSE,"unended query not ready");f.c->Begin(q);f.c->Draw(3,0);f.c->DrawIndexed(3,0,0);f.c->End(q);f.c->End(event);acfa::metal::flush_context(f.c,true);f.ok(f.c->GetData(q,&samples,sizeof samples,0),"occlusion result");check(samples==2*side*side,"actual accumulated occlusion count got "+std::to_string(samples));BOOL complete=FALSE;f.ok(f.c->GetData(event,&complete,sizeof complete,0),"event result");check(complete,"event completion");
 f.c->Begin(q);f.c->End(q);acfa::metal::flush_context(f.c,true);f.ok(f.c->GetData(q,&samples,sizeof samples,0),"empty occlusion result");check(samples==0,"empty query samples");
 ID3D11VertexShader* saved=nullptr;f.c->VSGetShader(&saved,nullptr,nullptr);check(saved==f.vs.p,"GetShader identity");f.c->VSSetShader(nullptr,nullptr,0);check(saved->Release()>=1,"GetShader AddRef ownership");f.c->VSSetShader(f.vs,nullptr,0);
 std::cout<<"context: full pixels, COW/discard/raw16..19/index/scissor and real visibility queries pass\n";
}
// Appending to a different range must neither submit nor wait for the old
// range. Both draws stay encoded until one explicit readback flushes the frame.
void disjoint_no_overwrite_ring(){
 Fixture f;
 const float fullscreen[12]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
 const float offscreen[12]={4,4,0,1, 6,4,0,1, 4,6,0,1};
 std::array<float,24> initial{};
 std::memcpy(initial.data(),fullscreen,sizeof fullscreen);
 std::memcpy(initial.data()+12,offscreen,sizeof offscreen);
 Owned<ID3D11Buffer> ring;Owned<ID3D11ShaderResourceView> first,second;
 D3D11_BUFFER_DESC bd{sizeof initial,D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,
     D3D11_CPU_ACCESS_WRITE,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
 D3D11_SUBRESOURCE_DATA init{initial.data(),0,0};
 f.ok(f.d->CreateBuffer(&bd,&init,ring.put()),"disjoint ring buffer");
 D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_TYPELESS;
 sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
 sd.BufferEx={0,12,D3D11_BUFFEREX_SRV_FLAG_RAW};
 f.ok(f.d->CreateShaderResourceView(ring,&sd,first.put()),"ring first region");
 sd.BufferEx.FirstElement=12;
 f.ok(f.d->CreateShaderResourceView(ring,&sd,second.put()),"ring appended region");
 ID3D11ShaderResourceView* raw[]={first,f.unit,f.unit,f.unit};
 f.c->VSSetShaderResources(0,4,raw);f.c->RSSetState(f.scissored);
 D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};
 f.clear();f.color(1,0,0);f.c->RSSetScissorRects(1,&left);f.c->Draw(3,0);
 id<MTLBuffer> allocation=ring->native->buffer;
 (void)acfa::metal::performance_report();
 D3D11_MAPPED_SUBRESOURCE map{};
 f.ok(f.c->Map(ring,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&map),"append without global synchronization");
 std::memcpy(static_cast<uint8_t*>(map.pData)+sizeof fullscreen,fullscreen,sizeof fullscreen);
 f.c->Unmap(ring,0);
 check(ring->native->buffer==allocation,"NO_OVERWRITE retains ring allocation");
 const auto after_map=acfa::metal::performance_report();
 // Count assertions establish that Map did not commit or wait. They avoid an
 // unstable duration threshold and would fail with the previous global flush.
 check(after_map.find("commit 0.00 ms/0 calls")!=std::string::npos,
       "NO_OVERWRITE must not commit pending draws: "+after_map);
 check(after_map.find("waitUntilCompleted 0.00 ms/0 calls")!=std::string::npos,
       "NO_OVERWRITE must not wait on pending draws: "+after_map);
 check(after_map.find("buffer NO_OVERWRITE Map")!=std::string::npos&&
       after_map.ends_with("/1 calls"),"one observed NO_OVERWRITE Map: "+after_map);
 raw[0]=second;f.c->VSSetShaderResources(0,4,raw);f.color(0,1,0);
 f.c->RSSetScissorRects(1,&right);f.c->Draw(3,0);
 const auto pixels=f.pixels();
 for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){
   const size_t k=(y*side+x)*4;
   check(pixels[k]==(x<side/2?255:0)&&pixels[k+1]==(x<side/2?0:255)&&
         pixels[k+2]==0&&pixels[k+3]==255,"disjoint ring old/new region pixels");
 }
 std::cout<<"context: disjoint NO_OVERWRITE ring draws preserve old/new pixels with zero Map commits/waits\n";
}

void sparse_raw_bindings(){
 Fixture f;
 static const char* sparse=R"(#include <metal_stdlib>
using namespace metal;
struct C {float4 color;float4 params;};
struct V {float4 position [[position]];float4 color;float2 uv;};
vertex V acfa_vs(uint i [[vertex_id]],constant C& c [[buffer(0)]],device const float4* v0 [[buffer(16)]],device const float4* v1 [[buffer(17)]],device const float4* v2 [[buffer(18)]],device const float4* v3 [[buffer(19)]]) {float4 p;switch(uint(c.params.y)){case 0:p=v0[i];break;case 1:p=v1[i];break;case 2:p=v2[i];break;default:p=v3[i];break;}V v;v.position=float4(p.xy,c.params.x,1);v.color=c.color;v.uv=p.xy*float2(.5,-.5)+.5;return v;}
)";
 Owned<ID3D11VertexShader> vs;f.ok(f.d->CreateVertexShader(sparse,std::strlen(sparse),nullptr,vs.put()),"sparse raw shader");f.c->VSSetShader(vs,nullptr,0);
 // This reproduces Slang's dynamic fetch signature: all four pointers remain
 // active Metal arguments, while the RSX format chooses only stream zero.
 ID3D11ShaderResourceView* raw[]={f.positions,nullptr,nullptr,nullptr};f.c->VSSetShaderResources(0,4,raw);f.clear();f.color(1,0,0);f.c->Draw(3,0);f.uniform({255,0,0,255});
 // A null selected geometry stream must not fabricate a passing image.
 Constants cb{{0,1,0,1},{.25f,1,0,0}};f.c->UpdateSubresource(f.cb,0,nullptr,&cb,0,0);f.clear();f.c->Draw(3,0);f.uniform({0,0,0,0});
 // Binding the actual geometry to that selected slot restores the expected
 // output, proving the zero fallback never overrides a nonnull stream.
 raw[0]=nullptr;raw[1]=f.positions;f.c->VSSetShaderResources(0,4,raw);f.c->Draw(3,0);f.uniform({0,255,0,255});
 std::cout<<"context: sparse Slang-style raw16..19 arguments and selected real stream binding pass\n";
}

void pipeline_shader_lifetime(){
 Fixture f;f.clear();f.color(1,0,0);f.c->Draw(3,0);f.uniform({255,0,0,255});
 __weak id<MTLFunction> old_function=f.vs->native->function;
 // Only the pipeline cache should need the old vertex function from this point.
 // Clearing the context binding and releasing the public wrapper must not make
 // the cache's pointer identity available for reuse by a different shader.
 f.c->VSSetShader(nullptr,nullptr,0);f.vs.p->Release();f.vs.p=nullptr;
 check(old_function!=nil,"pipeline cache retains old vertex function identity");
 std::string different(shader);auto start=different.find("v.color=c.color*");auto end=different.find(";v.uv=",start);check(start!=std::string::npos&&end!=std::string::npos,"shader lifetime fixture");different.replace(start,end-start,"v.color=float4(0,0,1,1)");Owned<ID3D11VertexShader> next;f.ok(f.d->CreateVertexShader(different.data(),different.size(),nullptr,next.put()),"distinct replacement shader");check(old_function!=next->native->function,"live cached function identity cannot be reused");f.c->VSSetShader(next,nullptr,0);f.clear();f.c->Draw(3,0);f.uniform({0,0,255,255});
 std::cout<<"context: cached shader identity survives wrapper release and replacement draws distinct output\n";
}

void blend_and_depth(){
 Fixture f;f.clear();D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=1|4;Owned<ID3D11BlendState> write;f.ok(f.d->CreateBlendState(&bd,write.put()),"write-mask blend");f.c->OMSetBlendState(write,nullptr,~0u);f.color(.2,.4,.6,.8);f.c->Draw(3,0);f.uniform({51,0,153,0});
 bd.RenderTarget[0].RenderTargetWriteMask=15;bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;Owned<ID3D11BlendState> alpha;f.ok(f.d->CreateBlendState(&bd,alpha.put()),"alpha blend");f.clear();f.c->OMSetBlendState(alpha,nullptr,~0u);f.color(1,0,0,.5);f.c->Draw(3,0);f.uniform({128,0,0,128});f.c->OMSetBlendState(nullptr,nullptr,~0u);
 D3D11_TEXTURE2D_DESC td{side,side,1,1,DXGI_FORMAT_R24G8_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0};Owned<ID3D11Texture2D> depth;Owned<ID3D11DepthStencilView> ds;f.ok(f.d->CreateTexture2D(&td,nullptr,depth.put()),"depth texture");f.ok(f.d->CreateDepthStencilView(depth,nullptr,ds.put()),"depth view");ID3D11RenderTargetView* rt=f.rt;f.c->OMSetRenderTargets(1,&rt,ds);f.c->ClearDepthStencilView(ds,3,1,0);f.clear();f.color(1,0,0,1,.2);f.c->Draw(3,0);f.color(0,1,0,1,.8);f.c->Draw(3,0);f.uniform({255,0,0,255});
 D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.StencilEnable=TRUE;dd.StencilReadMask=dd.StencilWriteMask=255;dd.FrontFace=dd.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};Owned<ID3D11DepthStencilState> replace,equal;f.ok(f.d->CreateDepthStencilState(&dd,replace.put()),"stencil replace");f.c->OMSetDepthStencilState(replace,7);f.color(0,0,1);f.c->Draw(3,0);dd.FrontFace=dd.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_EQUAL};f.ok(f.d->CreateDepthStencilState(&dd,equal.put()),"stencil equal");f.c->OMSetDepthStencilState(equal,8);f.color(0,1,0);f.c->Draw(3,0);f.uniform({0,0,255,255});f.c->OMSetDepthStencilState(equal,7);f.c->Draw(3,0);f.uniform({0,255,0,255});
 std::cout<<"context: channel mask, alpha blend, depth rejection and stencil reference pass\n";
}
void texture_binding_and_mips(){
 Fixture f;
 static const char* sampled=R"(#include <metal_stdlib>
using namespace metal;
struct V {float4 position [[position]];float4 color;float2 uv;};
fragment float4 acfa_ps(V v [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]]) {return t.sample(s,v.uv);}
)";
 Owned<ID3D11PixelShader> ps;f.ok(f.d->CreatePixelShader(sampled,std::strlen(sampled),nullptr,ps.put()),"sample shader");D3D11_TEXTURE2D_DESC td{2,2,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,D3D11_CPU_ACCESS_WRITE,0};std::array<uint8_t,16> red={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};D3D11_SUBRESOURCE_DATA init{red.data(),8,16};Owned<ID3D11Texture2D> tex;Owned<ID3D11ShaderResourceView> view;Owned<ID3D11SamplerState> sampler;f.ok(f.d->CreateTexture2D(&td,&init,tex.put()),"sample texture");f.ok(f.d->CreateShaderResourceView(tex,nullptr,view.put()),"sample SRV");D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxAnisotropy=1;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxLOD=D3D11_FLOAT32_MAX;f.ok(f.d->CreateSamplerState(&sd,sampler.put()),"sample sampler");f.c->PSSetShader(ps,nullptr,0);ID3D11ShaderResourceView* srv=view;f.c->PSSetShaderResources(0,1,&srv);ID3D11SamplerState* smp=sampler;f.c->PSSetSamplers(0,1,&smp);f.clear();f.c->RSSetState(f.scissored);D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};f.c->RSSetScissorRects(1,&left);f.c->Draw(3,0);
 // Texture Map waits for encoded reads before uploading new texels on Unmap.
 D3D11_MAPPED_SUBRESOURCE map{};f.ok(f.c->Map(tex,0,D3D11_MAP_WRITE_DISCARD,0,&map),"dynamic texture discard");for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x){auto* p=static_cast<uint8_t*>(map.pData)+y*map.RowPitch+x*4;p[0]=0;p[1]=255;p[2]=0;p[3]=255;}f.c->Unmap(tex,0);f.c->RSSetScissorRects(1,&right);f.c->Draw(3,0);auto p=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){auto k=(y*side+x)*4;check(p[k]==(x<side/2?255:0)&&p[k+1]==(x<side/2?0:255),"texture read/write lifetime");}
 ID3D11ShaderResourceView* got=nullptr;ID3D11SamplerState* gs=nullptr;f.c->PSGetShaderResources(0,1,&got);f.c->PSGetSamplers(0,1,&gs);check(got==view.p&&gs==sampler.p,"PS Get identity");srv=nullptr;smp=nullptr;f.c->PSSetShaderResources(0,1,&srv);f.c->PSSetSamplers(0,1,&smp);check(got->Release()>=1&&gs->Release()>=1,"PS Get AddRef");
 // Clear and draw must select the RTV's mip rather than always mip zero.
 td={side,side,2,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};Owned<ID3D11Texture2D> mips,staging;Owned<ID3D11RenderTargetView> miprt;f.ok(f.d->CreateTexture2D(&td,nullptr,mips.put()),"mipped target");D3D11_RENDER_TARGET_VIEW_DESC rd{DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_RTV_DIMENSION_TEXTURE2D,{1}};f.ok(f.d->CreateRenderTargetView(mips,&rd,miprt.put()),"mip RTV");const FLOAT blue[]={0,0,1,1};f.c->ClearRenderTargetView(miprt,blue);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;f.ok(f.d->CreateTexture2D(&td,nullptr,staging.put()),"mip staging");f.c->CopyResource(staging,mips);f.ok(f.c->Map(staging,1,D3D11_MAP_READ,0,&map),"map mip 1");for(UINT y=0;y<side/2;++y)for(UINT x=0;x<side/2;++x){auto* p=static_cast<uint8_t*>(map.pData)+y*map.RowPitch+x*4;check(p[0]==0&&p[1]==0&&p[2]==255&&p[3]==255,"mip RTV clear");}f.c->Unmap(staging,1);
 f.c->RSSetState(f.raster);f.c->PSSetShader(f.ps,nullptr,0);ID3D11RenderTargetView* mr=miprt;f.c->OMSetRenderTargets(1,&mr,nullptr);D3D11_VIEWPORT vp{0,0,side/2,side/2,0,1};f.c->RSSetViewports(1,&vp);f.color(1,0,1);f.c->Draw(3,0);f.c->CopyResource(staging,mips);f.ok(f.c->Map(staging,1,D3D11_MAP_READ,0,&map),"map drawn mip 1");for(UINT y=0;y<side/2;++y)for(UINT x=0;x<side/2;++x){auto* p=static_cast<uint8_t*>(map.pData)+y*map.RowPitch+x*4;check(p[0]==255&&p[1]==0&&p[2]==255&&p[3]==255,"mip RTV draw");}f.c->Unmap(staging,1);
 std::cout<<"context: texture/sampler slots, texture discard synchronization and mip render/clear pass\n";
}

void depth_mips_and_rejections(){
 Fixture f;Owned<ID3D11Texture2D> color,depth;Owned<ID3D11RenderTargetView> rt;Owned<ID3D11DepthStencilView> ds;
 D3D11_TEXTURE2D_DESC td{side/2,side/2,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};f.ok(f.d->CreateTexture2D(&td,nullptr,color.put()),"depth-mip color");f.ok(f.d->CreateRenderTargetView(color,nullptr,rt.put()),"depth-mip color view");td.Width=td.Height=side;td.MipLevels=2;td.Format=DXGI_FORMAT_R16_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;f.ok(f.d->CreateTexture2D(&td,nullptr,depth.put()),"depth mips");D3D11_DEPTH_STENCIL_VIEW_DESC dd{DXGI_FORMAT_D16_UNORM,D3D11_DSV_DIMENSION_TEXTURE2D,0,{1}};f.ok(f.d->CreateDepthStencilView(depth,&dd,ds.put()),"depth mip view");D3D11_DEPTH_STENCIL_DESC ignored_stencil{};ignored_stencil.DepthEnable=TRUE;ignored_stencil.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ignored_stencil.DepthFunc=D3D11_COMPARISON_LESS;ignored_stencil.StencilEnable=TRUE;ignored_stencil.StencilReadMask=ignored_stencil.StencilWriteMask=255;ignored_stencil.FrontFace=ignored_stencil.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_NEVER};Owned<ID3D11DepthStencilState> no_aspect;f.ok(f.d->CreateDepthStencilState(&ignored_stencil,no_aspect.put()),"z16 ignores stencil state");f.c->OMSetDepthStencilState(no_aspect,42);ID3D11RenderTargetView* r=rt;f.c->OMSetRenderTargets(1,&r,ds);D3D11_VIEWPORT vp{0,0,side/2,side/2,0,1};f.c->RSSetViewports(1,&vp);const float zero[4]={0,0,0,0};f.c->ClearRenderTargetView(rt,zero);
 // The RSX renderer passes both flags even when a z16 surface has no stencil.
 f.c->ClearDepthStencilView(ds,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.4f,39);f.color(1,0,0,1,.8);f.c->Draw(3,0);std::vector<uint8_t> pixels;uint32_t w=0,h=0;check(acfa::metal::readback_texture(f.c,color,pixels,w,h),"depth mip readback");for(auto b:pixels)check(b==0,"depth mip clear rejects farther samples");f.color(0,1,0,1,.2);f.c->Draw(3,0);check(acfa::metal::readback_texture(f.c,color,pixels,w,h),"depth mip draw readback");for(size_t i=0;i<pixels.size();++i)check(pixels[i]==((i%4==1||i%4==3)?255:0),"depth mip nearer draw");
 D3D11_MAPPED_SUBRESOURCE map{};check(FAILED(f.c->Map(f.cb,0,D3D11_MAP_WRITE,0,&map))&&!map.pData,"Map rejects absent CPU access");check(FAILED(f.c->Map(f.pos,1,D3D11_MAP_WRITE_DISCARD,0,&map)),"Map rejects buffer subresource");
 bool threw=false;const float changed[4]={.1,.2,.3,.4};try{f.c->UpdateSubresource(f.ones,0,nullptr,changed,0,0);}catch(const std::runtime_error&){threw=true;}check(threw,"Update rejects immutable buffer");threw=false;try{f.c->CopyResource(f.ones,f.ones);}catch(const std::runtime_error&){threw=true;}check(threw,"Copy rejects invalid alias");
 f.c->OMSetDepthStencilState(nullptr,0);f.bind();f.clear();f.color(1,0,0);float half=.5f;D3D11_BOX partial{4,0,0,8,1,1};f.c->UpdateSubresource(f.cb,0,&partial,&half,0,0);f.c->Draw(3,0);f.uniform({255,128,0,255});
 std::cout<<"context: z16 missing stencil, DSV mip, partial buffer preservation and explicit API rejections pass\n";
}

void copies_and_maps(){
 Fixture f;f.clear();f.color(.1,.2,.3);f.c->Draw(3,0);D3D11_TEXTURE2D_DESC td{side,side,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,0};Owned<ID3D11Texture2D> staging;f.ok(f.d->CreateTexture2D(&td,nullptr,staging.put()),"staging texture");f.c->CopyResource(staging,f.target);D3D11_MAPPED_SUBRESOURCE map{};f.ok(f.c->Map(staging,0,D3D11_MAP_READ,0,&map),"staging Map READ");check(map.RowPitch>=side*4&&map.DepthPitch>=map.RowPitch*side,"staging pitches");for(UINT y=0;y<side;++y){auto* row=static_cast<uint8_t*>(map.pData)+y*map.RowPitch;for(UINT x=0;x<side;++x)check(std::abs(int(row[x*4])-26)<=1&&std::abs(int(row[x*4+1])-51)<=1&&std::abs(int(row[x*4+2])-77)<=1,"staging copied pixels");}f.c->Unmap(staging,0);
 f.ok(f.c->Map(staging,0,D3D11_MAP_WRITE,0,&map),"staging Map WRITE");for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){auto* p=static_cast<uint8_t*>(map.pData)+y*map.RowPitch+x*4;p[0]=17;p[1]=34;p[2]=51;p[3]=255;}f.c->Unmap(staging,0);f.c->CopyResource(f.target,staging);f.uniform({17,34,51,255});
 std::vector<uint8_t> rows(3*24,0);for(UINT y=0;y<3;++y)for(UINT x=0;x<4;++x){auto* p=rows.data()+y*24+x*4;p[0]=90;p[1]=80;p[2]=70;p[3]=60;}D3D11_BOX box{2,3,0,6,6,1};f.c->UpdateSubresource(f.target,0,&box,rows.data(),24,72);auto pixels=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){bool inside=x>=2&&x<6&&y>=3&&y<6;size_t k=(y*side+x)*4;check(pixels[k]==(inside?90:17)&&pixels[k+3]==(inside?60:255),"boxed upload source row pitch");}
 D3D11_BOX region{2,3,0,6,6,1};f.c->CopySubresourceRegion(f.target,0,12,13,0,staging,0,&region);pixels=f.pixels();check(pixels[(13*side+12)*4]==17,"boxed texture copy");
 Owned<ID3D11Buffer> src,dst;D3D11_BUFFER_DESC bd{64,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,0,0};std::array<uint8_t,64> bytes{};for(UINT i=0;i<64;++i)bytes[i]=UINT8(i);D3D11_SUBRESOURCE_DATA init{bytes.data(),0,0};f.ok(f.d->CreateBuffer(&bd,&init,src.put()),"staging source buffer");f.ok(f.d->CreateBuffer(&bd,nullptr,dst.put()),"staging destination buffer");D3D11_BOX buffer_box{4,0,0,28,1,1};f.c->CopySubresourceRegion(dst,0,8,0,0,src,0,&buffer_box);f.ok(f.c->Map(dst,0,D3D11_MAP_READ,0,&map),"buffer copy map");for(UINT i=0;i<24;++i)check(static_cast<uint8_t*>(map.pData)[8+i]==4+i,"buffer copy offsets");f.c->Unmap(dst,0);
 f.c->ClearState();ID3D11RenderTargetView* r=nullptr;ID3D11DepthStencilView* d=nullptr;f.c->OMGetRenderTargets(1,&r,&d);check(!r&&!d,"ClearState resource references");
 std::cout<<"context: texture/buffer copies, CPU maps, source pitches, boxed updates, ClearState pass\n";
}
void consecutive_draw_batches(){
 Fixture f;f.clear();f.c->RSSetState(f.scissored);
 D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};
 const auto before=acfa::metal::render_batch_stats(f.c);
 for(unsigned i=0;i<120;++i){f.c->VSSetShader(f.vs,nullptr,0);f.c->PSSetShader(f.ps,nullptr,0);f.c->RSSetScissorRects(1,i<60?&left:&right);f.color(i<60?1:0,i<60?0:1,0,1,.25f+.001f*float(i+1));if(i%2)f.c->DrawIndexed(3,0,0);else f.c->Draw(3,0);}
 auto after=acfa::metal::render_batch_stats(f.c);
 check(after.draws-before.draws==120&&after.encoders-before.encoders==1&&after.reused-before.reused==119,"120 ordered COW/scissor/index draws must use one retained encoder");
 check(after.pipeline_requests-before.pipeline_requests==120&&after.pipeline_fast_reuses-before.pipeline_fast_reuses>=119,"unchanged shaders and dynamic state reuse the last immutable pipeline without descriptors or text keys");
 check(after.resource_binds-before.resource_binds==64+119&&after.resource_reuses-before.resource_reuses==63*119,"COW replaces native constant binding while unchanged encoder resources are reused");
 check(after.depth_requests-before.depth_requests==120&&after.depth_reuses-before.depth_reuses==119,"unchanged effective depth state avoids descriptor hashing on following draws");
 const auto p=f.pixels();after=acfa::metal::render_batch_stats(f.c);check(after.ends-before.ends==1,"readback closes retained encoder exactly once");
 for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){size_t k=(y*side+x)*4;check(p[k]==(x<side/2?255:0)&&p[k+1]==(x<side/2?0:255)&&p[k+2]==0&&p[k+3]==255,"batched draw COW/scissor pixels");}
 // Blending and resource copy must preserve every draw's order inside/outside
 // the pass; existing depth/stencil and mip fixtures cover those state changes.
 f.c->RSSetState(f.raster);f.clear();f.color(1,0,0);f.c->Draw(3,0);
 const float blue[4]={0,0,1,1};f.c->ClearRenderTargetView(f.rt,blue);
 f.c->RSSetState(f.scissored);f.c->RSSetScissorRects(1,&left);f.color(0,1,0);f.c->Draw(3,0);
 auto split=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const size_t k=(y*side+x)*4;check(split[k]==0&&split[k+1]==(x<side/2?255:0)&&split[k+2]==(x<side/2?0:255)&&split[k+3]==255,"clear terminates prior batch before new draws");}
 D3D11_TEXTURE2D_DESC td{side,side,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0};Owned<ID3D11Texture2D> copied;f.ok(f.d->CreateTexture2D(&td,nullptr,copied.put()),"batch copy staging");
 f.c->RSSetState(f.raster);f.clear();f.color(1,0,0);f.c->Draw(3,0);f.c->CopyResource(copied,f.target);f.color(0,1,0);f.c->Draw(3,0);
 D3D11_MAPPED_SUBRESOURCE mapped{};f.ok(f.c->Map(copied,0,D3D11_MAP_READ,0,&mapped),"batch copy map");for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){auto* v=static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4;check(v[0]==255&&v[1]==0&&v[2]==0&&v[3]==255,"copy boundary preserves earlier red draw");}f.c->Unmap(copied,0);f.uniform({0,255,0,255});
 // Merely binding an aliased texture as an input ends the old writing pass.
 // We do not draw while sampling the same current attachment.
 f.color(1,0,0);f.c->Draw(3,0);Owned<ID3D11ShaderResourceView> alias;f.ok(f.d->CreateShaderResourceView(f.target,nullptr,alias.put()),"batch alias SRV");
 auto a=acfa::metal::render_batch_stats(f.c);ID3D11ShaderResourceView* input=alias;f.c->PSSetShaderResources(0,1,&input);auto b=acfa::metal::render_batch_stats(f.c);check(b.ends==a.ends+1,"input alias closes preceding render pass");input=nullptr;f.c->PSSetShaderResources(0,1,&input);
 // Query draws stay separate and report the same accumulated visibility.
 Owned<ID3D11Query> query;D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION,0};f.ok(f.d->CreateQuery(&qd,query.put()),"batch query");f.clear();const auto q0=acfa::metal::render_batch_stats(f.c);
 f.c->Draw(3,0);f.c->Begin(query);f.c->Draw(3,0);f.c->DrawIndexed(3,0,0);f.c->End(query);f.c->Draw(3,0);f.c->Draw(3,0);acfa::metal::flush_context(f.c,true);
 uint64_t count=0;f.ok(f.c->GetData(query,&count,sizeof count,0),"batch query result");check(count==2*side*side,"query samples are unchanged by batching");
 const auto q1=acfa::metal::render_batch_stats(f.c);check(q1.draws-q0.draws==5&&q1.encoders-q0.encoders==4&&q1.reused-q0.reused==1&&q1.query_draws-q0.query_draws==2,"query boundaries preserve legacy per-draw visibility passes");
 std::cout<<"context: 120 COW/scissor/index draws share one encoder; clear/copy/readback/SRV-alias/query boundaries preserve pixels and counts\n";
}

void constant_content_reuse(){
 Fixture f;f.clear();f.c->RSSetState(f.scissored);D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};f.c->RSSetScissorRects(1,&left);f.c->Draw(3,0);
 id<MTLBuffer> original=f.cb->native->buffer;auto before=acfa::metal::render_batch_stats(f.c);
 for(unsigned i=0;i<100;++i)f.color(1,0,0);
 const float same=1;D3D11_BOX box{0,0,0,4,1,1};(void)acfa::metal::performance_report();f.c->UpdateSubresource(f.cb,0,&box,&same,0,0);
 auto after=acfa::metal::render_batch_stats(f.c);check(f.cb->native->buffer==original&&after.constant_requests-before.constant_requests==101&&after.constant_unchanged==before.constant_unchanged&&after.constant_allocations==before.constant_allocations,"identical full/partial constants retain native allocation while older draws are queued");
 const auto report=acfa::metal::performance_report();check(report.find("commit 0.00 ms/0 calls")!=std::string::npos&&report.find("waitUntilCompleted 0.00 ms/0 calls")!=std::string::npos,"identical partial constants need no global wait");
 f.color(0,1,0);check(f.cb->native->buffer==original,"small changed constants use draw snapshots without allocation");f.c->RSSetScissorRects(1,&right);f.c->Draw(3,0);
 auto pixels=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const size_t k=(y*side+x)*4;check(pixels[k]==(x<side/2?255:0)&&pixels[k+1]==(x<side/2?0:255),"unchanged/changed constant draw ordering");}
 // A queued GPU copy can change a buffer after CPU comparison. Such resources
 // must stay on the original unconditional replacement path.
 Constants green{{0,1,0,1},{.25f,0,0,0}};D3D11_BUFFER_DESC bd{sizeof green,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{&green,0,0};Owned<ID3D11Buffer> src;f.ok(f.d->CreateBuffer(&bd,&data,src.put()),"copy source constants");f.color(1,0,0);f.c->CopyResource(f.cb,src);before=acfa::metal::render_batch_stats(f.c);f.color(1,0,0);after=acfa::metal::render_batch_stats(f.c);check(after.constant_allocations-before.constant_allocations==1&&after.constant_unchanged==before.constant_unchanged,"pending GPU destination copy cannot be hidden by CPU comparison");f.c->RSSetState(f.raster);f.clear();f.c->Draw(3,0);f.uniform({255,0,0,255});
 // A source copy must keep its encoded bytes when that source is updated later.
 f.c->CopyResource(f.cb,src);Constants blue{{0,0,1,1},{0,0,0,0}};
 f.c->UpdateSubresource(src,0,nullptr,&blue,0,0);f.clear();f.c->Draw(3,0);f.uniform({0,255,0,255});
 f.color(1,0,0);
 // Stencil references are dynamic; changing only the reference preserves the
 // memo, changing the immutable state forces a lookup, ClearState resets it.
 before=acfa::metal::render_batch_stats(f.c);f.c->OMSetDepthStencilState(nullptr,7);f.c->Draw(3,0);after=acfa::metal::render_batch_stats(f.c);check(after.depth_reuses-before.depth_reuses==1,"stencil reference alone does not invalidate depth state");
 D3D11_DEPTH_STENCIL_DESC dd{};Owned<ID3D11DepthStencilState> ds;f.ok(f.d->CreateDepthStencilState(&dd,ds.put()),"changed depth state");f.c->OMSetDepthStencilState(ds,0);before=acfa::metal::render_batch_stats(f.c);f.c->Draw(3,0);after=acfa::metal::render_batch_stats(f.c);check(after.depth_reuses==before.depth_reuses,"new depth object invalidates memo");f.uniform({255,0,0,255});
 f.c->ClearState();f.bind();f.color(0,0,1);f.c->Draw(3,0);f.uniform({0,0,255,255});
 std::cout<<"context: identical constants avoid COW/waits; changed constants and pending GPU copies preserve pixels; depth state invalidation passes\n";
}

void resource_binding_changes(){
 Fixture f;f.clear();f.color(1,0,0);f.c->Draw(3,0);
 const auto before=acfa::metal::render_batch_stats(f.c);f.c->Draw(3,0);
 auto after=acfa::metal::render_batch_stats(f.c);
 check(after.resource_binds==before.resource_binds&&after.resource_reuses-before.resource_reuses==64,"unchanged draw preserves every encoder resource binding");
 // A memoized buffer must still be checked for an illegal active CPU map.
 D3D11_MAPPED_SUBRESOURCE map{};f.ok(f.c->Map(f.pos,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&map),"mapped binding validation");
 bool rejected=false;try{f.c->Draw(3,0);}catch(const std::runtime_error&){rejected=true;}f.c->Unmap(f.pos,0);check(rejected,"binding memo cannot hide a mapped-resource error");
 f.uniform({255,0,0,255});
 // Distinct texture/sampler bindings change inside one retained encoder.
 const char* sample=R"(#include <metal_stdlib>
using namespace metal;
struct V {float4 position [[position]];float4 color;float2 uv;};
fragment float4 acfa_ps(V v [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]]) {return t.sample(s,v.uv);}
)";
 Owned<ID3D11PixelShader> ps;f.ok(f.d->CreatePixelShader(sample,std::strlen(sample),nullptr,ps.put()),"binding texture shader");
 Owned<ID3D11Texture2D> red,green;Owned<ID3D11ShaderResourceView> rv,gv;Owned<ID3D11SamplerState> nearest,linear;
 D3D11_TEXTURE2D_DESC td{1,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0};
 const uint8_t colors[2][4]={{255,0,0,255},{0,255,0,255}};D3D11_SUBRESOURCE_DATA init{colors[0],4,4};f.ok(f.d->CreateTexture2D(&td,&init,red.put()),"red binding");init.pSysMem=colors[1];f.ok(f.d->CreateTexture2D(&td,&init,green.put()),"green binding");f.ok(f.d->CreateShaderResourceView(red,nullptr,rv.put()),"red view");f.ok(f.d->CreateShaderResourceView(green,nullptr,gv.put()),"green view");
 D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxAnisotropy=1;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxLOD=D3D11_FLOAT32_MAX;f.ok(f.d->CreateSamplerState(&sd,nearest.put()),"nearest binding");sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;f.ok(f.d->CreateSamplerState(&sd,linear.put()),"linear binding");
 f.clear();f.c->PSSetShader(ps,nullptr,0);f.c->RSSetState(f.scissored);D3D11_RECT left{0,0,side/2,side},right{side/2,0,side,side};ID3D11ShaderResourceView* view=rv;ID3D11SamplerState* sampler=nearest;f.c->PSSetShaderResources(0,1,&view);f.c->PSSetSamplers(0,1,&sampler);f.c->RSSetScissorRects(1,&left);f.c->Draw(3,0);
 auto first=acfa::metal::render_batch_stats(f.c);view=gv;sampler=linear;f.c->PSSetShaderResources(0,1,&view);f.c->PSSetSamplers(0,1,&sampler);f.c->RSSetScissorRects(1,&right);f.c->Draw(3,0);after=acfa::metal::render_batch_stats(f.c);check(after.encoders==first.encoders&&after.resource_binds-first.resource_binds==2,"two changed resources rebind inside the same encoder");
 const auto pixels=f.pixels();for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const size_t k=(y*side+x)*4;check(pixels[k]==(x<side/2?255:0)&&pixels[k+1]==(x<side/2?0:255)&&pixels[k+2]==0&&pixels[k+3]==255,"changed texture binding pixels");}
 f.c->ClearState();f.bind();f.clear();f.color(0,0,1);f.c->Draw(3,0);f.uniform({0,0,255,255});
 std::cout<<"context: encoder resource reuse preserves COW/map checks, texture/sampler changes and ClearState pixels\n";
}

void alternating_pipeline_lookup(){
 Fixture f;f.clear();D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=15;
 Owned<ID3D11BlendState> first,second;f.ok(f.d->CreateBlendState(&bd,first.put()),"first alternating blend");f.ok(f.d->CreateBlendState(&bd,second.put()),"second alternating blend");
 const auto before=acfa::metal::render_batch_stats(f.c);
 for(unsigned i=0;i<120;++i){f.c->OMSetBlendState(i%2?first:second,nullptr,~0u);f.color(i%2?1:0,i%2?0:1,0);f.c->Draw(3,0);}
 const auto after=acfa::metal::render_batch_stats(f.c);
 check(after.pipeline_requests-before.pipeline_requests==120&&after.pipeline_fast_reuses-before.pipeline_fast_reuses==0,"alternating immutable objects exercise full canonical key lookup");
 check(after.pipeline_descriptor_builds-before.pipeline_descriptor_builds==1&&after.pipeline_lookup_hits-before.pipeline_lookup_hits==119,"equivalent alternating states create exactly one native pipeline descriptor");
 f.uniform({255,0,0,255});
 f.c->ClearState();f.bind();f.color(0,0,1);f.c->Draw(3,0);f.uniform({0,0,255,255});
 std::cout<<"context: alternating state objects reuse canonical pipeline without descriptors; ClearState invalidates memo\n";
}

}
int main(){@autoreleasepool{try{constant_content_reuse();resource_binding_changes();alternating_pipeline_lookup();consecutive_draw_batches();drawing_and_queries();disjoint_no_overwrite_ring();sparse_raw_bindings();pipeline_shader_lifetime();blend_and_depth();texture_binding_and_mips();depth_mips_and_rejections();copies_and_maps();std::cout<<"native Metal context headless validation passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
