#pragma once
#include "AnalysisState.h"
#include "ARAEditableContentState.h"
#include "../Utils/SourceWindow.h"
#include <cstdint>

namespace OpenTune {

enum class ContentLifecycle : uint8_t
{
    Empty,
    Loading,
    Analyzing,
    Ready,
    Failed,
    Retired
};

// ARA AudioModification content state: modification-scoped plugin data only.
// Per ARA2 spec: Does NOT contain source PCM (comes from AudioSource via sample access).
struct AudioModificationContentState
{
    SourceWindow sourceWindow;
    AnalysisState analysis;
    ARAEditableContentState editable;  // ARA-specific editable state without audioBuffer
    ContentLifecycle lifecycle{ContentLifecycle::Empty};
    uint64_t contentRevision{0};

    bool isReady() const noexcept { return lifecycle == ContentLifecycle::Ready; }
};

} // namespace OpenTune
