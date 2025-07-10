#include "arena_pool.h"
#include <chrono>

namespace NYdb {
namespace NConsoleClient {

// TPooledArena implementation
TPooledArena::TPooledArena(std::shared_ptr<google::protobuf::Arena> arena, TArenaPool* pool)
    : Arena_(std::move(arena))
    , Pool_(pool)
{
}

TPooledArena::~TPooledArena() {
    if (Arena_ && Pool_) {
        Pool_->Release(std::move(Arena_));
    }
}

TPooledArena::TPooledArena(TPooledArena&& other) noexcept
    : Arena_(std::move(other.Arena_))
    , Pool_(other.Pool_)
{
    other.Pool_ = nullptr;
}

TPooledArena& TPooledArena::operator=(TPooledArena&& other) noexcept {
    if (this != &other) {
        if (Arena_ && Pool_) {
            Pool_->Release(std::move(Arena_));
        }
        Arena_ = std::move(other.Arena_);
        Pool_ = other.Pool_;
        other.Pool_ = nullptr;
    }
    return *this;
}

// TArenaPool implementation
TArenaPool::TArenaPool(const TConfig& config)
    : Config_(config)
    , LastCleanupTime_(std::chrono::steady_clock::now())
{
    StopFlag_ = false;
    CleanupThread_ = std::thread([this]() { BackgroundCleanupThread(); });
}

TArenaPool::~TArenaPool() {
    StopFlag_ = true;
    StopCv_.notify_all();
    if (CleanupThread_.joinable()) {
        CleanupThread_.join();
    }
    std::lock_guard<std::mutex> lock(Mutex_);
    Pool_.clear();
}

TPooledArena TArenaPool::Acquire() {
    std::lock_guard<std::mutex> lock(Mutex_);
    if (!Pool_.empty()) {
        auto arenaInfo = std::move(Pool_.back());
        Pool_.pop_back();
        TotalArenasReused_.fetch_add(1);
        return TPooledArena(std::move(arenaInfo.Arena), this);
    }
    if (Pool_.size() >= Config_.MaxPoolSize) {
        // Should not happen, but just in case
        Pool_.pop_front();
    }
    auto newArena = std::make_shared<google::protobuf::Arena>();
    TotalArenasCreated_.fetch_add(1);
    return TPooledArena(std::move(newArena), this);
}

void TArenaPool::Release(std::shared_ptr<google::protobuf::Arena> arena) {
    if (!arena) {
        return;
    }
    std::lock_guard<std::mutex> lock(Mutex_);
    if (Pool_.size() < Config_.MaxPoolSize) {
        arena->Reset();
        TArenaInfo info;
        info.Arena = std::move(arena);
        info.LastUsed = std::chrono::steady_clock::now();
        Pool_.push_back(std::move(info));
    }
    // else: just drop the arena
}

TArenaPool::TStats TArenaPool::GetStats() const {
    std::lock_guard<std::mutex> lock(Mutex_);
    TStats stats;
    stats.PoolSize = Pool_.size();
    stats.TotalArenasCreated = TotalArenasCreated_.load();
    stats.TotalArenasReused = TotalArenasReused_.load();
    return stats;
}

void TArenaPool::CleanupIdleArenas() {
    std::lock_guard<std::mutex> lock(Mutex_);
    auto now = std::chrono::steady_clock::now();
    auto maxIdleTime = std::chrono::seconds(Config_.MaxIdleTimeSeconds);
    while (!Pool_.empty() && now - Pool_.front().LastUsed > maxIdleTime) {
        Pool_.pop_front();
    }
    LastCleanupTime_ = std::chrono::steady_clock::now();
}

void TArenaPool::BackgroundCleanupThread() {
    while (!StopFlag_) {
        for (size_t i = 0; i < Config_.CleanupIntervalSeconds && !StopFlag_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (StopFlag_) {
            break;
        }
        CleanupIdleArenas();
    }
}

} // namespace NConsoleClient
} // namespace NYdb
