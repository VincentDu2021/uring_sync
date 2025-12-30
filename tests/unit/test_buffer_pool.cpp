#include <gtest/gtest.h>
#include "common.hpp"
#include <cstdint>

class BufferPoolTest : public ::testing::Test {
protected:
    static constexpr size_t kDefaultCount = 4;
    static constexpr size_t kDefaultSize = 4096;
};

TEST_F(BufferPoolTest, Construction) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    EXPECT_EQ(pool.buffer_size(), kDefaultSize);
    EXPECT_EQ(pool.available_count(), kDefaultCount);
    EXPECT_EQ(pool.buffers().size(), kDefaultCount);
}

TEST_F(BufferPoolTest, AcquireSingle) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    auto [ptr, index] = pool.acquire();

    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(index, 0);
    EXPECT_EQ(pool.available_count(), kDefaultCount - 1);
}

TEST_F(BufferPoolTest, AcquireAll) {
    BufferPool pool(kDefaultCount, kDefaultSize);
    std::vector<std::pair<char*, int>> acquired;

    for (size_t i = 0; i < kDefaultCount; i++) {
        auto [ptr, index] = pool.acquire();
        EXPECT_NE(ptr, nullptr);
        EXPECT_EQ(index, static_cast<int>(i));
        acquired.push_back({ptr, index});
    }

    EXPECT_EQ(pool.available_count(), 0);

    // Verify all pointers are unique
    for (size_t i = 0; i < acquired.size(); i++) {
        for (size_t j = i + 1; j < acquired.size(); j++) {
            EXPECT_NE(acquired[i].first, acquired[j].first);
        }
    }
}

TEST_F(BufferPoolTest, AcquireWhenExhausted) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    // Exhaust the pool
    for (size_t i = 0; i < kDefaultCount; i++) {
        pool.acquire();
    }

    // Next acquire should fail
    auto [ptr, index] = pool.acquire();

    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(index, -1);
}

TEST_F(BufferPoolTest, ReleaseAndReacquire) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    auto [ptr1, index1] = pool.acquire();
    EXPECT_EQ(pool.available_count(), kDefaultCount - 1);

    pool.release(index1);
    EXPECT_EQ(pool.available_count(), kDefaultCount);

    auto [ptr2, index2] = pool.acquire();
    EXPECT_EQ(ptr2, ptr1);  // Same buffer reused
    EXPECT_EQ(index2, index1);
}

TEST_F(BufferPoolTest, ReleaseInvalidIndex) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    // Release with invalid indices should not crash
    pool.release(-1);
    pool.release(-100);
    pool.release(static_cast<int>(kDefaultCount));
    pool.release(static_cast<int>(kDefaultCount + 100));

    // Pool should remain unchanged
    EXPECT_EQ(pool.available_count(), kDefaultCount);
}

TEST_F(BufferPoolTest, AvailableCount) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    EXPECT_EQ(pool.available_count(), kDefaultCount);

    std::vector<int> indices;
    for (size_t i = 0; i < kDefaultCount; i++) {
        auto [ptr, index] = pool.acquire();
        indices.push_back(index);
        EXPECT_EQ(pool.available_count(), kDefaultCount - i - 1);
    }

    for (size_t i = 0; i < kDefaultCount; i++) {
        pool.release(indices[i]);
        EXPECT_EQ(pool.available_count(), i + 1);
    }
}

TEST_F(BufferPoolTest, BufferAlignment) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    for (size_t i = 0; i < kDefaultCount; i++) {
        auto [ptr, index] = pool.acquire();
        EXPECT_NE(ptr, nullptr);
        // Check 4096-byte alignment for O_DIRECT
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 4096, 0);
    }
}

TEST_F(BufferPoolTest, BufferSize) {
    constexpr size_t customSize = 8192;
    BufferPool pool(2, customSize);

    EXPECT_EQ(pool.buffer_size(), customSize);
}

TEST_F(BufferPoolTest, BuffersAccessor) {
    BufferPool pool(kDefaultCount, kDefaultSize);

    const auto& buffers = pool.buffers();
    EXPECT_EQ(buffers.size(), kDefaultCount);

    for (const auto* buf : buffers) {
        EXPECT_NE(buf, nullptr);
    }
}
