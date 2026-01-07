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
| `IORING_OP_SPLICE` | Zero-copy via pipe (src→pipe→dst) | `SPLICE_F_MOVE` |

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

### 2. Registered Buffers (NOT BENEFICIAL)

Registered buffers (`io_uring_register_buffers`) pre-pin memory to avoid per-I/O mapping. However, benchmarks showed they **hurt performance** for file copy:

| File Size | Normal | Registered | Difference |
|-----------|--------|------------|------------|
| 4KB | 73,907 files/s | 67,412 files/s | **-9%** |
| 64KB | 20,817 files/s | 20,195 files/s | **-3%** |

**Why it doesn't help:** Registered buffers amortize their cost over many operations per buffer (e.g., network servers reusing receive buffers thousands of times). Our workload uses each buffer 1-2 times per file then releases it. The buffer index lookup overhead exceeds the pin/unpin savings.

See `docs/notes/registered-buffers-analysis.md` for detailed analysis.

### 3. Registered File Descriptors

For frequently accessed directories:

```cpp
int fds[] = {src_dir_fd, dst_dir_fd};
io_uring_register_files(&ring, fds, 2);

// Use fixed fd in operations
io_uring_prep_openat(sqe, 0, filename, flags, mode);  // 0 = first registered fd
sqe->flags |= IOSQE_FIXED_FILE;
```

### 4. Zero-Copy for Large Files (NOT AVAILABLE)

`IORING_OP_COPY_FILE_RANGE` is not available in kernel headers (checked 6.14). Instead, we use splice-based zero-copy:

```cpp
// Current implementation: splice via kernel pipe (no userspace copies)
// SPLICE_IN: src_fd → pipe
io_uring_prep_splice(sqe, src_fd, offset, pipe_write, -1, len, SPLICE_F_MOVE);
// SPLICE_OUT: pipe → dst_fd
io_uring_prep_splice(sqe, pipe_read, -1, dst_fd, offset, len, SPLICE_F_MOVE);
```

For synchronous code paths, `copy_file_range()` syscall can be used directly.

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

### 6. File Ordering for Sequential Access

Sort files by inode before processing to approximate physical disk order:

```cpp
// Current implementation: sort by inode
std::sort(files.begin(), files.end(), [](const FileWorkItem& a, const FileWorkItem& b) {
    return a.inode < b.inode;
});
```

**Impact:** 8x improvement on network storage (48s → 6s for 10K files)

**Limitations (TODO - adaptive sorting):**

| Scenario | Issue | Potential Solution |
|----------|-------|-------------------|
| After defrag | Blocks reorganized, inodes unchanged | Re-stat after defrag |
| Backup restore | New inodes in restore order | Accept limitation |
| Btrfs/ZFS (CoW) | Inode order ≠ physical order | Use `fiemap` ioctl |
| FAT32/exFAT | No inodes | Fall back to name sort |
| NTFS | MFT records, not inodes | Use MFT order if available |
| NFS/SMB | Synthetic inodes | Fall back to name sort |
| FUSE (S3/GCS) | Fake inodes, no physical disk | Skip sorting |

**Future improvement:** Detect filesystem type and choose optimal sorting:
- ext4/xfs: inode sort (current)
- Btrfs: `fiemap` ioctl for physical extent order
- Network/FUSE: skip sorting or use name sort
- Unknown: fall back to readdir order

### 7. io_uring with Splice for Zero-Copy ✓ IMPLEMENTED

Zero-copy file transfer via splice - data never enters userspace:

```cpp
// State machine: SPLICE_IN → SPLICE_OUT (per chunk)
// SPLICE_IN: src_fd → pipe
io_uring_prep_splice(sqe, src_fd, offset, pipe_write, -1, len, SPLICE_F_MOVE);
// SPLICE_OUT: pipe → dst_fd
io_uring_prep_splice(sqe, pipe_read, -1, dst_fd, offset, len, SPLICE_F_MOVE);
```

**Implementation details:**
- PipePool class manages pipe pairs (one per in-flight file)
- Pipe buffer sized to match chunk_size (128KB default)
- Falls back to read/write if no pipe available

**Performance impact (GCP pd-balanced, 100K × 4KB files, cold cache):**

| Method | Time | vs cp |
|--------|------|-------|
| cp -r | 59.9s | baseline |
| uring read/write | ~55s | 8% faster |
| **uring splice** | **17.2s** | **3.5x faster** |

**Pipe buffer tuning:**
- 64KB (default): Works but suboptimal
- 128KB (= chunk_size): Optimal, one splice fills buffer
- 1MB: Too large, hurts network storage (3x slower!)

**Future:** Linked splice ops (submit SPLICE_IN + SPLICE_OUT together)

### 8. kTLS + Splice (NOT BENEFICIAL for Network)

For network transfer, we investigated using kTLS with splice for zero-copy file→socket transfer:

```cpp
// Expected zero-copy: file → pipe → kTLS socket
splice(src_fd, pipe_write, ...)  // file to pipe
splice(pipe_read, tls_socket, ...)  // pipe to encrypted socket
```

**Result:** 2.9x SLOWER than read/send.

**Root cause (discovered via strace):**
```
splice(file→pipe):   27μs     ← instant
splice(pipe→socket): 33ms     ← 1000x slower! Blocks on TCP ACKs
```

The `splice(pipe → kTLS socket)` call blocks waiting for TCP acknowledgments. The kernel cannot buffer aggressively like it does with regular `send()` calls.

**Recommendation:** For network transfer, use `--tls` without `--splice`. The kTLS kernel encryption still works with regular send(), providing 58% speedup over rsync without the splice blocking issue.

See `docs/BENCHMARKS.md` for detailed timing data.

## CLI Interface

```
Local copy:
  uring-sync [options] <source> <destination>

Network transfer:
  uring-sync send <source> <host:port> [options]
  uring-sync recv <destination> --listen <port> [options]

Common options:
  -j, --jobs <n>          Number of worker threads (default: 1)
  -c, --chunk-size <size> I/O buffer size (default: 128K)
  -q, --queue-depth <n>   io_uring queue depth per worker (default: 64)
  -v, --verbose           Verbose output
  -h, --help              Show this help

Local copy options:
  --sync                  Use synchronous I/O (for benchmarking)
  --no-splice             Use read/write instead of splice zero-copy

Network options:
  --secret <key>          Pre-shared secret for authentication
  --tls                   Enable kTLS encryption (AES-128-GCM)
  --uring                 Use io_uring for network I/O (experimental)
  --splice                Use splice for file→socket (slower for small files)

Planned options:
  -p, --preserve          Preserve timestamps and permissions
  -n, --dry-run           Show what would be copied
  --progress              Show progress bar
  --resume <file>         Resume from checkpoint file
  --exclude <pattern>     Exclude files matching pattern

Examples:
  # Local copy (single worker optimal for NVMe)
  uring-sync /data/imagenet /ssd/imagenet

  # Local copy with multiple workers (for network storage)
  uring-sync -j 4 -q 128 /data /network-mount/backup

  # Network transfer with kTLS encryption
  uring-sync recv /backup --listen 9999 --secret mykey --tls
  uring-sync send /data remote-host:9999 --secret mykey --tls

  # Network transfer over SSH tunnel (alternative to --tls)
  ssh -L 9999:localhost:9999 user@remote
  uring-sync send /data localhost:9999 --secret mykey
```

## Implementation Phases

### Phase 1: Single-Threaded Multi-File (MVP) ✓
- [x] Refactor current code to handle multiple files
- [x] Implement FileContext state machine
- [x] Add buffer pool
- [x] Support directory argument (non-recursive)
- [x] Basic progress reporting

### Phase 2: Directory Traversal ✓
- [x] Add recursive directory walking
- [x] Create destination directory structure
- [ ] Handle symlinks (copy vs preserve)
- [x] Path transformation (src prefix → dst prefix)

### Phase 3: Multi-Threading ✓
- [x] Implement lock-free work queue
- [x] Worker thread pool
- [x] Per-thread io_uring instances
- [x] Atomic statistics

### Phase 4: Optimizations (Partial)
- [ ] Linked SQEs for open+read chains
- [x] Registered buffers - **Investigated, not beneficial** (see docs/notes/registered-buffers-analysis.md)
- [ ] Hard link detection
- [x] copy_file_range - **Investigated, no IORING_OP_COPY_FILE_RANGE in kernel**
- [x] Splice zero-copy - **Implemented and working**
- [x] Inode sorting for sequential access
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

#### Local NVMe (cold cache)

| Scenario | Files | cp -r | uring splice | uring no-splice | Speedup |
|----------|-------|-------|--------------|-----------------|---------|
| ml_large | 100K × 4KB | 7.67s | 5.14s | 5.05s | **1.5x** |
| ml_images | 100K × 100KB | 22.7s | 5.4s | 8.1s | **4.2x** |

Key findings on fast storage:
- **Larger files benefit MORE** - 4.2x speedup for 100KB vs 1.5x for 4KB
- **Splice beats no-splice** - Zero-copy matters at high throughput (3.8 GB/s vs 1.8 GB/s)
- **CPU is the bottleneck** - Disk can keep up, syscall overhead limits cp

#### Network Storage (GCP pd-balanced, cold cache, with burst credits)

| Scenario | cp -r | uring-sync | Speedup | Notes |
|----------|-------|------------|---------|-------|
| 100K × 4KB | 59.9s | 17.2s | **3.5x** | With splice + inode sort |

#### Network Storage (GCP pd-balanced, cold cache, burst exhausted)

| Scenario | cp -r | uring-sync | Speedup | Notes |
|----------|-------|------------|---------|-------|
| 100K × 100KB | 22m | 23m | **1.0x** | Throughput-limited, both hit ceiling |

Key insight: **Storage speed determines where io_uring helps**:
- Fast NVMe: io_uring wins big (CPU-bound, syscall overhead matters)
- Slow/throttled storage: io_uring only helps for small IOPS-limited files

### Why Results May Vary

1. **Storage speed**: Fast NVMe sees big gains; slow storage only gains for small files
2. **Burst credits**: Cloud storage performance varies with burst budget
3. **Cache state**: Warm cache reduces the advantage
4. **File size**: Small files = IOPS-limited (batching helps), large = throughput-limited
5. **File ordering**: Inode-sorted access critical for network/spinning disk

## When io_uring Helps: Storage Speed Matters

The benefit of io_uring depends heavily on **what the bottleneck is**.

### Fast Storage (NVMe): CPU is the Bottleneck

On fast NVMe, the disk can easily saturate the CPU's ability to issue syscalls:

```
  Storage: NVMe (500K+ IOPS, 3+ GB/s)
  Bottleneck: CPU / syscall overhead

  io_uring Benefit: ✓ HIGH for ALL file sizes
  - Batched syscalls reduce context switches
  - Larger files benefit MORE (more data to pipeline)
```

| Scenario | File Size | cp -r | uring | Speedup |
|----------|-----------|-------|-------|---------|
| ml_large | 4KB | 7.7s | 5.1s | **1.5x** |
| ml_images | 100KB | 22.7s | 5.4s | **4.2x** |

### Slow Storage (Cloud/HDD): IOPS vs Throughput

On slow storage, the disk itself is the bottleneck:

| Limit Type | What it means | GCP pd-balanced example |
|------------|---------------|------------------------|
| **IOPS** | Operations per second | ~3,000-15,000 IOPS |
| **Throughput** | Bytes per second | ~140-1,200 MB/s |

```
  File Size    Limit Type     io_uring Benefit
  ─────────────────────────────────────────────
  < 16 KB      IOPS           ✓ High (batching hides latency)
  16-256 KB    Mixed          ~ Depends on burst credits
  > 256 KB     Throughput     ✗ Minimal (disk is ceiling)
```

| Scenario | File Size | cp -r | uring | Speedup | Storage State |
|----------|-----------|-------|-------|---------|---------------|
| ml_large | 4KB | 60s | 17s | **3.5x** | With burst |
| ml_images | 100KB | 22m | 23m | **1.0x** | Burst exhausted |

### Summary: When to Use uring-sync

| Storage Type | File Size | Recommendation |
|--------------|-----------|----------------|
| NVMe/fast SSD | Any | ✓ Use uring-sync (splice mode) |
| Cloud storage (burst) | < 16KB | ✓ Use uring-sync |
| Cloud storage (burst) | > 100KB | ~ Marginal benefit |
| Cloud storage (throttled) | Any | ✗ No benefit, use cp |
| HDD | < 16KB | ✓ Use uring-sync |
| HDD | > 100KB | ✗ Seek-limited, use cp |

## ML Dataset File Size Survey

Real-world ML datasets span a wide range of file sizes:

### Per-File Datasets (Individual Samples)

| Dataset | Domain | Files | Avg Size | Total | Notes |
|---------|--------|-------|----------|-------|-------|
| MNIST | Digits | 70K | **0.8 KB** | 50 MB | Tiny grayscale images |
| CIFAR-10 | Images | 60K | **3 KB** | 180 MB | 32×32 color images |
| ImageNet | Images | 1.2M | **111 KB** | 133 GB | Variable JPEG sizes |
| COCO | Detection | 330K | **115-150 KB** | 20-37 GB | JPEG, varies by year |
| LAION-5B | Multi-modal | 5.8B | varies | **220 TB** | Distributed as parquet |

### Sharded Datasets (Training Format)

| Format | Typical Shard Size | Use Case |
|--------|-------------------|----------|
| WebDataset (tar) | **100 MB - 1 GB** | Streaming training |
| Parquet | **100-300 MB** (row groups) | Tabular/embedding data |
| TFRecord | **100-500 MB** | TensorFlow pipelines |
| HuggingFace | **500 MB - 5 GB** | Hub downloads |

### Implications for uring-sync

| Workload | uring-sync Benefit | Notes |
|----------|-------------------|-------|
| MNIST/CIFAR raw | **High** | Tiny files, IOPS-limited |
| ImageNet raw | **Moderate** | 111KB avg, mixed regime |
| WebDataset shards | **Low** | 100MB+ shards, throughput-limited |
| Parquet downloads | **Low** | Large shards |

**Sweet spot:** Raw image datasets with files < 16KB (MNIST, CIFAR, custom datasets with thumbnails).

**Not ideal:** Pre-sharded datasets (WebDataset, TFRecord, Parquet) where files are already large.

## Roadmap & Positioning

### Current Focus: Independent Tool for Cloud/Single-Node

uring-sync targets use cases where MPI infrastructure is unavailable or overkill:

| Use Case | uring-sync | mpifileutils (dcp) |
|----------|------------|-------------------|
| Cloud VMs (GCP/AWS) | ✓ Best fit | Overkill |
| Single workstation | ✓ Best fit | Overkill |
| HPC cluster + Lustre | Consider dcp | ✓ Best fit |
| No MPI runtime | ✓ Works | ✗ Requires MPI |

### Comparison with mpifileutils

mpifileutils dcp uses MPI processes with blocking I/O:
- Each process: 1 blocking I/O operation at a time
- Parallelism via process count (typically 8-64)
- Higher memory overhead (~50MB per process)

uring-sync uses io_uring with async I/O:
- Each worker: 128 async operations in-flight
- 4 workers = 512 ops in-flight (vs 8-64 for dcp)
- Lower memory overhead (~20MB total)

**Note:** mpifileutils has an open issue ([#372](https://github.com/hpc/mpifileutils/issues/372)) investigating io_uring since 2020, but no implementation yet.

### Development Phases

| Phase | Goal | Status |
|-------|------|--------|
| 1 | Basic io_uring copy with state machine | ✓ Done |
| 2 | Multi-worker, file ordering optimization | ✓ Done |
| 3 | Zero-copy via IORING_OP_SPLICE | ✓ Done |
| 4 | Network transfer (send/recv) | ✓ Done (sync I/O) |
| **5** | **kTLS encryption** | ✓ Done (58% faster than rsync) |
| 6 | Async network with io_uring | Experimental (`--uring` flag) |
| 7 | Benchmark vs fpsync, rclone, dcp | Planned |
| 8 | Contribute io_uring to mpifileutils | Future |

### Phase 4: Network Transfer (Complete)

Basic network file transfer working:

```bash
# Receiver
uring-sync recv /dest --listen 9999 --secret key

# Sender
uring-sync send /src host:9999 --secret key
```

**Implementation:**
- Wire protocol in `include/protocol.hpp`
- Sender/receiver in `src/net.cpp`
- Uses synchronous I/O (blocking send/recv)
- Pre-shared secret authentication (plaintext - use SSH tunnel for security)

### Phase 5: kTLS Encryption (Complete)

Network transfer with kernel TLS encryption:

```bash
# Enable with --tls flag
uring-sync recv /backup --listen 9999 --secret key --tls
uring-sync send /data remote:9999 --secret key --tls
```

**Implementation:**
- TLS 1.2 AES-128-GCM via kTLS (`setsockopt(SOL_TLS)`)
- HKDF key derivation from pre-shared secret
- Kernel handles encryption, not userspace OpenSSL

**Performance:** 58% faster than rsync for large transfers (9.7GB dataset)

**Note:** kTLS + splice was investigated but found to be 2.9x slower (see Optimization #8).

### Phase 6: Async Network (Experimental)

Async network I/O via io_uring (enabled with `--uring` flag):
- Batched file reads with io_uring
- Network send remains synchronous
- Multi-connection support for higher throughput (planned)

### Phase 7: Comprehensive Benchmarks

Compare against other LNSF tools on real ML datasets (100M+ files):

| Tool | Type | Install |
|------|------|---------|
| fpsync | Parallel rsync batches | `apt install fpart` |
| rclone | Parallel transfers | Single binary |
| dcp | MPI parallel | `apt install mpifileutils` |
| s5cmd | S3 parallel | Single binary (S3 only) |

### Phase 8: Contribute to mpifileutils

Long-term goal: contribute io_uring support to libmfu (mpifileutils library):

- Close their open issue [#372](https://github.com/hpc/mpifileutils/issues/372) (open since 2020)
- Benefit: MPI coordination + io_uring efficiency = best of both worlds

```
dcp (current):   MPI process → blocking read/write
dcp (future):    MPI process → io_uring async I/O batches
```

## References

- [io_uring documentation](https://kernel.dk/io_uring.pdf)
- [liburing API](https://github.com/axboe/liburing)
- [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf)
- [mpifileutils dcp](https://github.com/hpc/mpifileutils) - HPC parallel copy
- [mpifileutils io_uring issue #372](https://github.com/hpc/mpifileutils/issues/372) - Open since 2020
- [fpart + fpsync](https://github.com/martymac/fpart) - Parallel rsync wrapper
- [rclone](https://rclone.org/) - Multi-backend parallel file transfer
- [s5cmd](https://github.com/peak/s5cmd) - Parallel S3 operations
