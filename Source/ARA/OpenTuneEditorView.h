#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace OpenTune {

class OpenTuneDocumentController;

class OpenTuneEditorView : public juce::ARAEditorView
{
public:
    OpenTuneEditorView(ARA::PlugIn::DocumentController* araDocumentController,
                       OpenTuneDocumentController& openTuneDocumentController);

    void doNotifySelection(const ARA::PlugIn::ViewSelection* selection) noexcept override;

private:
    OpenTuneDocumentController& openTuneDocumentController_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneEditorView)
};

} // namespace OpenTune
