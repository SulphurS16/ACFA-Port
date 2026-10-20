// JP TextureCompressor::Compress (0x003fdfa0) reads a CPU-locked source texture
// before encoding BC1/BC3. Unlike RSX copy commands this is a direct PPU read.
// The original compressor, saved preview layout and DXT encoder remain intact.
#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/rsx/rsx.h"
namespace ppu {
namespace { thread_local unsigned compression_scope=0; }
void hook_texture_compress(Ctx& ctx,void (*orig)(Ctx&)) {
  struct Scope { Scope(){++compression_scope;} ~Scope(){--compression_scope;} } scope;
  orig(ctx);
}
void hook_compression_texture_lock(Ctx& ctx,void (*orig)(Ctx&)) {
  const u32 object=u32(ctx.r[3]),level=u32(ctx.r[4]),locked=u32(ctx.r[5]);
  const bool read=compression_scope&&ctx.r[7]==0&&ctx.r[6]==0&&level==0;
  orig(ctx);
  if(!read||ctx.r[3]==0||!rt::vm::committed(object,104)||!rt::vm::committed(locked,8))return;
  const u32 height_table=r32(object+100);
  if(!rt::vm::committed(height_table,4))return;
  const u32 height=r32(height_table),pitch=r32(locked),ea=r32(locked+4);
  const u64 size=u64(pitch)*height;
  if(!height||!pitch||size>64u*1024*1024||!rt::vm::committed(ea,u32(size)))return;
  const bool copied=rt::rsx::read_rendered_memory(ea,u32(size));
  LOG_I("gpu","texture compressor CPU source: guest 0x%08x pitch %u rows %u; GPU readback %s",ea,pitch,height,copied?"completed":"not needed or unavailable");
}
}  // namespace ppu
