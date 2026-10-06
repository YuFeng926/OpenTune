#pragma once

#include "F0Timeline.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenTune {
inline void fillF0GapsForVocoder(std::vector<float>& f0,
                                          const std::vector<F0FrameRange>& erasedRanges = {})
{
    const int n = static_cast<int>(f0.size());
    std::vector<bool> erased(static_cast<size_t>(n), false);
    for (const auto& range : erasedRanges)
        for (int i = std::max(0, range.startFrame); i < std::min(n, range.endFrameExclusive); ++i)
            erased[static_cast<size_t>(i)] = true;
    for (int start = 0; start < n;) {
        if (erased[static_cast<size_t>(start)]) { f0[static_cast<size_t>(start)] = 0.0f; ++start; continue; }
        int end = start + 1;
        while (end < n && !erased[static_cast<size_t>(end)]) ++end;
        std::vector<int> voiced;
        for (int i = start; i < end; ++i)
            if (f0[static_cast<size_t>(i)] > 0.0f) voiced.push_back(i);
        for (int i = start; i < end; ++i) {
            if (f0[static_cast<size_t>(i)] > 0.0f || voiced.empty()) continue;
            const auto it = std::lower_bound(voiced.begin(), voiced.end(), i);
            if (it == voiced.begin()) f0[static_cast<size_t>(i)] = f0[static_cast<size_t>(voiced.front())];
            else if (it == voiced.end()) f0[static_cast<size_t>(i)] = f0[static_cast<size_t>(voiced.back())];
            else {
                const int left = *(it - 1), right = *it;
                const float t = static_cast<float>(i - left) / static_cast<float>(right - left);
                f0[static_cast<size_t>(i)] = std::pow(2.0f,
                    std::log2(f0[static_cast<size_t>(left)]) * (1.0f - t)
                    + std::log2(f0[static_cast<size_t>(right)]) * t);
            }
        }
        start = end;
    }
}

}
