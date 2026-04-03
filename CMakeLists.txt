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
# JUCE Configuration
# ==============================================================================
set(OPENTUNE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE-master")
add_subdirectory("${OPENTUNE_JUCE_DIR}")

# ==============================================================================
# ONNX Runtime Configuration
# ==============================================================================
if(WIN32)
    # ---- Windows: DirectML + D3D12 Agility SDK (NuGet auto-download) ----

    set(OPENTUNE_DIRECTML_VERSION "1.15.4" CACHE STRING "Microsoft.AI.DirectML NuGet version")
    set(OPENTUNE_D3D12_AGILITY_VERSION "1.619.1" CACHE STRING "Microsoft.Direct3D.D3D12 NuGet version")

    if(OPENTUNE_DIRECTML_VERSION VERSION_LESS "1.13.0")
        message(FATAL_ERROR "DirectML version must be >= 1.13.0")
    endif()
    if(OPENTUNE_D3D12_AGILITY_VERSION VERSION_LESS "1.614.0")
        message(FATAL_ERROR "DirectX Agility SDK version must be >= 1.614.0")
    endif()

    function(opentune_fetch_nuget_package package_id package_version out_dir)
        string(TOLOWER "${package_id}" package_id_lower)
        set(nuget_cache_dir "${CMAKE_CURRENT_BINARY_DIR}/nuget")
        set(package_dir "${nuget_cache_dir}/${package_id_lower}.${package_version}")
        set(nupkg_path "${nuget_cache_dir}/${package_id_lower}.${package_version}.nupkg")
        set(marker_path "${package_dir}/.extracted")

        file(MAKE_DIRECTORY "${nuget_cache_dir}")

        if(NOT EXISTS "${marker_path}")
            if(NOT EXISTS "${nupkg_path}")
                set(download_url "https://api.nuget.org/v3-flatcontainer/${package_id_lower}/${package_version}/${package_id_lower}.${package_version}.nupkg")
                file(DOWNLOAD "${download_url}" "${nupkg_path}" STATUS download_status SHOW_PROGRESS TLS_VERIFY ON)
                list(GET download_status 0 download_code)
                if(NOT download_code EQUAL 0)
                    list(GET download_status 1 download_message)
                    message(FATAL_ERROR "Failed to download ${package_id} ${package_version}: ${download_message}")
                endif()
            endif()

            file(REMOVE_RECURSE "${package_dir}")
            file(MAKE_DIRECTORY "${package_dir}")
            file(ARCHIVE_EXTRACT INPUT "${nupkg_path}" DESTINATION "${package_dir}")
            file(WRITE "${marker_path}" "ok")
        endif()

        set(${out_dir} "${package_dir}" PARENT_SCOPE)
    endfunction()

    opentune_fetch_nuget_package("Microsoft.AI.DirectML" "${OPENTUNE_DIRECTML_VERSION}" OPENTUNE_DIRECTML_ROOT)
    opentune_fetch_nuget_package("Microsoft.Direct3D.D3D12" "${OPENTUNE_D3D12_AGILITY_VERSION}" OPENTUNE_D3D12_AGILITY_ROOT)

    set(DIRECTML_INCLUDE_DIR "${OPENTUNE_DIRECTML_ROOT}/include")
    set(DIRECTML_DLL "${OPENTUNE_DIRECTML_ROOT}/bin/x64-win/DirectML.dll")
    set(DIRECTML_LIB "${OPENTUNE_DIRECTML_ROOT}/bin/x64-win/DirectML.lib")

    set(D3D12_AGILITY_BIN_DIR "${OPENTUNE_D3D12_AGILITY_ROOT}/build/native/bin/x64")
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
    if(NOT EXISTS "${DIRECTML_LIB}")
        message(FATAL_ERROR "DirectML import library missing: ${DIRECTML_LIB}")
    endif()
    if(NOT EXISTS "${D3D12_CORE_DLL}")
        message(FATAL_ERROR "D3D12Core.dll missing from Agility SDK: ${D3D12_CORE_DLL}")
    endif()
    if(NOT EXISTS "${D3D12_SDK_LAYERS_DLL}")
        message(FATAL_ERROR "D3D12SDKLayers.dll missing from Agility SDK: ${D3D12_SDK_LAYERS_DLL}")
    endif()

    # CPU ONNX Runtime v1.24.4: headers + libs
    set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-win-x64-1.24.4" CACHE PATH "ONNX Runtime CPU root directory")

elseif(APPLE)
    # ---- macOS: arm64-only, ONNX Runtime v1.24.4 with CoreML ----
    set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-osx-arm64-1.24.4"
        CACHE PATH "ONNX Runtime root directory")

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
    set(ONNXRUNTIME_DML_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-dml-1.24.4" CACHE PATH "ONNX Runtime DirectML package root")
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
    message(STATUS "DirectML DLL: ${DIRECTML_DLL}")
    message(STATUS "DirectX Agility version: ${OPENTUNE_D3D12_AGILITY_VERSION}")
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
    
    # Formats (Standalone only)
    FORMATS Standalone
    
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
)

# ==============================================================================
# Binary Data (Resources)
# ==============================================================================
juce_add_binary_data(OpenTuneResources
    SOURCES
        Resources/Fonts/HONORSansCN-Medium.ttf
)

# ==============================================================================
# Source Files
# ==============================================================================
target_sources(OpenTune
    PRIVATE
        # Standalone specific files - Must be compiled for OpenTune target
        Source/Host/HostIntegrationStandalone.cpp
        Source/Standalone/EditorFactoryStandalone.cpp
        Source/Standalone/PluginEditor.cpp
        Source/Standalone/PluginEditor.h
        Source/Services/F0ExtractionService.cpp
        Source/Services/F0ExtractionService.h
        Source/Host/HostIntegration.h

        # Core Plugin Files
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h

        # Inference Layer
        Source/Inference/RenderCache.cpp
        Source/Inference/VocoderFactory.h
        Source/Inference/VocoderFactory.cpp
        Source/Inference/DmlVocoder.h
        Source/Inference/DmlVocoder.cpp

        # AI Model Interfaces
        Source/Inference/IF0Extractor.h
        Source/Inference/VocoderInterface.h
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
        Source/DSP/ScaleInference.cpp
        Source/DSP/ScaleInference.h

        # Third Party
        ThirdParty/r8brain-free-src-master/fft/pffft_double.c

        # UI Layer
        Source/Standalone/UI/UIColors.h
        Source/Standalone/UI/UiText.h
        Source/Standalone/UI/ThemeTokens.h
        Source/Standalone/UI/OpenTuneLookAndFeel.h
        Source/Standalone/UI/BlueBreezeTheme.h
        Source/Standalone/UI/BlueBreezeLookAndFeel.h
        Source/Standalone/UI/BlueBreezeLookAndFeel.cpp
        Source/Standalone/UI/DarkBlueGreyTheme.h
        Source/Standalone/UI/DarkBlueGreyLookAndFeel.h
        Source/Standalone/UI/DarkBlueGreyLookAndFeel.cpp
        Source/Standalone/UI/AuroraTheme.h
        Source/Standalone/UI/AuroraLookAndFeel.h
        Source/Standalone/UI/AuroraLookAndFeel.cpp
        Source/Standalone/UI/ToolIds.h
        Source/Standalone/UI/ToolbarIcons.h
        Source/Standalone/UI/ParameterPanel.cpp
        Source/Standalone/UI/ParameterPanel.h
        Source/Standalone/UI/TimelineComponent.cpp
        Source/Standalone/UI/TimelineComponent.h
        Source/Standalone/UI/PianoRollComponent.cpp
        Source/Standalone/UI/PianoRollComponent.h
        Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.cpp
        Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.h
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
        Source/Standalone/UI/PianoRoll/PianoRollRenderer.h
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
        Source/Standalone/UI/AutoRenderOverlayComponent.h

        # Utilities
        Source/Utils/PitchCurve.cpp
        Source/Utils/PitchCurve.h
        Source/Utils/NoteGenerator.cpp
        Source/Utils/NoteGenerator.h
        Source/Utils/UndoAction.cpp
        Source/Utils/UndoAction.h
        Source/Utils/AppLogger.cpp
        Source/Utils/AppLogger.h
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
        Source/Utils/SimdAccelerator.cpp
        Source/Utils/SimdAccelerator.h
        Source/Utils/SilentGapDetector.cpp
        Source/Utils/SilentGapDetector.h
        Source/Utils/TimeCoordinate.h
        Source/Utils/DmlRuntimeVerifier.cpp
        Source/Utils/DmlRuntimeVerifier.h
        Source/Utils/D3D12AgilityBootstrap.cpp
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
            "${DIRECTML_LIB}"
            d3d12
            dxgi
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
    target_compile_definitions(OpenTune PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(OpenTune PRIVATE /utf-8 /MP /wd4100 /wd4127)
        # Delay load onnxruntime.dll for Standalone
        target_link_options(OpenTune_Standalone PRIVATE "/DELAYLOAD:onnxruntime.dll")
        target_link_libraries(OpenTune_Standalone PRIVATE delayimp)
        target_compile_options(OpenTune_Standalone PRIVATE /wd4100)
    endif()
elseif(APPLE)
    target_compile_options(OpenTune PRIVATE -Wall -Wno-unused-parameter)
    # Set rpath so the app bundle can find the embedded dylib
    set_target_properties(OpenTune_Standalone PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
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

add_custom_command(TARGET OpenTune_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html"
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide.html"
    COMMENT "Copying UserGuide.html to Standalone..."
)

# ==============================================================================
# Unit Tests
# ==============================================================================
option(OPENTUNE_BUILD_TESTS "Build unit tests" ON)

if(OPENTUNE_BUILD_TESTS)
    add_executable(OpenTuneTests
        Tests/TestMain.cpp
    )

    target_include_directories(OpenTuneTests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests
            ${ONNXRUNTIME_INCLUDE_DIR}
    )
    if(WIN32)
        target_include_directories(OpenTuneTests
            PRIVATE
                ${ONNXRUNTIME_DML_GENERATED_INCLUDE_DIR}
        )
    endif()

    target_link_libraries(OpenTuneTests
        PRIVATE
            OpenTune
            juce::juce_core
            juce::juce_audio_basics
    )

    target_compile_definitions(OpenTuneTests
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_REPORT_APP_USAGE=0
    )

    if(WIN32)
        target_compile_definitions(OpenTuneTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
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
message(STATUS "Plugin Formats:   Standalone Only")
message(STATUS "========================================")
message(STATUS "")
