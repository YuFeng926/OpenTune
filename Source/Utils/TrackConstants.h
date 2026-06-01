#pragma once

namespace OpenTune {

/// Authoritative maximum track count for the entire application.
constexpr int MaxTracks = 12;

// ============================================================================
// Shared Lane Geometry Contract
// ============================================================================
// Single source of truth for vertical layout shared between TrackPanel and
// ArrangementView. Both sides MUST consume these constants — no local copies.

/// Vertical offset from component top to first track lane (ruler height).
constexpr int kTrackLaneTopOffset = 30;

/// TrackPanel card inset (left panel visual only — not part of arrangement clip geometry).
constexpr int kTrackPanelCardInsetX = 6;
constexpr int kTrackPanelCardInsetY = 6;

/// Clip shell inset within lane row (right-side arrangement clips).
/// Tighter than panel card — clips should fill the lane more fully.
constexpr int kClipShellInsetX = 4;
constexpr int kClipShellInsetY = 4;

} // namespace OpenTune
