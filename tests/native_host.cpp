#include <algorithm>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include "common/crypto.h"
#include "common/file.h"
#include "common/game_profile.h"
#include "common/ini.h"
#include "common/input_policy.h"
#include "common/known_inputs.h"
#include "runtime/config.h"
#include "runtime/input.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/content_snapshot.h"
#include "runtime/rsx/cpu_readback.h"
#include <thread>
namespace rt { Config& cfg() { static Config value; return value; } }
#include "ppu/ppu_recomp.h"
#include "spu/spu_ops.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

static void require(bool b, const char* what) { if (!b) throw std::runtime_error(what); }
template<class F> static void must_fail(F f, const char* what) {
  bool failed=false; try { f(); } catch (const std::exception&) { failed=true; }
  require(failed, what);
}
int main(int argc, char** argv) {
 try {
  require(argc==2,"scratch path required");
  {
    rt::rsx::CpuReadbackMailbox mailbox;bool result=false;unsigned copies=0;
    std::thread reader([&]{result=mailbox.request(123,456);});
    while(!mailbox.pending())std::this_thread::yield();
    mailbox.service([&](u32 ea,u32 size){++copies;require(ea==123&&size==456,"CPU readback range");return true;});
    reader.join();require(result&&copies==1,"CPU readback before CPU resumes");
    require(!mailbox.request(789,1,std::chrono::milliseconds(1)),"queued CPU readback timeout");
    mailbox.service([&](u32,u32){++copies;return true;});
    require(copies==1&&!mailbox.pending(),"cancelled CPU readback cannot write later");
    result=true;std::thread miss([&]{result=mailbox.request(123,456);});
    while(!mailbox.pending())std::this_thread::yield();
    mailbox.service([](u32,u32){return false;});miss.join();
    require(!result,"CPU source with no native render target is a miss");
  }
  { // Mutations within the same frame and at either edge must invalidate.
    rt::gpu::ContentSnapshot snapshot; std::vector<unsigned char> data(65536,17);
    require(!snapshot.matches(data.data(),data.size()),"empty snapshot misses");
    snapshot.capture(data.data(),data.size());
    require(snapshot.matches(data.data(),data.size()),"snapshot matches exact input");
    for(size_t at : {size_t(0),size_t(4095),size_t(32768),data.size()-1}) {
      data[at]^=1;require(!snapshot.matches(data.data(),data.size()),"single byte rewrite invalidates");
      data[at]^=1;require(snapshot.matches(data.data(),data.size()),"restored content matches");
    }
    require(!snapshot.matches(data.data(),data.size()-1),"different range cannot reuse");
    data[123]=99;snapshot.capture(data.data(),data.size());
    require(snapshot.matches(data.data(),data.size()),"replacement snapshot matches");
    snapshot.clear();require(snapshot.size()==0&&!snapshot.matches(data.data(),data.size()),"eviction releases snapshot");
  }

  const std::string abc="abc";
  auto a=acfa::crypto::sha1(abc.data(),abc.size());
  auto b=acfa::crypto::sha256(abc.data(),abc.size());
  require(acfa::hex(a.data(),a.size())=="a9993e364706816aba3e25717850c26c9cd0d89d","SHA1 abc");
  require(acfa::hex(b.data(),b.size())=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
  auto empty=acfa::crypto::sha256(nullptr,0);
  require(acfa::hex(empty.data(),empty.size())=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
  namespace fs=std::filesystem;
  fs::path scratch=fs::absolute(argv[1]);
  require(!fs::exists(scratch),"scratch must not already exist");fs::create_directories(scratch);
  std::string large((1<<20)+37,'a'); auto hash=acfa::crypto::sha256(large.data(),large.size());
  fs::path sample=scratch/u8"中文文件.bin";acfa::write_text(sample,large);
  require(acfa::crypto::sha256_file_hex(sample.string())==acfa::hex(hash.data(),hash.size()),"streamed SHA boundary/UTF8 path");
  auto ini=acfa::Ini::parse("[text]\nname = 简体中文\n");auto ip=scratch/u8"设置.ini";ini.save(ip);
  ini.set("text","name","日本語");ini.save(ip);
  require(acfa::Ini::load(ip).get_or("text","name","")=="日本語","atomic INI replace UTF8");
  fs::path protected_root=scratch/"protected";fs::create_directory(protected_root);acfa::add_protected_root(protected_root);
  must_fail([&]{acfa::write_text(protected_root/"should-not-exist",abc);},"protected root write accepted");
  fs::create_directory_symlink(protected_root,scratch/"alias");
  must_fail([&]{acfa::write_text(scratch/"alias/no-write",abc);},"symlink protection bypass");
  acfa::write_text(scratch/"protected-sibling/allowed",abc);
  if(fs::exists(scratch/"PROTECTED"))
    must_fail([&]{acfa::write_text(scratch/"PROTECTED/no-write",abc);},"case alias protection bypass");
  require(!acfa::input_policy::keyboard_mouse,"controller-only default not active");
  rt::gpu::input_key('W',true);rt::input_key_inject('W',true);
  require(!rt::input_key_down('W'),"keyboard injection still active");
  rt::gpu::input_mouse_capture(true);rt::input_mouse_inject(100,-20);rt::input_mouse_button_inject(0,true);
  s64 dx=1,dy=1;rt::input_mouse_take(&dx,&dy);
  require(dx==0&&dy==0&&!rt::input_mouse_captured()&&!rt::input_mouse_button(0),"mouse injection/capture still active");
  rt::gpu::input_focus(false);require(!rt::input_has_focus(),"focus lost");
  rt::gpu::input_focus(true);require(rt::input_has_focus(),"focus restored");

  for(auto name:{"look_update","input_turn_set","ac_turn_update","camera_follow","menu_frame","menu_dispatch"})
    require(!acfa::input_policy::allow_hook(name),"camera/menu hook escaped controller-only policy");
  require(acfa::input_policy::allow_hook("ms_stream_dtor"),"audio hook incorrectly disabled");
#if ACFA_GAME_JP
  require(std::string(acfa::inputs::kTitleId)=="BLJM55005","JP profile identity");
  require(acfa::inputs::valid_hook(0xef97c8,"ms_stream_dtor"),"JP hook whitelist");
  require(!acfa::inputs::valid_hook(0xef9078,"ms_stream_dtor"),"US hook accepted by JP profile");
  require(acfa::inputs::valid_hook(0x003fdfa0,"texture_compress") &&
          acfa::inputs::valid_hook(0x00d60f30,"compression_texture_lock"),"JP compressor hooks must regenerate without bypassing whitelist");
  require(!acfa::inputs::valid_hook(0x003fdfa4,"texture_compress") &&
          !acfa::inputs::valid_hook(0x00d60f30,"texture_compress"),"unknown/mismatched compressor hook rejected");
#endif
  using acfa::game_profile::Profile;
  require(std::string(acfa::game_profile::spu_manifest_file(Profile::JapanBLJM55005))==
          "data/profiles/BLJM55005/spu_manifest.txt","exact JP SPU manifest path");
  require(std::string(acfa::game_profile::spu_manifest_file(Profile::UsaBLUS30187))==
          "data/spu_manifest.txt","exact US SPU manifest path");
  require(acfa::game_profile::spu_manifest_file(static_cast<Profile>(-1))==nullptr,"unknown profile accepted SPU metadata");
  require(std::string(acfa::game_profile::cmake_profile_name(Profile::JapanBLJM55005))=="jp-bljm55005" &&
          std::string(acfa::game_profile::cmake_profile_name(Profile::UsaBLUS30187))=="us-blus30187",
          "exact JP/US CMake profile names");
  require(acfa::game_profile::cmake_profile_name(static_cast<Profile>(-1))==nullptr,"unknown CMake profile accepted");
#if ACFA_GAME_JP
  require(std::string(acfa::game_profile::kCMakeProfileName)=="jp-bljm55005","current JP CMake profile");
  require(acfa::game_profile::kCurrent==Profile::JapanBLJM55005 &&
          std::string(acfa::game_profile::kSpuManifestFile)=="data/profiles/BLJM55005/spu_manifest.txt",
          "current JP profile selects JP SPU manifest");
#else
  require(std::string(acfa::game_profile::kCMakeProfileName)=="us-blus30187","current US CMake profile");
  require(acfa::game_profile::kCurrent==Profile::UsaBLUS30187 &&
          std::string(acfa::game_profile::kSpuManifestFile)=="data/spu_manifest.txt",
          "current US profile preserves original SPU manifest");
#endif
  ppu::v128 round_input{};
  round_input.fx[0]=1.5f;round_input.fx[1]=2.5f;round_input.fx[2]=-1.5f;round_input.fx[3]=-2.5f;
  auto rounded=ppu::mkf(_mm_round_ps(round_input.vf,_MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC));
  require(rounded.fx[0]==2&&rounded.fx[1]==2&&rounded.fx[2]==-2&&rounded.fx[3]==-2,"VMX round nearest ties even");
  std::mt19937 random(0xACFA);
  for(int n=0;n<2000;++n) {
    ppu::v128 x{},y{},c{};
    for(int i=0;i<16;++i){ppu::be8(x,i)=static_cast<u8>(random());ppu::be8(y,i)=static_cast<u8>(random());ppu::be8(c,i)=static_cast<u8>(random());}
    auto perm=ppu::vperm(x,y,c);
    for(int i=0;i<16;++i) {int k=ppu::be8(c,i)&31;require(ppu::be8(perm,i)==(k<16?ppu::be8(x,k):ppu::be8(y,k-16)),"VMX permute endian");}
    auto l=ppu::vslw(x,y),r=ppu::vsrw(x,y),s=ppu::vsraw(x,y);
    for(int i=0;i<4;++i){auto sh=ppu::be32(y,i)&31;auto v=ppu::be32(x,i);
      require(ppu::be32(l,i)==(v<<sh),"VMX variable shift left");
      require(ppu::be32(r,i)==(v>>sh),"VMX variable shift right");
      require(ppu::be32(s,i)==u32(s32(v)>>sh),"VMX signed shift right");}
    auto packed=ppu::vpkswus(x,y);
    for(int i=0;i<8;++i){s32 v=s32(ppu::be32(i<4?x:y,i%4));require(ppu::be16(packed,i)==u16(std::clamp<s32>(v,0,65535)),"VMX saturating signed pack");}
    auto merged=ppu::vmrghw(x,y);
    for(int i=0;i<4;++i)require(ppu::be32(merged,i)==ppu::be32(i%2?y:x,i/2),"VMX merge high endian");
    require(spu::ops::pref(spu::ops::scalar(n))==u32(n),"SPU preferred scalar slot");
  }
#if defined(__APPLE__)
  mach_vm_address_t addr=PPU_MEM_BASE+0x10000;
  auto kr=mach_vm_allocate(mach_task_self(),&addr,16384,VM_FLAGS_FIXED);
  require(kr==KERN_SUCCESS,"fixed guest test page unavailable");
  ppu::w32(0x10000,0x12345678);require(ppu::r32(0x10000)==0x12345678,"guest big-endian memory");
  for(int i=0;i<16;++i)ppu::w8(0x10020+i,u8(i));
  auto v=ppu::lvx(0x10020);
  for(int i=0;i<16;++i)require(ppu::be8(v,i)==i,"VMX load byte order");
  ppu::stvx(0x10040,v);
  for(int i=0;i<16;++i)require(ppu::r8(0x10040+i)==i,"VMX store byte order");
  require(mach_vm_deallocate(mach_task_self(),addr,16384)==KERN_SUCCESS,"test page release");
#endif
  fs::remove_all(scratch);
  std::cout<<"PASS: hashes, UTF8/atomic IO, write guards, controller policy, JP hooks/profile manifests, 2000 SIMD oracle cases, guest memory\n";
  return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
