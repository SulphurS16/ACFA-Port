// Project-local RSX API vocabulary. This is not COM, a Windows SDK, or a general D3D implementation.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using UINT64 = uint64_t; using UINT = uint32_t; using UINT8 = uint8_t; using INT = int32_t; using LONG = int32_t;
#ifdef __APPLE__
#include <objc/objc.h>
#else
using BOOL = int32_t;
#endif
using FLOAT = float; using ULONG = uint32_t; using HRESULT = int32_t;
#ifndef TRUE
constexpr BOOL TRUE = 1;
#endif
#ifndef FALSE
constexpr BOOL FALSE = 0;
#endif
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_FAIL = HRESULT(0x80004005u), E_INVALIDARG = HRESULT(0x80070057u),
  E_NOTIMPL = HRESULT(0x80004001u), E_OUTOFMEMORY = HRESULT(0x8007000eu), DXGI_ERROR_WAS_STILL_DRAWING = HRESULT(0x887a000au);
constexpr bool SUCCEEDED(HRESULT h) { return h >= 0; } constexpr bool FAILED(HRESULT h) { return h < 0; }
constexpr FLOAT D3D11_FLOAT32_MAX = std::numeric_limits<float>::max();
enum DXGI_FORMAT : uint32_t { DXGI_FORMAT_UNKNOWN=0, DXGI_FORMAT_R32G32B32A32_FLOAT=2,
  DXGI_FORMAT_R16G16B16A16_FLOAT=10, DXGI_FORMAT_R32_TYPELESS=39, DXGI_FORMAT_D32_FLOAT=40,
  DXGI_FORMAT_R32_FLOAT=41, DXGI_FORMAT_R32_UINT=42, DXGI_FORMAT_R24G8_TYPELESS=44,
  DXGI_FORMAT_D24_UNORM_S8_UINT=45, DXGI_FORMAT_R24_UNORM_X8_TYPELESS=46, DXGI_FORMAT_X24_TYPELESS_G8_UINT=47,
  DXGI_FORMAT_R16_TYPELESS=53, DXGI_FORMAT_D16_UNORM=55, DXGI_FORMAT_R16_UNORM=56, DXGI_FORMAT_R16_UINT=57,
  DXGI_FORMAT_R8G8B8A8_UNORM=28, DXGI_FORMAT_BC1_UNORM=71, DXGI_FORMAT_BC2_UNORM=74,
  DXGI_FORMAT_BC3_UNORM=77, DXGI_FORMAT_B8G8R8A8_UNORM=87 };
enum D3D11_USAGE { D3D11_USAGE_DEFAULT=0, D3D11_USAGE_IMMUTABLE=1, D3D11_USAGE_DYNAMIC=2, D3D11_USAGE_STAGING=3 };
constexpr UINT D3D11_BIND_VERTEX_BUFFER=1, D3D11_BIND_INDEX_BUFFER=2, D3D11_BIND_CONSTANT_BUFFER=4,
 D3D11_BIND_SHADER_RESOURCE=8, D3D11_BIND_RENDER_TARGET=32, D3D11_BIND_DEPTH_STENCIL=64,
 D3D11_CPU_ACCESS_WRITE=0x10000, D3D11_CPU_ACCESS_READ=0x20000,
 D3D11_RESOURCE_MISC_TEXTURECUBE=4, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS=32,
 D3D11_BUFFEREX_SRV_FLAG_RAW=1, D3D11_CLEAR_DEPTH=1, D3D11_CLEAR_STENCIL=2,
 D3D11_ASYNC_GETDATA_DONOTFLUSH=1, D3D11_MAP_FLAG_DO_NOT_WAIT=0x100000;
enum D3D11_MAP { D3D11_MAP_READ=1, D3D11_MAP_WRITE=2, D3D11_MAP_READ_WRITE=3, D3D11_MAP_WRITE_DISCARD=4, D3D11_MAP_WRITE_NO_OVERWRITE=5 };
enum D3D11_PRIMITIVE_TOPOLOGY { D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED=0, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST=1,
 D3D11_PRIMITIVE_TOPOLOGY_LINELIST=2, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST=4 };
enum D3D11_BLEND { D3D11_BLEND_ZERO=1, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
 D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA,
 D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_SRC_ALPHA_SAT,
 D3D11_BLEND_BLEND_FACTOR=14, D3D11_BLEND_INV_BLEND_FACTOR=15 };
enum D3D11_BLEND_OP { D3D11_BLEND_OP_ADD=1,D3D11_BLEND_OP_SUBTRACT,D3D11_BLEND_OP_REV_SUBTRACT,D3D11_BLEND_OP_MIN,D3D11_BLEND_OP_MAX };
enum D3D11_COMPARISON_FUNC { D3D11_COMPARISON_NEVER=1,D3D11_COMPARISON_LESS,D3D11_COMPARISON_EQUAL,
 D3D11_COMPARISON_LESS_EQUAL,D3D11_COMPARISON_GREATER,D3D11_COMPARISON_NOT_EQUAL,D3D11_COMPARISON_GREATER_EQUAL,D3D11_COMPARISON_ALWAYS };
enum D3D11_STENCIL_OP { D3D11_STENCIL_OP_KEEP=1,D3D11_STENCIL_OP_ZERO,D3D11_STENCIL_OP_REPLACE,
 D3D11_STENCIL_OP_INCR_SAT,D3D11_STENCIL_OP_DECR_SAT,D3D11_STENCIL_OP_INVERT,D3D11_STENCIL_OP_INCR,D3D11_STENCIL_OP_DECR };
enum D3D11_DEPTH_WRITE_MASK { D3D11_DEPTH_WRITE_MASK_ZERO=0,D3D11_DEPTH_WRITE_MASK_ALL=1 };
enum D3D11_FILL_MODE { D3D11_FILL_WIREFRAME=2,D3D11_FILL_SOLID=3 };
enum D3D11_CULL_MODE { D3D11_CULL_NONE=1,D3D11_CULL_FRONT,D3D11_CULL_BACK };
enum D3D11_TEXTURE_ADDRESS_MODE { D3D11_TEXTURE_ADDRESS_WRAP=1,D3D11_TEXTURE_ADDRESS_MIRROR,
 D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_BORDER,D3D11_TEXTURE_ADDRESS_MIRROR_ONCE };
enum D3D11_FILTER_TYPE { D3D11_FILTER_TYPE_POINT=0,D3D11_FILTER_TYPE_LINEAR=1 };
enum D3D11_FILTER_REDUCTION_TYPE { D3D11_FILTER_REDUCTION_TYPE_STANDARD=0,D3D11_FILTER_REDUCTION_TYPE_COMPARISON=1 };
constexpr UINT D3D11_ENCODE_BASIC_FILTER(D3D11_FILTER_TYPE mn,D3D11_FILTER_TYPE mg,D3D11_FILTER_TYPE mp,D3D11_FILTER_REDUCTION_TYPE r) { return (UINT(mn)<<4)|(UINT(mg)<<2)|UINT(mp)|(UINT(r)<<7); }
enum D3D11_FILTER { D3D11_FILTER_MIN_MAG_MIP_POINT=0,D3D11_FILTER_MIN_MAG_MIP_LINEAR=0x15,
 D3D11_FILTER_ANISOTROPIC=0x55,D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT=0x80,
 D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT=0x94 };
constexpr UINT8 D3D11_COLOR_WRITE_ENABLE_ALL=15;
enum D3D11_SRV_DIMENSION { D3D11_SRV_DIMENSION_UNKNOWN=0,D3D11_SRV_DIMENSION_TEXTURE2D=4,
 D3D11_SRV_DIMENSION_TEXTURE2DARRAY=5,D3D11_SRV_DIMENSION_TEXTURE3D=8,D3D11_SRV_DIMENSION_TEXTURECUBE=9,D3D11_SRV_DIMENSION_BUFFEREX=11 };
enum D3D11_RTV_DIMENSION { D3D11_RTV_DIMENSION_UNKNOWN=0,D3D11_RTV_DIMENSION_TEXTURE2D=4 };
enum D3D11_DSV_DIMENSION { D3D11_DSV_DIMENSION_UNKNOWN=0,D3D11_DSV_DIMENSION_TEXTURE2D=3 };
enum D3D11_QUERY { D3D11_QUERY_EVENT=0,D3D11_QUERY_OCCLUSION=1 };
struct DXGI_SAMPLE_DESC { UINT Count,Quality; };
struct D3D11_BUFFER_DESC { UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags,CPUAccessFlags,MiscFlags,StructureByteStride; };
struct D3D11_TEXTURE2D_DESC { UINT Width,Height,MipLevels,ArraySize; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc; D3D11_USAGE Usage; UINT BindFlags,CPUAccessFlags,MiscFlags; };
struct D3D11_TEXTURE3D_DESC { UINT Width,Height,Depth,MipLevels; DXGI_FORMAT Format; D3D11_USAGE Usage; UINT BindFlags,CPUAccessFlags,MiscFlags; };
struct D3D11_SUBRESOURCE_DATA { const void* pSysMem; UINT SysMemPitch,SysMemSlicePitch; };
struct D3D11_MAPPED_SUBRESOURCE { void* pData; UINT RowPitch,DepthPitch; };
struct D3D11_BOX { UINT left,top,front,right,bottom,back; };
struct D3D11_RECT { LONG left,top,right,bottom; };
struct D3D11_VIEWPORT { FLOAT TopLeftX,TopLeftY,Width,Height,MinDepth,MaxDepth; };
struct D3D11_RENDER_TARGET_BLEND_DESC { BOOL BlendEnable; D3D11_BLEND SrcBlend,DestBlend; D3D11_BLEND_OP BlendOp;
 D3D11_BLEND SrcBlendAlpha,DestBlendAlpha; D3D11_BLEND_OP BlendOpAlpha; UINT8 RenderTargetWriteMask; };
struct D3D11_BLEND_DESC { BOOL AlphaToCoverageEnable,IndependentBlendEnable; D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8]; };
struct D3D11_DEPTH_STENCILOP_DESC { D3D11_STENCIL_OP StencilFailOp,StencilDepthFailOp,StencilPassOp; D3D11_COMPARISON_FUNC StencilFunc; };
struct D3D11_DEPTH_STENCIL_DESC { BOOL DepthEnable; D3D11_DEPTH_WRITE_MASK DepthWriteMask; D3D11_COMPARISON_FUNC DepthFunc;
 BOOL StencilEnable; UINT8 StencilReadMask,StencilWriteMask; D3D11_DEPTH_STENCILOP_DESC FrontFace,BackFace; };
struct D3D11_RASTERIZER_DESC { D3D11_FILL_MODE FillMode; D3D11_CULL_MODE CullMode; BOOL FrontCounterClockwise;
 INT DepthBias; FLOAT DepthBiasClamp,SlopeScaledDepthBias; BOOL DepthClipEnable,ScissorEnable,MultisampleEnable,AntialiasedLineEnable; };
struct D3D11_SAMPLER_DESC { D3D11_FILTER Filter; D3D11_TEXTURE_ADDRESS_MODE AddressU,AddressV,AddressW;
 FLOAT MipLODBias; UINT MaxAnisotropy; D3D11_COMPARISON_FUNC ComparisonFunc; FLOAT BorderColor[4],MinLOD,MaxLOD; };
struct D3D11_SHADER_RESOURCE_VIEW_DESC { DXGI_FORMAT Format; D3D11_SRV_DIMENSION ViewDimension;
 struct { UINT FirstElement,NumElements,Flags; } BufferEx;
 struct { UINT MostDetailedMip,MipLevels; } Texture2D;
 struct { UINT MostDetailedMip,MipLevels,FirstArraySlice,ArraySize; } Texture2DArray;
 struct { UINT MostDetailedMip,MipLevels; } Texture3D,TextureCube;
};
struct D3D11_RENDER_TARGET_VIEW_DESC { DXGI_FORMAT Format; D3D11_RTV_DIMENSION ViewDimension; struct { UINT MipSlice; } Texture2D; };
struct D3D11_DEPTH_STENCIL_VIEW_DESC { DXGI_FORMAT Format; D3D11_DSV_DIMENSION ViewDimension; UINT Flags; struct { UINT MipSlice; } Texture2D; };
struct D3D11_QUERY_DESC { D3D11_QUERY Query; UINT MiscFlags; };
namespace acfa::metal { struct DeviceNative; struct ContextNative; struct ResourceNative; struct ViewNative; struct ShaderNative; struct SamplerNative; struct DepthNative; struct QueryNative; }
struct IUnknown {
 ULONG AddRef() noexcept { return refs_.fetch_add(1,std::memory_order_relaxed)+1; }
 ULONG Release() noexcept { ULONG n=refs_.fetch_sub(1,std::memory_order_acq_rel)-1; if(!n) delete this; return n; }
 protected: virtual ~IUnknown() = default;
 private: std::atomic<ULONG> refs_{1};
};
struct ID3DBlob : IUnknown { explicit ID3DBlob(std::vector<uint8_t> bytes): bytes_(std::move(bytes)) {} void* GetBufferPointer(){return bytes_.data();} size_t GetBufferSize(){return bytes_.size();} private: std::vector<uint8_t> bytes_; };
struct ID3D11Resource : IUnknown { std::shared_ptr<acfa::metal::ResourceNative> native; };
struct ID3D11Buffer : ID3D11Resource { void GetDesc(D3D11_BUFFER_DESC*); };
struct ID3D11Texture2D : ID3D11Resource { void GetDesc(D3D11_TEXTURE2D_DESC*); };
struct ID3D11Texture3D : ID3D11Resource { void GetDesc(D3D11_TEXTURE3D_DESC*); };
struct ID3D11ShaderResourceView : IUnknown { std::shared_ptr<acfa::metal::ViewNative> native; void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC*); void GetResource(ID3D11Resource**); };
struct ID3D11RenderTargetView : IUnknown { std::shared_ptr<acfa::metal::ViewNative> native; void GetResource(ID3D11Resource**); };
struct ID3D11DepthStencilView : IUnknown { std::shared_ptr<acfa::metal::ViewNative> native; void GetResource(ID3D11Resource**); };
struct ID3D11VertexShader : IUnknown { std::shared_ptr<acfa::metal::ShaderNative> native; };
struct ID3D11PixelShader : IUnknown { std::shared_ptr<acfa::metal::ShaderNative> native; };
struct ID3D11BlendState : IUnknown { D3D11_BLEND_DESC desc{}; };
struct ID3D11RasterizerState : IUnknown { D3D11_RASTERIZER_DESC desc{}; };
struct ID3D11DepthStencilState : IUnknown { std::shared_ptr<acfa::metal::DepthNative> native; D3D11_DEPTH_STENCIL_DESC desc{}; };
struct ID3D11SamplerState : IUnknown { std::shared_ptr<acfa::metal::SamplerNative> native; D3D11_SAMPLER_DESC desc{}; };
struct ID3D11Query : IUnknown { std::shared_ptr<acfa::metal::QueryNative> native; };
struct ID3D11InputLayout : IUnknown {}; struct ID3D11ClassInstance : IUnknown {}; struct ID3D11ClassLinkage : IUnknown {};
struct ID3D11DeviceContext;
struct ID3D11Device : IUnknown {
 std::shared_ptr<acfa::metal::DeviceNative> native;
 HRESULT CreateBuffer(const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer**);
 HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**);
 HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture3D**);
 HRESULT CreateShaderResourceView(ID3D11Resource*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,ID3D11ShaderResourceView**);
 HRESULT CreateRenderTargetView(ID3D11Resource*,const D3D11_RENDER_TARGET_VIEW_DESC*,ID3D11RenderTargetView**);
 HRESULT CreateDepthStencilView(ID3D11Resource*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,ID3D11DepthStencilView**);
 HRESULT CreateVertexShader(const void*,size_t,ID3D11ClassLinkage*,ID3D11VertexShader**);
 HRESULT CreatePixelShader(const void*,size_t,ID3D11ClassLinkage*,ID3D11PixelShader**);
 HRESULT CreateBlendState(const D3D11_BLEND_DESC*,ID3D11BlendState**);
 HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC*,ID3D11DepthStencilState**);
 HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC*,ID3D11RasterizerState**);
 HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC*,ID3D11SamplerState**);
 HRESULT CreateQuery(const D3D11_QUERY_DESC*,ID3D11Query**);
 HRESULT GetDeviceRemovedReason();
};
struct ID3D11DeviceContext : IUnknown {
 std::shared_ptr<acfa::metal::ContextNative> native;
 explicit ID3D11DeviceContext(std::shared_ptr<acfa::metal::DeviceNative>);
 ~ID3D11DeviceContext() override;
 void OMSetRenderTargets(UINT,ID3D11RenderTargetView* const*,ID3D11DepthStencilView*);
 void OMGetRenderTargets(UINT,ID3D11RenderTargetView**,ID3D11DepthStencilView**);
 void OMSetBlendState(ID3D11BlendState*,const FLOAT[4],UINT); void OMGetBlendState(ID3D11BlendState**,FLOAT[4],UINT*);
 void OMSetDepthStencilState(ID3D11DepthStencilState*,UINT); void OMGetDepthStencilState(ID3D11DepthStencilState**,UINT*);
 void RSSetState(ID3D11RasterizerState*); void RSGetState(ID3D11RasterizerState**);
 void RSSetViewports(UINT,const D3D11_VIEWPORT*); void RSGetViewports(UINT*,D3D11_VIEWPORT*);
 void RSSetScissorRects(UINT,const D3D11_RECT*); void RSGetScissorRects(UINT*,D3D11_RECT*);
 void IASetInputLayout(ID3D11InputLayout*); void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY);
 void IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY*); void IASetIndexBuffer(ID3D11Buffer*,DXGI_FORMAT,UINT);
 void VSSetShader(ID3D11VertexShader*,ID3D11ClassInstance* const*,UINT); void VSGetShader(ID3D11VertexShader**,ID3D11ClassInstance**,UINT*);
 void PSSetShader(ID3D11PixelShader*,ID3D11ClassInstance* const*,UINT); void PSGetShader(ID3D11PixelShader**,ID3D11ClassInstance**,UINT*);
 void VSSetConstantBuffers(UINT,UINT,ID3D11Buffer* const*); void PSSetConstantBuffers(UINT,UINT,ID3D11Buffer* const*);
 void VSSetShaderResources(UINT,UINT,ID3D11ShaderResourceView* const*); void PSSetShaderResources(UINT,UINT,ID3D11ShaderResourceView* const*);
 void PSGetShaderResources(UINT,UINT,ID3D11ShaderResourceView**); void PSSetSamplers(UINT,UINT,ID3D11SamplerState* const*); void PSGetSamplers(UINT,UINT,ID3D11SamplerState**);
 void Draw(UINT,UINT); void DrawIndexed(UINT,UINT,INT);
 void ClearRenderTargetView(ID3D11RenderTargetView*,const FLOAT[4]); void ClearDepthStencilView(ID3D11DepthStencilView*,UINT,FLOAT,UINT8);
 void CopyResource(ID3D11Resource*,ID3D11Resource*);
 void CopySubresourceRegion(ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
 void UpdateSubresource(ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
 HRESULT Map(ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*); void Unmap(ID3D11Resource*,UINT);
 void Begin(ID3D11Query*); void End(ID3D11Query*); HRESULT GetData(ID3D11Query*,void*,UINT,UINT);
 void Flush(); void ClearState();
};
namespace acfa::metal {
HRESULT create_device(ID3D11Device**,ID3D11DeviceContext**);
void flush_context(ID3D11DeviceContext*,bool wait);
bool present_to_layer(ID3D11DeviceContext*,ID3D11ShaderResourceView*,void* layer,bool flip_y=false);
bool readback_texture(ID3D11DeviceContext*,ID3D11Texture2D*,std::vector<uint8_t>&,uint32_t&,uint32_t&);
std::string last_error(ID3D11Device*);
// Atomically drains timing observations; never flushes or waits on the GPU.
std::string performance_report();
}
