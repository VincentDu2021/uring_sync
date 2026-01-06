# Building a File Copier That's 4x Faster Than `cp` Using io_uring

I built a high-performance file copier for ML datasets using Linux io_uring. On the right workload, it's **4.2x faster than `cp -r`**. Here's what I learned about when async I/O helps—and when it doesn't.

## The Problem: Millions of Small Files

ML training datasets often contain millions of small files:

| Dataset | Files | Typical Size |
|---------|-------|--------------|
| ImageNet | 1.28M | 100-200KB JPEG |
| COCO | 330K | 50-500KB |
| MNIST | 70K | 784 bytes |
| CIFAR-10 | 60K | 3KB |

Copying these with `cp -r` is painfully slow. Each file requires multiple syscalls (open, read, write, close), and the kernel processes them one at a time. For 100,000 files, that's 400,000+ syscalls executed sequentially.

## The Solution: io_uring

io_uring is a Linux async I/O interface (kernel 5.1+) that enables:

1. **Batched submission** - Queue dozens of operations, submit with one syscall
2. **Async completion** - Operations complete out of order
3. **Zero-copy** - Splice data directly between file descriptors via kernel pipes

Instead of: open → read → write → close → repeat

We do: submit 64 opens → process completions → submit reads/writes → batch everything

## Architecture

```
┌──────────────┐     ┌─────────────────┐     ┌─────────────────────┐
│ Main Thread  │────▶│  WorkQueue<T>   │────▶│  Worker Threads     │
│ (scanner)    │     │  (thread-safe)  │     │  (per-thread uring) │
└──────────────┘     └─────────────────┘     └─────────────────────┘
```

Each file progresses through a state machine:

```
OPENING_SRC → STATING → OPENING_DST → SPLICE_IN ⇄ SPLICE_OUT → CLOSING
```

Key design decisions:

1. **64 files in-flight** per worker simultaneously
2. **Per-thread io_uring instances** (avoids lock contention)
3. **Inode sorting** for sequential disk access
4. **Splice zero-copy** for data transfer (source → pipe → destination)
5. **Buffer pool** with 4KB-aligned allocations (O_DIRECT compatible)

## Benchmark Results

### Local NVMe (Cold Cache)

| Workload | cp -r | uring-sync | Speedup |
|----------|-------|------------|---------|
| 100K × 4KB files (400MB) | 7.67s | 5.14s | **1.5x** |
| 100K × 100KB files (10GB) | 22.7s | 5.4s | **4.2x** |

**Key insight**: Larger files benefit MORE from io_uring on fast storage. The 100KB test shows 4.2x improvement because we're overlapping many large reads/writes.

### GCP pd-balanced (SSD-backed, 100GB)

| Workload | cp -r | uring-sync | Speedup |
|----------|-------|------------|---------|
| 100K × 4KB files | 67.7s | 31.5s | **2.15x** |
| 100K × 100KB files | 139.6s | 64.7s | **2.16x** |

Consistent 2x improvement on cloud SSD storage.

### GCP pd-standard (HDD-backed, 100GB)

| Workload | cp -r | uring-sync | Speedup |
|----------|-------|------------|---------|
| 100K × 4KB files | 60s | 19s | **3.2x** |
| 100K × 100KB files | 266s | 1054s | **0.25x (slower!)** |

Wait, uring is *slower* for medium files on HDD? Yes. Here's why.

## When io_uring Helps (And When It Doesn't)

### Fast Storage (NVMe, SSD)

- **Bottleneck**: CPU/syscall overhead
- **io_uring advantage**: Batching reduces context switches
- **Result**: Larger files benefit MORE (4.2x for 100KB vs 1.5x for 4KB)

### Slow Storage (HDD, Throttled Cloud)

- **Bottleneck**: IOPS (I/O operations per second)
- **io_uring advantage**: Batching hides latency for small files
- **Problem**: Parallel random access kills HDDs

HDDs have spinning platters. When we issue 64 parallel reads, the head seeks constantly. Sequential access (like `cp -r`) is actually better for large files on HDD.

### The Sweet Spot

```
io_uring wins when: (files are small) OR (storage is fast)
io_uring loses when: (files are large) AND (storage is slow/HDD)
```

## Implementation Details

### The State Machine

Each file copy is a state machine with these transitions:

```cpp
enum class FileState {
    OPENING_SRC,    // Opening source file
    STATING,        // Getting file size
    OPENING_DST,    // Creating destination
    SPLICE_IN,      // Reading into kernel pipe
    SPLICE_OUT,     // Writing from pipe to dest
    CLOSING_SRC,    // Closing source
    CLOSING_DST,    // Closing destination
    DONE
};
```

Completions drive state transitions. When a completion arrives, we look up the file context and advance its state.

### Splice Zero-Copy

Instead of read() → userspace buffer → write(), we use splice():

```
Source FD → Kernel Pipe → Destination FD
```

Data never touches userspace. The kernel moves pages directly between file descriptors.

```cpp
// Splice from source into pipe
io_uring_prep_splice(sqe, src_fd, offset, pipe_write_fd, -1, chunk_size, 0);

// Splice from pipe to destination
io_uring_prep_splice(sqe, pipe_read_fd, -1, dst_fd, offset, chunk_size, 0);
```

### Inode Sorting

Before copying, we sort files by inode number:

```cpp
std::sort(files.begin(), files.end(),
    [](const auto& a, const auto& b) { return a.inode < b.inode; });
```

This encourages sequential disk access since inodes are typically allocated sequentially for files created together.

## What I Learned

1. **Single worker beats multi-threading** for local NVMe. Lock contention outweighs parallelism benefits when the bottleneck is fast I/O.

2. **Queue depth matters more than thread count**. 64 files in-flight per worker is the sweet spot.

3. **Storage characteristics determine everything**. The same code is 4x faster OR 4x slower depending on the disk.

4. **Profile your actual workload**. Synthetic benchmarks lie. Test with your real data.

5. **io_uring isn't magic**. It reduces syscall overhead—but if you're I/O bound on slow storage, syscalls aren't your bottleneck.

## What's Next: Network Transfer

This tool now also supports **network file transfer** with kTLS encryption, achieving 58% faster transfers than rsync. See the companion post: [Beating rsync by 58% with Kernel TLS](/blog-ktls-vs-rsync.md).

## Code

The full implementation is ~500 lines of C++20. Key components:

| Component | Purpose |
|-----------|---------|
| `RingManager` | io_uring wrapper with SQE/CQE management |
| `BufferPool` | 4KB-aligned buffer allocation |
| `PipePool` | Reusable kernel pipes for splice |
| `WorkQueue` | Thread-safe file queue |
| `FileContext` | Per-file state machine |

Build requirements:
- Linux kernel 5.1+ (5.19+ for splice)
- liburing
- C++20

## Conclusion

io_uring can dramatically speed up small-file workloads—4.2x in our best case. But it's not universally faster. Understanding your storage characteristics is essential.

**When to use io_uring for file copying:**
- Millions of small files (<100KB)
- Fast storage (NVMe, SSD)
- CPU-bound on syscall overhead

**When to stick with `cp -r`:**
- Large files on HDD
- Throttled cloud storage hitting IOPS limits
- Simple one-off copies (not worth the complexity)

---

*The code is available at [github.com/VincentDu2021/uring_sync](https://github.com/VincentDu2021/uring_sync). Benchmarks were run on Ubuntu 24.04 with kernel 6.14 on local NVMe and GCP Compute Engine VMs.*

---

**Tags:** #linux #iouring #cpp #performance #filesystems #ml
