#pragma once
#include "common/bytes.h"
namespace rt::mac_memory {
bool reserve();
void release();
u32 host_page_size();
// Guest granularity stays 4 KiB. macOS protection is coalesced at the real host page size.
bool commit(u64 begin,u64 end);
bool decommit(u64 begin,u64 end);
}
