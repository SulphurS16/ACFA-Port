// Native resource half of the renderer's project-local D3D-shaped vocabulary.
#import <Foundation/Foundation.h>
#include "platform/macos/metal_compat_internal.h"
#include "platform/macos/metal_shader_cache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
namespace acfa::metal {
HRESULT set_error(const std::shared_ptr<DeviceNative>& d,HRESULT hr,const std::string& s) {
 if(d){std::lock_guard lock(d->error_mutex); d->error=s;} return hr;
}
std::string last_error(ID3D11Device* d) { if(!d||!d->native)return "no Metal device";std::lock_guard lock(d->native->error_mutex);return d->native->error; }
HRESULT create_device(ID3D11Device** out,ID3D11DeviceContext** ctx) {
 if(out)*out=nullptr;if(ctx)*ctx=nullptr;if(!out||!ctx)return E_INVALIDARG;
 @autoreleasepool {
 try {
  auto n=std::make_shared<DeviceNative>(); n->device=MTLCreateSystemDefaultDevice();
  if(!n->device)return E_FAIL; n->queue=[n->device newCommandQueue]; if(!n->queue)return E_FAIL;
  auto d=std::make_unique<ID3D11Device>(); d->native=n;
  auto c=std::make_unique<ID3D11DeviceContext>(n); *out=d.release();*ctx=c.release(); return S_OK;
 }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
 }
}
MTLCompareFunction compare_function(D3D11_COMPARISON_FUNC f) {
 switch(f){case D3D11_COMPARISON_LESS:return MTLCompareFunctionLess;case D3D11_COMPARISON_EQUAL:return MTLCompareFunctionEqual;
 case D3D11_COMPARISON_LESS_EQUAL:return MTLCompareFunctionLessEqual;case D3D11_COMPARISON_GREATER:return MTLCompareFunctionGreater;
 case D3D11_COMPARISON_NOT_EQUAL:return MTLCompareFunctionNotEqual;case D3D11_COMPARISON_GREATER_EQUAL:return MTLCompareFunctionGreaterEqual;
 case D3D11_COMPARISON_ALWAYS:return MTLCompareFunctionAlways;default:return MTLCompareFunctionNever;}
}
bool format_layout(DXGI_FORMAT f,UINT* bw,UINT* bh,UINT* bytes) {
 UINT w=1,h=1,b=0;
 switch(f){case DXGI_FORMAT_R32G32B32A32_FLOAT:b=16;break;case DXGI_FORMAT_R16G16B16A16_FLOAT:b=8;break;
 case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_R32_TYPELESS:
 case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R24G8_TYPELESS:
 case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:case DXGI_FORMAT_X24_TYPELESS_G8_UINT:b=4;break;
 case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:b=2;break;
 case DXGI_FORMAT_BC1_UNORM:w=h=4;b=8;break;case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC3_UNORM:w=h=4;b=16;break;
 default:return false;}
 if(bw)*bw=w;if(bh)*bh=h;if(bytes)*bytes=b;return true;
}
namespace {
MTLPixelFormat native_format(DXGI_FORMAT f,UINT bind) {
 switch(f){case DXGI_FORMAT_R32G32B32A32_FLOAT:return MTLPixelFormatRGBA32Float;
 case DXGI_FORMAT_R16G16B16A16_FLOAT:return MTLPixelFormatRGBA16Float;
 case DXGI_FORMAT_R8G8B8A8_UNORM:return MTLPixelFormatRGBA8Unorm;case DXGI_FORMAT_B8G8R8A8_UNORM:return MTLPixelFormatBGRA8Unorm;
 case DXGI_FORMAT_R32_FLOAT:return MTLPixelFormatR32Float;case DXGI_FORMAT_R32_UINT:return MTLPixelFormatR32Uint;
 case DXGI_FORMAT_R16_UINT:return MTLPixelFormatR16Uint;case DXGI_FORMAT_R16_UNORM:return MTLPixelFormatR16Unorm;
 case DXGI_FORMAT_R16_TYPELESS:return bind&D3D11_BIND_DEPTH_STENCIL?MTLPixelFormatDepth16Unorm:MTLPixelFormatR16Unorm;
 case DXGI_FORMAT_D16_UNORM:return MTLPixelFormatDepth16Unorm;case DXGI_FORMAT_D32_FLOAT:return MTLPixelFormatDepth32Float;
 // Apple GPUs store D24S8 in D32FS8. Depth stays normalized float; stencil is an X32_Stencil8 view.
 case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:return MTLPixelFormatDepth32Float_Stencil8;
 case DXGI_FORMAT_BC1_UNORM:return MTLPixelFormatBC1_RGBA;case DXGI_FORMAT_BC2_UNORM:return MTLPixelFormatBC2_RGBA;
 case DXGI_FORMAT_BC3_UNORM:return MTLPixelFormatBC3_RGBA;default:return MTLPixelFormatInvalid;}
}
bool depth_format(MTLPixelFormat f){return f==MTLPixelFormatDepth16Unorm||f==MTLPixelFormatDepth32Float||f==MTLPixelFormatDepth32Float_Stencil8;}
UINT mip_count(UINT w,UINT h,UINT d){UINT n=1;for(UINT x=std::max({w,h,d});x>1;x>>=1)++n;return n;}
bool valid_usage(D3D11_USAGE u,UINT bind,UINT cpu,bool has_initial) {
 if(u<D3D11_USAGE_DEFAULT||u>D3D11_USAGE_STAGING)return false;
 if(u==D3D11_USAGE_IMMUTABLE&&(!has_initial||cpu))return false;
 if(u==D3D11_USAGE_STAGING&&(bind||!cpu))return false;
 if(u==D3D11_USAGE_DYNAMIC&&cpu!=D3D11_CPU_ACCESS_WRITE)return false;
 if(u==D3D11_USAGE_DEFAULT&&cpu)return false;
 return !(cpu&~(D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE));
}
// BC color interpolation exactly follows the UNORM block representation, including BC1 alpha.
void bc_colors(const uint8_t* p,uint8_t c[4][4],bool force_four) {
 UINT a=p[0]|UINT(p[1])<<8,b=p[2]|UINT(p[3])<<8;
 auto unpack=[](UINT n,uint8_t* d){UINT r=(n>>11)&31,g=(n>>5)&63,b=n&31;d[0]=uint8_t((r<<3)|(r>>2));d[1]=uint8_t((g<<2)|(g>>4));d[2]=uint8_t((b<<3)|(b>>2));d[3]=255;};
 unpack(a,c[0]);unpack(b,c[1]);
 for(UINT k=0;k<3;++k){if(a>b||force_four){c[2][k]=uint8_t((2*UINT(c[0][k])+c[1][k])/3);c[3][k]=uint8_t((UINT(c[0][k])+2*c[1][k])/3);}else{c[2][k]=uint8_t((UINT(c[0][k])+c[1][k])/2);c[3][k]=0;}}
 c[2][3]=255;c[3][3]=(a>b||force_four)?255:0;
}
void decode_bc(const uint8_t* src,UINT row,UINT slice,UINT w,UINT h,UINT d,DXGI_FORMAT f,std::vector<uint8_t>& rgba) {
 rgba.resize(size_t(w)*h*d*4);UINT bs=f==DXGI_FORMAT_BC1_UNORM?8:16;
 for(UINT z=0;z<d;++z)for(UINT by=0;by<(h+3)/4;++by)for(UINT bx=0;bx<(w+3)/4;++bx){
  const uint8_t* p=src+size_t(z)*slice+size_t(by)*row+size_t(bx)*bs;const uint8_t* col=p+(bs==16?8:0);
  uint8_t colors[4][4];bc_colors(col,colors,f!=DXGI_FORMAT_BC1_UNORM);UINT ix=col[4]|UINT(col[5])<<8|UINT(col[6])<<16|UINT(col[7])<<24;
  uint8_t alpha[8]={};uint64_t ai=0;
  if(f==DXGI_FORMAT_BC3_UNORM){alpha[0]=p[0];alpha[1]=p[1];
   // Divide the interpolation sum once, avoiding two separately truncated terms.
   if(p[0]>p[1])for(UINT i=1;i<=6;++i)alpha[i+1]=uint8_t(((7-i)*UINT(p[0])+i*UINT(p[1]))/7);
   else{for(UINT i=1;i<=4;++i)alpha[i+1]=uint8_t(((5-i)*UINT(p[0])+i*UINT(p[1]))/5);alpha[6]=0;alpha[7]=255;}
   for(UINT i=0;i<6;++i)ai|=uint64_t(p[2+i])<<(8*i);
  }
  for(UINT y=0;y<4&&by*4+y<h;++y)for(UINT x=0;x<4&&bx*4+x<w;++x){UINT k=y*4+x;uint8_t* o=rgba.data()+((size_t(z)*h+by*4+y)*w+bx*4+x)*4;
   std::memcpy(o,colors[(ix>>(2*k))&3],4);
   if(f==DXGI_FORMAT_BC2_UNORM)o[3]=uint8_t(((p[k/2]>>((k%2)*4))&15)*17);
   if(f==DXGI_FORMAT_BC3_UNORM)o[3]=alpha[(ai>>(3*k))&7];
  }
 }
}
}
HRESULT upload_texture(ResourceNative& r,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice) {
 auto fail=[&](HRESULT hr,const char* s){return set_error(r.device,hr,s);};
 if(!data||!r.texture||r.kind==ResourceKind::Buffer)return fail(E_INVALIDARG,"texture upload: missing data/texture");
 if(r.texture.storageMode==MTLStorageModePrivate||depth_format(r.pixel_format))return fail(E_NOTIMPL,"CPU depth/private texture upload is not implemented");
 bool vol=r.kind==ResourceKind::Texture3D;UINT mips=vol?r.td3.MipLevels:r.td2.MipLevels;
 UINT arrays=vol?1:r.td2.ArraySize;if(sub>=mips*arrays)return fail(E_INVALIDARG,"texture upload: invalid subresource");
 UINT lv=sub%mips,face=sub/mips;UINT w=std::max(1u,(vol?r.td3.Width:r.td2.Width)>>lv),h=std::max(1u,(vol?r.td3.Height:r.td2.Height)>>lv),d=vol?std::max(1u,r.td3.Depth>>lv):1;
 D3D11_BOX b=box?*box:D3D11_BOX{0,0,0,w,h,d};
 if(b.left>=b.right||b.top>=b.bottom||b.front>=b.back||b.right>w||b.bottom>h||b.back>d)return fail(E_INVALIDARG,"texture upload: invalid box");
 UINT bw,bh,bs;DXGI_FORMAT f=vol?r.td3.Format:r.td2.Format;if(!format_layout(f,&bw,&bh,&bs))return fail(E_NOTIMPL,"texture upload: unsupported storage format");
 if(b.left%bw||b.top%bh||(b.right%bw&&b.right!=w)||(b.bottom%bh&&b.bottom!=h))return fail(E_INVALIDARG,"BC update must cover whole blocks or mip edge");
 UINT rw=b.right-b.left,rh=b.bottom-b.top,rd=b.back-b.front;uint64_t minrow=uint64_t((rw+bw-1)/bw)*bs;
 if(!row)row=UINT(minrow);uint64_t minslice=uint64_t(row)*((rh+bh-1)/bh);
 if(!slice&&minslice<=UINT32_MAX)slice=UINT(minslice);
 if(minrow>UINT32_MAX||minslice>UINT32_MAX||row<minrow||slice<minslice)return fail(E_INVALIDARG,"texture upload: insufficient or overflowing source pitch");
 MTLRegion region=MTLRegionMake3D(b.left,b.top,b.front,rw,rh,rd);std::vector<uint8_t> rgba;
 if(r.bc_fallback){if(box)return fail(E_NOTIMPL,"boxed BC fallback updates are not implemented");decode_bc(static_cast<const uint8_t*>(data),row,slice,w,h,d,f,rgba);data=rgba.data();row=w*4;slice=row*h;}
 [r.texture replaceRegion:region mipmapLevel:lv slice:face withBytes:data bytesPerRow:row bytesPerImage:slice];return S_OK;
}
}
using namespace acfa::metal;
void ID3D11Buffer::GetDesc(D3D11_BUFFER_DESC* d){if(d)*d=native->bd;}
void ID3D11Texture2D::GetDesc(D3D11_TEXTURE2D_DESC* d){if(d)*d=native->td2;}
void ID3D11Texture3D::GetDesc(D3D11_TEXTURE3D_DESC* d){if(d)*d=native->td3;}
void ID3D11ShaderResourceView::GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* d){if(d)*d=native->srv;}
namespace {void get_resource(const std::shared_ptr<ViewNative>& n,ID3D11Resource** out){if(out){*out=n?n->resource:nullptr;if(*out)(*out)->AddRef();}}}
void ID3D11ShaderResourceView::GetResource(ID3D11Resource** out){get_resource(native,out);}
void ID3D11RenderTargetView::GetResource(ID3D11Resource** out){get_resource(native,out);}
void ID3D11DepthStencilView::GetResource(ID3D11Resource** out){get_resource(native,out);}
HRESULT ID3D11Device::GetDeviceRemovedReason(){return native&&native->device?S_OK:E_FAIL;}
HRESULT ID3D11Device::CreateBuffer(const D3D11_BUFFER_DESC* d,const D3D11_SUBRESOURCE_DATA* init,ID3D11Buffer** out){
 if(out)*out=nullptr;if(!out||!d||!native)return E_INVALIDARG;
 if(!d->ByteWidth||!valid_usage(d->Usage,d->BindFlags,d->CPUAccessFlags,init&&init->pSysMem)||d->MiscFlags&~D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS||d->StructureByteStride)
  return set_error(native,E_INVALIDARG,"unsupported/invalid buffer descriptor");
 if(d->BindFlags&~(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_VERTEX_BUFFER))return set_error(native,E_NOTIMPL,"unsupported buffer binding");
 if((d->BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&(d->ByteWidth%16||d->BindFlags!=D3D11_BIND_CONSTANT_BUFFER))return set_error(native,E_INVALIDARG,"constant buffer must be 16-byte aligned with exclusive constant binding");
 auto n=std::make_shared<ResourceNative>();n->device=native;n->bd=*d;
 n->buffer=[native->device newBufferWithLength:d->ByteWidth options:MTLResourceStorageModeShared];
 if(!n->buffer)return set_error(native,E_OUTOFMEMORY,"Metal buffer allocation failed");
 if(init&&init->pSysMem)std::memcpy(n->buffer.contents,init->pSysMem,d->ByteWidth);else std::memset(n->buffer.contents,0,d->ByteWidth);
 auto p=new ID3D11Buffer;p->native=std::move(n);*out=p;return S_OK;
}
namespace {
HRESULT texture_create(const std::shared_ptr<DeviceNative>& device,D3D11_TEXTURE2D_DESC td,D3D11_TEXTURE3D_DESC tv,bool vol,const D3D11_SUBRESOURCE_DATA* init,std::shared_ptr<ResourceNative>& result){
 UINT w=vol?tv.Width:td.Width,h=vol?tv.Height:td.Height,d=vol?tv.Depth:1,arrays=vol?1:td.ArraySize;
 UINT mips=vol?tv.MipLevels:td.MipLevels,bind=vol?tv.BindFlags:td.BindFlags,cpu=vol?tv.CPUAccessFlags:td.CPUAccessFlags,misc=vol?tv.MiscFlags:td.MiscFlags;
 D3D11_USAGE usage=vol?tv.Usage:td.Usage;DXGI_FORMAT f=vol?tv.Format:td.Format;
 auto fail=[&](HRESULT hr,const char* s){return set_error(device,hr,s);};
 if(!w||!h||!d||!arrays||w>16384||h>16384||d>2048||arrays>2048||!valid_usage(usage,bind,cpu,init))return fail(E_INVALIDARG,"invalid texture dimensions/usage");
 if(misc&~D3D11_RESOURCE_MISC_TEXTURECUBE)return fail(E_NOTIMPL,"unsupported texture misc flags");
 bool cube=misc&D3D11_RESOURCE_MISC_TEXTURECUBE;if(cube&&(vol||w!=h||arrays%6))return fail(E_INVALIDARG,"cubemap requires square 2D faces and array size divisible by six");
 if(!vol&&(td.SampleDesc.Count!=1||td.SampleDesc.Quality))return fail(E_NOTIMPL,"multisampled textures are not implemented");
 if(bind&~(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL))return fail(E_NOTIMPL,"unsupported texture binding");
 if(!mips)mips=mip_count(w,h,d);if(mips>mip_count(w,h,d))return fail(E_INVALIDARG,"texture mip count exceeds dimensions");
 if(vol)tv.MipLevels=mips;else td.MipLevels=mips;
 MTLPixelFormat pf=native_format(f,bind);if(pf==MTLPixelFormatInvalid)return fail(E_NOTIMPL,"unsupported texture format");
 if((bind&D3D11_BIND_DEPTH_STENCIL)&&!depth_format(pf))return fail(E_INVALIDARG,"depth binding requires depth storage format");
 if(depth_format(pf)&&((bind&D3D11_BIND_RENDER_TARGET)||cpu||init||vol))return fail(E_NOTIMPL,"depth CPU upload/staging/3D texture unsupported");
 UINT bw,bh,bs;format_layout(f,&bw,&bh,&bs);bool compressed=bw==4;
 if(compressed&&(bind&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL)))return fail(E_INVALIDARG,"compressed render target unsupported");
 auto n=std::make_shared<ResourceNative>();n->device=device;n->kind=vol?ResourceKind::Texture3D:ResourceKind::Texture2D;n->td2=td;n->td3=tv;
 n->bc_fallback=compressed&&(!device->device.supportsBCTextureCompression||vol);if(n->bc_fallback)pf=MTLPixelFormatRGBA8Unorm;n->pixel_format=pf;
 MTLTextureDescriptor* mt=[MTLTextureDescriptor new];mt.pixelFormat=pf;mt.width=w;mt.height=h;mt.depth=d;mt.mipmapLevelCount=mips;
 mt.textureType=vol?MTLTextureType3D:cube?(arrays==6?MTLTextureTypeCube:MTLTextureTypeCubeArray):arrays>1?MTLTextureType2DArray:MTLTextureType2D;
 mt.arrayLength=cube?arrays/6:arrays;mt.storageMode=depth_format(pf)?MTLStorageModePrivate:MTLStorageModeShared;mt.sampleCount=1;
 mt.usage=MTLTextureUsagePixelFormatView;if(bind&D3D11_BIND_SHADER_RESOURCE)mt.usage|=MTLTextureUsageShaderRead;
 if(bind&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL))mt.usage|=MTLTextureUsageRenderTarget;
 n->texture=[device->device newTextureWithDescriptor:mt];if(!n->texture)return fail(E_OUTOFMEMORY,"Metal texture allocation failed");
 if(init)for(UINT sub=0;sub<mips*arrays;++sub){if(!init[sub].pSysMem)return fail(E_INVALIDARG,"null initial texture subresource");HRESULT hr=upload_texture(*n,sub,nullptr,init[sub].pSysMem,init[sub].SysMemPitch,init[sub].SysMemSlicePitch);if(FAILED(hr))return hr;}
 result=std::move(n);return S_OK;
}
}
HRESULT ID3D11Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* d,const D3D11_SUBRESOURCE_DATA* init,ID3D11Texture2D** out){
 if(out)*out=nullptr;if(!d||!out||!native)return E_INVALIDARG;std::shared_ptr<ResourceNative> n;
 HRESULT hr=texture_create(native,*d,{},false,init,n);if(FAILED(hr))return hr;auto p=new ID3D11Texture2D;p->native=std::move(n);*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* d,const D3D11_SUBRESOURCE_DATA* init,ID3D11Texture3D** out){
 if(out)*out=nullptr;if(!d||!out||!native)return E_INVALIDARG;std::shared_ptr<ResourceNative> n;
 HRESULT hr=texture_create(native,{},*d,true,init,n);if(FAILED(hr))return hr;auto p=new ID3D11Texture3D;p->native=std::move(n);*out=p;return S_OK;
}
namespace {
std::shared_ptr<ViewNative> view_resource(ID3D11Resource* r){auto n=std::make_shared<ViewNative>();n->resource=r;r->AddRef();return n;}
bool resource_matches(const std::shared_ptr<DeviceNative>& dev,ID3D11Resource* r){return r&&r->native&&r->native->device==dev;}
}
HRESULT ID3D11Device::CreateShaderResourceView(ID3D11Resource* r,const D3D11_SHADER_RESOURCE_VIEW_DESC* input,ID3D11ShaderResourceView** out){
 if(out)*out=nullptr;if(!out||!resource_matches(native,r))return E_INVALIDARG;auto& t=*r->native;
 UINT bind=t.kind==ResourceKind::Buffer?t.bd.BindFlags:t.kind==ResourceKind::Texture2D?t.td2.BindFlags:t.td3.BindFlags;
 if(!(bind&D3D11_BIND_SHADER_RESOURCE))return set_error(native,E_INVALIDARG,"resource is not shader-readable");
 auto v=view_resource(r);D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
 if(input)sd=*input;
 else if(t.kind==ResourceKind::Buffer)return set_error(native,E_INVALIDARG,"raw buffer SRV requires explicit descriptor");
 else{bool vol=t.kind==ResourceKind::Texture3D;sd.Format=vol?t.td3.Format:t.td2.Format;
 sd.ViewDimension=vol?D3D11_SRV_DIMENSION_TEXTURE3D:(t.td2.MiscFlags&D3D11_RESOURCE_MISC_TEXTURECUBE)?D3D11_SRV_DIMENSION_TEXTURECUBE:t.td2.ArraySize>1?D3D11_SRV_DIMENSION_TEXTURE2DARRAY:D3D11_SRV_DIMENSION_TEXTURE2D;
 UINT m=vol?t.td3.MipLevels:t.td2.MipLevels;sd.Texture2D.MipLevels=sd.Texture3D.MipLevels=sd.TextureCube.MipLevels=sd.Texture2DArray.MipLevels=m;sd.Texture2DArray.ArraySize=vol?1:t.td2.ArraySize;
 }
 if(t.kind==ResourceKind::Buffer){if(sd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFEREX||sd.Format!=DXGI_FORMAT_R32_TYPELESS||sd.BufferEx.Flags!=D3D11_BUFFEREX_SRV_FLAG_RAW||!(t.bd.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)||!sd.BufferEx.NumElements||uint64_t(sd.BufferEx.FirstElement+uint64_t(sd.BufferEx.NumElements))*4>t.bd.ByteWidth)return set_error(native,E_INVALIDARG,"invalid raw buffer SRV");
 v->buffer_offset=size_t(sd.BufferEx.FirstElement)*4;v->buffer_size=size_t(sd.BufferEx.NumElements)*4;
 }else{
 bool vol=t.kind==ResourceKind::Texture3D;UINT first=0,count=0,slice=0,slices=vol?1:t.td2.ArraySize;MTLTextureType type=t.texture.textureType;
 switch(sd.ViewDimension){case D3D11_SRV_DIMENSION_TEXTURE2D:if(vol||slices!=1)return set_error(native,E_INVALIDARG,"2D SRV dimension mismatch");first=sd.Texture2D.MostDetailedMip;count=sd.Texture2D.MipLevels;break;
 case D3D11_SRV_DIMENSION_TEXTURE3D:if(!vol)return E_INVALIDARG;first=sd.Texture3D.MostDetailedMip;count=sd.Texture3D.MipLevels;break;
 case D3D11_SRV_DIMENSION_TEXTURECUBE:if(vol||!(t.td2.MiscFlags&D3D11_RESOURCE_MISC_TEXTURECUBE)||slices!=6)return set_error(native,E_NOTIMPL,"cube array SRV is not implemented");first=sd.TextureCube.MostDetailedMip;count=sd.TextureCube.MipLevels;break;
 case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:if(vol||t.texture.textureType!=MTLTextureType2DArray)return E_INVALIDARG;first=sd.Texture2DArray.MostDetailedMip;count=sd.Texture2DArray.MipLevels;slice=sd.Texture2DArray.FirstArraySlice;slices=sd.Texture2DArray.ArraySize;break;
 default:return set_error(native,E_NOTIMPL,"unsupported texture SRV dimension");}
 UINT mips=vol?t.td3.MipLevels:t.td2.MipLevels;if(count==UINT32_MAX)count=mips-first;
 if(first>=mips||!count||uint64_t(first)+count>mips||!slices||(!vol&&uint64_t(slice)+slices>t.td2.ArraySize))return set_error(native,E_INVALIDARG,"SRV mip/slice range exceeds resource");
 MTLPixelFormat pf=t.pixel_format;DXGI_FORMAT original=vol?t.td3.Format:t.td2.Format;
 if(original==DXGI_FORMAT_R16_TYPELESS&&sd.Format==DXGI_FORMAT_R16_UNORM){}
 else if(original==DXGI_FORMAT_R24G8_TYPELESS&&(sd.Format==DXGI_FORMAT_R24_UNORM_X8_TYPELESS||sd.Format==DXGI_FORMAT_X24_TYPELESS_G8_UINT)){if(sd.Format==DXGI_FORMAT_X24_TYPELESS_G8_UINT)pf=MTLPixelFormatX32_Stencil8;}
 else if(sd.Format!=original)return set_error(native,E_NOTIMPL,"unsupported SRV format reinterpretation");
 // Reuse the original texture for full depth views. Stencil needs its explicit uint aspect view.
 if(first==0&&count==mips&&slice==0&&pf==t.pixel_format&&slices==(vol?1:t.td2.ArraySize))v->texture=t.texture;
 else if(pf==MTLPixelFormatX32_Stencil8) {
  // D3D X24_G8 exposes stencil in .y; Metal stencil views normally expose .r.
  auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero,MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleOne);
  v->texture=[t.texture newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(first,count) slices:NSMakeRange(slice,slices) swizzle:swizzle];
 }else v->texture=[t.texture newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(first,count) slices:NSMakeRange(slice,vol?1:slices)];
 if(!v->texture)return set_error(native,E_FAIL,"Metal SRV texture view creation failed");
 }
 v->srv=sd;auto p=new ID3D11ShaderResourceView;p->native=std::move(v);*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateRenderTargetView(ID3D11Resource* r,const D3D11_RENDER_TARGET_VIEW_DESC* input,ID3D11RenderTargetView** out){
 if(out)*out=nullptr;if(!out||!resource_matches(native,r))return E_INVALIDARG;auto& t=*r->native;
 if(t.kind!=ResourceKind::Texture2D||t.td2.ArraySize!=1||!(t.td2.BindFlags&D3D11_BIND_RENDER_TARGET))return set_error(native,E_NOTIMPL,"RTV requires renderable single-slice 2D texture");
 D3D11_RENDER_TARGET_VIEW_DESC d=input?*input:D3D11_RENDER_TARGET_VIEW_DESC{t.td2.Format,D3D11_RTV_DIMENSION_TEXTURE2D,{0}};
 if(d.Format!=t.td2.Format||d.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||d.Texture2D.MipSlice>=t.td2.MipLevels)return set_error(native,E_INVALIDARG,"invalid RTV format/mip");
 auto v=view_resource(r);v->rtv=d;v->texture=t.texture;auto p=new ID3D11RenderTargetView;p->native=std::move(v);*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateDepthStencilView(ID3D11Resource* r,const D3D11_DEPTH_STENCIL_VIEW_DESC* input,ID3D11DepthStencilView** out){
 if(out)*out=nullptr;if(!out||!resource_matches(native,r))return E_INVALIDARG;auto& t=*r->native;
 if(t.kind!=ResourceKind::Texture2D||t.td2.ArraySize!=1||!(t.td2.BindFlags&D3D11_BIND_DEPTH_STENCIL))return set_error(native,E_NOTIMPL,"DSV requires depth single-slice 2D texture");
 DXGI_FORMAT typed=t.td2.Format==DXGI_FORMAT_R16_TYPELESS?DXGI_FORMAT_D16_UNORM:t.td2.Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_D24_UNORM_S8_UINT:t.td2.Format;
 D3D11_DEPTH_STENCIL_VIEW_DESC d=input?*input:D3D11_DEPTH_STENCIL_VIEW_DESC{typed,D3D11_DSV_DIMENSION_TEXTURE2D,0,{0}};
 if(d.Format!=typed||d.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||d.Flags||d.Texture2D.MipSlice>=t.td2.MipLevels)return set_error(native,E_INVALIDARG,"invalid DSV format/mip/flags");
 auto v=view_resource(r);v->dsv=d;v->texture=t.texture;auto p=new ID3D11DepthStencilView;p->native=std::move(v);*out=p;return S_OK;
}
namespace acfa::metal {
ShaderCacheStats shader_cache_stats(ID3D11Device* d) {
 if(!d||!d->native)return {};
 auto& n=*d->native;std::lock_guard lock(n.shader_cache_mutex);
 return {n.shader_compile_attempts,n.shader_compilations,n.shader_cache_hits,
         n.shader_cache_evictions,n.shader_cache.size(),n.shader_cache_source_bytes};
}
}
namespace {
HRESULT shader_create(const std::shared_ptr<DeviceNative>& device,const void* data,size_t bytes,const char* entry,MTLFunctionType type,std::shared_ptr<ShaderNative>& result){
 @autoreleasepool {
 if(!data||!bytes)return set_error(device,E_INVALIDARG,"shader source is empty");
 // Exact byte keys avoid hash collisions and include the compile-option ABI.
 // This cache is device-local and process-local, so an OS/driver change can
 // never reuse an incompatible native object from an older run.
 std::string key;
 if(@available(macOS 15.0,*))key="MSL2.3/math-safe/v1\n";
 else key="MSL2.3/fast-math-off/v1\n";
 key.append(static_cast<const char*>(data),bytes);
 std::lock_guard lock(device->shader_cache_mutex);
 auto found=device->shader_cache.find(key);
 if(found!=device->shader_cache.end()){
  auto& cached=found->second;
  id<MTLFunction> function=type==MTLFunctionTypeVertex?cached.vertex:cached.fragment;
  if(!function){
   function=[cached.library newFunctionWithName:[NSString stringWithUTF8String:entry]];
   if(!function||function.functionType!=type)return set_error(device,E_FAIL,std::string("MSL function missing or wrong stage: ")+entry);
   if(type==MTLFunctionTypeVertex)cached.vertex=function;else cached.fragment=function;
  }
  auto n=std::make_shared<ShaderNative>();n->device=device;n->library=cached.library;n->function=function;
  cached.last_use=++device->shader_cache_clock;++device->shader_cache_hits;
  result=std::move(n);return S_OK;
 }
 NSString* text=[[NSString alloc] initWithBytes:data length:bytes encoding:NSUTF8StringEncoding];
 if(!text)return set_error(device,E_INVALIDARG,"shader bytecode must be MSL UTF-8");
 MTLCompileOptions* options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;
 if(@available(macOS 15.0,*))options.mathMode=MTLMathModeSafe;else{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
 options.fastMathEnabled=NO;
#pragma clang diagnostic pop
 }
 // Compile under the same lock as lookup/insertion: concurrent first users of
 // one source must not race into duplicate expensive Metal compilations.
 ++device->shader_compile_attempts;
 NSError* error=nil;auto n=std::make_shared<ShaderNative>();n->device=device;
 n->library=[device->device newLibraryWithSource:text options:options error:&error];
 if(!n->library)return set_error(device,E_FAIL,"Metal library: "+std::string(error.localizedDescription.UTF8String?:"unknown compilation failure"));
 ++device->shader_compilations;
 n->function=[n->library newFunctionWithName:[NSString stringWithUTF8String:entry]];
 if(!n->function||n->function.functionType!=type)return set_error(device,E_FAIL,std::string("MSL function missing or wrong stage: ")+entry);
 // Failed compilations and invalid requested stages are never inserted. Very
 // large valid modules still work, but bypass retention rather than exceeding
 // the bounded source-key memory budget.
 if(bytes<=acfa::metal::shader_cache_max_source_bytes){
  while(!device->shader_cache.empty()&&
       (device->shader_cache.size()>=acfa::metal::shader_cache_max_entries||
        device->shader_cache_source_bytes>acfa::metal::shader_cache_max_source_bytes-bytes)){
   auto oldest=std::min_element(device->shader_cache.begin(),device->shader_cache.end(),
        [](const auto& a,const auto& b){return a.second.last_use<b.second.last_use;});
   device->shader_cache_source_bytes-=oldest->second.source_bytes;
   device->shader_cache.erase(oldest);++device->shader_cache_evictions;
  }
  ShaderCacheEntry cached;cached.library=n->library;cached.source_bytes=bytes;
  cached.last_use=++device->shader_cache_clock;
  if(type==MTLFunctionTypeVertex)cached.vertex=n->function;else cached.fragment=n->function;
  device->shader_cache.emplace(std::move(key),std::move(cached));
  device->shader_cache_source_bytes+=bytes;
 }
 result=std::move(n);return S_OK;
 }
}
}
HRESULT ID3D11Device::CreateVertexShader(const void* s,size_t n,ID3D11ClassLinkage* linkage,ID3D11VertexShader** out){if(out)*out=nullptr;if(!out||!native)return E_INVALIDARG;if(linkage)return E_NOTIMPL;std::shared_ptr<ShaderNative> v;HRESULT hr=shader_create(native,s,n,"acfa_vs",MTLFunctionTypeVertex,v);if(FAILED(hr))return hr;auto p=new ID3D11VertexShader;p->native=std::move(v);*out=p;return S_OK;}
HRESULT ID3D11Device::CreatePixelShader(const void* s,size_t n,ID3D11ClassLinkage* linkage,ID3D11PixelShader** out){if(out)*out=nullptr;if(!out||!native)return E_INVALIDARG;if(linkage)return E_NOTIMPL;std::shared_ptr<ShaderNative> v;HRESULT hr=shader_create(native,s,n,"acfa_ps",MTLFunctionTypeFragment,v);if(FAILED(hr))return hr;auto p=new ID3D11PixelShader;p->native=std::move(v);*out=p;return S_OK;}
HRESULT ID3D11Device::CreateBlendState(const D3D11_BLEND_DESC* d,ID3D11BlendState** out){
 if(out)*out=nullptr;if(!out||!d)return E_INVALIDARG;
 for(const auto& a:d->RenderTarget){if(a.RenderTargetWriteMask&~15)return set_error(native,E_INVALIDARG,"invalid color write mask");if(a.BlendEnable){auto factor=[](D3D11_BLEND f){return (f>=D3D11_BLEND_ZERO&&f<=D3D11_BLEND_SRC_ALPHA_SAT)||f==D3D11_BLEND_BLEND_FACTOR||f==D3D11_BLEND_INV_BLEND_FACTOR;};if(!factor(a.SrcBlend)||!factor(a.DestBlend)||!factor(a.SrcBlendAlpha)||!factor(a.DestBlendAlpha)||a.BlendOp<D3D11_BLEND_OP_ADD||a.BlendOp>D3D11_BLEND_OP_MAX||a.BlendOpAlpha<D3D11_BLEND_OP_ADD||a.BlendOpAlpha>D3D11_BLEND_OP_MAX)return set_error(native,E_INVALIDARG,"invalid blend descriptor");}}
 // Metal bakes blend into a real render pipeline at Draw, not a standalone state object.
 auto p=new ID3D11BlendState;p->desc=*d;*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* d,ID3D11RasterizerState** out){
 if(out)*out=nullptr;if(!out||!d)return E_INVALIDARG;if((d->FillMode!=D3D11_FILL_SOLID&&d->FillMode!=D3D11_FILL_WIREFRAME)||d->CullMode<D3D11_CULL_NONE||d->CullMode>D3D11_CULL_BACK||!std::isfinite(d->DepthBiasClamp)||!std::isfinite(d->SlopeScaledDepthBias))return set_error(native,E_INVALIDARG,"invalid rasterizer descriptor");
 if(d->MultisampleEnable||d->AntialiasedLineEnable)return set_error(native,E_NOTIMPL,"multisample/antialiased line rasterization is not implemented");
 auto p=new ID3D11RasterizerState;p->desc=*d;*out=p;return S_OK;
}
namespace {
MTLStencilOperation stencil_operation(D3D11_STENCIL_OP x){switch(x){case D3D11_STENCIL_OP_ZERO:return MTLStencilOperationZero;case D3D11_STENCIL_OP_REPLACE:return MTLStencilOperationReplace;case D3D11_STENCIL_OP_INCR_SAT:return MTLStencilOperationIncrementClamp;case D3D11_STENCIL_OP_DECR_SAT:return MTLStencilOperationDecrementClamp;case D3D11_STENCIL_OP_INVERT:return MTLStencilOperationInvert;case D3D11_STENCIL_OP_INCR:return MTLStencilOperationIncrementWrap;case D3D11_STENCIL_OP_DECR:return MTLStencilOperationDecrementWrap;default:return MTLStencilOperationKeep;}}
MTLStencilDescriptor* stencil_desc(const D3D11_DEPTH_STENCILOP_DESC& s,UINT read,UINT write){MTLStencilDescriptor* d=[MTLStencilDescriptor new];d.stencilCompareFunction=compare_function(s.StencilFunc);d.stencilFailureOperation=stencil_operation(s.StencilFailOp);d.depthFailureOperation=stencil_operation(s.StencilDepthFailOp);d.depthStencilPassOperation=stencil_operation(s.StencilPassOp);d.readMask=read;d.writeMask=write;return d;}
MTLSamplerAddressMode address_mode(D3D11_TEXTURE_ADDRESS_MODE m){switch(m){case D3D11_TEXTURE_ADDRESS_WRAP:return MTLSamplerAddressModeRepeat;case D3D11_TEXTURE_ADDRESS_MIRROR:return MTLSamplerAddressModeMirrorRepeat;case D3D11_TEXTURE_ADDRESS_BORDER:return MTLSamplerAddressModeClampToBorderColor;case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE:return MTLSamplerAddressModeMirrorClampToEdge;default:return MTLSamplerAddressModeClampToEdge;}}
}
HRESULT ID3D11Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* d,ID3D11DepthStencilState** out){
 if(out)*out=nullptr;if(!out||!d||!native)return E_INVALIDARG;
 auto cmp=[](D3D11_COMPARISON_FUNC f){return f>=D3D11_COMPARISON_NEVER&&f<=D3D11_COMPARISON_ALWAYS;};
 auto face=[&](const D3D11_DEPTH_STENCILOP_DESC& f){return cmp(f.StencilFunc)&&f.StencilFailOp>=1&&f.StencilFailOp<=8&&f.StencilDepthFailOp>=1&&f.StencilDepthFailOp<=8&&f.StencilPassOp>=1&&f.StencilPassOp<=8;};
 if((d->DepthEnable&&!cmp(d->DepthFunc))||d->DepthWriteMask>D3D11_DEPTH_WRITE_MASK_ALL||(d->StencilEnable&&(!face(d->FrontFace)||!face(d->BackFace))))return set_error(native,E_INVALIDARG,"invalid depth/stencil descriptor");
 MTLDepthStencilDescriptor* md=[MTLDepthStencilDescriptor new];md.depthCompareFunction=d->DepthEnable?compare_function(d->DepthFunc):MTLCompareFunctionAlways;md.depthWriteEnabled=d->DepthEnable&&d->DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
 if(d->StencilEnable){md.frontFaceStencil=stencil_desc(d->FrontFace,d->StencilReadMask,d->StencilWriteMask);md.backFaceStencil=stencil_desc(d->BackFace,d->StencilReadMask,d->StencilWriteMask);}
 auto n=std::make_shared<DepthNative>();n->state=[native->device newDepthStencilStateWithDescriptor:md];if(!n->state)return set_error(native,E_FAIL,"Metal depth stencil state failed");auto p=new ID3D11DepthStencilState;p->native=std::move(n);p->desc=*d;*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateSamplerState(const D3D11_SAMPLER_DESC* d,ID3D11SamplerState** out){
 if(out)*out=nullptr;if(!out||!d||!native)return E_INVALIDARG;
 if(!std::isfinite(d->MipLODBias)||!std::isfinite(d->MinLOD)||!std::isfinite(d->MaxLOD)||d->MinLOD>d->MaxLOD)return set_error(native,E_INVALIDARG,"invalid sampler LOD range");
 if(d->MipLODBias!=0)return set_error(native,E_NOTIMPL,"nonzero sampler MipLODBias requires shader emulation on M1");
 if(d->AddressU<1||d->AddressU>5||d->AddressV<1||d->AddressV>5||d->AddressW<1||d->AddressW>5)return set_error(native,E_INVALIDARG,"invalid sampler address mode");
 UINT f=UINT(d->Filter);if(f&~0xd5u||(f&0x40u&&((f&0x15u)!=0x15u)))return set_error(native,E_NOTIMPL,"unsupported sampler filter encoding");
 bool aniso=f&0x40,comparison=f&0x80; if(aniso&&(d->MaxAnisotropy<1||d->MaxAnisotropy>16))return set_error(native,E_INVALIDARG,"anisotropy must be in [1,16]");
 if(comparison&&(d->ComparisonFunc<1||d->ComparisonFunc>8))return set_error(native,E_INVALIDARG,"invalid sampler comparison function");
 MTLSamplerDescriptor* sd=[MTLSamplerDescriptor new];sd.minFilter=(f&0x10)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;sd.magFilter=(f&4)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
 sd.mipFilter=(f&1)?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;sd.maxAnisotropy=aniso?d->MaxAnisotropy:1;sd.sAddressMode=address_mode(d->AddressU);sd.tAddressMode=address_mode(d->AddressV);sd.rAddressMode=address_mode(d->AddressW);
 sd.lodMinClamp=std::max(0.f,d->MinLOD);sd.lodMaxClamp=std::max(0.f,d->MaxLOD);sd.compareFunction=comparison?compare_function(d->ComparisonFunc):MTLCompareFunctionNever;
 if(d->AddressU==D3D11_TEXTURE_ADDRESS_BORDER||d->AddressV==D3D11_TEXTURE_ADDRESS_BORDER||d->AddressW==D3D11_TEXTURE_ADDRESS_BORDER){
  const FLOAT* c=d->BorderColor;if(c[0]==0&&c[1]==0&&c[2]==0&&c[3]==0)sd.borderColor=MTLSamplerBorderColorTransparentBlack;
  else if(c[0]==0&&c[1]==0&&c[2]==0&&c[3]==1)sd.borderColor=MTLSamplerBorderColorOpaqueBlack;
  else if(c[0]==1&&c[1]==1&&c[2]==1&&c[3]==1)sd.borderColor=MTLSamplerBorderColorOpaqueWhite;
  else return set_error(native,E_NOTIMPL,"arbitrary sampler border color requires shader emulation");
 }
 auto n=std::make_shared<SamplerNative>();n->state=[native->device newSamplerStateWithDescriptor:sd];if(!n->state)return set_error(native,E_FAIL,"Metal sampler state failed");auto p=new ID3D11SamplerState;p->desc=*d;p->native=std::move(n);*out=p;return S_OK;
}
HRESULT ID3D11Device::CreateQuery(const D3D11_QUERY_DESC* d,ID3D11Query** out){
 if(out)*out=nullptr;if(!out||!d||!native)return E_INVALIDARG;if((d->Query!=D3D11_QUERY_OCCLUSION&&d->Query!=D3D11_QUERY_EVENT)||d->MiscFlags)return set_error(native,E_NOTIMPL,"only occlusion/event queries are implemented");
 auto n=std::make_shared<QueryNative>();n->device=native;n->desc=*d;if(d->Query==D3D11_QUERY_OCCLUSION){n->visibility=[native->device newBufferWithLength:8 options:MTLResourceStorageModeShared];if(!n->visibility)return set_error(native,E_OUTOFMEMORY,"query visibility buffer allocation failed");std::memset(n->visibility.contents,0,8);}auto p=new ID3D11Query;p->native=std::move(n);*out=p;return S_OK;
}
