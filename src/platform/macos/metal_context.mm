// Native Metal implementation of the RSX renderer's D3D11-shaped context subset.
// This file never creates NSApplication, a window, an audio device, or game input.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "platform/macos/metal_compat_internal.h"
#include "platform/macos/metal_render_batch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace acfa::metal {
namespace {
// Wall-clock observations only: these counters do not change GPU submission or
// resource synchronization. NO_OVERWRITE Map reports the caller-promised disjoint-write fast path.
struct PerfCounter {
  std::atomic<uint64_t> calls{0}, nanoseconds{0};
};
PerfCounter perf_drawable, perf_commit, perf_wait, perf_no_overwrite, perf_pipeline_build;
std::atomic<uint64_t> perf_pipeline_requests{0},perf_pipeline_fast{0},perf_pipeline_lookup_hits{0};
std::atomic<uint64_t> perf_resource_binds{0},perf_resource_reuses{0};
std::atomic<uint64_t> perf_depth_lookups{0},perf_constant_requests{0},perf_constant_unchanged{0},perf_constant_allocations{0};
std::atomic<uint64_t> perf_draw_encoders{0},perf_draw_calls{0},perf_reused_draws{0},perf_query_draws{0},perf_feedback_draws{0};
class PerfScope {
 public:
  explicit PerfScope(PerfCounter* value):counter(value) {
    if(counter) start=std::chrono::steady_clock::now();
  }
  ~PerfScope() {
    if(!counter)return;
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
    counter->nanoseconds.fetch_add(uint64_t(elapsed),std::memory_order_relaxed);
    counter->calls.fetch_add(1,std::memory_order_relaxed);
  }
 private:
  PerfCounter* counter;
  std::chrono::steady_clock::time_point start;
};
template<class T> void assign(T*& dst,T* src) { if(src)src->AddRef(); if(dst)dst->Release();dst=src; }
template<class T> void output(T* src,T** dst) { if(dst){*dst=src;if(src)src->AddRef();} }
template<class T,size_t N> void set_slots(std::array<T*,N>& a,UINT first,UINT n,T* const* v) {
  for(UINT i=0;i<n&&first+i<N;++i)assign(a[first+i],v?v[i]:nullptr);
}
template<class T,size_t N> void get_slots(const std::array<T*,N>& a,UINT first,UINT n,T** v) {
  if(v)for(UINT i=0;i<n;++i)output(first+i<N?a[first+i]:nullptr,v+i);
}
template<class T,size_t N> void release_slots(std::array<T*,N>& a) {for(auto*& p:a)assign(p,(T*)nullptr);}
std::string ns_error(NSError* e){return e?std::string(e.localizedDescription.UTF8String):"Metal operation failed";}
MTLBlendFactor blend_factor(D3D11_BLEND b) {
  switch(b){
    case D3D11_BLEND_ZERO:return MTLBlendFactorZero;case D3D11_BLEND_ONE:return MTLBlendFactorOne;
    case D3D11_BLEND_SRC_COLOR:return MTLBlendFactorSourceColor;case D3D11_BLEND_INV_SRC_COLOR:return MTLBlendFactorOneMinusSourceColor;
    case D3D11_BLEND_SRC_ALPHA:return MTLBlendFactorSourceAlpha;case D3D11_BLEND_INV_SRC_ALPHA:return MTLBlendFactorOneMinusSourceAlpha;
    case D3D11_BLEND_DEST_ALPHA:return MTLBlendFactorDestinationAlpha;case D3D11_BLEND_INV_DEST_ALPHA:return MTLBlendFactorOneMinusDestinationAlpha;
    case D3D11_BLEND_DEST_COLOR:return MTLBlendFactorDestinationColor;case D3D11_BLEND_INV_DEST_COLOR:return MTLBlendFactorOneMinusDestinationColor;
    case D3D11_BLEND_SRC_ALPHA_SAT:return MTLBlendFactorSourceAlphaSaturated;
    case D3D11_BLEND_BLEND_FACTOR:return MTLBlendFactorBlendColor;case D3D11_BLEND_INV_BLEND_FACTOR:return MTLBlendFactorOneMinusBlendColor;


    default:return MTLBlendFactorOne;
  }
}
MTLBlendOperation blend_op(D3D11_BLEND_OP o){switch(o){case D3D11_BLEND_OP_SUBTRACT:return MTLBlendOperationSubtract;case D3D11_BLEND_OP_REV_SUBTRACT:return MTLBlendOperationReverseSubtract;case D3D11_BLEND_OP_MIN:return MTLBlendOperationMin;case D3D11_BLEND_OP_MAX:return MTLBlendOperationMax;default:return MTLBlendOperationAdd;}}
MTLCompareFunction comparison(D3D11_COMPARISON_FUNC x) {return x>=D3D11_COMPARISON_NEVER&&x<=D3D11_COMPARISON_ALWAYS?MTLCompareFunction(unsigned(x)-1):MTLCompareFunctionAlways;}
MTLStencilOperation stencil_op(D3D11_STENCIL_OP x) {switch(x){case D3D11_STENCIL_OP_ZERO:return MTLStencilOperationZero;case D3D11_STENCIL_OP_REPLACE:return MTLStencilOperationReplace;case D3D11_STENCIL_OP_INCR_SAT:return MTLStencilOperationIncrementClamp;case D3D11_STENCIL_OP_DECR_SAT:return MTLStencilOperationDecrementClamp;case D3D11_STENCIL_OP_INVERT:return MTLStencilOperationInvert;case D3D11_STENCIL_OP_INCR:return MTLStencilOperationIncrementWrap;case D3D11_STENCIL_OP_DECR:return MTLStencilOperationDecrementWrap;default:return MTLStencilOperationKeep;}}
MTLPrimitiveType primitive(D3D11_PRIMITIVE_TOPOLOGY p) {switch(p){case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:return MTLPrimitiveTypePoint;case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:return MTLPrimitiveTypeLine;default:return MTLPrimitiveTypeTriangle;}}
bool supported_topology(D3D11_PRIMITIVE_TOPOLOGY p){return p==D3D11_PRIMITIVE_TOPOLOGY_POINTLIST||p==D3D11_PRIMITIVE_TOPOLOGY_LINELIST||p==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;}
bool has_stencil(MTLPixelFormat f){return f==MTLPixelFormatDepth32Float_Stencil8||f==MTLPixelFormatDepth24Unorm_Stencil8||f==MTLPixelFormatX32_Stencil8||f==MTLPixelFormatX24_Stencil8;}
}

// setBytes snapshots small constant payloads into command-owned storage.
// Any GPU copy use permanently disables CPU in-place updates for that resource.
static bool inline_constant(const ResourceNative& r) {
  return r.kind==ResourceKind::Buffer && r.bd.BindFlags==D3D11_BIND_CONSTANT_BUFFER &&
         r.bd.ByteWidth>0 && r.bd.ByteWidth<=4096 && !r.gpu_buffer_write;
}

struct ContextNative {
  std::shared_ptr<DeviceNative> device;
  std::recursive_mutex mutex;
  id<MTLCommandBuffer> pending=nil,last=nil;
  // Consecutive draws with identical attachments stay inside one tile pass.
  // Texture identity includes its immutable format, sample count and stencil
  // aspect; mip levels are compared separately. No query draws are merged.
  id<MTLRenderCommandEncoder> render_encoder=nil;
  std::array<id<MTLTexture>,8> encoder_colors{};
  std::array<NSUInteger,8> encoder_color_levels{};
  id<MTLTexture> encoder_depth=nil;NSUInteger encoder_depth_level=0;
  RenderBatchStats batch_stats{};
  // Encoder-local native identities, not wrapper identities: DISCARD and COW
  // updates can replace the allocation behind the same public buffer/SRV.
  bool resource_bindings_valid=false;
  std::array<id<MTLBuffer>,14> bound_vcb{},bound_pcb{};
  std::array<uint64_t,14> bound_vrevision{},bound_prevision{};
  std::array<bool,14> bound_vinline{},bound_pinline{};
  std::array<id<MTLBuffer>,4> bound_raw{};
  std::array<NSUInteger,4> bound_raw_offsets{};
  std::array<id<MTLTexture>,16> bound_textures{};
  std::array<id<MTLSamplerState>,16> bound_samplers{};
  // Slang preserves all four arguments of the RSX dynamic raw-fetch switch.
  // D3D's unbound raw SRVs read zero; unused Metal arguments still require a
  // valid allocation. Real views always replace this bounded zero backing.
  id<MTLBuffer> null_raw=nil;
  std::array<ID3D11RenderTargetView*,8> rt{};
  ID3D11DepthStencilView* depth=nullptr;
  ID3D11BlendState* blend=nullptr;
  ID3D11DepthStencilState* depth_state=nullptr;
  ID3D11RasterizerState* raster=nullptr;
  ID3D11InputLayout* layout=nullptr;
  ID3D11VertexShader* vs=nullptr;
  ID3D11PixelShader* ps=nullptr;
  ID3D11Buffer* indices=nullptr;
  DXGI_FORMAT index_format=DXGI_FORMAT_R16_UINT;
  UINT index_offset=0,stencil_ref=0,sample_mask=~0u;
  FLOAT blend_color[4]={1,1,1,1};
  D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  std::vector<D3D11_VIEWPORT> viewports;
  std::vector<D3D11_RECT> scissors;
  std::array<ID3D11Buffer*,14> vcb{},pcb{};
  std::array<ID3D11ShaderResourceView*,16> vsrv{},psrv{};
  std::array<ID3D11SamplerState*,16> samplers{};
  ID3D11Query* active_query=nullptr;
  // A pipeline's compiled code does not guarantee that its MTLFunction ObjC
  // objects remain alive. The pointer-based cache key is valid only while both
  // original function objects are retained by the same cache entry.
  struct PipelineEntry {
    id<MTLRenderPipelineState> pipeline;
    id<MTLFunction> vertex,fragment;
  };
  // Canonical fields only: no padding, locale formatting, or transient view
  // pointer identities. The entry retains shader functions exactly as before.
  struct PipelineKey {
    std::array<uint64_t,79> words{};
    bool operator==(const PipelineKey& other) const {return words==other.words;}
  };
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const noexcept {
      uint64_t h=14695981039346656037ull;
      for(uint64_t word:key.words){h^=word;h*=1099511628211ull;}
      return size_t(h);
    }
  };
  std::unordered_map<PipelineKey,PipelineEntry,PipelineKeyHash> pipelines;
  // Pipeline state objects are immutable. The setters below invalidate this
  // memo only when a baked input changes; constants/scissor/depth tests and
  // blend factors remain dynamic and are rebound by every draw as before.
  id<MTLRenderPipelineState> current_pipeline=nil;
  bool pipeline_dirty=true;
  id<MTLDepthStencilState> current_depth_state=nil;
  bool depth_dirty=true;
  std::unordered_map<std::string,id<MTLDepthStencilState>> depth_states;
  std::unordered_map<unsigned,id<MTLRenderPipelineState>> present_pipelines;
  id<MTLFunction> present_vs=nil,present_ps=nil;
  id<MTLSamplerState> present_sampler=nil;
  explicit ContextNative(std::shared_ptr<DeviceNative> d):device(std::move(d)){
    null_raw=[device->device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if(!null_raw)throw std::bad_alloc();
    std::memset(null_raw.contents,0,256);
  }
  ~ContextNative();
  void fail(const std::string& s);
  id<MTLCommandBuffer> command(){if(!pending)pending=[device->queue commandBuffer];return pending;}
  bool flush(bool wait);
  void end_render();
  bool render_matches();
  bool aliases_render_target(ID3D11ShaderResourceView*);
  bool render_feedback();
  template<class T> id<MTLTexture> texture(T* v){return v&&v->native?v->native->texture:nil;}
  id<MTLDepthStencilState> depth_native();
  id<MTLRenderPipelineState> pipeline();
  void draw(UINT count,UINT start,INT base,bool indexed);
};

void ContextNative::end_render(){
  if(render_encoder){[render_encoder endEncoding];render_encoder=nil;++batch_stats.ends;}
  encoder_colors.fill(nil);encoder_color_levels.fill(0);encoder_depth=nil;encoder_depth_level=0;
  resource_bindings_valid=false;
  bound_vcb.fill(nil);bound_pcb.fill(nil);bound_raw.fill(nil);bound_raw_offsets.fill(0);
  bound_textures.fill(nil);bound_samplers.fill(nil);
}
bool ContextNative::render_matches(){
  if(!render_encoder)return false;
  for(size_t i=0;i<rt.size();++i){auto t=texture(rt[i]);const NSUInteger level=rt[i]?rt[i]->native->rtv.Texture2D.MipSlice:0;if(t!=encoder_colors[i]||level!=encoder_color_levels[i])return false;}
  return texture(depth)==encoder_depth&&(depth?depth->native->dsv.Texture2D.MipSlice:0)==encoder_depth_level;
}
bool ContextNative::aliases_render_target(ID3D11ShaderResourceView* v){
  if(!v||!v->native||!v->native->resource)return false;
  // Views of distinct mips/aspects still refer to one resource. Breaking the
  // pass conservatively preserves read-after-render order across every view.
  auto* r=v->native->resource;
  for(auto* a:rt)if(a&&a->native->resource==r)return true;
  return depth&&depth->native->resource==r;
}
bool ContextNative::render_feedback(){for(auto* v:psrv)if(aliases_render_target(v))return true;return false;}
void ContextNative::fail(const std::string& s){end_render();set_error(device,E_FAIL,s);throw std::runtime_error(s);}
bool ContextNative::flush(bool wait){
  end_render();
  if(pending){last=pending;pending=nil;PerfScope timer(&perf_commit);[last commit];}
  if(wait&&last){{PerfScope timer(&perf_wait);[last waitUntilCompleted];}if(last.status!=MTLCommandBufferStatusCompleted){fail(ns_error(last.error));return false;}}
  return true;
}
ContextNative::~ContextNative(){try{flush(true);}catch(...){/* Destructors cannot propagate GPU failures. */}release_slots(rt);assign(depth,(ID3D11DepthStencilView*)nullptr);assign(blend,(ID3D11BlendState*)nullptr);assign(depth_state,(ID3D11DepthStencilState*)nullptr);assign(raster,(ID3D11RasterizerState*)nullptr);assign(layout,(ID3D11InputLayout*)nullptr);assign(vs,(ID3D11VertexShader*)nullptr);assign(ps,(ID3D11PixelShader*)nullptr);assign(indices,(ID3D11Buffer*)nullptr);assign(active_query,(ID3D11Query*)nullptr);release_slots(vcb);release_slots(pcb);release_slots(vsrv);release_slots(psrv);release_slots(samplers);}

id<MTLDepthStencilState> ContextNative::depth_native(){
  ++batch_stats.depth_requests;
  if(!depth_dirty&&current_depth_state){++batch_stats.depth_reuses;return current_depth_state;}

  perf_depth_lookups.fetch_add(1,std::memory_order_relaxed);
  D3D11_DEPTH_STENCIL_DESC desc{};
  if(depth_state)desc=depth_state->desc;else{desc.DepthEnable=TRUE;desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;desc.DepthFunc=D3D11_COMPARISON_LESS;desc.StencilReadMask=desc.StencilWriteMask=0xff;}
  if(!depth)desc.DepthEnable=FALSE;
  if(!depth||!has_stencil(texture(depth).pixelFormat))desc.StencilEnable=FALSE;
  std::string key(reinterpret_cast<const char*>(&desc),sizeof desc);auto it=depth_states.find(key);if(it!=depth_states.end()){current_depth_state=it->second;depth_dirty=false;return current_depth_state;}
  MTLDepthStencilDescriptor* d=[MTLDepthStencilDescriptor new];
  d.depthCompareFunction=desc.DepthEnable?comparison(desc.DepthFunc):MTLCompareFunctionAlways;d.depthWriteEnabled=desc.DepthEnable&&desc.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
  auto face=[&](const D3D11_DEPTH_STENCILOP_DESC& f){MTLStencilDescriptor* s=[MTLStencilDescriptor new];s.stencilCompareFunction=comparison(f.StencilFunc);s.stencilFailureOperation=stencil_op(f.StencilFailOp);s.depthFailureOperation=stencil_op(f.StencilDepthFailOp);s.depthStencilPassOperation=stencil_op(f.StencilPassOp);s.readMask=desc.StencilReadMask;s.writeMask=desc.StencilWriteMask;return s;};
  if(desc.StencilEnable){d.frontFaceStencil=face(desc.FrontFace);d.backFaceStencil=face(desc.BackFace);}
  id<MTLDepthStencilState> state=[device->device newDepthStencilStateWithDescriptor:d];if(state){depth_states.emplace(std::move(key),state);current_depth_state=state;depth_dirty=false;}return state;
}

id<MTLRenderPipelineState> ContextNative::pipeline(){
  if(!vs||!vs->native->function){fail("draw without vertex shader");return nil;}
  ++batch_stats.pipeline_requests;perf_pipeline_requests.fetch_add(1,std::memory_order_relaxed);
  if(!pipeline_dirty&&current_pipeline){++batch_stats.pipeline_fast_reuses;perf_pipeline_fast.fetch_add(1,std::memory_order_relaxed);return current_pipeline;}
  PipelineKey key;size_t field=0;
  auto add=[&](uint64_t value){key.words[field++]=value;};
  id<MTLFunction> vertex=vs->native->function,fragment=ps?ps->native->function:nil;
  add(uintptr_t((__bridge void*)vertex));add(uintptr_t((__bridge void*)fragment));add(sample_mask);
  D3D11_BLEND_DESC bd{};if(blend)bd=blend->desc;else for(auto& color:bd.RenderTarget)color.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
  std::array<MTLPixelFormat,8> formats{};
  std::array<MTLColorWriteMask,8> masks{};
  NSUInteger samples=1;
  for(unsigned i=0;i<rt.size();++i){
    id<MTLTexture> tex=texture(rt[i]);formats[i]=tex?tex.pixelFormat:MTLPixelFormatInvalid;if(tex)samples=tex.sampleCount;
    const auto& color=bd.RenderTarget[bd.IndependentBlendEnable?i:0];
    masks[i]=MTLColorWriteMask(((color.RenderTargetWriteMask&1)<<3)|((color.RenderTargetWriteMask&2)<<1)|((color.RenderTargetWriteMask&4)>>1)|((color.RenderTargetWriteMask&8)>>3));
    add(formats[i]);add(masks[i]);add(color.BlendEnable);add(color.SrcBlend);add(color.DestBlend);add(color.BlendOp);add(color.SrcBlendAlpha);add(color.DestBlendAlpha);add(color.BlendOpAlpha);
  }
  id<MTLTexture> dt=texture(depth);
  const MTLPixelFormat depth_format=dt?dt.pixelFormat:MTLPixelFormatInvalid;
  const MTLPixelFormat stencil_format=dt&&has_stencil(depth_format)?depth_format:MTLPixelFormatInvalid;
  if(dt)samples=dt.sampleCount;
  add(depth_format);add(stencil_format);add(samples);add(bd.AlphaToCoverageEnable);
  if(field!=key.words.size())fail("internal pipeline key field mismatch");
  auto it=pipelines.find(key);
  if(it!=pipelines.end()){
    ++batch_stats.pipeline_lookup_hits;perf_pipeline_lookup_hits.fetch_add(1,std::memory_order_relaxed);
    current_pipeline=it->second.pipeline;pipeline_dirty=false;return current_pipeline;
  }
  // Build Objective-C descriptors only for a genuinely new pipeline. Cache
  // hits no longer allocate eight attachment descriptors or format text keys.
  if(samples>1&&sample_mask!=~0u){fail("multisample sample masks are not implemented");return nil;}
  ++batch_stats.pipeline_descriptor_builds;
  MTLRenderPipelineDescriptor* d=[MTLRenderPipelineDescriptor new];
  d.vertexFunction=vertex;d.fragmentFunction=fragment;d.rasterSampleCount=samples;
  d.alphaToCoverageEnabled=bd.AlphaToCoverageEnable;d.depthAttachmentPixelFormat=depth_format;d.stencilAttachmentPixelFormat=stencil_format;
  for(unsigned i=0;i<rt.size();++i){
    auto* attachment=d.colorAttachments[i];attachment.pixelFormat=formats[i];attachment.writeMask=masks[i];
    const auto& color=bd.RenderTarget[bd.IndependentBlendEnable?i:0];
    attachment.blendingEnabled=color.BlendEnable;
    attachment.sourceRGBBlendFactor=blend_factor(color.SrcBlend);attachment.destinationRGBBlendFactor=blend_factor(color.DestBlend);attachment.rgbBlendOperation=blend_op(color.BlendOp);
    attachment.sourceAlphaBlendFactor=blend_factor(color.SrcBlendAlpha);attachment.destinationAlphaBlendFactor=blend_factor(color.DestBlendAlpha);attachment.alphaBlendOperation=blend_op(color.BlendOpAlpha);
  }
  NSError* error=nil;id<MTLRenderPipelineState> p=nil;
  {PerfScope timer(&perf_pipeline_build);p=[device->device newRenderPipelineStateWithDescriptor:d error:&error];}
  if(!p){fail(ns_error(error));return nil;}
  pipelines.emplace(key,PipelineEntry{p,vertex,fragment});current_pipeline=p;pipeline_dirty=false;return p;
}

void ContextNative::draw(UINT count,UINT start,INT base,bool indexed){
  if(!count||!(sample_mask&1u)||viewports.empty())return;
  if(layout)fail("vertex input layouts are outside the raw-buffer RSX subset");
  if(!supported_topology(topology))fail("unsupported primitive topology");
  id<MTLRenderPipelineState> p=pipeline();if(!p)return;
  NSUInteger width=0,height=0;bool attachment=false;
  for(unsigned i=0;i<rt.size();++i)if(id<MTLTexture> t=texture(rt[i])){const auto level=rt[i]->native->rtv.Texture2D.MipSlice;width=std::max<NSUInteger>(1,t.width>>level);height=std::max<NSUInteger>(1,t.height>>level);attachment=true;}
  if(id<MTLTexture> t=texture(depth)){const auto level=depth->native->dsv.Texture2D.MipSlice;width=std::max<NSUInteger>(1,t.width>>level);height=std::max<NSUInteger>(1,t.height>>level);attachment=true;}
  if(!attachment)fail("draw without a render target or depth attachment");
  D3D11_RASTERIZER_DESC rd{};if(raster)rd=raster->desc;else{rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_BACK;rd.DepthClipEnable=TRUE;}
  if(rd.ScissorEnable&&scissors.empty())return;
  MTLScissorRect sc={0,0,width,height};
  if(rd.ScissorEnable&&!scissors.empty()){auto r=scissors[0];NSUInteger l=std::min<NSUInteger>(width,std::max<LONG>(0,r.left)),t=std::min<NSUInteger>(height,std::max<LONG>(0,r.top));NSUInteger right=std::min<NSUInteger>(width,std::max<LONG>(0,r.right)),bottom=std::min<NSUInteger>(height,std::max<LONG>(0,r.bottom));if(right<=l||bottom<=t)return;sc={l,t,right-l,bottom-t};}
  if(viewports.size()>1||scissors.size()>1)fail("multiple viewports/scissors are outside the RSX subset");
  // Queries retain the previous one-visibility-buffer-per-draw semantics.
  // Feedback/alias reads also retain explicit pass boundaries. Only ordinary
  // consecutive draws to exactly matching attachments may share an encoder.
  const bool feedback=render_feedback();
  const bool mergeable=!active_query&&!feedback;
  if(render_encoder&&(!mergeable||!render_matches()))end_render();
  const bool reused=render_encoder!=nil;
  id<MTLBuffer> visibility=nil;
  if(!render_encoder){
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    for(unsigned i=0;i<rt.size();++i)if(id<MTLTexture> t=texture(rt[i])){auto* a=pass.colorAttachments[i];a.texture=t;a.level=rt[i]->native->rtv.Texture2D.MipSlice;a.loadAction=MTLLoadActionLoad;a.storeAction=MTLStoreActionStore;encoder_colors[i]=t;encoder_color_levels[i]=a.level;}
    if(id<MTLTexture> t=texture(depth)){pass.depthAttachment.texture=t;pass.depthAttachment.level=depth->native->dsv.Texture2D.MipSlice;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;if(has_stencil(t.pixelFormat)){pass.stencilAttachment.texture=t;pass.stencilAttachment.level=depth->native->dsv.Texture2D.MipSlice;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}encoder_depth=t;encoder_depth_level=pass.depthAttachment.level;}
    if(active_query){visibility=[device->device newBufferWithLength:8 options:MTLResourceStorageModeShared];if(!visibility)fail("visibility buffer allocation failed");std::memset(visibility.contents,0,8);pass.visibilityResultBuffer=visibility;active_query->native->samples.push_back(visibility);}
    render_encoder=[command() renderCommandEncoderWithDescriptor:pass];if(!render_encoder)fail("Metal render encoder creation failed");
    ++batch_stats.encoders;perf_draw_encoders.fetch_add(1,std::memory_order_relaxed);
  }
  auto e=render_encoder;
  // Dynamic raster/depth state is still applied per draw. Resources below
  // reuse an encoder binding only when native allocation and offset match.
  [e setRenderPipelineState:p];[e setDepthStencilState:depth_native()];[e setStencilReferenceValue:stencil_ref];[e setBlendColorRed:blend_color[0] green:blend_color[1] blue:blend_color[2] alpha:blend_color[3]];
  [e setCullMode:rd.CullMode==D3D11_CULL_NONE?MTLCullModeNone:rd.CullMode==D3D11_CULL_FRONT?MTLCullModeFront:MTLCullModeBack];[e setFrontFacingWinding:rd.FrontCounterClockwise?MTLWindingCounterClockwise:MTLWindingClockwise];
  [e setTriangleFillMode:rd.FillMode==D3D11_FILL_WIREFRAME?MTLTriangleFillModeLines:MTLTriangleFillModeFill];[e setDepthClipMode:rd.DepthClipEnable?MTLDepthClipModeClip:MTLDepthClipModeClamp];[e setDepthBias:float(rd.DepthBias) slopeScale:rd.SlopeScaledDepthBias clamp:rd.DepthBiasClamp];[e setScissorRect:sc];
  if(viewports.empty())[e setViewport:MTLViewport{0,0,double(width),double(height),0,1}];else{auto v=viewports[0];[e setViewport:MTLViewport{v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth}];}
  auto check=[&](ID3D11Resource* r){if(r&&r->native->mapped)fail("draw uses a currently mapped resource");};
  uint64_t emitted=0,reused_bindings=0;
  const bool first_bind=!resource_bindings_valid;
  for(unsigned i=0;i<vcb.size();++i){
    check(vcb[i]);check(pcb[i]);
    id<MTLBuffer> vertex=vcb[i]?vcb[i]->native->buffer:nil;
    id<MTLBuffer> fragment=pcb[i]?pcb[i]->native->buffer:nil;
    if(vcb[i]&&inline_constant(*vcb[i]->native)){
      auto revision=vcb[i]->native->cpu_revision;
      if(first_bind||!bound_vinline[i]||bound_vcb[i]!=vertex||bound_vrevision[i]!=revision){[e setVertexBytes:vertex.contents length:vcb[i]->native->bd.ByteWidth atIndex:i];bound_vcb[i]=vertex;bound_vrevision[i]=revision;++emitted;}else ++reused_bindings;
      bound_vinline[i]=true;
    }else{if(first_bind||bound_vinline[i]||bound_vcb[i]!=vertex){[e setVertexBuffer:vertex offset:0 atIndex:i];bound_vcb[i]=vertex;++emitted;}else ++reused_bindings;bound_vinline[i]=false;}
    if(pcb[i]&&inline_constant(*pcb[i]->native)){
      auto revision=pcb[i]->native->cpu_revision;
      if(first_bind||!bound_pinline[i]||bound_pcb[i]!=fragment||bound_prevision[i]!=revision){[e setFragmentBytes:fragment.contents length:pcb[i]->native->bd.ByteWidth atIndex:i];bound_pcb[i]=fragment;bound_prevision[i]=revision;++emitted;}else ++reused_bindings;
      bound_pinline[i]=true;
    }else{if(first_bind||bound_pinline[i]||bound_pcb[i]!=fragment){[e setFragmentBuffer:fragment offset:0 atIndex:i];bound_pcb[i]=fragment;++emitted;}else ++reused_bindings;bound_pinline[i]=false;}
  }
  for(unsigned i=0;i<4;++i){
    auto* v=vsrv[i];id<MTLBuffer> raw=null_raw;NSUInteger offset=0;
    if(v){check(v->native->resource);if(v->native->resource->native->kind!=ResourceKind::Buffer)fail("VS SRVs must be raw buffers in slots 0..3");raw=v->native->resource->native->buffer;offset=v->native->buffer_offset;}
    if(first_bind||bound_raw[i]!=raw||bound_raw_offsets[i]!=offset){[e setVertexBuffer:raw offset:offset atIndex:16+i];bound_raw[i]=raw;bound_raw_offsets[i]=offset;++emitted;}else ++reused_bindings;
  }
  for(unsigned i=0;i<psrv.size();++i){
    auto* v=psrv[i];if(v){check(v->native->resource);if(v->native->resource->native->kind==ResourceKind::Buffer)fail("PS buffer SRVs are outside the texture RSX subset");}
    id<MTLTexture> tex=v?v->native->texture:nil;
    id<MTLSamplerState> sampler=samplers[i]?samplers[i]->native->state:nil;
    if(first_bind||bound_textures[i]!=tex){[e setFragmentTexture:tex atIndex:i];bound_textures[i]=tex;++emitted;}else ++reused_bindings;
    if(first_bind||bound_samplers[i]!=sampler){[e setFragmentSamplerState:sampler atIndex:i];bound_samplers[i]=sampler;++emitted;}else ++reused_bindings;
  }
  resource_bindings_valid=true;
  batch_stats.resource_binds+=emitted;batch_stats.resource_reuses+=reused_bindings;
  perf_resource_binds.fetch_add(emitted,std::memory_order_relaxed);
  perf_resource_reuses.fetch_add(reused_bindings,std::memory_order_relaxed);
  if(visibility)[e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];
  if(indexed){if(!indices)fail("DrawIndexed without index buffer");check(indices);NSUInteger element=index_format==DXGI_FORMAT_R16_UINT?2:4;NSUInteger offset=NSUInteger(index_offset)+NSUInteger(start)*element;if(offset>indices->native->bd.ByteWidth||NSUInteger(count)>(indices->native->bd.ByteWidth-offset)/element)fail("indexed draw exceeds index buffer");[e drawIndexedPrimitives:primitive(topology) indexCount:count indexType:element==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:indices->native->buffer indexBufferOffset:offset instanceCount:1 baseVertex:base baseInstance:0];}
  else [e drawPrimitives:primitive(topology) vertexStart:start vertexCount:count];
  ++batch_stats.draws;perf_draw_calls.fetch_add(1,std::memory_order_relaxed);
  if(active_query){++batch_stats.query_draws;perf_query_draws.fetch_add(1,std::memory_order_relaxed);}
  if(feedback){++batch_stats.feedback_draws;perf_feedback_draws.fetch_add(1,std::memory_order_relaxed);}
  if(reused){++batch_stats.reused;perf_reused_draws.fetch_add(1,std::memory_order_relaxed);}
  if(!mergeable)end_render();
}
} // namespace acfa::metal

using namespace acfa::metal;
ID3D11DeviceContext::ID3D11DeviceContext(std::shared_ptr<DeviceNative> d):native(std::make_shared<ContextNative>(std::move(d))){}
ID3D11DeviceContext::~ID3D11DeviceContext()=default;
// Every public context call has a local pool as well as the recorder's chunk
// pool: synchronous FIFO calls do not pass through the submission worker. Strong
// native fields retain pending commands/resources beyond the temporary scope.
#define LOCK_CONTEXT std::lock_guard lock(native->mutex)
void ID3D11DeviceContext::OMSetRenderTargets(UINT n,ID3D11RenderTargetView* const* v,ID3D11DepthStencilView* d){@autoreleasepool {LOCK_CONTEXT;if(n>8)native->fail("too many render targets");native->end_render();
 if(native->depth!=d){native->pipeline_dirty=true;native->depth_dirty=true;}
 for(UINT i=0;i<8;++i){auto* next=i<n&&v?v[i]:nullptr;if(native->rt[i]!=next)native->pipeline_dirty=true;assign(native->rt[i],next);}assign(native->depth,d);}}
void ID3D11DeviceContext::OMGetRenderTargets(UINT n,ID3D11RenderTargetView** v,ID3D11DepthStencilView** d){@autoreleasepool {LOCK_CONTEXT;get_slots(native->rt,0,n,v);output(native->depth,d);}}
void ID3D11DeviceContext::OMSetBlendState(ID3D11BlendState* s,const FLOAT c[4],UINT m){@autoreleasepool {LOCK_CONTEXT;if(native->blend!=s||native->sample_mask!=m)native->pipeline_dirty=true;assign(native->blend,s);for(unsigned i=0;i<4;++i)native->blend_color[i]=c?c[i]:1;native->sample_mask=m;}}
void ID3D11DeviceContext::OMGetBlendState(ID3D11BlendState** s,FLOAT c[4],UINT* m){@autoreleasepool {LOCK_CONTEXT;output(native->blend,s);if(c)std::memcpy(c,native->blend_color,16);if(m)*m=native->sample_mask;}}
void ID3D11DeviceContext::OMSetDepthStencilState(ID3D11DepthStencilState* s,UINT r){@autoreleasepool {LOCK_CONTEXT;if(native->depth_state!=s)native->depth_dirty=true;assign(native->depth_state,s);native->stencil_ref=r;}}
void ID3D11DeviceContext::OMGetDepthStencilState(ID3D11DepthStencilState** s,UINT* r){@autoreleasepool {LOCK_CONTEXT;output(native->depth_state,s);if(r)*r=native->stencil_ref;}}
void ID3D11DeviceContext::RSSetState(ID3D11RasterizerState* s){@autoreleasepool {LOCK_CONTEXT;assign(native->raster,s);}}
void ID3D11DeviceContext::RSGetState(ID3D11RasterizerState** s){@autoreleasepool {LOCK_CONTEXT;output(native->raster,s);}}
void ID3D11DeviceContext::RSSetViewports(UINT n,const D3D11_VIEWPORT* v){@autoreleasepool {LOCK_CONTEXT;if(n>16||(n&&!v))native->fail("invalid viewport array");native->viewports.clear();if(n)native->viewports.assign(v,v+n);}}
void ID3D11DeviceContext::RSGetViewports(UINT* n,D3D11_VIEWPORT* v){@autoreleasepool {LOCK_CONTEXT;if(!n)return;UINT available=UINT(native->viewports.size());if(v)std::copy_n(native->viewports.begin(),std::min(*n,available),v);*n=available;}}
void ID3D11DeviceContext::RSSetScissorRects(UINT n,const D3D11_RECT* v){@autoreleasepool {LOCK_CONTEXT;if(n>16||(n&&!v))native->fail("invalid scissor array");native->scissors.clear();if(n)native->scissors.assign(v,v+n);}}
void ID3D11DeviceContext::RSGetScissorRects(UINT* n,D3D11_RECT* v){@autoreleasepool {LOCK_CONTEXT;if(!n)return;UINT available=UINT(native->scissors.size());if(v)std::copy_n(native->scissors.begin(),std::min(*n,available),v);*n=available;}}
void ID3D11DeviceContext::IASetInputLayout(ID3D11InputLayout* p){@autoreleasepool {LOCK_CONTEXT;if(p)native->fail("input layouts are not used by this renderer");assign(native->layout,p);}}
void ID3D11DeviceContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t){@autoreleasepool {LOCK_CONTEXT;native->topology=t;}}
void ID3D11DeviceContext::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* t){@autoreleasepool {LOCK_CONTEXT;if(t)*t=native->topology;}}
void ID3D11DeviceContext::IASetIndexBuffer(ID3D11Buffer* b,DXGI_FORMAT f,UINT o){@autoreleasepool {LOCK_CONTEXT;if(b&&(f!=DXGI_FORMAT_R16_UINT&&f!=DXGI_FORMAT_R32_UINT))native->fail("invalid index format");if(b&&o%(f==DXGI_FORMAT_R16_UINT?2:4))native->fail("misaligned index buffer offset");assign(native->indices,b);native->index_format=f;native->index_offset=o;}}
void ID3D11DeviceContext::VSSetShader(ID3D11VertexShader* s,ID3D11ClassInstance* const*,UINT n){@autoreleasepool {LOCK_CONTEXT;if(n)native->fail("shader class instances are unsupported");if(native->vs!=s)native->pipeline_dirty=true;assign(native->vs,s);}}
void ID3D11DeviceContext::PSSetShader(ID3D11PixelShader* s,ID3D11ClassInstance* const*,UINT n){@autoreleasepool {LOCK_CONTEXT;if(n)native->fail("shader class instances are unsupported");if(native->ps!=s)native->pipeline_dirty=true;assign(native->ps,s);}}
void ID3D11DeviceContext::VSGetShader(ID3D11VertexShader** s,ID3D11ClassInstance**,UINT* n){@autoreleasepool {LOCK_CONTEXT;output(native->vs,s);if(n)*n=0;}}
void ID3D11DeviceContext::PSGetShader(ID3D11PixelShader** s,ID3D11ClassInstance**,UINT* n){@autoreleasepool {LOCK_CONTEXT;output(native->ps,s);if(n)*n=0;}}
void ID3D11DeviceContext::VSSetConstantBuffers(UINT f,UINT n,ID3D11Buffer* const* b){@autoreleasepool {LOCK_CONTEXT;if(f>14||n>14-f)native->fail("VS constant slot exceeds 14");set_slots(native->vcb,f,n,b);}}
void ID3D11DeviceContext::PSSetConstantBuffers(UINT f,UINT n,ID3D11Buffer* const* b){@autoreleasepool {LOCK_CONTEXT;if(f>14||n>14-f)native->fail("PS constant slot exceeds 14");set_slots(native->pcb,f,n,b);}}
void ID3D11DeviceContext::VSSetShaderResources(UINT f,UINT n,ID3D11ShaderResourceView* const* b){@autoreleasepool {LOCK_CONTEXT;if(f>4||n>4-f)native->fail("VS raw slot exceeds 4");set_slots(native->vsrv,f,n,b);}}
void ID3D11DeviceContext::PSSetShaderResources(UINT f,UINT n,ID3D11ShaderResourceView* const* b){@autoreleasepool {LOCK_CONTEXT;if(f>16||n>16-f)native->fail("PS resource slot exceeds 16");for(UINT i=0;i<n;++i)if(b&&native->aliases_render_target(b[i])){native->end_render();break;}set_slots(native->psrv,f,n,b);}}
void ID3D11DeviceContext::PSGetShaderResources(UINT f,UINT n,ID3D11ShaderResourceView** b){@autoreleasepool {LOCK_CONTEXT;get_slots(native->psrv,f,n,b);}}
void ID3D11DeviceContext::PSSetSamplers(UINT f,UINT n,ID3D11SamplerState* const* s){@autoreleasepool {LOCK_CONTEXT;if(f>16||n>16-f)native->fail("sampler slot exceeds 16");set_slots(native->samplers,f,n,s);}}
void ID3D11DeviceContext::PSGetSamplers(UINT f,UINT n,ID3D11SamplerState** s){@autoreleasepool {LOCK_CONTEXT;get_slots(native->samplers,f,n,s);}}
void ID3D11DeviceContext::Draw(UINT n,UINT s){@autoreleasepool {LOCK_CONTEXT;native->draw(n,s,0,false);}}
void ID3D11DeviceContext::DrawIndexed(UINT n,UINT s,INT b){@autoreleasepool {LOCK_CONTEXT;native->draw(n,s,b,true);}}
void ID3D11DeviceContext::ClearRenderTargetView(ID3D11RenderTargetView* v,const FLOAT c[4]){@autoreleasepool {LOCK_CONTEXT;if(!v||!c)native->fail("invalid color clear");native->end_render();auto p=[MTLRenderPassDescriptor renderPassDescriptor];p.colorAttachments[0].texture=v->native->texture;p.colorAttachments[0].level=v->native->rtv.Texture2D.MipSlice;p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;p.colorAttachments[0].clearColor=MTLClearColorMake(c[0],c[1],c[2],c[3]);id<MTLRenderCommandEncoder> e=[native->command() renderCommandEncoderWithDescriptor:p];if(!e)native->fail("color clear encoder failed");[e endEncoding];}}
void ID3D11DeviceContext::ClearDepthStencilView(ID3D11DepthStencilView* v,UINT flags,FLOAT z,UINT8 s){@autoreleasepool {LOCK_CONTEXT;if(!v||(flags&~3u))native->fail("invalid depth/stencil clear");if(!flags)return;native->end_render();auto p=[MTLRenderPassDescriptor renderPassDescriptor];auto t=v->native->texture;p.depthAttachment.texture=t;p.depthAttachment.level=v->native->dsv.Texture2D.MipSlice;p.depthAttachment.loadAction=flags&D3D11_CLEAR_DEPTH?MTLLoadActionClear:MTLLoadActionLoad;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=z;if(has_stencil(t.pixelFormat)){p.stencilAttachment.texture=t;p.stencilAttachment.level=v->native->dsv.Texture2D.MipSlice;p.stencilAttachment.loadAction=flags&D3D11_CLEAR_STENCIL?MTLLoadActionClear:MTLLoadActionLoad;p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=s;}id<MTLRenderCommandEncoder> e=[native->command() renderCommandEncoderWithDescriptor:p];if(!e)native->fail("depth clear encoder failed");[e endEncoding];}}

namespace {
struct Subresource { UINT level=0,slice=0,width=0,height=0,depth=0; };
bool subresource(ResourceNative& r,UINT sub,Subresource& s){if(r.kind==ResourceKind::Buffer)return sub==0;auto t=r.texture;if(!t)return false;UINT m=UINT(t.mipmapLevelCount);s.level=sub%m;s.slice=sub/m;if(r.kind==ResourceKind::Texture3D&&s.slice)return false;if(r.kind!=ResourceKind::Texture3D&&s.slice>=t.arrayLength*((t.textureType==MTLTextureTypeCube||t.textureType==MTLTextureTypeCubeArray)?6:1))return false;s.width=std::max<UINT>(1,UINT(t.width)>>s.level);s.height=std::max<UINT>(1,UINT(t.height)>>s.level);s.depth=r.kind==ResourceKind::Texture3D?std::max<UINT>(1,UINT(t.depth)>>s.level):1;return true;}
bool finished(ContextNative& c,UINT flags){if(c.pending){c.flush(false);}if(!c.last)return true;if((flags&D3D11_MAP_FLAG_DO_NOT_WAIT)&&c.last.status!=MTLCommandBufferStatusCompleted)return false;return c.flush(true);}
}
void ID3D11DeviceContext::CopySubresourceRegion(ID3D11Resource* dst,UINT dsub,UINT dx,UINT dy,UINT dz,ID3D11Resource* src,UINT ssub,const D3D11_BOX* box){@autoreleasepool {
  LOCK_CONTEXT;if(!dst||!src||dst==src)native->fail("invalid or overlapping copy resources");auto& d=*dst->native;auto& s=*src->native;D3D11_USAGE usage=d.kind==ResourceKind::Buffer?d.bd.Usage:d.kind==ResourceKind::Texture2D?d.td2.Usage:d.td3.Usage;if(usage==D3D11_USAGE_IMMUTABLE)native->fail("copy destination is immutable");if(d.mapped||s.mapped)native->fail("copy uses a mapped resource");
  if(d.kind==ResourceKind::Buffer||s.kind==ResourceKind::Buffer){if(d.kind!=s.kind||dsub||ssub||dy||dz)native->fail("invalid buffer copy");UINT first=box?box->left:0,end=box?box->right:s.bd.ByteWidth;if(first>end||end>s.bd.ByteWidth||dx>d.bd.ByteWidth||end-first>d.bd.ByteWidth-dx)native->fail("buffer copy exceeds allocation");if(end==first)return;d.gpu_buffer_write=true;s.gpu_buffer_write=true;native->end_render();auto e=[native->command() blitCommandEncoder];[e copyFromBuffer:s.buffer sourceOffset:first toBuffer:d.buffer destinationOffset:dx size:end-first];[e endEncoding];return;}
  Subresource ds,ss;if(!subresource(d,dsub,ds)||!subresource(s,ssub,ss)||d.pixel_format!=s.pixel_format)native->fail("incompatible texture copy");
  D3D11_BOX b=box?*box:D3D11_BOX{0,0,0,ss.width,ss.height,ss.depth};if(b.left>b.right||b.top>b.bottom||b.front>b.back||b.right>ss.width||b.bottom>ss.height||b.back>ss.depth||dx>ds.width||dy>ds.height||dz>ds.depth||b.right-b.left>ds.width-dx||b.bottom-b.top>ds.height-dy||b.back-b.front>ds.depth-dz)native->fail("texture copy exceeds allocation");if(b.left==b.right||b.top==b.bottom||b.front==b.back)return;
  native->end_render();auto e=[native->command() blitCommandEncoder];[e copyFromTexture:s.texture sourceSlice:ss.slice sourceLevel:ss.level sourceOrigin:MTLOriginMake(b.left,b.top,b.front) sourceSize:MTLSizeMake(b.right-b.left,b.bottom-b.top,b.back-b.front) toTexture:d.texture destinationSlice:ds.slice destinationLevel:ds.level destinationOrigin:MTLOriginMake(dx,dy,dz)];[e endEncoding];
}}
void ID3D11DeviceContext::CopyResource(ID3D11Resource* dst,ID3D11Resource* src){@autoreleasepool {
  LOCK_CONTEXT;if(!dst||!src||dst==src)native->fail("invalid CopyResource");auto& d=*dst->native;auto& s=*src->native;if(d.kind!=s.kind)native->fail("CopyResource dimension mismatch");
  if(d.kind==ResourceKind::Buffer){if(d.bd.ByteWidth!=s.bd.ByteWidth)native->fail("CopyResource buffer size mismatch");CopySubresourceRegion(dst,0,0,0,0,src,0,nullptr);return;}
  auto dt=d.texture,st=s.texture;if(dt.width!=st.width||dt.height!=st.height||dt.depth!=st.depth||dt.mipmapLevelCount!=st.mipmapLevelCount||dt.arrayLength!=st.arrayLength||dt.pixelFormat!=st.pixelFormat||dt.textureType!=st.textureType)native->fail("CopyResource texture shape mismatch");UINT slices=UINT(st.arrayLength)*((st.textureType==MTLTextureTypeCube||st.textureType==MTLTextureTypeCubeArray)?6:1);for(UINT a=0;a<slices;++a)for(UINT m=0;m<st.mipmapLevelCount;++m){UINT sub=a*UINT(st.mipmapLevelCount)+m;CopySubresourceRegion(dst,sub,0,0,0,src,sub,nullptr);}
}}
void ID3D11DeviceContext::UpdateSubresource(ID3D11Resource* obj,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT depth_pitch){@autoreleasepool {
  LOCK_CONTEXT;if(!obj||!data)native->fail("invalid UpdateSubresource");auto& r=*obj->native;if(r.mapped)native->fail("updating a mapped resource");
  if(r.kind==ResourceKind::Buffer){if(sub||r.bd.Usage==D3D11_USAGE_IMMUTABLE)native->fail("invalid buffer update");UINT first=box?box->left:0,end=box?box->right:r.bd.ByteWidth;if(first>end||end>r.bd.ByteWidth)native->fail("buffer update exceeds allocation");if(first==end)return;
    const bool constant=r.bd.BindFlags==D3D11_BIND_CONSTANT_BUFFER;
    if(constant){
      ++native->batch_stats.constant_requests;perf_constant_requests.fetch_add(1,std::memory_order_relaxed);
    }
    if(inline_constant(r)){
      ++r.cpu_revision;
      std::memcpy(static_cast<uint8_t*>(r.buffer.contents)+first,data,end-first);
      return;
    }
    // New native allocation protects draws already encoded with the old bytes.
    // Partial updates preserve old bytes only after pending GPU writes complete.
    if(first||end!=r.bd.ByteWidth)native->flush(true);
    id<MTLBuffer> replacement=[native->device->device newBufferWithLength:r.bd.ByteWidth options:MTLResourceStorageModeShared];if(!replacement)native->fail("buffer update allocation failed");if(first||end!=r.bd.ByteWidth)std::memcpy(replacement.contents,r.buffer.contents,r.bd.ByteWidth);std::memcpy(static_cast<uint8_t*>(replacement.contents)+first,data,end-first);r.buffer=replacement;if(constant){++native->batch_stats.constant_allocations;perf_constant_allocations.fetch_add(1,std::memory_order_relaxed);}return;
  }
  if((r.kind==ResourceKind::Texture2D?r.td2.Usage:r.td3.Usage)==D3D11_USAGE_IMMUTABLE)native->fail("texture update destination is immutable");
  native->flush(true);HRESULT hr=upload_texture(r,sub,box,data,row,depth_pitch);if(FAILED(hr))native->fail("texture UpdateSubresource failed");
}}
HRESULT ID3D11DeviceContext::Map(ID3D11Resource* obj,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* out){@autoreleasepool {
  LOCK_CONTEXT;if(out)*out={};auto error=[&](HRESULT hr,const char* s){return set_error(native->device,hr,s);};if(!obj||!out||(flags&~D3D11_MAP_FLAG_DO_NOT_WAIT)||mode<D3D11_MAP_READ||mode>D3D11_MAP_WRITE_NO_OVERWRITE)return error(E_INVALIDARG,"invalid Map arguments");auto& r=*obj->native;if(r.mapped)return error(E_INVALIDARG,"resource already mapped");
  UINT access=r.kind==ResourceKind::Buffer?r.bd.CPUAccessFlags:r.kind==ResourceKind::Texture2D?r.td2.CPUAccessFlags:r.td3.CPUAccessFlags;D3D11_USAGE usage=r.kind==ResourceKind::Buffer?r.bd.Usage:r.kind==ResourceKind::Texture2D?r.td2.Usage:r.td3.Usage;bool reading=mode==D3D11_MAP_READ||mode==D3D11_MAP_READ_WRITE,writing=mode!=D3D11_MAP_READ;
  if((reading&&!(access&D3D11_CPU_ACCESS_READ))||(writing&&!(access&D3D11_CPU_ACCESS_WRITE))||usage==D3D11_USAGE_IMMUTABLE)return error(E_INVALIDARG,"Map violates CPU access or usage");
  if((mode==D3D11_MAP_WRITE_DISCARD||mode==D3D11_MAP_WRITE_NO_OVERWRITE)&&usage!=D3D11_USAGE_DYNAMIC)return error(E_INVALIDARG,"discard/no-overwrite require a dynamic resource");
  if(r.kind==ResourceKind::Buffer){if(sub)return error(E_INVALIDARG,"invalid buffer subresource");
    PerfScope no_overwrite_timer(mode==D3D11_MAP_WRITE_NO_OVERWRITE?&perf_no_overwrite:nullptr);
    // NO_OVERWRITE is the caller's promise that CPU writes avoid every range
    // still read by an encoded/in-flight draw. The RSX upload rings append until
    // wrap, then DISCARD allocates fresh backing. Waiting for unrelated GPU work
    // here defeats that contract and serializes every small vertex/index upload.
    // Other map modes still synchronize; DISCARD retains its fresh allocation.
    if(mode==D3D11_MAP_WRITE_DISCARD){id<MTLBuffer> replacement=[native->device->device newBufferWithLength:r.bd.ByteWidth options:MTLResourceStorageModeShared];if(!replacement)return error(E_OUTOFMEMORY,"Map discard allocation failed");r.buffer=replacement;}else if(mode!=D3D11_MAP_WRITE_NO_OVERWRITE&&!finished(*native,flags))return DXGI_ERROR_WAS_STILL_DRAWING;out->pData=r.buffer.contents;out->RowPitch=out->DepthPitch=r.bd.ByteWidth;
  }else{
    if(mode==D3D11_MAP_WRITE_NO_OVERWRITE)return error(E_INVALIDARG,"NO_OVERWRITE texture maps are unsupported");Subresource s;if(!subresource(r,sub,s))return error(E_INVALIDARG,"invalid texture subresource");if(r.bc_fallback)return error(E_NOTIMPL,"mapping decompressed BC storage is unsupported");if(r.texture.storageMode==MTLStorageModePrivate)return error(E_NOTIMPL,"CPU maps of private textures are unsupported");if(!finished(*native,flags))return DXGI_ERROR_WAS_STILL_DRAWING;
    DXGI_FORMAT format=r.kind==ResourceKind::Texture2D?r.td2.Format:r.td3.Format;UINT bw,bh,bpp;if(!format_layout(format,&bw,&bh,&bpp))return error(E_NOTIMPL,"texture Map storage layout unavailable");r.map_row_pitch=((s.width+bw-1)/bw)*bpp;r.map_depth_pitch=r.map_row_pitch*((s.height+bh-1)/bh);r.map_bytes.assign(size_t(r.map_depth_pitch)*s.depth,0);
    if(reading)[r.texture getBytes:r.map_bytes.data() bytesPerRow:r.map_row_pitch bytesPerImage:r.map_depth_pitch fromRegion:MTLRegionMake3D(0,0,0,s.width,s.height,s.depth) mipmapLevel:s.level slice:s.slice];out->pData=r.map_bytes.data();out->RowPitch=r.map_row_pitch;out->DepthPitch=r.map_depth_pitch;
  }
  r.mapped=true;r.mapped_subresource=sub;r.map_mode=mode;return S_OK;
}}
void ID3D11DeviceContext::Unmap(ID3D11Resource* obj,UINT sub){@autoreleasepool {
  LOCK_CONTEXT;if(!obj)native->fail("null Unmap resource");auto& r=*obj->native;if(!r.mapped||sub!=r.mapped_subresource)native->fail("Unmap does not match active Map");if(r.kind!=ResourceKind::Buffer&&r.map_mode!=D3D11_MAP_READ){native->flush(true);HRESULT hr=upload_texture(r,sub,nullptr,r.map_bytes.data(),r.map_row_pitch,r.map_depth_pitch);if(FAILED(hr)){r.mapped=false;native->fail("texture Unmap upload failed");}}if(r.kind==ResourceKind::Buffer&&r.map_mode!=D3D11_MAP_READ)++r.cpu_revision;r.mapped=false;r.map_bytes.clear();
}}
void ID3D11DeviceContext::Begin(ID3D11Query* obj){@autoreleasepool {LOCK_CONTEXT;if(!obj||obj->native->desc.Query!=D3D11_QUERY_OCCLUSION||native->active_query)native->fail("invalid/nested occlusion Begin");native->end_render();auto& q=*obj->native;q.active=true;q.ended=false;q.samples.clear();q.completion=nil;assign(native->active_query,obj);}}
void ID3D11DeviceContext::End(ID3D11Query* obj){@autoreleasepool {LOCK_CONTEXT;if(!obj)native->fail("null query End");auto& q=*obj->native;if(q.desc.Query==D3D11_QUERY_OCCLUSION){if(native->active_query!=obj||!q.active)native->fail("query End without matching Begin");q.active=false;assign(native->active_query,(ID3D11Query*)nullptr);}else if(q.desc.Query!=D3D11_QUERY_EVENT)native->fail("unsupported query kind");native->command();native->flush(false);q.completion=native->last;q.ended=true;}}
HRESULT ID3D11DeviceContext::GetData(ID3D11Query* obj,void* out,UINT bytes,UINT flags){@autoreleasepool {LOCK_CONTEXT;if(!obj||(flags&~D3D11_ASYNC_GETDATA_DONOTFLUSH))return set_error(native->device,E_INVALIDARG,"invalid GetData arguments");auto& q=*obj->native;if(!q.ended||!q.completion)return S_FALSE;if(!(flags&D3D11_ASYNC_GETDATA_DONOTFLUSH))native->flush(false);auto status=q.completion.status;if(status==MTLCommandBufferStatusError)return set_error(native->device,E_FAIL,ns_error(q.completion.error));if(status!=MTLCommandBufferStatusCompleted)return S_FALSE;UINT needed=q.desc.Query==D3D11_QUERY_EVENT?sizeof(BOOL):sizeof(uint64_t);if(!out&&bytes==0)return S_OK;if(!out||bytes!=needed)return set_error(native->device,E_INVALIDARG,"GetData result size mismatch");if(q.desc.Query==D3D11_QUERY_EVENT){BOOL done=TRUE;std::memcpy(out,&done,sizeof done);}else{uint64_t total=0;for(auto b:q.samples){uint64_t count=0;std::memcpy(&count,b.contents,sizeof count);if(total>UINT64_MAX-count)total=UINT64_MAX;else total+=count;}std::memcpy(out,&total,sizeof total);}return S_OK;}}
void ID3D11DeviceContext::Flush(){@autoreleasepool {LOCK_CONTEXT;native->flush(false);}}
void ID3D11DeviceContext::ClearState(){@autoreleasepool {LOCK_CONTEXT;native->end_render();native->pipeline_dirty=true;native->current_pipeline=nil;native->depth_dirty=true;native->current_depth_state=nil;release_slots(native->rt);assign(native->depth,(ID3D11DepthStencilView*)nullptr);assign(native->blend,(ID3D11BlendState*)nullptr);assign(native->depth_state,(ID3D11DepthStencilState*)nullptr);assign(native->raster,(ID3D11RasterizerState*)nullptr);assign(native->layout,(ID3D11InputLayout*)nullptr);assign(native->indices,(ID3D11Buffer*)nullptr);assign(native->vs,(ID3D11VertexShader*)nullptr);assign(native->ps,(ID3D11PixelShader*)nullptr);release_slots(native->vcb);release_slots(native->pcb);release_slots(native->vsrv);release_slots(native->psrv);release_slots(native->samplers);native->viewports.clear();native->scissors.clear();native->topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;native->index_format=DXGI_FORMAT_R16_UINT;native->index_offset=native->stencil_ref=0;native->sample_mask=~0u;std::fill_n(native->blend_color,4,1.f);}}
#undef LOCK_CONTEXT

namespace acfa::metal {
std::string performance_report(){
  std::ostringstream out;
  out<<"Metal wall time since previous report (overlapping counters): "<<std::fixed<<std::setprecision(2);
  auto append=[&](const char* name,PerfCounter& counter){
    const uint64_t ns=counter.nanoseconds.exchange(0,std::memory_order_relaxed);
    const uint64_t count=counter.calls.exchange(0,std::memory_order_relaxed);
    out<<name<<" "<<double(ns)/1.0e6<<" ms/"<<count<<" calls";
  };
  append("nextDrawable",perf_drawable);out<<", ";append("commit",perf_commit);
  out<<", ";append("waitUntilCompleted",perf_wait);out<<", ";
  out<<"draw encoders "<<perf_draw_encoders.exchange(0,std::memory_order_relaxed)
     <<", draw calls "<<perf_draw_calls.exchange(0,std::memory_order_relaxed)
     <<", reused draws "<<perf_reused_draws.exchange(0,std::memory_order_relaxed)
     <<", query draws "<<perf_query_draws.exchange(0,std::memory_order_relaxed)
     <<", feedback draws "<<perf_feedback_draws.exchange(0,std::memory_order_relaxed)
     <<", pipeline fast reuses "<<perf_pipeline_fast.exchange(0,std::memory_order_relaxed)
     <<"/"<<perf_pipeline_requests.exchange(0,std::memory_order_relaxed)
     <<", canonical pipeline hits "<<perf_pipeline_lookup_hits.exchange(0,std::memory_order_relaxed)<<", ";
  append("pipeline build",perf_pipeline_build);
  out<<", resource binds "<<perf_resource_binds.exchange(0,std::memory_order_relaxed)
     <<", resource binding reuses "<<perf_resource_reuses.exchange(0,std::memory_order_relaxed)<<", ";
  out<<"depth state lookups "<<perf_depth_lookups.exchange(0,std::memory_order_relaxed)
     <<", constant updates "<<perf_constant_requests.exchange(0,std::memory_order_relaxed)<<", unchanged "<<perf_constant_unchanged.exchange(0,std::memory_order_relaxed)
     <<", constant buffer allocations "<<perf_constant_allocations.exchange(0,std::memory_order_relaxed)<<", ";
  append("buffer NO_OVERWRITE Map",perf_no_overwrite);
  return out.str();
}
RenderBatchStats render_batch_stats(ID3D11DeviceContext* c){
  if(!c)return {};std::lock_guard lock(c->native->mutex);return c->native->batch_stats;
}
void flush_context(ID3D11DeviceContext* c,bool wait){@autoreleasepool {if(!c)return;std::lock_guard lock(c->native->mutex);c->native->flush(wait);}}
bool readback_texture(ID3D11DeviceContext* c,ID3D11Texture2D* obj,std::vector<uint8_t>& pixels,uint32_t& width,uint32_t& height){@autoreleasepool {
  pixels.clear();width=height=0;if(!c||!obj)return false;std::lock_guard lock(c->native->mutex);auto& n=*c->native;if(obj->native->mapped){set_error(n.device,E_INVALIDARG,"readback resource is mapped");return false;}auto t=obj->native->texture;
  if(!t||t.sampleCount!=1||(t.pixelFormat!=MTLPixelFormatRGBA8Unorm&&t.pixelFormat!=MTLPixelFormatBGRA8Unorm)){set_error(n.device,E_NOTIMPL,"RGBA readback requires single-sample RGBA8/BGRA8");return false;}
  NSUInteger pitch=(t.width*4+255)&~NSUInteger(255);id<MTLBuffer> b=[n.device->device newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];if(!b){set_error(n.device,E_OUTOFMEMORY,"readback buffer allocation failed");return false;}
  n.end_render();auto e=[n.command() blitCommandEncoder];[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*t.height];[e endEncoding];if(!n.flush(true))return false;
  width=uint32_t(t.width);height=uint32_t(t.height);pixels.resize(size_t(width)*height*4);for(unsigned y=0;y<height;++y)std::memcpy(pixels.data()+size_t(y)*width*4,static_cast<uint8_t*>(b.contents)+y*pitch,size_t(width)*4);
  if(t.pixelFormat==MTLPixelFormatBGRA8Unorm)for(size_t i=0;i<pixels.size();i+=4)std::swap(pixels[i],pixels[i+2]);return true;
}}
bool present_to_layer(ID3D11DeviceContext* c,ID3D11ShaderResourceView* src,void* handle,bool flip_y){@autoreleasepool {
  if(!c||!src||!handle)return false;std::lock_guard lock(c->native->mutex);auto& n=*c->native;n.end_render();if(src->native->resource->native->mapped){set_error(n.device,E_INVALIDARG,"present resource is mapped");return false;}id<MTLTexture> source=src->native->texture;if(!source){set_error(n.device,E_INVALIDARG,"present requires a texture SRV");return false;}
  CAMetalLayer* layer=(__bridge CAMetalLayer*)handle;layer.device=n.device->device;id<CAMetalDrawable> drawable=nil;{PerfScope timer(&perf_drawable);drawable=[layer nextDrawable];}if(!drawable){set_error(n.device,E_FAIL,"CAMetalLayer has no drawable");return false;}
  if(!n.present_vs){
    static const char* text=R"(#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; float2 uv; };
vertex V acfa_present_vs(uint id [[vertex_id]],constant uint& flip [[buffer(0)]]) { float2 p=float2((id<<1)&2,id&2);V o;o.position=float4(p*2.0-1.0,0,1);o.uv=float2(p.x,1.0-p.y);if(flip)o.uv.y=1.0-o.uv.y;return o; }
fragment float4 acfa_present_ps(V v [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]]) { return t.sample(s,v.uv); }
)";
    NSError* error=nil;MTLCompileOptions* options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;id<MTLLibrary> lib=[n.device->device newLibraryWithSource:[NSString stringWithUTF8String:text] options:options error:&error];if(!lib){set_error(n.device,E_FAIL,ns_error(error));return false;}n.present_vs=[lib newFunctionWithName:@"acfa_present_vs"];n.present_ps=[lib newFunctionWithName:@"acfa_present_ps"];auto sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear;sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeClampToEdge;n.present_sampler=[n.device->device newSamplerStateWithDescriptor:sd];
  }
  auto target=drawable.texture;unsigned key=unsigned(target.pixelFormat);id<MTLRenderPipelineState> pipeline=nil;auto found=n.present_pipelines.find(key);if(found!=n.present_pipelines.end())pipeline=found->second;else{auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=n.present_vs;desc.fragmentFunction=n.present_ps;desc.colorAttachments[0].pixelFormat=target.pixelFormat;NSError* error=nil;pipeline=[n.device->device newRenderPipelineStateWithDescriptor:desc error:&error];if(!pipeline){set_error(n.device,E_FAIL,ns_error(error));return false;}n.present_pipelines.emplace(key,pipeline);}
  auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
  double scale=std::min(double(target.width)/source.width,double(target.height)/source.height);double width=source.width*scale,height=source.height*scale;auto e=[n.command() renderCommandEncoderWithDescriptor:pass];if(!e){set_error(n.device,E_FAIL,"present render encoder failed");return false;}[e setRenderPipelineState:pipeline];[e setViewport:MTLViewport{(target.width-width)*.5,(target.height-height)*.5,width,height,0,1}];[e setCullMode:MTLCullModeNone];UINT flip=flip_y?1:0;[e setVertexBytes:&flip length:sizeof flip atIndex:0];[e setFragmentTexture:source atIndex:0];[e setFragmentSamplerState:n.present_sampler atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[n.command() presentDrawable:drawable];return n.flush(false);
}}
} // namespace acfa::metal
