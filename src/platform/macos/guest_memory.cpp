#include "platform/macos/guest_memory.h"
#include "ppu/ppu_recomp.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <vector>
namespace rt::mac_memory {
namespace {
constexpr u64 size=0x100000000ull;
std::mutex mutex;
bool reserved=false;
u32 page=0;
std::vector<u64> live;
bool valid(u64 b,u64 e){return reserved&&b<=e&&e<=size&&!(b&4095)&&!(e&4095);}
}
u32 host_page_size(){static const u32 value=u32(sysconf(_SC_PAGESIZE));return value;}
bool reserve(){
 std::lock_guard guard(mutex);
 if(reserved)return false;
 auto p=host_page_size();page=p;
 if(p<4096||p%4096||p/4096>64||(p&(p-1)))return false;
 mach_vm_address_t at=PPU_MEM_BASE;
 auto kr=mach_vm_allocate(mach_task_self(),&at,size,VM_FLAGS_FIXED);
 if(kr!=KERN_SUCCESS||at!=PPU_MEM_BASE)return false;
 kr=mach_vm_protect(mach_task_self(),at,size,false,VM_PROT_NONE);
 if(kr!=KERN_SUCCESS){mach_vm_deallocate(mach_task_self(),at,size);return false;}
 live.assign(size/p,0);reserved=true;return true;
}
void release(){
 std::lock_guard guard(mutex);
 if(reserved)mach_vm_deallocate(mach_task_self(),PPU_MEM_BASE,size);
 live.clear();reserved=false;
}
bool commit(u64 b,u64 e){
 std::lock_guard guard(mutex);if(!valid(b,e))return false;
 for(u64 g=b;g<e;g+=4096){
  auto index=g/page;auto bit=u64(1)<<((g%page)/4096);
  if(live[index]&bit)continue;
  auto addr=PPU_MEM_BASE+index*page;
  if(!live[index]&&mach_vm_protect(mach_task_self(),addr,page,false,VM_PROT_READ|VM_PROT_WRITE)!=KERN_SUCCESS)return false;
  // A previously freed guest subpage may share a live host page; every fresh commit must be zero.
  std::memset(reinterpret_cast<void*>(PPU_MEM_BASE+g),0,4096);live[index]|=bit;
 }
 return true;
}
bool decommit(u64 b,u64 e){
 std::lock_guard guard(mutex);if(!valid(b,e))return false;
 for(u64 g=b;g<e;g+=4096){
  auto index=g/page;auto bit=u64(1)<<((g%page)/4096);
  if(!(live[index]&bit))continue;
  live[index]&=~bit;
  if(!live[index]){
   auto addr=PPU_MEM_BASE+index*page;
   if(mach_vm_protect(mach_task_self(),addr,page,false,VM_PROT_NONE)!=KERN_SUCCESS)return false;
   madvise(reinterpret_cast<void*>(addr),page,MADV_DONTNEED);
  }
 }
 return true;
}
}
