if(WIN32 AND NOT OPENTUNE_IOS)
    add_library(OpenTuneRealtimeAudit STATIC
        Tests/RealtimeAudit.cpp
        Tests/RealtimeWindowsHooks.cpp
    )
    target_include_directories(OpenTuneRealtimeAudit PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests"
    )
    target_compile_definitions(OpenTuneRealtimeAudit PUBLIC OPENTUNE_REALTIME_AUDIT=1)

    if(MSVC)
        target_compile_options(OpenTuneRealtimeAudit INTERFACE
            "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/Source/Utils/RealtimeAuditScope.h>")
    else()
        target_compile_options(OpenTuneRealtimeAudit INTERFACE
            "$<$<COMPILE_LANGUAGE:CXX>:-include;${CMAKE_CURRENT_SOURCE_DIR}/Source/Utils/RealtimeAuditScope.h>")
    endif()

    target_link_libraries(OpenTune PUBLIC OpenTuneRealtimeAudit)
    target_link_libraries(OpenTuneStandalone PUBLIC OpenTuneRealtimeAudit)

    set(OPENTUNE_REALTIME_ARA_PLUGIN_SOURCES
        "${ARA_SDK_PATH}/ARA_Library/Dispatch/ARAPlugInDispatch.cpp"
        "${ARA_SDK_PATH}/ARA_Library/PlugIn/ARAPlug.cpp"
        "${ARA_SDK_PATH}/ARA_Library/Utilities/ARAPitchInterpretation.cpp"
        "${ARA_SDK_PATH}/ARA_Library/Utilities/ARAChannelFormat.cpp"
    )

    foreach(realtime_contract IN ITEMS AuditionRealtimeTests PlaybackCacheRealtimeTests)
        add_executable(OpenTune${realtime_contract}
            Tests/${realtime_contract}.cpp
            ${OPENTUNE_REALTIME_ARA_PLUGIN_SOURCES})
        target_include_directories(OpenTune${realtime_contract} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Source ${ARA_SDK_PATH})
        target_link_libraries(OpenTune${realtime_contract} PRIVATE
            OpenTune OpenTuneResources juce::juce_audio_basics juce::juce_events)
        if(MSVC)
            target_compile_options(OpenTune${realtime_contract} PRIVATE /utf-8 /EHsc)
        endif()
        add_test(NAME ${realtime_contract} COMMAND OpenTune${realtime_contract})
        set_tests_properties(${realtime_contract} PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "OPENTUNE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/${realtime_contract}")
    endforeach()

    add_executable(RealtimeCallbackTests
        Tests/RealtimeCallbackTests.cpp
        Tests/HeadlessEditorFactory.cpp
        "${ARA_SDK_PATH}/ARA_Library/Dispatch/ARAHostDispatch.cpp"
        ${OPENTUNE_REALTIME_ARA_PLUGIN_SOURCES}
    )
    target_include_directories(RealtimeCallbackTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "${ONNXRUNTIME_INCLUDE_DIR}"
        "${ARA_SDK_PATH}"
        "${ARA_SDK_PATH}/ARA_Library"
        "${ARA_SDK_PATH}/ARA_API"
    )
    target_compile_definitions(RealtimeCallbackTests PRIVATE JucePlugin_Build_Standalone=0)
    target_link_libraries(RealtimeCallbackTests PRIVATE
        OpenTune
        OpenTuneResources
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_events
    )
    if(MSVC)
        target_compile_options(RealtimeCallbackTests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME RealtimeCallbackTests COMMAND RealtimeCallbackTests)
    set_tests_properties(RealtimeCallbackTests PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "OPENTUNE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/RealtimeCallbackTests"
    )

    add_executable(StandaloneRealtimeCallbackTests
        Tests/RealtimeCallbackTests.cpp
        Tests/HeadlessEditorFactory.cpp)
    target_include_directories(StandaloneRealtimeCallbackTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "${ONNXRUNTIME_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone")
    target_compile_definitions(StandaloneRealtimeCallbackTests PRIVATE
        JucePlugin_Build_Standalone=1)
    target_link_libraries(StandaloneRealtimeCallbackTests PRIVATE
        OpenTuneStandalone
        OpenTuneResources
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_events
    )
    if(MSVC)
        target_compile_options(StandaloneRealtimeCallbackTests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME StandaloneRealtimeCallbackTests COMMAND StandaloneRealtimeCallbackTests)
    set_tests_properties(StandaloneRealtimeCallbackTests PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "OPENTUNE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/StandaloneRealtimeCallbackTests"
    )

    add_executable(RealtimeVST3Tests
        Tests/RealtimeVST3Tests.cpp
        Tests/HeadlessEditorFactory.cpp
        Source/Utils/Vst3ModulePin.cpp)
    target_include_directories(RealtimeVST3Tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "$<TARGET_PROPERTY:OpenTune,INCLUDE_DIRECTORIES>"
        "${OPENTUNE_JUCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK"
        "${ARA_SDK_PATH}"
        "${ARA_SDK_PATH}/ARA_Library"
        "${ARA_SDK_PATH}/ARA_API"
    )
    target_link_libraries(RealtimeVST3Tests PRIVATE
        OpenTune
        OpenTuneResources
        juce::juce_audio_processors
        juce::juce_events
        juce::juce_audio_plugin_client_VST3
    )
    if(MSVC)
        target_compile_options(RealtimeVST3Tests PRIVATE /utf-8 /EHsc /bigobj)
    endif()
    add_test(NAME RealtimeVST3Tests COMMAND RealtimeVST3Tests)
    set_tests_properties(RealtimeVST3Tests PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "OPENTUNE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/RealtimeVST3Tests"
    )

    foreach(realtime_target IN ITEMS
            RealtimeCallbackTests
            StandaloneRealtimeCallbackTests
            RealtimeVST3Tests
            OpenTuneAuditionRealtimeTests
            OpenTunePlaybackCacheRealtimeTests)
        add_custom_command(TARGET ${realtime_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DLL}"
                "$<TARGET_FILE_DIR:${realtime_target}>/${OPENTUNE_ORT_DLL_NAME}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DIRECTML_DLL}"
                "$<TARGET_FILE_DIR:${realtime_target}>/DirectML.dll")
    endforeach()
endif()
