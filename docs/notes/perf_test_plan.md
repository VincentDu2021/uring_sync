# Performance Test Plan

## Dimensions to Test

| Dimension | Values | Why |
|-----------|--------|-----|
| **File count** | 100, 1K, 10K, 100K | uring-sync targets many-file workloads |
| **File size** | 1KB, 4KB, 64KB, 1MB, 100MB | Different I/O patterns |
| **Cache state** | Cold, Warm | Cold cache = real-world, Warm = best-case |
| **Workers** | 1, 2, 4, 8 | Verify single-worker is optimal |
| **Queue depth** | 32, 64, 128 | io_uring batching impact |

## Baseline Tools

| Tool | Command | Use Case |
|------|---------|----------|
| `cp -r` | `cp -r src dst` | Standard baseline |
| `rsync` | `rsync -a src/ dst/` | Common alternative |
| `tar pipe` | `tar cf - src \| tar xf - -C dst` | Pipeline approach |
| `cp + parallel` | `find \| parallel cp` | Parallelized cp |

## Metrics

- Wall time (seconds)
- Throughput (MB/s)
- Files/second
- CPU usage (user + sys)
- Syscall count (strace -c)

## Test Scenarios

| Scenario | Files | Size | Total | Simulates |
|----------|-------|------|-------|-----------|
| **ML dataset** | 10K | 4KB | 40MB | ImageNet-style |
| **Large ML dataset** | 100K | 4KB | 400MB | Full dataset |
| **Large files** | 10 | 100MB | 1GB | Model weights |
| **Mixed** | 1K + 10 | 4KB + 100MB | ~1GB | Realistic mix |
| **Deep tree** | 1K | 4KB | 4MB | 10-level nesting |

## Test Structure

```
tests/
├── perf/
│   ├── bench.sh           # Main benchmark runner
│   ├── gen_test_data.sh   # Generate test datasets
│   ├── run_baseline.sh    # Run cp/rsync/tar
│   ├── run_uring.sh       # Run uring-sync variants
│   └── report.sh          # Generate comparison report
```

## Implementation Notes

### Cache Control

To ensure cold cache tests are accurate:
```bash
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches
```

### Statistical Validity

- Run each test 3-5 times
- Report median and stddev
- Discard first run (warmup)

### Output Format

CSV for analysis:
```csv
scenario,tool,run,files,total_mb,time_s,throughput_mbs,files_per_sec
ml_dataset,cp,1,10000,40,5.2,7.69,1923
ml_dataset,uring-sync,1,10000,40,3.1,12.90,3226
```

## Expected Results

Based on CLAUDE.md, uring-sync shows:
- **1.6x faster** than `cp -r` on cold cache (10K × 4KB files)
- Single worker with deep queue (64) is optimal
- Wins via async parallelism + batched submission

## Questions to Resolve

1. Where to run? (Local SSD, NVMe, network storage)
2. Need sudo for cache control?
3. How many iterations per test?
4. Include network tests (scp) now or later?
