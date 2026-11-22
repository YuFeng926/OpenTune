function(opentune_vst3_overlay_count_occurrences source needle out_count)
    string(LENGTH "${needle}" needle_length)
    set(remaining "${source}")
    set(count 0)

    while(TRUE)
        string(FIND "${remaining}" "${needle}" position)
        if(position EQUAL -1)
            break()
        endif()

        math(EXPR count "${count} + 1")
        math(EXPR next_position "${position} + ${needle_length}")
        string(SUBSTRING "${remaining}" ${next_position} -1 next_remaining)
        set(remaining "${next_remaining}")
    endwhile()

    set(${out_count} "${count}" PARENT_SCOPE)
endfunction()

function(opentune_vst3_overlay_patch source_variable patch_name upstream patched source_path)
    set(source "${${source_variable}}")
    string(FIND "${source}" "\r\n" crlf_position)

    if(NOT crlf_position EQUAL -1)
        string(REPLACE "\n" "\r\n" upstream_to_find "${upstream}")
        string(REPLACE "\n" "\r\n" patched_to_find "${patched}")
    else()
        set(upstream_to_find "${upstream}")
        set(patched_to_find "${patched}")
    endif()

    opentune_vst3_overlay_count_occurrences("${source}" "${upstream_to_find}" upstream_count)
    opentune_vst3_overlay_count_occurrences("${source}" "${patched_to_find}" patched_count)

    if(upstream_count EQUAL 1 AND patched_count EQUAL 0)
        string(REPLACE "${upstream_to_find}" "${patched_to_find}" source "${source}")
        set(${source_variable} "${source}" PARENT_SCOPE)
        message(STATUS "OpenTune JUCE VST3 overlay applied: ${patch_name}")
    elseif(upstream_count EQUAL 0 AND patched_count EQUAL 1)
        message(STATUS "OpenTune JUCE VST3 overlay already applied: ${patch_name}")
    else()
        message(FATAL_ERROR
            "OpenTune JUCE VST3 overlay expected exactly one upstream or patched needle for "
            "${patch_name} in ${source_path}; found upstream=${upstream_count}, patched=${patched_count}.")
    endif()
endfunction()

function(opentune_vst3_overlay_replace_expected source_variable patch_name needle replacement expected_count source_path)
    set(source "${${source_variable}}")
    string(FIND "${source}" "\r\n" crlf_position)

    if(NOT crlf_position EQUAL -1)
        string(REPLACE "\n" "\r\n" needle_to_find "${needle}")
        string(REPLACE "\n" "\r\n" replacement_text "${replacement}")
    else()
        set(needle_to_find "${needle}")
        set(replacement_text "${replacement}")
    endif()

    opentune_vst3_overlay_count_occurrences("${source}" "${needle_to_find}" needle_count)

    if(needle_count EQUAL expected_count)
        string(REPLACE "${needle_to_find}" "${replacement_text}" source "${source}")
        set(${source_variable} "${source}" PARENT_SCOPE)
    elseif(NOT needle_count EQUAL 0)
        message(FATAL_ERROR
            "OpenTune JUCE VST3 overlay expected ${expected_count} ${patch_name} needles in "
            "${source_path}; found ${needle_count}.")
    endif()
endfunction()

function(opentune_apply_juce_vst3_realtime_overlay source_variable juce_root generated_dir vst3_cpp_path)
    set(source "${${source_variable}}")

    set(source_include_needle "#include <juce_core/system/juce_TargetPlatform.h>")
    set(source_include_patched [=[#include <atomic>
#include <mutex>
#include <thread>
#include <juce_core/system/juce_TargetPlatform.h>]=])
    opentune_vst3_overlay_patch(source
        "real-time support headers" "${source_include_needle}" "${source_include_patched}" "${vst3_cpp_path}")

    set(midi_macro_needle "#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS")
    set(midi_macro_patched [=[#if defined(JUCE_OPENTUNE_REALTIME_AUDIO) && JUCE_OPENTUNE_REALTIME_AUDIO
 #undef JucePlugin_WantsMidiInput
 #define JucePlugin_WantsMidiInput 0
 #undef JucePlugin_ProducesMidiOutput
 #define JucePlugin_ProducesMidiOutput 0
 #undef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
 #define JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS 0
#endif

#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS]=])
    opentune_vst3_overlay_patch(source
        "VST3 MIDI disabled for real-time mode" "${midi_macro_needle}" "${midi_macro_patched}" "${vst3_cpp_path}")

    set(component_public_needle [=[public:
    JuceVST3Component (const VSTComSmartPtr<Vst::IHostApplication>& h,]=])
    set(component_public_patched [=[public:
    // The high bit closes admission; the remaining bits count active callbacks.
    static constexpr unsigned int audioProcessClosedBit = 1u << 31;
    static constexpr unsigned int audioProcessReaderMask = audioProcessClosedBit - 1;
    static_assert (std::atomic<unsigned int>::is_always_lock_free);

    class ScopedAudioProcessAccess
    {
    public:
        explicit ScopedAudioProcessAccess (JuceVST3Component& component) noexcept
            : owner (component)
        {
            const auto previous = owner.audioProcessGate.fetch_add (1, std::memory_order_acquire);

            if ((previous & audioProcessClosedBit) == 0)
                admitted = true;
            else
                owner.audioProcessGate.fetch_sub (1, std::memory_order_release);
        }

        ~ScopedAudioProcessAccess()
        {
            if (admitted)
                owner.audioProcessGate.fetch_sub (1, std::memory_order_release);
        }

        explicit operator bool() const noexcept { return admitted; }
        ScopedAudioProcessAccess (const ScopedAudioProcessAccess&) = delete;
        ScopedAudioProcessAccess& operator= (const ScopedAudioProcessAccess&) = delete;

    private:
        JuceVST3Component& owner;
        bool admitted = false;
    };

    class ScopedAudioProcessQuiescence
    {
    public:
        explicit ScopedAudioProcessQuiescence (JuceVST3Component& component, bool keepClosed = false)
            : owner (component), writerLock (owner.audioProcessWriterMutex)
        {
            if (owner.audioProcessWriterDepth == 0)
            {
                const auto previous = owner.audioProcessGate.fetch_or (audioProcessClosedBit,
                                                                         std::memory_order_acq_rel);
                owner.audioProcessKeepClosed = (previous & audioProcessClosedBit) != 0;

                while ((owner.audioProcessGate.load (std::memory_order_acquire)
                        & audioProcessReaderMask) != 0)
                    std::this_thread::yield();
            }

            ++owner.audioProcessWriterDepth;
            owner.audioProcessKeepClosed = owner.audioProcessKeepClosed || keepClosed;
        }

        ~ScopedAudioProcessQuiescence()
        {
            if (--owner.audioProcessWriterDepth == 0 && ! owner.audioProcessKeepClosed)
                owner.audioProcessGate.fetch_and (audioProcessReaderMask, std::memory_order_release);
        }

    private:
        JuceVST3Component& owner;
        std::unique_lock<std::recursive_mutex> writerLock;
    };

    JuceVST3Component (const VSTComSmartPtr<Vst::IHostApplication>& h,]=])
    opentune_vst3_overlay_patch(source
        "audio admission and nested writer guards"
        "${component_public_needle}" "${component_public_patched}" "${vst3_cpp_path}")

    set(destructor_needle [=[    ~JuceVST3Component() override
    {]=])
    set(destructor_patched [=[    ~JuceVST3Component() override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this, true);]=])
    opentune_vst3_overlay_patch(source
        "component destruction quiescence" "${destructor_needle}" "${destructor_patched}" "${vst3_cpp_path}")

    set(initialize_needle [=[    tresult PLUGIN_API initialize (FUnknown* hostContext) override
    {]=])
    set(initialize_patched [=[    tresult PLUGIN_API initialize (FUnknown* hostContext) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "component initialization quiescence" "${initialize_needle}" "${initialize_patched}" "${vst3_cpp_path}")

    set(terminate_needle [=[    tresult PLUGIN_API terminate() override
    {
        getPluginInstance().releaseResources();]=])
    set(terminate_patched [=[    tresult PLUGIN_API terminate() override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this, true);
        getPluginInstance().releaseResources();]=])
    opentune_vst3_overlay_patch(source
        "component termination quiescence" "${terminate_needle}" "${terminate_patched}" "${vst3_cpp_path}")

    # IConnectionPoint connect/disconnect/notify are UI-thread entry points in
    # the VST3 SDK. They mutate the controller borrowed by process().
    set(connect_needle [=[    tresult PLUGIN_API connect (IConnectionPoint* other) override
    {
        if (other != nullptr && juceVST3EditController == nullptr)]=])
    set(connect_patched [=[    tresult PLUGIN_API connect (IConnectionPoint* other) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);
        if (other != nullptr && juceVST3EditController == nullptr)]=])
    opentune_vst3_overlay_patch(source
        "controller connection quiescence" "${connect_needle}" "${connect_patched}" "${vst3_cpp_path}")

    set(disconnect_needle [=[    tresult PLUGIN_API disconnect (IConnectionPoint*) override
    {
        if (juceVST3EditController != nullptr)]=])
    set(disconnect_patched [=[    tresult PLUGIN_API disconnect (IConnectionPoint*) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);
        if (juceVST3EditController != nullptr)]=])
    opentune_vst3_overlay_patch(source
        "controller disconnection quiescence" "${disconnect_needle}" "${disconnect_patched}" "${vst3_cpp_path}")

    set(notify_needle [=[    tresult PLUGIN_API notify (Vst::IMessage* message) override
    {
        if (message != nullptr && juceVST3EditController == nullptr)]=])
    set(notify_patched [=[    tresult PLUGIN_API notify (Vst::IMessage* message) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);
        if (message != nullptr && juceVST3EditController == nullptr)]=])
    opentune_vst3_overlay_patch(source
        "controller notification quiescence" "${notify_needle}" "${notify_patched}" "${vst3_cpp_path}")

    set(ara_bind_needle [=[    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentControllerWithRoles (ARA::ARADocumentControllerRef documentControllerRef,
                                                                                         ARA::ARAPlugInInstanceRoleFlags knownRoles, ARA::ARAPlugInInstanceRoleFlags assignedRoles) SMTG_OVERRIDE
    {]=])
    set(ara_bind_patched [=[    const ARA::ARAPlugInExtensionInstance* PLUGIN_API bindToDocumentControllerWithRoles (ARA::ARADocumentControllerRef documentControllerRef,
                                                                                         ARA::ARAPlugInInstanceRoleFlags knownRoles, ARA::ARAPlugInInstanceRoleFlags assignedRoles) SMTG_OVERRIDE
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "ARA binding quiescence" "${ara_bind_needle}" "${ara_bind_patched}" "${vst3_cpp_path}")

    set(set_active_needle [=[    tresult PLUGIN_API setActive (TBool state) override
    {]=])
    set(set_active_patched [=[    tresult PLUGIN_API setActive (TBool state) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "setActive quiescence" "${set_active_needle}" "${set_active_patched}" "${vst3_cpp_path}")

    set(activate_bus_needle [=[    tresult PLUGIN_API activateBus (Vst::MediaType type,
                                    Vst::BusDirection dir,
                                    Steinberg::int32 index,
                                    TBool state) override
    {]=])
    set(activate_bus_patched [=[    tresult PLUGIN_API activateBus (Vst::MediaType type,
                                    Vst::BusDirection dir,
                                    Steinberg::int32 index,
                                    TBool state) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "activateBus quiescence" "${activate_bus_needle}" "${activate_bus_patched}" "${vst3_cpp_path}")

    set(set_bus_arrangements_needle [=[    tresult PLUGIN_API setBusArrangements (Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                           Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override
    {]=])
    set(set_bus_arrangements_patched [=[    tresult PLUGIN_API setBusArrangements (Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                           Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "setBusArrangements quiescence" "${set_bus_arrangements_needle}" "${set_bus_arrangements_patched}" "${vst3_cpp_path}")

    set(setup_processing_needle [=[    tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& newSetup) override
    {]=])
    set(setup_processing_patched [=[    tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& newSetup) override
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "setupProcessing quiescence" "${setup_processing_needle}" "${setup_processing_patched}" "${vst3_cpp_path}")

    set(set_processing_needle [=[        if (! state)
            getPluginInstance().reset();

        return kResultTrue;]=])
    set(set_processing_patched [=[        if (! state)
        {
            ScopedAudioProcessQuiescence audioQuiescence (*this);
            getPluginInstance().reset();
        }

        return kResultTrue;]=])
    opentune_vst3_overlay_patch(source
        "setProcessing reset quiescence" "${set_processing_needle}" "${set_processing_patched}" "${vst3_cpp_path}")

    set(set_state_information_needle [=[    void setStateInformation (const void* data, int sizeAsInt)
    {]=])
    set(set_state_information_patched [=[    void setStateInformation (const void* data, int sizeAsInt)
    {
        ScopedAudioProcessQuiescence audioQuiescence (*this);]=])
    opentune_vst3_overlay_patch(source
        "state loading quiescence" "${set_state_information_needle}" "${set_state_information_patched}" "${vst3_cpp_path}")

    set(process_entry_needle [=[    tresult PLUGIN_API process (Vst::ProcessData& data) override
    {
        const FLStudioDIYSpecificationEnforcementLock lock (flStudioDIYSpecificationEnforcementMutex);]=])
    set(process_entry_patched [=[    tresult PLUGIN_API process (Vst::ProcessData& data) override
    {
        const FLStudioDIYSpecificationEnforcementLock lock (flStudioDIYSpecificationEnforcementMutex);
        ScopedAudioProcessAccess audioAccess (*this);

        if (! audioAccess)
        {
            clearProcessOutputs (data);
            return kResultTrue;
        }

        if (! active && data.numSamples > 0)
        {
            clearProcessOutputs (data);
            return kResultTrue;
        }]=])
    opentune_vst3_overlay_patch(source
        "complete process entry admission" "${process_entry_needle}" "${process_entry_patched}" "${vst3_cpp_path}")

    # Host-originated values already belong to the host's automation stream.
    # OpenTune's editVersion and wrapper bypass values are atomic parameters;
    # notifying JUCE listeners here would acquire AudioProcessorParameter's lock.
    set(parameter_apply_needle "                        setValueAndNotifyIfChanged (*param, (float) change->value);")
    set(parameter_apply_patched "                        param->setValue ((float) change->value);")
    opentune_vst3_overlay_patch(source
        "host parameter application without listener dispatch"
        "${parameter_apply_needle}" "${parameter_apply_patched}" "${vst3_cpp_path}")

    set(clear_outputs_needle [=[private:
    /*  FL's Patcher implements the VST3 specification incorrectly, calls process() before/during]=])
    set(clear_outputs_patched [=[private:
    void clearProcessOutputs (Vst::ProcessData& data) noexcept
    {
        if (data.numSamples <= 0 || data.outputs == nullptr)
            return;

        if (data.symbolicSampleSize == Vst::kSample32)
        {
            for (Steinberg::int32 bus = 0; bus < data.numOutputs; ++bus)
            {
                auto& output = data.outputs[bus];
                if (output.channelBuffers32 == nullptr)
                    continue;

                for (Steinberg::int32 channel = 0; channel < output.numChannels; ++channel)
                    if (output.channelBuffers32[channel] != nullptr)
                        FloatVectorOperations::clear (output.channelBuffers32[channel], (size_t) data.numSamples);
            }

            return;
        }

        if (data.symbolicSampleSize == Vst::kSample64)
        {
            for (Steinberg::int32 bus = 0; bus < data.numOutputs; ++bus)
            {
                auto& output = data.outputs[bus];
                if (output.channelBuffers64 == nullptr)
                    continue;

                for (Steinberg::int32 channel = 0; channel < output.numChannels; ++channel)
                    if (output.channelBuffers64[channel] != nullptr)
                        FloatVectorOperations::clear (output.channelBuffers64[channel], (size_t) data.numSamples);
            }

            return;
        }

        jassertfalse;
    }

    /*  FL's Patcher implements the VST3 specification incorrectly, calls process() before/during]=])
    opentune_vst3_overlay_patch(source
        "closed-admission host output clearing" "${clear_outputs_needle}" "${clear_outputs_patched}" "${vst3_cpp_path}")

    opentune_vst3_overlay_replace_expected(source
        "callback-lock removal" "            const ScopedLock sl (pluginInstance->getCallbackLock());" "" 2 "${vst3_cpp_path}")
    opentune_vst3_overlay_count_occurrences("${source}" "getCallbackLock" callback_lock_count)
    if(NOT callback_lock_count EQUAL 0)
        message(FATAL_ERROR
            "OpenTune JUCE VST3 realtime overlay left ${callback_lock_count} callback-lock references in ${vst3_cpp_path}.")
    endif()

    opentune_vst3_overlay_replace_expected(source
        "FL Studio process-lock removal"
        "        const FLStudioDIYSpecificationEnforcementLock lock (flStudioDIYSpecificationEnforcementMutex);"
        "" 4 "${vst3_cpp_path}")

    set(fl_lock_class_needle [=[    /*  FL's Patcher implements the VST3 specification incorrectly, calls process() before/during
        setActive().
    */
    class [[nodiscard]] FLStudioDIYSpecificationEnforcementLock
    {
    public:
        explicit FLStudioDIYSpecificationEnforcementLock (CriticalSection& mutex)
        {
            static const auto lockRequired = PluginHostType().isFruityLoops();

            if (lockRequired)
                lock.emplace (mutex);
        }

    private:
        std::optional<ScopedLock> lock;
    };]=])
    opentune_vst3_overlay_replace_expected(source
        "FL Studio process-lock helper removal" "${fl_lock_class_needle}" "" 1 "${vst3_cpp_path}")
    opentune_vst3_overlay_replace_expected(source
        "FL Studio process-lock state removal"
        "    CriticalSection flStudioDIYSpecificationEnforcementMutex;" "" 1 "${vst3_cpp_path}")
    opentune_vst3_overlay_count_occurrences(
        "${source}" "FLStudioDIYSpecificationEnforcementLock" fl_lock_reference_count)
    if(NOT fl_lock_reference_count EQUAL 0)
        message(FATAL_ERROR
            "OpenTune JUCE VST3 realtime overlay left ${fl_lock_reference_count} FL Studio process-lock references in ${vst3_cpp_path}.")
    endif()

    set(process_state_needle [=[    MidiBuffer midiBuffer;
    ClientBufferMapper bufferMapper;]=])
    set(process_state_patched [=[    MidiBuffer midiBuffer;
    ClientBufferMapper bufferMapper;

    std::atomic<unsigned int> audioProcessGate { 0 };
    std::recursive_mutex audioProcessWriterMutex;
    unsigned int audioProcessWriterDepth = 0;
    bool audioProcessKeepClosed = false;]=])
    opentune_vst3_overlay_patch(source
        "reader gate state" "${process_state_needle}" "${process_state_patched}" "${vst3_cpp_path}")

    set(common_header_path
        "${juce_root}/modules/juce_audio_processors_headless/format_types/juce_VST3Common.h")
    if(NOT EXISTS "${common_header_path}")
        message(FATAL_ERROR "JUCE VST3 common header not found: ${common_header_path}")
    endif()

    file(READ "${common_header_path}" common_header_source)
    set(channels_reserve_needle "channels.reserve ((size_t) jmin (128, numChannels));")
    set(channels_reserve_patched "channels.reserve (static_cast<size_t> (numChannels));")
    opentune_vst3_overlay_patch(common_header_source
        "remapping channel capacity reservation"
        "${channels_reserve_needle}" "${channels_reserve_patched}" "${common_header_path}")

    set(generated_include_dir "${generated_dir}/include")
    set(generated_header_dir
        "${generated_include_dir}/juce_audio_processors_headless/format_types")
    set(generated_header_path "${generated_header_dir}/juce_VST3Common.h")
    file(MAKE_DIRECTORY "${generated_header_dir}")
    file(WRITE "${generated_header_path}" "${common_header_source}")

    set(common_header_include_needle
        "#include <juce_audio_processors_headless/format_types/juce_VST3Common.h>")
    set(common_header_include_patched
        "#include \"include/juce_audio_processors_headless/format_types/juce_VST3Common.h\"")
    opentune_vst3_overlay_patch(source
        "generated remapping header selection"
        "${common_header_include_needle}" "${common_header_include_patched}" "${vst3_cpp_path}")

    set(${source_variable} "${source}" PARENT_SCOPE)
endfunction()
