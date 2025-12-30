#!/bin/bash
# Performance benchmark runner
# Usage: ./bench.sh [options]
# Options:
#   --scenario NAME    Run specific scenario (default: all available)
#   --tool NAME        Run specific tool (cp, rsync, tar, uring)
#   --runs N           Number of runs per test (default: 3)
#   --cold             Drop caches between runs (requires sudo)
#   --quick            Quick mode: 1 run, ml_small only

set -e

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$BENCH_DIR/lib/common.sh"

# Cleanup on interrupt
cleanup() {
    echo ""
    warn "Interrupted - cleaning up..."
    rm -rf "$DST_BASE" 2>/dev/null || true
    exit 130
}
trap cleanup INT TERM

# Defaults
RUNS=3
SCENARIOS=""
TOOLS="cp rsync tar uring"
DROP_CACHES=false
QUICK=false
URING_WORKERS=1

# ============================================================
# Argument Parsing
# ============================================================

while [[ $# -gt 0 ]]; do
    case $1 in
        --scenario)
            SCENARIOS="$2"
            shift 2
            ;;
        --tool)
            TOOLS="$2"
            shift 2
            ;;
        --runs)
            RUNS="$2"
            shift 2
            ;;
        --cold)
            DROP_CACHES=true
            shift
            ;;
        --quick)
            QUICK=true
            RUNS=1
            SCENARIOS="ml_small"
            shift
            ;;
        -j|--uring-workers)
            URING_WORKERS="$2"
            shift 2
            ;;
        --data-dir=*)
            DATA_DIR="${1#*=}"
            shift
            ;;
        --data-dir)
            DATA_DIR="$2"
            shift 2
            ;;
        --dst-dir=*)
            DST_DIR="${1#*=}"
            shift
            ;;
        --dst-dir)
            DST_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --scenario NAME    Run specific scenario (auto-generates if missing)"
            echo "  --tool NAME        Run specific tool (cp, rsync, tar, uring)"
            echo "  --runs N           Number of runs per test (default: 3)"
            echo "  --cold             Drop caches between runs (requires sudo)"
            echo "  --quick            Quick mode: 1 run, ml_small only"
            echo "  --data-dir DIR     Set source data directory (default: ./test_data/src)"
            echo "  --dst-dir DIR      Set destination directory (default: ./test_data/dst)"
            echo "  -j, --uring-workers N  Number of uring-sync workers (default: 1)"
            echo ""
            echo "Scenarios: ml_small, ml_small_aligned, ml_large, ml_large_aligned,"
            echo "           large_files, mixed, deep_tree"
            echo "Tools: cp, rsync, tar, uring"
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Valid scenarios
VALID_SCENARIOS="ml_small ml_small_aligned ml_large ml_large_aligned large_files mixed deep_tree"

# If no scenarios specified, use all available
if [[ -z "$SCENARIOS" ]]; then
    SCENARIOS=""
    for s in $VALID_SCENARIOS; do
        if scenario_exists "$s"; then
            SCENARIOS="$SCENARIOS $s"
        fi
    done
    SCENARIOS=$(echo "$SCENARIOS" | xargs)  # trim
fi

# If still no scenarios, default to ml_small and generate it
if [[ -z "$SCENARIOS" ]]; then
    SCENARIOS="ml_small"
fi

# Auto-generate missing scenarios
GEN_SCRIPT="$BENCH_DIR/gen_data.sh"
for s in $SCENARIOS; do
    if ! scenario_exists "$s"; then
        # Validate scenario name
        if [[ ! " $VALID_SCENARIOS " =~ " $s " ]]; then
            error "Unknown scenario: $s"
            echo "Valid scenarios: $VALID_SCENARIOS"
            exit 1
        fi
        info "Scenario '$s' not found, generating..."
        "$GEN_SCRIPT" --data-dir "$DATA_DIR" "$s"
        echo ""
    fi
done

# ============================================================
# Main Benchmark
# ============================================================

header "Performance Benchmark"
echo "Scenarios: $SCENARIOS"
echo "Tools: $TOOLS"
echo "Runs per test: $RUNS"
echo "Drop caches: $DROP_CACHES"
echo ""

# Create results directory
RESULTS_DIR=$(create_results_dir)
CSV_FILE="$RESULTS_DIR/raw.csv"
SUMMARY_FILE="$RESULTS_DIR/summary.md"

info "Results will be saved to: $RESULTS_DIR"

# Write system info
write_system_info "$RESULTS_DIR/system.txt"

# Initialize CSV
init_csv "$CSV_FILE"

# Destination base directory
DST_BASE="$DST_DIR"
mkdir -p "$DST_BASE"

# Summary data for markdown
declare -A BEST_TIMES
declare -A BEST_TOOLS

# Run benchmarks
for scenario in $SCENARIOS; do
    header "Scenario: $scenario"

    read files bytes <<< $(scenario_info "$scenario")
    if [[ $files -eq 0 ]]; then
        warn "Scenario $scenario not found, skipping"
        continue
    fi

    src="$DATA_DIR/$scenario"
    dst="$DST_BASE/$scenario"

    info "Files: $files, Size: $(numfmt --to=iec $bytes)"
    echo ""

    best_time=999999
    best_tool=""

    for tool in $TOOLS; do
        times=()
        for run in $(seq 1 $RUNS); do
            # Show progress
            printf "  %-6s run %d/%d... " "$tool" "$run" "$RUNS"

            # Drop caches if requested
            if $DROP_CACHES; then
                drop_caches
            fi

            # Run the tool
            case $tool in
                cp)     t=$(time_cmd run_cp "$src" "$dst") ;;
                rsync)  t=$(time_cmd run_rsync "$src" "$dst") ;;
                tar)    t=$(time_cmd run_tar "$src" "$dst") ;;
                uring)  t=$(time_cmd run_uring "$src" "$dst" "$URING_WORKERS") ;;
                *)
                    warn "Unknown tool: $tool"
                    continue 2
                    ;;
            esac

            printf "%.3fs\n" "$t"
            times+=("$t")

            # Calculate metrics for CSV
            throughput=$(echo "scale=2; $bytes / $t / 1048576" | bc)
            fps=$(echo "scale=0; $files / $t" | bc)

            append_csv "$CSV_FILE" "$scenario,$tool,$run,$files,$bytes,$t,$throughput,$fps"
        done

        # Calculate stats
        read median mean min max <<< $(calc_stats "${times[@]}")

        # Display summary for this tool
        throughput=$(echo "scale=2; $bytes / $median / 1048576" | bc)
        fps=$(echo "scale=0; $files / $median" | bc)
        printf "  %-6s => %.3fs median, %s MB/s, %s files/s\n" "$tool" "$median" "$throughput" "$fps"

        # Track best
        if (( $(echo "$median < $best_time" | bc -l) )); then
            best_time=$median
            best_tool=$tool
        fi
    done

    echo ""
    info "Best: $best_tool (${best_time}s)"
    BEST_TIMES[$scenario]=$best_time
    BEST_TOOLS[$scenario]=$best_tool
    echo ""
done

# Cleanup temp directory
rm -rf "$DST_BASE"

# ============================================================
# Generate Summary
# ============================================================

{
    echo "# Performance Benchmark Results"
    echo ""
    echo "Date: $(date)"
    echo ""
    echo "## Configuration"
    echo ""
    echo "- Scenarios: $SCENARIOS"
    echo "- Tools: $TOOLS"
    echo "- Runs per test: $RUNS"
    echo "- Cache control: $DROP_CACHES"
    echo ""
    echo "## Results Summary"
    echo ""
    echo "| Scenario | Best Tool | Time (s) |"
    echo "|----------|-----------|----------|"

    for scenario in $SCENARIOS; do
        if [[ -n "${BEST_TOOLS[$scenario]}" ]]; then
            printf "| %s | %s | %.3f |\n" "$scenario" "${BEST_TOOLS[$scenario]}" "${BEST_TIMES[$scenario]}"
        fi
    done

    echo ""
    echo "## Raw Data"
    echo ""
    echo "See \`raw.csv\` for detailed timing data."
    echo ""
    echo "## System Info"
    echo ""
    echo '```'
    cat "$RESULTS_DIR/system.txt"
    echo '```'
} > "$SUMMARY_FILE"

echo ""
header "Benchmark Complete"
info "Results saved to: $RESULTS_DIR"
info "Summary: $SUMMARY_FILE"
info "Raw data: $CSV_FILE"
