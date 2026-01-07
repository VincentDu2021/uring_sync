# Why Registered Buffers Don't Help (and Make Things Worse)

## Benchmark Results

| File Size | Normal Buffers | Registered Buffers | Difference |
|-----------|----------------|-------------------|------------|
| 4KB | 73,907 files/s | 67,412 files/s | **-9%** |
| 64KB | 20,817 files/s | 20,195 files/s | **-3%** |

Registered buffers are consistently **slower** for our file copy workload.

## What Registered Buffers Do

When you register buffers with io_uring:

```c
io_uring_register_buffers(&ring, iovecs, count);
```

The kernel:
1. **Pins the memory pages** - locks them in RAM, prevents swapping
2. **Pre-computes DMA mappings** - ready for immediate I/O
3. **Stores metadata** - buffer addresses, sizes, indices

When you use `io_uring_prep_read_fixed()`:

```c
io_uring_prep_read_fixed(sqe, fd, buffer, len, offset, buf_index);
```

The kernel:
1. **Looks up `buf_index`** in the registered set
2. **Validates** the buffer is registered and index is valid
3. **Uses pre-computed DMA mapping** (skips per-I/O mapping)

## Why It's Slower for File Copy

### 1. Index Lookup Overhead

Every `read_fixed`/`write_fixed` requires an index lookup:

```
Normal path:   validate_buffer() → pin_pages() → setup_dma() → do_io()
Fixed path:    lookup_index() → validate_registered() → do_io()
```

For our workload with 1-2 I/O operations per file, the lookup overhead **exceeds** the pin/DMA savings.

### 2. Wrong Amortization

Registered buffers amortize their cost over **many operations per buffer**:

```
Cost model:
  Normal:  N_files × (pin_cost + io_cost + unpin_cost)
  Fixed:   register_cost + N_files × (lookup_cost + io_cost)
```

Registered buffers win when:
```
register_cost + N_ops × lookup_cost < N_ops × (pin_cost + unpin_cost)
```

Rearranging:
```
N_ops > register_cost / (pin_cost + unpin_cost - lookup_cost)
```

For our workload:
- **N_ops per buffer** = 1-2 (one file, one or two chunks)
- We need N_ops >> 100 for registered buffers to amortize

### 3. Buffer Reuse Pattern

Our pattern:
```
acquire_buffer(idx=0) → read → write → release(idx=0)
acquire_buffer(idx=1) → read → write → release(idx=1)
...
```

Each buffer sees only 2 operations before being released. The "registered" status provides no benefit because we're not reusing buffers across many operations.

Ideal pattern for registered buffers:
```
register buffer_0
for each block in huge_file:
    read_fixed(buffer_0)  // Same buffer, thousands of times
    process(buffer_0)
```

## When Registered Buffers Help

1. **Network servers** - same receive buffer handles thousands of packets
2. **Database engines** - same page buffers handle many queries
3. **Streaming I/O** - reading huge files in a loop with fixed buffers
4. **High-frequency polling** - same buffer polled repeatedly

## When They Hurt

1. **File copy** - each buffer used 1-2 times per file ❌
2. **One-shot operations** - buffer used once then discarded
3. **Many small files** - per-operation overhead dominates

## Kernel Code Path

From Linux kernel `io_uring.c`:

```c
// Normal read - inline buffer handling
static int io_read(struct io_kiocb *req) {
    struct iovec iov;
    // Direct validation, no lookup
    iov.iov_base = req->buf;
    iov.iov_len = req->len;
    return io_iter_do_read(req, &iov);
}

// Fixed read - requires index lookup
static int io_read_fixed(struct io_kiocb *req) {
    struct io_mapped_ubuf *imu;
    // Lookup in registered buffer table
    imu = io_buffer_select(req, req->buf_index);
    if (!imu)
        return -EFAULT;
    // Now do the actual read
    return io_iter_do_read(req, &imu->bvec);
}
```

The `io_buffer_select()` lookup adds overhead on every operation.

## Conclusion

For file copying workloads with many small files:

| Factor | Impact |
|--------|--------|
| Index lookup overhead | Adds ~5-10% latency per operation |
| Low operations per buffer | No amortization of registration cost |
| Sequential file access | Kernel already optimizes page pinning |

**Recommendation**: Don't use registered buffers for file copy. They're designed for high-frequency buffer reuse, not one-shot file operations.

## The Right Optimization

For our workload, the wins come from:

1. **Splice** - zero-copy via kernel pipe, no userspace buffers
2. **Batching** - 64 files in-flight, one syscall per batch
3. **Inode sorting** - sequential disk access pattern

These match the actual bottlenecks (syscall overhead, disk seeks) rather than buffer pinning, which is already fast on modern kernels.
