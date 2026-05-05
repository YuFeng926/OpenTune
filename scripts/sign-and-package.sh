#!/bin/bash
# ==============================================================================
# OpenTune macOS Sign & Package Script
#
# Signs the app bundle with Hardened Runtime, creates a DMG with Applications
# symlink, optionally notarizes with Apple, and staples the ticket.
#
# Usage:
#   ./scripts/sign-and-package.sh "<Developer ID Application: ...>" [--skip-notarize]
#
# Environment variables (required for notarization):
#   APPLE_ID          - Apple ID email
#   APPLE_TEAM_ID     - Team identifier
#   APPLE_APP_PASSWORD - App-specific password
#
# Prerequisites:
#   - Xcode command line tools installed
#   - Valid Developer ID certificate in keychain
#   - Release build completed: cmake --build build-arm64 --config Release
# ==============================================================================

set -euo pipefail

# ==============================================================================
# Configuration
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_BUNDLE="$PROJECT_ROOT/build-arm64/OpenTune_artefacts/Release/Standalone/OpenTune.app"
VST3_BUNDLE="$PROJECT_ROOT/build-arm64/OpenTune_artefacts/Release/VST3/OpenTune.vst3"
ENTITLEMENTS="$PROJECT_ROOT/Resources/macOS/OpenTune.entitlements"
DYLIB_NAME="libonnxruntime.1.24.4.dylib"

# Extract version from CMakeLists.txt
VERSION=$(grep 'VERSION "' "$PROJECT_ROOT/CMakeLists.txt" | head -1 | sed 's/.*VERSION "\([^"]*\)".*/\1/')
if [ -z "$VERSION" ]; then
    VERSION="1.0.0"
fi

DMG_NAME="OpenTune-${VERSION}-arm64.dmg"
DMG_OUTPUT="$PROJECT_ROOT/$DMG_NAME"

# ==============================================================================
# Argument parsing
# ==============================================================================

DEVELOPER_ID=""
SKIP_NOTARIZE=false

for arg in "$@"; do
    case "$arg" in
        --skip-notarize)
            SKIP_NOTARIZE=true
            ;;
        *)
            if [ -z "$DEVELOPER_ID" ]; then
                DEVELOPER_ID="$arg"
            fi
            ;;
    esac
done

if [ -z "$DEVELOPER_ID" ]; then
    echo "Error: Developer ID is required."
    echo "Usage: $0 \"Developer ID Application: <Name> (<TeamID>)\" [--skip-notarize]"
    exit 1
fi

# ==============================================================================
# Preflight checks
# ==============================================================================

echo "========================================"
echo "OpenTune Sign & Package"
echo "========================================"
echo "Version:      $VERSION"
echo "Developer ID: $DEVELOPER_ID"
echo "Notarize:     $([ "$SKIP_NOTARIZE" = true ] && echo "SKIP" || echo "YES")"
echo "App Bundle:   $APP_BUNDLE"
echo "VST3 Bundle:  $VST3_BUNDLE"
echo "DMG Output:   $DMG_OUTPUT"
echo "========================================"
echo ""

# Verify app bundle exists
if [ ! -d "$APP_BUNDLE" ]; then
    echo "Error: App bundle not found at $APP_BUNDLE"
    echo "Run 'cmake --build build-arm64 --config Release' first."
    exit 1
fi

# Verify entitlements file
if [ ! -f "$ENTITLEMENTS" ]; then
    echo "Error: Entitlements file not found at $ENTITLEMENTS"
    exit 1
fi

# Detect VST3 bundle (optional — packaged when present)
INCLUDE_VST3=false
VST3_DYLIB_PATH=""
VST3_BINARY_PATH=""
if [ -d "$VST3_BUNDLE" ]; then
    INCLUDE_VST3=true
    VST3_DYLIB_PATH="$VST3_BUNDLE/Contents/Frameworks/$DYLIB_NAME"
    VST3_BINARY_PATH="$VST3_BUNDLE/Contents/MacOS/OpenTune"
fi

# Verify bundle structure
echo "[1/8] Verifying bundle structure..."
DYLIB_PATH="$APP_BUNDLE/Contents/Frameworks/$DYLIB_NAME"
BINARY_PATH="$APP_BUNDLE/Contents/MacOS/OpenTune"

if [ ! -f "$DYLIB_PATH" ]; then
    echo "Error: ONNX Runtime dylib not found at $DYLIB_PATH"
    exit 1
fi

if [ ! -f "$BINARY_PATH" ]; then
    echo "Error: Main binary not found at $BINARY_PATH"
    exit 1
fi

echo "  Standalone bundle OK"

if [ "$INCLUDE_VST3" = true ]; then
    if [ ! -f "$VST3_BINARY_PATH" ]; then
        echo "Error: VST3 binary not found at $VST3_BINARY_PATH"
        exit 1
    fi
    if [ ! -f "$VST3_DYLIB_PATH" ]; then
        echo "Warning: VST3 has no embedded ONNX dylib at $VST3_DYLIB_PATH"
        echo "         (will sign binary + bundle only)"
        VST3_DYLIB_PATH=""
    fi
    echo "  VST3 bundle OK"
else
    echo "  VST3 bundle not present — building Standalone-only DMG"
    echo "  (To include VST3: cmake --build build-arm64 --config Release --target OpenTune_VST3)"
fi

# ==============================================================================
# Step 2: Fix dylib install name (ensure @rpath is correct)
# ==============================================================================

echo "[2/8] Fixing dylib install names..."

# Standalone: ensure dylib ID + main binary reference both use @rpath
install_name_tool -id "@rpath/$DYLIB_NAME" "$DYLIB_PATH" 2>/dev/null || true
if ! otool -L "$BINARY_PATH" | grep -q "@rpath/$DYLIB_NAME"; then
    echo "  Standalone: binary doesn't reference dylib via @rpath. Attempting fix..."
    CURRENT_REF=$(otool -L "$BINARY_PATH" | grep "onnxruntime" | awk '{print $1}')
    if [ -n "$CURRENT_REF" ]; then
        install_name_tool -change "$CURRENT_REF" "@rpath/$DYLIB_NAME" "$BINARY_PATH"
    fi
fi
echo "  Standalone install names OK"

# VST3: same install-name normalization (only if VST3 has its own embedded dylib)
if [ "$INCLUDE_VST3" = true ] && [ -n "$VST3_DYLIB_PATH" ]; then
    install_name_tool -id "@rpath/$DYLIB_NAME" "$VST3_DYLIB_PATH" 2>/dev/null || true
    if ! otool -L "$VST3_BINARY_PATH" | grep -q "@rpath/$DYLIB_NAME"; then
        echo "  VST3: binary doesn't reference dylib via @rpath. Attempting fix..."
        CURRENT_REF=$(otool -L "$VST3_BINARY_PATH" | grep "onnxruntime" | awk '{print $1}')
        if [ -n "$CURRENT_REF" ]; then
            install_name_tool -change "$CURRENT_REF" "@rpath/$DYLIB_NAME" "$VST3_BINARY_PATH"
        fi
    fi
    echo "  VST3 install names OK"
fi

# ==============================================================================
# Step 3: Sign inside-out with Hardened Runtime
# ==============================================================================

echo "[3/8] Signing bundles (inside-out, Hardened Runtime)..."

# --- Standalone (.app) ---
echo "  [Standalone] Signing Frameworks/$DYLIB_NAME..."
codesign --force --options runtime \
    --sign "$DEVELOPER_ID" \
    --entitlements "$ENTITLEMENTS" \
    --timestamp \
    "$DYLIB_PATH"

echo "  [Standalone] Signing MacOS/OpenTune..."
codesign --force --options runtime \
    --sign "$DEVELOPER_ID" \
    --entitlements "$ENTITLEMENTS" \
    --timestamp \
    "$BINARY_PATH"

echo "  [Standalone] Signing OpenTune.app..."
codesign --force --options runtime \
    --sign "$DEVELOPER_ID" \
    --entitlements "$ENTITLEMENTS" \
    --timestamp \
    "$APP_BUNDLE"

# --- VST3 (.vst3 bundle) ---
if [ "$INCLUDE_VST3" = true ]; then
    if [ -n "$VST3_DYLIB_PATH" ]; then
        echo "  [VST3] Signing Frameworks/$DYLIB_NAME..."
        codesign --force --options runtime \
            --sign "$DEVELOPER_ID" \
            --entitlements "$ENTITLEMENTS" \
            --timestamp \
            "$VST3_DYLIB_PATH"
    fi

    echo "  [VST3] Signing MacOS/OpenTune..."
    codesign --force --options runtime \
        --sign "$DEVELOPER_ID" \
        --entitlements "$ENTITLEMENTS" \
        --timestamp \
        "$VST3_BINARY_PATH"

    echo "  [VST3] Signing OpenTune.vst3..."
    codesign --force --options runtime \
        --sign "$DEVELOPER_ID" \
        --entitlements "$ENTITLEMENTS" \
        --timestamp \
        "$VST3_BUNDLE"
fi

echo "  Signing complete"

# ==============================================================================
# Step 4: Verify signature
# ==============================================================================

echo "[4/8] Verifying code signatures..."

verify_signed_bundle() {
    local label="$1"
    local bundle="$2"

    if codesign --verify --strict --verbose=2 "$bundle" 2>&1; then
        echo "  [$label] Signature verification PASSED"
    else
        echo "Error: [$label] Signature verification FAILED"
        exit 1
    fi

    if codesign -d --verbose=4 "$bundle" 2>&1 | grep -q "flags=.*runtime"; then
        echo "  [$label] Hardened Runtime confirmed"
    else
        echo "Error: [$label] Hardened Runtime flag not set"
        exit 1
    fi

    if codesign -d --entitlements - "$bundle" 2>&1 | grep -q "disable-library-validation"; then
        echo "  [$label] Entitlements embedded OK"
    else
        echo "Error: [$label] Entitlements not found in signature"
        exit 1
    fi
}

verify_signed_bundle "Standalone" "$APP_BUNDLE"

if [ "$INCLUDE_VST3" = true ]; then
    verify_signed_bundle "VST3" "$VST3_BUNDLE"
fi

# ==============================================================================
# Step 5: Create DMG
# ==============================================================================

echo "[5/8] Creating DMG..."

# Clean up any existing DMG
rm -f "$DMG_OUTPUT"
TEMP_DMG="$PROJECT_ROOT/temp_opentune.dmg"
MOUNT_POINT="/Volumes/OpenTune"
rm -f "$TEMP_DMG"

# Unmount if already mounted
if [ -d "$MOUNT_POINT" ]; then
    hdiutil detach "$MOUNT_POINT" -force 2>/dev/null || true
fi

# Calculate required size (app + vst3 + 50MB headroom)
APP_SIZE_KB=$(du -sk "$APP_BUNDLE" | awk '{print $1}')
VST3_SIZE_KB=0
if [ "$INCLUDE_VST3" = true ]; then
    VST3_SIZE_KB=$(du -sk "$VST3_BUNDLE" | awk '{print $1}')
fi
DMG_SIZE_KB=$((APP_SIZE_KB + VST3_SIZE_KB + 51200))

# Create temporary writable DMG
hdiutil create -size "${DMG_SIZE_KB}k" \
    -fs HFS+ \
    -volname "OpenTune" \
    -type SPARSE \
    "$TEMP_DMG"

# Mount the writable DMG
hdiutil attach "${TEMP_DMG}.sparseimage" -mountpoint "$MOUNT_POINT" -nobrowse

# Copy app bundle
cp -R "$APP_BUNDLE" "$MOUNT_POINT/"

# Copy VST3 bundle (when present)
if [ "$INCLUDE_VST3" = true ]; then
    cp -R "$VST3_BUNDLE" "$MOUNT_POINT/"
fi

# Copy install script (removes quarantine for unsigned builds; installs both formats)
INSTALL_SCRIPT="$SCRIPT_DIR/install.command"
if [ -f "$INSTALL_SCRIPT" ]; then
    cp "$INSTALL_SCRIPT" "$MOUNT_POINT/Install OpenTune.command"
    chmod +x "$MOUNT_POINT/Install OpenTune.command"
fi

# Create Applications symlink
ln -s /Applications "$MOUNT_POINT/Applications"

# Unmount
sync
hdiutil detach "$MOUNT_POINT"

# Convert to compressed read-only DMG (UDZO = zlib compressed)
hdiutil convert "${TEMP_DMG}.sparseimage" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$DMG_OUTPUT"

# Clean up temp file
rm -f "${TEMP_DMG}.sparseimage"

echo "  DMG created: $DMG_NAME"

# ==============================================================================
# Step 6: Sign the DMG
# ==============================================================================

echo "[6/8] Signing DMG..."

codesign --force --sign "$DEVELOPER_ID" --timestamp "$DMG_OUTPUT"

echo "  DMG signed"

# ==============================================================================
# Step 7: Notarize (optional)
# ==============================================================================

if [ "$SKIP_NOTARIZE" = true ]; then
    echo "[7/8] Notarization SKIPPED (--skip-notarize)"
else
    echo "[7/8] Submitting for notarization..."

    # Validate credentials
    if [ -z "${APPLE_ID:-}" ]; then
        echo "Error: APPLE_ID environment variable not set"
        exit 1
    fi
    if [ -z "${APPLE_TEAM_ID:-}" ]; then
        echo "Error: APPLE_TEAM_ID environment variable not set"
        exit 1
    fi
    if [ -z "${APPLE_APP_PASSWORD:-}" ]; then
        echo "Error: APPLE_APP_PASSWORD environment variable not set"
        exit 1
    fi

    # Submit for notarization and wait
    xcrun notarytool submit "$DMG_OUTPUT" \
        --apple-id "$APPLE_ID" \
        --team-id "$APPLE_TEAM_ID" \
        --password "$APPLE_APP_PASSWORD" \
        --wait

    # Staple the notarization ticket
    echo "  Stapling notarization ticket..."
    xcrun stapler staple "$DMG_OUTPUT"

    echo "  Notarization complete"
fi

# ==============================================================================
# Step 8: Final assessment
# ==============================================================================

echo "[8/8] Final verification..."

if [ "$SKIP_NOTARIZE" = false ]; then
    # Full Gatekeeper assessment (only meaningful after notarization)
    if spctl --assess --type open --context context:primary-signature "$DMG_OUTPUT" 2>&1; then
        echo "  Gatekeeper assessment PASSED"
    else
        echo "  Warning: Gatekeeper assessment did not pass (may need notarization)"
    fi
else
    echo "  Skipping Gatekeeper assessment (notarization was skipped)"
fi

# Show DMG info
echo ""
echo "========================================"
echo "Build Complete"
echo "========================================"
echo "DMG:     $DMG_OUTPUT"
echo "Size:    $(du -sh "$DMG_OUTPUT" | awk '{print $1}')"
echo "Format:  $(hdiutil imageinfo "$DMG_OUTPUT" 2>/dev/null | grep 'Format Description' | sed 's/.*: //')"
echo "========================================"
echo ""

if [ "$SKIP_NOTARIZE" = true ]; then
    echo "Note: DMG is signed but NOT notarized."
    echo "For distribution, re-run without --skip-notarize."
else
    echo "DMG is signed, notarized, and ready for distribution."
fi
