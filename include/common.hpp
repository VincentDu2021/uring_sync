#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <linux/stat.h>  // For struct statx

// ============================================================
// Operation Types
// ============================================================
enum class OpType {
    // File operations
    OPEN_SRC,
    OPEN_DST,
    STATX,
    READ,
    WRITE,
    COPY_FILE_RANGE,  // Zero-copy kernel-to-kernel copy (5.19+)
    CLOSE_SRC,
    CLOSE_DST,
    // Directory operations
    MKDIR,
    // Network (future)
    NETWORK_SEND,
    NETWORK_RECV
};

// ============================================================
// File State Machine
// ============================================================
enum class FileState {
    QUEUED,           // In work queue, not started
    OPENING_SRC,      // Waiting for source open
    STATING,          // Getting file metadata
    OPENING_DST,      // Waiting for dest open
    READING,          // Reading chunk
    WRITING,          // Writing chunk
    COPYING,          // Using copy_file_range (zero-copy, 5.19+)
    SPLICING,         // Using splice for large file (zero-copy, requires pipe)
    CLOSING_SRC,      // Closing source fd
    CLOSING_DST,      // Closing dest fd
    DONE,             // Complete
    FAILED            // Failed
};

// ============================================================
// File Context - tracks one file copy operation
// ============================================================
struct FileContext {
    // Paths
    std::string src_path;
    std::string dst_path;

    // File descriptors
    int src_fd = -1;
    int dst_fd = -1;

    // State machine
    FileState state = FileState::QUEUED;
    OpType current_op;

    // File info (from statx)
    uint64_t file_size = 0;
    uint64_t offset = 0;          // Current read/write position
    mode_t mode = 0644;

    // Buffer (assigned from pool)
    char* buffer = nullptr;
    int buffer_index = -1;        // Index in buffer pool
    uint32_t last_read_size = 0;  // Bytes from last read

    // For statx result
    struct statx stx;

    // Optimization flags
    bool use_splice = false;      // Use splice for this file (large files)
    bool use_fixed_buffers = false;  // Use registered buffers
};

// ============================================================
// Buffer Pool - pre-allocated buffers to avoid malloc per file
// ============================================================
class BufferPool {
public:
    BufferPool(size_t count, size_t buffer_size)
        : buffer_size_(buffer_size), count_(count) {
        // Allocate all buffers upfront
        buffers_.resize(count);
        available_.resize(count, true);
        for (size_t i = 0; i < count; i++) {
            // Aligned allocation for O_DIRECT compatibility
            buffers_[i] = static_cast<char*>(aligned_alloc(4096, buffer_size));
        }
    }

    ~BufferPool() {
        for (auto* buf : buffers_) {
            free(buf);
        }
    }

    // Non-copyable
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Acquire a buffer, returns {pointer, index} or {nullptr, -1} if none available
    std::pair<char*, int> acquire() {
        for (size_t i = 0; i < count_; i++) {
            if (available_[i]) {
                available_[i] = false;
                return {buffers_[i], static_cast<int>(i)};
            }
        }
        return {nullptr, -1};
    }

    // Release a buffer back to the pool
    void release(int index) {
        if (index >= 0 && index < static_cast<int>(count_)) {
            available_[index] = true;
        }
    }

    size_t buffer_size() const { return buffer_size_; }
    size_t available_count() const {
        size_t count = 0;
        for (bool a : available_) if (a) count++;
        return count;
    }

    // Expose buffers for io_uring registration
    const std::vector<char*>& buffers() const { return buffers_; }

private:
    std::vector<char*> buffers_;
    std::vector<bool> available_;
    size_t buffer_size_;
    size_t count_;
};

// ============================================================
// Statistics - atomic counters for progress
// ============================================================
struct Stats {
    std::atomic<uint64_t> files_total{0};
    std::atomic<uint64_t> files_completed{0};
    std::atomic<uint64_t> files_failed{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> bytes_copied{0};
    std::atomic<uint64_t> dirs_created{0};
};

// ============================================================
// Thread-Safe Work Queue
// ============================================================
template<typename T>
class WorkQueue {
public:
    WorkQueue() = default;

    // Non-copyable
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    // Push an item to the queue
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Push multiple items
    void push_bulk(std::vector<T>& items) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& item : items) {
                queue_.push(std::move(item));
            }
        }
        cv_.notify_all();
    }

    // Try to pop an item (non-blocking)
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Pop with wait (blocking)
    bool wait_pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Signal that no more items will be added
    void set_done() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool is_done() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_ && queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool done_ = false;
};

// ============================================================
// File Work Item - what gets passed to workers
// ============================================================
struct FileWorkItem {
    std::string src_path;
    std::string dst_path;
};

// Legacy struct for backwards compatibility (single-file mode)
struct RequestContext {
    OpType type;
    int fd;
    char* buffer;
    uint32_t length;
    uint64_t offset;

    bool owns_buffer = false;
    ~RequestContext() {
        if (owns_buffer && buffer) delete[] buffer;
    }
};
