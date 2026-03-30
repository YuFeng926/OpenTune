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
# JUCE Configuration
# ==============================================================================
set(OPENTUNE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE-master")
add_subdirectory("${OPENTUNE_JUCE_DIR}")

# ==============================================================================
# ONNX Runtime Configuration
# ==============================================================================
set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-win-x64-1.17.3" CACHE PATH "ONNX Runtime root directory")
set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")

# Verify ONNX Runtime exists
if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime not found at ${ONNXRUNTIME_ROOT}")
endif()

message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_ROOT}")

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
        Source/Inference/InferenceManager.cpp
        Source/Inference/InferenceManager.h
        Source/Inference/RenderCache.cpp
        Source/Inference/RenderCache.h
        Source/Inference/RenderingManager.cpp
        Source/Inference/RenderingManager.h

        # AI Model Interfaces
        Source/Inference/IF0Extractor.h
        Source/Inference/IVocoder.h

        # AI Model Implementations
        Source/Inference/RMVPEExtractor.cpp
        Source/Inference/RMVPEExtractor.h
        Source/Inference/HifiGANVocoder.cpp
        Source/Inference/HifiGANVocoder.h
        Source/Inference/PCNSFHifiGANVocoder.cpp
        Source/Inference/PCNSFHifiGANVocoder.h
        Source/Inference/ModelFactory.cpp
        Source/Inference/ModelFactory.h

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
        Source/Standalone/UI/MainControlPanel.cpp
        Source/Standalone/UI/MainControlPanel.h
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
        Source/Standalone/UI/PianoRoll/PianoRollCoordinateSystem.h
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
        Source/Utils/GpuDetector.cpp
        Source/Utils/GpuDetector.h
        Source/Utils/SimdAccelerator.cpp
        Source/Utils/SimdAccelerator.h
        Source/Utils/SilentGapDetector.cpp
        Source/Utils/SilentGapDetector.h
        Source/Utils/TimeCoordinate.h
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
target_link_libraries(OpenTune
    PRIVATE
        "${ONNXRUNTIME_LIB_DIR}/onnxruntime.lib"
)

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
        JUCE_ASIO=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1

    PRIVATE
        # OpenTune specific
        OPENTUNE_VERSION="${PROJECT_VERSION}"
        ORT_API_MANUAL_INIT
        # Audio format support (import)
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_MP3AUDIOFORMAT=1
)

if(WIN32)
    target_compile_definitions(OpenTune PRIVATE JUCE_USE_WINDOWS_MEDIA_FORMAT=1)
endif()

# ==============================================================================
# Platform-specific Settings
# ==============================================================================
if(WIN32)
    target_compile_definitions(OpenTune PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(OpenTune PRIVATE /utf-8 /MP /wd4100 /wd4127)
    
    # Delay load onnxruntime.dll for Standalone
    if(MSVC)
        target_link_options(OpenTune_Standalone PRIVATE "/DELAYLOAD:onnxruntime.dll")
        target_link_libraries(OpenTune_Standalone PRIVATE delayimp)
        target_compile_options(OpenTune_Standalone PRIVATE /wd4100)
    endif()
endif()

# ==============================================================================
# Standalone Bundle Resources
# ==============================================================================
# Define model files
set(HIFIGAN_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx")
set(RMVPE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/rmvpe.onnx")

add_custom_command(TARGET OpenTune_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_LIB_DIR}/onnxruntime.dll"
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_LIB_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/onnxruntime_providers_shared.dll"
    COMMENT "Copying ONNX Runtime DLL to Standalone..."
)

add_custom_command(TARGET OpenTune_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/models"
    COMMENT "Creating Standalone models directory..."
)

add_custom_command(TARGET OpenTune_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HIFIGAN_MODEL}"
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/hifigan.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RMVPE_MODEL}"
        "$<TARGET_FILE_DIR:OpenTune_Standalone>/models/rmvpe.onnx"
    COMMENT "Copying AI models to Standalone..."
)

# ==============================================================================
# Tests and Tools disabled for standalone build
# ==============================================================================
# Note: Test executables removed - source files not present in repository

# ============================================================================== 
# CTest 测试注册 - disabled
# ============================================================================== 
# enable_testing()

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
