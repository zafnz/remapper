#!/bin/bash
set -euo pipefail

# Linux test suite for remapper
# Runs Group 1 (filesystem interposition) only.
# Hardened binary tests (Groups 2-6) are macOS-only.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$PROJECT_ROOT/build"
PASS=0
FAIL=0
CLEANUP_DIRS=()

# Use a fake HOME so nothing touches the real home directory
TESTHOME=$(mktemp -d)
CLEANUP_DIRS+=("$TESTHOME")
export HOME="$TESTHOME"

cleanup() {
    for d in "${CLEANUP_DIRS[@]}"; do
        rm -rf "$d"
    done
}
trap cleanup EXIT

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

###############################################################################
# Group 1: Filesystem interposition (test_interpose + verify)
###############################################################################
echo "=== Group 1: Filesystem interposition ==="
TMPDIR1=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR1")

"$BUILD/remapper" "$TMPDIR1" "$HOME/.dummy*" -- "$BUILD/test_interpose"
if "$BUILD/verify_test_interpose" "$TMPDIR1" "$HOME"; then
    pass "filesystem interposition"
else
    fail "filesystem interposition"
fi

###############################################################################
# Summary
###############################################################################
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo "--- All tests passed ---"
