#include "ProjectModel.h"

#include <juce_core/juce_core.h>
#include <sstream>

namespace OpenTune {

juce::String ProjectSnapshot::generateProjectId()
{
    const auto timestamp = juce::Time::currentTimeMillis();
    const auto uuid = juce::Uuid().toString().substring(0, 8);
    std::ostringstream oss;
    oss << std::hex << timestamp << "-" << uuid;
    return juce::String(oss.str());
}

juce::String ProjectSnapshot::generateTimestamp()
{
    return juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H:%M:%S");
}

} // namespace OpenTune
