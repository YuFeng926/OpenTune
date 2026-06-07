#pragma once
#include "AnalysisState.h"
#include "EditableContentState.h"
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

// ARA AudioModification 持有的聚合内容状态
struct AudioModificationContentState
{
    SourceWindow sourceWindow;
    AnalysisState analysis;
    EditableContentState editable;
    ContentLifecycle lifecycle{ContentLifecycle::Empty};
    uint64_t contentRevision{0};

    bool isReady() const noexcept { return lifecycle == ContentLifecycle::Ready; }
};

} // namespace OpenTune
