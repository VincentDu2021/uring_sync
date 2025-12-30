#include <gtest/gtest.h>
#include "common.hpp"
#include <thread>
#include <vector>
#include <set>
#include <chrono>
#include <atomic>

class WorkQueueTest : public ::testing::Test {
protected:
    WorkQueue<int> queue;
};

TEST_F(WorkQueueTest, PushAndTryPop) {
    queue.push(42);

    int value;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 42);
}

TEST_F(WorkQueueTest, TryPopEmpty) {
    int value;
    EXPECT_FALSE(queue.try_pop(value));
}

TEST_F(WorkQueueTest, PushBulk) {
    std::vector<int> items = {1, 2, 3, 4, 5};
    queue.push_bulk(items);

    EXPECT_EQ(queue.size(), 5);

    for (int expected = 1; expected <= 5; expected++) {
        int value;
        EXPECT_TRUE(queue.try_pop(value));
        EXPECT_EQ(value, expected);
    }
}

TEST_F(WorkQueueTest, FIFOOrder) {
    queue.push(1);
    queue.push(2);
    queue.push(3);

    int value;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 3);
}

TEST_F(WorkQueueTest, WaitPopBlocks) {
    std::atomic<bool> started{false};
    std::atomic<bool> popped{false};
    int result = 0;

    std::thread consumer([&]() {
        started = true;
        int value;
        if (queue.wait_pop(value)) {
            result = value;
            popped = true;
        }
    });

    // Wait for consumer to start
    while (!started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Consumer should be blocked
    EXPECT_FALSE(popped);

    // Push unblocks consumer
    queue.push(99);
    consumer.join();

    EXPECT_TRUE(popped);
    EXPECT_EQ(result, 99);
}

TEST_F(WorkQueueTest, SetDoneUnblocks) {
    std::atomic<bool> started{false};
    std::atomic<bool> returned{false};
    bool pop_result = true;

    std::thread consumer([&]() {
        started = true;
        int value;
        pop_result = queue.wait_pop(value);
        returned = true;
    });

    // Wait for consumer to start waiting
    while (!started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_FALSE(returned);

    // set_done should unblock
    queue.set_done();
    consumer.join();

    EXPECT_TRUE(returned);
    EXPECT_FALSE(pop_result);  // wait_pop returns false when done
}

TEST_F(WorkQueueTest, IsDone) {
    EXPECT_FALSE(queue.is_done());

    queue.push(1);
    EXPECT_FALSE(queue.is_done());

    queue.set_done();
    EXPECT_FALSE(queue.is_done());  // Still has items

    int value;
    queue.try_pop(value);
    EXPECT_TRUE(queue.is_done());  // Now empty and done
}

TEST_F(WorkQueueTest, Size) {
    EXPECT_EQ(queue.size(), 0);

    queue.push(1);
    EXPECT_EQ(queue.size(), 1);

    queue.push(2);
    queue.push(3);
    EXPECT_EQ(queue.size(), 3);

    int value;
    queue.try_pop(value);
    EXPECT_EQ(queue.size(), 2);
}

TEST_F(WorkQueueTest, ConcurrentPushPop) {
    constexpr int kNumItems = 1000;
    constexpr int kNumProducers = 4;
    constexpr int kNumConsumers = 4;

    std::atomic<int> produced{0};
    std::set<int> consumed;
    std::mutex consumed_mutex;

    // Producers
    std::vector<std::thread> producers;
    for (int p = 0; p < kNumProducers; p++) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kNumItems; i++) {
                queue.push(p * kNumItems + i);
                produced++;
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumers;
    for (int c = 0; c < kNumConsumers; c++) {
        consumers.emplace_back([&]() {
            int value;
            while (true) {
                if (queue.try_pop(value)) {
                    std::lock_guard<std::mutex> lock(consumed_mutex);
                    consumed.insert(value);
                } else if (queue.is_done()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait for producers
    for (auto& t : producers) {
        t.join();
    }

    queue.set_done();

    // Wait for consumers
    for (auto& t : consumers) {
        t.join();
    }

    // Verify all items consumed exactly once
    EXPECT_EQ(consumed.size(), kNumProducers * kNumItems);
}

TEST_F(WorkQueueTest, ConcurrentWaitPop) {
    constexpr int kNumItems = 100;
    constexpr int kNumConsumers = 4;

    std::set<int> consumed;
    std::mutex consumed_mutex;

    // Start consumers first
    std::vector<std::thread> consumers;
    for (int c = 0; c < kNumConsumers; c++) {
        consumers.emplace_back([&]() {
            int value;
            while (queue.wait_pop(value)) {
                std::lock_guard<std::mutex> lock(consumed_mutex);
                consumed.insert(value);
            }
        });
    }

    // Small delay to let consumers start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Push items
    for (int i = 0; i < kNumItems; i++) {
        queue.push(i);
    }

    queue.set_done();

    for (auto& t : consumers) {
        t.join();
    }

    // Each item delivered exactly once
    EXPECT_EQ(consumed.size(), kNumItems);
    for (int i = 0; i < kNumItems; i++) {
        EXPECT_TRUE(consumed.count(i) == 1);
    }
}

// Test with FileWorkItem (actual usage type)
TEST(WorkQueueFileWorkItemTest, BasicOperations) {
    WorkQueue<FileWorkItem> queue;

    FileWorkItem item1{"/src/file1.txt", "/dst/file1.txt"};
    FileWorkItem item2{"/src/file2.txt", "/dst/file2.txt"};

    queue.push(std::move(item1));
    queue.push(std::move(item2));

    EXPECT_EQ(queue.size(), 2);

    FileWorkItem result;
    EXPECT_TRUE(queue.try_pop(result));
    EXPECT_EQ(result.src_path, "/src/file1.txt");
    EXPECT_EQ(result.dst_path, "/dst/file1.txt");
}
