#pragma once
// Small helpers for recognising the game's shaders from the device hooks.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dlaa_core {

// Lock-free set of pointers (shader objects, buffers), readable from any
// thread. The game creates the same shader many times, so sets can grow to
// thousands.
template <size_t kSize>
class PointerSet {
 public:
  // False only when the set is full and `p` is not in it.
  bool Insert(const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      uintptr_t cur = slots_[i].load(std::memory_order_acquire);
      if (cur == v) return true;
      const bool empty = cur == 0;
      if ((empty || cur == kTombstone) && slots_[i].compare_exchange_strong(cur, v)) {
        if (empty) used_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      if (cur == v) return true;
    }
    return false;
  }
  // Slots ever filled (tombstones included): what a failed Contains probes through.
  size_t Used() const { return used_.load(std::memory_order_relaxed); }
  void Erase(const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      uintptr_t cur = slots_[i].load(std::memory_order_acquire);
      if (cur == 0) return;
      if (cur == v) slots_[i].compare_exchange_strong(cur, kTombstone);
    }
  }
  // Empties the set. Nothing may Insert, Erase or Contains at the same time.
  void Clear() {
    for (auto& slot : slots_) slot.store(0, std::memory_order_relaxed);
    used_.store(0, std::memory_order_relaxed);
  }
  bool Contains(const void* p) const {
    if (!p) return false;
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      const uintptr_t cur = slots_[i].load(std::memory_order_relaxed);
      if (cur == v) return true;
      if (cur == 0) return false;
    }
    return false;
  }

 private:
  static constexpr uintptr_t kTombstone = 1;
  static_assert((kSize & (kSize - 1)) == 0);
  static size_t Slot(uintptr_t v) { return ((v >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (kSize - 1); }
  std::atomic<uintptr_t> slots_[kSize] = {};
  std::atomic<size_t> used_{0};
};

// True if the shader bytecode contains `text` (names in its reflection data).
inline bool BytecodeContains(const void* code, size_t length, const char* text) {
  if (!code) return false;
  const auto* begin = static_cast<const char*>(code);
  const auto* end = begin + length;
  return std::search(begin, end, text, text + std::strlen(text)) != end;
}

}  // namespace dlaa_core
