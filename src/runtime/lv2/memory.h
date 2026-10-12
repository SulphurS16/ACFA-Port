#pragma once
#include "common/bytes.h"

namespace rt::lv2 {

void memory_charge(u64 bytes);  // account fixed allocations (ELF image, primary stack)
s32 mmapper_allocate_memory(u64 size, u64 flags, u32* mem_id);
s32 mmapper_free_memory(u32 mem_id);
s32 mmapper_unmap_memory(u32 addr, u32* mem_id);

}  // namespace rt::lv2
