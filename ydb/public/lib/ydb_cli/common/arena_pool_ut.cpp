#include "arena_pool.h"
#include <library/cpp/testing/unittest/registar.h>
#include <thread>
#include <chrono>

using namespace NYdb::NConsoleClient;

Y_UNIT_TEST_SUITE(TArenaPoolTest) {

Y_UNIT_TEST(BasicAcquireRelease) {
    TArenaPool pool;

    // Acquire arena
    auto arena1 = pool.Acquire();
    UNIT_ASSERT(arena1);
    UNIT_ASSERT(arena1.Get() != nullptr);

    // Release by destroying arena
    {
        auto arena2 = pool.Acquire();
        UNIT_ASSERT(arena2);
    } // arena2 destroyed here, arena returned to pool

    // Acquire again - should get the same arena back
    auto arena3 = pool.Acquire();
    UNIT_ASSERT(arena3);
}

Y_UNIT_TEST(PoolSizeLimit) {
    TArenaPool::TConfig config;
    config.MaxPoolSize = 2;
    TArenaPool pool(config);

    std::vector<TPooledArena> arenas;

    // Create more arenas than pool size
    for (size_t i = 0; i < 5; ++i) {
        arenas.push_back(pool.Acquire());
        UNIT_ASSERT(arenas.back());
    }

    // Check stats
    auto stats = pool.GetStats();
    UNIT_ASSERT_EQUAL(stats.TotalArenasCreated, 5);
    UNIT_ASSERT_EQUAL(stats.PoolSize, 0); // All arenas are in use

    // Release all arenas
    arenas.clear();

    // Check stats again
    stats = pool.GetStats();
    UNIT_ASSERT_EQUAL(stats.PoolSize, 2); // Only 2 arenas in pool due to size limit
    UNIT_ASSERT_EQUAL(stats.TotalArenasReused, 0); // No reuses yet

    // Acquire again - should reuse from pool
    auto arena = pool.Acquire();
    UNIT_ASSERT(arena);

    stats = pool.GetStats();
    UNIT_ASSERT_EQUAL(stats.TotalArenasReused, 1);
}

Y_UNIT_TEST(MoveSemantics) {
    TArenaPool pool;

    auto arena1 = pool.Acquire();
    UNIT_ASSERT(arena1);

    // Move constructor
    TPooledArena arena2(std::move(arena1));
    UNIT_ASSERT(!arena1); // Original should be invalid
    UNIT_ASSERT(arena2);  // Moved should be valid

    // Move assignment
    TPooledArena arena3 = pool.Acquire();
    arena3 = std::move(arena2);
    UNIT_ASSERT(!arena2); // Original should be invalid
    UNIT_ASSERT(arena3);  // Moved should be valid
}

Y_UNIT_TEST(ThreadSafety) {
    TArenaPool pool;
    std::vector<std::thread> threads;
    std::atomic<size_t> totalAcquired{0};

    // Create multiple threads that acquire and release arenas
    for (size_t i = 0; i < 10; ++i) {
        threads.emplace_back([&pool, &totalAcquired]() {
            for (size_t j = 0; j < 100; ++j) {
                auto arena = pool.Acquire();
                UNIT_ASSERT(arena);
                totalAcquired.fetch_add(1);

                // Simulate some work
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    UNIT_ASSERT_EQUAL(totalAcquired.load(), 1000);

    // Check that pool is still functional and statistics are sane
    auto stats = pool.GetStats();
    UNIT_ASSERT(stats.TotalArenasCreated > 0);
    UNIT_ASSERT(stats.TotalArenasReused > 0); // There was at least one reuse
    UNIT_ASSERT(stats.PoolSize <= 10); // Default MaxPoolSize is 10
}

Y_UNIT_TEST(Cleanup) {
    TArenaPool::TConfig config;
    config.MaxIdleTimeSeconds = 1; // Very short idle time for testing
    config.CleanupIntervalSeconds = 1;
    TArenaPool pool(config);

    // Acquire and release arena
    {
        auto arena = pool.Acquire();
        UNIT_ASSERT(arena);
    }

    auto stats = pool.GetStats();
    UNIT_ASSERT_EQUAL(stats.PoolSize, 1);

    // Wait for cleanup
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Force cleanup
    pool.Cleanup();

    stats = pool.GetStats();
    UNIT_ASSERT_EQUAL(stats.PoolSize, 0); // Arena should be cleaned up
}

} // Y_UNIT_TEST_SUITE 