#include <gtest/gtest.h>
#include "common.hpp"
#include "ring.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

class ErrorHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        mkdir("/tmp/error_test", 0755);
    }

    void TearDown() override {
        system("rm -rf /tmp/error_test");
    }
};

// ============================================================
// RingManager Error Tests
// ============================================================

TEST_F(ErrorHandlingTest, OpenNonexistentFile) {
    RingManager ring(8);
    FileContext ctx;

    ring.prepare_openat(AT_FDCWD, "/tmp/error_test/does_not_exist.txt",
                        O_RDONLY, 0, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -ENOENT);  // No such file or directory
}

TEST_F(ErrorHandlingTest, OpenNoPermission) {
    RingManager ring(8);

    // Create file with no read permission
    int fd = open("/tmp/error_test/noperm.txt", O_CREAT | O_WRONLY, 0000);
    close(fd);

    FileContext ctx;
    ring.prepare_openat(AT_FDCWD, "/tmp/error_test/noperm.txt",
                        O_RDONLY, 0, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    // Should fail with permission denied (unless running as root)
    if (geteuid() != 0) {
        EXPECT_EQ(res, -EACCES);
    }

    // Cleanup - restore permissions to delete
    chmod("/tmp/error_test/noperm.txt", 0644);
}

TEST_F(ErrorHandlingTest, ReadFromBadFd) {
    RingManager ring(8);
    FileContext ctx;
    char buffer[64];

    // Use an invalid fd
    ring.prepare_read(99999, buffer, sizeof(buffer), 0, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -EBADF);  // Bad file descriptor
}

TEST_F(ErrorHandlingTest, WriteToReadOnlyFd) {
    RingManager ring(8);

    // Create and open file read-only
    int fd = open("/tmp/error_test/readonly.txt", O_CREAT | O_RDONLY, 0644);
    ASSERT_GE(fd, 0);

    FileContext ctx;
    char buffer[] = "test data";

    ring.prepare_write(fd, buffer, sizeof(buffer), 0, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -EBADF);  // Bad file descriptor (wrong mode)

    close(fd);
}

TEST_F(ErrorHandlingTest, StatxNonexistent) {
    RingManager ring(8);
    FileContext ctx;

    ring.prepare_statx(AT_FDCWD, "/tmp/error_test/no_such_file.txt",
                       0, STATX_SIZE, &ctx.stx, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -ENOENT);
}

TEST_F(ErrorHandlingTest, MkdirAlreadyExists) {
    RingManager ring(8);

    // Create directory first
    mkdir("/tmp/error_test/existing_dir", 0755);

    FileContext ctx;
    ring.prepare_mkdirat(AT_FDCWD, "/tmp/error_test/existing_dir", 0755, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -EEXIST);
}

TEST_F(ErrorHandlingTest, MkdirInNonexistentParent) {
    RingManager ring(8);
    FileContext ctx;

    ring.prepare_mkdirat(AT_FDCWD, "/tmp/error_test/no/such/parent/dir", 0755, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, -ENOENT);
}

// ============================================================
// BufferPool Error Tests
// ============================================================

TEST(BufferPoolErrorTest, ZeroBuffers) {
    // Edge case: zero buffers
    BufferPool pool(0, 4096);

    EXPECT_EQ(pool.available_count(), 0);

    auto [ptr, idx] = pool.acquire();
    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(idx, -1);
}

TEST(BufferPoolErrorTest, AcquireExhausted) {
    BufferPool pool(2, 4096);

    // Acquire all
    auto [p1, i1] = pool.acquire();
    auto [p2, i2] = pool.acquire();

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);

    // Third should fail
    auto [p3, i3] = pool.acquire();
    EXPECT_EQ(p3, nullptr);
    EXPECT_EQ(i3, -1);
}

TEST(BufferPoolErrorTest, DoubleRelease) {
    BufferPool pool(2, 4096);

    auto [ptr, idx] = pool.acquire();
    pool.release(idx);

    // Double release - should not crash, just mark available again
    pool.release(idx);

    EXPECT_EQ(pool.available_count(), 2);
}

TEST(BufferPoolErrorTest, ReleaseNegativeIndex) {
    BufferPool pool(2, 4096);

    // Should not crash
    pool.release(-1);
    pool.release(-100);

    EXPECT_EQ(pool.available_count(), 2);
}

TEST(BufferPoolErrorTest, ReleaseOutOfBounds) {
    BufferPool pool(2, 4096);

    // Should not crash
    pool.release(2);
    pool.release(100);

    EXPECT_EQ(pool.available_count(), 2);
}

// ============================================================
// WorkQueue Error Tests
// ============================================================

TEST(WorkQueueErrorTest, PopFromEmpty) {
    WorkQueue<int> queue;

    int value = 999;
    bool result = queue.try_pop(value);

    EXPECT_FALSE(result);
    EXPECT_EQ(value, 999);  // Unchanged
}

TEST(WorkQueueErrorTest, IsDoneWhenNotDone) {
    WorkQueue<int> queue;

    EXPECT_FALSE(queue.is_done());

    queue.push(1);
    EXPECT_FALSE(queue.is_done());
}

TEST(WorkQueueErrorTest, SetDoneMultipleTimes) {
    WorkQueue<int> queue;

    queue.set_done();
    queue.set_done();  // Should not crash
    queue.set_done();

    EXPECT_TRUE(queue.is_done());
}

TEST(WorkQueueErrorTest, PushAfterDone) {
    WorkQueue<int> queue;

    queue.set_done();
    queue.push(42);  // Should still work (implementation doesn't block)

    int value;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 42);
}

// ============================================================
// FileContext Edge Cases
// ============================================================

TEST(FileContextErrorTest, NegativeFileDescriptors) {
    FileContext ctx;

    ctx.src_fd = -1;
    ctx.dst_fd = -1;

    EXPECT_EQ(ctx.src_fd, -1);
    EXPECT_EQ(ctx.dst_fd, -1);
}

TEST(FileContextErrorTest, ZeroFileSize) {
    FileContext ctx;
    ctx.file_size = 0;
    ctx.offset = 0;

    // Offset equals file_size means done
    EXPECT_EQ(ctx.offset, ctx.file_size);
}

TEST(FileContextErrorTest, EmptyPaths) {
    FileContext ctx;

    EXPECT_TRUE(ctx.src_path.empty());
    EXPECT_TRUE(ctx.dst_path.empty());
}

// ============================================================
// Stats Edge Cases
// ============================================================

TEST(StatsErrorTest, Overflow) {
    Stats stats;

    // Set to max - 1
    stats.bytes_copied.store(UINT64_MAX - 1);

    // Increment should wrap (or not overflow unsafely)
    stats.bytes_copied++;

    EXPECT_EQ(stats.bytes_copied.load(), UINT64_MAX);

    stats.bytes_copied++;
    EXPECT_EQ(stats.bytes_copied.load(), 0);  // Wrapped
}
