#!/bin/bash
# Build the macOS ONNX Runtime dependency used by OpenTune.
#
# The upstream macOS 1.20.0 binary package has a macOS 13.3 deployment target.
# OpenTune needs macOS 12.0, so this script builds the same upstream tag from
# source twice and combines the arm64/x86_64 libraries into one universal2
# package. v1.20.0 includes the CoreML MLModelConfiguration initialization fix
# for macOS 15 and newer.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.20.0"
TAG="v${VERSION}"
THIRDPARTY_DIR="${ROOT_DIR}/ThirdParty"
SOURCE_DIR="${THIRDPARTY_DIR}/.onnxruntime-${VERSION}-src"
BUILD_ROOT="${THIRDPARTY_DIR}/.onnxruntime-${VERSION}-build"
OUTPUT_DIR="${THIRDPARTY_DIR}/onnxruntime-osx-universal2-${VERSION}"
EIGEN_REV="e7248b26a1ed53fa030c5c459f7ea095dfd276ac"
# GitLab regenerated the archive metadata after ORT v1.20.0 was published;
# this is the current archive hash for the same immutable Eigen revision.
EIGEN_SHA1="32b145f525a8308d7ab1c09388b2e288312d8eba"
EIGEN_DIR="${THIRDPARTY_DIR}/.eigen-${EIGEN_REV}"
EIGEN_ARCHIVE="${THIRDPARTY_DIR}/.eigen-${EIGEN_REV}.zip"
EIGEN_URL="https://gitlab.com/libeigen/eigen/-/archive/${EIGEN_REV}/eigen-${EIGEN_REV}.zip"

CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=true ;;
        -h|--help)
            echo "用法: $0 [--clean]"
            echo "  --clean  删除本脚本创建的 ORT 源码/构建/输出目录后重建"
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

for cmd in git python3 cmake curl unzip shasum lipo otool install_name_tool; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "❌ 缺少工具: $cmd"
        exit 1
    fi
done

mkdir -p "$THIRDPARTY_DIR"

if [ "$CLEAN" = true ]; then
    rm -rf "$SOURCE_DIR" "$BUILD_ROOT" "$OUTPUT_DIR" "$EIGEN_DIR" "$EIGEN_ARCHIVE"
fi

if [ ! -d "${SOURCE_DIR}/.git" ]; then
    echo "▶ 获取 ONNX Runtime ${TAG} 源码（含子模块）"
    rm -rf "$SOURCE_DIR"
    git clone --depth 1 --branch "$TAG" --recurse-submodules \
        https://github.com/microsoft/onnxruntime.git "$SOURCE_DIR"
else
    actual_tag="$(git -C "$SOURCE_DIR" describe --tags --exact-match HEAD 2>/dev/null || true)"
    if [ "$actual_tag" != "$TAG" ]; then
        echo "❌ ORT 源码目录不是 ${TAG}: ${SOURCE_DIR}"
        echo "   请使用 --clean 重新获取固定版本。"
        exit 1
    fi
fi

if [ ! -d "$EIGEN_DIR" ]; then
    echo "▶ 获取 Eigen ${EIGEN_REV}（规避 CMake 4.x 对 GitLab 下载的兼容问题）"
    if [ ! -f "$EIGEN_ARCHIVE" ]; then
        curl -L --fail --retry 5 --retry-delay 2 --silent --show-error \
            "$EIGEN_URL" -o "$EIGEN_ARCHIVE"
    fi
    actual_eigen_sha1="$(shasum -a 1 "$EIGEN_ARCHIVE" | awk '{ print $1 }')"
    if [ "$actual_eigen_sha1" != "$EIGEN_SHA1" ]; then
        echo "❌ Eigen SHA-1 校验失败: ${actual_eigen_sha1}"
        rm -f "$EIGEN_ARCHIVE"
        exit 1
    fi
    rm -rf "${THIRDPARTY_DIR}/eigen-${EIGEN_REV}" "$EIGEN_DIR"
    unzip -q "$EIGEN_ARCHIVE" -d "$THIRDPARTY_DIR"
    mv "${THIRDPARTY_DIR}/eigen-${EIGEN_REV}" "$EIGEN_DIR"
fi

build_arch() {
    local arch="$1"
    local build_dir="${BUILD_ROOT}/${arch}"
    local library

    mkdir -p "$BUILD_ROOT"
    echo "▶ 构建 ONNX Runtime ${VERSION} (${arch}, CoreML, macOS 12.0)"
    python3 "${SOURCE_DIR}/tools/ci_build/build.py" \
        --build_dir "$build_dir" \
        --config Release \
        --parallel 0 \
        --skip_tests \
        --build_shared_lib \
        --use_coreml \
        --osx_arch "$arch" \
        --apple_deploy_target 12.0 \
        --cmake_extra_defines \
            onnxruntime_BUILD_UNIT_TESTS=OFF \
            CMAKE_POLICY_VERSION_MINIMUM=3.5 \
            onnxruntime_USE_PREINSTALLED_EIGEN=ON \
            eigen_SOURCE_PATH="$EIGEN_DIR" \
            CMAKE_CXX_FLAGS="-Wno-deprecated-literal-operator -Wno-dangling-capture"

    library="$(find "$build_dir" -type f \
        \( -name "libonnxruntime.${VERSION}.dylib" -o -name "libonnxruntime.dylib" \) \
        -print -quit)"
    if [ -z "$library" ]; then
        echo "❌ 未找到 ${arch} ONNX Runtime dylib: ${build_dir}"
        exit 1
    fi
    printf '%s\n' "$library"
}

ARM64_LIB="$(build_arch arm64 | tail -1)"
X86_64_LIB="$(build_arch x86_64 | tail -1)"

echo "▶ 组装 universal2 包: ${OUTPUT_DIR}"
rm -rf "$OUTPUT_DIR"
mkdir -p "${OUTPUT_DIR}/include" "${OUTPUT_DIR}/lib"

# The source tree keeps public headers in several subdirectories. The OpenTune
# CMake target intentionally consumes the same flat include layout as the
# upstream package; copy only the public headers required by the app so
# internal headers with duplicate basenames cannot overwrite one another.
for header_name in \
        onnxruntime_c_api.h \
        onnxruntime_cxx_api.h \
        onnxruntime_cxx_inline.h \
        onnxruntime_float16.h \
        onnxruntime_lite_custom_op.h \
        onnxruntime_run_options_config_keys.h \
        onnxruntime_session_options_config_keys.h \
        provider_options.h \
        coreml_provider_factory.h; do
    header="$(find "${SOURCE_DIR}/include/onnxruntime" -type f \
        -name "$header_name" -print -quit)"
    if [ -z "$header" ]; then
        echo "❌ 未找到 ORT 公共头文件: ${header_name}"
        exit 1
    fi
    cp "$header" "${OUTPUT_DIR}/include/${header_name}"
done

if [ ! -f "${OUTPUT_DIR}/include/onnxruntime_cxx_api.h" \
        ] || [ ! -f "${OUTPUT_DIR}/include/coreml_provider_factory.h" ]; then
    echo "❌ ORT public headers were not assembled correctly"
    exit 1
fi

UNIVERSAL_LIB="${OUTPUT_DIR}/lib/libonnxruntime.${VERSION}.dylib"
lipo -create "$ARM64_LIB" "$X86_64_LIB" -output "$UNIVERSAL_LIB"
install_name_tool -id "@rpath/libonnxruntime.${VERSION}.dylib" "$UNIVERSAL_LIB"
ln -s "libonnxruntime.${VERSION}.dylib" "${OUTPUT_DIR}/lib/libonnxruntime.dylib"

cp "${SOURCE_DIR}/LICENSE" "${OUTPUT_DIR}/LICENSE"
if [ -f "${SOURCE_DIR}/ThirdPartyNotices.txt" ]; then
    cp "${SOURCE_DIR}/ThirdPartyNotices.txt" "${OUTPUT_DIR}/ThirdPartyNotices.txt"
fi
printf '%s\n' "$VERSION" > "${OUTPUT_DIR}/VERSION_NUMBER"
git -C "$SOURCE_DIR" rev-parse HEAD > "${OUTPUT_DIR}/GIT_COMMIT_ID"

archs="$(lipo -archs "$UNIVERSAL_LIB")"
if ! printf '%s\n' "$archs" | tr ' ' '\n' | grep -Fxq arm64 \
        || ! printf '%s\n' "$archs" | tr ' ' '\n' | grep -Fxq x86_64; then
    echo "❌ universal2 架构校验失败: ${archs}"
    exit 1
fi

min_versions="$(otool -l "$UNIVERSAL_LIB" | awk '$1 == "minos" { print $2 }')"
while IFS= read -r min_version; do
    if [ "$min_version" != "12.0" ]; then
        echo "❌ ORT 最低系统版本不是 12.0: ${min_version}"
        exit 1
    fi
done <<< "$min_versions"

echo "✅ ONNX Runtime ${VERSION} universal2 已生成"
echo "   package: ${OUTPUT_DIR}"
echo "   arch:    ${archs}"
echo "   min OS:  ${min_versions//$'\n'/, }"
