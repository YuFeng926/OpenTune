#include "OpenTuneEditorView.h"

#include "OpenTuneDocumentController.h"

#include <utility>
#include <vector>

namespace OpenTune {

OpenTuneEditorView::OpenTuneEditorView(ARA::PlugIn::DocumentController* araDocumentController,
                                       OpenTuneDocumentController& openTuneDocumentController)
    : ARAEditorView(araDocumentController)
    , openTuneDocumentController_(openTuneDocumentController)
{
}

void OpenTuneEditorView::doNotifySelection(const ARA::PlugIn::ViewSelection* selection) noexcept
{
    juce::ARAEditorView::doNotifySelection(selection);

    auto playbackRegions = selection->getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
    openTuneDocumentController_.setEditorViewSelectionPlaybackRegions(std::move(playbackRegions));
}

} // namespace OpenTune
