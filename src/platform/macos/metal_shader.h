#pragma once
#include <string>

namespace acfa::metal {
enum class ShaderStage { Vertex, Fragment, Compute };
struct ShaderSource {
  std::string source;
  std::string entry;
  std::string diagnostics;
  unsigned remapped_vertex_buffers = 0;
};
// Vertex byte-address streams t0..t3 map to Metal buffers 16..19; b0..b3 retain
// their slots. Metal shares the constant/raw buffer namespace, unlike D3D11.
constexpr unsigned kVertexBufferBase = 16;
bool hlsl_to_msl(const std::string& hlsl, const std::string& entry,
                 ShaderStage stage, ShaderSource* out);
const char* shader_compiler_version();
}  // namespace acfa::metal
