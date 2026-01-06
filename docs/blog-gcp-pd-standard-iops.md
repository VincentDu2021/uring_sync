# GCP pd-standard: We Observed 78x the Documented IOPS Limit

I recently benchmarked copying 100,000 small files on a GCP VM with a pd-standard disk. The documented IOPS for our 100GB disk is approximately **75-150 IOPS**. We observed effective rates of **10,000+ IOPS**—roughly **78x above the documented limit**.

This post shares the raw data and asks genuine questions about how GCP persistent disk performance actually works.

## The Setup

| Component | Specification |
|-----------|---------------|
| VM | GCP e2-standard-4 (4 vCPUs, 16GB RAM) |
| Disk | **pd-standard**, 100GB |
| Kernel | 6.14.0-1021-gcp |
| Filesystem | ext4 |
| Workload | Copy 100,000 files (4KB and 100KB sizes) |

## What the GCP Docs Say

According to [GCP's Persistent Disk Performance documentation](https://cloud.google.com/compute/docs/disks/performance):

**pd-standard characteristics:**
- HDD-backed (not SSD)
- IOPS scales with disk size: ~0.75 read IOPS/GB, ~1.5 write IOPS/GB
- Best for sequential I/O, not random access

**For our 100GB pd-standard disk:**
```
Read IOPS:   ~75 IOPS (0.75 × 100)
Write IOPS:  ~150 IOPS (1.5 × 100)
Throughput:  ~100-120 MB/s
```

So we should expect roughly **75-150 IOPS** for our workload.

## What We Actually Observed

### Test 1: 100K × 4KB Files (~400MB total)

| Run | Tool | Time | Files/sec | Implied IOPS* |
|-----|------|------|-----------|---------------|
| Run 1 | cp | 59.5s | 1,679 | ~11,750 |
| Run 2 | cp | 59.8s | 1,672 | ~11,700 |
| Run 3 | cp | 69.5s | 1,439 | ~10,000 |
| Run 4 | cp | 54.8s | 1,826 | ~12,800 |
| Warm cache | cp | 5.1s | 19,610 | ~137,000 |

*Implied IOPS = files/sec × ~7 operations per file (open, stat, create, write, close×2)

### Test 2: 100K × 100KB Files (~10GB total)

| Run | Tool | Time | Files/sec | Throughput |
|-----|------|------|-----------|------------|
| With burst | cp | 266s | 375 | 36.7 MB/s |
| After sustained load | cp | 22m 6s | 75 | 7.7 MB/s |

### The Massive Discrepancy

| Metric | Documented | Observed Low | Observed High |
|--------|------------|--------------|---------------|
| IOPS | 75-150 | ~2,300 | ~12,000+ |
| Ratio to spec | 1x | **15x** | **78x** |

We observed performance **15-78x above** the documented specification.

## How Is This Possible?

Several hypotheses:

### 1. Aggressive SSD Caching
GCP may use SSD as a write-back cache in front of HDD, providing burst performance far above HDD capability.

### 2. Burst Credits (Undocumented)
The documentation mentions "I/O bursting" but doesn't detail the mechanism. Our performance degraded over sustained testing, suggesting a credit-based system.

### 3. Metadata Operations Are "Free"
Perhaps file operations (open, close, stat) don't count against IOPS quota the same way data operations do.

### 4. Our IOPS Calculation Is Wrong
Maybe 7 ops per file is an overestimate. But even at 1 op per file, 1,679 files/sec far exceeds 150 IOPS.

## Evidence of Burst Depletion

The same test showed massive variance:

| State | Files/sec | Implied IOPS | vs Documented |
|-------|-----------|--------------|---------------|
| Fresh (burst available) | 1,826 | ~12,800 | 85x |
| After heavy testing | 1,439 | ~10,000 | 67x |
| Warm cache (RAM) | 19,610 | N/A | Not disk |
| Exhausted (ml_images) | 75 | ~525 | 3.5x |

Performance decreased with sustained load, suggesting burst credits exist.

## The "Exhausted" State

After running the larger ml_images test (10GB, 100K files), we observed:
- 75 files/sec = ~525 implied IOPS
- Still **3.5x above** the documented 150 IOPS maximum
- But **23x slower** than our burst performance

This suggests even the "exhausted" state exceeds documented limits, possibly due to SSD caching layer.

## Calculating IOPS for File Operations

The documentation doesn't specify what counts as an "IOPS" for file operations:

```
Per-file copy operations:
  open(source)        → 1 IOPS?
  stat(source)        → 1 IOPS?
  open(dest, create)  → 1 IOPS?
  write(data)         → 1+ IOPS
  close(source)       → 1 IOPS?
  close(dest)         → 1 IOPS?

Assumption: ~6-7 IOPS per small file
Reality: Unknown - may be batched/cached differently
```

## Key Takeaways

1. **pd-standard performance far exceeds documented specs** during burst. We saw 78x the documented IOPS limit.

2. **Burst credits appear to exist** but are undocumented. Performance degraded from 1,826 to 75 files/sec over sustained testing.

3. **Even "exhausted" exceeds spec.** At our slowest, we still saw 3.5x the documented IOPS—suggesting SSD caching.

4. **Don't trust the docs for capacity planning.** Real-world performance is dramatically different from specifications.

5. **Benchmark your actual workload.** Synthetic benchmarks may not reflect file-heavy workloads.

## Questions for GCP Engineers

We'd genuinely appreciate clarification on:

1. **Is there SSD caching in front of pd-standard HDDs?** Our burst performance suggests yes.

2. **How do burst credits work for pd-standard?** Pool size? Regeneration rate?

3. **How are file metadata operations counted?** Does `open()` cost IOPS?

4. **Why does even exhausted performance exceed spec?** Is there a guaranteed SSD cache layer?

## Next Steps

We plan to repeat these tests on:
- [ ] pd-balanced (SSD-backed, documented 3,000+ IOPS)
- [ ] pd-ssd (higher performance SSD)
- [ ] pd-standard (fresh VM, controlled conditions)

Results will be added to this post.

## Raw Data

Full benchmark data is available at: [GitHub repo link]

**Test methodology:**
- Cold cache (dropped between runs with `echo 3 > /proc/sys/vm/drop_caches`)
- Timed with `time` command
- Multiple runs across different times
- Files generated with random content (not compressible)

---

*Have you observed similar behavior on GCP persistent disks? I'd love to hear about your experience in the comments.*

---

**Tags:** #gcp #googlecloud #performance #storage #devops #benchmarking
