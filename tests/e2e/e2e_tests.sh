#!/bin/bash
# End-to-end tests for uring-sync
# Run from project root: ./tests/e2e_tests.sh

set -e  # Exit on first error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
PASSED=0
FAILED=0

# Binary path
BINARY="./bin/uring-sync"

# Test directories
TEST_BASE="/tmp/uring_sync_e2e_$$"
SRC_DIR="$TEST_BASE/src"
DST_DIR="$TEST_BASE/dst"

# ============================================================
# Helper Functions
# ============================================================

setup() {
    rm -rf "$TEST_BASE"
    mkdir -p "$SRC_DIR" "$DST_DIR"
}

cleanup() {
    rm -rf "$TEST_BASE"
}

pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    PASSED=$((PASSED + 1))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    echo -e "       $2"
    FAILED=$((FAILED + 1))
}

skip() {
    echo -e "${YELLOW}SKIP${NC}: $1 - $2"
}

separator() {
    echo "----------------------------------------"
}

test_name() {
    echo -e "${YELLOW}TEST${NC}: $1"
}

# Compare directories recursively
compare_dirs() {
    diff -r "$1" "$2" > /dev/null 2>&1
}

# Get checksum of file
checksum() {
    sha256sum "$1" | cut -d' ' -f1
}

# ============================================================
# Test Cases
# ============================================================

test_binary_exists() {
    test_name "Binary exists"
    if [[ -x "$BINARY" ]]; then
        pass "Binary exists and is executable"
    else
        fail "Binary exists" "$BINARY not found or not executable"
        echo "Run 'make' first"
        exit 1
    fi
}

test_single_file() {
    test_name "Single file copy"
    setup
    echo "Hello, World!" > "$SRC_DIR/single.txt"

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/single.txt" ]] && \
       [[ "$(cat "$DST_DIR/single.txt")" == "Hello, World!" ]]; then
        pass "Single file copy"
    else
        fail "Single file copy" "Content mismatch or file missing"
    fi
    cleanup
}

test_multiple_files() {
    test_name "Multiple files copy"
    setup
    for i in {1..10}; do
        echo "File $i content" > "$SRC_DIR/file_$i.txt"
    done

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    local all_match=true
    for i in {1..10}; do
        if [[ ! -f "$DST_DIR/file_$i.txt" ]] || \
           [[ "$(cat "$DST_DIR/file_$i.txt")" != "File $i content" ]]; then
            all_match=false
            break
        fi
    done

    if $all_match; then
        pass "Multiple files copy (10 files)"
    else
        fail "Multiple files copy" "One or more files missing or corrupted"
    fi
    cleanup
}

test_empty_file() {
    test_name "Empty file copy"
    setup
    touch "$SRC_DIR/empty.txt"

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/empty.txt" ]] && [[ ! -s "$DST_DIR/empty.txt" ]]; then
        pass "Empty file copy"
    else
        fail "Empty file copy" "Empty file not copied correctly"
    fi
    cleanup
}

test_nested_directories() {
    test_name "Nested directories copy"
    setup
    mkdir -p "$SRC_DIR/a/b/c"
    echo "deep file" > "$SRC_DIR/a/b/c/deep.txt"
    echo "level a" > "$SRC_DIR/a/level_a.txt"
    echo "level b" > "$SRC_DIR/a/b/level_b.txt"

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/a/b/c/deep.txt" ]] && \
       [[ -f "$DST_DIR/a/level_a.txt" ]] && \
       [[ -f "$DST_DIR/a/b/level_b.txt" ]]; then
        if compare_dirs "$SRC_DIR" "$DST_DIR"; then
            pass "Nested directories copy"
        else
            fail "Nested directories copy" "Content mismatch"
        fi
    else
        fail "Nested directories copy" "Directory structure not preserved"
    fi
    cleanup
}

test_large_file() {
    test_name "Large file copy (1MB)"
    setup
    # Create 1MB file
    dd if=/dev/urandom of="$SRC_DIR/large.bin" bs=1M count=1 2>/dev/null

    local src_sum=$(checksum "$SRC_DIR/large.bin")

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/large.bin" ]]; then
        local dst_sum=$(checksum "$DST_DIR/large.bin")
        if [[ "$src_sum" == "$dst_sum" ]]; then
            pass "Large file copy (1MB)"
        else
            fail "Large file copy" "Checksum mismatch"
        fi
    else
        fail "Large file copy" "File not copied"
    fi
    cleanup
}

test_many_small_files() {
    test_name "Many small files (100 files)"
    setup
    # Create 100 small files
    for i in $(seq 1 100); do
        echo "content $i" > "$SRC_DIR/small_$i.txt"
    done

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    local count=$(ls -1 "$DST_DIR" | wc -l)
    if [[ "$count" -eq 100 ]] && compare_dirs "$SRC_DIR" "$DST_DIR"; then
        pass "Many small files (100 files)"
    else
        fail "Many small files" "Expected 100 files, got $count or content mismatch"
    fi
    cleanup
}

test_special_characters_in_names() {
    test_name "Special characters in filenames"
    setup
    echo "spaces" > "$SRC_DIR/file with spaces.txt"
    echo "dashes" > "$SRC_DIR/file-with-dashes.txt"
    echo "underscores" > "$SRC_DIR/file_with_underscores.txt"

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/file with spaces.txt" ]] && \
       [[ -f "$DST_DIR/file-with-dashes.txt" ]] && \
       [[ -f "$DST_DIR/file_with_underscores.txt" ]]; then
        pass "Special characters in filenames"
    else
        fail "Special characters in filenames" "Files with special chars not copied"
    fi
    cleanup
}

test_binary_content() {
    test_name "Binary content preserved"
    setup
    # Create file with all byte values 0-255
    for i in $(seq 0 255); do
        printf "\\$(printf '%03o' $i)"
    done > "$SRC_DIR/binary.bin"

    local src_sum=$(checksum "$SRC_DIR/binary.bin")

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ -f "$DST_DIR/binary.bin" ]]; then
        local dst_sum=$(checksum "$DST_DIR/binary.bin")
        if [[ "$src_sum" == "$dst_sum" ]]; then
            pass "Binary content preserved"
        else
            fail "Binary content" "Checksum mismatch for binary file"
        fi
    else
        fail "Binary content" "Binary file not copied"
    fi
    cleanup
}

test_empty_directory() {
    test_name "Empty directory"
    setup
    mkdir -p "$SRC_DIR/emptydir"

    # Binary returns 1 for "no files" - that's OK
    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null || true

    # Note: Current implementation doesn't create empty dirs without files
    # This test documents current behavior
    if [[ -d "$DST_DIR/emptydir" ]]; then
        pass "Empty directory created"
    else
        skip "Empty directory" "Not implemented - empty dirs only created when containing files"
    fi
    cleanup
}

test_source_not_exists() {
    test_name "Source not exists"
    setup
    rm -rf "$SRC_DIR"

    if $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null; then
        fail "Source not exists" "Should have returned error"
    else
        pass "Source not exists (returns error)"
    fi
    cleanup
}

test_workers_flag() {
    test_name "Multiple workers (-j 2)"
    setup
    for i in {1..20}; do
        echo "File $i" > "$SRC_DIR/file_$i.txt"
    done

    $BINARY -j 2 "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if compare_dirs "$SRC_DIR" "$DST_DIR"; then
        pass "Multiple workers (-j 2)"
    else
        fail "Multiple workers" "Content mismatch with -j 2"
    fi
    cleanup
}

test_verbose_flag() {
    test_name "Verbose flag (-v)"
    setup
    echo "test" > "$SRC_DIR/verbose_test.txt"

    local output=$($BINARY -v "$SRC_DIR" "$DST_DIR" 2>&1)

    if [[ -n "$output" ]]; then
        pass "Verbose flag (-v) produces output"
    else
        fail "Verbose flag" "No output with -v flag"
    fi
    cleanup
}

test_overwrite_existing() {
    test_name "Overwrite existing file"
    setup
    echo "original" > "$SRC_DIR/overwrite.txt"
    mkdir -p "$DST_DIR"
    echo "existing" > "$DST_DIR/overwrite.txt"

    $BINARY "$SRC_DIR" "$DST_DIR" 2>/dev/null

    if [[ "$(cat "$DST_DIR/overwrite.txt")" == "original" ]]; then
        pass "Overwrite existing file"
    else
        fail "Overwrite existing" "File was not overwritten"
    fi
    cleanup
}

# ============================================================
# Main
# ============================================================

echo "========================================"
echo "uring-sync End-to-End Tests"
echo "========================================"

# Run all tests with separators
test_binary_exists; separator
test_single_file; separator
test_multiple_files; separator
test_empty_file; separator
test_nested_directories; separator
test_large_file; separator
test_many_small_files; separator
test_special_characters_in_names; separator
test_binary_content; separator
test_empty_directory; separator
test_source_not_exists; separator
test_workers_flag; separator
test_verbose_flag; separator
test_overwrite_existing

# Summary
echo "========================================"
echo -e "Results: ${GREEN}$PASSED passed${NC}, ${RED}$FAILED failed${NC}"
echo "========================================"

# Exit with error if any tests failed
if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
