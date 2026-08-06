#pragma once

#include <cstdint>

namespace OpenTune {

enum class OriginalF0State : uint8_t {
    NotRequested = 0,
    Extracting,
    Ready,
    Failed
};

} // namespace OpenTune
