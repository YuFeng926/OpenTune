function(opentune_use_juce_vst3_client_ara_legacy_bind_overlay juce_root)
    if(NOT OPENTUNE_ENABLE_ARA)
        return()
    endif()

    if(NOT TARGET juce_audio_plugin_client_VST3)
        message(FATAL_ERROR
            "JUCE VST3 wrapper target is not available. Call "
            "opentune_use_juce_vst3_client_ara_legacy_bind_overlay() after adding JUCE modules.")
    endif()

    set(vst3_client_path
        "${juce_root}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")

    if(NOT EXISTS "${vst3_client_path}")
        message(FATAL_ERROR "JUCE VST3 client source not found: ${vst3_client_path}")
    endif()

    file(READ "${vst3_client_path}" vst3_client_source)

    set(patched_needle
        "return bindToDocumentControllerWithRoles (controllerRef, 0, 0);")
    string(FIND "${vst3_client_source}" "${patched_needle}" patched_pos)
    if(NOT patched_pos EQUAL -1)
        message(STATUS
            "JUCE VST3 client already supports legacy ARA bind; using vendored source unchanged")
        return()
    endif()

    set(upstream_snippet [=[
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentController (ARA::ARADocumentControllerRef /*controllerRef*/) SMTG_OVERRIDE
    {
        ARA_VALIDATE_API_STATE (false && "call is deprecated in ARA 2, host must not call this");
        return nullptr;
    }
]=])

    set(patched_snippet [=[
    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentController (ARA::ARADocumentControllerRef controllerRef) SMTG_OVERRIDE
    {
        // ARA SDK 2.x deprecates this entry point, but ARA 1.x hosts still call it.
        // The SDK specifies that this is equivalent to the roles-aware call with no known roles.
        return bindToDocumentControllerWithRoles (controllerRef, 0, 0);
    }
]=])

    string(FIND "${vst3_client_source}" "${upstream_snippet}" upstream_pos)
    if(upstream_pos EQUAL -1)
        string(REPLACE "\n" "\r\n" upstream_snippet_crlf "${upstream_snippet}")
        string(REPLACE "\n" "\r\n" patched_snippet_crlf "${patched_snippet}")
        string(FIND "${vst3_client_source}" "${upstream_snippet_crlf}" upstream_crlf_pos)

        if(upstream_crlf_pos EQUAL -1)
            message(FATAL_ERROR
                "OpenTune JUCE VST3 client overlay cannot be generated. Inspect ${vst3_client_path}; "
                "the vendored JUCE VST3 ARA entry point no longer matches the audited source.")
        endif()

        string(REPLACE "${upstream_snippet_crlf}" "${patched_snippet_crlf}"
               overlay_vst3_client_source "${vst3_client_source}")
    else()
        string(REPLACE "${upstream_snippet}" "${patched_snippet}"
               overlay_vst3_client_source "${vst3_client_source}")
    endif()

    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated/OpenTune/JUCE")
    set(generated_vst3_client_path "${generated_dir}/juce_audio_plugin_client_VST3.cpp")
    file(MAKE_DIRECTORY "${generated_dir}")
    file(WRITE "${generated_vst3_client_path}" "${overlay_vst3_client_source}")

    get_target_property(vst3_wrapper_sources juce_audio_plugin_client_VST3 INTERFACE_SOURCES)
    if(NOT vst3_wrapper_sources)
        message(FATAL_ERROR "JUCE VST3 wrapper target has no INTERFACE_SOURCES to overlay.")
    endif()

    set(updated_vst3_wrapper_sources)
    set(replaced_source FALSE)
    foreach(source_path IN LISTS vst3_wrapper_sources)
        if(source_path STREQUAL "${vst3_client_path}")
            set(replaced_source TRUE)
        else()
            list(APPEND updated_vst3_wrapper_sources "${source_path}")
        endif()
    endforeach()

    if(NOT replaced_source)
        message(FATAL_ERROR
            "OpenTune JUCE VST3 client overlay could not find ${vst3_client_path} "
            "in juce_audio_plugin_client_VST3 INTERFACE_SOURCES.")
    endif()

    set_property(TARGET juce_audio_plugin_client_VST3
        PROPERTY INTERFACE_SOURCES "${updated_vst3_wrapper_sources}")
    target_sources(juce_audio_plugin_client_VST3 INTERFACE "${generated_vst3_client_path}")

    message(STATUS
        "OpenTune uses generated JUCE VST3 client overlay for ARA 1.x legacy bind: "
        "${generated_vst3_client_path}")
endfunction()
