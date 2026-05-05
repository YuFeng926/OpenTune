#!/bin/bash
# ==============================================================================
# OpenTune Installer
#
# Double-click this file to install OpenTune to /Applications, and optionally
# install the VST3 plugin to ~/Library/Audio/Plug-Ins/VST3 or system-wide.
# Automatically removes macOS quarantine flag for unsigned builds.
# ==============================================================================

set -euo pipefail

APP_NAME="OpenTune.app"
VST3_NAME="OpenTune.vst3"
INSTALL_DIR="/Applications"
VST3_USER_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
VST3_SYSTEM_DIR="/Library/Audio/Plug-Ins/VST3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_APP="$SCRIPT_DIR/$APP_NAME"
SOURCE_VST3="$SCRIPT_DIR/$VST3_NAME"

echo "========================================"
echo "  OpenTune Installer"
echo "========================================"
echo ""

# Verify source app exists
if [ ! -d "$SOURCE_APP" ]; then
    echo "Error: $APP_NAME not found in the same directory as this installer."
    echo "Please run this script from within the mounted DMG."
    echo ""
    read -n 1 -s -r -p "Press any key to exit..."
    exit 1
fi

# ------------------------------------------------------------------------------
# Standalone .app installation
# ------------------------------------------------------------------------------

# Check if already installed
if [ -d "$INSTALL_DIR/$APP_NAME" ]; then
    echo "OpenTune is already installed at $INSTALL_DIR/$APP_NAME"
    echo ""
    read -p "Overwrite existing installation? [y/N] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Installation cancelled."
        echo ""
        read -n 1 -s -r -p "Press any key to exit..."
        exit 0
    fi
    echo "Removing old version..."
    rm -rf "$INSTALL_DIR/$APP_NAME"
fi

# Copy app to /Applications
echo "Installing $APP_NAME to $INSTALL_DIR..."
cp -R "$SOURCE_APP" "$INSTALL_DIR/"

# Remove quarantine attribute
echo "Removing quarantine flag..."
xattr -rd com.apple.quarantine "$INSTALL_DIR/$APP_NAME" 2>/dev/null || true

echo ""
echo "$APP_NAME installation complete."
echo ""

# ------------------------------------------------------------------------------
# VST3 plugin installation (when bundled in DMG)
# ------------------------------------------------------------------------------

VST3_INSTALLED_PATH=""

if [ -d "$SOURCE_VST3" ]; then
    echo "----------------------------------------"
    echo "  VST3 Plugin Installation"
    echo "----------------------------------------"
    echo ""
    echo "Found $VST3_NAME. Choose install location:"
    echo "  [U] User only       (~/Library/Audio/Plug-Ins/VST3, no admin password)"
    echo "  [S] System-wide     (/Library/Audio/Plug-Ins/VST3, requires admin password)"
    echo "  [N] Skip VST3 installation"
    echo ""
    read -p "Choice [U/s/n]: " -n 1 -r
    echo ""

    VST3_TARGET=""
    USE_SUDO=""

    case "${REPLY:-U}" in
        u|U|"")
            VST3_TARGET="$VST3_USER_DIR"
            ;;
        s|S)
            VST3_TARGET="$VST3_SYSTEM_DIR"
            USE_SUDO="sudo"
            ;;
        n|N)
            echo "VST3 installation skipped."
            ;;
        *)
            echo "Unrecognized choice — defaulting to user-level install."
            VST3_TARGET="$VST3_USER_DIR"
            ;;
    esac

    if [ -n "$VST3_TARGET" ]; then
        echo ""
        echo "Installing $VST3_NAME to $VST3_TARGET..."

        $USE_SUDO mkdir -p "$VST3_TARGET"

        if [ -d "$VST3_TARGET/$VST3_NAME" ]; then
            echo "Removing existing $VST3_NAME at $VST3_TARGET..."
            $USE_SUDO rm -rf "$VST3_TARGET/$VST3_NAME"
        fi

        $USE_SUDO cp -R "$SOURCE_VST3" "$VST3_TARGET/"
        $USE_SUDO xattr -rd com.apple.quarantine "$VST3_TARGET/$VST3_NAME" 2>/dev/null || true

        VST3_INSTALLED_PATH="$VST3_TARGET/$VST3_NAME"
        echo "$VST3_NAME installed to $VST3_INSTALLED_PATH"
    fi
    echo ""
fi

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------

echo "========================================"
echo "  Installation Summary"
echo "========================================"
echo "  Standalone : $INSTALL_DIR/$APP_NAME"
if [ -n "$VST3_INSTALLED_PATH" ]; then
    echo "  VST3       : $VST3_INSTALLED_PATH"
elif [ -d "$SOURCE_VST3" ]; then
    echo "  VST3       : (skipped)"
fi
echo "========================================"
echo ""

# Ask to launch standalone
read -p "Launch OpenTune now? [Y/n] " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Nn]$ ]]; then
    open "$INSTALL_DIR/$APP_NAME"
fi

echo ""
echo "You can now eject the DMG."
read -n 1 -s -r -p "Press any key to exit..."
