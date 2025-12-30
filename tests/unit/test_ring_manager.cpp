#include <gtest/gtest.h>
#include "ring.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>

class RingManagerTest : public ::testing::Test {
protected:
    static constexpr unsigned int kDefaultDepth = 8;
    static constexpr size_t kBufferSize = 4096;

    void SetUp() override {
        // Create test directory
        mkdir("/tmp/ring_test", 0755);
    }

    void TearDown() override {
        // Cleanup test files
        system("rm -rf /tmp/ring_test");
    }

    // Helper to create a test file with content
    void create_test_file(const char* path, const char* content) {
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        ASSERT_GE(fd, 0);
        write(fd, content, strlen(content));
        close(fd);
    }
};

// ============================================================
// Initialization Tests
// ============================================================

TEST_F(RingManagerTest, Construction) {
    RingManager ring(kDefaultDepth);
    EXPECT_EQ(ring.depth(), kDefaultDepth);
    EXPECT_FALSE(ring.buffers_registered());
}

TEST_F(RingManagerTest, ConstructionDifferentDepths) {
    RingManager ring4(4);
    EXPECT_EQ(ring4.depth(), 4);

    RingManager ring64(64);
    EXPECT_EQ(ring64.depth(), 64);
}

TEST_F(RingManagerTest, HasSqeSpace) {
    RingManager ring(kDefaultDepth);
    EXPECT_TRUE(ring.has_sqe_space());
}

// ============================================================
// Read Tests
// ============================================================

TEST_F(RingManagerTest, PrepareReadAndComplete) {
    RingManager ring(kDefaultDepth);

    // Create test file
    const char* test_content = "Hello io_uring!";
    create_test_file("/tmp/ring_test/read.txt", test_content);

    // Open file
    int fd = open("/tmp/ring_test/read.txt", O_RDONLY);
    ASSERT_GE(fd, 0);

    // Prepare context
    FileContext ctx;
    ctx.src_path = "/tmp/ring_test/read.txt";
    ctx.src_fd = fd;
    ctx.buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));
    memset(ctx.buffer, 0, kBufferSize);
    ctx.state = FileState::READING;
    ctx.current_op = OpType::READ;

    // Prepare and submit read
    ring.prepare_read(fd, ctx.buffer, kBufferSize, 0, &ctx);
    int submitted = ring.submit();
    EXPECT_EQ(submitted, 1);

    // Wait for completion
    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_EQ(res, static_cast<int>(strlen(test_content)));
    EXPECT_STREQ(ctx.buffer, test_content);

    close(fd);
    free(ctx.buffer);
}

TEST_F(RingManagerTest, PrepareReadWithOffset) {
    RingManager ring(kDefaultDepth);

    const char* test_content = "0123456789ABCDEF";
    create_test_file("/tmp/ring_test/offset.txt", test_content);

    int fd = open("/tmp/ring_test/offset.txt", O_RDONLY);
    ASSERT_GE(fd, 0);

    FileContext ctx;
    ctx.buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));
    memset(ctx.buffer, 0, kBufferSize);

    // Read from offset 10
    ring.prepare_read(fd, ctx.buffer, kBufferSize, 10, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_EQ(res, 6);  // "ABCDEF"
    EXPECT_STREQ(ctx.buffer, "ABCDEF");

    close(fd);
    free(ctx.buffer);
}

// ============================================================
// Write Tests
// ============================================================

TEST_F(RingManagerTest, PrepareWriteAndComplete) {
    RingManager ring(kDefaultDepth);

    int fd = open("/tmp/ring_test/write.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);

    FileContext ctx;
    ctx.buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));
    const char* write_data = "Written via io_uring!";
    strcpy(ctx.buffer, write_data);

    ring.prepare_write(fd, ctx.buffer, strlen(write_data), 0, &ctx);
    int submitted = ring.submit();
    EXPECT_EQ(submitted, 1);

    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_EQ(res, static_cast<int>(strlen(write_data)));

    close(fd);

    // Verify by reading back
    char verify[64] = {0};
    int fd2 = open("/tmp/ring_test/write.txt", O_RDONLY);
    read(fd2, verify, sizeof(verify));
    close(fd2);

    EXPECT_STREQ(verify, write_data);

    free(ctx.buffer);
}

// ============================================================
// Batching Tests
// ============================================================

TEST_F(RingManagerTest, BatchMultipleReads) {
    RingManager ring(kDefaultDepth);

    constexpr int kNumFiles = 4;
    int fds[kNumFiles];
    FileContext ctxs[kNumFiles];

    // Create files
    for (int i = 0; i < kNumFiles; i++) {
        char path[64], content[32];
        snprintf(path, sizeof(path), "/tmp/ring_test/batch_%d.txt", i);
        snprintf(content, sizeof(content), "File %d", i);
        create_test_file(path, content);

        fds[i] = open(path, O_RDONLY);
        ASSERT_GE(fds[i], 0);

        ctxs[i].buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));
        memset(ctxs[i].buffer, 0, kBufferSize);

        ring.prepare_read(fds[i], ctxs[i].buffer, kBufferSize, 0, &ctxs[i]);
    }

    // Single submit for all
    int submitted = ring.submit();
    EXPECT_EQ(submitted, kNumFiles);

    // Collect all completions
    int completed_count = 0;
    bool seen[kNumFiles] = {false};

    for (int i = 0; i < kNumFiles; i++) {
        int res;
        FileContext* ctx = ring.wait_one(res);
        EXPECT_GT(res, 0);

        // Find which context completed
        for (int j = 0; j < kNumFiles; j++) {
            if (ctx == &ctxs[j]) {
                EXPECT_FALSE(seen[j]);  // Not seen before
                seen[j] = true;
                completed_count++;
                break;
            }
        }
    }

    EXPECT_EQ(completed_count, kNumFiles);
    for (int i = 0; i < kNumFiles; i++) {
        EXPECT_TRUE(seen[i]);
    }

    // Cleanup
    for (int i = 0; i < kNumFiles; i++) {
        close(fds[i]);
        free(ctxs[i].buffer);
    }
}

// ============================================================
// Open/Close Tests
// ============================================================

TEST_F(RingManagerTest, PrepareOpenat) {
    RingManager ring(kDefaultDepth);

    create_test_file("/tmp/ring_test/open.txt", "test");

    FileContext ctx;
    ctx.src_path = "/tmp/ring_test/open.txt";
    ctx.state = FileState::OPENING_SRC;
    ctx.current_op = OpType::OPEN_SRC;

    ring.prepare_openat(AT_FDCWD, ctx.src_path.c_str(), O_RDONLY, 0, &ctx);
    ring.submit();

    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_GE(res, 0);  // res is the fd

    // Close the fd we got
    close(res);
}

TEST_F(RingManagerTest, PrepareClose) {
    RingManager ring(kDefaultDepth);

    create_test_file("/tmp/ring_test/close.txt", "test");

    int fd = open("/tmp/ring_test/close.txt", O_RDONLY);
    ASSERT_GE(fd, 0);

    FileContext ctx;
    ring.prepare_close(fd, &ctx);
    ring.submit();

    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_EQ(res, 0);  // 0 = success for close
}

// ============================================================
// Statx Tests
// ============================================================

TEST_F(RingManagerTest, PrepareStatx) {
    RingManager ring(kDefaultDepth);

    const char* content = "0123456789";  // 10 bytes
    create_test_file("/tmp/ring_test/statx.txt", content);

    FileContext ctx;
    ctx.src_path = "/tmp/ring_test/statx.txt";

    ring.prepare_statx(AT_FDCWD, ctx.src_path.c_str(), 0,
                       STATX_SIZE | STATX_MODE, &ctx.stx, &ctx);
    ring.submit();

    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_EQ(res, 0);
    EXPECT_EQ(ctx.stx.stx_size, 10);
    EXPECT_TRUE(S_ISREG(ctx.stx.stx_mode));
}

// ============================================================
// Error Handling Tests
// ============================================================

TEST_F(RingManagerTest, ReadNonexistentFile) {
    RingManager ring(kDefaultDepth);

    FileContext ctx;
    ring.prepare_openat(AT_FDCWD, "/tmp/ring_test/nonexistent.txt",
                        O_RDONLY, 0, &ctx);
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_LT(res, 0);  // Negative = error
    EXPECT_EQ(res, -ENOENT);
}

TEST_F(RingManagerTest, ReadBadFd) {
    RingManager ring(kDefaultDepth);

    FileContext ctx;
    ctx.buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));

    ring.prepare_read(9999, ctx.buffer, kBufferSize, 0, &ctx);  // Bad fd
    ring.submit();

    int res;
    ring.wait_one(res);

    EXPECT_LT(res, 0);
    EXPECT_EQ(res, -EBADF);

    free(ctx.buffer);
}

// ============================================================
// Process Completions Callback Tests
// ============================================================

TEST_F(RingManagerTest, ProcessCompletions) {
    RingManager ring(kDefaultDepth);

    create_test_file("/tmp/ring_test/proc.txt", "data");

    int fd = open("/tmp/ring_test/proc.txt", O_RDONLY);
    ASSERT_GE(fd, 0);

    FileContext ctx;
    ctx.buffer = static_cast<char*>(aligned_alloc(4096, kBufferSize));

    ring.prepare_read(fd, ctx.buffer, kBufferSize, 0, &ctx);
    ring.submit();

    int callback_count = 0;
    FileContext* callback_ctx = nullptr;
    int callback_res = 0;

    int processed = ring.wait_and_process([&](FileContext* c, int r) {
        callback_count++;
        callback_ctx = c;
        callback_res = r;
    });

    EXPECT_GE(processed, 1);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(callback_ctx, &ctx);
    EXPECT_EQ(callback_res, 4);

    close(fd);
    free(ctx.buffer);
}

// ============================================================
// Buffer Registration Tests
// ============================================================

TEST_F(RingManagerTest, RegisterBuffers) {
    RingManager ring(kDefaultDepth);

    std::vector<char*> buffers(4);
    for (auto& buf : buffers) {
        buf = static_cast<char*>(aligned_alloc(4096, kBufferSize));
    }

    EXPECT_FALSE(ring.buffers_registered());

    bool result = ring.register_buffers(buffers, kBufferSize);
    EXPECT_TRUE(result);
    EXPECT_TRUE(ring.buffers_registered());

    // Can't register twice
    bool result2 = ring.register_buffers(buffers, kBufferSize);
    EXPECT_FALSE(result2);

    for (auto& buf : buffers) {
        free(buf);
    }
}

// ============================================================
// Mkdir Tests
// ============================================================

TEST_F(RingManagerTest, PrepareMkdirat) {
    RingManager ring(kDefaultDepth);

    FileContext ctx;
    ring.prepare_mkdirat(AT_FDCWD, "/tmp/ring_test/newdir", 0755, &ctx);
    ring.submit();

    int res;
    FileContext* completed = ring.wait_one(res);

    EXPECT_EQ(completed, &ctx);
    EXPECT_EQ(res, 0);

    // Verify directory exists
    struct stat st;
    EXPECT_EQ(stat("/tmp/ring_test/newdir", &st), 0);
    EXPECT_TRUE(S_ISDIR(st.st_mode));
}
