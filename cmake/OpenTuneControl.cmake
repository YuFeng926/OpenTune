# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Local control API of OpenTune Standalone (OTCP v1, loopback only).
#
# Included at the end of the top-level CMakeLists.txt. Everything below is inside
# if(OPENTUNE_ENABLE_CONTROL_API): with the option OFF (the default) this file adds no source,
# definition, library or test, so the build is the same as without it.
#
# The control sources are compiled into the Standalone SharedCode target (OpenTuneStandalone) only;
# they must not reference symbols that exist only in the application wrapper target.

if(OPENTUNE_ENABLE_CONTROL_API)
    # OPENTUNE_IOS is set by the top-level CMakeLists.txt before this file is included.
    if(OPENTUNE_IOS)
        message(FATAL_ERROR
            "OPENTUNE_ENABLE_CONTROL_API supports the Standalone application on Windows and macOS only; "
            "it is not available for iPadOS builds. Configure with -DOPENTUNE_ENABLE_CONTROL_API=OFF.")
    endif()

    # Explicit, ordered source list, in blocks by topic.
    # Channel: run-time options, path rules, CSPRNG, JSON, protocol framing, sockets, discovery file and
    # the listener with authentication and limits. Standard library and operating system only: the unit
    # tests compile these files on their own.
    set(OPENTUNE_CONTROL_CHANNEL_SOURCES
        Source/Control/ControlApiOptions.cpp
        Source/Control/ControlApiOptions.h
        Source/Control/ControlPathPolicy.cpp
        Source/Control/ControlPathPolicy.h
        Source/Control/SecureRandom.cpp
        Source/Control/SecureRandom.h
        Source/Control/ControlJson.cpp
        Source/Control/ControlJson.h
        Source/Control/ControlProtocol.cpp
        Source/Control/ControlProtocol.h
        Source/Control/ControlSocket.cpp
        Source/Control/ControlSocket.h
        Source/Control/ControlDiscovery.cpp
        Source/Control/ControlDiscovery.h
        Source/Control/ControlServer.cpp
        Source/Control/ControlServer.h
    )
    set(OPENTUNE_CONTROL_SOURCES
        ${OPENTUNE_CONTROL_CHANNEL_SOURCES}
        # Service, processor start/stop and data-directory redirection: the service facade, the
        # message-thread marshalling with its liveness token, the editor bridge interface and
        # OPENTUNE_DATA_DIR. These and all the files below use JUCE and the processor.
        Source/Control/ControlDataDirectory.cpp
        Source/Control/ControlDataDirectory.h
        Source/Control/ControlMessageThread.cpp
        Source/Control/ControlMessageThread.h
        Source/Control/ControlUiBridge.h
        Source/Control/ControlService.cpp
        Source/Control/ControlService.h
        # Dispatcher and the read methods: handles, revision tokens and cursors (standard library only),
        # the request handler with its method table and error mapping, the helpers the operations share
        # (parameter checks, handle resolution, the world revision vector, asOf) and the session,
        # project and content reads.
        Source/Control/ContentHandles.cpp
        Source/Control/ContentHandles.h
        Source/Control/ControlDispatcher.cpp
        Source/Control/ControlDispatcher.h
        Source/Control/Ops/OpsRegistry.cpp
        Source/Control/Ops/OpsRegistry.h
        Source/Control/Ops/OpsSupport.cpp
        Source/Control/Ops/OpsSupport.h
        Source/Control/Ops/SessionOps.cpp
        Source/Control/Ops/SessionOps.h
        Source/Control/Ops/ProjectReadOps.cpp
        Source/Control/Ops/ProjectReadOps.h
        Source/Control/Ops/ContentReadOps.cpp
        Source/Control/Ops/ContentReadOps.h
        # The rest of the reads: content details, preferences and the log, the wire forms they share
        # with the other reads, and the redacting log tail (standard library only).
        Source/Control/Ops/OpsJson.cpp
        Source/Control/Ops/OpsJson.h
        Source/Control/Ops/ContentDetailsOps.cpp
        Source/Control/Ops/ContentDetailsOps.h
        Source/Control/Ops/AppReadOps.cpp
        Source/Control/Ops/AppReadOps.h
        Source/Control/ControlLogTail.cpp
        Source/Control/ControlLogTail.h
        # Jobs, import, export and the editor bridge: the jobs registry (tick on the message thread, long
        # poll on a thread of its own, retention), the write envelope and the busy checks that every write
        # shares, job.get and job.cancel, and import.start. The editor's side of the bridge is below.
        Source/Control/ControlJobs.cpp
        Source/Control/ControlJobs.h
        Source/Control/Ops/WriteEnvelope.cpp
        Source/Control/Ops/WriteEnvelope.h
        Source/Control/Ops/JobOps.cpp
        Source/Control/Ops/JobOps.h
        Source/Control/Ops/ImportOps.cpp
        Source/Control/Ops/ImportOps.h
        # export.start: file identity and the final rename (operating system only), and the export job.
        Source/Control/ControlFileIdentity.cpp
        Source/Control/ControlFileIdentity.h
        Source/Control/Ops/ExportOps.cpp
        Source/Control/Ops/ExportOps.h
        # Undo and redo, key and AUTO: the undo entries of the API (ControlOriginAction) and what the instance
        # knows about OpenTune's history (ControlHistory), undo entries built from snapshots as the piano roll
        # builds them (ContentEditTransaction), what content edits share, edit.getHistory, edit.undo,
        # edit.redo, content.setKey and content.autoTune.
        Source/Control/Edit/ControlOriginAction.cpp
        Source/Control/Edit/ControlOriginAction.h
        Source/Control/Edit/ContentEditTransaction.cpp
        Source/Control/Edit/ContentEditTransaction.h
        Source/Control/Ops/EditSupport.cpp
        Source/Control/Ops/EditSupport.h
        Source/Control/Ops/UndoOps.cpp
        Source/Control/Ops/UndoOps.h
        Source/Control/Ops/ContentTuneOps.cpp
        Source/Control/Ops/ContentTuneOps.h
        # Note edits and the pitch shift: the steps of OpenTune's pitch shifts that the revision tokens leave
        # out, the correction rebuild of the piano roll and its SNAP, content.editNotes and
        # content.setPitchShift.
        Source/Control/Edit/PitchShiftLedger.cpp
        Source/Control/Edit/PitchShiftLedger.h
        Source/Control/Edit/NoteCorrection.cpp
        Source/Control/Edit/NoteCorrection.h
        Source/Control/Ops/ContentEditOps.cpp
        Source/Control/Ops/ContentEditOps.h
        # Time grid: the handle moves of OpenTune's time tool without the piano roll (TimeGridOps),
        # content.shiftTiming and content.editTimeGrid (seed as a job, moveHandles).
        Source/Control/Edit/TimeGridOps.cpp
        Source/Control/Edit/TimeGridOps.h
        Source/Control/Ops/TimingOps.cpp
        Source/Control/Ops/TimingOps.h
        # Project files: project.save, project.saveAs, project.open (jobs on the editor's project session and worker pool,
        # reached through the bridge) and project.clearRecent.
        Source/Control/Ops/ProjectOps.cpp
        Source/Control/Ops/ProjectOps.h
        # Transport and tracks: transport.control, transport.setTempo, transport.setLoop, and track.set, track.add,
        # track.duplicate, track.delete (the track helpers are shared with the clip methods).
        Source/Control/Ops/TrackSupport.cpp
        Source/Control/Ops/TrackSupport.h
        Source/Control/Ops/TransportOps.cpp
        Source/Control/Ops/TransportOps.h
        Source/Control/Ops/TrackOps.cpp
        Source/Control/Ops/TrackOps.h
        # Dialogs and onboarding: ui.getState and ui.dismiss (ui.bridge; ui.setSelection is in the selection block),
        # on the editor's modal windows and onboarding overlay reached through the bridge.
        Source/Control/Ops/UiOps.cpp
        Source/Control/Ops/UiOps.h
        # Preferences and audio device: prefs.set (a job of kind prefsApply for a change of the editing scheme) and
        # audio.get / audio.set (the steps of the audio settings page on the editor's device manager, with rollback).
        Source/Control/Ops/PrefsOps.cpp
        Source/Control/Ops/PrefsOps.h
        Source/Control/Ops/AudioDeviceOps.cpp
        Source/Control/Ops/AudioDeviceOps.h
        # Arrangement: arrangement.edit (move, trim, gain, fades, split, merge, delete) and
        # arrangement.copy, through the arrangement view's setters, processor functions and undo actions.
        Source/Control/Ops/ArrangementOps.cpp
        Source/Control/Ops/ArrangementOps.h
        # Selection: ui.setSelection, through the editor's bridge, with the effects it predicts and observes.
        Source/Control/Ops/SelectionOps.cpp
        Source/Control/Ops/SelectionOps.h
        # Workspace: ui.setWorkspace (capability ui.workspace), the two view buttons of the transport bar through the bridge.
        Source/Control/Ops/WorkspaceOps.cpp
        Source/Control/Ops/WorkspaceOps.h
        # Quit: app.quit (capability app.quit), the close button of the window through the bridge, after the reply.
        Source/Control/Ops/AppQuitOps.cpp
        Source/Control/Ops/AppQuitOps.h
        # Preference options: the vocoder weights and pitch models that prefs.get offers (processing.options) and prefs.set
        # accepts, and the vocoder state and processor calls that the prefs.set job waits on and makes.
        Source/Control/Ops/PrefsModels.cpp
        Source/Control/Ops/PrefsModels.h
        # Clip ids: how the methods that make clips (import, split, merge, copy, track duplicate) get past the ids that
        # a project that was opened leaves in use ahead of OpenTune's clip id counter.
        Source/Control/Ops/ClipIds.cpp
        Source/Control/Ops/ClipIds.h
    )

    target_sources(OpenTuneStandalone PRIVATE ${OPENTUNE_CONTROL_SOURCES})
    # The editor's side of the bridge belongs to the application wrapper, like the editor itself
    # (OPENTUNE_STANDALONE_APP_SOURCES in CMakeLists.txt); the headless tests use a bridge of their own.
    target_sources(OpenTuneStandalone_Standalone PRIVATE
        Source/Standalone/ControlEditorBridge.cpp
        Source/Standalone/ControlEditorBridge.h
        Source/Standalone/ControlEditorBridgeProject.cpp
        Source/Standalone/ControlEditorBridgeTracks.cpp
        # Dialogs and onboarding: the modal windows, closing them as Escape does, the overlay, the scale of the UI.
        Source/Standalone/ControlEditorBridgeModals.cpp
        Source/Standalone/ControlEditorBridgeColour.h
        # Preferences and audio device (the editor applies the preferences; the ASIO restore of a device switch).
        Source/Standalone/ControlEditorBridgePrefs.cpp
        # Selection: a click and a double click on a clip, through the arrangement view's hook.
        Source/Standalone/ControlEditorBridgeSelection.cpp
        # Workspace: the arrangement view or the piano roll, as the view buttons of the transport bar switch them.
        Source/Standalone/ControlEditorBridgeWorkspace.cpp
    )
    # PUBLIC, so that every target that compiles or links the SharedCode sees the same class layouts.
    target_compile_definitions(OpenTuneStandalone PUBLIC OPENTUNE_ENABLE_CONTROL_API=1)
    if(WIN32)
        # Winsock for the listener, BCrypt for the system CSPRNG.
        target_link_libraries(OpenTuneStandalone PRIVATE ws2_32 bcrypt)
    endif()

    # Build identity published in the discovery file (PROTOCOL.md section 5.3). By default the commit
    # and its exact tag are read from git each time CMake configures; builds that are distributed pass
    # the values explicitly. Only ControlService.cpp sees them, so a new commit recompiles one file.
    set(OPENTUNE_CONTROL_BUILD_COMMIT "" CACHE STRING
        "Commit id the control API publishes (40 hexadecimal digits; empty: git HEAD at configure time)")
    set(OPENTUNE_CONTROL_COMMIT_TAG "" CACHE STRING
        "Version-control tag of that commit (empty: the exact git tag at configure time, if any)")
    set(OPENTUNE_CONTROL_BUILD_TAG "" CACHE STRING "Distribution label of this binary (empty: none)")
    set(OPENTUNE_CONTROL_SOURCE_URL "https://github.com/YuFeng926/OpenTune" CACHE STRING
        "Where the corresponding source code of this build can be obtained (https URL)")
    set(_opentune_control_commit "${OPENTUNE_CONTROL_BUILD_COMMIT}")
    set(_opentune_control_commit_tag "${OPENTUNE_CONTROL_COMMIT_TAG}")
    find_package(Git QUIET)
    if(GIT_FOUND AND _opentune_control_commit STREQUAL "")
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _opentune_control_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_opentune_control_commit_tag STREQUAL "")
            execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE _opentune_control_commit_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
    endif()
    string(TOLOWER "${_opentune_control_commit}" _opentune_control_commit)
    if(NOT _opentune_control_commit MATCHES "^[0-9a-f]+$")
        set(_opentune_control_commit "")
    endif()
    string(LENGTH "${_opentune_control_commit}" _opentune_control_commit_length)
    if(NOT _opentune_control_commit_length EQUAL 40)
        message(STATUS "Control API: the commit id is unknown; the discovery file reports 40 zeros")
        set(_opentune_control_commit "0000000000000000000000000000000000000000")
    endif()
    set_source_files_properties(Source/Control/ControlService.cpp PROPERTIES COMPILE_DEFINITIONS
        "OPENTUNE_CONTROL_BUILD_COMMIT=\"${_opentune_control_commit}\";OPENTUNE_CONTROL_COMMIT_TAG=\"${_opentune_control_commit_tag}\";OPENTUNE_CONTROL_BUILD_TAG=\"${OPENTUNE_CONTROL_BUILD_TAG}\";OPENTUNE_CONTROL_SOURCE_URL=\"${OPENTUNE_CONTROL_SOURCE_URL}\"")

    if(OPENTUNE_BUILD_TESTS)
        # Unit tests of the channel: standard library and the channel sources only.
        # Handles and tokens are standard library only as well.
        set(OPENTUNE_CONTROL_TEST_SOURCES ${OPENTUNE_CONTROL_CHANNEL_SOURCES} Source/Control/ContentHandles.cpp)
        # The log tail and its redaction are standard library only as well.
        list(APPEND OPENTUNE_CONTROL_TEST_SOURCES Source/Control/ControlLogTail.cpp)
        list(FILTER OPENTUNE_CONTROL_TEST_SOURCES INCLUDE REGEX "\\.cpp$")
        add_executable(OpenTuneControlUnitTests
            Tests/Control/ControlUnitTests.cpp
            Tests/Control/ControlTestSupport.h
            ${OPENTUNE_CONTROL_TEST_SOURCES}
        )
        target_include_directories(OpenTuneControlUnitTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_compile_definitions(OpenTuneControlUnitTests PRIVATE
            OPENTUNE_CONTROL_SPEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Control/spec/otcp/v1"
            OPENTUNE_CONTROL_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/ControlUnitTests"
        )
        if(WIN32)
            target_link_libraries(OpenTuneControlUnitTests PRIVATE ws2_32 bcrypt)
        else()
            find_package(Threads REQUIRED)
            target_link_libraries(OpenTuneControlUnitTests PRIVATE Threads::Threads)
        endif()
        if(MSVC)
            target_compile_options(OpenTuneControlUnitTests PRIVATE /utf-8 /EHsc /W4)
        endif()
        add_test(NAME ControlUnitTests COMMAND OpenTuneControlUnitTests)
        set_tests_properties(ControlUnitTests PROPERTIES TIMEOUT 300)

        # Mirrored upstream functions: the hashes in Source/Control/mirror-map.json against the source tree
        # (see Source/Control/MIRRORS.md). Standard library and the JSON reader only.
        add_executable(OpenTuneControlMirrorDriftTests
            Tests/Control/ControlMirrorDriftTests.cpp
            Tests/Control/ControlMirrorHash.h
            Source/Control/ControlJson.cpp
            Source/Control/ControlJson.h
        )
        target_include_directories(OpenTuneControlMirrorDriftTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_compile_definitions(OpenTuneControlMirrorDriftTests PRIVATE
            OPENTUNE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if(MSVC)
            target_compile_options(OpenTuneControlMirrorDriftTests PRIVATE /utf-8 /EHsc /W4)
        endif()
        add_test(NAME ControlMirrorDriftTests COMMAND OpenTuneControlMirrorDriftTests)
        set_tests_properties(ControlMirrorDriftTests PROPERTIES TIMEOUT 120)

        # Processor tests without a window or an audio device: the real OpenTuneAudioProcessor of the
        # Standalone SharedCode with its control service. The executable links the SharedCode and no
        # source of the application wrapper (the test defines the createOpenTuneEditor stub). It copies
        # the SharedCode's compile definitions (JucePlugin_Build_* and the rest, JUCE_SHARED_CODE
        # excepted) and include directories: PluginProcessor.h changes its class layout with them, and
        # a test compiled without them would break the one-definition rule without any diagnostic. The
        # language standard and the MSVC runtime come from the directory scope, as for the SharedCode.
        add_executable(OpenTuneControlProcessorTests
            Tests/Control/ControlProcessorTests.cpp
            Tests/Control/ControlDispatcherTests.cpp
            Tests/Control/ControlProcessorSupport.h
            Tests/Control/ControlHeadlessSupport.h
            Tests/Control/TestUiBridge.h
            Tests/Control/ControlTestSupport.h
        )
        target_compile_definitions(OpenTuneControlProcessorTests PRIVATE
            $<FILTER:$<TARGET_PROPERTY:OpenTuneStandalone,COMPILE_DEFINITIONS>,EXCLUDE,^JUCE_SHARED_CODE>
            OPENTUNE_CONTROL_SPEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Control/spec/otcp/v1"
            OPENTUNE_CONTROL_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/ControlProcessorTests"
        )
        target_include_directories(OpenTuneControlProcessorTests PRIVATE
            $<TARGET_PROPERTY:OpenTuneStandalone,INCLUDE_DIRECTORIES>
            ${OPENTUNE_JUCE_DIR}/modules
            ${CMAKE_CURRENT_SOURCE_DIR}/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests
        )
        target_link_libraries(OpenTuneControlProcessorTests PRIVATE OpenTuneStandalone)
        if(APPLE)
            # The test message pump drives the main CFRunLoop directly.
            target_link_libraries(OpenTuneControlProcessorTests PRIVATE "-framework CoreFoundation")
        endif()
        # Jobs, import and the write envelope.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlJobTests.cpp)
        # export.start and the export lock.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlExportTests.cpp)
        # Undo and redo, key, AUTO, note edits and the pitch shift, with the contents they share with the reads.
        target_sources(OpenTuneControlProcessorTests PRIVATE
            Tests/Control/ControlEditTests.cpp
            Tests/Control/ControlContentFixtures.h
        )
        # The time grid: shiftTiming, moveHandles and the seed job.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlTimingTests.cpp)
        # Project files: save, save as, open and clear recent, with a project session and a worker pool of the test.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlProjectTests.cpp)
        # Transport and tracks: the play state, the tempo, the track panel and the barrier of a track deletion.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlTracksTests.cpp)
        # Dialogs and onboarding: ui.getState and ui.dismiss with dialogs and an overlay of the test bridge.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlUiTests.cpp)
        # Preferences and audio device: prefs.set (with its job), audio.get and audio.set on a fake device manager.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlPrefsTests.cpp)
        # Arrangement: arrangement.edit and arrangement.copy, their undo entries and revert recipes.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlArrangementTests.cpp)
        # Selection: ui.setSelection with a test bridge that does what the editor does when a clip is clicked.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlSelectionTests.cpp)
        # app.quit: a test bridge that records the request to close the window, and the scenarios of the call and of what
        # it leaves behind.
        target_sources(OpenTuneControlProcessorTests PRIVATE Tests/Control/ControlQuitTests.cpp)
        if(MSVC)
            target_compile_options(OpenTuneControlProcessorTests PRIVATE /utf-8 /EHsc /bigobj)
        endif()
        if(WIN32)
            # Without /DELAYLOAD the inference runtime is loaded at start, as for the upstream tests.
            add_custom_command(TARGET OpenTuneControlProcessorTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:OpenTuneControlProcessorTests>/${OPENTUNE_ORT_DLL_NAME}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIRECTML_DLL}"
                    "$<TARGET_FILE_DIR:OpenTuneControlProcessorTests>/DirectML.dll"
                COMMENT "Copying inference runtime DLLs to OpenTuneControlProcessorTests...")
        endif()
        # One process per scenario, each with its own OPENTUNE_DATA_DIR (<build>/ot-test-data/<test>).
        # The scenario name comes first; the control API reads the other arguments from the process
        # command line, as in the application. @DATA@ stands for the test's data directory. Paths are
        # native: the control API accepts only the platform's absolute form (PROTOCOL.md section 20.1).
        set(_opentune_control_scenarios
            "StartAndHello|--control-api|--control-api-launch-nonce=headless-test-nonce-0123456789|--control-api-ready-file=@DATA@/ready.json"
            "Disabled|--control-api-dir=not/absolute"
            "RuntimeInitFailure|--control-api"
            "InvalidOption|--control-api|--control-api-dir=not/absolute"
            "DataDirectory"
            "UiBridge"
            "DestroyBeforeStart|--control-api"
            "RepeatedStop|--control-api"
            "Shutdown|--control-api"
            "Reads|--control-api"
            "ThreadProbe|--control-api"
            "DispatchShutdown|--control-api"
            "DispatchTimeout|--control-api"
            "Details|--control-api"
            "Completeness|--control-api"
            "Vectors|--control-api"
            "OffMainThread|--control-api"
        )
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-traces" _opentune_control_trace_dir)
        # Jobs, import and the write envelope (Tests/Control/ControlJobTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Jobs|--control-api"
            "JobShutdown|--control-api"
            "WriteEnvelope|--control-api"
            "Import|--control-api"
            "ImportAfterOpen|--control-api"
            "ImportCancel|--control-api"
            "ImportRevert|--control-api"
            "JobVectors|--control-api"
            "BusyMatrix|--control-api"
            "JobDisconnect|--control-api"
            "ImportFailures|--control-api"
        )
        # export.start and the export lock (Tests/Control/ControlExportTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Export|--control-api"
            "ExportFailures|--control-api"
            "ExportDeferredRetry|--control-api"
            "ExportVectors|--control-api"
            "ExportTimeStretch|--control-api"
            "ExportAfterEdit|--control-api"
            "ExportCancelOverwrite|--control-api"
            "ExportDestroy|--control-api"
            "ExportUnicodePath|--control-api"
            "ExportDryReasons|--control-api"
        )
        # Undo and redo, key and AUTO, note edits and the pitch shift (Tests/Control/ControlEditTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Undo|--control-api"
            "SetKey|--control-api"
            "AutoTune|--control-api"
            "EditNotes|--control-api"
            "EditMultiNoteRender|--control-api"
            "SetPitchShift|--control-api"
            "SnapNotes|--control-api"
            "EditConflictRetry|--control-api"
            "TokenAspects|--control-api"
            "EditCapability|--control-api"
            "EditVectors|--control-api"
        )
        # The time grid (Tests/Control/ControlTimingTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Timing|--control-api"
            "TimeGridSeed|--control-api"
            "TimingTokenAspects|--control-api"
            "TimingVectors|--control-api"
        )
        # Project files (Tests/Control/ControlProjectTests.cpp).
        list(APPEND _opentune_control_scenarios
            "ProjectRoundTrip|--control-api"
            "ProjectOpenTrackCount|--control-api"
            "ProjectJobs|--control-api"
            "ProjectWithoutEditor|--control-api"
            "ProjectSaveCopy|--control-api"
            "ProjectSaveCopyChecks|--control-api"
            "ProjectSaveCopyCapability|--control-api"
            "ProjectReopenKeepsTuning|--control-api"
            "ProjectVectors|--control-api"
        )
        # Transport and tracks (Tests/Control/ControlTracksTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Transport|--control-api"
            "Tracks|--control-api"
            "TrackBarrier|--control-api"
            "TrackVectors|--control-api"
        )
        # Dialogs and onboarding (Tests/Control/ControlUiTests.cpp).
        list(APPEND _opentune_control_scenarios
            "UiState|--control-api"
            "UiDismiss|--control-api"
            "UiOnboardingWithheld|--control-api"
            "UiWorkspace|--control-api"
            "UiVectors|--control-api"
        )
        # Preferences and audio device (Tests/Control/ControlPrefsTests.cpp).
        list(APPEND _opentune_control_scenarios
            "PrefsRoundTrip|--control-api"
            "PrefsSchemeJob|--control-api"
            "PrefsOptions|--control-api"
            "PrefsVocoderJob|--control-api"
            "PrefsVocoderOverlap|--control-api"
            "PrefsVectors|--control-api"
            "AudioDevice|--control-api"
            "AudioVectors|--control-api"
        )
        # Arrangement (Tests/Control/ControlArrangementTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Arrangement|--control-api"
            "ArrangementUndo|--control-api"
            "ArrangementCopy|--control-api"
            "ArrangementVectors|--control-api"
        )
        # Selection (Tests/Control/ControlSelectionTests.cpp).
        list(APPEND _opentune_control_scenarios
            "Selection|--control-api"
            "SelectionMatrix|--control-api"
            "SelectionVectors|--control-api"
        )
        # app.quit (Tests/Control/ControlQuitTests.cpp).
        list(APPEND _opentune_control_scenarios
            "AppQuit|--control-api"
            "AppQuitWithoutWindow|--control-api"
            "AppQuitResult|--control-api"
            "AppQuitSequence|--control-api"
            "AppQuitStopping|--control-api"
            "AppQuitVectors|--control-api"
        )
        # Clip ids after a project was opened (Tests/Control/ControlArrangementTests.cpp).
        list(APPEND _opentune_control_scenarios
            "ClipIdSkip|--control-api"
            "ClipIdsAfterOpen|--control-api"
            "ClipIdsWithoutOpen|--control-api"
        )
        set(_opentune_control_tests)
        foreach(_opentune_control_scenario IN LISTS _opentune_control_scenarios)
            string(REPLACE "|" ";" _opentune_control_scenario "${_opentune_control_scenario}")
            list(GET _opentune_control_scenario 0 _opentune_control_name)
            set(_opentune_control_test "ControlProcessorTests.${_opentune_control_name}")
            set(_opentune_control_data "${CMAKE_CURRENT_BINARY_DIR}/ot-test-data/${_opentune_control_test}")
            set(_opentune_control_arguments)
            foreach(_opentune_control_argument IN LISTS _opentune_control_scenario)
                if(_opentune_control_argument MATCHES "@DATA@")
                    string(REPLACE "@DATA@" "${_opentune_control_data}" _opentune_control_argument
                        "${_opentune_control_argument}")
                    file(TO_NATIVE_PATH "${_opentune_control_argument}" _opentune_control_argument)
                endif()
                list(APPEND _opentune_control_arguments "${_opentune_control_argument}")
            endforeach()
            file(TO_NATIVE_PATH "${_opentune_control_data}" _opentune_control_data)
            add_test(NAME ${_opentune_control_test}
                COMMAND OpenTuneControlProcessorTests ${_opentune_control_arguments})
            set_tests_properties(${_opentune_control_test} PROPERTIES
                TIMEOUT 60
                ENVIRONMENT "OPENTUNE_DATA_DIR=${_opentune_control_data};OPENTUNE_LIFECYCLE_TRACE_DIR=${_opentune_control_trace_dir}")
            list(APPEND _opentune_control_tests ${_opentune_control_test})
        endforeach()
        # EditMultiNoteRender renders five blocks five times and exports: about 12 s on a Windows developer machine.
        # A sanitizer build runs 5 to 15 times slower (ASan/UBSan 2 to 3 times, TSan up to 15 times, on the slower
        # macOS runners), so 12 s * 15 = 180 s; the timeout adds a margin on top. TSan runs have taken 248 to 260 s
        # there, too close to 300 s, so a sanitizer build (-fsanitize in the compiler flags) gets 900 s. The scenario
        # derives its own wait budget from this timeout, so the number of render steps stays the same.
        set(_opentune_control_render_timeout 300)
        if(CMAKE_C_FLAGS MATCHES "fsanitize" OR CMAKE_CXX_FLAGS MATCHES "fsanitize")
            set(_opentune_control_render_timeout 900)
        endif()
        set_tests_properties(ControlProcessorTests.EditMultiNoteRender PROPERTIES
            TIMEOUT ${_opentune_control_render_timeout})
        # Every vector starts a processor of its own.
        set_tests_properties(ControlProcessorTests.Vectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.JobVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.ExportVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.EditVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.TimingVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.ProjectVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.TrackVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.UiVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.PrefsVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.AudioVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.ArrangementVectors PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.SelectionMatrix PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.SelectionVectors PROPERTIES TIMEOUT 300)
        # Several processors, and the wait of up to 15 s for an instance that quits to end.
        set_tests_properties(ControlProcessorTests.AppQuit PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.AppQuitResult PROPERTIES TIMEOUT 300)
        set_tests_properties(ControlProcessorTests.AppQuitVectors PROPERTIES TIMEOUT 300)
        # Two processors and seven times saving and opening a project.
        set_tests_properties(ControlProcessorTests.ClipIdsAfterOpen PROPERTIES TIMEOUT 300)
        # Each scenario process gets its own test's TIMEOUT, from which its watchdog takes its bound (a margin before;
        # ScenarioWatchdog in Tests/Control/ControlProcessorTests.cpp). Keep this after every TIMEOUT of the scenarios.
        foreach(_opentune_control_test IN LISTS _opentune_control_tests)
            get_test_property(${_opentune_control_test} TIMEOUT _opentune_control_timeout)
            set_property(TEST ${_opentune_control_test} APPEND PROPERTY ENVIRONMENT
                "OPENTUNE_SCENARIO_TIMEOUT_SEC=${_opentune_control_timeout}")
        endforeach()
    endif()
endif()
