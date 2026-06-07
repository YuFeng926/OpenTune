#pragma once
#include "ContentKey.h"
#include "AudioModificationContentState.h"
#include <memory>

namespace OpenTune {

// 退休记录持有权威内容状态 + ContentKey，不含 render cache、playback snapshot、worker jobs、placement geometry。
struct RetiredContentRecord
{
    ContentKey key;
    AudioModificationContentState content;
};

} // namespace OpenTune
