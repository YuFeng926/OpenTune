#pragma once

#include "../MaterializationStore.h"

namespace OpenTune {

class BasicReferenceFeatureBuilder {
public:
    BasicReferenceFeatureBuilder() = delete;

    static MaterializationStore::DerivedAnalysis build(
        const MaterializationStore::MaterializationSnapshot& snapshot);
};

} // namespace OpenTune
