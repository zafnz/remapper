#!/bin/sh
# Install script for remapper
# Usage: curl -fsSL https://raw.githubusercontent.com/zafnz/remapper/main/install.sh | sh
#
# Options (via environment variables):
#   INSTALL_DIR  — where to install (default: ~/.local/bin)
#   VERSION      — release tag to install (default: latest)

set -eu

REPO="zafnz/remapper"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${VERSION:-latest}"

# --- Platform detection ---

OS=$(uname -s)
ARCH=$(uname -m)

case "$OS" in
    Linux|Darwin) ;;
    *) echo "Error: unsupported OS: $OS" >&2; exit 1 ;;
esac

case "$ARCH" in
    x86_64|aarch64|arm64) ;;
    *) echo "Error: unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

ASSET="remapper-${OS}-${ARCH}"

if [ "$VERSION" = "latest" ]; then
    URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
else
    URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
fi

# --- Download ---

echo "Installing remapper for ${OS}/${ARCH}..."

mkdir -p "$INSTALL_DIR"

if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$INSTALL_DIR/remapper" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$INSTALL_DIR/remapper" "$URL"
else
    echo "Error: curl or wget required" >&2
    exit 1
fi

chmod +x "$INSTALL_DIR/remapper"

# --- macOS: remove quarantine flag ---

if [ "$OS" = "Darwin" ]; then
    xattr -d com.apple.quarantine "$INSTALL_DIR/remapper" 2>/dev/null || true
fi

echo "Installed to $INSTALL_DIR/remapper"

# --- PATH check ---

case ":${PATH}:" in
    *":${INSTALL_DIR}:"*) ;;
    *)
        EXPORT_LINE="export PATH=\"$INSTALL_DIR:\$PATH\""
        ADDED=false
        for RC_FILE in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
            if [ -f "$RC_FILE" ]; then
                if ! grep -qF "$INSTALL_DIR" "$RC_FILE" 2>/dev/null; then
                    echo "" >> "$RC_FILE"
                    echo "# Added by remapper installer" >> "$RC_FILE"
                    echo "$EXPORT_LINE" >> "$RC_FILE"
                    echo "Added $INSTALL_DIR to PATH in $RC_FILE"
                    ADDED=true
                else
                    ADDED=true
                fi
            fi
        done
        if [ "$ADDED" = false ]; then
            # No rc file found, create .profile
            echo "" >> "$HOME/.profile"
            echo "# Added by remapper installer" >> "$HOME/.profile"
            echo "$EXPORT_LINE" >> "$HOME/.profile"
            echo "Added $INSTALL_DIR to PATH in ~/.profile"
        fi
        ;;
esac

# --- AppArmor check (Linux only) ---

if [ "$OS" = "Linux" ] && [ -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
    RESTRICT=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo "0")
    if [ "$RESTRICT" = "1" ]; then
        echo ""
        echo "AppArmor is restricting unprivileged user namespaces on this system."
        echo "remapper needs an AppArmor profile to work."
        echo ""
        printf "Install AppArmor profile to /usr/local/bin/remapper? [y/N] "
        read -r REPLY </dev/tty
        case "$REPLY" in
            [yY]|[yY][eE][sS])
                echo "Running: sudo $INSTALL_DIR/remapper --install-apparmor-at /usr/local/bin/remapper"
                sudo "$INSTALL_DIR/remapper" --install-apparmor-at /usr/local/bin/remapper
                echo ""
                echo "remapper is now at /usr/local/bin/remapper with AppArmor profile installed."
                ;;
            *)
                echo ""
                echo "Skipped. You can install the profile later with:"
                echo "  sudo $INSTALL_DIR/remapper --install-apparmor"
                echo ""
                echo "Or move to /usr/local/bin with a profile:"
                echo "  sudo $INSTALL_DIR/remapper --install-apparmor-at /usr/local/bin/remapper"
                ;;
        esac
    fi
fi

echo ""
echo "Done! Open a new terminal to begin using remapper."
