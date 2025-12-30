# uring-sync: Parallel Small-File Copier Design

## Problem Statement

Copying large numbers of small files (e.g., ML training datasets like ImageNet with millions of images) is extremely slow with traditional tools:

| Tool | Approach | Bottleneck |
|------|----------|------------|
| `cp -r` | Sequential per-file | Each file waits for previous |
| `rsync` | Sequential + checksums | Even worse |
| `tar \| tar` | Streaming | Still single-threaded I/O |

### What Modern `cp` Actually Does

Modern GNU coreutils `cp` (9.x) uses `copy_file_range()` - an efficient kernel-side zero-copy syscall. Per file, it issues approximately:

| Syscall | Count | Purpose |
|---------|-------|---------|
| openat | 2 | Open src + dst |
| copy_file_range | 2 | Data copy + EOF check |
| close | 2 | Close both fds |
| fstat | 2 | File metadata |
| newfstatat | 1 | Path stat |
| ioctl/fadvise64 | 2 | Hints to kernel |

**~12 syscalls per file** - not the old read/write pattern.

### Why io_uring Still Wins

The advantage is **NOT** about avoiding read/write overhead. It's:

1. **Async parallelism** - 64 files in-flight simultaneously vs 1
2. **Batched syscall submission** - One `io_uring_enter()` for 64 operations
3. **Reduced context switches** - Fewer user↔kernel transitions
4. **Overlapped I/O** - Kernel processes multiple files concurrently

**Target:** Process 64 files concurrently instead of 1 at a time

## Design Goals

1. **Parallel directory traversal** - Don't block I/O on tree walking
2. **Batched syscalls** - Submit 64+ operations per `io_uring_enter()`
3. **Concurrent file processing** - Many files in-flight simultaneously
4. **Memory efficient** - Fixed buffer pool, not per-file allocation
5. **Resumable** - Track progress for interrupted copies

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Main Thread                                     │
│  - Parse CLI arguments                                                       │
│  - Initialize worker threads                                                 │
│  - Wait for completion                                                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
              ┌───────────────────────┴───────────────────────┐
              ▼                                               ▼
┌──────────────────────────┐                    ┌──────────────────────────┐
│    Directory Walker      │                    │    Directory Creator     │
│    (1 thread)            │                    │    (1 thread)            │
│                          │                    │                          │
│  - nftw() or getdents64  │                    │  - Consumes dir queue    │
│  - Queues files to work  │                    │  - Creates dest dirs     │
│  - Queues dirs to create │                    │  - Uses io_uring MKDIRAT │
└──────────────────────────┘                    └──────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Lock-Free Work Queue                                  │
│                     (MPSC: walker produces, workers consume)                 │
└─────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         io_uring Worker Pool                                 │
│                         (N threads, default N=1 for local copy)              │
│                                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  Worker 0   │  │  Worker 1   │  │  Worker 2   │  │  Worker N   │         │
│  │             │  │             │  │             │  │             │         │
│  │ io_uring    │  │ io_uring    │  │ io_uring    │  │ io_uring    │         │
│  │ instance    │  │ instance    │  │ instance    │  │ instance    │         │
│  │             │  │             │  │             │  │             │         │
│  │ 64 files    │  │ 64 files    │  │ 64 files    │  │ 64 files    │         │
│  │ in-flight   │  │ in-flight   │  │ in-flight   │  │ in-flight   │         │
│  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘         │
└─────────────────────────────────────────────────────────────────────────────┘

**Note:** For local copy, single worker (N=1) is optimal because io_uring already
provides async parallelism within a single thread. Multi-worker adds lock contention
on the work queue without benefit. Multi-worker is designed for future network
transfer mode where it helps with multiple TCP connections and CPU-bound encryption.
```

## io_uring Operations Used

| Operation | Purpose | Flags |
|-----------|---------|-------|
| `IORING_OP_OPENAT` | Open source file | `O_RDONLY` |
| `IORING_OP_OPENAT` | Create dest file | `O_WRONLY \| O_CREAT \| O_TRUNC` |
| `IORING_OP_STATX` | Get file size & metadata | - |
| `IORING_OP_READ` | Read file content | - |
| `IORING_OP_WRITE` | Write file content | - |
| `IORING_OP_CLOSE` | Close file descriptors | - |
| `IORING_OP_MKDIRAT` | Create directories | - |
| `IORING_OP_LINKAT` | Hard link (same fs optimization) | - |
| `IORING_OP_COPY_FILE_RANGE` | Zero-copy for large files | - |

## Core Data Structures

```cpp
// File operation state machine
enum class FileState {
    QUEUED,           // In work queue, not started
    OPENING_SRC,      // Waiting for source open
    STATING,          // Getting file metadata
    OPENING_DST,      // Waiting for dest open
    READING,          // Reading chunk
    WRITING,          // Writing chunk
    CLOSING,          // Closing both fds
    DONE,             // Complete
    ERROR             // Failed
};

// Represents one file copy operation
struct FileContext {
    // Paths
    std::string src_path;
    std::string dst_path;

    // File descriptors
    int src_fd = -1;
    int dst_fd = -1;

    // State machine
    FileState state = FileState::QUEUED;

    // Progress
    uint64_t file_size = 0;
    uint64_t bytes_copied = 0;

    // Buffer (from pool)
    char* buffer = nullptr;
    uint32_t buffer_size = 0;

    // Metadata to preserve
    mode_t mode;
    struct timespec mtime;
};

// Pre-allocated buffer pool (avoid malloc per file)
class BufferPool {
    std::vector<char*> buffers;          // Fixed-size buffers
    std::atomic<uint64_t> free_mask;     // Bitmask of available buffers

public:
    BufferPool(size_t count, size_t buffer_size);
    char* acquire();                      // Get a buffer (blocks if none available)
    void release(char* buf);              // Return buffer to pool
};

// Lock-free work queue (single producer, multiple consumers)
template<typename T>
class SPMCQueue {
    std::atomic<Node*> head;
    // ... lock-free implementation
public:
    void push(T item);           // Called by walker thread
    bool try_pop(T& item);       // Called by worker threads
};

// Global statistics (atomic counters)
struct Stats {
    std::atomic<uint64_t> files_discovered{0};
    std::atomic<uint64_t> files_copied{0};
    std::atomic<uint64_t> files_failed{0};
    std::atomic<uint64_t> bytes_copied{0};
    std::atomic<uint64_t> dirs_created{0};
};
```

## Worker Thread Logic

```cpp
void worker_thread(int worker_id, SPMCQueue<FileContext*>& queue, Stats& stats) {
    // Each worker has its own io_uring instance
    io_uring ring;
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    BufferPool buffer_pool(QUEUE_DEPTH, CHUNK_SIZE);

    // Track in-flight operations
    std::array<FileContext*, QUEUE_DEPTH> in_flight{};
    int active_files = 0;

    while (!done || active_files > 0) {
        // 1. Fill the pipeline with new files
        while (active_files < QUEUE_DEPTH) {
            FileContext* ctx;
            if (!queue.try_pop(ctx)) break;

            ctx->buffer = buffer_pool.acquire();
            ctx->state = FileState::OPENING_SRC;

            // Submit async open for source file
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            io_uring_prep_openat(sqe, AT_FDCWD, ctx->src_path.c_str(), O_RDONLY, 0);
            io_uring_sqe_set_data(sqe, ctx);

            in_flight[active_files++] = ctx;
        }

        // 2. Submit all queued operations
        io_uring_submit(&ring);

        // 3. Process completions
        io_uring_cqe* cqe;
        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            FileContext* ctx = (FileContext*)io_uring_cqe_get_data(cqe);
            int result = cqe->res;
            io_uring_cqe_seen(&ring, cqe);

            // 4. State machine transition
            advance_state(ctx, result, &ring, buffer_pool, stats);

            if (ctx->state == FileState::DONE || ctx->state == FileState::ERROR) {
                buffer_pool.release(ctx->buffer);
                remove_from_in_flight(in_flight, ctx);
                active_files--;
                delete ctx;
            }
        }
    }

    io_uring_queue_exit(&ring);
}
```

## State Machine Diagram

```
                    ┌──────────────────────────────────────────────┐
                    │                                              │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │        QUEUED             │                                  │
    └───────────────────────────┘                                  │
                    │                                              │
                    │ Worker picks up                              │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │     OPENING_SRC           │──── error ──────────────────────►│
    │   (IORING_OP_OPENAT)      │                                  │
    └───────────────────────────┘                                  │
                    │ success (src_fd)                             │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │       STATING             │──── error ──────────────────────►│
    │   (IORING_OP_STATX)       │                                  │
    └───────────────────────────┘                                  │
                    │ success (file_size, mode)                    │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │     OPENING_DST           │──── error ──────────────────────►│
    │   (IORING_OP_OPENAT)      │                                  │
    └───────────────────────────┘                                  │
                    │ success (dst_fd)                             │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │       READING             │──── error ──────────────────────►│
    │   (IORING_OP_READ)        │◄─────────────────────┐           │
    └───────────────────────────┘                      │           │
                    │ success (bytes read)             │           │
                    ▼                                  │           │
    ┌───────────────────────────┐                      │           │
    │       WRITING             │──── error ───────────┼──────────►│
    │   (IORING_OP_WRITE)       │                      │           │
    └───────────────────────────┘                      │           │
                    │ success                          │           │
                    │                                  │           │
                    ├── more data? ────────────────────┘           │
                    │                                              │
                    │ EOF                                          │
                    ▼                                              │
    ┌───────────────────────────┐                                  │
    │       CLOSING             │                                  │
    │   (IORING_OP_CLOSE x2)    │                                  │
    └───────────────────────────┘                                  │
                    │                                              │
                    ▼                                              │
    ┌───────────────────────────┐      ┌───────────────────────────┐
    │         DONE              │      │         ERROR             │
    └───────────────────────────┘      └───────────────────────────┘
```

## Optimizations

### 1. Linked SQEs (Chain Operations)

Chain related operations to reduce round-trips:

```cpp
// Link: open -> statx -> read (executes atomically)
sqe1 = io_uring_get_sqe(&ring);
io_uring_prep_openat(sqe1, ...);
sqe1->flags |= IOSQE_IO_LINK;  // Link to next

sqe2 = io_uring_get_sqe(&ring);
io_uring_prep_statx(sqe2, ...);
sqe2->flags |= IOSQE_IO_LINK;  // Link to next

sqe3 = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe3, ...);
// No link flag - end of chain
```

### 2. Registered Buffers

Pre-register buffers with the kernel to avoid per-operation mapping:

```cpp
// At init
std::vector<iovec> iovecs(QUEUE_DEPTH);
for (int i = 0; i < QUEUE_DEPTH; i++) {
    iovecs[i].iov_base = buffers[i];
    iovecs[i].iov_len = CHUNK_SIZE;
}
io_uring_register_buffers(&ring, iovecs.data(), QUEUE_DEPTH);

// When submitting reads/writes
io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buf_index);
```

### 3. Registered File Descriptors

For frequently accessed directories:

```cpp
int fds[] = {src_dir_fd, dst_dir_fd};
io_uring_register_files(&ring, fds, 2);

// Use fixed fd in operations
io_uring_prep_openat(sqe, 0, filename, flags, mode);  // 0 = first registered fd
sqe->flags |= IOSQE_FIXED_FILE;
```

### 4. Zero-Copy for Large Files

Switch to `copy_file_range` for files > threshold:

```cpp
if (file_size > ZERO_COPY_THRESHOLD) {  // e.g., 1MB
    io_uring_prep_copy_file_range(sqe, src_fd, 0, dst_fd, 0, file_size, 0);
}
```

### 5. Hard Link Detection

Avoid copying identical files (same inode):

```cpp
std::unordered_map<ino_t, std::string> inode_to_dest;

if (auto it = inode_to_dest.find(statx.stx_ino); it != inode_to_dest.end()) {
    // File already copied, create hard link instead
    io_uring_prep_linkat(sqe, AT_FDCWD, it->second.c_str(),
                         AT_FDCWD, dst_path.c_str(), 0);
} else {
    inode_to_dest[statx.stx_ino] = dst_path;
    // Proceed with copy
}
```

## CLI Interface

```
Usage: uring-sync [options] <source> <destination>

Options:
  -j, --jobs <n>          Number of worker threads (default: 1, optimal for local copy)
  -c, --chunk-size <size> I/O buffer size (default: 128K)
  -q, --queue-depth <n>   io_uring queue depth per worker (default: 64)
  -p, --preserve          Preserve timestamps and permissions
  -n, --dry-run           Show what would be copied
  -v, --verbose           Verbose output
  --progress              Show progress bar
  --resume <file>         Resume from checkpoint file
  --exclude <pattern>     Exclude files matching pattern
  -h, --help              Show this help

Examples:
  # Copy ImageNet dataset (single worker is optimal for local copy)
  uring-sync /data/imagenet /ssd/imagenet

  # Copy with progress and preservation
  uring-sync -p --progress /backup/photos /external/photos

  # Dry run to see what would be copied
  uring-sync -n /src /dst
```

## Implementation Phases

### Phase 1: Single-Threaded Multi-File (MVP)
- [ ] Refactor current code to handle multiple files
- [ ] Implement FileContext state machine
- [ ] Add buffer pool
- [ ] Support directory argument (non-recursive)
- [ ] Basic progress reporting

### Phase 2: Directory Traversal
- [ ] Add recursive directory walking
- [ ] Create destination directory structure
- [ ] Handle symlinks (copy vs preserve)
- [ ] Path transformation (src prefix → dst prefix)

### Phase 3: Multi-Threading
- [ ] Implement lock-free work queue
- [ ] Worker thread pool
- [ ] Per-thread io_uring instances
- [ ] Atomic statistics

### Phase 4: Optimizations
- [ ] Linked SQEs for open+read chains
- [ ] Registered buffers
- [ ] Hard link detection
- [ ] copy_file_range for large files
- [ ] NUMA-aware buffer allocation

### Phase 5: Production Features
- [ ] Checkpoint/resume for interrupted copies
- [ ] Exclude patterns
- [ ] Preserve metadata (permissions, timestamps, xattrs)
- [ ] Verify mode (compare after copy)
- [ ] JSON progress output for scripting

## Benchmarking Plan

### Test Datasets

| Dataset | Files | Total Size | Avg File Size |
|---------|-------|------------|---------------|
| Synthetic small | 100,000 | 400 MB | 4 KB |
| Synthetic mixed | 50,000 | 5 GB | 100 KB |
| ImageNet subset | 100,000 | 12 GB | 120 KB |
| Large files | 1,000 | 100 GB | 100 MB |

### Comparison Targets

```bash
# Baseline: cp
time cp -r $SRC $DST

# tar pipe
time sh -c 'tar cf - $SRC | tar xf - -C $DST'

# GNU parallel + cp
time find $SRC -type f | parallel -j 32 'cp {} $DST/{//}'

# rsync
time rsync -a $SRC/ $DST/

# Our tool (single worker optimal for local copy)
time uring-sync $SRC $DST
```

### Metrics to Collect

- Wall clock time
- Files per second
- MB/s throughput
- CPU utilization (user vs sys)
- System call count (`strace -c`)
- Context switches (`perf stat`)

## Expected Performance

Target: **2-5x faster than `cp -r`** for small file workloads (storage-dependent)

### Theoretical Analysis

For N small files, `cp -r` does N sequential file copies. With io_uring:
- 64 files in-flight simultaneously
- Theoretical max speedup: 64x (I/O bound scenarios)
- Practical speedup: 2-5x (limited by storage IOPS, kernel overhead)

### Observed Results

| Scenario | cp -r | uring-sync | Speedup | Notes |
|----------|-------|------------|---------|-------|
| 10K × 4KB (cold) | 0.78s | 0.48s | **1.6x** | Measured |
| 10K × 4KB (warm) | 0.33s | 0.30s | 1.1x | Cache-bound |
| 100K × 4KB (SSD) | TBD | TBD | ~3-5x | Expected |
| 1K × 100MB files | ~same | ~same | 1.0x | copy_file_range optimal |

### Why Results May Vary

1. **Storage type**: NVMe benefits most (high IOPS), HDD sees less gain
2. **Cache state**: Warm cache reduces syscall overhead advantage
3. **File system**: ext4/XFS/btrfs have different metadata performance
4. **Kernel version**: io_uring improvements ongoing (5.x → 6.x)

Note: Large sequential files won't see improvement - kernel's `copy_file_range()` is already zero-copy optimal.

## References

- [io_uring documentation](https://kernel.dk/io_uring.pdf)
- [liburing API](https://github.com/axboe/liburing)
- [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf)
- [mpifileutils dcp](https://github.com/hpc/mpifileutils) - HPC parallel copy
- [fpart + fpsync](https://github.com/martymac/fpart) - Parallel rsync wrapper
