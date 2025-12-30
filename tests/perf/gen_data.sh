#!/bin/bash
# Generate test datasets for performance benchmarks
# Usage: ./gen_data.sh [scenario...]
# Examples:
#   ./gen_data.sh              # Generate all scenarios
#   ./gen_data.sh ml_small     # Generate only ml_small
#   ./gen_data.sh ml_small large_files  # Generate specific scenarios

set -e

# Data location - default to ./test_data/src
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DATA_DIR="${PERF_DATA_DIR:-$PROJECT_ROOT/test_data/src}"

# Parse --data-dir early (before other args)
ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --data-dir=*) DATA_DIR="${1#*=}"; shift ;;
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done
set -- "${ARGS[@]}"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# ============================================================
# Helper: Generate file with random size variation
# ============================================================

# Generate a single file with random size: base * (1 +/- x%)
# Random 0-100: if > 50 then +, else -
# Usage: gen_random_file <path> <base_size> <max_pct>
gen_random_file() {
    local path="$1"
    local base="$2"
    local max_pct="$3"  # e.g., 75 means +/- 0-75%

    # Random 0 to 100
    local rand=$((RANDOM % 101))
    local pct size

    # If rand > 50: positive adjustment (rand-50 gives 0-50, scale to max_pct)
    # If rand <= 50: negative adjustment (50-rand gives 0-50, scale to max_pct)
    if (( rand > 50 )); then
        pct=$(( (rand - 50) * max_pct / 50 ))
        size=$(( (base * (100 + pct)) / 100 ))
    else
        pct=$(( (50 - rand) * max_pct / 50 ))
        size=$(( (base * (100 - pct)) / 100 ))
    fi

    # Ensure minimum size of 256 bytes
    (( size < 256 )) && size=256

    dd if=/dev/urandom of="$path" bs=$size count=1 2>/dev/null
}

# ============================================================
# Scenario Generators
# ============================================================

gen_ml_small() {
    local dir="$DATA_DIR/ml_small"
    local count=10000
    local base=4096
    local max_pct=75  # +/- 0-75% = files from 1KB to 7KB

    info "Generating ml_small: ${count} files, ~4KB +/-75% random sizes"
    rm -rf "$dir"
    mkdir -p "$dir"

    # Sequential to ensure unique RANDOM seeds
    for i in $(seq 1 $count); do
        gen_random_file "$dir/file_${i}.bin" $base $max_pct
        # Progress every 1000 files
        if (( i % 1000 == 0 )); then
            echo "  Progress: $i/$count files"
        fi
    done

    local actual=$(ls -1 "$dir" | wc -l)
    local total_size=$(du -sh "$dir" | cut -f1)
    info "Created $actual files ($total_size) in $dir"
}

gen_ml_small_aligned() {
    local dir="$DATA_DIR/ml_small_aligned"
    local count=10000
    local size=4096  # 4KB aligned

    info "Generating ml_small_aligned: ${count} x 4KB files (40MB total, aligned)"
    rm -rf "$dir"
    mkdir -p "$dir"

    # Use parallel generation for speed
    seq 1 $count | xargs -P 8 -I{} sh -c "dd if=/dev/urandom of='$dir/file_{}.bin' bs=$size count=1 2>/dev/null"

    local actual=$(ls -1 "$dir" | wc -l)
    info "Created $actual files in $dir"
}

gen_ml_large() {
    local dir="$DATA_DIR/ml_large"
    local count=100000
    local base=4096
    local max_pct=75  # +/- 0-75% = files from 1KB to 7KB

    info "Generating ml_large: ${count} files, ~4KB +/-75% random sizes"
    warn "This may take several minutes..."
    rm -rf "$dir"
    mkdir -p "$dir"

    for i in $(seq 1 $count); do
        gen_random_file "$dir/file_${i}.bin" $base $max_pct
        if (( i % 10000 == 0 )); then
            echo "  Progress: $i/$count files"
        fi
    done

    local actual=$(ls -1 "$dir" | wc -l)
    local total_size=$(du -sh "$dir" | cut -f1)
    info "Created $actual files ($total_size) in $dir"
}

gen_ml_large_aligned() {
    local dir="$DATA_DIR/ml_large_aligned"
    local count=100000
    local size=4096  # 4KB aligned

    info "Generating ml_large_aligned: ${count} x 4KB files (400MB total, aligned)"
    warn "This may take a few minutes..."
    rm -rf "$dir"
    mkdir -p "$dir"

    # Use parallel generation for speed
    seq 1 $count | xargs -P 16 -I{} sh -c "dd if=/dev/urandom of='$dir/file_{}.bin' bs=$size count=1 2>/dev/null"

    local actual=$(ls -1 "$dir" | wc -l)
    info "Created $actual files in $dir"
}

gen_large_files() {
    local dir="$DATA_DIR/large_files"
    local count=10
    local size_mb=100

    info "Generating large_files: ${count} x ${size_mb}MB files (1GB total)"
    rm -rf "$dir"
    mkdir -p "$dir"

    for i in $(seq 1 $count); do
        echo -n "  Creating file $i/$count..."
        dd if=/dev/urandom of="$dir/large_$i.bin" bs=1M count=$size_mb 2>/dev/null
        echo " done"
    done

    info "Created $count files in $dir"
}

gen_mixed() {
    local dir="$DATA_DIR/mixed"
    local small_count=1000
    local small_size=4096
    local large_count=10
    local large_size_mb=100

    info "Generating mixed: ${small_count} x 4KB + ${large_count} x ${large_size_mb}MB (~1GB total)"
    rm -rf "$dir"
    mkdir -p "$dir/small" "$dir/large"

    # Small files
    echo "  Creating $small_count small files..."
    seq 1 $small_count | xargs -P 8 -I{} sh -c "dd if=/dev/urandom of='$dir/small/file_{}.bin' bs=$small_size count=1 2>/dev/null"

    # Large files
    for i in $(seq 1 $large_count); do
        echo -n "  Creating large file $i/$large_count..."
        dd if=/dev/urandom of="$dir/large/large_$i.bin" bs=1M count=$large_size_mb 2>/dev/null
        echo " done"
    done

    info "Created mixed dataset in $dir"
}

gen_deep_tree() {
    local dir="$DATA_DIR/deep_tree"
    local depth=10
    local files_per_level=100

    info "Generating deep_tree: ${files_per_level} files x ${depth} levels (1000 files, 4MB total)"
    rm -rf "$dir"

    # Build path for deepest level
    local deep_path="$dir"
    for i in $(seq 0 $((depth - 1))); do
        deep_path="$deep_path/level_$i"
    done
    mkdir -p "$deep_path"

    # Create files at each level
    local current="$dir"
    for level in $(seq 0 $((depth - 1))); do
        current="$current/level_$level"
        for f in $(seq 1 $files_per_level); do
            dd if=/dev/urandom of="$current/file_${f}.bin" bs=4096 count=1 2>/dev/null
        done
        echo "  Level $level: created $files_per_level files"
    done

    local total=$(find "$dir" -type f | wc -l)
    info "Created $total files in $dir"
}

# ============================================================
# Utilities
# ============================================================

show_status() {
    echo ""
    echo "========================================="
    echo "Test Data Status"
    echo "========================================="

    for scenario in ml_small ml_small_aligned ml_large ml_large_aligned large_files mixed deep_tree; do
        local dir="$DATA_DIR/$scenario"
        if [[ -d "$dir" ]]; then
            local count=$(find "$dir" -type f | wc -l)
            local size=$(du -sh "$dir" 2>/dev/null | cut -f1)
            echo -e "  ${GREEN}$scenario${NC}: $count files, $size"
        else
            echo -e "  ${YELLOW}$scenario${NC}: not generated"
        fi
    done
    echo ""
}

clean_all() {
    info "Removing all test data from $DATA_DIR"
    # Remove all scenario directories but keep .gitignore
    find "$DATA_DIR" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} \; 2>/dev/null || true
    info "Done"
}

usage() {
    echo "Usage: $0 [options] [command] [scenarios...]"
    echo ""
    echo "Options:"
    echo "  --data-dir DIR   Set data directory (default: ./test_data/src)"
    echo ""
    echo "Commands:"
    echo "  (none)     Generate specified scenarios (or all if none specified)"
    echo "  status     Show current test data status"
    echo "  clean      Remove all test data"
    echo ""
    echo "Scenarios (default - realistic random sizes):"
    echo "  ml_small         10K files, ~4KB +/-75% (~40MB)"
    echo "  ml_large         100K files, ~4KB +/-75% (~400MB)"
    echo ""
    echo "Scenarios (aligned - for peak I/O testing):"
    echo "  ml_small_aligned 10K x 4KB exact (40MB, aligned)"
    echo "  ml_large_aligned 100K x 4KB exact (400MB, aligned)"
    echo ""
    echo "Other scenarios:"
    echo "  large_files      10 x 100MB files (1GB)"
    echo "  mixed            1K small + 10 large (~1GB)"
    echo "  deep_tree        1K files in 10-level tree (4MB)"
    echo "  all              All scenarios"
    echo ""
    echo "Environment:"
    echo "  PERF_DATA_DIR  Override data location (default: ./test_data/src)"
}

# ============================================================
# Main
# ============================================================

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
fi

if [[ "$1" == "status" ]]; then
    show_status
    exit 0
fi

if [[ "$1" == "clean" ]]; then
    clean_all
    exit 0
fi

# Determine which scenarios to generate
if [[ $# -eq 0 || "$1" == "all" ]]; then
    scenarios="ml_small ml_small_aligned ml_large ml_large_aligned large_files mixed deep_tree"
else
    scenarios="$@"
fi

echo "========================================="
echo "Generating Performance Test Data"
echo "Data directory: $DATA_DIR"
echo "========================================="
echo ""

mkdir -p "$DATA_DIR"

for scenario in $scenarios; do
    case $scenario in
        ml_small)         gen_ml_small ;;
        ml_small_aligned) gen_ml_small_aligned ;;
        ml_large)         gen_ml_large ;;
        ml_large_aligned) gen_ml_large_aligned ;;
        large_files)      gen_large_files ;;
        mixed)            gen_mixed ;;
        deep_tree)        gen_deep_tree ;;
        *)
            warn "Unknown scenario: $scenario"
            ;;
    esac
    echo ""
done

show_status
