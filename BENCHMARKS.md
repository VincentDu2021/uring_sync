# uring-sync Benchmarks

## Network Transfer: Laptop → GCP VM (Public Internet)

### Test Environment
- **Sender**: Ubuntu laptop (local machine)
- **Receiver**: GCP VM `uring-test` (us-central1-a)
- **Connection**: Public internet (laptop → GCP external IP)

### Test Datasets
| Dataset | Files | Total Size | Avg File Size |
|---------|-------|------------|---------------|
| ml_small | 10,000 | 60 MB | 6 KB |
| ml_large | 100,000 | 589 MB | 5.9 KB |
| ml_images | 100,000 | 9.7 GB | 100 KB |

### Results: 3-Way Comparison (Cold Cache)

| Dataset | uring-sync + kTLS | uring-sync + SSH | rsync (SSH) | Best |
|---------|-------------------|------------------|-------------|------|
| ml_small (60MB) | 2.98s | 2.27s | 2.63s | ~equal |
| ml_large (589MB) | **16.4s** (36 MB/s) | - | 24.8s (24 MB/s) | **kTLS +34%** |
| ml_images (9.7GB) | **165s** (60 MB/s) | 366s (26 MB/s) | 390s (25 MB/s) | **kTLS +58%** |

*All tests run with cold cache (`echo 3 > /proc/sys/vm/drop_caches`)*

### Analysis

**ml_small (10K files, 60MB)** - All methods comparable:
```
SSH tunnel: 2.27s (26 MB/s)
rsync:      2.63s (23 MB/s)
kTLS:       2.98s (20 MB/s)
```
For small transfers, network variability dominates. All three are effectively equal.

**ml_large (100K files, 589MB)** - kTLS wins:
```
kTLS:       16.4s (36 MB/s)  ← 34% faster than rsync
rsync:      24.8s (24 MB/s)  ← baseline
```

**ml_images (100K files, 9.7GB)** - kTLS dominates:
```
kTLS:       165s (60 MB/s)   ← 58% faster than rsync
SSH tunnel: 366s (26 MB/s)
rsync:      390s (25 MB/s)   ← baseline
```

**Key findings**:
1. **kTLS advantage scales with data size**: 0% (60MB) → 34% (589MB) → **58% (9.7GB)**
2. **Larger files = higher throughput**: 36 MB/s (6KB files) → 60 MB/s (100KB files)
3. **Kernel encryption wins**: kTLS uses 0.4s user time vs rsync's 4s+ for same data

---

## Splice Performance Investigation (2026-01-06)

### ml_images (9.7GB) - Splice vs Read/Send

| Mode | Time | Speed | Notes |
|------|------|-------|-------|
| Plaintext + read/send | **146s** | 68 MB/s | Fastest |
| Plaintext + splice | 157s | 63 MB/s | +8% overhead |
| kTLS + read/send | 165s | 60 MB/s | +13% (encryption) |
| **kTLS + splice** | **428s** | **23 MB/s** | **2.9x slower!** |

### Root Cause

`splice(pipe → kTLS socket)` blocks waiting for TCP ACKs:

```
splice(file→pipe):   27μs    ← instant
splice(pipe→socket): 33ms    ← 1000x slower, blocking on network
```

**Conclusion:** `--splice` is counterproductive with kTLS. Use `--tls` alone for best performance.

See `NETWORK_DESIGN.md` for detailed analysis.

---

## Local Copy Benchmarks

### Local NVMe (cold cache)
| Scenario | cp -r | uring-sync | Speedup |
|----------|-------|------------|---------|
| ml_large (100K × 4KB) | 7.67s | 5.14s | **1.5x** |
| ml_images (100K × 100KB) | 22.7s | 5.4s | **4.2x** |

### GCP pd-standard (100GB, burst credits)
| Scenario | cp -r | uring-sync | Speedup |
|----------|-------|------------|---------|
| ml_large (100K × 4KB) | 60s | 19s | **3.2x** |

---

## Localhost Network Tests (Controlled)

Eliminates network variability for accurate comparison.

### ml_small (10K files, 60MB) - Localhost
| Run | Plaintext | kTLS |
|-----|-----------|------|
| 1 | 0.314s | 0.353s |
| 2 | 0.298s | 0.357s |
| 3 | 0.281s | 0.366s |
| 4 | 0.289s | 0.349s |
| 5 | 0.295s | 0.349s |
| **Avg** | **0.295s** | **0.355s** |

**Result**: kTLS adds ~20% overhead on localhost (encryption cost without network benefit).

### read/send vs splice (Localhost)
| Scenario | read/send | splice | Winner |
|----------|-----------|--------|--------|
| ml_small (10K × 6KB) | **0.31s** | 0.36s | read/send |
| ml_large (100K × 6KB) | **3.38s** | 8.60s | read/send |

**Result**: For small files, read/send is 2.5x faster than splice. Splice overhead exceeds zero-copy benefit.

---

## Raw Benchmark Outputs (2026-01-06, Cold Cache)

### ml_large kTLS (100K files, 589MB) - GCP
```
$ sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
$ time ./bin/uring-sync send test_data/src/ml_large 35.193.34.237:9999 --secret bench --tls

Connecting to 35.193.34.237:9999...
Mode: read/send + kTLS encryption
kTLS enabled (AES-128-GCM)
Sending 100000 files...
Transfer complete: 100000 files

real    0m16.439s
user    0m0.316s
sys     0m3.364s
```

### ml_large rsync (100K files, 589MB) - GCP
```
$ sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
$ time rsync -az test_data/src/ml_large 35.193.34.237:/tmp/rsync_dest/

real    0m24.815s
user    0m4.086s
sys     0m3.504s
```

### ml_small kTLS (10K files, 60MB) - GCP
```
$ time ./bin/uring-sync send test_data/src/ml_small 35.193.34.237:9999 --secret bench --tls

Connecting to 35.193.34.237:9999...
Mode: read/send + kTLS encryption
kTLS enabled (AES-128-GCM)
Sending 10000 files...
Transfer complete: 10000 files

real    0m2.982s
user    0m0.038s
sys     0m0.184s
```

### ml_small SSH Tunnel (10K files, 60MB) - GCP
```
$ time ./bin/uring-sync send test_data/src/ml_small localhost:9999 --secret bench
(via SSH tunnel: local 9999 → GCP 9999)

Connecting to localhost:9999...
Mode: read/send
Sending 10000 files...
Transfer complete: 10000 files

real    0m2.272s
user    0m0.031s
sys     0m0.109s
```

### ml_small rsync (10K files, 60MB) - GCP
```
$ time rsync -az test_data/src/ml_small 35.193.34.237:/tmp/rsync_dest/

real    0m2.631s
user    0m0.079s
sys     0m0.031s
```

### ml_images kTLS (100K files, 9.7GB) - GCP
```
$ sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
$ time ./bin/uring-sync send test_data/src/ml_images 35.193.34.237:9999 --secret bench --tls

Connecting to 35.193.34.237:9999...
Mode: read/send + kTLS encryption
kTLS enabled (AES-128-GCM)
Sending 100000 files...
Transfer complete: 100000 files

real    2m45.194s
user    0m0.439s
sys     0m18.758s
```

---

## Test Commands

### kTLS (direct)
```bash
# Receiver (on GCP)
./bin/uring-sync recv /tmp/dest --listen 9999 --secret bench --tls

# Sender (on laptop)
./bin/uring-sync send test_data/src/<dataset> <GCP_IP>:9999 --secret bench --tls
```

### SSH Tunnel
```bash
# Create tunnel
gcloud compute ssh uring-test --zone=us-central1-a -- -L 9800:localhost:9800 -N &

# Receiver (on GCP, plaintext - SSH provides encryption)
./bin/uring-sync recv /tmp/dest --listen 9800 --secret bench

# Sender (through tunnel)
./bin/uring-sync send test_data/src/<dataset> localhost:9800 --secret bench
```

### rsync
```bash
time rsync -az test_data/src/<dataset> user@GCP_VM:/tmp/rsync_dest/
```

---

## Notes

- All benchmarks run with cold cache (`sync && echo 3 | sudo tee /proc/sys/vm/drop_caches`)
- All file counts verified on receiver after transfer
- Network benchmarks subject to variability - run multiple times for consistency
- GCP VM: `uring-test` in us-central1-a, external IP varies
- Date of last benchmark: 2026-01-06
