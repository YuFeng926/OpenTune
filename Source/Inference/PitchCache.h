/**
 * PitchCache — alias for RenderCache (vocal-time-stretch §6.1).
 *
 * The Stage-1 output cache (post-NSF, pre-time-stretch) is conceptually
 * "PitchCache" — chunk-wise, keyed by (materializationId, chunkKey, pitchRevision).
 * The existing RenderCache class already implements this exact behavior, so we
 * preserve the class name to avoid churning ~91 call sites and add a typedef
 * alias here for code that wants to express the conceptual role explicitly.
 *
 * v7 two-stage pipeline:
 *   Stage 1 (NSF) → PitchCache (= RenderCache)         — chunk-wise
 *   Stage 2 (RB)  → TimeStretchCache (TimeStretchCache.h) — clip-wide
 *
 * Cache invalidation rules (vocal-time-stretch §6.5):
 *   PitchCurve revision changed (pitch edit):
 *     PitchCache    → invalidate affected chunks (existing RenderCache behavior)
 *     TimeStretchCache → invalidate full clip entry (downstream of pitch)
 *   TimeGrid revision changed (time edit, handle drag):
 *     PitchCache    → unchanged  ⭐ (handle drag does NOT make NSF re-run)
 *     TimeStretchCache → invalidate full clip entry
 */
#pragma once

#include "RenderCache.h"

namespace OpenTune {

using PitchCache = RenderCache;

} // namespace OpenTune
