#pragma once

#include <google/protobuf/arena.h>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <deque>

namespace NYdb {
namespace NConsoleClient {

/**
 * RAII wrapper for protobuf Arena that automatically returns it to the pool in destructor
 */
class TPooledArena {
public:
    TPooledArena(std::shared_ptr<google::protobuf::Arena> arena, class TArenaPool* pool);
    ~TPooledArena();
    TPooledArena(TPooledArena&& other) noexcept;
    TPooledArena& operator=(TPooledArena&& other) noexcept;
    TPooledArena(const TPooledArena&) = delete;
    TPooledArena& operator=(const TPooledArena&) = delete;

    google::protobuf::Arena* Get() const {
        return Arena_.get();
    }

    google::protobuf::Arena* operator->() const {
        return Arena_.get();
    }

    google::protobuf::Arena& operator*() const {
        return *Arena_;
    }

    explicit operator bool() const {
        return Arena_ != nullptr;
    }

private:
    std::shared_ptr<google::protobuf::Arena> Arena_;
    class TArenaPool* Pool_;
};

/**
 * Thread-safe pool for reusing protobuf Arena objects
 */
class TArenaPool {
public:
    struct TConfig {
        size_t MaxPoolSize;
        size_t MaxIdleTimeSeconds;
        size_t CleanupIntervalSeconds;
        TConfig()
            : MaxPoolSize(10)
            , MaxIdleTimeSeconds(300)
            , CleanupIntervalSeconds(10)
        {}
    };
    explicit TArenaPool(const TConfig& config = TConfig{});
    ~TArenaPool();
    TPooledArena Acquire();
    void Release(std::shared_ptr<google::protobuf::Arena> arena);
    struct TStats {
        size_t PoolSize;
        size_t TotalArenasCreated;
        size_t TotalArenasReused;
    };
    TStats GetStats() const;
    void Cleanup();
private:
    struct TArenaInfo {
        std::shared_ptr<google::protobuf::Arena> Arena;
        std::chrono::steady_clock::time_point LastUsed;
    };
    void CleanupIdleArenas();
    void BackgroundCleanupThread();
    TConfig Config_;
    mutable std::mutex Mutex_;
    std::deque<TArenaInfo> Pool_;
    std::atomic<size_t> TotalArenasCreated_{0};
    std::atomic<size_t> TotalArenasReused_{0};
    std::chrono::steady_clock::time_point LastCleanupTime_;
    std::thread CleanupThread_;
    std::atomic<bool> StopFlag_{false};
    std::condition_variable_any StopCv_;
};

} // namespace NConsoleClient
} // namespace NYdb
