#!/bin/bash
# ==============================================================================
# OpenTune - macOS 打包脚本
# 功能：构建 + ad-hoc 签名 + 生成 DMG / PKG 安装包
# 用法：
#   ./scripts/package-macos.sh                     # 构建 Universal2 并生成 PKG
#   ./scripts/package-macos.sh --format dmg        # 生成 Universal2 DMG
#   ./scripts/package-macos.sh --format both       # 同时生成 DMG 与 PKG
#   ./scripts/package-macos.sh --skip-build        # 跳过构建，直接打包已有产物
#   ./scripts/package-macos.sh --clean             # 清空构建目录后重新构建
# ==============================================================================
set -euo pipefail

# ── 配置 ──────────────────────────────────────────────────────────────────────
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STAGING="${ROOT_DIR}/dist/staging"
DMG_DIR="${ROOT_DIR}/dist"
PKG_BUILD_DIR="${ROOT_DIR}/dist/pkg-build"
APP_NAME="OpenTune"
BUNDLE_ID="com.daya.opentune"
VERSION=$(sed -n 's/.*project(OpenTune VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt")
SIGN_IDENTITY="-"

# ── 参数解析 ──────────────────────────────────────────────────────────────────
SKIP_BUILD=false
CLEAN=false
FORMAT="pkg"
PREV_ARG=""
for arg in "$@"; do
    if [ "${PREV_ARG}" = "--format" ] && [[ "${arg}" == -* ]]; then
        echo "❌ --format 缺少值（仅支持 dmg|pkg|both）"
        exit 1
    fi

    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --clean)      CLEAN=true ;;
        --format)
            # handled via next arg
            ;;
        --format=dmg)
            FORMAT="dmg"
            ;;
        --format=pkg)
            FORMAT="pkg"
            ;;
        --format=both)
            FORMAT="both"
            ;;
        --format=*)
            echo "❌ 未知打包格式: ${arg#--format=}（仅支持 dmg|pkg|both）"
            exit 1
            ;;
        -h|--help)
            echo "用法: $0 [--format dmg|pkg|both] [--skip-build] [--clean]"
            echo "  --format FMT   指定打包格式: dmg、pkg 或 both（默认 pkg）"
            echo "  --skip-build   跳过构建，直接打包已有产物"
            echo "  --clean        清空构建目录后重新构建"
            exit 0
            ;;
        *)
            # Handle --format <value> form (space-separated)
            if [ "${PREV_ARG:-}" = "--format" ]; then
                case "$arg" in
                    dmg|pkg|both) FORMAT="$arg" ;;
                    *) echo "❌ 未知打包格式: $arg（仅支持 dmg|pkg|both）"; exit 1 ;;
                esac
                PREV_ARG=""
                continue
            fi
            echo "未知参数: $arg"; exit 1
            ;;
    esac
    PREV_ARG="$arg"
done

if [ "${PREV_ARG}" = "--format" ]; then
    echo "❌ --format 缺少值（仅支持 dmg|pkg|both）"
    exit 1
fi

# ── 唯一 macOS Release 目标：Universal2 ───────────────────────────────────────
OSX_ARCHS="arm64 x86_64"
PRESET="macos-universal2-ara-ninja"
BUILD_DIR="${ROOT_DIR}/build-macos-universal2-ninja"
DMG_ARCH_LABEL="Universal2"
PKG_HOST_ARCHS="x86_64,arm64"
PKG_MIN_OS="12.0"

STANDALONE_ARTIFACTS="${BUILD_DIR}/OpenTuneStandalone_artefacts/Release"
VST3_ARTIFACTS="${BUILD_DIR}/OpenTune_artefacts/Release"
DMG_NAME="${APP_NAME}-${VERSION}-macOS-${DMG_ARCH_LABEL}"
PKG_NAME="${APP_NAME}-${VERSION}-macOS-${DMG_ARCH_LABEL}"

# ── 工具检查 ──────────────────────────────────────────────────────────────────
for cmd in cmake ninja hdiutil codesign xattr otool lipo file; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "❌ 缺少工具: $cmd"
        exit 1
    fi
done

if [ "$FORMAT" != "dmg" ]; then
    for cmd in pkgbuild productbuild; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "❌ 缺少工具: $cmd（生成 PKG 需要 macOS 自带命令行工具）"
            exit 1
        fi
    done
    if [ ! -f "${ROOT_DIR}/Installer/NOTICE.md" ]; then
        echo "❌ 生成 PKG 需要文案文件: Installer/NOTICE.md"
        exit 1
    fi
fi

# ── 构建 ──────────────────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = false ]; then
    echo "▶ 构建 OpenTune ${VERSION} (Universal2: x86_64 + arm64, Release, Ninja)"

    if [ "$CLEAN" = true ] && [ -d "${BUILD_DIR}" ]; then
        echo "  清空构建目录..."
        rm -rf "${BUILD_DIR}"
    fi

    cmake --preset "${PRESET}" --warn-uninitialized 2>&1 | tail -5
    cmake --build "${BUILD_DIR}" --parallel 2>&1 | tail -10

    echo "✅ 构建完成"
fi

# ── 验证产物 ──────────────────────────────────────────────────────────────────
APP_BUNDLE="${STANDALONE_ARTIFACTS}/Standalone/${APP_NAME}.app"
VST3_BUNDLE="${VST3_ARTIFACTS}/VST3/${APP_NAME}.vst3"

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

validate_macho_architectures() {
    local binary="$1"
    local label="$2"
    local actual_archs

    actual_archs="$(lipo -archs "${binary}" | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
    if [ "${actual_archs}" != "${OSX_ARCHS}" ]; then
        echo "❌ ${label} 架构不是 Universal2: ${actual_archs}（期望 ${OSX_ARCHS}）"
        exit 1
    fi
}

validate_bundle_macho_architectures() {
    local bundle="$1"
    local label="$2"
    local path
    local relative_path
    local count=0

    while IFS= read -r -d '' path; do
        if [[ "$(file -b "${path}")" == *Mach-O* ]]; then
            relative_path="${path#"${bundle}"/}"
            validate_macho_architectures "${path}" "${label}: ${relative_path}"
            count=$((count + 1))
        fi
    done < <(find "${bundle}" -type f -print0)

    echo "  ✓ ${label}: ${count} Mach-O 文件均含 x86_64 + arm64"
}

validate_bundle_linkage() {
    local bundle="$1"
    local expected_rpath="$2"
    local label="$3"
    local binary="${bundle}/Contents/MacOS/${APP_NAME}"
    local rpaths
    local ort_dependency
    local ort_filename
    local ort_library
    local ort_min_os

    if [ ! -f "${binary}" ]; then
        echo "❌ ${label} 二进制不存在: ${binary}"
        exit 1
    fi

    rpaths="$(otool -l "${binary}" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" {
            getline
            getline
            print $2
        }
    ')"

    if ! printf '%s\n' "${rpaths}" | awk -v expected="${expected_rpath}" '
        $0 == expected { found = 1 }
        END { exit found ? 0 : 1 }
    '; then
        echo "❌ ${label} 缺少 LC_RPATH: ${expected_rpath}"
        printf '  当前 RPATH:\n%s\n' "${rpaths}"
        exit 1
    fi

    if printf '%s\n' "${rpaths}" | awk '
        substr($0, 1, 1) == "/" { found = 1 }
        END { exit found ? 0 : 1 }
    '; then
        echo "❌ ${label} 包含构建机绝对 LC_RPATH"
        printf '  当前 RPATH:\n%s\n' "${rpaths}"
        exit 1
    fi

    ort_dependency="$(otool -L "${binary}" | awk '
        $1 ~ /^@rpath\/libonnxruntime\.[0-9].*\.dylib$/ {
            print $1
            exit
        }
    ')"
    if [ -z "${ort_dependency}" ]; then
        echo "❌ ${label} 未通过 @rpath 链接版本化 ONNX Runtime"
        exit 1
    fi

    ort_filename="${ort_dependency#@rpath/}"
    ort_library="${bundle}/Contents/Frameworks/${ort_filename}"
    if [ ! -f "${ort_library}" ]; then
        echo "❌ ${label} 缺少嵌入式运行库: ${ort_library}"
        exit 1
    fi

    ort_min_os="$(otool -l "${ort_library}" | awk '$1 == "minos" { print $2 }' | sort -u)"
    if [ -z "${ort_min_os}" ] || printf '%s\n' "${ort_min_os}" | grep -vFxq "${PKG_MIN_OS}"; then
        echo "❌ ${label} ORT 最低系统版本不符合 macOS ${PKG_MIN_OS}: ${ort_min_os}"
        exit 1
    fi

    echo "  ✓ ${label}: ${expected_rpath}, ${ort_filename}, macOS ${PKG_MIN_OS}+"
}

validate_bundle_linkage "${APP_BUNDLE}" "@executable_path/../Frameworks" "Standalone"
validate_bundle_linkage "${VST3_BUNDLE}" "@loader_path/../Frameworks" "VST3"

# ── PKG：从 Installer/NOTICE.md 提取首屏文案 ──────────────────────────────────
NOTICE_SOURCE="${ROOT_DIR}/Installer/NOTICE.md"

extract_notice_paragraph() {
    # $1 = 语言段标记（NOTICE.md 中 "### " 行包含的文本）
    # $2 = 段落序号（1 = 标题，2 = 防诈声明，3 = 许可/版权警示）
    awk -v marker="$1" -v want="$2" '
        function flush() {
            if (collecting && count == want && !done) { print text; done = 1 }
            text = ""
            collecting = 0
        }
        /^## 完整版/ { in_full = 1; next }
        /^## / {
            if (in_full && in_lang) flush()
            in_full = 0
            in_lang = 0
        }
        in_full && /^### / {
            if (in_lang) flush()
            in_lang = index($0, marker) ? 1 : 0
            next
        }
        in_full && in_lang {
            if (length($0) > 0) {
                if (!collecting) { collecting = 1; count++ }
                text = (length(text) > 0) ? text " " $0 : $0
            } else {
                flush()
            }
        }
        END { if (in_full && in_lang) flush() }
    ' "${NOTICE_SOURCE}"
}

escape_html() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

render_welcome_html() {
    # $1 = 输出文件, $2 = 语言段标记
    local out="$1"
    local marker="$2"
    local title fraud license
    title="$(extract_notice_paragraph "${marker}" 1)"
    fraud="$(extract_notice_paragraph "${marker}" 2)"
    license="$(extract_notice_paragraph "${marker}" 3)"
    title="${title#\*\*}"
    title="${title%\*\*}"

    if [ -z "${title}" ] || [ -z "${fraud}" ] || [ -z "${license}" ]; then
        echo "❌ 无法从 Installer/NOTICE.md 提取「${marker}」完整版文案"
        exit 1
    fi

    cat > "${out}" << WELCOME_EOF
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
    body { font-family: -apple-system, "PingFang SC", "Helvetica Neue", sans-serif; font-size: 13px; color: #000000; background: #ffffff; margin: 0 4px; }
    h2 { color: #000000; font-size: 16px; font-weight: 600; margin: 0 0 12px 0; }
    .notice { color: #000000; margin: 0 0 12px 0; }
    .notice p { color: #000000; margin: 0; line-height: 1.55; }
</style>
</head>
<body>
<h2>$(escape_html "${title}")</h2>
<div class="notice fraud"><p>$(escape_html "${fraud}")</p></div>
<div class="notice license"><p>$(escape_html "${license}")</p></div>
</body>
</html>
WELCOME_EOF
}

# ── PKG：组装产品包 ───────────────────────────────────────────────────────────
build_pkg() {
    echo "▶ 准备 PKG staging"

    local payload_dir="${PKG_BUILD_DIR}/payload"
    local packages_dir="${PKG_BUILD_DIR}/packages"
    local resources_dir="${PKG_BUILD_DIR}/resources"
    local scripts_dir="${PKG_BUILD_DIR}/scripts"

    rm -rf "${PKG_BUILD_DIR}"
    mkdir -p "${packages_dir}" "${resources_dir}" "${scripts_dir}"
    mkdir -p "${payload_dir}/Applications/${APP_NAME}"
    mkdir -p "${payload_dir}/Library/Audio/Plug-Ins/VST3"

    echo "  复制 Standalone.app（含模型）"
    cp -R "${STAGING}/${APP_NAME}.app" "${payload_dir}/Applications/${APP_NAME}.app"

    echo "  复制 VST3（无模型副本）"
    cp -R "${STAGING}/${APP_NAME}.vst3" "${payload_dir}/Library/Audio/Plug-Ins/VST3/${APP_NAME}.vst3"

    # 模型只保留 app bundle 内一份：VST3 通过 ModelPathResolver 的
    # /Applications/OpenTune.app/Contents/Resources/models 回退路径直接读取。
    local models_src="${payload_dir}/Applications/${APP_NAME}.app/Contents/Resources/models"
    if [ ! -d "${models_src}" ]; then
        echo "❌ 未找到模型目录: ${models_src}"
        exit 1
    fi

    echo "  复制许可证与声明文件"
    for doc in LICENSE NOTICE.txt NOTICE.zh-CN.txt STATEMENTS.txt; do
        cp "${ROOT_DIR}/${doc}" "${payload_dir}/Applications/${APP_NAME}/" 2>/dev/null || true
    done
    cp "${NOTICE_SOURCE}" "${payload_dir}/Applications/${APP_NAME}/NOTICE.md" 2>/dev/null || true

    # 安装前清理：移除旧 DMG 安装脚本写入当前用户目录的 VST3 副本，
    # 避免与本次系统级 VST3 在 DAW 中出现重复条目。
    cat > "${scripts_dir}/preinstall" << 'PREINSTALL_EOF'
#!/bin/bash
set -u
console_user="$(stat -f %Su /dev/console 2>/dev/null || true)"
if [ -n "${console_user}" ] && [ "${console_user}" != "root" ] && [ "${console_user}" != "loginwindow" ]; then
    user_vst3="/Users/${console_user}/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
    if [ -d "${user_vst3}" ]; then
        rm -rf "${user_vst3}"
    fi
fi
exit 0
PREINSTALL_EOF
    chmod +x "${scripts_dir}/preinstall"

    echo "  渲染欢迎页文案（Installer/NOTICE.md 完整版）"
    # 根文件是 Installer 未命中本地化资源时的回退内容。项目主要面向中文用户，
    # 因此使用中文作为回退；英文放入 en.lproj，避免英文系统显示中文。
    render_welcome_html "${resources_dir}/welcome.html" "中文（zh）"
    # 同时提供系统首选语言的精确 BCP-47 目录和通用简体中文目录。
    # zh_CN.lproj 会被 productbuild 重写为 zh-Hans.lproj，因此直接使用
    # zh-Hans-CN.lproj 可避免 Installer 在语言匹配时依赖旧式别名转换。
    mkdir -p "${resources_dir}/en.lproj" "${resources_dir}/zh-Hans.lproj" \
             "${resources_dir}/zh-Hans-CN.lproj" "${resources_dir}/ja.lproj"
    mkdir -p "${resources_dir}/ru.lproj" "${resources_dir}/es.lproj"
    render_welcome_html "${resources_dir}/en.lproj/welcome.html" "English (en)"
    render_welcome_html "${resources_dir}/zh-Hans.lproj/welcome.html" "中文（zh）"
    render_welcome_html "${resources_dir}/zh-Hans-CN.lproj/welcome.html" "中文（zh）"
    render_welcome_html "${resources_dir}/ja.lproj/welcome.html" "日本語（ja）"
    render_welcome_html "${resources_dir}/ru.lproj/welcome.html" "Русский（ru）"
    render_welcome_html "${resources_dir}/es.lproj/welcome.html" "Español (es)"

    cat > "${PKG_BUILD_DIR}/Distribution.xml" << DISTRIBUTION_EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>${APP_NAME} ${VERSION} Universal2</title>
    <organization>com.daya</organization>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <options customize="never" require-scripts="true" hostArchitectures="${PKG_HOST_ARCHS}"/>
    <allowed-os-versions>
        <os-version min="${PKG_MIN_OS}"/>
    </allowed-os-versions>
    <welcome file="welcome.html"/>
    <choices-outline>
        <line choice="${BUNDLE_ID}.pkg"/>
    </choices-outline>
    <choice id="${BUNDLE_ID}.pkg" visible="false">
        <pkg-ref id="${BUNDLE_ID}.pkg"/>
    </choice>
    <pkg-ref id="${BUNDLE_ID}.pkg" version="${VERSION}" onConclusion="none">${APP_NAME}.pkg</pkg-ref>
</installer-gui-script>
DISTRIBUTION_EOF

    # 关闭 bundle 重定位：pkgbuild 默认把 bundle 标记为 relocatable，安装时
    # Installer 若在别处（如开发构建目录）发现相同 CFBundleIdentifier 的副本，
    # 会把内容“升级”到那份副本上，导致 /Applications 下没有主程序。
    cat > "${PKG_BUILD_DIR}/components.plist" << COMPONENT_PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
    <dict>
        <key>RootRelativeBundlePath</key>
        <string>Applications/${APP_NAME}.app</string>
        <key>BundleIsRelocatable</key>
        <false/>
        <key>BundleIsVersionChecked</key>
        <false/>
        <key>BundleHasStrictIdentifier</key>
        <true/>
        <key>BundleOverwriteAction</key>
        <string>upgrade</string>
    </dict>
    <dict>
        <key>RootRelativeBundlePath</key>
        <string>Library/Audio/Plug-Ins/VST3/${APP_NAME}.vst3</string>
        <key>BundleIsRelocatable</key>
        <false/>
        <key>BundleIsVersionChecked</key>
        <false/>
        <key>BundleHasStrictIdentifier</key>
        <true/>
        <key>BundleOverwriteAction</key>
        <string>upgrade</string>
    </dict>
</array>
</plist>
COMPONENT_PLIST_EOF

    echo "  构建组件包"
    pkgbuild --root "${payload_dir}" \
             --identifier "${BUNDLE_ID}.pkg" \
             --version "${VERSION}" \
             --install-location "/" \
             --scripts "${scripts_dir}" \
             --component-plist "${PKG_BUILD_DIR}/components.plist" \
             "${packages_dir}/${APP_NAME}.pkg"

    # 回归保护：确认组件包不再携带可重定位 bundle 标记。
    local verify_dir relocate_marked
    verify_dir="$(mktemp -d)"
    rmdir "${verify_dir}"
    pkgutil --expand "${packages_dir}/${APP_NAME}.pkg" "${verify_dir}" >/dev/null
    relocate_marked="$(awk '
        /<relocate>/ { in_relocate = 1; next }
        /<\/relocate>/ { in_relocate = 0 }
        in_relocate && /<bundle/ { print "yes"; exit }
    ' "${verify_dir}/PackageInfo")"
    if [ -n "${relocate_marked}" ]; then
        rm -rf "${verify_dir}"
        echo "❌ 组件包仍包含可重定位 bundle，安装后主程序可能不会出现在 /Applications"
        exit 1
    fi
    rm -rf "${verify_dir}"

    echo "  组装产品包"
    if [ -n "${PKG_SIGN_IDENTITY:-}" ]; then
        productbuild --distribution "${PKG_BUILD_DIR}/Distribution.xml" \
                     --resources "${resources_dir}" \
                     --package-path "${packages_dir}" \
                     --sign "${PKG_SIGN_IDENTITY}" \
                     "${DMG_DIR}/${PKG_NAME}.pkg"
    else
        productbuild --distribution "${PKG_BUILD_DIR}/Distribution.xml" \
                     --resources "${resources_dir}" \
                     --package-path "${packages_dir}" \
                     "${DMG_DIR}/${PKG_NAME}.pkg"
    fi
}

# ── 清理 staging ─────────────────────────────────────────────────────────────
echo "▶ 准备安装包 staging"
rm -rf "${STAGING}"
mkdir -p "${STAGING}"

# ── 复制 Standalone.app（保留完整 bundle，含模型）──────────────────────────────
cp -R "${APP_BUNDLE}" "${STAGING}/${APP_NAME}.app"

# ── 复制 VST3，剥离重复模型 ──────────────────────────────────────────────────
# VST3 运行时通过 ModelPathResolver 回退到 Standalone.app 内的模型目录，
# 无需自带模型副本。
cp -R "${VST3_BUNDLE}" "${STAGING}/${APP_NAME}.vst3"

# 删除 VST3 bundle 中与 Standalone 重复的模型（节省 ~570MB）
VST3_RESOURCES="${STAGING}/${APP_NAME}.vst3/Contents/Resources"
if [ -d "${VST3_RESOURCES}/models" ]; then
    MODELS_SIZE=$(du -sh "${VST3_RESOURCES}/models" | cut -f1)
    rm -rf "${VST3_RESOURCES}/models"
    echo "  VST3 剥离重复模型: -${MODELS_SIZE}"
fi

# ── 清理 bundle 中未引用的旧版 ONNX Runtime 动态库 ────────────────────────────
# 构建目录切换 ORT 版本后，旧版 libonnxruntime.<old>.dylib 可能残留在
# Contents/Frameworks 中。这里以二进制实际链接（otool -L）的版本为准，
# 删除其余同名库，避免把旧运行库打进安装包。
prune_stale_ort_dylibs() {
    local bundle="$1"
    local label="$2"
    local binary="${bundle}/Contents/MacOS/${APP_NAME}"
    local frameworks="${bundle}/Contents/Frameworks"
    local linked keep lib name size

    [ -d "${frameworks}" ] || return 0
    [ -f "${binary}" ] || return 0

    linked="$(otool -L "${binary}" | awk '
        $1 ~ /^@rpath\/libonnxruntime\.[0-9].*\.dylib$/ { print $1; exit }
    ')"
    [ -n "${linked}" ] || return 0
    keep="${linked#@rpath/}"

    for lib in "${frameworks}"/libonnxruntime.*.dylib; do
        [ -e "${lib}" ] || continue
        name="$(basename "${lib}")"
        if [ "${name}" != "${keep}" ]; then
            size="$(du -sh "${lib}" | cut -f1)"
            rm -f "${lib}"
            echo "  ${label} 移除未引用的 ONNX Runtime 动态库: ${name} (-${size})"
        fi
    done
}

prune_stale_ort_dylibs "${STAGING}/${APP_NAME}.app" "Standalone"
prune_stale_ort_dylibs "${STAGING}/${APP_NAME}.vst3" "VST3"

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

# ── 校验最终 staging 中的双架构 Mach-O 与动态库链接 ────────────────────────────
validate_bundle_macho_architectures "${STAGING}/${APP_NAME}.app" "Staged Standalone"
validate_bundle_macho_architectures "${STAGING}/${APP_NAME}.vst3" "Staged VST3"
validate_bundle_linkage "${STAGING}/${APP_NAME}.app" "@executable_path/../Frameworks" "Staged Standalone"
validate_bundle_linkage "${STAGING}/${APP_NAME}.vst3" "@loader_path/../Frameworks" "Staged VST3"

# ── 在最终双架构产物合并、复制和校验后进行 ad-hoc 签名 ─────────────────────────
echo "▶ Ad-hoc 签名所有 bundle"
codesign --force --deep --sign - "${STAGING}/${APP_NAME}.app" 2>/dev/null
echo "  ✓ ${APP_NAME}.app"
codesign --force --deep --sign - "${STAGING}/${APP_NAME}.vst3" 2>/dev/null
echo "  ✓ ${APP_NAME}.vst3"
codesign --verify --deep --strict --verbose=2 "${STAGING}/${APP_NAME}.app"
codesign --verify --deep --strict --verbose=2 "${STAGING}/${APP_NAME}.vst3"

# ── 统计 staging 大小 ─────────────────────────────────────────────────────────
STAGING_SIZE=$(du -sh "${STAGING}" | cut -f1)
echo ""
echo "▶ Staging 总大小: ${STAGING_SIZE}"

# ── 生成 PKG（--format pkg|both）─────────────────────────────────────────────
if [ "$FORMAT" != "dmg" ]; then
    build_pkg

    PKG_PATH="${DMG_DIR}/${PKG_NAME}.pkg"
    if [ ! -f "${PKG_PATH}" ]; then
        echo "❌ PKG 生成失败: ${PKG_PATH}"
        exit 1
    fi
    PKG_SIZE=$(du -sh "${PKG_PATH}" | cut -f1)
    echo ""
    echo "══════════════════════════════════════════════"
    echo "  ✅ PKG 安装包生成完成"
    echo "══════════════════════════════════════════════"
    echo "  文件: ${PKG_PATH}"
    echo "  大小: ${PKG_SIZE}"
    echo "  版本: ${VERSION}"
    echo "  架构: Universal2 (x86_64 + arm64)"
    echo "  欢迎页: 防诈声明 + 许可/版权警示（文案来自 Installer/NOTICE.md）"
    echo "  安装方式：双击 PKG，按提示完成安装"
    echo "══════════════════════════════════════════════"
fi

# ── 生成 DMG（--format dmg|both）─────────────────────────────────────────────
if [ "$FORMAT" != "pkg" ]; then
    echo "▶ 生成 DMG 安装包"

    DMG_PATH="${DMG_DIR}/${DMG_NAME}.dmg"
    # 如果已有同名 DMG，先删除
    rm -f "${DMG_PATH}"

    # 直接从 staging 创建压缩只读 DMG（跳过 AppleScript 美化，终端环境不稳定）
    hdiutil create \
        -srcfolder "${STAGING}" \
        -volname "${APP_NAME} ${VERSION} Universal2" \
        -fs HFS+ \
        -fsargs "-c c=64,a=16,e=16" \
        -format UDZO \
        -imagekey zlib-level=9 \
        "${DMG_PATH}" \
        2>/dev/null

    # ── 最终结果 ──
    DMG_SIZE=$(du -sh "${DMG_PATH}" | cut -f1)
    echo ""
    echo "══════════════════════════════════════════════"
    echo "  ✅ DMG 安装包生成完成"
    echo "══════════════════════════════════════════════"
    echo "  文件: ${DMG_PATH}"
    echo "  大小: ${DMG_SIZE}"
    echo "  版本: ${VERSION}"
    echo "  架构: Universal2 (x86_64 + arm64)"
    echo ""
    echo "  安装方式：双击 DMG → 双击「安装 OpenTune.command」"
    echo "  或手动复制 .app 到 /Applications、.vst3 到当前用户的 VST3 目录"
    echo "══════════════════════════════════════════════"
fi
