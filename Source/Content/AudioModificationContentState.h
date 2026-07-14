#pragma once
#include "AnalysisState.h"
#include "ARAEditableContentState.h"
#include "../Utils/SourceWindow.h"
#include <cstdint>
#include <optional>

namespace OpenTune {

// ARA AudioModification content state: modification-scoped plugin data only.
// Per ARA2 spec: Does NOT contain source PCM (comes from AudioSource via sample access).
struct AudioModificationContentState
{
    SourceWindow sourceWindow;
    AnalysisState analysis;
    ARAEditableContentState editable;  // ARA-specific editable state without audioBuffer
    uint64_t contentRevision{0};

    static std::optional<AudioModificationContentState> makeBorn(const SourceWindow& window);
};

} // namespace OpenTune
