#include <gtest/gtest.h>
#include "common.hpp"

// ============================================================
// FileContext Initialization Tests
// ============================================================

TEST(FileContextTest, DefaultConstruction) {
    FileContext ctx;

    // Check default values
    EXPECT_TRUE(ctx.src_path.empty());
    EXPECT_TRUE(ctx.dst_path.empty());
    EXPECT_EQ(ctx.src_fd, -1);
    EXPECT_EQ(ctx.dst_fd, -1);
    EXPECT_EQ(ctx.state, FileState::QUEUED);
    EXPECT_EQ(ctx.file_size, 0);
    EXPECT_EQ(ctx.offset, 0);
    EXPECT_EQ(ctx.buffer, nullptr);
    EXPECT_EQ(ctx.buffer_index, -1);
    EXPECT_EQ(ctx.last_read_size, 0);
    EXPECT_FALSE(ctx.use_splice);
    EXPECT_FALSE(ctx.use_fixed_buffers);
}

TEST(FileContextTest, SetPaths) {
    FileContext ctx;
    ctx.src_path = "/source/file.txt";
    ctx.dst_path = "/dest/file.txt";

    EXPECT_EQ(ctx.src_path, "/source/file.txt");
    EXPECT_EQ(ctx.dst_path, "/dest/file.txt");
}

TEST(FileContextTest, SetFileDescriptors) {
    FileContext ctx;
    ctx.src_fd = 5;
    ctx.dst_fd = 6;

    EXPECT_EQ(ctx.src_fd, 5);
    EXPECT_EQ(ctx.dst_fd, 6);
}

// ============================================================
// FileState Tests
// ============================================================

TEST(FileStateTest, AllStatesExist) {
    // Verify all states are distinct
    EXPECT_NE(FileState::QUEUED, FileState::OPENING_SRC);
    EXPECT_NE(FileState::OPENING_SRC, FileState::STATING);
    EXPECT_NE(FileState::STATING, FileState::OPENING_DST);
    EXPECT_NE(FileState::OPENING_DST, FileState::READING);
    EXPECT_NE(FileState::READING, FileState::WRITING);
    EXPECT_NE(FileState::WRITING, FileState::SPLICING);
    EXPECT_NE(FileState::SPLICING, FileState::CLOSING_SRC);
    EXPECT_NE(FileState::CLOSING_SRC, FileState::CLOSING_DST);
    EXPECT_NE(FileState::CLOSING_DST, FileState::DONE);
    EXPECT_NE(FileState::DONE, FileState::FAILED);
}

TEST(FileContextTest, StateTransitions) {
    FileContext ctx;

    // Simulate state machine flow
    EXPECT_EQ(ctx.state, FileState::QUEUED);

    ctx.state = FileState::OPENING_SRC;
    EXPECT_EQ(ctx.state, FileState::OPENING_SRC);

    ctx.state = FileState::STATING;
    EXPECT_EQ(ctx.state, FileState::STATING);

    ctx.state = FileState::OPENING_DST;
    EXPECT_EQ(ctx.state, FileState::OPENING_DST);

    ctx.state = FileState::READING;
    EXPECT_EQ(ctx.state, FileState::READING);

    ctx.state = FileState::WRITING;
    EXPECT_EQ(ctx.state, FileState::WRITING);

    ctx.state = FileState::CLOSING_SRC;
    EXPECT_EQ(ctx.state, FileState::CLOSING_SRC);

    ctx.state = FileState::CLOSING_DST;
    EXPECT_EQ(ctx.state, FileState::CLOSING_DST);

    ctx.state = FileState::DONE;
    EXPECT_EQ(ctx.state, FileState::DONE);
}

TEST(FileContextTest, FailedState) {
    FileContext ctx;

    // Can transition to FAILED from any state
    ctx.state = FileState::READING;
    ctx.state = FileState::FAILED;
    EXPECT_EQ(ctx.state, FileState::FAILED);
}

// ============================================================
// OpType Tests
// ============================================================

TEST(OpTypeTest, AllOpTypesExist) {
    // Verify all op types are defined
    OpType op;

    op = OpType::OPEN_SRC;
    EXPECT_EQ(op, OpType::OPEN_SRC);

    op = OpType::OPEN_DST;
    EXPECT_EQ(op, OpType::OPEN_DST);

    op = OpType::STATX;
    EXPECT_EQ(op, OpType::STATX);

    op = OpType::READ;
    EXPECT_EQ(op, OpType::READ);

    op = OpType::WRITE;
    EXPECT_EQ(op, OpType::WRITE);

    op = OpType::CLOSE_SRC;
    EXPECT_EQ(op, OpType::CLOSE_SRC);

    op = OpType::CLOSE_DST;
    EXPECT_EQ(op, OpType::CLOSE_DST);

    op = OpType::MKDIR;
    EXPECT_EQ(op, OpType::MKDIR);

    op = OpType::NETWORK_SEND;
    EXPECT_EQ(op, OpType::NETWORK_SEND);

    op = OpType::NETWORK_RECV;
    EXPECT_EQ(op, OpType::NETWORK_RECV);
}

TEST(FileContextTest, CurrentOp) {
    FileContext ctx;

    ctx.current_op = OpType::READ;
    EXPECT_EQ(ctx.current_op, OpType::READ);

    ctx.current_op = OpType::WRITE;
    EXPECT_EQ(ctx.current_op, OpType::WRITE);
}

// ============================================================
// Buffer Management Tests
// ============================================================

TEST(FileContextTest, BufferAssignment) {
    FileContext ctx;

    char* buf = new char[4096];
    ctx.buffer = buf;
    ctx.buffer_index = 3;

    EXPECT_EQ(ctx.buffer, buf);
    EXPECT_EQ(ctx.buffer_index, 3);

    delete[] buf;
}

TEST(FileContextTest, ReadWriteTracking) {
    FileContext ctx;
    ctx.file_size = 10000;
    ctx.offset = 0;
    ctx.last_read_size = 4096;

    // Simulate progress
    ctx.offset += ctx.last_read_size;
    EXPECT_EQ(ctx.offset, 4096);

    ctx.last_read_size = 4096;
    ctx.offset += ctx.last_read_size;
    EXPECT_EQ(ctx.offset, 8192);

    ctx.last_read_size = 1808;  // Last chunk
    ctx.offset += ctx.last_read_size;
    EXPECT_EQ(ctx.offset, 10000);
    EXPECT_EQ(ctx.offset, ctx.file_size);  // Done!
}

// ============================================================
// Statx Result Tests
// ============================================================

TEST(FileContextTest, StatxResult) {
    FileContext ctx;

    // Simulate statx result
    ctx.stx.stx_size = 12345;
    ctx.stx.stx_mode = S_IFREG | 0644;

    EXPECT_EQ(ctx.stx.stx_size, 12345);
    EXPECT_TRUE(S_ISREG(ctx.stx.stx_mode));
}

// ============================================================
// Optimization Flags Tests
// ============================================================

TEST(FileContextTest, OptimizationFlags) {
    FileContext ctx;

    EXPECT_FALSE(ctx.use_splice);
    EXPECT_FALSE(ctx.use_fixed_buffers);

    ctx.use_splice = true;
    ctx.use_fixed_buffers = true;

    EXPECT_TRUE(ctx.use_splice);
    EXPECT_TRUE(ctx.use_fixed_buffers);
}

// ============================================================
// FileWorkItem Tests
// ============================================================

TEST(FileWorkItemTest, Construction) {
    FileWorkItem item;
    item.src_path = "/src/file.txt";
    item.dst_path = "/dst/file.txt";

    EXPECT_EQ(item.src_path, "/src/file.txt");
    EXPECT_EQ(item.dst_path, "/dst/file.txt");
}

TEST(FileWorkItemTest, MoveSemantics) {
    FileWorkItem item1;
    item1.src_path = "/src/file.txt";
    item1.dst_path = "/dst/file.txt";

    FileWorkItem item2 = std::move(item1);

    EXPECT_EQ(item2.src_path, "/src/file.txt");
    EXPECT_EQ(item2.dst_path, "/dst/file.txt");
}
