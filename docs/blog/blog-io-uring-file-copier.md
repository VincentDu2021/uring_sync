---
title: "Building a File Copier 4x Faster Than cp Using io_uring"
published: false
description: "How I used Linux io_uring to build a file copier that's 4.2x faster than cp for ML datasets. Lessons on when async I/O helps—and when it doesn't."
tags: linux, cpp, performance, tutorial
series: "High-Performance File Transfer"
---

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

## Why io_uring Helps

On fast storage (NVMe, SSD), the bottleneck is **CPU and syscall overhead**, not the disk:

- **cp -r**: Processes files sequentially, 12+ syscalls per file
- **io_uring**: 64 files in-flight, batched syscalls, async completion

The bigger the files, the more time we spend waiting for I/O to complete—and the more io_uring's async approach helps. That's why we see 4.2x speedup for 100KB files vs 1.5x for 4KB files on NVMe.

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

3. **Profile your actual workload**. Synthetic benchmarks lie. Test with your real data.

4. **io_uring shines on fast storage**. When the disk can keep up, reducing syscall overhead yields big gains.

## What's Next: Network Transfer

This tool now also supports **network file transfer** with kTLS encryption, achieving 58% faster transfers than rsync. See the companion post: [Beating rsync by 58% with Kernel TLS](blog-ktls-vs-rsync.md).

## Code

The local copy implementation is ~1,400 lines of C++20. Key components:

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

io_uring can dramatically speed up small-file workloads—**4.2x faster on NVMe** and **2x faster on cloud SSD**. The key is reducing syscall overhead through batching and async I/O.

**When to use io_uring for file copying:**
- Many small files (ML datasets, source trees)
- Fast storage (NVMe, SSD)
- CPU-bound on syscall overhead

**When `cp -r` is fine:**
- Single large files (already efficient)
- One-off copies where complexity isn't worth it

---

*The code is available at [github.com/VincentDu2021/uring_sync](https://github.com/VincentDu2021/uring_sync). Benchmarks were run on Ubuntu 24.04 with kernel 6.14 on local NVMe and GCP Compute Engine VMs.*

