#!/bin/bash
set -euo pipefail

# Linux test suite for remapper (mount namespace approach)
#
# Tests that remapper correctly sets up bind mounts so that
# programs see target-dir content when accessing the original paths.
# No LD_PRELOAD, no interpose library — pure kernel-level redirection.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$PROJECT_ROOT/build"
REMAPPER="$BUILD/remapper"
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

assert_file_exists() {
    if [ -f "$1" ]; then
        pass "$2"
    else
        fail "$2 (file not found: $1)"
    fi
}

assert_file_not_exists() {
    if [ ! -f "$1" ]; then
        pass "$2"
    else
        fail "$2 (file should not exist: $1)"
    fi
}

assert_dir_exists() {
    if [ -d "$1" ]; then
        pass "$2"
    else
        fail "$2 (dir not found: $1)"
    fi
}

assert_dir_not_exists() {
    if [ ! -d "$1" ]; then
        pass "$2"
    else
        fail "$2 (dir should not exist: $1)"
    fi
}

assert_file_content() {
    if [ -f "$1" ] && [ "$(cat "$1")" = "$2" ]; then
        pass "$3"
    else
        fail "$3 (expected '$2', got '$(cat "$1" 2>/dev/null || echo "<missing>")')"
    fi
}

###############################################################################
# Group 1: Basic directory remapping
#   Create ~/.dummy-test/ with a file, verify remapper redirects reads to target
###############################################################################
echo "=== Group 1: Directory remapping ==="
TARGET1=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET1")

# Pre-create the original dir (simulating "program ran once already")
mkdir -p "$HOME/.dummy-test"
echo "original" > "$HOME/.dummy-test/data.txt"

# Pre-populate the target with different content
mkdir -p "$TARGET1/.dummy-test"
echo "remapped" > "$TARGET1/.dummy-test/data.txt"

# Run a command under remapper — it should see the target content
RESULT=$("$REMAPPER" "$TARGET1" "$HOME/.dummy*" -- cat "$HOME/.dummy-test/data.txt")
if [ "$RESULT" = "remapped" ]; then
    pass "directory read sees target content"
else
    fail "directory read sees target content (got '$RESULT', expected 'remapped')"
fi

# Verify the original is untouched outside of remapper
assert_file_content "$HOME/.dummy-test/data.txt" "original" \
    "original file untouched outside remapper"

###############################################################################
# Group 2: File remapping
#   Create ~/.dummy.json, verify remapper redirects it to target
###############################################################################
echo "=== Group 2: File remapping ==="
TARGET2=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET2")

# Pre-create the original file
echo '{"version": "original"}' > "$HOME/.dummy.json"

# Pre-populate the target with different content
echo '{"version": "remapped"}' > "$TARGET2/.dummy.json"

RESULT=$("$REMAPPER" "$TARGET2" "$HOME/.dummy*" -- cat "$HOME/.dummy.json")
if [ "$RESULT" = '{"version": "remapped"}' ]; then
    pass "file read sees target content"
else
    fail "file read sees target content (got '$RESULT')"
fi

assert_file_content "$HOME/.dummy.json" '{"version": "original"}' \
    "original file untouched outside remapper"

###############################################################################
# Group 3: Writes go to target
#   Verify that writes inside remapper go to the target, not the original
###############################################################################
echo "=== Group 3: Writes go to target ==="
TARGET3=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET3")

mkdir -p "$HOME/.dummy-write"
echo "before" > "$HOME/.dummy-write/test.txt"

mkdir -p "$TARGET3/.dummy-write"
echo "before-target" > "$TARGET3/.dummy-write/test.txt"

# Write to the path inside remapper
"$REMAPPER" "$TARGET3" "$HOME/.dummy*" -- \
    sh -c "echo 'written-by-child' > '$HOME/.dummy-write/test.txt'"

# The target should have the new content
assert_file_content "$TARGET3/.dummy-write/test.txt" "written-by-child" \
    "write landed in target dir"

# The original should be untouched
assert_file_content "$HOME/.dummy-write/test.txt" "before" \
    "original untouched after remapped write"

###############################################################################
# Group 4: Multiple patterns
#   Test that multiple glob patterns all get remapped
###############################################################################
echo "=== Group 4: Multiple patterns ==="
TARGET4=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET4")

mkdir -p "$HOME/.alpha-conf"
echo "alpha-orig" > "$HOME/.alpha-conf/a.txt"

mkdir -p "$HOME/.beta-conf"
echo "beta-orig" > "$HOME/.beta-conf/b.txt"

mkdir -p "$TARGET4/.alpha-conf"
echo "alpha-remapped" > "$TARGET4/.alpha-conf/a.txt"

mkdir -p "$TARGET4/.beta-conf"
echo "beta-remapped" > "$TARGET4/.beta-conf/b.txt"

RESULT_A=$("$REMAPPER" "$TARGET4" "$HOME/.alpha*" "$HOME/.beta*" -- \
    cat "$HOME/.alpha-conf/a.txt")
if [ "$RESULT_A" = "alpha-remapped" ]; then
    pass "first pattern remapped"
else
    fail "first pattern remapped (got '$RESULT_A')"
fi

RESULT_B=$("$REMAPPER" "$TARGET4" "$HOME/.alpha*" "$HOME/.beta*" -- \
    cat "$HOME/.beta-conf/b.txt")
if [ "$RESULT_B" = "beta-remapped" ]; then
    pass "second pattern remapped"
else
    fail "second pattern remapped (got '$RESULT_B')"
fi

###############################################################################
# Group 5: Non-matching paths are unaffected
#   Verify that paths not matching any pattern are passed through unchanged
###############################################################################
echo "=== Group 5: Non-matching paths pass through ==="
TARGET5=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET5")

mkdir -p "$HOME/.dummy-test5"
echo "dummy5" > "$HOME/.dummy-test5/file.txt"

# Create a non-matching file
echo "untouched" > "$HOME/.other-file"

mkdir -p "$TARGET5/.dummy-test5"
echo "remapped5" > "$TARGET5/.dummy-test5/file.txt"

# The non-matching file should be readable with its original content
RESULT=$("$REMAPPER" "$TARGET5" "$HOME/.dummy*" -- cat "$HOME/.other-file")
if [ "$RESULT" = "untouched" ]; then
    pass "non-matching path passes through"
else
    fail "non-matching path passes through (got '$RESULT')"
fi

###############################################################################
# Group 6: Child processes inherit namespace
#   Verify that child processes spawned by the program also see the remapped paths
###############################################################################
echo "=== Group 6: Child processes inherit namespace ==="
TARGET6=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET6")

mkdir -p "$HOME/.dummy-inherit"
echo "original-inherit" > "$HOME/.dummy-inherit/child.txt"

mkdir -p "$TARGET6/.dummy-inherit"
echo "remapped-inherit" > "$TARGET6/.dummy-inherit/child.txt"

# sh -c spawns a child; the child reads the file
RESULT=$("$REMAPPER" "$TARGET6" "$HOME/.dummy*" -- \
    sh -c "cat '$HOME/.dummy-inherit/child.txt'")
if [ "$RESULT" = "remapped-inherit" ]; then
    pass "child process sees remapped content"
else
    fail "child process sees remapped content (got '$RESULT')"
fi

###############################################################################
# Group 7: Statically linked binary works
#   This is the whole reason we use namespaces instead of LD_PRELOAD.
#   Compile a static test binary and verify remapping works on it.
###############################################################################
echo "=== Group 7: Statically linked binary ==="
TARGET7=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET7")

# Write a tiny C program that reads a file and prints its content
STATIC_SRC="$TARGET7/static_test.c"
STATIC_BIN="$TARGET7/static_test"
cat > "$STATIC_SRC" << 'CSOURCE'
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *fp = fopen(argv[1], "r");
    if (!fp) { perror(argv[1]); return 1; }
    char buf[256];
    while (fgets(buf, sizeof(buf), fp))
        fputs(buf, stdout);
    fclose(fp);
    return 0;
}
CSOURCE

# Compile statically
if gcc -static -o "$STATIC_BIN" "$STATIC_SRC" 2>/dev/null; then
    mkdir -p "$HOME/.dummy-static"
    echo "original-static" > "$HOME/.dummy-static/data.txt"

    mkdir -p "$TARGET7/.dummy-static"
    echo "remapped-static" > "$TARGET7/.dummy-static/data.txt"

    RESULT=$("$REMAPPER" "$TARGET7" "$HOME/.dummy*" -- \
        "$STATIC_BIN" "$HOME/.dummy-static/data.txt")
    if [ "$RESULT" = "remapped-static" ]; then
        pass "static binary sees remapped content"
    else
        fail "static binary sees remapped content (got '$RESULT')"
    fi
else
    echo "  SKIP: static compilation not available (no static libc)"
fi

###############################################################################
# Group 8: Debug log output
#   Verify that --debug-log produces output
###############################################################################
echo "=== Group 8: Debug log ==="
TARGET8=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET8")
DEBUGLOG="$TARGET8/debug.log"

mkdir -p "$HOME/.dummy-debug"
echo "debug-test" > "$HOME/.dummy-debug/file.txt"
mkdir -p "$TARGET8/.dummy-debug"
echo "debug-remapped" > "$TARGET8/.dummy-debug/file.txt"

"$REMAPPER" --debug-log "$DEBUGLOG" "$TARGET8" "$HOME/.dummy*" -- \
    cat "$HOME/.dummy-debug/file.txt" > /dev/null

if [ -f "$DEBUGLOG" ] && grep -q "\[remapper\]" "$DEBUGLOG"; then
    pass "debug log file created with content"
else
    fail "debug log file created with content"
fi

if grep -q "mounted:" "$DEBUGLOG"; then
    pass "debug log shows mount operations"
else
    fail "debug log shows mount operations"
fi

###############################################################################
# Group 9: New files created inside remapper go to target
#   Test creating new files/dirs that didn't exist before
###############################################################################
echo "=== Group 9: New file creation in remapped dir ==="
TARGET9=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET9")

mkdir -p "$HOME/.dummy-create"
mkdir -p "$TARGET9/.dummy-create"

# Create a new file inside the remapped directory
"$REMAPPER" "$TARGET9" "$HOME/.dummy*" -- \
    sh -c "echo 'new-content' > '$HOME/.dummy-create/new-file.txt'"

assert_file_content "$TARGET9/.dummy-create/new-file.txt" "new-content" \
    "new file created in target dir"

assert_file_not_exists "$HOME/.dummy-create/new-file.txt" \
    "new file not created in original dir"

###############################################################################
# Group 10: No-match warning
#   When no paths match, remapper should warn and still exec the program
###############################################################################
echo "=== Group 10: No-match warning ==="
TARGET10=$(mktemp -d)
CLEANUP_DIRS+=("$TARGET10")

# Use a pattern that matches nothing
RESULT=$("$REMAPPER" "$TARGET10" "$HOME/.nonexistent-pattern*" -- echo "ran-ok" 2>/dev/null || true)
if [ "$RESULT" = "ran-ok" ]; then
    pass "program runs even with no matches"
else
    fail "program runs even with no matches (got '$RESULT')"
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
