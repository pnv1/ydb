#pragma once

#include <util/generic/singleton.h>

#include <arrow/memory_pool.h>

namespace NKikimr::NMiniKQL {

class TArrowMemoryPool : public arrow20::MemoryPool {
public:
    arrow20::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) final;
    arrow20::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) final;
    void Free(uint8_t* buffer, int64_t size, int64_t alignment) final;

    int64_t max_memory() const final {
        return MaxMemory_.load();
    }

    int64_t bytes_allocated() const final {
        return BytesAllocated_.load();
    }

    int64_t total_bytes_allocated() const final {
        return bytes_allocated();
    }

    int64_t num_allocations() const final {
        return 0;
    }

    std::string backend_name() const final {
        return "MKQL";
    }

private:
    inline void UpdateAllocatedBytes(int64_t diff) {
        // inspired by arrow/memory_pool.h impl.
        int64_t allocated = BytesAllocated_.fetch_add(diff) + diff;
        if (diff > 0 && allocated > MaxMemory_) {
            MaxMemory_ = allocated;
        }
    }

private:
    std::atomic<int64_t> BytesAllocated_{0};
    std::atomic<int64_t> MaxMemory_{0};
};

static inline arrow20::MemoryPool* GetArrowMemoryPool() {
    return Singleton<TArrowMemoryPool>();
}

} // namespace NKikimr::NMiniKQL
