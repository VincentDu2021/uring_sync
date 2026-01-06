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
    SPLICE_IN,        // splice: src_fd → pipe_write
    SPLICE_OUT,       // splice: pipe_read → dst_fd
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
    SPLICE_IN,        // Splicing: src_fd → pipe (zero-copy)
    SPLICE_OUT,       // Splicing: pipe → dst_fd (zero-copy)
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

    // Buffer (assigned from pool) - used for read/write path
    char* buffer = nullptr;
    int buffer_index = -1;        // Index in buffer pool
    uint32_t last_read_size = 0;  // Bytes from last read

    // Pipe (assigned from pool) - used for splice path
    int pipe_read_fd = -1;        // Read end of pipe
    int pipe_write_fd = -1;       // Write end of pipe
    int pipe_index = -1;          // Index in pipe pool
    uint32_t splice_len = 0;      // Bytes in current splice operation

    // For statx result
    struct statx stx;

    // Use splice for this file
    bool use_splice = false;
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
// Pipe Pool - pre-allocated pipes for splice operations
// ============================================================
class PipePool {
public:
    // Pipe fds as a struct (can't use int[2] in vector)
    struct Pipe {
        int read_fd = -1;
        int write_fd = -1;
    };

    explicit PipePool(size_t count, size_t pipe_size = 0) : count_(count) {
        pipes_.resize(count);
        available_.resize(count, true);
        for (size_t i = 0; i < count; i++) {
            int fds[2];
            if (pipe(fds) < 0) {
                // Clean up previously created pipes
                for (size_t j = 0; j < i; j++) {
                    close(pipes_[j].read_fd);
                    close(pipes_[j].write_fd);
                }
                throw std::runtime_error("Failed to create pipe");
            }
            pipes_[i].read_fd = fds[0];
            pipes_[i].write_fd = fds[1];
            // Set pipe buffer to match chunk size (0 = use default 64KB)
            if (pipe_size > 0) {
                fcntl(fds[0], F_SETPIPE_SZ, pipe_size);
            }
        }
    }

    ~PipePool() {
        for (auto& p : pipes_) {
            if (p.read_fd >= 0) close(p.read_fd);
            if (p.write_fd >= 0) close(p.write_fd);
        }
    }

    // Non-copyable
    PipePool(const PipePool&) = delete;
    PipePool& operator=(const PipePool&) = delete;

    // Acquire a pipe, returns {read_fd, write_fd, index} or {-1, -1, -1} if none available
    struct PipeHandle {
        int read_fd;
        int write_fd;
        int index;
    };

    PipeHandle acquire() {
        for (size_t i = 0; i < count_; i++) {
            if (available_[i]) {
                available_[i] = false;
                return {pipes_[i].read_fd, pipes_[i].write_fd, static_cast<int>(i)};
            }
        }
        return {-1, -1, -1};
    }

    // Release a pipe back to the pool
    void release(int index) {
        if (index >= 0 && index < static_cast<int>(count_)) {
            available_[index] = true;
        }
    }

    size_t count() const { return count_; }
    size_t available_count() const {
        size_t c = 0;
        for (bool a : available_) if (a) c++;
        return c;
    }

private:
    std::vector<Pipe> pipes_;
    std::vector<bool> available_;
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
// Size Statistics - for adaptive chunk sizing
// ============================================================
struct SizeStats {
    std::vector<uint64_t> samples;       // Sampled file sizes
    uint64_t file_count = 0;             // Total files seen

    // Add a file to sampling
    // Uses reservoir-style sampling: always sample first 20, then every Nth
    void observe(uint64_t size) {
        file_count++;

        // Always sample the first 20 files for small datasets
        if (file_count <= 20) {
            samples.push_back(size);
            return;
        }

        // After that, sample every Nth file to keep ~100-200 samples max
        // Interval grows with file count to cap memory usage
        uint64_t interval = std::max(1UL, file_count / 100);
        if (file_count % interval == 0 && samples.size() < 200) {
            samples.push_back(size);
        }
    }

    // Get percentile from samples (0-100)
    uint64_t percentile(int pct) const {
        if (samples.empty()) return 0;

        std::vector<uint64_t> sorted = samples;
        std::sort(sorted.begin(), sorted.end());

        size_t idx = (sorted.size() * pct) / 100;
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    // Pick optimal chunk size based on file size distribution
    size_t pick_chunk_size() const {
        if (samples.empty()) {
            return 128 * 1024;  // Default 128KB
        }

        uint64_t p90 = percentile(90);

        if (p90 <= 32 * 1024)   return 64 * 1024;
        if (p90 <= 128 * 1024)  return 128 * 1024;
        if (p90 <= 512 * 1024)  return 256 * 1024;
        if (p90 <= 2 * 1024 * 1024) return 512 * 1024;
        return 1024 * 1024;  // 1MB max
    }

    // Summary for logging
    void print_summary() const {
        if (samples.empty()) return;

        uint64_t p50 = percentile(50);
        uint64_t p90 = percentile(90);
        uint64_t p99 = percentile(99);

        // Using printf to avoid fmt dependency in header
        printf("  File size distribution (from %zu samples):\n", samples.size());
        printf("    p50: %lu bytes\n", (unsigned long)p50);
        printf("    p90: %lu bytes\n", (unsigned long)p90);
        printf("    p99: %lu bytes\n", (unsigned long)p99);
    }
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
    ino_t inode = 0;  // For sorting by disk location
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
