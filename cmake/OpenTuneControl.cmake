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
endif()
