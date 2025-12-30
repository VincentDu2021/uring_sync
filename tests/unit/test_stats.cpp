#include <gtest/gtest.h>
#include "common.hpp"
#include <thread>
#include <vector>

class StatsTest : public ::testing::Test {
protected:
    Stats stats;
};

TEST_F(StatsTest, InitialValues) {
    EXPECT_EQ(stats.files_total.load(), 0);
    EXPECT_EQ(stats.files_completed.load(), 0);
    EXPECT_EQ(stats.files_failed.load(), 0);
    EXPECT_EQ(stats.bytes_total.load(), 0);
    EXPECT_EQ(stats.bytes_copied.load(), 0);
    EXPECT_EQ(stats.dirs_created.load(), 0);
}

TEST_F(StatsTest, IncrementSingle) {
    stats.files_total++;
    EXPECT_EQ(stats.files_total.load(), 1);

    stats.files_completed++;
    EXPECT_EQ(stats.files_completed.load(), 1);

    stats.files_failed++;
    EXPECT_EQ(stats.files_failed.load(), 1);

    stats.bytes_total += 1024;
    EXPECT_EQ(stats.bytes_total.load(), 1024);

    stats.bytes_copied += 512;
    EXPECT_EQ(stats.bytes_copied.load(), 512);

    stats.dirs_created++;
    EXPECT_EQ(stats.dirs_created.load(), 1);
}

TEST_F(StatsTest, IncrementMultiple) {
    for (int i = 0; i < 100; i++) {
        stats.files_total++;
        stats.bytes_total += 4096;
    }

    EXPECT_EQ(stats.files_total.load(), 100);
    EXPECT_EQ(stats.bytes_total.load(), 100 * 4096);
}

TEST_F(StatsTest, ConcurrentIncrement) {
    constexpr int kNumThreads = 8;
    constexpr int kIncrementsPerThread = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumThreads; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIncrementsPerThread; i++) {
                stats.files_total++;
                stats.files_completed++;
                stats.bytes_total += 100;
                stats.bytes_copied += 100;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify no lost updates
    const uint64_t expected_count = kNumThreads * kIncrementsPerThread;
    const uint64_t expected_bytes = expected_count * 100;

    EXPECT_EQ(stats.files_total.load(), expected_count);
    EXPECT_EQ(stats.files_completed.load(), expected_count);
    EXPECT_EQ(stats.bytes_total.load(), expected_bytes);
    EXPECT_EQ(stats.bytes_copied.load(), expected_bytes);
}

TEST_F(StatsTest, FetchAdd) {
    uint64_t old_value = stats.bytes_copied.fetch_add(100);
    EXPECT_EQ(old_value, 0);
    EXPECT_EQ(stats.bytes_copied.load(), 100);

    old_value = stats.bytes_copied.fetch_add(200);
    EXPECT_EQ(old_value, 100);
    EXPECT_EQ(stats.bytes_copied.load(), 300);
}

TEST_F(StatsTest, Store) {
    stats.files_total.store(42);
    EXPECT_EQ(stats.files_total.load(), 42);

    stats.bytes_total.store(1024 * 1024);
    EXPECT_EQ(stats.bytes_total.load(), 1024 * 1024);
}
