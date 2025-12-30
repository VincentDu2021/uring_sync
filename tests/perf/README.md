# Performance Tests

Benchmarks comparing uring-sync against standard file copy tools.

## Quick Start

```bash
# 1. Generate test data (one-time)
./tests/perf/gen_data.sh ml_small

# 2. Build uring-sync
make

# 3. Run benchmark
./tests/perf/bench.sh --scenario ml_small
```

## Test Scenarios

| Scenario | Files | Size Each | Total | Use Case |
|----------|-------|-----------|-------|----------|
| `ml_small` | 10K | 4KB | 40MB | ImageNet-style dataset |
| `ml_large` | 100K | 4KB | 400MB | Full ML dataset |
| `large_files` | 10 | 100MB | 1GB | Model weights |
| `mixed` | 1K + 10 | 4KB + 100MB | ~1GB | Realistic mix |
| `deep_tree` | 1K | 4KB | 4MB | Nested directories |

## Usage

### Generate Test Data

```bash
# Generate all scenarios
./tests/perf/gen_data.sh

# Generate specific scenario
./tests/perf/gen_data.sh ml_small

# Check status
./tests/perf/gen_data.sh status

# Clean up
./tests/perf/gen_data.sh clean
```

### Run Benchmarks

```bash
# Run all available scenarios
./tests/perf/bench.sh

# Run specific scenario
./tests/perf/bench.sh --scenario ml_small

# Run specific tool only
./tests/perf/bench.sh --tool uring

# Change number of runs (default: 3)
./tests/perf/bench.sh --runs 5

# Cold cache testing (requires sudo)
sudo ./tests/perf/bench.sh --cold

# Quick test (1 run, ml_small only)
./tests/perf/bench.sh --quick
```

### Make Target

```bash
make perf
```

## Output

Results are saved to `tests/perf/results/<timestamp>/`:

- `summary.md` - Human-readable report
- `raw.csv` - Detailed timing data for analysis
- `system.txt` - System configuration

## Baseline Tools

| Tool | Command | Notes |
|------|---------|-------|
| `cp` | `cp -r` | Standard baseline |
| `rsync` | `rsync -a` | Common alternative |
| `tar` | `tar cf - \| tar xf -` | Pipeline approach |
| `uring` | `uring-sync -j 1` | Our tool |

## Expected Results

Based on initial testing:

| Cache | uring-sync vs cp |
|-------|------------------|
| Warm | ~1.2x faster |
| Cold | ~1.6x faster |

Cold cache testing requires root to drop filesystem caches.

## Directory Structure

```
tests/perf/
├── bench.sh          # Main benchmark runner
├── gen_data.sh       # Test data generator
├── lib/
│   └── common.sh     # Shared functions
├── results/          # Output (gitignored)
└── README.md
```
