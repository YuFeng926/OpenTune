# ==============================================================================
# OpenTune - AI Pitch Correction Application
# JUCE-based Standalone Application
# Company: DAYA
# ==============================================================================
cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# Project Definition
# ==============================================================================
project(OpenTune VERSION 1.0.0 LANGUAGES CXX C)

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
include("${OPENTUNE_JUCE_DIR}/extras/Build/CMake/JUCEModuleSupport.cmake")
include("${OPENTUNE_JUCE_DIR}/extras/Build/CMake/JUCEUtils.cmake")
add_subdirectory("${OPENTUNE_JUCE_DIR}/extras/Build" "${JUCE_BINARY_DIR}/extras/Build")
add_subdirectory("${OPENTUNE_JUCE_DIR}/modules" "${JUCE_BINARY_DIR}/modules")

# Set ARA SDK path for JUCE before creating plugin targets (align with VST3 reference workspace)
if(EXISTS "${ARA_SDK_PATH}")
    juce_set_ara_sdk_path("${ARA_SDK_PATH}")
    message(STATUS "ARA SDK found at: ${ARA_SDK_PATH} (VST3 ARA support enabled)")
else()
    message(WARNING "ARA SDK not found at ${ARA_SDK_PATH}. VST3 ARA features will be disabled.")
    message(STATUS "To enable ARA, clone: git clone --recursive --branch releases/${OPENTUNE_ARA_SDK_VERSION} https://github.com/Celemony/ARA_SDK")
endif()

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

    set(DIRECTML_INCLUDE_DIR "")
    set(OPENTUNE_DEFAULT_DIRECTML_DLL "")
    if(EXISTS "${OPENTUNE_DIRECTML_PACKAGE_INCLUDE_DIR}/DirectML.h"
            AND EXISTS "${OPENTUNE_DIRECTML_PACKAGE_DLL}")
        set(DIRECTML_INCLUDE_DIR "${OPENTUNE_DIRECTML_PACKAGE_INCLUDE_DIR}")
        set(OPENTUNE_DEFAULT_DIRECTML_DLL "${OPENTUNE_DIRECTML_PACKAGE_DLL}")
    else()
        opentune_find_windows_um_include_file("DirectML.h" OPENTUNE_WINDOWS_SDK_DIRECTML_HEADER)
        get_filename_component(DIRECTML_INCLUDE_DIR "${OPENTUNE_WINDOWS_SDK_DIRECTML_HEADER}" DIRECTORY)
        set(OPENTUNE_DEFAULT_DIRECTML_DLL "C:/Windows/System32/DirectML.dll")
        message(STATUS "Vendored Microsoft.AI.DirectML package not found; using Windows SDK DirectML headers/libs.")
    endif()

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

    # CPU ONNX Runtime v1.24.4: headers + libs
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-win-x64-1.24.4")
    if(NOT DEFINED ONNXRUNTIME_ROOT OR "${ONNXRUNTIME_ROOT}" STREQUAL "")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime CPU root directory" FORCE)
    elseif(NOT EXISTS "${ONNXRUNTIME_ROOT}" AND EXISTS "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        message(STATUS "ONNXRUNTIME_ROOT reset to bundled dependency path: ${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}")
        set(ONNXRUNTIME_ROOT "${OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime CPU root directory" FORCE)
    endif()

elseif(APPLE)
    # ---- macOS: arm64-only, ONNX Runtime v1.24.4 with CoreML ----
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-osx-arm64-1.24.4")
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
    # DirectML ONNX Runtime v1.24.4: runtime DLLs + DML provider headers
    set(OPENTUNE_DEFAULT_ONNXRUNTIME_DML_ROOT "${OPENTUNE_THIRDPARTY_DIR}/onnxruntime-dml-1.24.4")
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

    # Shared providers DLL: prefer CPU build's copy
    set(ONNXRUNTIME_PROVIDERS_SHARED_DLL "")
    if(EXISTS "${ONNXRUNTIME_LIB_DIR}/onnxruntime_providers_shared.dll")
        set(ONNXRUNTIME_PROVIDERS_SHARED_DLL "${ONNXRUNTIME_LIB_DIR}/onnxruntime_providers_shared.dll")
    elseif(EXISTS "${ONNXRUNTIME_DML_RUNTIME_DIR}/onnxruntime_providers_shared.dll")
        set(ONNXRUNTIME_PROVIDERS_SHARED_DLL "${ONNXRUNTIME_DML_RUNTIME_DIR}/onnxruntime_providers_shared.dll")
    endif()

    # DML provider DLL: none in v1.24.4 (builtin DML in onnxruntime.dll)
    set(ONNXRUNTIME_PROVIDERS_DML_DLL "")

    if(NOT EXISTS "${ONNXRUNTIME_DLL}")
        message(FATAL_ERROR "ONNX Runtime DirectML DLL missing: ${ONNXRUNTIME_DLL}")
    endif()

    message(STATUS "ONNX Runtime version: 1.24.4")
    message(STATUS "ONNX Runtime CPU root: ${ONNXRUNTIME_ROOT}")
    message(STATUS "ONNX Runtime DML root: ${ONNXRUNTIME_DML_ROOT}")
    message(STATUS "ONNX Runtime DLL: ${ONNXRUNTIME_DLL}")
    message(STATUS "ONNX Runtime DML mode: builtin (DML integrated in onnxruntime.dll)")
    message(STATUS "DirectML version: ${OPENTUNE_DIRECTML_VERSION}")
    message(STATUS "DirectML include dir: ${DIRECTML_INCLUDE_DIR}")
    message(STATUS "DirectML DLL: ${DIRECTML_DLL}")
    message(STATUS "DirectX Agility version: ${OPENTUNE_D3D12_AGILITY_VERSION}")
    message(STATUS "DirectX Agility root: ${OPENTUNE_D3D12_AGILITY_ROOT}")
    message(STATUS "DirectX Agility SDK version token: ${OPENTUNE_D3D12_SDK_VERSION}")

elseif(APPLE)
    message(STATUS "ONNX Runtime version: 1.24.4 (macOS arm64)")
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
    VERSION "1.0.0"

    # Plugin Metadata
    PLUGIN_NAME "OpenTune"
    PLUGIN_DESCRIPTION "AI-powered Auto-Tune Plugin"
    PLUGIN_CATEGORY "Fx|Pitch Shift"
    
    # Formats (Standalone + VST3 dual-format build)
    FORMATS Standalone VST3

    # Enable ARA Extension (VST3 target only, but required here for SDK detection)
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
        <string>12.0</string>
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
)

# ==============================================================================
# Source Files
# ==============================================================================
target_sources(OpenTune
    PRIVATE
        Source/Services/F0ExtractionService.cpp
        Source/Services/F0ExtractionService.h
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
        Source/MaterializationStore.cpp
        Source/MaterializationStore.h
        Source/StandaloneArrangement.cpp
        Source/StandaloneArrangement.h

        # Inference Layer
        Source/Inference/RenderCache.cpp
        Source/Inference/VocoderFactory.h
        Source/Inference/VocoderFactory.cpp
        Source/Inference/DmlVocoder.h
        Source/Inference/DmlVocoder.cpp

        # AI Model Interfaces
        Source/Inference/IF0Extractor.h
        Source/Inference/VocoderInterface.h
        Source/Inference/OnnxVocoderBase.h
        Source/Inference/OnnxVocoderBase.cpp
        Source/Inference/DmlConfig.h

        # AI Model Implementations
        Source/Inference/RMVPEExtractor.cpp
        Source/Inference/RMVPEExtractor.h
        Source/Inference/PCNSFHifiGANVocoder.cpp
        Source/Inference/PCNSFHifiGANVocoder.h
        Source/Inference/ModelFactory.cpp
        Source/Inference/ModelFactory.h
        Source/Inference/F0InferenceService.cpp
        Source/Inference/VocoderDomain.cpp
        Source/Inference/VocoderInferenceService.cpp
        Source/Inference/VocoderRenderScheduler.cpp

        # DSP
        Source/DSP/ResamplingManager.cpp
        Source/DSP/ResamplingManager.h
        Source/DSP/MelSpectrogram.cpp
        Source/DSP/MelSpectrogram.h
        Source/DSP/ChromaKeyDetector.cpp
        Source/DSP/ChromaKeyDetector.h
        Source/DSP/CrossoverMixer.cpp
        Source/DSP/CrossoverMixer.h

        # UI Layer
        Source/Standalone/UI/UIColors.h
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
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.h
        Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp
        Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h
        Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp
        Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h
        Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp
        Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h
        Source/Standalone/UI/PianoRoll/InteractionState.cpp
        Source/Standalone/UI/PianoRoll/InteractionState.h
        Source/Standalone/UI/PlayheadOverlayComponent.cpp
        Source/Standalone/UI/PlayheadOverlayComponent.h
        Source/Standalone/UI/TimeConverter.cpp
        Source/Standalone/UI/TimeConverter.h
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
        Source/Standalone/UI/WaveformMipmap.cpp
        Source/Standalone/UI/WaveformMipmap.h
        Source/Standalone/UI/RippleOverlayComponent.h
        Source/Editor/AutoRenderOverlayComponent.h

        # Utilities
        Source/Utils/PitchCurve.cpp
        Source/Utils/PitchCurve.h
        Source/Utils/NoteGenerator.cpp
        Source/Utils/NoteGenerator.h
        Source/Utils/PianoKeyAudition.cpp
        Source/Utils/PianoKeyAudition.h
        Source/Utils/AppLogger.cpp
        Source/Utils/AppLogger.h
        Source/Utils/AppPreferences.cpp
        Source/Utils/AppPreferences.h
        Source/Utils/PianoRollVisualPreferences.h
        Source/Utils/PitchUtils.h
        Source/Utils/Note.h
        Source/Utils/WindowsDllSearchPath.cpp
        Source/Utils/OnnxRuntimeDelayLoadHook.cpp
        Source/Utils/ModelPathResolver.h
        Source/Utils/PresetManager.cpp
        Source/Utils/PresetManager.h
        Source/Utils/CpuFeatures.cpp
        Source/Utils/CpuFeatures.h
        Source/Utils/CpuBudgetManager.cpp
        Source/Utils/CpuBudgetManager.h
        Source/Utils/AccelerationDetector.cpp
        Source/Utils/AccelerationDetector.h
        Source/Utils/UndoManager.cpp
        Source/Utils/UndoManager.h
        Source/Utils/PianoRollEditAction.cpp
        Source/Utils/PianoRollEditAction.h
        Source/Utils/PlacementActions.cpp
        Source/Utils/PlacementActions.h
        Source/Utils/SilentGapDetector.cpp
        Source/Utils/SilentGapDetector.h
        Source/Utils/TimeCoordinate.h
)

# ==============================================================================
# Conditional Source Files per Format
# ==============================================================================
target_sources(OpenTune_Standalone
    PRIVATE
        Source/Utils/D3D12AgilityBootstrap.cpp
        Source/Standalone/EditorFactoryStandalone.cpp
        Source/Standalone/PluginEditor.cpp
        Source/Standalone/PluginEditor.h
)

target_compile_definitions(OpenTune_Standalone
    PRIVATE
        JucePlugin_Build_Standalone=1
        JucePlugin_Build_VST3=0
)

target_sources(OpenTune_VST3
    PRIVATE
        Source/Utils/D3D12AgilityBootstrap.cpp
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

# ARA files (conditionally compiled via #if JucePlugin_Enable_ARA in source files)
# These are only used in VST3 builds but we include them unconditionally
target_sources(OpenTune PRIVATE
    Source/ARA/OpenTuneDocumentController.cpp
    Source/ARA/OpenTuneDocumentController.h
    Source/ARA/OpenTunePlaybackRenderer.cpp
    Source/ARA/OpenTunePlaybackRenderer.h
    Source/ARA/VST3AraSession.cpp
    Source/ARA/VST3AraSession.h
)

# ==============================================================================
# Include Directories
# ==============================================================================
target_include_directories(OpenTune
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/r8brain-free-src-master
        ${ONNXRUNTIME_INCLUDE_DIR}
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
    target_link_libraries(OpenTune PRIVATE "${ONNXRUNTIME_LIB_DIR}/onnxruntime.lib")
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
    target_compile_definitions(OpenTune PUBLIC JUCE_ASIO=1)
    target_compile_definitions(OpenTune PRIVATE JUCE_USE_WINDOWS_MEDIA_FORMAT=1)
endif()

# ==============================================================================
# Platform-specific Settings
# ==============================================================================
if(WIN32)
    if(MSVC)
        target_compile_options(OpenTune PRIVATE /utf-8 /MP /wd4100 /wd4127)
        # Delay load onnxruntime.dll for Standalone
        target_link_options(OpenTune_Standalone PRIVATE "/DELAYLOAD:onnxruntime.dll")
        target_link_libraries(OpenTune_Standalone PRIVATE delayimp)
        target_compile_options(OpenTune_Standalone PRIVATE /wd4100)

        # Delay load onnxruntime.dll for VST3 plugin
        target_link_options(OpenTune_VST3 PRIVATE "/DELAYLOAD:onnxruntime.dll")
        target_link_libraries(OpenTune_VST3 PRIVATE delayimp)
        target_compile_options(OpenTune_VST3 PRIVATE /wd4100)
    endif()
elseif(APPLE)
    target_compile_options(OpenTune PRIVATE -Wall -Wno-unused-parameter)
    get_target_property(OPENTUNE_GENERATED_SOURCES_DIR OpenTune JUCE_GENERATED_SOURCES_DIRECTORY)
    set(OPENTUNE_STANDALONE_DEFAULT_PLIST "${OPENTUNE_GENERATED_SOURCES_DIR}/OpenTune_Standalone/Info.plist")
    set(OPENTUNE_STANDALONE_MERGED_PLIST "${OPENTUNE_GENERATED_SOURCES_DIR}/OpenTune_Standalone/Info-Standalone.plist")
    set(OPENTUNE_STANDALONE_PLIST_ENTRIES
        "    <key>LSMinimumSystemVersion</key>\n"
        "    <string>12.0</string>\n"
        "    <key>LSApplicationCategoryType</key>\n"
        "    <string>public.app-category.music</string>\n"
        "    <key>NSHumanReadableCopyright</key>\n"
        "    <string>Copyright (c) 2024-2026 DAYA. All rights reserved.</string>\n"
        "    <key>NSMicrophoneUsageDescription</key>\n"
        "    <string>OpenTune needs microphone access for real-time audio monitoring.</string>"
    )

    if(NOT EXISTS "${OPENTUNE_STANDALONE_DEFAULT_PLIST}")
        message(FATAL_ERROR "JUCE did not generate the standalone Info.plist at ${OPENTUNE_STANDALONE_DEFAULT_PLIST}")
    endif()

    file(READ "${OPENTUNE_STANDALONE_DEFAULT_PLIST}" OPENTUNE_STANDALONE_PLIST_CONTENT)
    string(REGEX REPLACE "</dict>[ \t\r\n]*</plist>"
        "${OPENTUNE_STANDALONE_PLIST_ENTRIES}\n</dict>\n</plist>"
        OPENTUNE_STANDALONE_PLIST_CONTENT
        "${OPENTUNE_STANDALONE_PLIST_CONTENT}"
    )

    if(NOT OPENTUNE_STANDALONE_PLIST_CONTENT MATCHES "NSMicrophoneUsageDescription")
        message(FATAL_ERROR "Failed to merge standalone-only macOS Info.plist metadata")
    endif()

    file(WRITE "${OPENTUNE_STANDALONE_MERGED_PLIST}" "${OPENTUNE_STANDALONE_PLIST_CONTENT}")

    # Set rpath so the app bundle can find the embedded dylib
    set_target_properties(OpenTune_Standalone PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
        MACOSX_BUNDLE_INFO_PLIST "${OPENTUNE_STANDALONE_MERGED_PLIST}"
    )

    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/docs"
    )
    target_sources(OpenTune_Standalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html")
endif()

# ==============================================================================
# Standalone Bundle Resources
# ==============================================================================
# Define model files
set(HIFIGAN_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx")
set(RMVPE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/rmvpe.onnx")

# --- ONNX Runtime library copying ---
if(WIN32)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime.dll"
        COMMENT "Copying ONNX Runtime DLL to Standalone..."
    )

    if(ONNXRUNTIME_PROVIDERS_SHARED_DLL)
        add_custom_command(TARGET OpenTune_Standalone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_PROVIDERS_SHARED_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime_providers_shared.dll"
            COMMENT "Copying ONNX Runtime shared providers DLL to Standalone..."
        )
    endif()

    if(ONNXRUNTIME_PROVIDERS_DML_DLL)
        add_custom_command(TARGET OpenTune_Standalone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_PROVIDERS_DML_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime_providers_dml.dll"
            COMMENT "Copying ONNX Runtime DML providers DLL to Standalone..."
        )
    endif()

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
            "${HIFIGAN_MODEL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/hifigan.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RMVPE_MODEL}"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/rmvpe.onnx"
        COMMENT "Copying AI models to Standalone..."
    )

elseif(APPLE)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.24.4.dylib"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Frameworks/"
        COMMENT "Copying ONNX Runtime dylib to app bundle Frameworks..."
    )

    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/hifigan.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RMVPE_MODEL}"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/models/rmvpe.onnx"
        COMMENT "Copying AI models to app bundle Resources..."
    )
endif()

if(APPLE)
    # macOS: docs go in Contents/Resources/docs/ (bundle standard)
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_en.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_en.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_ja.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_ja.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_ru.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_ru.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_es.html"
            "$<TARGET_BUNDLE_DIR:OpenTune_Standalone>/Contents/Resources/docs/UserGuide_es.html"
        COMMENT "Copying UserGuide translations to app bundle Resources..."
    )
else()
    # Windows: docs go alongside the executable
    add_custom_command(TARGET OpenTune_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_en.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_en.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_ja.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_ja.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_ru.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_ru.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide_es.html"
            "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide_es.html"
        COMMENT "Copying UserGuide translations to Standalone..."
    )
endif()

# ==============================================================================
# VST3-Specific POST_BUILD Commands
# ==============================================================================
# Copy runtime DLLs and AI models to VST3 plugin bundle
if(NOT JucePlugin_Build_Standalone)
    # ONNX Runtime DLL
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:OpenTune_VST3>/onnxruntime.dll"
        COMMENT "Copying ONNX Runtime DLL to VST3 plugin..."
    )

    # ONNX Runtime providers shared DLL
    if(ONNXRUNTIME_PROVIDERS_SHARED_DLL)
        add_custom_command(TARGET OpenTune_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_PROVIDERS_SHARED_DLL}"
                "$<TARGET_FILE_DIR:OpenTune_VST3>/onnxruntime_providers_shared.dll"
            COMMENT "Copying ONNX Runtime shared providers DLL to VST3 plugin..."
        )
    endif()

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

    # AI Models
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenTune_VST3>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HIFIGAN_MODEL}"
            "$<TARGET_FILE_DIR:OpenTune_VST3>/models/hifigan.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RMVPE_MODEL}"
            "$<TARGET_FILE_DIR:OpenTune_VST3>/models/rmvpe.onnx"
        COMMENT "Copying AI models to VST3 plugin..."
    )
endif()

# ==============================================================================
# Unit Tests
# ==============================================================================
option(OPENTUNE_BUILD_TESTS "Build unit tests" ON)

if(OPENTUNE_BUILD_TESTS)
    add_executable(OpenTuneTests
        Source/Utils/D3D12AgilityBootstrap.cpp
        Tests/TestMain.cpp
        Tests/TestEditorFactoryStub.cpp
    )

    target_include_directories(OpenTuneTests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests
            ${ONNXRUNTIME_INCLUDE_DIR}
            ${ARA_SDK_PATH}
    )
    if(WIN32)
        target_include_directories(OpenTuneTests
            PRIVATE
                ${ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR}
                ${D3D12_AGILITY_INCLUDE_DIR}
                ${D3D12_AGILITY_D3DX12_INCLUDE_DIR}
                ${D3D12_AGILITY_NATIVE_DIR}
                ${DIRECTML_INCLUDE_DIR}
        )
    endif()

    target_link_libraries(OpenTuneTests
        PRIVATE
            OpenTune
            juce::juce_core
            juce::juce_audio_basics
    )

    if(EXISTS "${ARA_SDK_PATH}")
        target_sources(OpenTuneTests
            PRIVATE
                ${ARA_SDK_PATH}/ARA_Library/PlugIn/ARAPlug.cpp
                ${ARA_SDK_PATH}/ARA_Library/Dispatch/ARAPlugInDispatch.cpp
                ${ARA_SDK_PATH}/ARA_Library/Utilities/ARAPitchInterpretation.cpp
                ${ARA_SDK_PATH}/ARA_Library/Utilities/ARAChannelArrangement.cpp
        )
    endif()

    target_compile_definitions(OpenTuneTests
        PRIVATE
            OPENTUNE_TEST_BUILD=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_REPORT_APP_USAGE=0
            # CRITICAL: Must match OpenTune_Standalone target so that
            # OpenTuneAudioProcessor's class layout (conditional members
            # behind #if !JucePlugin_Build_Standalone) is identical to the
            # SharedCode .lib that PluginProcessor.cpp was compiled into.
            # Mismatch causes ODR violation and silent field offset corruption.
            JucePlugin_Build_Standalone=1
            JucePlugin_Build_VST3=0
    )

    if(WIN32)
        target_compile_options(OpenTuneTests PRIVATE /utf-8 /MP /wd4100 /wd4127)

        add_custom_command(TARGET OpenTuneTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DLL}"
                "$<TARGET_FILE_DIR:OpenTuneTests>/onnxruntime.dll"
            COMMENT "Copying ONNX Runtime DLLs to unit tests..."
        )

        if(ONNXRUNTIME_PROVIDERS_SHARED_DLL)
            add_custom_command(TARGET OpenTuneTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_PROVIDERS_SHARED_DLL}"
                    "$<TARGET_FILE_DIR:OpenTuneTests>/onnxruntime_providers_shared.dll"
                COMMENT "Copying ONNX Runtime shared providers DLL to unit tests..."
            )
        endif()

        if(ONNXRUNTIME_PROVIDERS_DML_DLL)
            add_custom_command(TARGET OpenTuneTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_PROVIDERS_DML_DLL}"
                    "$<TARGET_FILE_DIR:OpenTuneTests>/onnxruntime_providers_dml.dll"
                COMMENT "Copying ONNX Runtime DML providers DLL to unit tests..."
            )
        endif()

        add_custom_command(TARGET OpenTuneTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DIRECTML_DLL}"
                "$<TARGET_FILE_DIR:OpenTuneTests>/DirectML.dll"
            COMMENT "Copying DirectML runtime DLL to unit tests..."
        )

        add_custom_command(TARGET OpenTuneTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:OpenTuneTests>/D3D12"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3D12_CORE_DLL}"
                "$<TARGET_FILE_DIR:OpenTuneTests>/D3D12/D3D12Core.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3D12_SDK_LAYERS_DLL}"
                "$<TARGET_FILE_DIR:OpenTuneTests>/D3D12/D3D12SDKLayers.dll"
            COMMENT "Copying DirectX Agility runtime to unit tests..."
        )
    endif()

    enable_testing()
    
    add_test(NAME OpenTuneCoreTests COMMAND OpenTuneTests)

    message(STATUS "Unit tests enabled - run with: OpenTuneTests")
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
