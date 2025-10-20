#include "memory_pool.h"

#include <yql/essentials/public/udf/udf_allocator.h>

#include <util/generic/singleton.h>
#include <util/generic/yexception.h>

namespace NYql {
namespace NUdf {

class TYqlMemoryPool: public arrow20::MemoryPool {
    arrow20::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) final {
        Y_ENSURE(size >= 0 && out);
        Y_UNUSED(alignment);
        *out = (uint8_t*)UdfArrowAllocate(size);
        UpdateAllocatedBytes(size);
        return arrow20::Status::OK();
    }

    arrow20::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) final {
        Y_ENSURE(old_size >= 0 && new_size >= 0 && ptr);
        Y_UNUSED(alignment);
        *ptr = (uint8_t*)UdfArrowReallocate(*ptr, old_size, new_size);
        UpdateAllocatedBytes(new_size - old_size);
        return arrow20::Status::OK();
    }

    void Free(uint8_t* buffer, int64_t size, int64_t alignment) final {
        Y_ENSURE(size >= 0);
        Y_UNUSED(alignment);
        UdfArrowFree(buffer, size);
        UpdateAllocatedBytes(-size);
    }

    int64_t max_memory() const final {
        return MaxMemory_.load();
    }

    int64_t bytes_allocated() const final {
        return BytesAllocated_.load();
    }

    int64_t total_bytes_allocated() const final {
        return BytesAllocated_.load();
    }

    int64_t num_allocations() const final {
        return 0;
    }

    inline void UpdateAllocatedBytes(int64_t diff) {
        // inspired by arrow/memory_pool.h impl.
        int64_t allocated = BytesAllocated_.fetch_add(diff) + diff;
        if (diff > 0 && allocated > MaxMemory_) {
            MaxMemory_ = allocated;
        }
    }

    virtual std::string backend_name() const final {
        return "yql";
    }

private:
    std::atomic<int64_t> BytesAllocated_{0};
    std::atomic<int64_t> MaxMemory_{0};
};

arrow20::MemoryPool* GetYqlMemoryPool() {
    return Singleton<TYqlMemoryPool>();
}

} // namespace NUdf
} // namespace NYql
