// fastpdf2png - Thread-local memory pool
// SPDX-License-Identifier: MIT

#ifndef FASTPDF2PNG_MEMORY_POOL_H_
#define FASTPDF2PNG_MEMORY_POOL_H_

#include <cstdint>
#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#include <malloc.h>
inline void* aligned_alloc_portable(size_t align, size_t size) {
  return _aligned_malloc(size, align);
}
inline void aligned_free_portable(void* ptr) { _aligned_free(ptr); }
#else
inline void* aligned_alloc_portable(size_t align, size_t size) {
  // C11/C++17 std::aligned_alloc requires size to be a multiple of align
  size = (size + align - 1) & ~(align - 1);
  return std::aligned_alloc(align, size);
}
inline void aligned_free_portable(void* ptr) { std::free(ptr); }
#endif

#if defined(__linux__)
#include <sys/mman.h>
#define USE_HUGE_PAGES 1
#endif

namespace fast_png {

constexpr size_t kCacheLineSize = 64;
constexpr size_t kHugePageThreshold = 2 * 1024 * 1024;

class PageMemoryPool {
 public:
  PageMemoryPool() = default;
  ~PageMemoryPool() { Release(); }
  PageMemoryPool(const PageMemoryPool&) = delete;
  PageMemoryPool& operator=(const PageMemoryPool&) = delete;

  uint8_t* Acquire(size_t size) {
    if (size > capacity_) {
      Release();
      size_t new_capacity = size + size / 4;

#if USE_HUGE_PAGES
      if (new_capacity >= kHugePageThreshold) {
        buffer_ = static_cast<uint8_t*>(
            mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (buffer_ != MAP_FAILED) {
          madvise(buffer_, new_capacity, MADV_HUGEPAGE);
          capacity_ = new_capacity;
          use_mmap_ = true;
          return buffer_;
        }
        buffer_ = nullptr;
      }
#endif
      buffer_ = static_cast<uint8_t*>(
          aligned_alloc_portable(kCacheLineSize, new_capacity));
      if (buffer_) {
        capacity_ = new_capacity;
        use_mmap_ = false;
      }
    }
    return buffer_;
  }

 private:
  void Release() {
    if (!buffer_) return;
#if USE_HUGE_PAGES
    if (use_mmap_) munmap(buffer_, capacity_);
    else aligned_free_portable(buffer_);
#else
    aligned_free_portable(buffer_);
#endif
    buffer_ = nullptr;
    capacity_ = 0;
  }

  uint8_t* buffer_ = nullptr;
  size_t capacity_ = 0;
  bool use_mmap_ = false;
};

// thread_local ensures each fork'd child gets its own pool instance
inline PageMemoryPool& GetProcessLocalPool() {
  static thread_local PageMemoryPool pool;
  return pool;
}

}  // namespace fast_png

#endif  // FASTPDF2PNG_MEMORY_POOL_H_
