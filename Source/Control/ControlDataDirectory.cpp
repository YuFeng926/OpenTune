// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlDataDirectory.h"

#include "ControlApiOptions.h"
#include "ControlPathPolicy.h"

namespace OpenTune {

juce::File controlDataDirectory()
{
    const auto value = controlSystemEnvironment("OPENTUNE_DATA_DIR");
    if (!value || value->empty())
        return {};
    // The raw text is checked before a juce::File is built from it (PROTOCOL.md section 20.1).
    if (!controlCheckAbsolutePath(*value, controlNativePathStyle()).empty())
        return {};
    return juce::File(juce::String::fromUTF8(value->c_str()));
}

} // namespace OpenTune
