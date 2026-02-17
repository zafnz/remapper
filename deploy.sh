#!/bin/bash
#
# deploy.sh - Tag, push, create a GitHub release, and upload the binary.
#
# Usage:
#   ./deploy.sh                    # auto-increment patch version (v0.0.1 → v0.0.2)
#   ./deploy.sh v1.2.3             # use an explicit version
#   ./deploy.sh --docker-linux     # also build and include Linux binary via Docker
#   ./deploy.sh --docker-linux v1.2.3
#
# Prerequisites:
#   - Clean working tree (everything committed and pushed)
#   - gh CLI authenticated with GitHub
#   - Docker installed (if using --docker-linux)
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

die() { echo -e "${RED}Error: $1${NC}" >&2; exit 1; }
info() { echo -e "${GREEN}$1${NC}"; }
warn() { echo -e "${YELLOW}$1${NC}"; }

BUILD="build"
BINARY="$BUILD/remapper"
DOCKER_LINUX=0

########################################
# 0. Parse flags
########################################
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --docker-linux) DOCKER_LINUX=1 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

########################################
# 1. Ensure working tree is clean
########################################
if ! git diff --quiet || ! git diff --cached --quiet; then
    die "Working tree has uncommitted changes. Commit or stash them first."
fi

if [ -n "$(git ls-files --others --exclude-standard)" ]; then
    die "There are untracked files. Commit or remove them first."
fi

########################################
# 2. Ensure we're pushed to remote
########################################
BRANCH=$(git rev-parse --abbrev-ref HEAD)
REMOTE_REF=$(git rev-parse "origin/$BRANCH" 2>/dev/null || echo "")
LOCAL_REF=$(git rev-parse HEAD)

if [ -z "$REMOTE_REF" ]; then
    die "Branch '$BRANCH' has no upstream. Push it first: git push -u origin $BRANCH"
fi

if [ "$LOCAL_REF" != "$REMOTE_REF" ]; then
    die "Local branch is ahead of or behind origin/$BRANCH. Push or pull first."
fi

########################################
# 3. Determine the new version
########################################
if [ $# -ge 1 ]; then
    # Explicit version provided
    NEW_VERSION="$1"
    # Ensure it starts with 'v'
    if [[ ! "$NEW_VERSION" =~ ^v ]]; then
        NEW_VERSION="v$NEW_VERSION"
    fi
else
    # Auto-increment: find the latest tag matching vX.Y.Z
    LATEST_TAG=$(git tag -l 'v*' --sort=-v:refname | head -1)

    if [ -z "$LATEST_TAG" ]; then
        # No tags yet -- start at v0.0.1
        NEW_VERSION="v0.0.1"
    else
        # Parse major.minor.patch and increment patch
        if [[ "$LATEST_TAG" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
            MAJOR="${BASH_REMATCH[1]}"
            MINOR="${BASH_REMATCH[2]}"
            PATCH="${BASH_REMATCH[3]}"
            NEW_VERSION="v${MAJOR}.${MINOR}.$((PATCH + 1))"
        else
            die "Latest tag '$LATEST_TAG' doesn't match vX.Y.Z format. Specify version manually."
        fi
    fi
fi

# Check the tag doesn't already exist
if git rev-parse "$NEW_VERSION" >/dev/null 2>&1; then
    die "Tag '$NEW_VERSION' already exists."
fi

info "Version: $NEW_VERSION"

########################################
# 4. Build the macOS binary
########################################
info "Building macOS binary..."
make clean
make

if [ ! -f "$BINARY" ]; then
    die "Build failed: $BINARY not found."
fi

# Determine architecture for the release asset name
ARCH=$(uname -m)
MACOS_ASSET="$BUILD/remapper-macos-${ARCH}"
cp "$BINARY" "$MACOS_ASSET"

info "Built: $MACOS_ASSET ($(du -h "$MACOS_ASSET" | cut -f1 | xargs))"

ASSETS=("$MACOS_ASSET#remapper (macOS $ARCH)")

########################################
# 4b. Build the Linux binary (optional)
########################################
if [ "$DOCKER_LINUX" -eq 1 ]; then
    info "Building Linux binary via Docker..."

    LINUX_ARCH=$(docker run --rm ubuntu:24.04 uname -m)
    LINUX_ASSET="$BUILD/remapper-linux-${LINUX_ARCH}"

    docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c '
        apt-get update -qq && apt-get install -y -qq gcc make binutils >/dev/null 2>&1
        make clean && make' || die "Linux Docker build failed."

    if [ ! -f "$BINARY" ]; then
        die "Linux build failed: $BINARY not found."
    fi

    cp "$BINARY" "$LINUX_ASSET"
    info "Built: $LINUX_ASSET ($(du -h "$LINUX_ASSET" | cut -f1 | xargs))"

    ASSETS+=("$LINUX_ASSET#remapper (Linux $LINUX_ARCH)")

    # Rebuild macOS so the local build dir is back to native
    info "Rebuilding macOS binary..."
    make clean
    make
    cp "$BINARY" "$MACOS_ASSET"
fi

########################################
# 5. Tag and push
########################################
info "Tagging $NEW_VERSION..."
git tag "$NEW_VERSION"
git push origin "$NEW_VERSION"

########################################
# 6. Create draft release and upload
########################################
info "Creating draft release on GitHub..."
gh release create "$NEW_VERSION" \
    --draft \
    --title "$NEW_VERSION" \
    --generate-notes \
    "${ASSETS[@]}"

RELEASE_URL=$(gh release view "$NEW_VERSION" --json url -q '.url')
info "Done! Draft release created: $RELEASE_URL"
warn "Review the release notes and publish when ready."
