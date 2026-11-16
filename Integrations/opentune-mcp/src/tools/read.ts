// SPDX-License-Identifier: AGPL-3.0-or-later
import { z, obj, opaque, contentRange, timelineRange, oneOf, tool } from './common.js';
const target = { contentId: opaque.optional(), placementId: opaque.optional() };
const read = { read: true };
export const readTools = [
  tool('session.info', 'session.v1', obj({}), 'Read version, capabilities, models, audio status and import/export limits.', read),
  tool('project.get', 'read.v1', obj({}), 'Read the complete project, tracks, placements, contents, selection and current revisions before editing.', read),
  tool('content.getNotes', 'read.v1', obj({ ...target,
    range: z.union([contentRange, timelineRange]).optional(), cursor: opaque.optional(), limit: z.number().int().min(1).max(500).optional(),
  }).refine(v => oneOf(v, 'contentId', 'placementId'), 'Supply exactly one of contentId and placementId.')
    .refine(v => !v.contentId || !v.range || 'startContentSec' in v.range, 'contentId requires a content-clock range.'),
  'Read one note page (default 200). Supply exactly one contentId or placementId. Timeline ranges require placementId. Preserve cursor and selection; restart after cursor_stale.', read),
  tool('content.getPitchCurve', 'read.v1', obj({ contentId: opaque, range: contentRange.optional(),
    stepFrame: z.number().int().min(1).max(1000).optional(), cursor: opaque.optional(), limit: z.number().int().min(1).max(10000).optional(),
  }), 'Read one F0/correction page in content seconds. Start with a small range and limit; default limit is 10000. Pass cursor unchanged.', read),
  tool('content.segmentNotes', 'read.v1', obj({ contentId: opaque, range: contentRange.optional(), noteSplitCents: z.number().min(0).max(200).optional() }),
    'Compute note segmentation from existing pitch analysis, using an optional content-clock range.', read),
  tool('content.getDetails', 'read.v1', obj(target).refine(v => oneOf(v, 'contentId', 'placementId'), 'Supply exactly one contentId or placementId.'),
    'Read analysis, content details, render state and time-grid handles; supply exactly one contentId or placementId.', read),
  tool('transport.get', 'read.v1', obj({}), 'Read playback position in timeline seconds, tempo and loop state.', read),
  tool('prefs.get', 'read.v1', obj({}), 'Read preferences, processing options and recent local project paths.', { ...read, open: true }),
  tool('app.getLog', 'read.v1', obj({ maxLines: z.number().int().min(1).max(1000).optional() }), 'Read the API-redacted log tail; default 200 lines.', { ...read, open: true }),
  tool('ui.getState', 'ui.bridge', obj({}), 'Read workspace, editor window and modal state.', read),
  tool('edit.getHistory', 'history.v1', obj({}), 'Read current undo/redo identities, origins and availability before undo or redo.', read),
  tool('audio.get', 'audio.device', obj({}), 'Read active audio devices and supported device settings.', { ...read, open: true }),
];
