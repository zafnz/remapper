#!/bin/bash
#
# deploy-from-linux.sh - Build Linux binary, tag, push, and create a GitHub release.
#
# Usage:
#   ./deploy-from-linux.sh                    # auto-increment patch version
#   ./deploy-from-linux.sh v1.2.3             # use an explicit version
#   ./deploy-from-linux.sh --dry-run          # build but skip tagging and release
#
# Prerequisites:
#   - Clean working tree (everything committed and pushed)
#   - gh CLI authenticated with GitHub
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
DRY_RUN=0

########################################
# 0. Parse flags
########################################
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

if [ "$DRY_RUN" -eq 0 ]; then
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

RELEASE="release"

########################################
# 4. Build release assets
########################################
rm -rf "$RELEASE"

info "Building Linux binary..."
make clean
make

# Collect all release assets
ASSETS=()
for asset in "$RELEASE"/remapper-*; do
    [ -f "$asset" ] || die "No release assets found in $RELEASE/"
    OS_ARCH="${asset#$RELEASE/remapper-}"
    info "Built: $asset ($(du -h "$asset" | cut -f1 | xargs))"
    ASSETS+=("$asset#remapper ($OS_ARCH)")
done

if [ "$DRY_RUN" -eq 1 ]; then
    warn "Skipped tagging and release creation (--dry-run)."
    exit 0
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
