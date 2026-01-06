#!/bin/bash
# GCP Disk Performance Benchmark Script
# Usage: ./gcp_benchmark.sh [--quick] [--runs N]
#
# This script benchmarks file copy performance on GCP VMs
# to compare against documented disk specifications.
#
# Prerequisites:
#   - Run on a GCP VM
#   - sudo access (for cache dropping)
#   - Test data generated (will auto-generate if missing)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
header() { echo -e "\n${BLUE}=== $1 ===${NC}\n"; }

# Defaults
RUNS=3
QUICK=false
DATA_DIR="${PROJECT_ROOT}/test_data/src"
DST_DIR="${PROJECT_ROOT}/test_data/dst"
RESULTS_DIR="${PROJECT_ROOT}/gcp_results/$(date +%Y-%m-%d_%H%M%S)"

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick) QUICK=true; RUNS=1; shift ;;
        --runs) RUNS="$2"; shift 2 ;;
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --quick       Quick mode: 1 run, ml_small only"
            echo "  --runs N      Number of runs per test (default: 3)"
            echo "  --data-dir    Source data directory"
            echo "  -h, --help    Show this help"
            exit 0
            ;;
        *) error "Unknown option: $1"; exit 1 ;;
    esac
done

# ============================================================
# System Information
# ============================================================

collect_system_info() {
    header "Collecting System Information"

    local outfile="$RESULTS_DIR/system_info.txt"

    {
        echo "=== GCP Disk Benchmark System Info ==="
        echo "Date: $(date)"
        echo ""

        echo "=== VM Info ==="
        if command -v curl &>/dev/null; then
            echo "Machine Type: $(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/machine-type 2>/dev/null | cut -d'/' -f4 || echo 'N/A')"
            echo "Zone: $(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/zone 2>/dev/null | cut -d'/' -f4 || echo 'N/A')"
            echo "Instance Name: $(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/name 2>/dev/null || echo 'N/A')"
        fi
        echo ""

        echo "=== CPU ==="
        grep "model name" /proc/cpuinfo | head -1 | cut -d':' -f2 | xargs
        echo "Cores: $(nproc)"
        echo ""

        echo "=== Memory ==="
        free -h | grep Mem
        echo ""

        echo "=== Disk ==="
        df -h "$DATA_DIR" 2>/dev/null || df -h /
        echo ""

        echo "=== Disk Type (from mount) ==="
        mount | grep "^/dev" | head -5
        echo ""

        echo "=== Kernel ==="
        uname -r
        echo ""

        echo "=== GCP Disk Details (if gcloud available) ==="
        if command -v gcloud &>/dev/null; then
            INSTANCE=$(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/name 2>/dev/null)
            ZONE=$(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/zone 2>/dev/null | cut -d'/' -f4)
            if [[ -n "$INSTANCE" && -n "$ZONE" ]]; then
                gcloud compute disks list --filter="name~${INSTANCE}" --format="table(name,type,sizeGb,status)" 2>/dev/null || echo "Could not fetch disk info"
            fi
        else
            echo "gcloud not available"
        fi

    } | tee "$outfile"

    info "System info saved to $outfile"
}

# ============================================================
# Test Data Generation
# ============================================================

ensure_test_data() {
    local scenario="$1"
    local dir="$DATA_DIR/$scenario"

    if [[ -d "$dir" ]]; then
        local count=$(find "$dir" -type f | wc -l)
        if [[ $count -gt 0 ]]; then
            info "Test data exists: $scenario ($count files)"
            return 0
        fi
    fi

    info "Generating test data: $scenario"
    "$SCRIPT_DIR/gen_data.sh" --data-dir "$DATA_DIR" "$scenario"
}

# ============================================================
# Benchmark Functions
# ============================================================

drop_caches() {
    if [[ $EUID -eq 0 ]]; then
        sync
        echo 3 > /proc/sys/vm/drop_caches
    else
        sync
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    fi
}

run_benchmark() {
    local scenario="$1"
    local tool="$2"
    local run="$3"
    local src="$DATA_DIR/$scenario"
    local dst="$DST_DIR/$scenario"

    # Clean destination
    rm -rf "$dst"
    mkdir -p "$(dirname "$dst")"

    # Drop caches
    drop_caches 2>/dev/null || warn "Could not drop caches (need sudo)"

    # Get file count and size
    local files=$(find "$src" -type f | wc -l)
    local bytes=$(du -sb "$src" | cut -f1)

    # Run the tool
    local start=$(date +%s.%N)

    case $tool in
        cp)
            cp -r "$src" "$dst"
            ;;
        rsync)
            rsync -a "$src/" "$dst/"
            ;;
        tar)
            mkdir -p "$dst"
            tar -C "$src" -cf - . | tar -C "$dst" -xf -
            ;;
        uring)
            if [[ -x "$PROJECT_ROOT/bin/uring-sync" ]]; then
                "$PROJECT_ROOT/bin/uring-sync" "$src" "$dst"
            else
                warn "uring-sync not found, skipping"
                return 1
            fi
            ;;
        *)
            error "Unknown tool: $tool"
            return 1
            ;;
    esac

    local end=$(date +%s.%N)
    local duration=$(echo "$end - $start" | bc)
    local throughput=$(echo "scale=2; $bytes / $duration / 1048576" | bc)
    local fps=$(echo "scale=0; $files / $duration" | bc)

    # Output CSV line
    echo "$scenario,$tool,$run,$files,$bytes,$duration,$throughput,$fps"

    # Cleanup
    rm -rf "$dst"
}

# ============================================================
# Main
# ============================================================

main() {
    header "GCP Disk Performance Benchmark"

    mkdir -p "$RESULTS_DIR"
    info "Results will be saved to: $RESULTS_DIR"

    # Collect system info
    collect_system_info

    # Scenarios to test
    if $QUICK; then
        SCENARIOS="ml_small"
        TOOLS="cp uring"
    else
        SCENARIOS="ml_small ml_large"
        TOOLS="cp rsync tar uring"
    fi

    # Ensure test data exists
    for scenario in $SCENARIOS; do
        ensure_test_data "$scenario"
    done

    # CSV output
    CSV_FILE="$RESULTS_DIR/results.csv"
    echo "scenario,tool,run,files,bytes,time_s,throughput_mbs,files_per_sec" > "$CSV_FILE"

    # Run benchmarks
    for scenario in $SCENARIOS; do
        header "Benchmarking: $scenario"

        src="$DATA_DIR/$scenario"
        files=$(find "$src" -type f | wc -l)
        size=$(du -sh "$src" | cut -f1)
        info "Files: $files, Size: $size"
        echo ""

        for tool in $TOOLS; do
            for run in $(seq 1 $RUNS); do
                printf "  %-8s run %d/%d... " "$tool" "$run" "$RUNS"

                result=$(run_benchmark "$scenario" "$tool" "$run" 2>/dev/null) || continue
                echo "$result" >> "$CSV_FILE"

                time=$(echo "$result" | cut -d',' -f6)
                fps=$(echo "$result" | cut -d',' -f8)
                printf "%.2fs (%s files/s)\n" "$time" "$fps"
            done
        done
        echo ""
    done

    # Summary
    header "Results Summary"
    echo ""
    column -t -s',' "$CSV_FILE"
    echo ""

    info "Full results: $CSV_FILE"
    info "System info: $RESULTS_DIR/system_info.txt"

    # Calculate implied IOPS
    header "Implied IOPS Analysis"
    echo ""
    echo "Assuming ~7 IOPS per file (open, stat, create, write, close×2):"
    echo ""

    tail -n +2 "$CSV_FILE" | while IFS=',' read -r scenario tool run files bytes time throughput fps; do
        iops=$(echo "scale=0; $fps * 7" | bc)
        printf "  %-12s %-8s run %s: %6s files/s = ~%6s implied IOPS\n" "$scenario" "$tool" "$run" "$fps" "$iops"
    done

    echo ""
    info "Compare against GCP documented limits:"
    echo "  pd-standard 100GB: ~75-150 IOPS"
    echo "  pd-balanced 100GB: ~3,000-3,600 IOPS"
    echo "  pd-ssd 100GB:      ~3,000-6,000 IOPS"
}

main "$@"
