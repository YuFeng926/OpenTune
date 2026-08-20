#!/bin/bash
# ==============================================================================
# OpenTune - macOS 打包脚本
# 功能：构建 + ad-hoc 签名 + 生成 DMG 安装包
# 用法：
#   ./scripts/package-macos.sh                # Release 构建 + 打包
#   ./scripts/package-macos.sh --skip-build   # 跳过构建，直接打包已有产物
#   ./scripts/package-macos.sh --clean        # 清空构建目录后重新构建
# ==============================================================================
set -euo pipefail

# ── 配置 ──────────────────────────────────────────────────────────────────────
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ara-ninja"
ARTIFACTS="${BUILD_DIR}/OpenTune_artefacts/Release"
STAGING="${ROOT_DIR}/dist/staging"
DMG_DIR="${ROOT_DIR}/dist"
APP_NAME="OpenTune"
VERSION=$(sed -n 's/.*project(OpenTune VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt")
DMG_NAME="${APP_NAME}-${VERSION}-macOS-universal2"
SIGN_IDENTITY="-"

# ── 参数解析 ──────────────────────────────────────────────────────────────────
SKIP_BUILD=false
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --clean)      CLEAN=true ;;
        -h|--help)
            echo "用法: $0 [--skip-build] [--clean]"
            echo "  --skip-build  跳过构建，直接打包已有产物"
            echo "  --clean       清空构建目录后重新构建"
            exit 0
            ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

# ── 工具检查 ──────────────────────────────────────────────────────────────────
for cmd in cmake ninja hdiutil codesign xattr; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "❌ 缺少工具: $cmd"
        exit 1
    fi
done

# ── 构建 ──────────────────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = false ]; then
    echo "▶ 构建 OpenTune ${VERSION} (Release, Ninja)"

    if [ "$CLEAN" = true ] && [ -d "${BUILD_DIR}" ]; then
        echo "  清空构建目录..."
        rm -rf "${BUILD_DIR}"
    fi

    cmake --preset macos-ara-ninja --warn-uninitialized 2>&1 | tail -5
    cmake --build "${BUILD_DIR}" --parallel 2>&1 | tail -10

    echo "✅ 构建完成"
fi

# ── 验证产物 ──────────────────────────────────────────────────────────────────
APP_BUNDLE="${ARTIFACTS}/Standalone/${APP_NAME}.app"
VST3_BUNDLE="${ARTIFACTS}/VST3/${APP_NAME}.vst3"

if [ ! -d "${APP_BUNDLE}" ]; then
    echo "❌ Standalone 产物不存在: ${APP_BUNDLE}"
    exit 1
fi
if [ ! -d "${VST3_BUNDLE}" ]; then
    echo "❌ VST3 产物不存在: ${VST3_BUNDLE}"
    exit 1
fi

echo "▶ 产物验证"
echo "  Standalone.app: $(du -sh "${APP_BUNDLE}" | cut -f1)"
echo "  VST3.vst3:      $(du -sh "${VST3_BUNDLE}" | cut -f1)"

# ── 清理 staging ─────────────────────────────────────────────────────────────
echo "▶ 准备安装包 staging"
rm -rf "${STAGING}"
mkdir -p "${STAGING}"

# ── 复制 Standalone.app（保留完整 bundle，含模型）──────────────────────────────
cp -R "${APP_BUNDLE}" "${STAGING}/${APP_NAME}.app"

# ── 复制 VST3，剥离重复模型 ──────────────────────────────────────────────────
# VST3 运行时通过 ModelPathResolver 查找 /Applications/OpenTune/models，
# 无需自带模型副本，节省约 570MB。
cp -R "${VST3_BUNDLE}" "${STAGING}/${APP_NAME}.vst3"

# 删除 VST3 bundle 中与 Standalone 重复的模型（节省 ~570MB）
VST3_RESOURCES="${STAGING}/${APP_NAME}.vst3/Contents/Resources"
if [ -d "${VST3_RESOURCES}/models" ]; then
    MODELS_SIZE=$(du -sh "${VST3_RESOURCES}/models" | cut -f1)
    rm -rf "${VST3_RESOURCES}/models"
    echo "  VST3 剥离重复模型: -${MODELS_SIZE}"
fi

# ── 生成安装脚本 ─────────────────────────────────────────────────────────────
INSTALL_SCRIPT="${STAGING}/安装 OpenTune.command"
cat > "${INSTALL_SCRIPT}" << 'INSTALLER_EOF'
#!/bin/bash
# ==============================================================================
# OpenTune macOS 安装器
# 双击运行即可完成安装（Standalone + VST3 插件）
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="OpenTune"
APP_SRC="${SCRIPT_DIR}/${APP_NAME}.app"
VST3_SRC="${SCRIPT_DIR}/${APP_NAME}.vst3"
APP_DEST="/Applications/${APP_NAME}.app"
VST3_DEST="${HOME}/Library/Audio/Plug-Ins/VST3/${APP_NAME}.vst3"

echo "╔══════════════════════════════════════════╗"
echo "║     OpenTune macOS Installer             ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── 关闭运行中的 OpenTune ────────────────────────────────────────────────────
if pgrep -x "OpenTune" >/dev/null 2>&1; then
    echo "⚠  OpenTune 正在运行，正在关闭..."
    osascript -e 'quit app "OpenTune"' 2>/dev/null || pkill -x "OpenTune"
    sleep 2
fi

# ── 安装 Standalone.app ──────────────────────────────────────────────────────
echo "▶ 安装 Standalone 到 /Applications"
if [ -d "${APP_DEST}" ]; then
    rm -rf "${APP_DEST}"
fi
cp -R "${APP_SRC}" "${APP_DEST}"
echo "  ✓ ${APP_DEST}"

# ── 安装 VST3 插件 ──────────────────────────────────────────────────────────
echo "▶ 安装 VST3 插件"
mkdir -p "$(dirname "${VST3_DEST}")"
if [ -d "${VST3_DEST}" ]; then
    rm -rf "${VST3_DEST}"
fi
cp -R "${VST3_SRC}" "${VST3_DEST}"
echo "  ✓ ${VST3_DEST}"

# ── 模型放置（VST3 运行时查找路径）────────────────────────────────────────────
# ModelPathResolver 优先级 1: /Applications/OpenTune/models
# Standalone.app 内已有模型，此处确保 /Applications/OpenTune/models 也存在
# 供 VST3 插件在 DAW 中加载时使用
MODELS_SRC="${APP_DEST}/Contents/Resources/models"
MODELS_DEST="/Applications/${APP_NAME}/models"
if [ -d "${MODELS_SRC}" ]; then
    mkdir -p "${MODELS_DEST}"
    # 仅在目标不存在或为空时复制，避免重复拷贝大文件
    if [ ! -f "${MODELS_DEST}/rmvpe.onnx" ]; then
        echo "▶ 部署共享模型到 /Applications/OpenTune/models（供 VST3 使用）"
        cp -R "${MODELS_SRC}/." "${MODELS_DEST}/"
        echo "  ✓ 模型部署完成"
    else
        echo "  ✓ 共享模型已存在，跳过"
    fi
fi

# ── Ad-hoc 签名 ──────────────────────────────────────────────────────────────
echo "▶ Ad-hoc 签名（绕过 Gatekeeper）"
codesign --force --deep --sign - "${APP_DEST}" 2>/dev/null
echo "  ✓ ${APP_NAME}.app 已签名"
codesign --force --deep --sign - "${VST3_DEST}" 2>/dev/null
echo "  ✓ ${APP_NAME}.vst3 已签名"

# ── 解除隔离属性 ──────────────────────────────────────────────────────────────
echo "▶ 解除 macOS 隔离验证"
xattr -cr "${APP_DEST}" 2>/dev/null
xattr -cr "${VST3_DEST}" 2>/dev/null
echo "  ✓ 隔离属性已清除"

# ── 完成 ──────────────────────────────────────────────────────────────────────
echo ""
echo "✅ 安装完成！"
echo ""
echo "  Standalone:  ${APP_DEST}"
echo "  VST3 插件:   ${VST3_DEST}"
echo ""
echo "  如首次打开遇到「无法验证开发者」提示："
echo "  系统设置 → 隐私与安全性 → 仍要打开"
echo ""
echo "  按 Enter 清理安装包，或关闭窗口保留..."
read -r -t 10 _ 2>/dev/null || true
rm -rf "${SCRIPT_DIR}"
INSTALLER_EOF
chmod +x "${INSTALL_SCRIPT}"

# ── 也生成英文版安装脚本 ──────────────────────────────────────────────────────
INSTALL_SCRIPT_EN="${STAGING}/Install OpenTune.command"
cat > "${INSTALL_SCRIPT_EN}" << 'INSTALLER_EN_EOF'
#!/bin/bash
# ==============================================================================
# OpenTune macOS Installer
# Double-click to install (Standalone + VST3 plugin)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="OpenTune"
APP_SRC="${SCRIPT_DIR}/${APP_NAME}.app"
VST3_SRC="${SCRIPT_DIR}/${APP_NAME}.vst3"
APP_DEST="/Applications/${APP_NAME}.app"
VST3_DEST="${HOME}/Library/Audio/Plug-Ins/VST3/${APP_NAME}.vst3"

echo "╔══════════════════════════════════════════╗"
echo "║     OpenTune macOS Installer             ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── Close running OpenTune ────────────────────────────────────────────────────
if pgrep -x "OpenTune" >/dev/null 2>&1; then
    echo "⚠  OpenTune is running, closing..."
    osascript -e 'quit app "OpenTune"' 2>/dev/null || pkill -x "OpenTune"
    sleep 2
fi

# ── Install Standalone.app ────────────────────────────────────────────────────
echo "▶ Installing Standalone to /Applications"
if [ -d "${APP_DEST}" ]; then
    rm -rf "${APP_DEST}"
fi
cp -R "${APP_SRC}" "${APP_DEST}"
echo "  ✓ ${APP_DEST}"

# ── Install VST3 plugin ──────────────────────────────────────────────────────
echo "▶ Installing VST3 plugin"
mkdir -p "$(dirname "${VST3_DEST}")"
if [ -d "${VST3_DEST}" ]; then
    rm -rf "${VST3_DEST}"
fi
cp -R "${VST3_SRC}" "${VST3_DEST}"
echo "  ✓ ${VST3_DEST}"

# ── Deploy shared models (VST3 runtime lookup path) ──────────────────────────
MODELS_SRC="${APP_DEST}/Contents/Resources/models"
MODELS_DEST="/Applications/${APP_NAME}/models"
if [ -d "${MODELS_SRC}" ]; then
    mkdir -p "${MODELS_DEST}"
    if [ ! -f "${MODELS_DEST}/rmvpe.onnx" ]; then
        echo "▶ Deploying shared models to /Applications/OpenTune/models (for VST3)"
        cp -R "${MODELS_SRC}/." "${MODELS_DEST}/"
        echo "  ✓ Models deployed"
    else
        echo "  ✓ Shared models already present, skipping"
    fi
fi

# ── Ad-hoc code signing ──────────────────────────────────────────────────────
echo "▶ Ad-hoc signing (bypass Gatekeeper)"
codesign --force --deep --sign - "${APP_DEST}" 2>/dev/null
echo "  ✓ ${APP_NAME}.app signed"
codesign --force --deep --sign - "${VST3_DEST}" 2>/dev/null
echo "  ✓ ${APP_NAME}.vst3 signed"

# ── Strip quarantine ─────────────────────────────────────────────────────────
echo "▶ Removing macOS quarantine attributes"
xattr -cr "${APP_DEST}" 2>/dev/null
xattr -cr "${VST3_DEST}" 2>/dev/null
echo "  ✓ Quarantine cleared"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "✅ Installation complete!"
echo ""
echo "  Standalone:  ${APP_DEST}"
echo "  VST3 Plugin: ${VST3_DEST}"
echo ""
echo "  If you see '无法验证开发者' on first launch:"
echo "  System Settings → Privacy & Security → Open Anyway"
echo ""
echo "  Press Enter to clean up installer, or close this window to keep it..."
read -r -t 10 _ 2>/dev/null || true
rm -rf "${SCRIPT_DIR}"
INSTALLER_EN_EOF
chmod +x "${INSTALL_SCRIPT_EN}"

# ── 复制许可证文件 ────────────────────────────────────────────────────────────
cp "${ROOT_DIR}/LICENSE" "${STAGING}/" 2>/dev/null || true
cp "${ROOT_DIR}/NOTICE.txt" "${STAGING}/" 2>/dev/null || true
cp "${ROOT_DIR}/NOTICE.zh-CN.txt" "${STAGING}/" 2>/dev/null || true
cp "${ROOT_DIR}/STATEMENTS.txt" "${STAGING}/" 2>/dev/null || true

# ── Ad-hoc 签名 staging 中的所有 bundle ──────────────────────────────────────
echo "▶ Ad-hoc 签名所有 bundle"
codesign --force --deep --sign - "${STAGING}/${APP_NAME}.app" 2>/dev/null
echo "  ✓ ${APP_NAME}.app"
codesign --force --deep --sign - "${STAGING}/${APP_NAME}.vst3" 2>/dev/null
echo "  ✓ ${APP_NAME}.vst3"

# ── 统计 staging 大小 ─────────────────────────────────────────────────────────
STAGING_SIZE=$(du -sh "${STAGING}" | cut -f1)
echo ""
echo "▶ Staging 总大小: ${STAGING_SIZE}"

# ── 生成 DMG ─────────────────────────────────────────────────────────────────
echo "▶ 生成 DMG 安装包"

DMG_PATH="${DMG_DIR}/${DMG_NAME}.dmg"
# 如果已有同名 DMG，先删除
rm -f "${DMG_PATH}"

# 直接从 staging 创建压缩只读 DMG（跳过 AppleScript 美化，终端环境不稳定）
hdiutil create \
    -srcfolder "${STAGING}" \
    -volname "${APP_NAME} ${VERSION}" \
    -fs HFS+ \
    -fsargs "-c c=64,a=16,e=16" \
    -format UDZO \
    -imagekey zlib-level=9 \
    "${DMG_PATH}" \
    2>/dev/null

# ── 最终结果 ──────────────────────────────────────────────────────────────────
DMG_SIZE=$(du -sh "${DMG_PATH}" | cut -f1)
echo ""
echo "══════════════════════════════════════════════"
echo "  ✅ DMG 安装包生成完成"
echo "══════════════════════════════════════════════"
echo "  文件: ${DMG_PATH}"
echo "  大小: ${DMG_SIZE}"
echo "  版本: ${VERSION}"
echo ""
echo "  安装方式：双击 DMG → 双击「安装 OpenTune.command」"
echo "  或直接拖拽 .app 到 /Applications，.vst3 到 VST3 目录"
echo "══════════════════════════════════════════════"
