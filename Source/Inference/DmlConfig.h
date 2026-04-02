#pragma once

namespace OpenTune {

struct DmlConfig {
    int deviceId = 0;
    int performancePreference = 1;  // 0=Default, 1=HighPerformance, 2=MinimumPower
    int deviceFilter = 1;           // 1=Gpu (OrtDmlDeviceFilter::Gpu)
};

} // namespace OpenTune
