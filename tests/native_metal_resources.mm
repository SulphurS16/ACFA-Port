// Real, headless Metal resource tests. Synthetic content; no game/window/input/audio startup.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat_internal.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
void ok(HRESULT hr,ID3D11Device* d,const char* s){if(FAILED(hr))throw std::runtime_error(std::string(s)+": "+acfa::metal::last_error(d));}
template<class T>struct Ref{T* p=nullptr;~Ref(){if(p)p->Release();}T** put(){return &p;}T* operator->()const{return p;}operator T*()const{return p;}};
struct Fixture {
 std::vector<std::vector<uint8_t>> data;std::vector<D3D11_SUBRESOURCE_DATA> init;
 Fixture(UINT w,UINT h,UINT d,UINT mips,UINT faces){
  for(UINT f=0;f<faces;++f)for(UINT m=0;m<mips;++m){UINT mw=std::max(1u,w>>m),mh=std::max(1u,h>>m),md=std::max(1u,d>>m),row=mw*4+16,slice=row*mh+32;data.emplace_back(size_t(slice)*md,0xda);
   auto& bytes=data.back();for(UINT z=0;z<md;++z)for(UINT y=0;y<mh;++y)for(UINT x=0;x<mw;++x){size_t o=size_t(z)*slice+size_t(y)*row+x*4;bytes[o]=uint8_t(f*31+m*13+x);bytes[o+1]=uint8_t(y*17);bytes[o+2]=uint8_t(z*29);bytes[o+3]=255;}
   init.push_back({bytes.data(),row,slice});
  }
 }
 void verify(id<MTLTexture> tex,UINT mips,UINT faces,UINT d){
  for(UINT f=0;f<faces;++f)for(UINT m=0;m<mips;++m){UINT w=std::max(1u,UINT(tex.width)>>m),h=std::max(1u,UINT(tex.height)>>m),depth=std::max(1u,d>>m);std::vector<uint8_t> got(size_t(w)*h*depth*4);
   [tex getBytes:got.data() bytesPerRow:w*4 bytesPerImage:w*h*4 fromRegion:MTLRegionMake3D(0,0,0,w,h,depth) mipmapLevel:m slice:f];const auto& expected=init[f*mips+m];
   for(UINT z=0;z<depth;++z)for(UINT y=0;y<h;++y)check(!std::memcmp(got.data()+(size_t(z)*h+y)*w*4,static_cast<const uint8_t*>(expected.pSysMem)+size_t(z)*expected.SysMemSlicePitch+size_t(y)*expected.SysMemPitch,w*4),"mip/face/slice upload pixel mismatch");
  }
 }
};
void lifetimes(ID3D11Device* d){
 std::atomic<int> deleted=0;struct Probe:IUnknown{std::atomic<int>* d;explicit Probe(std::atomic<int>* a):d(a){}~Probe(){++*d;}};auto p=new Probe(&deleted);
 std::array<std::thread,4> threads;for(auto& t:threads)t=std::thread([p]{for(int i=0;i<20000;++i){p->AddRef();p->Release();}});for(auto& t:threads)t.join();check(p->Release()==0&&deleted==1,"atomic lifetime invariant");
 ID3D11Buffer* b=nullptr;D3D11_BUFFER_DESC bd{64,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};ok(d->CreateBuffer(&bd,nullptr,&b),d,"buffer");
 std::weak_ptr<acfa::metal::ResourceNative> weak=b->native;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_TYPELESS;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sd.BufferEx={0,16,D3D11_BUFFEREX_SRV_FLAG_RAW};ID3D11ShaderResourceView* view=nullptr;ok(d->CreateShaderResourceView(b,&sd,&view),d,"raw SRV");ID3D11Resource* retained=nullptr;view->GetResource(&retained);
 check(b->Release()==2,"view/getter must retain resource");check(retained->Release()==1,"GetResource transfers owned reference");view->Release();check(weak.expired(),"view must release resource");
}
void textures(ID3D11Device* d){
 for(UINT faces:{1u,2u,6u}){Fixture f(8,8,1,4,faces);D3D11_TEXTURE2D_DESC td{8,8,4,faces,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,faces==6?D3D11_RESOURCE_MISC_TEXTURECUBE:0};Ref<ID3D11Texture2D> t;ok(d->CreateTexture2D(&td,f.init.data(),t.put()),d,"2D/array/cube texture");f.verify(t->native->texture,4,faces,1);Ref<ID3D11ShaderResourceView> v;ok(d->CreateShaderResourceView(t,nullptr,v.put()),d,"default texture SRV");}
 Fixture f(8,4,4,4,1);D3D11_TEXTURE3D_DESC td{8,4,4,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0};Ref<ID3D11Texture3D> t;ok(d->CreateTexture3D(&td,f.init.data(),t.put()),d,"3D texture");f.verify(t->native->texture,4,1,4);Ref<ID3D11ShaderResourceView> v;ok(d->CreateShaderResourceView(t,nullptr,v.put()),d,"3D SRV");
}
void bc_fallback(ID3D11Device* d){
 for(auto format:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM}){
  // Force the CPU fallback through the common upload helper independent of this Mac's BC support.
  acfa::metal::ResourceNative n;n.device=d->native;n.kind=acfa::metal::ResourceKind::Texture2D;n.td2={4,4,1,1,format,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0};n.bc_fallback=true;n.pixel_format=MTLPixelFormatRGBA8Unorm;
  auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];td.storageMode=MTLStorageModeShared;n.texture=[d->native->device newTextureWithDescriptor:td];
  std::array<uint8_t,16> block{};UINT color=format==DXGI_FORMAT_BC1_UNORM?0:8;block[color]=0;block[color+1]=0xf8; // red RGB565 endpoint 0, selector 0
  if(format==DXGI_FORMAT_BC2_UNORM)for(UINT i=0;i<8;++i)block[i]=0x88; // alpha=136
  if(format==DXGI_FORMAT_BC3_UNORM){block[0]=73;block[1]=17;} // alpha selector 0
  UINT size=format==DXGI_FORMAT_BC1_UNORM?8:16;ok(acfa::metal::upload_texture(n,0,nullptr,block.data(),size,size),d,"BC fallback upload");std::array<uint8_t,64> rgba{};[n.texture getBytes:rgba.data() bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
  for(UINT p=0;p<16;++p){check(rgba[p*4]==255&&rgba[p*4+1]==0&&rgba[p*4+2]==0,"BC fallback RGB");check(rgba[p*4+3]==(format==DXGI_FORMAT_BC1_UNORM?255:format==DXGI_FORMAT_BC2_UNORM?136:73),"BC fallback alpha");}
  D3D11_SUBRESOURCE_DATA init{block.data(),size,size};Ref<ID3D11Texture2D> actual;ok(d->CreateTexture2D(&n.td2,&init,actual.put()),d,"BC actual-device path");check(actual->native->bc_fallback==!bool(d->native->device.supportsBCTextureCompression),"BC capability must choose storage path");
 }
}
void bc_subresources(ID3D11Device* d){
 for(auto format:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM}){
  const UINT bs=format==DXGI_FORMAT_BC1_UNORM?8:16,mips=4,faces=6;
  std::vector<std::vector<uint8_t>> bytes;std::vector<D3D11_SUBRESOURCE_DATA> init;
  for(UINT face=0;face<faces;++face)for(UINT m=0;m<mips;++m){UINT w=std::max(1u,8u>>m),h=w,row=((w+3)/4)*bs+16,slice=row*((h+3)/4)+32;bytes.emplace_back(slice,uint8_t(19+face*31+m*7));init.push_back({bytes.back().data(),row,slice});}
  D3D11_TEXTURE2D_DESC td{8,8,mips,faces,format,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_TEXTURECUBE};Ref<ID3D11Texture2D> t;ok(d->CreateTexture2D(&td,init.data(),t.put()),d,"BC cube mips");
  if(!t->native->bc_fallback)for(UINT face=0;face<faces;++face)for(UINT m=0;m<mips;++m){UINT w=std::max(1u,8u>>m),h=w,row=((w+3)/4)*bs,rows=(h+3)/4;std::vector<uint8_t> out(row*rows);[t->native->texture getBytes:out.data() bytesPerRow:row bytesPerImage:row*rows fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:m slice:face];for(UINT y=0;y<rows;++y)check(!std::memcmp(out.data()+y*row,static_cast<const uint8_t*>(init[face*mips+m].pSysMem)+y*init[face*mips+m].SysMemPitch,row),"BC mip/cube block pitch mismatch");}
 }
}
void typed_formats(ID3D11Device* d){
 for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_B8G8R8A8_UNORM}){
  UINT bw,bh,bs;check(acfa::metal::format_layout(format,&bw,&bh,&bs),"typed layout");UINT row=3*bs+16;std::vector<uint8_t> input(row*2,0xad),out(3*bs*2);for(UINT y=0;y<2;++y)for(UINT x=0;x<3*bs;++x)input[y*row+x]=uint8_t(x+y*71);D3D11_SUBRESOURCE_DATA init{input.data(),row,row*2};D3D11_TEXTURE2D_DESC td{3,2,1,1,format,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0};Ref<ID3D11Texture2D> t;ok(d->CreateTexture2D(&td,&init,t.put()),d,"typed float/BGRA");[t->native->texture getBytes:out.data() bytesPerRow:3*bs fromRegion:MTLRegionMake2D(0,0,3,2) mipmapLevel:0];for(UINT y=0;y<2;++y)check(!std::memcmp(out.data()+y*3*bs,input.data()+y*row,3*bs),"typed texture row pitch");
 }
 Fixture f(8,8,1,4,2);D3D11_TEXTURE2D_DESC td{8,8,4,2,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0};Ref<ID3D11Texture2D> t;ok(d->CreateTexture2D(&td,f.init.data(),t.put()),d,"view range source");D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=td.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;sd.Texture2DArray={1,2,1,1};Ref<ID3D11ShaderResourceView> v;ok(d->CreateShaderResourceView(t,&sd,v.put()),d,"partial mip/slice view");check(v->native->texture.width==4&&v->native->texture.height==4&&v->native->texture.mipmapLevelCount==2&&v->native->texture.arrayLength==1,"SRV must restrict mip/slice range");
 std::array<uint8_t,4*4*4> pixels{};[v->native->texture getBytes:pixels.data() bytesPerRow:16 bytesPerImage:64 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 slice:0];for(UINT y=0;y<4;++y)check(!std::memcmp(pixels.data()+y*16,static_cast<const uint8_t*>(f.init[5].pSysMem)+y*f.init[5].SysMemPitch,16),"SRV selected mip/slice pixels");
}
void stencil_channel(ID3D11Device* d,ID3D11DeviceContext* c){
 D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R24G8_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0};Ref<ID3D11Texture2D> depth;ok(d->CreateTexture2D(&td,nullptr,depth.put()),d,"D24S8 backing");Ref<ID3D11DepthStencilView> dsv;ok(d->CreateDepthStencilView(depth,nullptr,dsv.put()),d,"DSV");D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;Ref<ID3D11ShaderResourceView> srv;ok(d->CreateShaderResourceView(depth,&sd,srv.put()),d,"stencil SRV");
 c->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0.5f,93);acfa::metal::flush_context(c,true);
 const char* source=R"(#include <metal_stdlib>
using namespace metal;
kernel void inspect(texture2d<uint,access::read> stencil [[texture(0)]],device uint4* out [[buffer(0)]],uint2 xy [[thread_position_in_grid]]) { out[xy.y*8+xy.x]=stencil.read(xy); })";
 NSError* error=nil;auto lib=[d->native->device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];check(lib!=nil,"stencil test Metal library");auto fn=[lib newFunctionWithName:@"inspect"];auto pipe=[d->native->device newComputePipelineStateWithFunction:fn error:&error];check(pipe!=nil,"stencil test pipeline");auto out=[d->native->device newBufferWithLength:64*16 options:MTLResourceStorageModeShared];auto cmd=[d->native->queue commandBuffer];auto enc=[cmd computeCommandEncoder];[enc setComputePipelineState:pipe];[enc setTexture:srv->native->texture atIndex:0];[enc setBuffer:out offset:0 atIndex:0];[enc dispatchThreads:MTLSizeMake(8,8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[enc endEncoding];[cmd commit];[cmd waitUntilCompleted];check(cmd.status==MTLCommandBufferStatusCompleted,"stencil test GPU completion");auto pixels=static_cast<const uint32_t*>(out.contents);for(UINT i=0;i<64;++i)check(pixels[i*4+1]==93,"stencil must appear in .y as D3D uint2");
}
void creation_failures(ID3D11Device* d){
 D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Ref<ID3D11SamplerState> sampler;ok(d->CreateSamplerState(&sd,sampler.put()),d,"sampler");sd.MipLODBias=0.25;ID3D11SamplerState* bad=nullptr;check(d->CreateSamplerState(&sd,&bad)==E_NOTIMPL&&!bad,"LOD bias must fail explicitly");sd.MipLODBias=0;sd.AddressU=D3D11_TEXTURE_ADDRESS_BORDER;sd.BorderColor[0]=0.2f;check(d->CreateSamplerState(&sd,&bad)==E_NOTIMPL&&!bad,"arbitrary border must fail explicitly");
 ID3D11PixelShader* ps=nullptr;const char invalid[]="DXBC";check(FAILED(d->CreatePixelShader(invalid,4,nullptr,&ps))&&!ps,"DXBC must not be accepted as MSL");D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION,0};Ref<ID3D11Query> query;ok(d->CreateQuery(&q,query.put()),d,"query");
}
}
int main(){@autoreleasepool{try{Ref<ID3D11Device> d;Ref<ID3D11DeviceContext> c;ok(acfa::metal::create_device(d.put(),c.put()),d,"native device");lifetimes(d);textures(d);typed_formats(d);bc_fallback(d);bc_subresources(d);stencil_channel(d,c);creation_failures(d);std::cout<<"native-metal-resources: PASS (lifetime, mip/array/cube/3D pitches, typed float/BGRA, partial SRV, BC1/2/3 mip/cube native+fallback, stencil .y GPU read, explicit failures) BC_native="<<bool(d->native->device.supportsBCTextureCompression)<<"\n";return 0;}catch(const std::exception& e){std::cerr<<"native-metal-resources: FAIL: "<<e.what()<<"\n";return 1;}}}
