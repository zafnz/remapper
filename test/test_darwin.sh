#!/bin/bash
set -euo pipefail

# Resolve paths relative to the project root (one level up from this script)
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

# Use a temp directory for remapper config/cache
RMP_TMPDIR=$(mktemp -d)
CLEANUP_DIRS+=("$RMP_TMPDIR")
export RMP_CONFIG="$RMP_TMPDIR/config"
export RMP_CACHE="$RMP_TMPDIR/cache"

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
# Group 1: Filesystem interposition (existing test_interpose + verify)
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
# Group 2: Hardened binary — direct exec via remapper CLI
###############################################################################
echo "=== Group 2: Hardened binary — direct exec ==="
TMPDIR2=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR2")
DEBUGLOG2="$TMPDIR2/debug.log"

HARDENED_ABS="$(cd "$(dirname "$BUILD/hardened_test")" && pwd)/$(basename "$BUILD/hardened_test")"
"$BUILD/remapper" --debug-log "$DEBUGLOG2" "$TMPDIR2" "$HOME/.dummy*" -- "$HARDENED_ABS"

if [ -f "$TMPDIR2/.dummy-hardened/proof.txt" ]; then
    pass "hardened direct exec: proof.txt in target dir"
else
    fail "hardened direct exec: proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened/proof.txt" ]; then
    pass "hardened direct exec: proof.txt NOT in home"
else
    fail "hardened direct exec: proof.txt found in home (should have been redirected)"
    rm -rf "$HOME/.dummy-hardened"
fi

###############################################################################
# Group 3: Hardened binary — posix_spawn (full path) via spawn_hardened
###############################################################################
echo "=== Group 3: Hardened binary — posix_spawn (full path) ==="
TMPDIR3=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR3")
DEBUGLOG3="$TMPDIR3/debug.log"

HARDENED_ABS="$(cd "$(dirname "$BUILD/hardened_test")" && pwd)/$(basename "$BUILD/hardened_test")"

"$BUILD/remapper" --debug-log "$DEBUGLOG3" "$TMPDIR3" "$HOME/.dummy*" -- \
    "$BUILD/spawn_hardened" "$HARDENED_ABS"

if [ -f "$TMPDIR3/.dummy-hardened/proof.txt" ]; then
    pass "posix_spawn full path: proof.txt in target dir"
else
    fail "posix_spawn full path: proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened/proof.txt" ]; then
    pass "posix_spawn full path: proof.txt NOT in home"
else
    fail "posix_spawn full path: proof.txt found in home (should have been redirected)"
    rm -rf "$HOME/.dummy-hardened"
fi

###############################################################################
# Group 4: Hardened binary — posix_spawnp (PATH lookup)
###############################################################################
echo "=== Group 4: Hardened binary — posix_spawnp (PATH lookup) ==="
TMPDIR4=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR4")
DEBUGLOG4="$TMPDIR4/debug.log"

HARDENED_DIR="$(cd "$(dirname "$BUILD/hardened_test")" && pwd)"
export PATH="$HARDENED_DIR:$PATH"

"$BUILD/remapper" --debug-log "$DEBUGLOG4" "$TMPDIR4" "$HOME/.dummy*" -- \
    "$BUILD/spawn_hardened" --spawnp hardened_test

if [ -f "$TMPDIR4/.dummy-hardened/proof.txt" ]; then
    pass "posix_spawnp PATH lookup: proof.txt in target dir"
else
    fail "posix_spawnp PATH lookup: proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened/proof.txt" ]; then
    pass "posix_spawnp PATH lookup: proof.txt NOT in home"
else
    fail "posix_spawnp PATH lookup: proof.txt found in home (should have been redirected)"
    rm -rf "$HOME/.dummy-hardened"
fi

###############################################################################
# Group 5: Hardened binary — execvp (PATH lookup)
###############################################################################
echo "=== Group 5: Hardened binary — execvp (PATH lookup) ==="
TMPDIR5=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR5")
DEBUGLOG5="$TMPDIR5/debug.log"

"$BUILD/remapper" --debug-log "$DEBUGLOG5" "$TMPDIR5" "$HOME/.dummy*" -- \
    "$BUILD/spawn_hardened" --execvp hardened_test

if [ -f "$TMPDIR5/.dummy-hardened/proof.txt" ]; then
    pass "execvp PATH lookup: proof.txt in target dir"
else
    fail "execvp PATH lookup: proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened/proof.txt" ]; then
    pass "execvp PATH lookup: proof.txt NOT in home"
else
    fail "execvp PATH lookup: proof.txt found in home (should have been redirected)"
    rm -rf "$HOME/.dummy-hardened"
fi

###############################################################################
# Group 6: Hardened shebang interpreter
#   Script whose #! interpreter is a hardened binary (not SIP, just hardened).
#   Tests that both remapper.c and the interposer detect and re-sign it.
###############################################################################
echo "=== Group 6: Hardened shebang interpreter ==="
TMPDIR6=$(mktemp -d)
CLEANUP_DIRS+=("$TMPDIR6")

INTERP_ABS="$(cd "$(dirname "$BUILD/hardened_interp")" && pwd)/$(basename "$BUILD/hardened_interp")"

# Create a script with a hardened-binary shebang
SCRIPT6="$TMPDIR6/test_script"
cat > "$SCRIPT6" <<ENDOFSCRIPT
#!${INTERP_ABS}
# test script with hardened interpreter
ENDOFSCRIPT
chmod +x "$SCRIPT6"

# 6a: remapper directly runs the script
echo "--- 6a: remapper → script with hardened shebang ---"
TARGET6a="$TMPDIR6/target-a"
mkdir -p "$TARGET6a"
DEBUGLOG6a="$TMPDIR6/debug-a.log"

"$BUILD/remapper" --debug-log "$DEBUGLOG6a" "$TARGET6a" "$HOME/.dummy*" -- "$SCRIPT6"

if [ -f "$TARGET6a/.dummy-hardened-interp/proof.txt" ]; then
    pass "hardened shebang (direct): proof.txt in target dir"
else
    fail "hardened shebang (direct): proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened-interp/proof.txt" ]; then
    pass "hardened shebang (direct): proof.txt NOT in home"
else
    fail "hardened shebang (direct): proof.txt found in home"
    rm -rf "$HOME/.dummy-hardened-interp"
fi

# 6b: hardened spawner (itself hardened) spawns the script — full chain
#   remapper re-signs hardened_spawner → interposer detects hardened shebang → re-signs interpreter
echo "--- 6b: hardened spawner → script with hardened shebang ---"
TARGET6b="$TMPDIR6/target-b"
mkdir -p "$TARGET6b"
DEBUGLOG6b="$TMPDIR6/debug-b.log"

"$BUILD/remapper" --debug-log "$DEBUGLOG6b" "$TARGET6b" "$HOME/.dummy*" -- \
    "$BUILD/hardened_spawner" "$SCRIPT6"

if [ -f "$TARGET6b/.dummy-hardened-interp/proof.txt" ]; then
    pass "hardened shebang (hardened spawner): proof.txt in target dir"
else
    fail "hardened shebang (hardened spawner): proof.txt NOT in target dir"
fi
if [ ! -f "$HOME/.dummy-hardened-interp/proof.txt" ]; then
    pass "hardened shebang (hardened spawner): proof.txt NOT in home"
else
    fail "hardened shebang (hardened spawner): proof.txt found in home"
    rm -rf "$HOME/.dummy-hardened-interp"
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
