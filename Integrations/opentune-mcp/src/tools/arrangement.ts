// SPDX-License-Identifier: AGPL-3.0-or-later
import { z, obj, opaque, revision, track, sec, bool, oneOf, anyOf, unique, tool } from './common.js';
const placement = { placementId: opaque };
const op = z.discriminatedUnion('op', [
  obj({ op: z.literal('move'), ...placement, toTrackId: track.optional(), startTimelineSec: sec.optional() })
    .refine(v => anyOf(v, ['toTrackId', 'startTimelineSec']), 'move requires toTrackId or startTimelineSec.'),
  obj({ op: z.literal('trim'), ...placement, edge: z.enum(['start', 'end']), toTimelineSec: sec }),
  obj({ op: z.literal('setGain'), ...placement, gainDb: z.number().optional(), gainLinear: z.number().nonnegative().optional() })
    .refine(v => oneOf(v, 'gainDb', 'gainLinear'), 'Provide exactly one gain unit.'),
  obj({ op: z.literal('setFade'), ...placement, fadeInSec: sec.optional(), fadeOutSec: sec.optional() })
    .refine(v => anyOf(v, ['fadeInSec', 'fadeOutSec']), 'Provide at least one fade duration.'),
  obj({ op: z.literal('split'), ...placement, atTimelineSec: sec }),
  obj({ op: z.literal('merge'), leadingPlacementId: opaque, trailingPlacementId: opaque }),
  obj({ op: z.literal('delete'), placementIds: z.array(opaque).min(1).max(64).refine(unique, 'placementIds must be unique.') }),
]);
export const arrangementTools = [
  tool('transport.control', 'transport.v1', obj({
    action: z.enum(['play', 'pause', 'stop', 'seek', 'playFromLastStart', 'pauseAtLastStart']), positionTimelineSec: sec.optional(),
  }).refine(v => (v.action === 'seek') === (v.positionTimelineSec !== undefined), 'Only seek requires and accepts positionTimelineSec.'),
  'Control playback through the editor. seek requires an absolute timeline position; other actions take no position.', { destructive: false }),
  tool('transport.setTempo', 'transport.v1', obj({ bpm: z.number().min(1).max(999).optional(),
    timeSignature: obj({ numerator: z.number().int().min(1).max(32), denominator: z.union([z.literal(1), z.literal(2), z.literal(4), z.literal(8), z.literal(16), z.literal(32)]) }).optional(),
  }), 'Set project BPM and/or time signature through the editor; returns owner changes and revert information.'),
  tool('transport.setLoop', 'transport.v1', obj({ enabled: z.boolean() }), 'Set loop enabled state through the editor.', { destructive: false, idempotent: true }),
  tool('arrangement.edit', 'arrangement.v1', obj({ expectedArrangementRevision: revision, ops: z.array(op).min(1).max(64), dryRun: bool, confirm: bool })
    .refine(v => v.ops.length === 1 || !v.ops.some(o => ['split', 'merge', 'delete'].includes(o.op)), 'split, merge and delete each require a single-op call.')
    .refine(v => v.confirm === undefined || v.ops.some(o => o.op === 'delete'), 'Only delete accepts confirm.'),
  'Edit placements using current arrangement revision. Positions use absolute timeline seconds; fade fields are durations. split/merge/delete must be the sole op. delete accepts confirm. dryRun previews the owner operation.'),
  tool('arrangement.copy', 'arrangement.v1', obj({ sources: z.array(obj(placement)).min(1).max(64),
    mode: z.enum(['window', 'full']), toTrackId: track, startTimelineSec: sec, expectedArrangementRevision: revision,
  }), 'Copy placement windows or full contents to a track at an absolute timeline start. Read current arrangement revision first.', { destructive: false }),
  tool('track.set', 'tracks.v1', obj({ tracks: z.array(obj({ trackId: track, muted: bool, solo: bool,
    volumeDb: z.number().optional(), volumeLinear: z.number().nonnegative().optional(), colour: z.string().regex(/^#[0-9A-Fa-f]{6}$/).optional(),
  }).refine(v => anyOf(v, ['muted', 'solo', 'volumeDb', 'volumeLinear', 'colour']), 'Provide a track setting.')
    .refine(v => v.volumeDb === undefined || v.volumeLinear === undefined, 'Supply at most one volume unit.')).min(1).max(64) }),
  'Set mute, solo, volume and/or colour on existing tracks. Read track IDs first. This API takes no revision parameter.'),
  tool('track.add', 'tracks.v1', obj({ count: z.number().int().min(1).max(64).optional() }), 'Add tracks through the editor; count defaults to 1.', { destructive: false }),
  tool('track.duplicate', 'tracks.v1', obj({ trackId: track, mode: z.enum(['clone', 'share']).optional() }),
    'Duplicate a track. clone (default) copies content; share reuses content.', { destructive: false }),
  tool('track.delete', 'tracks.v1', obj({ trackId: track, expectedArrangementRevision: revision, confirm: bool }),
    'Delete a track and its placements with current arrangement revision and explicit confirm. Inspect the returned undo/revert semantics.'),
];
