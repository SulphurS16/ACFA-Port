#pragma once
#include "platform/macos/metal_compat.h"
#import <Metal/Metal.h>
#include <mutex>
#include <unordered_map>
namespace acfa::metal {
// Cache entries retain Metal objects only. Retaining ShaderNative here would
// create a shared_ptr cycle because each shader owns its DeviceNative.
struct ShaderCacheEntry {
 id<MTLLibrary> library; id<MTLFunction> vertex,fragment;
 uint64_t last_use=0; size_t source_bytes=0;
};
struct DeviceNative {
 id<MTLDevice> device; id<MTLCommandQueue> queue;
 std::mutex error_mutex; std::string error;
 std::mutex shader_cache_mutex;
 std::unordered_map<std::string,ShaderCacheEntry> shader_cache;
 uint64_t shader_cache_clock=0,shader_compile_attempts=0,shader_compilations=0;
 uint64_t shader_cache_hits=0,shader_cache_evictions=0;
 size_t shader_cache_source_bytes=0;
};
enum class ResourceKind { Buffer,Texture2D,Texture3D };
struct ResourceNative {
 std::shared_ptr<DeviceNative> device;
 ResourceKind kind=ResourceKind::Buffer;
 D3D11_BUFFER_DESC bd{}; D3D11_TEXTURE2D_DESC td2{}; D3D11_TEXTURE3D_DESC td3{};
 id<MTLBuffer> buffer; id<MTLTexture> texture;
 MTLPixelFormat pixel_format=MTLPixelFormatInvalid;
 // BC formats fall back to RGBA8 when the actual device cannot sample BC. Desc remains original.
 bool bc_fallback=false;
 std::mutex mutex;
 // Sticky guard: CPU contents cannot validate pending GPU buffer copies.
 bool gpu_buffer_write=false;
 uint64_t cpu_revision=0;
 bool mapped=false; UINT mapped_subresource=0; D3D11_MAP map_mode=D3D11_MAP_READ;
 std::vector<uint8_t> map_bytes; UINT map_row_pitch=0,map_depth_pitch=0;
};
struct ViewNative {
 ID3D11Resource* resource=nullptr; // Owns one AddRef. Texture/buffer remain owned by resource too.
 id<MTLTexture> texture;
 D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
 D3D11_RENDER_TARGET_VIEW_DESC rtv{};
 D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
 size_t buffer_offset=0,buffer_size=0;
 ~ViewNative(){if(resource)resource->Release();}
};
struct ShaderNative { std::shared_ptr<DeviceNative> device; id<MTLLibrary> library; id<MTLFunction> function; };
struct SamplerNative { id<MTLSamplerState> state; };
struct DepthNative { id<MTLDepthStencilState> state; };
struct QueryNative {
 std::shared_ptr<DeviceNative> device;
 D3D11_QUERY_DESC desc{};
 id<MTLBuffer> visibility; // Shared 8-byte occlusion result. Context owns begin/end completion semantics.
 std::mutex mutex;
 std::vector<id<MTLBuffer>> samples;
 bool active=false,ended=false;
 id<MTLCommandBuffer> completion;
};
// Implemented by resources; context must finish in-flight commands before CPU writes to any resource.
HRESULT set_error(const std::shared_ptr<DeviceNative>&,HRESULT,const std::string&);
MTLCompareFunction compare_function(D3D11_COMPARISON_FUNC);
// Format information uses original D3D storage layout; BC has 4x4 blocks, otherwise 1x1.
bool format_layout(DXGI_FORMAT,UINT* block_width,UINT* block_height,UINT* bytes_per_block);
// Upload one full/boxed subresource. Box coordinates are texels; row/depth pitch describe source storage.
// CPU-visible textures only; caller serializes against all GPU work. Explicitly rejects depth uploads.
HRESULT upload_texture(ResourceNative&,UINT subresource,const D3D11_BOX*,const void*,UINT row_pitch,UINT depth_pitch);
}
