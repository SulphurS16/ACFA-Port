#pragma once
#include <cstddef>
#include <cstring>
#include <vector>
namespace rt::gpu {
// Exact byte validation: never treats a hash collision or frame number as proof.
class ContentSnapshot {
  std::vector<unsigned char> bytes_;
 public:
  size_t size() const { return bytes_.size(); }
  const void* data() const { return bytes_.data(); }
  bool matches(const void* source,size_t size) const {
    return size==bytes_.size() && size && std::memcmp(bytes_.data(),source,size)==0;
  }
  void capture(const void* source,size_t size) {
    bytes_.resize(size); if(size)std::memcpy(bytes_.data(),source,size);
  }
  void clear() { std::vector<unsigned char>().swap(bytes_); }
};
}
