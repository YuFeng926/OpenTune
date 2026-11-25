#!/bin/bash
# Assemble the macOS ONNX Runtime dependency used by OpenTune.
#
# Microsoft no longer serves the ONNX Runtime source repository or its macOS
# packages, and every published macOS ONNX Runtime binary requires macOS 13.0
# or newer while OpenTune still supports macOS 12.0. The universal2 library is
# therefore taken from the macOS package this project published for 1.8.2 —
# built from ONNX Runtime v1.20.0 with CoreML and a macOS 12.0 deployment
# target — and the public headers come from the official 1.20.0 NuGet package.
#
# Both inputs are pinned by SHA-256: the package digest published by GitHub for
# the release asset, and the digest of the immutable NuGet package.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.20.0"
THIRDPARTY_DIR="${ROOT_DIR}/ThirdParty"
WORK_DIR="${THIRDPARTY_DIR}/.onnxruntime-${VERSION}-package"
OUTPUT_DIR="${THIRDPARTY_DIR}/onnxruntime-osx-universal2-${VERSION}"

PKG_NAME="OpenTune-1.8.2-macOS-Universal2.pkg"
PKG_URL="https://github.com/YuFeng926/OpenTune/releases/download/OpenTuneV1.8.2/${PKG_NAME}"
PKG_SHA256="ee602d3d2be09e1db18f475ac54e7d50ac8f33015f4830ba5ea58d8d2de3686f"

NUPKG_NAME="microsoft.ml.onnxruntime.${VERSION}.nupkg"
NUPKG_URL="https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${VERSION}"
NUPKG_SHA256="36dbafacb1bc532e92d02371bd70582a61105872561fc9982314854d1c7ebf72"

MAX_MIN_OS="12.0"

CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=true ;;
        -h|--help)
            echo "用法: $0 [--clean]"
            echo "  --clean  删除缓存的输入包与输出目录后重新组装"
            exit 0
            ;;
        *)
            echo "未知参数: $arg"
            exit 1
            ;;
    esac
done

if [ "$(uname -s)" != "Darwin" ]; then
    echo "❌ 此脚本只能在 macOS 上运行"
    exit 1
fi

for cmd in curl shasum pkgutil unzip lipo otool install_name_tool codesign file find; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "❌ 缺少工具: $cmd"
        exit 1
    fi
done

mkdir -p "$THIRDPARTY_DIR"

if [ "$CLEAN" = true ]; then
    rm -rf "$WORK_DIR" "$OUTPUT_DIR"
fi
mkdir -p "$WORK_DIR"

fetch_verified() {
    local url="$1"
    local destination="$2"
    local expected="$3"
    local label="$4"

    if [ -f "$destination" ]; then
        local cached
        cached="$(shasum -a 256 "$destination" | awk '{ print $1 }')"
        if [ "$cached" = "$expected" ]; then
            echo "▶ 复用已验证的 ${label}"
            return 0
        fi
        echo "▶ ${label} 缓存校验失败，重新下载"
        rm -f "$destination"
    fi

    echo "▶ 下载 ${label}"
    curl -L --fail --retry 5 --retry-delay 2 --silent --show-error \
        "$url" -o "$destination"
    local actual
    actual="$(shasum -a 256 "$destination" | awk '{ print $1 }')"
    if [ "$actual" != "$expected" ]; then
        echo "❌ ${label} SHA-256 校验失败: ${actual}"
        rm -f "$destination"
        exit 1
    fi
}

PKG_FILE="${WORK_DIR}/${PKG_NAME}"
NUPKG_FILE="${WORK_DIR}/${NUPKG_NAME}"
fetch_verified "$PKG_URL" "$PKG_FILE" "$PKG_SHA256" "OpenTune 1.8.2 macOS 安装包"
fetch_verified "$NUPKG_URL" "$NUPKG_FILE" "$NUPKG_SHA256" "ONNX Runtime ${VERSION} NuGet 包"

EXPAND_DIR="${WORK_DIR}/expanded"
if [ ! -d "$EXPAND_DIR" ]; then
    echo "▶ 展开安装包"
    rm -rf "$EXPAND_DIR"
    pkgutil --expand-full "$PKG_FILE" "$EXPAND_DIR"
fi

SOURCE_LIB="$(find "$EXPAND_DIR" -type f -name "libonnxruntime.${VERSION}.dylib" -print -quit)"
if [ -z "$SOURCE_LIB" ]; then
    echo "❌ 安装包中未找到 libonnxruntime.${VERSION}.dylib"
    exit 1
fi

NUGET_DIR="${WORK_DIR}/nuget"
rm -rf "$NUGET_DIR"
mkdir -p "$NUGET_DIR"
unzip -q "$NUPKG_FILE" -d "$NUGET_DIR"

echo "▶ 组装 universal2 包: ${OUTPUT_DIR}"
rm -rf "$OUTPUT_DIR"
mkdir -p "${OUTPUT_DIR}/include" "${OUTPUT_DIR}/lib"

cp "${NUGET_DIR}"/build/native/include/*.h "${OUTPUT_DIR}/include/"
if [ ! -f "${OUTPUT_DIR}/include/onnxruntime_cxx_api.h" ]; then
    echo "❌ ONNX Runtime 公共头文件未就位"
    exit 1
fi

UNIVERSAL_LIB="${OUTPUT_DIR}/lib/libonnxruntime.${VERSION}.dylib"
cp "$SOURCE_LIB" "$UNIVERSAL_LIB"
chmod u+w "$UNIVERSAL_LIB"
install_name_tool -id "@rpath/libonnxruntime.${VERSION}.dylib" "$UNIVERSAL_LIB"
codesign --force --sign - "$UNIVERSAL_LIB" >/dev/null 2>&1 || true
ln -s "libonnxruntime.${VERSION}.dylib" "${OUTPUT_DIR}/lib/libonnxruntime.dylib"

for notice in LICENSE ThirdPartyNotices.txt; do
    if [ -f "${NUGET_DIR}/${notice}" ]; then
        cp "${NUGET_DIR}/${notice}" "${OUTPUT_DIR}/${notice}"
    fi
done
printf '%s\n' "$VERSION" > "${OUTPUT_DIR}/VERSION_NUMBER"
{
    echo "onnxruntime=${VERSION}"
    echo "package=${PKG_NAME}"
    echo "package_sha256=${PKG_SHA256}"
    echo "nuget_sha256=${NUPKG_SHA256}"
} > "${OUTPUT_DIR}/SOURCE.txt"

archs="$(lipo -archs "$UNIVERSAL_LIB")"
if ! printf '%s\n' "$archs" | tr ' ' '\n' | grep -Fxq arm64 \
        || ! printf '%s\n' "$archs" | tr ' ' '\n' | grep -Fxq x86_64; then
    echo "❌ universal2 架构校验失败: ${archs}"
    exit 1
fi

# Every slice must run on the oldest macOS this project supports.
version_le() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        split(a, x, "."); split(b, y, ".");
        for (i = 1; i <= 3; ++i) {
            if (x[i] + 0 < y[i] + 0) exit 0;
            if (x[i] + 0 > y[i] + 0) exit 1;
        }
        exit 0;
    }'
}

min_versions="$(otool -l "$UNIVERSAL_LIB" | awk '$1 == "minos" { print $2 }')"
if [ -z "$min_versions" ]; then
    echo "❌ 无法读取 ONNX Runtime 最低系统版本"
    exit 1
fi
while IFS= read -r min_version; do
    if ! version_le "$min_version" "$MAX_MIN_OS"; then
        echo "❌ ONNX Runtime 最低系统版本 ${min_version} 高于项目支持的最低版本 ${MAX_MIN_OS}"
        exit 1
    fi
done <<< "$min_versions"

echo "✅ ONNX Runtime ${VERSION} universal2 已组装"
echo "   package: ${OUTPUT_DIR}"
echo "   arch:    ${archs}"
echo "   min OS:  ${min_versions//$'\n'/, }"
