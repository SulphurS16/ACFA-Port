// Real PPU registry/reservations/timebase and VM. Only guest code/HLE dispatch
// provenance are fixtures: this does not load or execute any game code.
#include "platform/macos/host_thread.h"
#include "platform/macos/guest_memory.h"
#include "runtime/config.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/ppu_thread.h"
#include "runtime/reservation.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
namespace rt {
thread_local PpuThread* test_cur_thread=nullptr;
PpuThread* cur_thread() { return test_cur_thread; }
Config& cfg() { static Config c; return c; }
CallScope::CallScope(const char*,const ppu::Context&) {}
CallScope::~CallScope() {}
namespace hle { const Syscall* syscall_entry(u32) { return nullptr; } }
}
namespace ppu {
struct FuncEntry { u32 addr; Fn fn; };
static void fixture_low(Ctx& c) { c.r[3]=123; }
static void fixture_high(Ctx& c) { c.r[3]=456; }
extern const FuncEntry g_funcs[]={{0x10000,fixture_low},{0x10100,fixture_high}};
extern const size_t g_func_count=2;
}
namespace rt::rsx {
unsigned texture_read_requests=0;
bool read_rendered_memory(u32 ea,u32 size) {
  ++texture_read_requests;if(ea!=0x61000||size!=1024)throw std::runtime_error("compressor source range");
  ppu::w32(ea,0x11223344);return true;
}
}
namespace ppu {
void hook_texture_compress(Ctx&,void (*)(Ctx&));
void hook_compression_texture_lock(Ctx&,void (*)(Ctx&));
}
namespace {
void require(bool yes,const char* why) { if(!yes) throw std::runtime_error(why); }
std::atomic<unsigned> hle_calls{0};
void hle_fixture(ppu::Ctx& c) { ++hle_calls;c.r[3]=789; }
}
int main() {
 try {
  require(rt::vm::init(),"real VM initialization");
  rt::ppu_runtime_init();
  auto low=rt::find_function(0x10000),high=rt::find_function(0x10100);
  require(low && high && !rt::find_function(0x10004) && !rt::find_function(0x10001) && !rt::find_function(0xffff),"sparse function table bounds/holes");
  ppu::Ctx c{};low(c);require(c.r[3]==123,"low registered function");high(c);require(c.r[3]==456,"high registered function");
  auto opd=rt::hle_internal_opd("native-test",hle_fixture);
  require(ppu::r32(opd)==rt::kHleCodeBase && ppu::r32(opd+4)==0,"real HLE OPD creation");
  // Imports may be added while several guest dispatchers hold references to old entries.
  std::thread registrar([] {
    for(unsigned n=0;n<4096;++n) rt::hle_internal_opd(("parallel-"+std::to_string(n)).c_str(),hle_fixture);
  });
  std::thread dispatchers[4];
  for(auto& t:dispatchers)t=std::thread([] {
    ppu::Ctx local{};
    for(unsigned n=0;n<4000;++n)ppu::icall(local,rt::kHleCodeBase);
  });
  registrar.join();for(auto& t:dispatchers)t.join();
  require(hle_calls==16000,"HLE concurrent registration/dispatch lost calls");
  auto t0=ppu::timebase(); acfa::host::sleep_us(10000);auto dt=ppu::timebase()-t0;
  require(dt>=79800000ull/500 && dt<79800000ull,"79.8MHz monotonic timebase");

  constexpr u32 base=0x20000;
  require(rt::vm::alloc_fixed(rt::vm::Area::Main,base,4096),"reservation test memory");
  ppu::w32(base,0x11223344);require(ppu::lwarx32(c,base)==0x11223344,"big endian reserving load32");
  require(ppu::stcx32(c,base,0xaabbccdd) && ppu::r32(base)==0xaabbccdd,"conditional store32");
  require(!ppu::stcx32(c,base,0),"conditional store consumes reservation");
  ppu::w64(base+8,0x1122334455667788ull);
  require(ppu::ldarx64(c,base+8)==0x1122334455667788ull && ppu::stcx64(c,base+8,0xffeeddccbbaa9988ull),"big endian reservation64");
  (void)ppu::lwarx32(c,base);ppu::w32(base+64,1);
  require(!ppu::stcx32(c,base,1),"another word in reservation granule invalidates store");
  ppu::w32(base,0);
  constexpr unsigned iterations=10000,workers=4;
  std::atomic<unsigned> successes{0};std::thread threads[workers];
  for(auto& t:threads) t=std::thread([&] {
    ppu::Ctx local{};
    for(unsigned n=0;n<iterations;) {
      auto value=ppu::lwarx32(local,base);
      if(ppu::stcx32(local,base,value+1)){++n;++successes;}
    }
  });
  for(auto& t:threads)t.join();
  require(ppu::r32(base)==iterations*workers && successes==iterations*workers,"racing PPU reservations lost increments");
  // A notify can happen before the waiter actually starts sleeping, or while it sleeps.
  for(unsigned i=0;i<64;++i) {
    std::atomic<bool> ready{false};std::atomic<bool> woke{false};
    std::atomic<u64> waited_us{0};
    std::thread t([&] {
      auto seq=rt::res::begin_wait();ready=true;
      auto wait_start=acfa::host::monotonic_time_us();
      rt::res::wait(seq,100000);waited_us=acfa::host::monotonic_time_us()-wait_start;
      woke=true;rt::res::end_wait();
    });
    while(!ready.load()) acfa::host::yield();
    if(i&1)acfa::host::sleep_us(1000);
    rt::res::notify();t.join();require(woke && rt::res::g_waiters==0,"reservation notification/waiter balance");
    require(waited_us<75000,"reservation waiter timed out instead of receiving notification");
  }
  // A separate child verifies the actual signal diagnostic/termination path safely.
  int diag[2];require(pipe(diag)==0,"fault diagnostic pipe");
  pid_t pid=fork();require(pid>=0,"fault child creation");
  if(pid==0){
    close(diag[0]);dup2(diag[1],STDERR_FILENO);close(diag[1]);
    rt::PpuThread fixture;fixture.id=0xabc;fixture.ctx.r[3]=1;fixture.ctx.lr=0xd356a8;
    rt::test_cur_thread=&fixture;
    rt::install_fault_handler();raise(SIGBUS);_exit(1);
  }
  close(diag[1]);char diagnostic[4096];std::string captured;
  for(ssize_t n;(n=read(diag[0],diagnostic,sizeof diagnostic))>0;)captured.append(diagnostic,size_t(n));
  close(diag[0]);
  int status=0;require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==128+SIGBUS,"host fault handler exit");
  require(captured.find("PPU context snapshot thread=0x0000000000000abc")!=std::string::npos &&
          captured.find("r03=0x0000000000000001")!=std::string::npos &&
          captured.find("image=0x0000000000000000")==std::string::npos,"signal-safe native/PPU snapshot");
  require(rt::vm::alloc_fixed(rt::vm::Area::Main,0x60000,8192),"compressor lock fixture memory");
  ppu::w32(0x60000+100,0x60200);ppu::w32(0x60200,4);
  auto lock=[](ppu::Ctx& c){ppu::w32(u32(c.r[5]),256);ppu::w32(u32(c.r[5])+4,0x61000);c.r[3]=1;};
  auto init_lock=[](ppu::Ctx& c){c.r[3]=0x60000;c.r[4]=0;c.r[5]=0x60300;c.r[6]=0;c.r[7]=0;};
  init_lock(c);ppu::hook_compression_texture_lock(c,lock);
  require(rt::rsx::texture_read_requests==0,"ordinary texture locks must not wait for GPU");
  auto compress=[](ppu::Ctx& c){
    c.r[3]=0x60000;c.r[4]=0;c.r[5]=0x60300;c.r[6]=0;c.r[7]=0;
    ppu::hook_compression_texture_lock(c,[](ppu::Ctx& c){ppu::w32(u32(c.r[5]),256);ppu::w32(u32(c.r[5])+4,0x61000);c.r[3]=1;});
    require(c.r[3]==1&&ppu::r32(0x61000)==0x11223344,"original compression sees synchronized pixels");
    c.r[3]=0x60000;c.r[7]=1;
    ppu::hook_compression_texture_lock(c,[](ppu::Ctx& c){c.r[3]=1;});
    c.r[3]=0x60000;c.r[7]=0;
    ppu::hook_compression_texture_lock(c,[](ppu::Ctx& c){c.r[3]=0;});
    c.r[3]=77;
  };
  ppu::hook_texture_compress(c,compress);
  require(c.r[3]==77&&rt::rsx::texture_read_requests==1,"only successful compressor source read synchronizes; return preserved");
  init_lock(c);ppu::hook_compression_texture_lock(c,lock);
  require(rt::rsx::texture_read_requests==1,"compressor scope restored after return");
  rt::mac_memory::release();
  std::cout<<"PASS actual PPU core: sparse table/HLE OPD/16000 concurrent dispatches+4096 registrations, 79.8MHz timebase, 40000 racing reservations, granule conflicts, 64 wake races, fault child\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
