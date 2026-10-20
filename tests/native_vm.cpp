#include <iostream>
#include <stdexcept>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "platform/macos/guest_memory.h"
#include "runtime/memory.h"
#include "runtime/log.h"
#include "runtime/ppu_thread.h"
#include "ppu/ppu_recomp.h"
// Only logging and current-thread provenance are fixtures. VM allocation/protection code is real.
namespace rt {
PpuThread* cur_thread(){return nullptr;}
std::string guest_backtrace(const ppu::Ctx&){return "<host test>";}
double log_time(){return 0;}
Level log_level(){return Level::Error;}
void log_write(Level,const char*,const std::string&){}
[[noreturn]] void fatal(const std::string&s){throw std::runtime_error(s);}
}
static void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
static vm_prot_t protection(u64 at){
 mach_vm_address_t address=at;mach_vm_size_t size=0;
 vm_region_basic_info_data_64_t info{};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=0;
 auto kr=mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&object);
 if(object)mach_port_deallocate(mach_task_self(),object);
 check(kr==KERN_SUCCESS&&address<=at,"VM region lookup");return info.protection;
}
int main(){
 try{
  check(rt::vm::init(),"real VM reserve failed");auto hp=rt::mac_memory::host_page_size();
  check(protection(PPU_MEM_BASE+0x10000)==VM_PROT_NONE,"reservation not guarded");
  rt::vm::commit(0x10000,4096);ppu::w32(0x10000,0x11223344);
  rt::vm::commit(0x11000,4096);ppu::w32(0x11000,0xaabbccdd);
  rt::vm::decommit(0x10000,4096);
  check(!rt::vm::committed(0x10000)&&rt::vm::committed(0x11000),"guest subpage tracking");
  check(ppu::r32(0x11000)==0xaabbccdd,"neighbor damaged by decommit");
  rt::vm::commit(0x10000,4096);check(ppu::r32(0x10000)==0,"recommitted subpage not zero");
  rt::vm::decommit(0x10000,8192);
  check(protection(PPU_MEM_BASE+0x10000)==VM_PROT_NONE,"dead host page not protected");
  check(!rt::mac_memory::commit(0xfffff000ull,0x100001000ull),"guest wraparound accepted");
  auto a=rt::vm::alloc(rt::vm::Area::Stack,0x10000);
  check(a&&a%hp==0,"stack alignment");
  check(protection(PPU_MEM_BASE+a-hp)==VM_PROT_NONE,"stack guard not a full host page");
  ppu::w32(a,0x88776655);check(ppu::r32(a)==0x88776655,"stack access");rt::vm::free(rt::vm::Area::Stack,a);
  rt::mac_memory::release();
  std::cout<<"PASS real VM: host page "<<hp<<", 4K neighbor isolation/zeroing, full stack guard, overflow rejection\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
