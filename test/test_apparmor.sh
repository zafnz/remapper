#!/bin/bash
set -euo pipefail

# AppArmor profile installation tests for remapper.
#
# Must be run inside the remapper-test-apparmor container (non-root user
# with sudo NOPASSWD, /etc/apparmor.d/ exists).
#
# Usage:
#   docker build -f Dockerfile.test -t remapper-test-apparmor .
#   docker run --rm -v "$(pwd)":/src -w /src remapper-test-apparmor ./test/test_apparmor.sh
#
# We compile a test binary with fake sysctl/apparmor paths so we can test
# the detection and profile installation without real AppArmor kernel support.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$PROJECT_ROOT/build"
PASS=0
FAIL=0
CLEANUP=()

cleanup() {
    for f in "${CLEANUP[@]}"; do
        rm -rf "$f" 2>/dev/null || true
    done
    # Clean up any installed profiles or binaries
    sudo rm -f /etc/apparmor.d/home.testuser.* 2>/dev/null || true
    sudo rm -f /etc/apparmor.d/usr.local.bin.remapper 2>/dev/null || true
    sudo rm -f /usr/local/bin/remapper 2>/dev/null || true
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

# Build the test binary with fake paths
FAKE_DIR=$(mktemp -d)
CLEANUP+=("$FAKE_DIR")
FAKE_APPARMOR="$FAKE_DIR/apparmor_sysctl"
FAKE_USERNS="$FAKE_DIR/userns_sysctl"

echo -n "1" > "$FAKE_APPARMOR"
echo -n "0" > "$FAKE_USERNS"

echo "Building test binary with fake sysctl paths..."
gcc -Wall -Wextra -O2 \
    -DAPPARMOR_SYSCTL="\"$FAKE_APPARMOR\"" \
    -DUSERNS_SYSCTL="\"$FAKE_USERNS\"" \
    -o "$BUILD/remapper-apparmor-test" \
    "$PROJECT_ROOT/remapper_linux.c"

RMP="$BUILD/remapper-apparmor-test"

###############################################################################
# Group 1: --install-apparmor without sudo fails
###############################################################################
echo "=== Group 1: --install-apparmor requires sudo ==="

OUTPUT=$("$RMP" --install-apparmor 2>&1 || true)
if echo "$OUTPUT" | grep -q "must be run with sudo"; then
    pass "--install-apparmor rejected without sudo"
else
    fail "--install-apparmor rejected without sudo (got: $OUTPUT)"
fi

###############################################################################
# Group 2: --install-apparmor with sudo writes profile to correct path
###############################################################################
echo "=== Group 2: --install-apparmor writes profile ==="

# Copy binary to a known location so /proc/self/exe resolves predictably
cp "$RMP" /home/testuser/.local/bin/remapper
CLEANUP+=("/home/testuser/.local/bin/remapper")

OUTPUT=$(sudo /home/testuser/.local/bin/remapper --install-apparmor 2>&1 || true)
EXPECTED_PROFILE="/etc/apparmor.d/home.testuser..local.bin.remapper"

if [ -f "$EXPECTED_PROFILE" ]; then
    pass "profile file created at $EXPECTED_PROFILE"
else
    fail "profile file created at $EXPECTED_PROFILE (not found)"
fi

# Check profile content contains the binary path
if grep -q "/home/testuser/.local/bin/remapper" "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile contains correct binary path"
else
    fail "profile contains correct binary path"
fi

# Check profile has userns permission
if grep -q "userns," "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile contains userns permission"
else
    fail "profile contains userns permission"
fi

# Check profile has the right structure
if grep -q "abi <abi/4.0>" "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile has abi declaration"
else
    fail "profile has abi declaration"
fi

if grep -q 'flags=(unconfined)' "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile has unconfined flag"
else
    fail "profile has unconfined flag"
fi

# Clean up for next test
sudo rm -f "$EXPECTED_PROFILE"

###############################################################################
# Group 3: --install-apparmor-at copies binary and writes profile
###############################################################################
echo "=== Group 3: --install-apparmor-at moves binary and writes profile ==="

# Put a fresh copy in ~/.local/bin
cp "$RMP" /home/testuser/.local/bin/remapper

OUTPUT=$(sudo /home/testuser/.local/bin/remapper --install-apparmor-at /usr/local/bin/remapper 2>&1 || true)

# Binary should be at new location
if [ -f /usr/local/bin/remapper ]; then
    pass "binary copied to /usr/local/bin/remapper"
else
    fail "binary copied to /usr/local/bin/remapper (not found)"
fi

# Binary should be removed from old location
if [ ! -f /home/testuser/.local/bin/remapper ]; then
    pass "original binary removed from ~/.local/bin"
else
    fail "original binary removed from ~/.local/bin (still exists)"
fi

# Profile should point to /usr/local/bin/remapper
EXPECTED_PROFILE="/etc/apparmor.d/usr.local.bin.remapper"
if [ -f "$EXPECTED_PROFILE" ]; then
    pass "profile file created at $EXPECTED_PROFILE"
else
    fail "profile file created at $EXPECTED_PROFILE (not found)"
fi

if grep -q "/usr/local/bin/remapper" "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile points to /usr/local/bin/remapper"
else
    fail "profile points to /usr/local/bin/remapper"
fi

# Should NOT reference the old path
if ! grep -q "/home/testuser" "$EXPECTED_PROFILE" 2>/dev/null; then
    pass "profile does not reference old path"
else
    fail "profile does not reference old path"
fi

###############################################################################
# Group 4: Error detection messages
###############################################################################
echo "=== Group 4: Error detection messages ==="

# AppArmor detection (fake sysctl = "1")
echo -n "1" > "$FAKE_APPARMOR"
rm -f "$FAKE_USERNS"

mkdir -p /tmp/home/.dummy-test
echo "orig" > /tmp/home/.dummy-test/data.txt

OUTPUT=$(HOME=/tmp/home "$RMP" /tmp/target "/tmp/home/.dummy*" -- cat /tmp/home/.dummy-test/data.txt 2>&1 || true)

if echo "$OUTPUT" | grep -q "AppArmor is blocking"; then
    pass "AppArmor restriction detected"
else
    fail "AppArmor restriction detected (got: $OUTPUT)"
fi

if echo "$OUTPUT" | grep -q "sudo.*--install-apparmor"; then
    pass "AppArmor message suggests --install-apparmor"
else
    fail "AppArmor message suggests --install-apparmor"
fi

# userns_clone detection (fake sysctl = "0", no apparmor)
rm -f "$FAKE_APPARMOR"
echo -n "0" > "$FAKE_USERNS"

OUTPUT=$(HOME=/tmp/home "$RMP" /tmp/target "/tmp/home/.dummy*" -- cat /tmp/home/.dummy-test/data.txt 2>&1 || true)

if echo "$OUTPUT" | grep -q "Unprivileged user namespaces are disabled"; then
    pass "userns_clone=0 restriction detected"
else
    fail "userns_clone=0 restriction detected (got: $OUTPUT)"
fi

if echo "$OUTPUT" | grep -q "kernel.unprivileged_userns_clone=1"; then
    pass "userns message suggests sysctl fix"
else
    fail "userns message suggests sysctl fix"
fi

if echo "$OUTPUT" | grep -q "/etc/sysctl.d/"; then
    pass "userns message suggests persistent config"
else
    fail "userns message suggests persistent config"
fi

# Generic detection (no sysctl files at all)
rm -f "$FAKE_APPARMOR" "$FAKE_USERNS"

OUTPUT=$(HOME=/tmp/home "$RMP" /tmp/target "/tmp/home/.dummy*" -- cat /tmp/home/.dummy-test/data.txt 2>&1 || true)

if echo "$OUTPUT" | grep -q "appear to be disabled"; then
    pass "generic restriction message shown"
else
    fail "generic restriction message shown (got: $OUTPUT)"
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
