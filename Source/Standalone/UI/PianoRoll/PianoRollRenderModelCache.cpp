#include "PianoRollRenderModelCache.h"

namespace OpenTune {

void PianoRollRenderModelCache::rebuild(const Key& newKey,
                                         PianoRollRenderer::RenderContext ctx) {
    currentKey_ = newKey;
    cachedCtx_ = std::move(ctx);
    valid_ = true;
}

} // namespace OpenTune
