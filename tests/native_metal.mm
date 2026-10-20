// Exercises the real RSX -> HLSL -> MSL path on a headless Metal render target.
// Synthetic microcode only. No NSApplication/window, input, audio or game startup.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "platform/macos/metal_shader.h"
#include "rsx_shader_fixture.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {
void require(bool b, const std::string& why) { if (!b) throw std::runtime_error(why); }
std::string error_text(NSError* error) {
  return error ? std::string(error.localizedDescription.UTF8String) : "no Metal diagnostic";
}
id<MTLFunction> compile(id<MTLDevice> device, const acfa::metal::ShaderSource& source) {
  MTLCompileOptions* options = [MTLCompileOptions new];
  options.languageVersion = MTLLanguageVersion2_3;
  if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
  else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
  }
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.source.c_str()]
                                              options:options error:&error];
  require(library != nil, "Metal library: " + error_text(error));
  id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:source.entry.c_str()]];
  require(function != nil, "Metal entry missing: " + source.entry);
  return function;
}
std::vector<uint8_t> guest_floats(const std::vector<float>& values) {
  std::vector<uint8_t> bytes(values.size() * 4 + 32, 0); // Load4 + look-ahead padding.
  for (size_t i = 0; i < values.size(); ++i) {
    uint32_t bits; std::memcpy(&bits, &values[i], 4);
    for (unsigned j = 0; j < 4; ++j) bytes[i*4+j] = uint8_t(bits >> (24-j*8));
  }
  return bytes;
}
void draw_case(id<MTLDevice> device, id<MTLCommandQueue> queue, bool textured, bool constants_case=false) {
  auto vp = acfa::test::vertex_program(constants_case);
  auto plain_fp = acfa::test::fragment_program(textured);
  auto constant_fp = acfa::test::fragment_constant_program();
  const uint8_t* fp = constants_case ? constant_fp.data() : plain_fp.data();
  size_t fp_size = constants_case ? constant_fp.size() : plain_fp.size();
  rt::gpu::VpInfo vi; rt::gpu::FpInfo fi; rt::gpu::FpEnv env;
  if (textured) env.tex_dim[0] = 2;
  std::string vh = rt::gpu::vp_to_hlsl(vp.data(), 0, &vi);
  std::string fh = rt::gpu::fp_to_hlsl(fp, unsigned(fp_size), env, &fi);
  require(vi.error.empty() && vi.input_mask == ((1u<<0)|(1u<<3)), "RSX vertex fixture: " + vi.error);
  require(fi.error.empty() && fi.tex_mask == unsigned(textured), "RSX fragment fixture: " + fi.error);
  acfa::metal::ShaderSource vs, ps;
  require(acfa::metal::hlsl_to_msl(vh, "main", acfa::metal::ShaderStage::Vertex, &vs), vs.diagnostics);
  require(vs.remapped_vertex_buffers == 4, "RSX raw stream bindings were not remapped");
  require(acfa::metal::hlsl_to_msl(fh, "main", acfa::metal::ShaderStage::Fragment, &ps), ps.diagnostics);
  MTLRenderPipelineDescriptor* pipeline_desc = [MTLRenderPipelineDescriptor new];
  pipeline_desc.vertexFunction = compile(device, vs);
  pipeline_desc.fragmentFunction = compile(device, ps);
  pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
  require(pipeline != nil, "Metal pipeline: " + error_text(error));

  constexpr unsigned side = 64;
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
       width:side height:side mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> target = [device newTextureWithDescriptor:td];
  require(target != nil, "Metal render target allocation");
  auto vertices = guest_floats({-1,-1,0,1, 3,-1,0,1, -1,3,0,1});
  auto colors = guest_floats({.2f,.6f,.9f,1, .2f,.6f,.9f,1, .2f,.6f,.9f,1});
  id<MTLBuffer> position = [device newBufferWithBytes:vertices.data() length:vertices.size() options:MTLResourceStorageModeShared];
  id<MTLBuffer> color = [device newBufferWithBytes:colors.data() length:colors.size() options:MTLResourceStorageModeShared];
  id<MTLBuffer> dummy = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
  id<MTLBuffer> constants = [device newBufferWithLength:8192 options:MTLResourceStorageModeShared];
  require(position && color && dummy && constants, "Metal vertex/constant allocation");
  std::memset(dummy.contents, 0, 256); std::memset(constants.contents, 0, 8192);
  id<MTLBuffer> fp_constants = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
  require(fp_constants!=nil,"fragment constants allocation");std::memset(fp_constants.contents,0,4096);
  if(constants_case) {
    const float vc[4]={.5f,.5f,.5f,1};std::memcpy(constants.contents,vc,sizeof(vc));
    require(fi.const_offsets.size()==1,"FP inline constant slot");
    for(unsigned c=0;c<4;++c) {
      uint32_t bits=rt::gpu::fp_word(fp+fi.const_offsets[0]+4*c);
      std::memcpy(static_cast<uint8_t*>(fp_constants.contents)+c*4,&bits,4);
    }
  }
  rt::gpu::VtxFmt format{};
  format.attr[0] = {0,16,1,2u|(4u<<4)|((constants_case?2u:0u)<<8)|(1u<<16)};
  format.attr[3] = {0,16,1,2u|(4u<<4)|((constants_case?3u:1u)<<8)|(1u<<16)};
  std::array<float,16> vp_state = {32,-32,1,0, 32,32,0,0, 64,64,1.f/64,1.f/64, 0,0,0,0};
  std::array<float,80> fp_state{};
  fp_state[0] = side; fp_state[10] = 1; fp_state[11] = 1;
  for (unsigned i=0;i<16;++i) fp_state[16+i*4] = fp_state[17+i*4] = fp_state[18+i*4] = 1;
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
  require(enc != nil, "Metal render encoder");
  [enc setRenderPipelineState:pipeline];
  [enc setViewport:MTLViewport{0,0,side,side,0,1}];
  [enc setCullMode:MTLCullModeNone];
  [enc setVertexBuffer:constants offset:0 atIndex:0];
  [enc setVertexBytes:vp_state.data() length:sizeof(vp_state) atIndex:1];
  [enc setVertexBytes:&format length:sizeof(format) atIndex:2];
  [enc setVertexBuffer:(constants_case?dummy:position) offset:0 atIndex:16];
  [enc setVertexBuffer:(constants_case?dummy:color) offset:0 atIndex:17];
  [enc setVertexBuffer:(constants_case?position:dummy) offset:0 atIndex:18];
  [enc setVertexBuffer:(constants_case?color:dummy) offset:0 atIndex:19];
  [enc setFragmentBytes:fp_state.data() length:sizeof(fp_state) atIndex:2];
  [enc setFragmentBuffer:fp_constants offset:0 atIndex:3];
  if (textured) {
    MTLTextureDescriptor* sample_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:2 height:2 mipmapped:NO];
    sample_desc.storageMode = MTLStorageModeShared;
    sample_desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sample = [device newTextureWithDescriptor:sample_desc];
    const std::array<uint8_t,16> pixels = {230,26,102,255, 230,26,102,255, 230,26,102,255, 230,26,102,255};
    [sample replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:8];
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    [enc setFragmentTexture:sample atIndex:0];
    [enc setFragmentSamplerState:[device newSamplerStateWithDescriptor:sd] atIndex:0];
  }
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
  require(cb.status == MTLCommandBufferStatusCompleted, "Metal command: " + error_text(cb.error));
  std::vector<uint8_t> readback(side*side*4);
  [target getBytes:readback.data() bytesPerRow:side*4 fromRegion:MTLRegionMake2D(0,0,side,side) mipmapLevel:0];
  const std::array<int,4> expected = constants_case ? std::array<int,4>{13,77,115,255} : textured ? std::array<int,4>{230,26,102,255} : std::array<int,4>{51,153,230,255};
  for (size_t p=0;p<side*side;++p) for (size_t c=0;c<4;++c)
    require(std::abs(int(readback[p*4+c])-expected[c])<=1,
            std::string(textured ? "texture" : "color") + " readback mismatch pixel="+std::to_string(p)+
            " component="+std::to_string(c)+" actual="+std::to_string(readback[p*4+c])+" expected="+std::to_string(expected[c]));
  std::cout << (constants_case ? "all raw/constant bindings" : textured ? "textured" : "color") << " RSX shader draw: all 4096 pixels match\n";
}
void renderer_utilities(id<MTLDevice> device,const std::filesystem::path& source_root) {
  // Read the actual renderer literals so a later shader edit cannot leave stale fixtures green.
  std::ifstream input(source_root/"src/runtime/gpu/renderer.cpp");
  require(bool(input),"cannot read renderer utility shader source");
  std::ostringstream bytes;bytes<<input.rdbuf();auto source=bytes.str();
  for(const char* name : {"kClearHlsl","kDepthToColorHlsl","kStretchHlsl"}) {
    std::string marker=std::string("const char* ")+name+" = R\"(";
    auto start=source.find(marker);require(start!=std::string::npos,"renderer utility literal missing");
    start+=marker.size();auto end=source.find(")\";",start);
    require(end!=std::string::npos,"renderer utility literal unterminated");
    auto hlsl=source.substr(start,end-start);acfa::metal::ShaderSource vs,ps;
    require(acfa::metal::hlsl_to_msl(hlsl,"vs_main",acfa::metal::ShaderStage::Vertex,&vs),vs.diagnostics);
    require(acfa::metal::hlsl_to_msl(hlsl,"ps_main",acfa::metal::ShaderStage::Fragment,&ps),ps.diagnostics);
    MTLRenderPipelineDescriptor* desc=[MTLRenderPipelineDescriptor new];
    desc.vertexFunction=compile(device,vs);desc.fragmentFunction=compile(device,ps);
    desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    NSError* error=nil;
    require([device newRenderPipelineStateWithDescriptor:desc error:&error]!=nil,
            std::string(name)+" pipeline: "+error_text(error));
  }
  std::cout<<"3 actual renderer utility shader pairs: Metal pipelines compiled\n";
}
void negative_inputs() {
  acfa::metal::ShaderSource out;
  require(!acfa::metal::hlsl_to_msl("", "main", acfa::metal::ShaderStage::Vertex, &out), "empty shader accepted");
  require(!acfa::metal::hlsl_to_msl("void main(){}", "bad entry", acfa::metal::ShaderStage::Vertex, &out), "bad entry accepted");
  require(!acfa::metal::hlsl_to_msl("ByteAddressBuffer vb0 : register(t0); void main(){}", "main", acfa::metal::ShaderStage::Vertex, &out), "partial raw bindings accepted");
  require(!acfa::metal::hlsl_to_msl("not valid HLSL", "main", acfa::metal::ShaderStage::Fragment, &out), "malformed shader accepted");
}
}
int main(int argc,char** argv) {
  @autoreleasepool {
    try {
      negative_inputs();
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      require(device != nil, "no native Metal device available");
      id<MTLCommandQueue> queue = [device newCommandQueue];
      require(queue != nil, "Metal command queue allocation");
      std::cout << "Metal GPU: " << device.name.UTF8String << '\n';
      draw_case(device,queue,false); draw_case(device,queue,true);draw_case(device,queue,false,true);
      require(argc==2,"source path required for renderer utility validation");
      renderer_utilities(device,argv[1]);
      return 0;
    } catch (const std::exception& e) {
      std::cerr << "native-metal: " << e.what() << '\n'; return 1;
    }
  }
}
