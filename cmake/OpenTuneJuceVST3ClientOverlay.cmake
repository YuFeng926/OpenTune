function(opentune_use_juce_vst3_client_ara_legacy_bind_overlay juce_root)
    if(NOT TARGET juce_audio_plugin_client_VST3)
        message(FATAL_ERROR
            "JUCE VST3 wrapper target is not available. Call "
            "opentune_use_juce_vst3_client_ara_legacy_bind_overlay() after adding JUCE modules.")
    endif()

    # The real source is always .cpp; on Apple platforms JUCE exposes a tiny
    # .mm wrapper in INTERFACE_SOURCES that simply #includes the .cpp.
    set(vst3_cpp_path
        "${juce_root}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")
    set(vst3_mm_path
        "${juce_root}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.mm")

    if(NOT EXISTS "${vst3_cpp_path}")
        message(FATAL_ERROR "JUCE VST3 client source not found: ${vst3_cpp_path}")
    endif()

    file(READ "${vst3_cpp_path}" vst3_client_source)

    # ---- Patch 1: zero-data transport-only branch -------------------------------
    # JUCE's VST3 process() guards processAudio<sample type>() behind
    #   if (data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0)
    # so a zero-sample block that still carries a real ProcessContext is
    # dropped before the AudioProcessor can observe the current PositionInfo.
    # The fix keeps the original audio guard untouched (so a pure parameter
    # flush with no ProcessContext still does not synthesize audio) and adds a
    # transport-only else branch that, under the existing callback lock, builds
    # an empty AudioBuffer and calls pluginInstance->processBlock so the
    # processor's own zero-sample early return updates PlayHeadState without
    # touching ClientRemappedBuffer, the ARA renderer or CaptureSession.
    set(transport_upstream_snippet [=[
        // If all of these are zero, the host is attempting to flush parameters without processing audio.
        if (data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0)
        {
            if      (processSetup.symbolicSampleSize == Vst::kSample32) processAudio<float>  (data);
            else if (processSetup.symbolicSampleSize == Vst::kSample64) processAudio<double> (data);
            else jassertfalse;
        }
]=])

    set(transport_patched_snippet [=[
        // If all of these are zero, the host is attempting to flush parameters without processing audio.
        if (data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0)
        {
            if      (processSetup.symbolicSampleSize == Vst::kSample32) processAudio<float>  (data);
            else if (processSetup.symbolicSampleSize == Vst::kSample64) processAudio<double> (data);
            else jassertfalse;
        }
        else if (data.processContext != nullptr)
        {
            const ScopedLock sl (pluginInstance->getCallbackLock());

            pluginInstance->setNonRealtime (data.processMode == Vst::kOffline);

            if (!pluginInstance->isSuspended())
            {
                if      (processSetup.symbolicSampleSize == Vst::kSample32)
                {
                    juce::AudioBuffer<float> emptyFloatBuffer;
                    pluginInstance->processBlock (emptyFloatBuffer, midiBuffer);
                }
                else if (processSetup.symbolicSampleSize == Vst::kSample64)
                {
                    juce::AudioBuffer<double> emptyDoubleBuffer;
                    pluginInstance->processBlock (emptyDoubleBuffer, midiBuffer);
                }
                else
                {
                    jassertfalse;
                }
            }
        }
]=])

    string(FIND "${vst3_client_source}" "${transport_patched_snippet}" transport_patched_pos)
    if(NOT transport_patched_pos EQUAL -1)
        message(STATUS
            "JUCE VST3 client already supports zero-data transport-only branch; skipping transport patch")
    else()
        string(REPLACE "\n" "\r\n" transport_patched_snippet_crlf "${transport_patched_snippet}")
        string(FIND "${vst3_client_source}" "${transport_patched_snippet_crlf}" transport_patched_crlf_pos)
        if(NOT transport_patched_crlf_pos EQUAL -1)
            message(STATUS
                "JUCE VST3 client already supports zero-data transport-only branch (CRLF); skipping transport patch")
        else()
            string(FIND "${vst3_client_source}" "${transport_upstream_snippet}" transport_upstream_pos)
            if(transport_upstream_pos EQUAL -1)
                string(REPLACE "\n" "\r\n" transport_upstream_snippet_crlf "${transport_upstream_snippet}")
                string(FIND "${vst3_client_source}" "${transport_upstream_snippet_crlf}" transport_upstream_crlf_pos)
                if(transport_upstream_crlf_pos EQUAL -1)
                    message(FATAL_ERROR
                        "OpenTune JUCE VST3 client overlay cannot apply zero-data transport patch. "
                        "Inspect ${vst3_cpp_path}; the audited JUCE process() guard no longer matches.")
                endif()
                string(REPLACE "${transport_upstream_snippet_crlf}" "${transport_patched_snippet_crlf}"
                       vst3_client_source "${vst3_client_source}")
            else()
                string(REPLACE "${transport_upstream_snippet}" "${transport_patched_snippet}"
                       vst3_client_source "${vst3_client_source}")
            endif()
        endif()
    endif()

    # ---- Patch 2: ARA 1.x legacy bind -------------------------------------------
    set(ara_patched_needle
        "return bindToDocumentControllerWithRoles (controllerRef, 0, 0);")
    string(FIND "${vst3_client_source}" "${ara_patched_needle}" ara_patched_pos)
    if(NOT ara_patched_pos EQUAL -1)
        message(STATUS
            "JUCE VST3 client already supports legacy ARA bind; skipping ARA patch")
    else()
        set(ara_upstream_snippet [=[
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentController (ARA::ARADocumentControllerRef /*controllerRef*/) SMTG_OVERRIDE
    {
        ARA_VALIDATE_API_STATE (false && "call is deprecated in ARA 2, host must not call this");
        return nullptr;
    }
]=])
        set(ara_patched_snippet [=[
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentController (ARA::ARADocumentControllerRef controllerRef) SMTG_OVERRIDE
    {
        // ARA SDK 2.x deprecates this entry point, but ARA 1.x hosts still call it.
        // The SDK specifies that this is equivalent to the roles-aware call with no known roles.
        return bindToDocumentControllerWithRoles (controllerRef, 0, 0);
    }
]=])

        string(FIND "${vst3_client_source}" "${ara_upstream_snippet}" ara_upstream_pos)
        if(ara_upstream_pos EQUAL -1)
            string(REPLACE "\n" "\r\n" ara_upstream_snippet_crlf "${ara_upstream_snippet}")
            string(REPLACE "\n" "\r\n" ara_patched_snippet_crlf "${ara_patched_snippet}")
            string(FIND "${vst3_client_source}" "${ara_upstream_snippet_crlf}" ara_upstream_crlf_pos)

            if(ara_upstream_crlf_pos EQUAL -1)
                message(FATAL_ERROR
                    "OpenTune JUCE VST3 client overlay cannot be generated. Inspect ${vst3_cpp_path}; "
                    "the vendored JUCE VST3 ARA entry point no longer matches the audited source.")
            endif()

            string(REPLACE "${ara_upstream_snippet_crlf}" "${ara_patched_snippet_crlf}"
                   vst3_client_source "${vst3_client_source}")
        else()
            string(REPLACE "${ara_upstream_snippet}" "${ara_patched_snippet}"
                   vst3_client_source "${vst3_client_source}")
        endif()
    endif()

    # ---- Emit patched source and rewire INTERFACE_SOURCES -----------------------
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated/OpenTune/JUCE")
    set(generated_vst3_cpp_path "${generated_dir}/juce_audio_plugin_client_VST3.cpp")
    file(MAKE_DIRECTORY "${generated_dir}")
    file(WRITE "${generated_vst3_cpp_path}" "${vst3_client_source}")

    get_target_property(vst3_wrapper_sources juce_audio_plugin_client_VST3 INTERFACE_SOURCES)
    if(NOT vst3_wrapper_sources)
        message(FATAL_ERROR "JUCE VST3 wrapper target has no INTERFACE_SOURCES to overlay.")
    endif()

    # Decide which file is the actual source in INTERFACE_SOURCES:
    # Apple -> .mm (which #includes the .cpp), other platforms -> .cpp directly.
    set(updated_vst3_wrapper_sources)
    set(replaced_source FALSE)
    set(matched_source_path "")
    foreach(source_path IN LISTS vst3_wrapper_sources)
        if(source_path STREQUAL "${vst3_cpp_path}" OR source_path STREQUAL "${vst3_mm_path}")
            set(replaced_source TRUE)
            set(matched_source_path "${source_path}")
        else()
            list(APPEND updated_vst3_wrapper_sources "${source_path}")
        endif()
    endforeach()

    if(NOT replaced_source)
        message(FATAL_ERROR
            "OpenTune JUCE VST3 client overlay could not find "
            "${vst3_cpp_path} or ${vst3_mm_path} in juce_audio_plugin_client_VST3 INTERFACE_SOURCES.")
    endif()

    # When the original source is the .mm wrapper, generate a parallel .mm that
    # #includes the patched .cpp and substitute that into INTERFACE_SOURCES.
    if(matched_source_path STREQUAL "${vst3_mm_path}")
        set(generated_vst3_mm_path "${generated_dir}/juce_audio_plugin_client_VST3.mm")
        file(WRITE "${generated_vst3_mm_path}"
            "// Generated by OpenTuneJuceVST3ClientOverlay.cmake - do not edit.\n"
            "#include \"${generated_vst3_cpp_path}\"\n")
        set(generated_entry_path "${generated_vst3_mm_path}")
    else()
        set(generated_entry_path "${generated_vst3_cpp_path}")
    endif()

    set_property(TARGET juce_audio_plugin_client_VST3
        PROPERTY INTERFACE_SOURCES "${updated_vst3_wrapper_sources}")
    target_sources(juce_audio_plugin_client_VST3 INTERFACE "${generated_entry_path}")

    message(STATUS
        "OpenTune uses generated JUCE VST3 client overlay: ${generated_entry_path}")
endfunction()
