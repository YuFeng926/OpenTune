# ==============================================================================
# OpenTune - AI Pitch Correction Application
# JUCE-based Standalone Application
# Company: DAYA
# ==============================================================================
cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# macOS Deployment Target (must be set before project() to take effect)
# Locks both the binary's LC_VERSION_MIN_MACOSX and Info.plist LSMinimumSystemVersion
# to macOS 13.3 (Ventura) — required because bundled libonnxruntime 1.23.0 was built
# for macOS 13.3+ and its internal API references would lazy-bind fail on older systems.
# ==============================================================================
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment target")

# ==============================================================================
# Project Definition
# ==============================================================================
project(OpenTune VERSION 1.7.5 LANGUAGES CXX C)

# 为 clangd/LSP 导出编译数据库（compile_commands.json）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(OPENTUNE_THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")

# ==============================================================================
# C++ Standard
# ==============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# MSVC Runtime (Static Linking - No external dependencies)
# ==============================================================================
if(MSVC)
    # /MTd for Debug, /MT for non-Debug.
    # NOTE: "$<CONFIG:Release>" expands to 0/1 and would produce invalid values like "MultiThreaded0".
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ==============================================================================
# ARA SDK Configuration (for VST3 ARA Extension support)
# ==============================================================================
set(OPENTUNE_ARA_SDK_VERSION "2.2.0" CACHE STRING "ARA SDK version")
set(OPENTUNE_DEFAULT_ARA_SDK_PATH "${OPENTUNE_THIRDPARTY_DIR}/ARA_SDK-releases-${OPENTUNE_ARA_SDK_VERSION}")
if(NOT DEFINED ARA_SDK_PATH OR "${ARA_SDK_PATH}" STREQUAL "")
    set(ARA_SDK_PATH "${OPENTUNE_DEFAULT_ARA_SDK_PATH}" CACHE PATH "Path to ARA SDK" FORCE)
elseif(NOT EXISTS "${ARA_SDK_PATH}" AND EXISTS "${OPENTUNE_DEFAULT_ARA_SDK_PATH}")
    message(STATUS "ARA_SDK_PATH reset to bundled dependency path: ${OPENTUNE_DEFAULT_ARA_SDK_PATH}")
    set(ARA_SDK_PATH "${OPENTUNE_DEFAULT_ARA_SDK_PATH}" CACHE PATH "Path to ARA SDK" FORCE)
endif()

# ==============================================================================
# JUCE Configuration
# ==============================================================================
set(OPENTUNE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE-master")
set(JUCE_SOURCE_DIR "${OPENTUNE_JUCE_DIR}" CACHE INTERNAL "Vendored JUCE source root")
set(JUCE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/JUCE" CACHE INTERNAL "Vendored JUCE binary root")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenTuneJuceVST3ClientOverlay.cmake")
include("${OPENTUNE_JUCE_DIR}/extras/Build/CMake/JUCEModuleSupport.cmake")
include("${OPENTUNE_JUCE_DIR}/extras/Build/CMake/JUCEUtils.cmake")
add_subdirectory("${OPENTUNE_JUCE_DIR}/extras/Build" "${JUCE_BINARY_DIR}/extras/Build")
add_subdirectory("${OPENTUNE_JUCE_DIR}/modules" "${JUCE_BINARY_DIR}/modules")
opentune_use_juce_vst3_client_ara_legacy_bind_overlay("${OPENTUNE_JUCE_DIR}")

# ARA2 VST3 is the only plugin build. Hosts without ARA binding use the same
# binary and naturally fall back to the Capture workflow at runtime.
if(NOT EXISTS "${ARA_SDK_PATH}")
    message(FATAL_ERROR
        "ARA SDK ${OPENTUNE_ARA_SDK_VERSION} is required at ${ARA_SDK_PATH}. "
        "Clone it with: git clone --recursive --branch releases/${OPENTUNE_ARA_SDK_VERSION} https://github.com/Celemony/ARA_SDK")
endif()
juce_set_ara_sdk_path("${ARA_SDK_PATH}")
message(STATUS "ARA SDK found at: ${ARA_SDK_PATH} (ARA2 VST3 enabled)")

# ==============================================================================
# ONNX Runtime Configuration
# ==============================================================================
if(WIN32)
    # ---- Windows: vendored DirectML + D3D12 Agility SDK ----

    set(OPENTUNE_DIRECTML_VERSION "1.15.4" CACHE STRING "Microsoft.AI.DirectML NuGet version")
    set(OPENTUNE_D3D12_AGILITY_VERSION "1.619.1" CACHE STRING "Microsoft.Direct3D.D3D12 NuGet version")

    if(OPENTUNE_DIRECTML_VERSION VERSION_LESS "1.13.0")
        message(FATAL_ERROR "DirectML version must be >= 1.13.0")
    endif()
    if(OPENTUNE_D3D12_AGILITY_VERSION VERSION_LESS "1.614.0")
        message(FATAL_ERROR "DirectX Agility SDK version must be >= 1.614.0")
    endif()

    set(OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT "C:/Program Files (x86)/Windows Kits/10/Include"
        CACHE PATH "Windows Kits 10 include root")
    set(OPENTUNE_WINDOWS_KITS_LIB_ROOT "C:/Program Files (x86)/Windows Kits/10/Lib"
        CACHE PATH "Windows Kits 10 library root")

    function(opentune_find_windows_um_include_file file_name out_var)
        if(NOT EXISTS "${OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT}")
            message(FATAL_ERROR "Windows Kits include root missing: ${OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT}")
        endif()

        file(GLOB windows_kits_version_dirs LIST_DIRECTORIES true "${OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT}/*")
        list(SORT windows_kits_version_dirs COMPARE NATURAL ORDER DESCENDING)

        foreach(version_dir IN LISTS windows_kits_version_dirs)
            set(candidate_path "${version_dir}/um/${file_name}")
            if(EXISTS "${candidate_path}")
                set(${out_var} "${candidate_path}" PARENT_SCOPE)
                return()
            endif()
        endforeach()

        message(FATAL_ERROR
            "Windows SDK header not found: ${file_name} under ${OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT}")
    endfunction()

    function(opentune_find_windows_um_x64_lib library_name out_var)
        if(NOT EXISTS "${OPENTUNE_WINDOWS_KITS_LIB_ROOT}")
            message(FATAL_ERROR "Windows Kits lib root missing: ${OPENTUNE_WINDOWS_KITS_LIB_ROOT}")
        endif()

        file(GLOB windows_kits_version_dirs LIST_DIRECTORIES true "${OPENTUNE_WINDOWS_KITS_LIB_ROOT}/*")
        list(SORT windows_kits_version_dirs COMPARE NATURAL ORDER DESCENDING)

        foreach(version_dir IN LISTS windows_kits_version_dirs)
            set(candidate_path "${version_dir}/um/x64/${library_name}.lib")
            if(EXISTS "${candidate_path}")
                set(${out_var} "${candidate_path}" PARENT_SCOPE)
                return()
            endif()
        endforeach()

        message(FATAL_ERROR
            "Windows SDK import library not found: ${library_name}.lib under ${OPENTUNE_WINDOWS_KITS_LIB_ROOT}")
    endfunction()

    set(OPENTUNE_DEFAULT_DIRECTML_ROOT
        "${OPENTUNE_THIRDPARTY_DIR}/microsoft.ai.directml.${OPENTUNE_DIRECTML_VERSION}")
    if(NOT DEFINED OPENTUNE_DIRECTML_ROOT OR "${OPENTUNE_DIRECTML_ROOT}" STREQUAL "")
        set(OPENTUNE_DIRECTML_ROOT "${OPENTUNE_DEFAULT_DIRECTML_ROOT}" CACHE PATH "Vendored Microsoft.AI.DirectML package root" FORCE)
    elseif(NOT EXISTS "${OPENTUNE_DIRECTML_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_DIRECTML_ROOT}")
        message(STATUS "OPENTUNE_DIRECTML_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_DIRECTML_ROOT}")
        set(OPENTUNE_DIRECTML_ROOT "${OPENTUNE_DEFAULT_DIRECTML_ROOT}" CACHE PATH "Vendored Microsoft.AI.DirectML package root" FORCE)
    endif()

    set(OPENTUNE_DEFAULT_D3D12_AGILITY_ROOT
        "${OPENTUNE_THIRDPARTY_DIR}/microsoft.direct3d.d3d12.${OPENTUNE_D3D12_AGILITY_VERSION}")
    if(NOT DEFINED OPENTUNE_D3D12_AGILITY_ROOT OR "${OPENTUNE_D3D12_AGILITY_ROOT}" STREQUAL "")
        set(OPENTUNE_D3D12_AGILITY_ROOT "${OPENTUNE_DEFAULT_D3D12_AGILITY_ROOT}" CACHE PATH "Vendored Microsoft.Direct3D.D3D12 package root" FORCE)
    elseif(NOT EXISTS "${OPENTUNE_D3D12_AGILITY_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_D3D12_AGILITY_ROOT}")
        message(STATUS "OPENTUNE_D3D12_AGILITY_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_D3D12_AGILITY_ROOT}")
        set(OPENTUNE_D3D12_AGILITY_ROOT "${OPENTUNE_DEFAULT_D3D12_AGILITY_ROOT}" CACHE PATH "Vendored Microsoft.Direct3D.D3D12 package root" FORCE)
    endif()

    set(OPENTUNE_DIRECTML_PACKAGE_INCLUDE_DIR "${OPENTUNE_DIRECTML_ROOT}/include")
    set(OPENTUNE_DIRECTML_PACKAGE_DLL "${OPENTUNE_DIRECTML_ROOT}/bin/x64-win/DirectML.dll")
    set(OPENTUNE_DIRECTML_PACKAGE_LIB "${OPENTUNE_DIRECTML_ROOT}/bin/x64-win/DirectML.lib")

    if(NOT EXISTS "${OPENTUNE_DIRECTML_PACKAGE_INCLUDE_DIR}/DirectML.h"
            OR NOT EXISTS "${OPENTUNE_DIRECTML_PACKAGE_DLL}"
            OR NOT EXISTS "${OPENTUNE_DIRECTML_PACKAGE_LIB}")
        message(FATAL_ERROR
            "Vendored DirectML package is required under ${OPENTUNE_DEFAULT_DIRECTML_ROOT}. "
            "Install it via ThirdParty/ or provide DIRECTML_ROOT.")
    endif()

    set(DIRECTML_INCLUDE_DIR "${OPENTUNE_DIRECTML_PACKAGE_INCLUDE_DIR}")
    set(OPENTUNE_DEFAULT_DIRECTML_DLL "${OPENTUNE_DIRECTML_PACKAGE_DLL}")
    set(DIRECTML_LIB "${OPENTUNE_DIRECTML_PACKAGE_LIB}")

    if(NOT DEFINED OPENTUNE_DIRECTML_DLL OR "${OPENTUNE_DIRECTML_DLL}" STREQUAL "")
        set(OPENTUNE_DIRECTML_DLL "${OPENTUNE_DEFAULT_DIRECTML_DLL}" CACHE FILEPATH "DirectML runtime DLL source" FORCE)
    elseif(NOT EXISTS "${OPENTUNE_DIRECTML_DLL}" AND EXISTS "${OPENTUNE_DEFAULT_DIRECTML_DLL}")
        message(STATUS "OPENTUNE_DIRECTML_DLL reset to available runtime path: ${OPENTUNE_DEFAULT_DIRECTML_DLL}")
        set(OPENTUNE_DIRECTML_DLL "${OPENTUNE_DEFAULT_DIRECTML_DLL}" CACHE FILEPATH "DirectML runtime DLL source" FORCE)
    endif()
    set(DIRECTML_DLL "${OPENTUNE_DIRECTML_DLL}")

    set(D3D12_AGILITY_NATIVE_DIR "${OPENTUNE_D3D12_AGILITY_ROOT}/build/native")
    set(D3D12_AGILITY_INCLUDE_DIR "${D3D12_AGILITY_NATIVE_DIR}/include")
    set(D3D12_AGILITY_D3DX12_INCLUDE_DIR "${D3D12_AGILITY_INCLUDE_DIR}/d3dx12")
    set(D3D12_AGILITY_BIN_DIR "${D3D12_AGILITY_NATIVE_DIR}/bin/x64")
    set(D3D12_CORE_DLL "${D3D12_AGILITY_BIN_DIR}/D3D12Core.dll")
    set(D3D12_SDK_LAYERS_DLL "${D3D12_AGILITY_BIN_DIR}/D3D12SDKLayers.dll")

    string(REPLACE "." ";" OPENTUNE_D3D12_AGILITY_VERSION_PARTS "${OPENTUNE_D3D12_AGILITY_VERSION}")
    list(LENGTH OPENTUNE_D3D12_AGILITY_VERSION_PARTS OPENTUNE_D3D12_AGILITY_PARTS_LEN)
    if(OPENTUNE_D3D12_AGILITY_PARTS_LEN LESS 2)
        message(FATAL_ERROR "Invalid OPENTUNE_D3D12_AGILITY_VERSION: ${OPENTUNE_D3D12_AGILITY_VERSION}")
    endif()
    list(GET OPENTUNE_D3D12_AGILITY_VERSION_PARTS 1 OPENTUNE_D3D12_SDK_VERSION)

    if(NOT EXISTS "${DIRECTML_INCLUDE_DIR}/DirectML.h")
        message(FATAL_ERROR "DirectML header missing: ${DIRECTML_INCLUDE_DIR}/DirectML.h")
    endif()
    if(NOT EXISTS "${DIRECTML_DLL}")
        message(FATAL_ERROR "DirectML runtime DLL missing: ${DIRECTML_DLL}")
    endif()
    if(NOT EXISTS "${D3D12_AGILITY_INCLUDE_DIR}/d3d12.h")
        message(FATAL_ERROR "D3D12 header missing from Agility SDK: ${D3D12_AGILITY_INCLUDE_DIR}/d3d12.h")
    endif()
    if(NOT EXISTS "${D3D12_AGILITY_D3DX12_INCLUDE_DIR}")
        message(FATAL_ERROR "d3dx12 headers missing from Agility SDK: ${D3D12_AGILITY_D3DX12_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${D3D12_CORE_DLL}")
        message(FATAL_ERROR "D3D12Core.dll missing from Agility SDK: ${D3D12_CORE_DLL}")
    endif()
    if(NOT EXISTS "${D3D12_SDK_LAYERS_DLL}")
        message(FATAL_ERROR "D3D12SDKLayers.dll missing from Agility SDK: ${D3D12_SDK_LAYERS_DLL}")
    endif()

    opentune_find_windows_um_x64_lib("d3d12" OPENTUNE_D3D12_SYSTEM_LIB)
    opentune_find_windows_um_x64_lib("dxgi" OPENTUNE_DXGI_SYSTEM_LIB)

    # CPU ONNX Runtime v1.23.0: headers + libs
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-win-x64-1.23.0")
    if(NOT DEFINED ONNXRUNTIME_ROOT OR "${ONNXRUNTIME_ROOT}" STREQUAL "")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime CPU root directory" FORCE)
    elseif(NOT EXISTS "${ONNXRUNTIME_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        message(STATUS "ONNXRUNTIME_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime CPU root directory" FORCE)
    endif()

elseif(APPLE)
    # ---- macOS: universal2 (arm64 + x86_64), ONNX Runtime v1.23.0 with CoreML ----
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-osx-universal2-1.23.0")
    if(NOT DEFINED ONNXRUNTIME_ROOT OR "${ONNXRUNTIME_ROOT}" STREQUAL "")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}"
            CACHE PATH "ONNX Runtime root directory" FORCE)
    elseif(NOT EXISTS "${ONNXRUNTIME_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        message(STATUS "ONNXRUNTIME_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}"
            CACHE PATH "ONNX Runtime root directory" FORCE)
    endif()

else()
    message(FATAL_ERROR "Unsupported platform. Only Windows and macOS are supported.")
endif()
set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")

# Verify core header exists
if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime C++ API header not found at ${ONNXRUNTIME_INCLUDE_DIR}")
endif()

if(WIN32)
    # DirectML ONNX Runtime v1.23.0: runtime DLLs + DML provider headers
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-dml-1.23.0")
    if(NOT DEFINED ONNXRUNTIME_DML_ROOT OR "${ONNXRUNTIME_DML_ROOT}" STREQUAL "")
        set(ONNXRUNTIME_DML_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT}" CACHE PATH "ONNX Runtime DirectML package root" FORCE)
    elseif(NOT EXISTS "${ONNXRUNTIME_DML_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT}")
        message(STATUS "ONNXRUNTIME_DML_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT}")
        set(ONNXRUNTIME_DML_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT}" CACHE PATH "ONNX Runtime DirectML package root" FORCE)
    endif()
    set(ONNXRUNTIME_DML_INCLUDE_DIR "${ONNXRUNTIME_DML_ROOT}/build/native/include")
    set(ONNXRUNTIME_DML_RUNTIME_DIR "${ONNXRUNTIME_DML_ROOT}/runtimes/win-x64/native")

    # Use DML provider header from DirectML package, but avoid pulling its duplicate ONNX headers.
    # Copy dml_provider_factory.h into a generated include dir so it resolves onnxruntime_c_api.h
    # from ONNXRUNTIME_INCLUDE_DIR.
    set(ONNXRUNTIME_DML_PROVIDER_HEADER "${ONNXRUNTIME_DML_INCLUDE_DIR}/dml_provider_factory.h")
    set(ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/onnxruntime_dml_include")
    if(NOT EXISTS "${ONNXRUNTIME_DML_PROVIDER_HEADER}")
        message(FATAL_ERROR "DirectML provider header missing: ${ONNXRUNTIME_DML_PROVIDER_HEADER}")
    endif()
    file(MAKE_DIRECTORY "${ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR}")
    configure_file(
        "${ONNXRUNTIME_DML_PROVIDER_HEADER}"
        "${ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR}/dml_provider_factory.h"
        COPYONLY
    )

    # Core DLL: always use the DML build (builtin DML, larger, covers CPU+DML)
    set(ONNXRUNTIME_DLL "${ONNXRUNTIME_DML_RUNTIME_DIR}/onnxruntime.dll")

    # 运行时 DLL 唯一名（Windows）：OpenTune 专用 ONNX Runtime 运行库，带版本后缀，
    # 绝不使用裸名 onnxruntime.dll，避免与宿主 DAW 自带同名 DLL 撞名。
    # 源仍是第三方包内 onnxruntime.dll（builtin DML），仅输出改名。
    set(OPENTUNE_ORT_DLL_NAME "OpenTuneOnnxRuntime_1_23_0.dll")

    # 用 MSVC lib.exe 从项目内 .def（导出表取自上述 DML DLL 精确导出）生成与新 DLL 名
    # 匹配的导入库，供 OpenTune / Standalone / VST3 / Windows 测试目标链接。
    set(OPENTUNE_ORT_DEF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenTuneOnnxRuntime_1_23_0.def")
    set(OPENTUNE_ORT_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/OpenTuneOnnxRuntime_1_23_0.lib")
    add_custom_command(
        OUTPUT "${OPENTUNE_ORT_IMPORT_LIB}"
        COMMAND "${CMAKE_AR}" /NOLOGO
            "/DEF:${OPENTUNE_ORT_DEF_FILE}"
            "/OUT:${OPENTUNE_ORT_IMPORT_LIB}"
            /MACHINE:X64
        DEPENDS "${OPENTUNE_ORT_DEF_FILE}"
        COMMENT "Generating import library for renamed ONNX Runtime DLL (${OPENTUNE_ORT_DLL_NAME})..."
        VERBATIM
    )
    add_custom_target(OpenTuneOnnxRuntimeImportLib DEPENDS "${OPENTUNE_ORT_IMPORT_LIB}")

    if(NOT EXISTS "${ONNXRUNTIME_DLL}")
        message(FATAL_ERROR "ONNX Runtime DirectML DLL missing: ${ONNXRUNTIME_DLL}")
    endif()

    message(STATUS "ONNX Runtime version: 1.23.0")
    message(STATUS "ONNX Runtime CPU root: ${ONNXRUNTIME_ROOT}")
    message(STATUS "ONNX Runtime DML root: ${ONNXRUNTIME_DML_ROOT}")
    message(STATUS "ONNX Runtime DLL source: ${ONNXRUNTIME_DLL}")
    message(STATUS "ONNX Runtime DLL output name: ${OPENTUNE_ORT_DLL_NAME}")
    message(STATUS "ONNX Runtime DML mode: builtin (DML integrated in ONNX Runtime DLL)")
    message(STATUS "DirectML version: ${OPENTUNE_DIRECTML_VERSION}")
    message(STATUS "DirectML include dir: ${DIRECTML_INCLUDE_DIR}")
    message(STATUS "DirectML DLL: ${DIRECTML_DLL}")
    message(STATUS "DirectX Agility version: ${OPENTUNE_D3D12_AGILITY_VERSION}")
    message(STATUS "DirectX Agility root: ${OPENTUNE_D3D12_AGILITY_ROOT}")
    message(STATUS "DirectX Agility SDK version token: ${OPENTUNE_D3D12_SDK_VERSION}")

elseif(APPLE)
    message(STATUS "ONNX Runtime version: 1.23.0 (macOS universal2)")
    message(STATUS "ONNX Runtime root: ${ONNXRUNTIME_ROOT}")
endif()

# ==============================================================================
# Application Definition - JUCE juce_add_plugin
# ==============================================================================
juce_add_plugin(OpenTune
    # Company Information
    COMPANY_NAME "DAYA"
    COMPANY_WEBSITE "https://daya.audio"
    COMPANY_EMAIL "support@daya.audio"
    
    # Plugin Identification
    PLUGIN_MANUFACTURER_CODE DAYA
    PLUGIN_CODE De77
    
    # Product Information
    PRODUCT_NAME "OpenTune"
    BUNDLE_ID "com.daya.opentune"
    VERSION "${PROJECT_VERSION}"

    # Plugin Metadata
    PLUGIN_NAME "OpenTune"
    PLUGIN_DESCRIPTION "AI-powered Auto-Tune Plugin"
    PLUGIN_CATEGORY "Fx|Pitch Shift"
    
    # Formats (Standalone + VST3 dual-format build)
    FORMATS Standalone VST3

    # The VST3 binary always includes the ARA2 extension.
    IS_ARA_EFFECT TRUE
    
    # Plugin Type
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    
    # Editor Configuration
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    
    # Icons
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    
    # Build Options
    COPY_PLUGIN_AFTER_BUILD FALSE

    # macOS Info.plist merge
    PLIST_TO_MERGE
    "<plist version=\"1.0\">
    <dict>
        <key>LSMinimumSystemVersion</key>
        <string>13.3</string>
        <key>LSApplicationCategoryType</key>
        <string>public.app-category.music</string>
        <key>NSHumanReadableCopyright</key>
        <string>Copyright © 2024-2026 DAYA. All rights reserved.</string>
        <key>NSMicrophoneUsageDescription</key>
        <string>OpenTune needs microphone access for real-time audio monitoring.</string>
    </dict>
    </plist>"
)

# ==============================================================================
# Binary Data (Resources)
# ==============================================================================
file(GLOB OPENTUNE_UI_THEME_ASSETS CONFIGURE_DEPENDS
    "Resources/UI/assets/*.png")
list(SORT OPENTUNE_UI_THEME_ASSETS)

file(GLOB OPENTUNE_CURSOR_ASSETS CONFIGURE_DEPENDS
    "Resources/UI/cursors/*.png")
list(SORT OPENTUNE_CURSOR_ASSETS)

juce_add_binary_data(OpenTuneResources
    SOURCES
        Resources/Fonts/HONORSansCN-Medium.ttf
        Resources/PianoSamples-mp3/piano_21.mp3
        Resources/PianoSamples-mp3/piano_22.mp3
        Resources/PianoSamples-mp3/piano_23.mp3
        Resources/PianoSamples-mp3/piano_24.mp3
        Resources/PianoSamples-mp3/piano_25.mp3
        Resources/PianoSamples-mp3/piano_26.mp3
        Resources/PianoSamples-mp3/piano_27.mp3
        Resources/PianoSamples-mp3/piano_28.mp3
        Resources/PianoSamples-mp3/piano_29.mp3
        Resources/PianoSamples-mp3/piano_30.mp3
        Resources/PianoSamples-mp3/piano_31.mp3
        Resources/PianoSamples-mp3/piano_32.mp3
        Resources/PianoSamples-mp3/piano_33.mp3
        Resources/PianoSamples-mp3/piano_34.mp3
        Resources/PianoSamples-mp3/piano_35.mp3
        Resources/PianoSamples-mp3/piano_36.mp3
        Resources/PianoSamples-mp3/piano_37.mp3
        Resources/PianoSamples-mp3/piano_38.mp3
        Resources/PianoSamples-mp3/piano_39.mp3
        Resources/PianoSamples-mp3/piano_40.mp3
        Resources/PianoSamples-mp3/piano_41.mp3
        Resources/PianoSamples-mp3/piano_42.mp3
        Resources/PianoSamples-mp3/piano_43.mp3
        Resources/PianoSamples-mp3/piano_44.mp3
        Resources/PianoSamples-mp3/piano_45.mp3
        Resources/PianoSamples-mp3/piano_46.mp3
        Resources/PianoSamples-mp3/piano_47.mp3
        Resources/PianoSamples-mp3/piano_48.mp3
        Resources/PianoSamples-mp3/piano_49.mp3
        Resources/PianoSamples-mp3/piano_50.mp3
        Resources/PianoSamples-mp3/piano_51.mp3
        Resources/PianoSamples-mp3/piano_52.mp3
        Resources/PianoSamples-mp3/piano_53.mp3
        Resources/PianoSamples-mp3/piano_54.mp3
        Resources/PianoSamples-mp3/piano_55.mp3
        Resources/PianoSamples-mp3/piano_56.mp3
        Resources/PianoSamples-mp3/piano_57.mp3
        Resources/PianoSamples-mp3/piano_58.mp3
        Resources/PianoSamples-mp3/piano_59.mp3
        Resources/PianoSamples-mp3/piano_60.mp3
        Resources/PianoSamples-mp3/piano_61.mp3
        Resources/PianoSamples-mp3/piano_62.mp3
        Resources/PianoSamples-mp3/piano_63.mp3
        Resources/PianoSamples-mp3/piano_64.mp3
        Resources/PianoSamples-mp3/piano_65.mp3
        Resources/PianoSamples-mp3/piano_66.mp3
        Resources/PianoSamples-mp3/piano_67.mp3
        Resources/PianoSamples-mp3/piano_68.mp3
        Resources/PianoSamples-mp3/piano_69.mp3
        Resources/PianoSamples-mp3/piano_70.mp3
        Resources/PianoSamples-mp3/piano_71.mp3
        Resources/PianoSamples-mp3/piano_72.mp3
        Resources/PianoSamples-mp3/piano_73.mp3
        Resources/PianoSamples-mp3/piano_74.mp3
        Resources/PianoSamples-mp3/piano_75.mp3
        Resources/PianoSamples-mp3/piano_76.mp3
        Resources/PianoSamples-mp3/piano_77.mp3
        Resources/PianoSamples-mp3/piano_78.mp3
        Resources/PianoSamples-mp3/piano_79.mp3
        Resources/PianoSamples-mp3/piano_80.mp3
        Resources/PianoSamples-mp3/piano_81.mp3
        Resources/PianoSamples-mp3/piano_82.mp3
        Resources/PianoSamples-mp3/piano_83.mp3
        Resources/PianoSamples-mp3/piano_84.mp3
        Resources/PianoSamples-mp3/piano_85.mp3
        Resources/PianoSamples-mp3/piano_86.mp3
        Resources/PianoSamples-mp3/piano_87.mp3
        Resources/PianoSamples-mp3/piano_88.mp3
        Resources/PianoSamples-mp3/piano_89.mp3
        Resources/PianoSamples-mp3/piano_90.mp3
        Resources/PianoSamples-mp3/piano_91.mp3
        Resources/PianoSamples-mp3/piano_92.mp3
        Resources/PianoSamples-mp3/piano_93.mp3
        Resources/PianoSamples-mp3/piano_94.mp3
        Resources/PianoSamples-mp3/piano_95.mp3
        Resources/PianoSamples-mp3/piano_96.mp3
        Resources/PianoSamples-mp3/piano_97.mp3
        Resources/PianoSamples-mp3/piano_98.mp3
        Resources/PianoSamples-mp3/piano_99.mp3
        Resources/PianoSamples-mp3/piano_100.mp3
        Resources/PianoSamples-mp3/piano_101.mp3
        Resources/PianoSamples-mp3/piano_102.mp3
        Resources/PianoSamples-mp3/piano_103.mp3
        Resources/PianoSamples-mp3/piano_104.mp3
        Resources/PianoSamples-mp3/piano_105.mp3
        Resources/PianoSamples-mp3/piano_106.mp3
        Resources/PianoSamples-mp3/piano_107.mp3
        Resources/PianoSamples-mp3/piano_108.mp3
        ${OPENTUNE_UI_THEME_ASSETS}
        ${OPENTUNE_CURSOR_ASSETS}
)

# ==============================================================================
# Source Files
# ==============================================================================
target_sources(OpenTune
    PRIVATE
        Source/Services/F0ExtractionService.cpp
        Source/Services/F0ExtractionService.h
        Source/Services/ReferenceAnalysisService.cpp
        Source/Services/ReferenceAnalysisService.h
        Source/Audio/AudioFormatRegistry.cpp
        Source/Audio/AudioFormatRegistry.h

        # Preferences UI
        Source/Editor/Preferences/SharedPreferencePages.cpp
        Source/Editor/Preferences/SharedPreferencePages.h
        Source/Editor/Preferences/StandalonePreferencePages.cpp
        Source/Editor/Preferences/StandalonePreferencePages.h
        Source/Editor/Preferences/TabbedPreferencesDialog.h

        # Core Plugin Files
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/SourceStore.cpp
        Source/SourceStore.h
        Source/StandaloneArrangement.cpp
        Source/StandaloneArrangement.h

        # Inference Layer
        Source/Inference/RenderCache.cpp
        Source/Inference/VocoderFactory.h
        Source/Inference/VocoderFactory.cpp
        Source/Inference/DmlVocoder.h
        Source/Inference/DmlVocoder.cpp
        Source/Inference/SoundTouchStretcher.cpp
        Source/Inference/SoundTouchStretcher.h
        Source/Inference/TimeStretchCache.cpp
        Source/Inference/TimeStretchCache.h
        Source/Inference/ChunkRenderStrategy.h
        # AI Model Interfaces
        Source/Inference/IF0Extractor.h
        Source/Inference/VocoderInterface.h
        Source/Inference/OnnxVocoderBase.h
        Source/Inference/OnnxVocoderBase.cpp

        # AI Model Implementations
        Source/Inference/RMVPEExtractor.cpp
        Source/Inference/RMVPEExtractor.h
        Source/Inference/FCPEExtractor.cpp
        Source/Inference/FCPEExtractor.h
        Source/Inference/PCNSFHifiGANVocoder.cpp
        Source/Inference/PCNSFHifiGANVocoder.h
        Source/Inference/GameNoteGenerator.cpp
        Source/Inference/GameNoteGenerator.h
        Source/Inference/ModelFactory.cpp
        Source/Inference/ModelFactory.h
        Source/Inference/F0InferenceService.cpp
        Source/Runtime/ProcessF0Runtime.cpp
        Source/Runtime/ProcessRenderRuntime.cpp
        Source/Inference/VocoderDomain.cpp
        Source/Inference/VocoderInferenceService.cpp
        Source/Inference/VocoderRenderScheduler.cpp

        # DSP
        Source/DSP/ResamplingManager.cpp
        Source/DSP/ResamplingManager.h
        Source/DSP/MelSpectrogram.cpp
        Source/DSP/MelSpectrogram.h
        Source/DSP/F0KeyDetector.cpp
        Source/DSP/F0KeyDetector.h
        Source/DSP/ReferenceAutoAlign.cpp
        Source/DSP/ReferenceAutoAlign.h
        Source/DSP/AutoTunePitchShifter.cpp
        Source/DSP/AutoTunePitchShifter.h
        Source/DSP/AutoTunePeriodDetector.cpp
        Source/DSP/AutoTunePeriodDetector.h
        Source/DSP/NoteEqProcessor.h

        # UI Layer
        Source/Standalone/UI/UIColors.h
        Source/Standalone/UI/UiAssets.h
        Source/Standalone/UI/UiText.h
        Source/Standalone/UI/ThemeTokens.h
        Source/Standalone/UI/OpenTuneLookAndFeel.h
        Source/Standalone/UI/OpenTuneTooltipWindow.h
        Source/Standalone/UI/BlueBreezeTheme.h
        Source/Standalone/UI/DarkBlueGreyTheme.h
        Source/Standalone/UI/AuroraTheme.h
        Source/Standalone/UI/AuroraLookAndFeel.h
        Source/Standalone/UI/AuroraLookAndFeel.cpp
        Source/Standalone/UI/ToolIds.h
        Source/Standalone/UI/ToolbarIcons.h
        Source/Standalone/UI/ParameterPanel.cpp
        Source/Standalone/UI/ParameterPanel.h
        Source/Standalone/UI/PianoRollComponent.cpp
        Source/Standalone/UI/PianoRollComponent.h
        Source/Standalone/UI/PianoRoll/PianoRollOverlayComponent.cpp
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.h

        Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp
        Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h
        Source/Standalone/UI/PianoRoll/InteractionState.cpp
        Source/Standalone/UI/PianoRoll/InteractionState.h
        
        Source/Standalone/UI/PianoRoll/EqGraphRenderer.cpp
        Source/Standalone/UI/PianoRoll/EqGraphRenderer.h
        Source/Standalone/UI/PianoRoll/EqBandInteraction.h
        Source/Standalone/UI/PianoRoll/EqBandInteraction.cpp
        Source/Standalone/UI/PianoRoll/EqPopupComponent.cpp
        Source/Standalone/UI/PianoRoll/EqPopupComponent.h

        Source/Standalone/UI/ViewMapper.h
        Source/Standalone/UI/MenuBarComponent.cpp
        Source/Standalone/UI/MenuBarComponent.h
        Source/Standalone/UI/TransportBarComponent.cpp
        Source/Standalone/UI/TransportBarComponent.h
        Source/Standalone/UI/TopBarComponent.cpp
        Source/Standalone/UI/TopBarComponent.h
        Source/Standalone/UI/TrackPanelComponent.cpp
        Source/Standalone/UI/TrackPanelComponent.h
        Source/Standalone/UI/ArrangementViewComponent.cpp
        Source/Standalone/UI/ArrangementViewComponent.h
        Source/Standalone/UI/TimelineOverviewComponent.cpp
        Source/Standalone/UI/TimelineOverviewComponent.h
        Source/Standalone/UI/TimelineCompositeCache.cpp
        Source/Standalone/UI/TimelineCompositeCache.h

        Source/Standalone/UI/TimelineViewportCamera.h
        Source/Standalone/UI/TimelineViewportPolicy.cpp
        Source/Standalone/UI/TimelineViewportPolicy.h
        Source/Standalone/UI/TimelineLayerComposer.cpp
        Source/Standalone/UI/TimelineLayerComposer.h
        Source/Standalone/UI/WaveformMipmap.cpp
        Source/Standalone/UI/WaveformMipmap.h
        Source/Standalone/UI/RippleOverlayComponent.h
        Source/Editor/AutoRenderOverlayComponent.h
        Source/Editor/PitchShiftDialogContent.h
        Source/Editor/ConfirmDialogContent.h

        # Utilities
        Source/Utils/PitchCurve.cpp
        Source/Utils/PitchCurve.h
        Source/Utils/LegacyNoteGenerator.cpp
        Source/Utils/LegacyNoteGenerator.h
        Source/Utils/NoteGeneratorTypes.h
        Source/Inference/INoteGenerator.h
        Source/Utils/PianoKeyAudition.cpp
        Source/Utils/PianoKeyAudition.h
        Source/Utils/AppLogger.cpp
        Source/Utils/AppLogger.h
        Source/Utils/ChannelLayoutLogger.cpp
        Source/Utils/ChannelLayoutLogger.h
        Source/Utils/AppPreferences.cpp
        Source/Utils/AppPreferences.h
        Source/Utils/PianoRollVisualPreferences.h
        Source/Utils/PitchUtils.h
        Source/Utils/DetectedKey.h
        Source/Utils/Note.h
        Source/Utils/NoteEqSettings.h
        Source/Utils/OnnxRuntimeDelayLoadHook.cpp
        Source/Utils/ModelPathResolver.h
        Source/Utils/ProjectModel.cpp
        Source/Utils/ProjectModel.h
        Source/Utils/ProjectPersistence.cpp
        Source/Utils/ProjectPersistence.h
        Source/Utils/ProjectSession.cpp
        Source/Utils/ProjectSession.h
        Source/Utils/CpuBudgetManager.cpp
        Source/Utils/CpuBudgetManager.h
        Source/Utils/AccelerationDetector.cpp
        Source/Utils/AccelerationDetector.h
        Source/Utils/SimdAccelerator.cpp
        Source/Utils/SimdAccelerator.h
        Source/Utils/UndoManager.cpp
        Source/Utils/UndoManager.h
        Source/Utils/PianoRollEditAction.cpp
        Source/Utils/PianoRollEditAction.h
        Source/Utils/PianoRollNotePatchAction.cpp
        Source/Utils/PianoRollNotePatchAction.h
        Source/Utils/AutomationLane.cpp
        Source/Utils/AutomationLane.h
        Source/Utils/VolumeEnvelopeEditAction.cpp
        Source/Utils/VolumeEnvelopeEditAction.h
        Source/Utils/ScissorsUndoAction.cpp
        Source/Utils/ScissorsUndoAction.h
        Source/Utils/PitchShiftSettings.h
        Source/Utils/PitchShiftEditAction.cpp
        Source/Utils/PitchShiftEditAction.h
        Source/Utils/TimeGrid.cpp
        Source/Utils/TimeGrid.h
        Source/Utils/TimeGridEditAction.cpp
        Source/Utils/TimeGridEditAction.h
        Source/Utils/PlacementActions.cpp
        Source/Utils/PlacementActions.h
        Source/Utils/SilentGapDetector.cpp
        Source/Utils/SilentGapDetector.h
        Source/Utils/TimeCoordinate.h

        # Content Layer
        Source/Content/ContentKey.h
        Source/Content/DomainContentOwner.h
        Source/Content/AnalysisState.h
        Source/Content/EditableContentState.h
        Source/Content/AudioModificationContentState.h
        Source/Content/ContentPayloadState.h
        Source/Content/StandaloneRetiredContentRecord.h
        Source/Content/EditableContentSnapshot.h
        Source/Content/StandaloneClipContent.h
        Source/Content/StandaloneClipContent.cpp
        Source/Content/StandaloneContentRepository.h
        Source/Content/StandaloneContentRepository.cpp
        Source/Content/CaptureSegmentContent.h
        Source/Content/CaptureSegmentContent.cpp

        # Render Layer
        Source/Render/PlaybackReadSource.h
        Source/Render/PlaybackSourcePublisher.h
        Source/Render/PlaybackSourcePublisher.cpp
        Source/Render/RenderJob.h
        Source/Render/RenderCacheRegistry.h
        Source/Render/RenderCacheRegistry.cpp
        Source/Render/RenderWorker.h
        Source/Render/RenderWorker.cpp
        Source/Render/ContentRenderService.h
        Source/Render/ContentRenderService.cpp
        Source/Render/Stage2TimeStretchRebuilder.h
        Source/Render/Stage2TimeStretchRebuilder.cpp
        Source/Render/StretcherPool.h
        Source/Render/StretcherPool.cpp
        Source/Render/RenderChunkPlanner.h
        Source/Render/RenderChunkPlanner.cpp
)

# ==============================================================================
# Conditional Source Files per Format
# ==============================================================================
# Shared Standalone editor sources
set(OPENTUNE_STANDALONE_UI_SOURCES
    Source/Standalone/EditorFactoryStandalone.cpp
    Source/Standalone/PluginEditor.cpp
    Source/Standalone/PluginEditor.h
)

target_sources(OpenTune_Standalone
    PRIVATE
        Source/Utils/D3D12AgilityBootstrap.cpp
        ${OPENTUNE_STANDALONE_UI_SOURCES}
)

target_compile_definitions(OpenTune_Standalone
    PRIVATE
        JucePlugin_Build_Standalone=1
        JucePlugin_Build_VST3=0
)

target_sources(OpenTune_VST3
    PRIVATE
        Source/Utils/D3D12AgilityBootstrap.cpp
        Source/Utils/Vst3ModulePin.cpp
        Source/Editor/EditorFactoryPlugin.cpp
        Source/Plugin/PluginEditor.cpp
        Source/Plugin/PluginEditor.h
)

target_compile_definitions(OpenTune_VST3
    PRIVATE
        JucePlugin_Build_Standalone=0
        JucePlugin_Build_VST3=1
)

if(MSVC)
    target_compile_options(OpenTune_Standalone PRIVATE /utf-8 /bigobj)
    target_compile_options(OpenTune_VST3 PRIVATE /utf-8 /bigobj)
endif()

# ARA2 document integration is always compiled into the VST3 binary.
target_sources(OpenTune PRIVATE
    Source/ARA/AudioSource.cpp
    Source/ARA/AudioSource.h
    Source/ARA/AudioModification.cpp
    Source/ARA/AudioModification.h
    Source/ARA/PlaybackRegion.cpp
    Source/ARA/PlaybackRegion.h
    Source/ARA/OpenTuneDocumentController.cpp
    Source/ARA/OpenTuneDocumentController.h
    Source/ARA/OpenTuneEditorView.cpp
    Source/ARA/OpenTuneEditorView.h
    Source/ARA/OpenTunePlaybackRenderer.cpp
    Source/ARA/OpenTunePlaybackRenderer.h
)

# Runtime fallback for VST3 hosts that do not bind the ARA2 extension.
# CaptureSession remains part of the same ARA2-capable VST3 binary.
target_sources(OpenTune PRIVATE
    Source/Plugin/Capture/CaptureRingBuffer.cpp
    Source/Plugin/Capture/CaptureRingBuffer.h
    Source/Plugin/Capture/CaptureSegment.cpp
    Source/Plugin/Capture/CaptureSegment.h
    Source/Plugin/Capture/CaptureCompactor.cpp
    Source/Plugin/Capture/CaptureCompactor.h
    Source/Plugin/Capture/CaptureSession.cpp
    Source/Plugin/Capture/CaptureSession.h
    Source/Plugin/Capture/CapturePersistence.cpp
    Source/Plugin/Capture/CapturePersistence.h
)

# SoundTouch Library v2.3.3 (WSOLA time-stretch for vocal Stage 2).
# Replaced Rubber Band R3 (phase vocoder) — see archive change swap-time-stretch-to-soundtouch.
# License: LGPL-2.1. See LICENSES/soundtouch.LGPL-2.1.txt.
set(SOUNDTOUCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/soundtouch-2.3.3)
if(EXISTS ${SOUNDTOUCH_DIR}/source/SoundTouch/SoundTouch.cpp)
    target_sources(OpenTune PRIVATE
        ${SOUNDTOUCH_DIR}/source/SoundTouch/AAFilter.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/FIFOSampleBuffer.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/FIRFilter.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateCubic.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateLinear.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateShannon.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/RateTransposer.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/SoundTouch.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/TDStretch.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/cpu_detect_x86.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/sse_optimized.cpp
        ${SOUNDTOUCH_DIR}/source/SoundTouch/mmx_optimized.cpp
    )
    target_compile_definitions(OpenTune PRIVATE SOUNDTOUCH_FLOAT_SAMPLES)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        target_compile_definitions(OpenTune PRIVATE SOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS)
    endif()
    if(MSVC)
        # Suppress benign MSVC warnings from vendored SoundTouch code:
        #   C4458 - 'output' parameter shadows FIFOProcessor::output member (SoundTouch.cpp:511)
        #   C4702 - unreachable code after ST_THROW_RT_ERROR (FIFOSamplePipe.h:63)
        set(SOUNDTOUCH_MSVC_WARN "/wd4458" "/wd4702")
        set_source_files_properties(
            "${SOUNDTOUCH_DIR}/source/SoundTouch/AAFilter.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/FIFOSampleBuffer.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/FIRFilter.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateCubic.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateLinear.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/InterpolateShannon.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/RateTransposer.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/SoundTouch.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/TDStretch.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/cpu_detect_x86.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/sse_optimized.cpp"
            "${SOUNDTOUCH_DIR}/source/SoundTouch/mmx_optimized.cpp"
            PROPERTIES
                COMPILE_OPTIONS "${SOUNDTOUCH_MSVC_WARN}"
        )
    endif()
    message(STATUS "SoundTouch: vendored at ${SOUNDTOUCH_DIR}")
else()
    message(FATAL_ERROR "SoundTouch source not found at ${SOUNDTOUCH_DIR}/source/SoundTouch/SoundTouch.cpp")
endif()

# ==============================================================================
# Include Directories
# ==============================================================================
target_include_directories(OpenTune
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/r8brain-free-src-master
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/soundtouch-2.3.3/include
        ${ONNXRUNTIME_INCLUDE_DIR}
        # JUCE module include paths — required for juce::* namespace resolution
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_audio_basics>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_audio_utils>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_core>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_data_structures>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_dsp>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_events>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_gui_basics>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_gui_extra>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_graphics>
        $<BUILD_INTERFACE:${OPENTUNE_JUCE_DIR}/modules/juce_opengl>
)

if(WIN32)
    target_include_directories(OpenTune
        PRIVATE
            ${ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR}
            ${D3D12_AGILITY_INCLUDE_DIR}
            ${D3D12_AGILITY_D3DX12_INCLUDE_DIR}
            ${D3D12_AGILITY_NATIVE_DIR}
            ${DIRECTML_INCLUDE_DIR}
    )
endif()

# ==============================================================================
# JUCE Modules Linking
# ==============================================================================
target_link_libraries(OpenTune
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_opengl
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        OpenTuneResources

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ==============================================================================
# ONNX Runtime Linking
# ==============================================================================
if(WIN32)
    target_link_libraries(OpenTune PRIVATE "${OPENTUNE_ORT_IMPORT_LIB}")
    add_dependencies(OpenTune OpenTuneOnnxRuntimeImportLib)
elseif(APPLE)
    target_link_libraries(OpenTune PRIVATE "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.dylib")
    target_link_libraries(OpenTune PRIVATE "-framework Accelerate")
endif()

if(WIN32)
    target_link_libraries(OpenTune
        PRIVATE
            "${OPENTUNE_D3D12_SYSTEM_LIB}"
            "${OPENTUNE_DXGI_SYSTEM_LIB}"
            version
    )
endif()

# ==============================================================================
# Compile Definitions
# ==============================================================================
target_compile_definitions(OpenTune
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1

    PRIVATE
        # OpenTune specific
        OPENTUNE_VERSION="${PROJECT_VERSION}"
        OPENTUNE_D3D12_AGILITY_SDK_VERSION=${OPENTUNE_D3D12_SDK_VERSION}
        ORT_API_MANUAL_INIT
        # Audio format support (import)
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_MP3AUDIOFORMAT=1
)

if(WIN32)
    target_compile_definitions(OpenTune PUBLIC JUCE_ASIO=1 JUCE_WASAPI=1)
    target_compile_definitions(OpenTune PRIVATE JUCE_USE_WINDOWS_MEDIA_FORMAT=1)
endif()

# ==============================================================================
# Platform-specific Settings
# ==============================================================================
if(WIN32)
    if(MSVC)
        target_compile_options(OpenTune PRIVATE /utf-8 /MP /wd4100 /wd4127)
        # AVX2 仅对 SIMD 热点文件开启，避免影响 JUCE 编译单元和老 CPU 兼容性
        set_source_files_properties(
            Source/Utils/SimdAccelerator.cpp
            Source/DSP/MelSpectrogram.cpp
            PROPERTIES COMPILE_FLAGS "/arch:AVX2"
        )
        # Delay load the renamed ONNX Runtime DLL for Standalone
        target_link_options(OpenTune_Standalone PRIVATE "/DELAYLOAD:${OPENTUNE_ORT_DLL_NAME}")
        target_link_libraries(OpenTune_Standalone PRIVATE delayimp)
        target_compile_options(OpenTune_Standalone PRIVATE /wd4100)
        add_dependencies(OpenTune_Standalone OpenTuneOnnxRuntimeImportLib)

        # Delay load the renamed ONNX Runtime DLL for VST3 plugin
        target_link_options(OpenTune_VST3 PRIVATE "/DELAYLOAD:${OPENTUNE_ORT_DLL_NAME}")
        target_link_libraries(OpenTune_VST3 PRIVATE delayimp)
        target_compile_options(OpenTune_VST3 PRIVATE /wd4100)
        add_dependencies(OpenTune_VST3 OpenTuneOnnxRuntimeImportLib)
    endif()
elseif(APPLE)
    target_compile_options(OpenTune PRIVATE -Wall -Wno-unused-parameter)

    # Info.plist metadata (LSMinimumSystemVersion / LSApplicationCategoryType /
    # NSHumanReadableCopyright / NSMicrophoneUsageDescription) is injected via
    # juce_add_plugin's PLIST_TO_MERGE above — no second pass needed here.

    # Set rpath so the app bundle can find the embedded dylib
    set_target_properties(OpenTune_Standalone PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide.html" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/docs"
    )
    target_sources(OpenTune_Standalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide.html")
endif()

# ==============================================================================
# Standalone Bundle Resources
# ==============================================================================
# Define model files
# 社区声码器权重（默认）
set(HIFIGAN_MODEL_COMMUNITY "${CMAKE_CURRENT_SOURCE_DIR}/pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx")
# Coulin9 微调版声码器权重（实验性）
set(HIFIGAN_MODEL_COULIN9   "${CMAKE_CURRENT_SOURCE_DIR}/pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44k_hop512_128bin_opentune_fmax22050_v4_user_zh_female_step20000.onnx")
set(RMVPE_MODEL             "${CMAKE_CURRENT_SOURCE_DIR}/models/rmvpe.onnx")
set(FCPE_MODEL              "${CMAKE_CURRENT_SOURCE_DIR}/models/fcpe.onnx")
set(GAME_MODEL_DIR          "${CMAKE_CURRENT_SOURCE_DIR}/models/GAME")

if(EXISTS "${GAME_MODEL_DIR}")
    message(STATUS "GAME note generator model bundle found: ${GAME_MODEL_DIR}")
    set(STANDALONE_GAME_MODEL_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GAME_MODEL_DIR}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/GAME"
    )
    set(STANDALONE_APPLE_GAME_MODEL_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GAME_MODEL_DIR}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/GAME"
    )
    set(VST3_GAME_MODEL_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GAME_MODEL_DIR}"
            "$<TARGET_FILE_DIR:OpenTune_VST3>/models/GAME"
    )
    set(VST3_APPLE_GAME_MODEL_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GAME_MODEL_DIR}"
            "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/GAME"
    )
else()
    message(STATUS "Optional GAME note generator model bundle not found at ${GAME_MODEL_DIR}; Standalone/VST3 packages will omit models/GAME and use Legacy fallback.")
endif()

# --- ONNX Runtime library copying ---
if(WIN32)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime.dll"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime_providers_shared.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/${OPENTUNE_ORT_DLL_NAME}"
        COMMENT "Copying ONNX Runtime DLL (renamed) to Standalone..."
    )

    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DIRECTML_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/DirectML.dll"
        COMMENT "Copying DirectML runtime DLL to Standalone..."
    )

    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${D3D12_CORE_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/D3D12/D3D12Core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${D3D12_SDK_LAYERS_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/D3D12/D3D12SDKLayers.dll"
        COMMENT "Copying DirectX Agility runtime to Standalone..."
    )

    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL_COMMUNITY}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/hifigan.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL_COULIN9}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/hifigan_coulin9.onnx"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/vocoder_weights"
        # RMVPE disabled - FCPE is the default model (smaller, faster, DML-capable)
        # COMMAND ${CMAKE_COMMAND} -E copy_if_different
        #     "${RMVPE_MODEL}"
        #     "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/rmvpe.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FCPE_MODEL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/fcpe.onnx"
        ${STANDALONE_GAME_MODEL_COPY_COMMANDS}
        COMMENT "Copying AI models to Standalone..."
    )

elseif(APPLE)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.23.0.dylib"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Frameworks/"
        COMMENT "Copying ONNX Runtime dylib to app bundle Frameworks..."
    )

    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL_COMMUNITY}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/hifigan.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL_COULIN9}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/hifigan_coulin9.onnx"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/vocoder_weights"
        # RMVPE disabled - FCPE is the default model (smaller, faster, DML-capable)
        # COMMAND ${CMAKE_COMMAND} -E copy_if_different
        #     "${RMVPE_MODEL}"
        #     "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/rmvpe.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FCPE_MODEL}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/fcpe.onnx"
        ${STANDALONE_APPLE_GAME_MODEL_COPY_COMMANDS}
        COMMENT "Copying AI models to app bundle Resources..."
    )
endif()

if(APPLE)
    # macOS: docs go in Contents/Resources/docs/ (bundle standard)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_en.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_en.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ja.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_ja.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ru.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_ru.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_es.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_es.html"
        COMMENT "Copying UserGuide translations to app bundle Resources..."
    )
else()
    # Windows: docs go alongside the executable
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_en.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_en.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ja.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_ja.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ru.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_ru.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_es.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_es.html"
        COMMENT "Copying UserGuide translations to Standalone..."
    )
endif()

# ==============================================================================
# VST3-Specific POST_BUILD Commands
# ==============================================================================
# Copy runtime libraries and AI models into the VST3 plugin bundle.
# Windows lays files alongside the .vst3, macOS embeds them in the bundle.
if(TARGET OpenTune_VST3)
    if(WIN32)
        # ONNX Runtime DLL (renamed)
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:OpenTune_VST3>/onnxruntime.dll"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/onnxruntime_providers_shared.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/${OPENTUNE_ORT_DLL_NAME}"
            COMMENT "Copying ONNX Runtime DLL (renamed) to VST3 plugin..."
        )

        # DirectML DLL
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DIRECTML_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/DirectML.dll"
            COMMENT "Copying DirectML runtime DLL to VST3 plugin..."
        )

        # D3D12 Agility SDK
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:OpenTune_VST3>/D3D12"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3D12_CORE_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/D3D12/D3D12Core.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3D12_SDK_LAYERS_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/D3D12/D3D12SDKLayers.dll"
            COMMENT "Copying DirectX Agility runtime to VST3 plugin..."
        )

        # AI Models (alongside the .vst3)
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:OpenTune_VST3>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIFIGAN_MODEL_COMMUNITY}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/models/hifigan.onnx"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIFIGAN_MODEL_COULIN9}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/models/hifigan_coulin9.onnx"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:OpenTune_VST3>/models/vocoder_weights"
            # RMVPE disabled - FCPE is the default model (smaller, faster, DML-capable)
            # COMMAND ${CMAKE_COMMAND} -E copy_if_different
            #     "${RMVPE_MODEL}"
            #     "$<TARGET_FILE_DIR:OpenTune_VST3>/models/rmvpe.onnx"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FCPE_MODEL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/models/fcpe.onnx"
            ${VST3_GAME_MODEL_COPY_COMMANDS}
            COMMENT "Copying AI models to VST3 plugin..."
        )

    elseif(APPLE)
        # ONNX Runtime dylib → Contents/Frameworks/
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.23.0.dylib"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Frameworks/"
            COMMENT "Copying ONNX Runtime dylib to VST3 bundle Frameworks..."
        )

        # AI Models → Contents/Resources/models/
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIFIGAN_MODEL_COMMUNITY}"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/hifigan.onnx"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIFIGAN_MODEL_COULIN9}"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/hifigan_coulin9.onnx"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/vocoder_weights"
            # RMVPE disabled - FCPE is the default model (smaller, faster, DML-capable)
            # COMMAND ${CMAKE_COMMAND} -E copy_if_different
            #     "${RMVPE_MODEL}"
            #     "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/rmvpe.onnx"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FCPE_MODEL}"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/models/fcpe.onnx"
            ${VST3_APPLE_GAME_MODEL_COPY_COMMANDS}
            COMMENT "Copying AI models to VST3 bundle Resources..."
        )

        # UserGuide translations → Contents/Resources/docs/
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide.html"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs/UserGuide.html"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_en.html"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs/UserGuide_en.html"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ja.html"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs/UserGuide_ja.html"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_ru.html"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs/UserGuide_ru.html"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/userguide/UserGuide_es.html"
                "$<TARGET_BUNDLE_DIR:OpenTune_VST3>/Contents/Resources/docs/UserGuide_es.html"
            COMMENT "Copying UserGuide translations to VST3 bundle Resources..."
        )
    endif()
endif()

# ==============================================================================
# Installation
# ==============================================================================
install(TARGETS OpenTune
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RESOURCE DESTINATION bin
)

# ==============================================================================
# Summary
# ==============================================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "OpenTune Build Configuration")
message(STATUS "========================================")
message(STATUS "Version:          ${PROJECT_VERSION}")
message(STATUS "C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "Company:          DAYA")
message(STATUS "Manufacturer Code: DAYA")
message(STATUS "Plugin Code:      De77")
message(STATUS "ONNX Runtime:     ${ONNXRUNTIME_ROOT}")
message(STATUS "JUCE Path:        ${OPENTUNE_JUCE_DIR}")
message(STATUS "Plugin Formats:   Standalone + VST3")
message(STATUS "========================================")
message(STATUS "")

# ==============================================================================
# Tests (optional, pure standard library — no JUCE / ONNX Runtime linkage)
# ==============================================================================
option(OPENTUNE_BUILD_TESTS "Build AutoTune tests" OFF)
if(OPENTUNE_BUILD_TESTS)
    enable_testing()
    add_executable(OpenTuneAutoTuneTests
        Tests/AutoTunePitchShifterTests.cpp
        Source/DSP/AutoTunePitchShifter.cpp
        Source/DSP/AutoTunePeriodDetector.cpp
    )
    target_include_directories(OpenTuneAutoTuneTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    if(MSVC)
        # 与主工程目标一致：/utf-8 消除 C4819（源码含 UTF-8 注释字符），
        # /EHsc 消除 C4530（std::vector 实例化含异常处理）。
        target_compile_options(OpenTuneAutoTuneTests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME AutoTunePitchShifterTests COMMAND OpenTuneAutoTuneTests)
endif()
