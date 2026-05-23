#include "ProjectModel.h"

#include <juce_core/juce_core.h>
#include <random>
#include <sstream>

namespace OpenTune {

juce::String ProjectSnapshot::generateProjectId()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    const auto timestamp = juce::Time::currentTimeMillis();
    const auto random = dis(gen);

    std::ostringstream oss;
    oss << std::hex << timestamp << "-" << random;
    return juce::String(oss.str());
}

juce::String ProjectSnapshot::generateTimestamp()
{
    return juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H:%M:%S");
}

} // namespace OpenTune
