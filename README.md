# uring-sync

High-performance file transfer tool for ML datasets using Linux io_uring and kTLS.

**58% faster than rsync** for large encrypted transfers. **4.2x faster than `cp`** for local copies.

## Features

- **Local copy**: Async I/O with io_uring, splice zero-copy
- **Network transfer**: TCP with kTLS (kernel TLS) encryption
- **Optimized for ML datasets**: Millions of small files

## Benchmarks

### Network Transfer (Laptop → GCP VM)

| Dataset | uring-sync + kTLS | rsync (SSH) | Speedup |
|---------|-------------------|-------------|---------|
| 60MB (10K files) | 2.98s | 2.63s | ~equal |
| 589MB (100K files) | 16.4s | 24.8s | **34% faster** |
| 9.7GB (100K files) | 165s | 390s | **58% faster** |

### Local Copy (NVMe, cold cache)

| Dataset | cp -r | uring-sync | Speedup |
|---------|-------|------------|---------|
| 100K × 4KB files | 7.67s | 5.14s | **1.5x** |
| 100K × 100KB files | 22.7s | 5.4s | **4.2x** |

## Quick Start

### Build

```bash
# Dependencies (Ubuntu/Debian)
sudo apt install liburing-dev libfmt-dev libssl-dev

# Build
make
```

### Local Copy

```bash
# Copy directory (like cp -r, but faster)
./bin/uring-sync /source/path /dest/path

# Options
./bin/uring-sync -j 4 -q 128 /source /dest  # 4 workers, queue depth 128
```

### Network Transfer

```bash
# Receiver (on remote host)
./bin/uring-sync recv /dest --listen 9999 --secret mykey --tls

# Sender (on local host)
./bin/uring-sync send /source remote-host:9999 --secret mykey --tls
```

## How It Works

### Local Copy

1. **io_uring batching**: Submit 64 file operations at once, reducing syscall overhead
2. **Splice zero-copy**: Data flows `file → kernel pipe → file` without touching userspace
3. **Inode sorting**: Process files in disk order for sequential access

### Network Transfer

1. **kTLS encryption**: TLS in kernel (AES-128-GCM), not userspace SSH
2. **Simple protocol**: HELLO → FILE_HDR → FILE_DATA → FILE_END → ALL_DONE
3. **Pre-shared secret**: HKDF key derivation, no certificate management

## CLI Reference

```
Local copy:
  uring-sync [options] <source> <dest>

Options:
  -j <N>        Worker threads (default: 1)
  -q <N>        Queue depth per worker (default: 64)
  -c <bytes>    Chunk size (default: 128KB)
  --sync        Use synchronous I/O (for comparison)
  --no-splice   Use read/write instead of splice
  -v            Verbose output

Network transfer:
  uring-sync send <source> <host:port> [options]
  uring-sync recv <dest> --listen <port> [options]

Options:
  --secret <s>  Pre-shared secret for authentication
  --tls         Enable kTLS encryption
  --uring       Use io_uring for network I/O
  --splice      Use splice for file→socket (slower for small files)
```

## Requirements

- Linux kernel 5.1+ (5.19+ for all features)
- liburing
- libfmt
- OpenSSL (for kTLS key derivation)
- C++20 compiler

## Project Structure

```
src/
  main.cpp        # CLI, local copy state machine
  net.cpp         # Network sender/receiver (sync)
  net_uring.cpp   # Network sender/receiver (async)

include/
  ring.hpp        # io_uring wrapper
  common.hpp      # BufferPool, WorkQueue, Stats
  protocol.hpp    # Wire protocol definitions
  ktls.hpp        # kTLS setup helpers

tests/
  unit/           # Unit tests
  e2e/            # End-to-end tests
  perf/           # Benchmark scripts
```

## Testing

```bash
# Unit tests
make test

# End-to-end tests
./tests/e2e/e2e_tests.sh

# Performance benchmarks
./tests/perf/gen_data.sh --scenario ml_large
sudo ./tests/perf/bench.sh --scenario ml_large --cold
```

## Documentation

- [Design Document](DESIGN.md) - Architecture and implementation details
- [Network Design](NETWORK_DESIGN.md) - kTLS and network transfer design
- [Benchmarks](BENCHMARKS.md) - Detailed benchmark results

### Blog Posts

- [Building a File Copier 4x Faster Than cp](docs/blog-io-uring-file-copier.md)
- [Beating rsync by 58% with Kernel TLS](docs/blog-ktls-vs-rsync.md)
- [GCP pd-standard: 78x the Documented IOPS](docs/blog-gcp-pd-standard-iops.md)

## Performance Tips

1. **Local NVMe**: Single worker (`-j 1`) with deep queue (`-q 64`) is optimal
2. **Network storage**: Multiple workers (`-j 4 -q 128`) to saturate IOPS
3. **Network transfer**: Use `--tls` without `--splice` for best throughput
4. **Large datasets**: Increase file descriptor limit (`ulimit -n 65535`)

## License

MIT License - see [LICENSE](LICENSE)

## Acknowledgments

- [liburing](https://github.com/axboe/liburing) - io_uring library by Jens Axboe
- [Linux kTLS](https://docs.kernel.org/networking/tls.html) - Kernel TLS documentation
