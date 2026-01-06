#!/bin/bash
# Common functions for performance tests

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
DATA_DIR="${PERF_DATA_DIR:-$PROJECT_ROOT/test_data/src}"
DST_DIR="${PERF_DST_DIR:-$PROJECT_ROOT/test_data/dst}"
RESULTS_DIR_BASE="$SCRIPT_DIR/../results"
URING_BINARY="$PROJECT_ROOT/bin/uring-sync"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Logging
info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
header() { echo -e "${BLUE}=== $1 ===${NC}"; }

# ============================================================
# Timing Functions
# ============================================================

# Run a command and return elapsed time in seconds (with decimals)
time_cmd() {
    local start end elapsed
    start=$(date +%s.%N)
    "$@" >/dev/null 2>&1
    local exit_code=$?
    end=$(date +%s.%N)
    elapsed=$(echo "$end - $start" | bc)
    echo "$elapsed"
    return $exit_code
}

# Run command N times, return median time
run_benchmark() {
    local runs=${1:-3}
    shift
    local times=()

    for i in $(seq 1 $runs); do
        local t=$(time_cmd "$@")
        times+=("$t")
    done

    # Sort and get median
    local sorted=($(printf '%s\n' "${times[@]}" | sort -n))
    local mid=$((runs / 2))
    echo "${sorted[$mid]}"
}

# ============================================================
# Stats Functions
# ============================================================

# Calculate stats from array of times
calc_stats() {
    local -a times=("$@")
    local n=${#times[@]}

    if [[ $n -eq 0 ]]; then
        echo "0 0 0 0"
        return
    fi

    # Sort times
    local sorted=($(printf '%s\n' "${times[@]}" | sort -n))

    # Min, max, median
    local min=${sorted[0]}
    local max=${sorted[$((n-1))]}
    local mid=$((n / 2))
    local median=${sorted[$mid]}

    # Mean
    local sum=0
    for t in "${times[@]}"; do
        sum=$(echo "$sum + $t" | bc)
    done
    local mean=$(echo "scale=3; $sum / $n" | bc)

    echo "$median $mean $min $max"
}

# ============================================================
# Cache Control
# ============================================================

# Drop filesystem caches (uses sudo if needed)
drop_caches() {
    sync
    if [[ $EUID -eq 0 ]]; then
        echo 3 > /proc/sys/vm/drop_caches
    else
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    fi
    info "Dropped filesystem caches"
}

# ============================================================
# Results Management
# ============================================================

# Create timestamped results directory
create_results_dir() {
    local timestamp=$(date +%Y-%m-%d_%H%M%S)
    local dir="$RESULTS_DIR_BASE/$timestamp"
    mkdir -p "$dir"
    echo "$dir"
}

# Write system info to file
write_system_info() {
    local file="$1"
    {
        echo "Date: $(date)"
        echo "Hostname: $(hostname)"
        echo "Kernel: $(uname -r)"
        echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
        echo "Cores: $(nproc)"
        echo "Memory: $(free -h | awk '/^Mem:/ {print $2}')"
        echo ""
        echo "Disk info for $DATA_DIR:"
        df -h "$DATA_DIR" 2>/dev/null || echo "  (not available)"
        echo ""
        echo "Mount info:"
        findmnt -T "$DATA_DIR" 2>/dev/null || echo "  (not available)"
    } > "$file"
}

# Append result to CSV
append_csv() {
    local file="$1"
    shift
    echo "$@" >> "$file"
}

# Initialize CSV with header
init_csv() {
    local file="$1"
    echo "scenario,tool,run,files,total_bytes,time_s,throughput_mbs,files_per_sec" > "$file"
}

# ============================================================
# Scenario Helpers
# ============================================================

# Get scenario info: files count, total bytes
scenario_info() {
    local scenario="$1"
    local dir="$DATA_DIR/$scenario"

    if [[ ! -d "$dir" ]]; then
        echo "0 0"
        return 1
    fi

    local files=$(find "$dir" -type f | wc -l)
    local bytes=$(du -sb "$dir" | cut -f1)
    echo "$files $bytes"
}

# Check if scenario data exists
scenario_exists() {
    local scenario="$1"
    [[ -d "$DATA_DIR/$scenario" ]]
}

# ============================================================
# Tool Runners
# ============================================================

# Run cp -r (use /. to avoid glob expansion limits with many files)
run_cp() {
    local src="$1"
    local dst="$2"
    rm -rf "$dst"
    mkdir -p "$dst"
    cp -r "$src"/. "$dst/"
}

# Run rsync
run_rsync() {
    local src="$1"
    local dst="$2"
    rm -rf "$dst"
    mkdir -p "$dst"
    rsync -a "$src/" "$dst/"
}

# Run tar pipe
run_tar() {
    local src="$1"
    local dst="$2"
    rm -rf "$dst"
    mkdir -p "$dst"
    tar -C "$src" -cf - . | tar -C "$dst" -xf -
}

# Run uring-sync
run_uring() {
    local src="$1"
    local dst="$2"
    local workers="${3:-1}"
    local queue_depth="${4:-64}"
    local sync_mode="${5:-false}"
    rm -rf "$dst"
    mkdir -p "$dst"
    if [[ "$sync_mode" == "true" ]]; then
        "$URING_BINARY" --sync --quiet "$src" "$dst"
    else
        "$URING_BINARY" -j "$workers" -q "$queue_depth" --quiet "$src" "$dst"
    fi
}
