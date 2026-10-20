#include "platform/macos/metal_shader.h"
#include "rsx_shader_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
int main(int argc,char**argv){
  if(argc!=2)return 2;std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
  std::cout<<acfa::metal::shader_compiler_version()<<"\n";
  rt::gpu::VpInfo vi;auto vp=acfa::test::vertex_program();std::string vs=rt::gpu::vp_to_hlsl(vp.data(),0,&vi);
  if(!vi.error.empty())throw std::runtime_error(vi.error);
  for(bool textured:{false,true}){
    rt::gpu::FpInfo fi;rt::gpu::FpEnv env;if(textured)env.tex_dim[0]=2;
    auto fp=acfa::test::fragment_program(textured);std::string fs=rt::gpu::fp_to_hlsl(fp.data(),fp.size(),env,&fi);
    if(!fi.error.empty())throw std::runtime_error(fi.error);
    const char* name=textured?"textured":"color";
    for(bool vertex:{true,false}){
      const std::string& hlsl=vertex?vs:fs;acfa::metal::ShaderSource msl;
      bool ok=acfa::metal::hlsl_to_msl(hlsl,"main",vertex?acfa::metal::ShaderStage::Vertex:acfa::metal::ShaderStage::Fragment,&msl);
      std::string base=std::string(name)+(vertex?".vs":".ps");
      std::ofstream(out/(base+".hlsl"))<<hlsl;std::ofstream(out/(base+".diagnostics.txt"))<<msl.diagnostics;
      if(!ok){std::cerr<<base<<" "<<msl.diagnostics;return 1;}
      std::ofstream(out/(base+".metal"))<<msl.source;
      std::cout<<base<<" entry="<<msl.entry<<" remapped="<<msl.remapped_vertex_buffers<<" bytes="<<msl.source.size()<<"\n";
    }
  }
}
