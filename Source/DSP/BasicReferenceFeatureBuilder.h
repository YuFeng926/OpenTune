#pragma once

#include "../MaterializationStore.h"

namespace OpenTune {

class BasicReferenceFeatureBuilder {
public:
    BasicReferenceFeatureBuilder() = delete;

    static ReferenceFeatureSet build(
        const MaterializationStore::MaterializationSnapshot& snapshot);
};

} // namespace OpenTune
