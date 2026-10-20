#include "platform/macos/metal_shader.h"
#include <slang.h>
#include <slang-com-ptr.h>
#include <mutex>
#include <regex>
#include <sstream>

namespace acfa::metal {
namespace {
std::mutex compiler_mutex;
Slang::ComPtr<slang::IGlobalSession> compiler;
std::string blob_text(slang::IBlob* b) {
  return b ? std::string(static_cast<const char*>(b->getBufferPointer()), b->getBufferSize()) : std::string();
}
bool remap_vertex_streams(std::string& text, unsigned* remapped, std::string& error) {
  const std::regex decl(R"((ByteAddressBuffer\s+vb([0-3])\s*:\s*register\(\s*t)([0-3])(\s*\)))");
  std::string result;size_t cursor=0;*remapped=0;
  for (auto it=std::sregex_iterator(text.begin(),text.end(),decl);it!=std::sregex_iterator();++it) {
    const auto& m=*it;
    if(m[2].str()!=m[3].str()){error="vertex stream name/register disagree";return false;}
    result.append(text,cursor,size_t(m.position())-cursor);
    result+=m[1].str()+std::to_string(kVertexBufferBase+unsigned(m[3].str()[0]-'0'))+m[4].str();
    cursor=size_t(m.position()+m.length());++*remapped;
  }
  result.append(text,cursor,std::string::npos);
  if(*remapped!=0 && *remapped!=4){error="partial RSX vertex stream declaration";return false;}
  text=std::move(result);return true;
}
}
const char* shader_compiler_version() {
  static const std::string version=[] {
    std::lock_guard lock(compiler_mutex);
    if(!compiler && SLANG_FAILED(slang::createGlobalSession(compiler.writeRef())))
      return std::string("slang-unavailable/metal-2.3/raw-buffers-16-v1");
    const char* tag=compiler->getBuildTagString();
    return std::string("slang-")+(tag?tag:"unknown")+"/metal-2.3/raw-buffers-16-v1";
  }();
  return version.c_str();
}
bool hlsl_to_msl(const std::string& hlsl,const std::string& entry,ShaderStage stage,ShaderSource* out) {
  if(!out)return false;*out={};
  if(!std::regex_match(entry,std::regex("[A-Za-z_][A-Za-z0-9_]*"))){out->diagnostics="invalid entry name";return false;}
  if(hlsl.empty()){out->diagnostics="empty shader";return false;}
  std::string text=hlsl;
  if(stage==ShaderStage::Vertex && !remap_vertex_streams(text,&out->remapped_vertex_buffers,out->diagnostics))return false;
  out->entry=stage==ShaderStage::Vertex?"acfa_vs":stage==ShaderStage::Fragment?"acfa_ps":"acfa_cs";
  text="#define "+entry+" "+out->entry+"\n"+text;
  std::lock_guard lock(compiler_mutex);
  if(!compiler && SLANG_FAILED(slang::createGlobalSession(compiler.writeRef()))){out->diagnostics="cannot initialize Slang";return false;}
  slang::TargetDesc target;target.format=SLANG_METAL;target.profile=compiler->findProfile("metal_2_3");
  slang::SessionDesc desc;desc.targets=&target;desc.targetCount=1;
  Slang::ComPtr<slang::ISession> session;
  if(SLANG_FAILED(compiler->createSession(desc,session.writeRef()))){out->diagnostics="cannot create Metal compiler session";return false;}
  Slang::ComPtr<slang::IBlob> diag;
  Slang::ComPtr<slang::IModule> module(session->loadModuleFromSourceString("acfa_shader","acfa_shader.hlsl",text.c_str(),diag.writeRef()));
  out->diagnostics+=blob_text(diag);diag.setNull();
  if(!module)return false;
  Slang::ComPtr<slang::IEntryPoint> ep;
  SlangStage ss=stage==ShaderStage::Vertex?SLANG_STAGE_VERTEX:stage==ShaderStage::Fragment?SLANG_STAGE_FRAGMENT:SLANG_STAGE_COMPUTE;
  if(SLANG_FAILED(module->findAndCheckEntryPoint(out->entry.c_str(),ss,ep.writeRef(),diag.writeRef()))){out->diagnostics+=blob_text(diag);return false;}
  diag.setNull();slang::IComponentType* parts[]={module,ep};
  Slang::ComPtr<slang::IComponentType> program,linked;
  if(SLANG_FAILED(session->createCompositeComponentType(parts,2,program.writeRef(),diag.writeRef()))){out->diagnostics+=blob_text(diag);return false;}
  diag.setNull();
  if(SLANG_FAILED(program->link(linked.writeRef(),diag.writeRef()))){out->diagnostics+=blob_text(diag);return false;}
  diag.setNull();Slang::ComPtr<slang::IBlob> code;
  if(SLANG_FAILED(linked->getEntryPointCode(0,0,code.writeRef(),diag.writeRef()))){out->diagnostics+=blob_text(diag);return false;}
  out->diagnostics+=blob_text(diag);out->source=blob_text(code);
  return !out->source.empty();
}
}  // namespace acfa::metal
