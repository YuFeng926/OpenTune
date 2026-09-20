#pragma once
#include "ContentKey.h"
#include "ContentState.h"

namespace OpenTune {

/// Standalone 域退休记录 — 持有权威内容状态 + ContentKey。
/// 不含 render cache、playback snapshot、worker jobs、placement geometry。
struct StandaloneRetiredContentRecord
{
    ContentKey key;
    ContentState content;
};

} // namespace OpenTune
